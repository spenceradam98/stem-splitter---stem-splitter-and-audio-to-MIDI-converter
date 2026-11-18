#include "BasicPitch.h"

#include "Audio.h"
#include "Dsp.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

extern const unsigned char kBasicPitchModel[];
extern const unsigned long kBasicPitchModelSize;

namespace ss
{
bool BasicPitch::loadEmbedded() { return graph.load (kBasicPitchModel, (size_t) kBasicPitchModelSize); }
bool BasicPitch::load (const void* data, size_t size) { return graph.load (data, size); }

BasicPitch::Output BasicPitch::infer (const std::vector<float>& mono, double sampleRate, int threads,
                                      std::function<void (float)> progress, const std::atomic<bool>* cancel) const
{
    Output out;
    if (! graph.isLoaded() || mono.empty()) return out;
    auto audio = resample (mono, sampleRate, kSampleRate);
    const size_t originalLength = audio.size();
    const int overlapLen = kOverlapFrames * kHop;
    const int hopSize = kWindow - overlapLen;
    audio.insert (audio.begin(), (size_t) overlapLen / 2, 0.0f);

    std::vector<size_t> starts;
    for (size_t i = 0; i < audio.size(); i += (size_t) hopSize) starts.push_back (i);
    const size_t nW = starts.size();
    std::vector<std::vector<Tensor>> results (nW);

    if (threads <= 0) threads = (int) std::max (1u, std::thread::hardware_concurrency());
    threads = std::min<int> (threads, (int) nW);
    std::atomic<size_t> nextWin { 0 }, done { 0 };
    std::mutex progressLock;
    auto worker = [&] {
        for (;;)
        {
            const size_t w = nextWin++;
            if (w >= nW || (cancel && cancel->load())) return;
            Tensor in = Tensor::floats ({ 1, kWindow, 1 });
            for (int k = 0; k < kWindow; ++k)
            {
                const size_t s = starts[w] + (size_t) k;
                in.f[(size_t) k] = s < audio.size() ? audio[s] : 0.0f;
            }
            results[w] = graph.run (in);
            const size_t d = ++done;
            if (progress) { std::lock_guard<std::mutex> l (progressLock); progress ((float) d / (float) nW); }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back (worker);
    worker();
    for (auto& t : pool) t.join();
    if (cancel && cancel->load()) return out;

    // unwrap: drop half the overlap on each side of every window, then trim to the audio length
    const int nOlap = kOverlapFrames / 2;
    const size_t nFramesOriginal = (size_t) std::floor ((double) originalLength * (86.0 / kSampleRate));
    std::vector<std::vector<float>>* dst[3] = { &out.note, &out.onset, &out.contour };
    for (size_t w = 0; w < nW; ++w)
        for (int o = 0; o < 3; ++o)
        {
            const Tensor& t = results[w][(size_t) o];
            const int T = (int) t.shape[1], Fq = (int) t.shape[2];
            for (int fr = nOlap; fr < T - nOlap; ++fr)
            {
                if (dst[o]->size() >= nFramesOriginal) break;
                dst[o]->emplace_back (t.f.begin() + (long) fr * Fq, t.f.begin() + (long) (fr + 1) * Fq);
            }
        }
    return out;
}

double BasicPitch::frameTime (size_t i)
{
    const double original = (double) i * kHop / kSampleRate;
    const double windowNumber = std::floor ((double) i / kFramesPerWindow);
    const double windowOffset = ((double) kHop / kSampleRate) * (kFramesPerWindow - (double) kWindow / kHop) + 0.0018;
    return original - windowOffset * windowNumber;
}

std::vector<Note> BasicPitch::toNotes (Output out, const PolySettings& s)
{
    std::vector<Note> notes;
    auto& frames = out.note;
    auto& onsets = out.onset;
    const int nF = (int) frames.size();
    if (nF < 3) return notes;
    const int nP = 88, maxIdx = 87;
    const int minLen = (int) std::lround (s.minNoteMs / 1000.0 * (kSampleRate / (double) kHop));
    const int energyTol = 11;

    // constrain frequency
    auto zeroRange = [&] (int a, int b) {
        for (int t = 0; t < nF; ++t) for (int p = std::max (0, a); p < std::min (nP, b); ++p) { onsets[(size_t) t][(size_t) p] = 0; frames[(size_t) t][(size_t) p] = 0; }
    };
    if (s.maxHz > 0) zeroRange ((int) std::lround (hzToMidi (s.maxHz) - kMidiOffset), nP);
    if (s.minHz > 0) zeroRange (0, (int) std::lround (hzToMidi (s.minHz) - kMidiOffset));

    if (s.inferOnsets)
    {
        const int nDiff = 2;
        std::vector<std::vector<float>> diff ((size_t) nF, std::vector<float> ((size_t) nP, 0.0f));
        float maxDiff = 0, maxOnset = 0;
        for (int t = 0; t < nF; ++t)
            for (int p = 0; p < nP; ++p)
            {
                float mn = 1e9f;
                for (int n = 1; n <= nDiff; ++n)
                {
                    const float prev = t - n >= 0 ? frames[(size_t) (t - n)][(size_t) p] : 0.0f;
                    mn = std::min (mn, frames[(size_t) t][(size_t) p] - prev);
                }
                if (mn < 0 || t < nDiff) mn = 0;
                diff[(size_t) t][(size_t) p] = mn;
                maxDiff = std::max (maxDiff, mn);
                maxOnset = std::max (maxOnset, onsets[(size_t) t][(size_t) p]);
            }
        if (maxDiff > 0)
            for (int t = 0; t < nF; ++t)
                for (int p = 0; p < nP; ++p)
                    onsets[(size_t) t][(size_t) p] = std::max (onsets[(size_t) t][(size_t) p], maxOnset * diff[(size_t) t][(size_t) p] / maxDiff);
    }

    // peaks along time (scipy argrelmax: strictly greater than both neighbours)
    struct On { int t, p; };
    std::vector<On> ons;
    for (int t = 1; t < nF - 1; ++t)
        for (int p = 0; p < nP; ++p)
        {
            const float v = onsets[(size_t) t][(size_t) p];
            if (v >= s.onsetThreshold && v > onsets[(size_t) (t - 1)][(size_t) p] && v > onsets[(size_t) (t + 1)][(size_t) p]) ons.push_back ({ t, p });
        }
    // go backwards in time (same order as numpy's reversed np.where)
    std::sort (ons.begin(), ons.end(), [] (const On& a, const On& b) { return a.t != b.t ? a.t > b.t : a.p > b.p; });

    auto remaining = frames;
    struct Ev { int a, b, p; float amp; };
    std::vector<Ev> events;
    auto meanAmp = [&] (int a, int b, int p) { double s2 = 0; for (int t = a; t < b; ++t) s2 += frames[(size_t) t][(size_t) p]; return b > a ? (float) (s2 / (b - a)) : 0.0f; };
    for (auto& o : ons)
    {
        if (o.t >= nF - 1) continue;
        int i = o.t + 1, k = 0;
        while (i < nF - 1 && k < energyTol) { k = remaining[(size_t) i][(size_t) o.p] < s.frameThreshold ? k + 1 : 0; ++i; }
        i -= k;
        if (i - o.t <= minLen) continue;
        for (int t = o.t; t < i; ++t)
        {
            remaining[(size_t) t][(size_t) o.p] = 0;
            if (o.p < maxIdx) remaining[(size_t) t][(size_t) o.p + 1] = 0;
            if (o.p > 0) remaining[(size_t) t][(size_t) o.p - 1] = 0;
        }
        events.push_back ({ o.t, i, o.p, meanAmp (o.t, i, o.p) });
    }

    if (s.melodiaTrick)
    {
        for (;;)
        {
            float mx = 0; int mt = 0, mp = 0;
            for (int t = 0; t < nF; ++t) for (int p = 0; p < nP; ++p) if (remaining[(size_t) t][(size_t) p] > mx) { mx = remaining[(size_t) t][(size_t) p]; mt = t; mp = p; }
            if (mx <= s.frameThreshold) break;
            remaining[(size_t) mt][(size_t) mp] = 0;
            auto clear = [&] (int t) {
                remaining[(size_t) t][(size_t) mp] = 0;
                if (mp < maxIdx) remaining[(size_t) t][(size_t) mp + 1] = 0;
                if (mp > 0) remaining[(size_t) t][(size_t) mp - 1] = 0;
            };
            int i = mt + 1, k = 0;
            while (i < nF - 1 && k < energyTol) { k = remaining[(size_t) i][(size_t) mp] < s.frameThreshold ? k + 1 : 0; clear (i); ++i; }
            const int iEnd = i - 1 - k;
            i = mt - 1; k = 0;
            while (i > 0 && k < energyTol) { k = remaining[(size_t) i][(size_t) mp] < s.frameThreshold ? k + 1 : 0; clear (i); --i; }
            const int iStart = i + 1 + k;
            if (iEnd - iStart <= minLen) continue;
            events.push_back ({ iStart, iEnd, mp, meanAmp (iStart, iEnd, mp) });
        }
    }

    // pitch bends from the contour output (1/3-semitone bins, gaussian-weighted around the note)
    const int tol = 25, winLen = 2 * tol + 1, nC = 264;
    std::vector<float> gauss ((size_t) winLen);
    for (int k = 0; k < winLen; ++k) { const float x = (float) (k - tol); gauss[(size_t) k] = std::exp (-0.5f * x * x / 25.0f); }
    for (auto& e : events)
    {
        Note n;
        n.start = frameTime ((size_t) e.a);
        n.end = frameTime ((size_t) std::min (e.b, nF - 1));
        n.pitch = e.p + kMidiOffset;
        n.velocity = std::clamp (e.amp, 0.05f, 1.0f);
        if (s.pitchBends && ! out.contour.empty())
        {
            const int fi = (int) std::lround (12.0 * 3 * std::log2 (midiToHz ((float) n.pitch) / 27.5));
            const int fs = std::max (fi - tol, 0), fe = std::min (nC, fi + tol + 1);
            const int gOff = std::max (0, tol - fi);
            const int pbShift = tol - std::max (0, tol - fi);
            bool any = false;
            for (int t = e.a; t < e.b && t < (int) out.contour.size(); ++t)
            {
                float best = -1; int arg = 0;
                for (int c = fs; c < fe; ++c)
                {
                    const float v = out.contour[(size_t) t][(size_t) c] * gauss[(size_t) (gOff + c - fs)];
                    if (v > best) { best = v; arg = c - fs; }
                }
                const float semis = (float) (arg - pbShift) / 3.0f;
                n.bend.push_back (semis);
                any = any || semis != 0.0f;
            }
            if (! any) n.bend.clear();
        }
        notes.push_back (std::move (n));
    }
    std::sort (notes.begin(), notes.end());
    return notes;
}
} // namespace ss
