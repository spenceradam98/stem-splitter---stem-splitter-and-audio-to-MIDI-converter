#include "MonoTranscriber.h"

#include "Dsp.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ss
{
MonoSettings MonoSettings::bass()
{
    MonoSettings s;
    s.minHz = 30.0f; s.maxHz = 400.0f; s.minNoteSeconds = 0.06f; s.splitOnOnsets = true;
    return s;
}

MonoSettings MonoSettings::vocals()
{
    MonoSettings s;
    s.minHz = 75.0f; s.maxHz = 1100.0f; s.minNoteSeconds = 0.08f; s.splitSemitones = 0.75f;
    s.splitHoldSeconds = 0.07f; s.scoopSeconds = 0.14f; s.splitOnOnsets = false; s.gateDb = -40.0f;
    return s;
}

namespace
{
struct Trough { float hz; float prob; };

// Beta(2, 18) prior over 100 YIN thresholds 0.01..1.00 (the pYIN default, mean 0.1)
const std::vector<float>& thresholdPrior()
{
    static const std::vector<float> p = [] {
        std::vector<float> v (100);
        double sum = 0;
        for (int i = 0; i < 100; ++i)
        {
            const double x = (i + 1) / 100.0;
            v[(size_t) i] = (float) (x * std::pow (1.0 - x, 17.0));
            sum += v[(size_t) i];
        }
        for (auto& x : v) x = (float) (x / sum);
        return v;
    }();
    return p;
}
} // namespace

PitchCurve trackPitch (const std::vector<float>& monoIn, double sampleRate, const MonoSettings& s,
                       const std::atomic<bool>* cancel)
{
    constexpr double sr = 16000.0;
    constexpr int hop = 160;   // 10 ms
    const auto x = resample (monoIn, sampleRate, sr);
    PitchCurve curve;
    curve.hopSeconds = hop / sr;
    const size_t frames = x.size() / hop + 1;
    if (x.empty()) return curve;

    const int tauMin = std::max (2, (int) std::floor (sr / s.maxHz));
    const int tauMax = (int) std::ceil (sr / s.minHz);
    const int W = std::max (320, (int) (tauMax * 1.2));
    const int L = W + tauMax + 2;
    const int N = nextPow2 (L + W);

    // pitch bins (25 cents) for the HMM
    const float midiLo = hzToMidi (s.minHz), midiHi = hzToMidi (s.maxHz);
    const int nB = (int) std::ceil ((midiHi - midiLo) * 4.0f) + 1;
    auto binOf = [&] (float hz) { return std::clamp ((int) std::lround ((hzToMidi (hz) - midiLo) * 4.0f), 0, nB - 1); };

    // per-frame RMS gate
    auto rms = rmsEnvelope (x, hop);
    float peakRms = 1e-9f;
    for (float r : rms) peakRms = std::max (peakRms, r);
    const float gate = peakRms * std::pow (10.0f, s.gateDb / 20.0f);

    std::vector<std::vector<Trough>> troughs (frames);
    std::vector<std::complex<float>> X ((size_t) N), Y ((size_t) N);
    std::vector<float> d ((size_t) tauMax + 2), cm ((size_t) tauMax + 2), cumsq ((size_t) L + 1);
    const auto& prior = thresholdPrior();

    for (size_t f = 0; f < frames; ++f)
    {
        if (cancel && (f & 255) == 0 && cancel->load()) return curve;
        if (f < rms.size() && rms[f] < gate) continue;
        const long start = (long) f * hop - W / 2;
        auto sample = [&] (long i) { const long k = start + i; return k >= 0 && k < (long) x.size() ? x[(size_t) k] : 0.0f; };
        for (int i = 0; i < N; ++i)
        {
            X[(size_t) i] = { i < L ? sample (i) : 0.0f, 0.0f };
            Y[(size_t) i] = { i < W ? sample (i) : 0.0f, 0.0f };
        }
        cumsq[0] = 0;
        for (int i = 0; i < L; ++i) cumsq[(size_t) i + 1] = cumsq[(size_t) i] + X[(size_t) i].real() * X[(size_t) i].real();
        fft (X); fft (Y);
        for (int i = 0; i < N; ++i) X[(size_t) i] *= std::conj (Y[(size_t) i]);
        fft (X, true);
        const float e0 = cumsq[(size_t) W];
        if (e0 <= 1e-10f) continue;
        for (int tau = 0; tau <= tauMax; ++tau)
        {
            const float et = cumsq[(size_t) (tau + W)] - cumsq[(size_t) tau];
            d[(size_t) tau] = std::max (0.0f, e0 + et - 2.0f * X[(size_t) tau].real());
        }
        cm[0] = 1.0f;
        double run = 0;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            run += d[(size_t) tau];
            cm[(size_t) tau] = run > 0 ? (float) (d[(size_t) tau] * tau / run) : 1.0f;
        }
        // troughs within range
        std::vector<int> tr;
        for (int tau = tauMin; tau < tauMax; ++tau)
            if (cm[(size_t) tau] < cm[(size_t) tau - 1] && cm[(size_t) tau] <= cm[(size_t) tau + 1]) tr.push_back (tau);
        if (tr.empty()) continue;
        std::vector<float> prob (tr.size(), 0.0f);
        size_t globalMin = 0;
        for (size_t k = 1; k < tr.size(); ++k) if (cm[(size_t) tr[k]] < cm[(size_t) tr[globalMin]]) globalMin = k;
        for (int t = 0; t < 100; ++t)
        {
            const float th = (t + 1) / 100.0f;
            size_t hit = tr.size();
            for (size_t k = 0; k < tr.size(); ++k) if (cm[(size_t) tr[k]] < th) { hit = k; break; }
            if (hit < tr.size()) prob[hit] += prior[(size_t) t];
            else prob[globalMin] += prior[(size_t) t] * 0.01f;
        }
        for (size_t k = 0; k < tr.size(); ++k)
        {
            if (prob[k] <= 1e-4f) continue;
            const int tau = tr[k];
            const float a = cm[(size_t) tau - 1], b = cm[(size_t) tau], c = cm[(size_t) tau + 1];
            const float den = a - 2 * b + c;
            const float off = std::abs (den) > 1e-9f ? std::clamp (0.5f * (a - c) / den, -0.5f, 0.5f) : 0.0f;
            troughs[f].push_back ({ (float) (sr / (tau + off)), prob[k] });
        }
    }

    // ---- Viterbi over (voiced, bin) x 2 ----
    const int maxJump = 12;   // 3 semitones per 10 ms
    std::vector<float> jumpLog ((size_t) maxJump + 1);
    {
        double sum = 0;
        for (int j = -maxJump; j <= maxJump; ++j) sum += (maxJump + 1 - std::abs (j));
        for (int j = 0; j <= maxJump; ++j) jumpLog[(size_t) j] = (float) std::log ((maxJump + 1 - j) / sum);
    }
    const float stay = std::log (0.99f), sw = std::log (0.01f);
    const int S = 2 * nB;
    std::vector<float> score ((size_t) S, -std::log ((float) S)), next ((size_t) S);
    std::vector<std::vector<int>> back (frames, std::vector<int> ((size_t) S, 0));
    std::vector<float> obs ((size_t) S);
    std::vector<float> voicedSum (frames, 0.0f);
    for (size_t f = 0; f < frames; ++f)
    {
        std::fill (obs.begin(), obs.end(), 0.0f);
        float sum = 0;
        for (auto& t : troughs[f]) { obs[(size_t) binOf (t.hz)] += t.prob; sum += t.prob; }
        sum = std::min (sum, 0.99f);
        voicedSum[f] = sum;
        for (int b = 0; b < nB; ++b)
        {
            obs[(size_t) b] = std::log (std::max (obs[(size_t) b], 1e-6f));
            obs[(size_t) (nB + b)] = std::log (std::max ((1.0f - sum) / (float) nB, 1e-6f));
        }
        for (int to = 0; to < S; ++to)
        {
            const int tb = to % nB;
            const bool tv = to < nB;
            float best = -std::numeric_limits<float>::infinity();
            int arg = 0;
            for (int fb = std::max (0, tb - maxJump); fb <= std::min (nB - 1, tb + maxJump); ++fb)
            {
                const float j = jumpLog[(size_t) std::abs (fb - tb)];
                const float a = score[(size_t) fb] + j + (tv ? stay : sw);          // from voiced
                const float u = score[(size_t) (nB + fb)] + j + (tv ? sw : stay);   // from unvoiced
                if (a > best) { best = a; arg = fb; }
                if (u > best) { best = u; arg = nB + fb; }
            }
            next[(size_t) to] = best + obs[(size_t) to];
            back[f][(size_t) to] = arg;
        }
        const float mx = *std::max_element (next.begin(), next.end());
        for (int k = 0; k < S; ++k) score[(size_t) k] = next[(size_t) k] - mx;
    }
    std::vector<int> path (frames);
    path[frames - 1] = (int) (std::max_element (score.begin(), score.end()) - score.begin());
    for (size_t f = frames - 1; f > 0; --f) path[f - 1] = back[f][(size_t) path[f]];

    curve.midi.assign (frames, 0.0f);
    curve.confidence.assign (frames, 0.0f);
    for (size_t f = 0; f < frames; ++f)
    {
        curve.confidence[f] = voicedSum[f];
        if (path[f] >= nB) continue;
        const int b = path[f];
        float bestP = 0, hz = 0;
        for (auto& t : troughs[f])
            if (std::abs (binOf (t.hz) - b) <= 1 && t.prob > bestP) { bestP = t.prob; hz = t.hz; }
        curve.midi[f] = hz > 0 ? hzToMidi (hz) : midiLo + b / 4.0f;
    }
    return curve;
}

std::vector<Note> segmentNotes (const PitchCurve& curve, const std::vector<float>& mono, double sampleRate,
                                const MonoSettings& s)
{
    std::vector<Note> notes;
    const size_t F = curve.midi.size();
    if (F == 0) return notes;
    const double hop = curve.hopSeconds;
    const int holdFrames = std::max (1, (int) std::lround (s.splitHoldSeconds / hop));

    // light median smoothing inside voiced runs
    std::vector<float> m = curve.midi;
    for (size_t f = 0; f < F; ++f)
    {
        if (curve.midi[f] <= 0) continue;
        std::vector<float> w;
        for (long j = (long) f - 2; j <= (long) f + 2; ++j)
            if (j >= 0 && j < (long) F && curve.midi[(size_t) j] > 0) w.push_back (curve.midi[(size_t) j]);
        m[f] = median (w);
    }

    std::vector<double> onsets;
    if (s.splitOnOnsets) onsets = detectOnsets (mono, sampleRate, nullptr, 1.0f, 0.06);
    std::vector<bool> onsetAt (F, false);
    for (double t : onsets) { const long i = (long) std::lround (t / hop); if (i >= 0 && i < (long) F) onsetAt[(size_t) i] = true; }

    struct Seg { size_t a, b; };   // [a, b)
    std::vector<Seg> segs;
    size_t f = 0;
    while (f < F)
    {
        if (m[f] <= 0) { ++f; continue; }
        size_t a = f;
        std::vector<float> held { m[f] };
        size_t g = f + 1;
        int dev = 0;
        size_t devStart = 0;
        for (; g < F && m[g] > 0; ++g)
        {
            const float ref = median (held);
            if (std::abs (m[g] - ref) > s.splitSemitones)
            {
                if (dev == 0) devStart = g;
                if (++dev >= holdFrames) { segs.push_back ({ a, devStart }); a = devStart; held.assign (m.begin() + (long) devStart, m.begin() + (long) g + 1); dev = 0; continue; }
            }
            else dev = 0;
            if (onsetAt[g] && g - a >= (size_t) holdFrames) { segs.push_back ({ a, g }); a = g; held.clear(); }
            held.push_back (m[g]);
            if (held.size() > 40) held.erase (held.begin());
        }
        segs.push_back ({ a, g });
        f = g;
    }

    // glue short scoops onto the following note when they touch it
    const size_t scoop = (size_t) std::lround (s.scoopSeconds / hop);
    for (size_t k = 0; k + 1 < segs.size();)
    {
        auto& c = segs[k];
        auto& n = segs[k + 1];
        auto segMedian = [&] (const Seg& q) { return median (std::vector<float> (m.begin() + (long) q.a, m.begin() + (long) q.b)); };
        if (c.b - c.a < scoop && n.a <= c.b + 1 && std::abs (segMedian (c) - segMedian (n)) <= 2.5f && n.b - n.a >= c.b - c.a)
        {
            n.a = c.a;
            segs.erase (segs.begin() + (long) k);
        }
        else ++k;
    }

    // loudness for velocity
    const int rmsHop = std::max (1, (int) std::lround (sampleRate * hop));
    auto rms = rmsEnvelope (mono, rmsHop);
    float peak = 1e-9f;
    for (float r : rms) peak = std::max (peak, r);

    const size_t minFrames = (size_t) std::max (1L, std::lround (s.minNoteSeconds / hop));
    for (auto& sg : segs)
    {
        if (sg.b - sg.a < minFrames) continue;
        // the pitch of the note is the median of its stable part (after any scoop)
        const size_t skip = std::min ((sg.b - sg.a) / 3, scoop);
        const float pm = median (std::vector<float> (m.begin() + (long) (sg.a + skip), m.begin() + (long) sg.b));
        Note n;
        n.start = sg.a * hop;
        n.end = sg.b * hop;
        n.pitch = (int) std::lround (pm);
        float mx = 0;
        for (size_t i = sg.a; i < sg.b && i < rms.size(); ++i) mx = std::max (mx, rms[i]);
        const float db = 20.0f * std::log10 (std::max (mx, 1e-9f) / peak);
        n.velocity = std::clamp ((db + 36.0f) / 36.0f, 0.15f, 1.0f);
        if (s.keepBends)
        {
            bool any = false;
            for (size_t i = sg.a; i < sg.b; ++i)
            {
                const float dv = curve.midi[i] > 0 ? curve.midi[i] - (float) n.pitch : 0.0f;
                n.bend.push_back (std::clamp (dv, -12.0f, 12.0f));
                any = any || std::abs (dv) > 0.15f;
            }
            if (! any) n.bend.clear();
        }
        notes.push_back (std::move (n));
    }
    return notes;
}
} // namespace ss
