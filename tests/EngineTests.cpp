// Engine tests: synthetic audio with known notes, drums, chords and tempo, checked against what
// the analysers find. No JUCE. If STEMSCRIBE_TEST_MODEL points at the HTDemucs weights, the
// separation engine is tested too (CI does this after converting the model).
#include "Audio.h"
#include "BasicPitch.h"
#include "Dsp.h"
#include "DrumTranscriber.h"
#include "Harmony.h"
#include "MidiFile.h"
#include "MonoTranscriber.h"
#include "Pipeline.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <random>
#include <string>

using namespace ss;

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); std::printf (__VA_ARGS__); std::printf ("\n"); } } while (0)

static void run (const char* name, const std::function<void()>& f)
{
    const int before = failures;
    std::printf ("%s\n", name);
    f();
    if (failures == before) std::printf ("  ok\n");
}

// ---- synthesis helpers ----------------------------------------------------------------
struct SynthNote { double start, end; int pitch; float amp = 0.3f; };

static void addTone (std::vector<float>& x, double sr, double start, double end, float hz, float amp, int harmonics,
                     float vibratoDepthSemis = 0, float vibratoHz = 5.5f, float rolloff = 1.0f)
{
    double phase = 0;
    const long a = (long) (start * sr), b = std::min ((long) x.size(), (long) (end * sr));
    for (long i = std::max (0L, a); i < b; ++i)
    {
        const double t = (i - a) / sr;
        const double f = hz * std::pow (2.0, vibratoDepthSemis * std::sin (2 * M_PI * vibratoHz * t) / 12.0);
        phase += 2 * M_PI * f / sr;
        const double env = std::min (1.0, t / 0.01) * std::min (1.0, (b - i) / (0.02 * sr)) * std::exp (-0.8 * t);
        double s = 0;
        for (int h = 1; h <= harmonics; ++h) s += std::sin (h * phase) / std::pow ((double) h, (double) rolloff);
        x[(size_t) i] += (float) (amp * env * s);
    }
}

static std::vector<float> render (const std::vector<SynthNote>& notes, double sr, double seconds, int harmonics = 6, float rolloff = 1.0f)
{
    std::vector<float> x ((size_t) (seconds * sr), 0.0f);
    for (auto& n : notes) addTone (x, sr, n.start, n.end, midiToHz ((float) n.pitch), n.amp, harmonics, 0, 5.5f, rolloff);
    return x;
}

static void addKick (std::vector<float>& x, double sr, double t, float amp = 0.9f)
{
    double ph = 0;
    for (long i = 0; i < (long) (0.25 * sr); ++i)
    {
        const long k = (long) (t * sr) + i;
        if (k >= (long) x.size()) break;
        const double tt = i / sr;
        ph += 2 * M_PI * (50 + 90 * std::exp (-tt * 30)) / sr;
        x[(size_t) k] += (float) (amp * std::sin (ph) * std::exp (-tt * 12));
    }
}

static void addNoiseHit (std::vector<float>& x, double sr, double t, float amp, double decay, double hpHz, float toneHz, std::mt19937& rng)
{
    std::uniform_real_distribution<float> u (-1, 1);
    const float a = (float) std::exp (-2 * M_PI * hpHz / sr);
    float prev[3] {}, hp[3] {};
    for (long i = 0; i < (long) (decay * 6 * sr); ++i)
    {
        const long k = (long) (t * sr) + i;
        if (k >= (long) x.size()) break;
        const double tt = i / sr;
        float n = u (rng);
        for (int st = 0; st < 3; ++st) { hp[st] = a * (hp[st] + n - prev[st]); prev[st] = n; n = hp[st]; }   // 3 one-pole high-passes
        float s = n;
        if (toneHz > 0) s = 0.6f * s + 0.5f * (float) std::sin (2 * M_PI * toneHz * tt);
        x[(size_t) k] += (float) (amp * s * std::exp (-tt / decay));
    }
}

static bool hasNote (const std::vector<Note>& notes, int pitch, double start, double tol)
{
    for (auto& n : notes) if (n.pitch == pitch && std::abs (n.start - start) <= tol) return true;
    return false;
}

int main()
{
    const double sr = 44100.0;

    run ("WAV round trip and resampling", [&] {
        AudioBuffer a; a.sampleRate = sr; a.setSize (2, 4410);
        for (int i = 0; i < 4410; ++i) { a.ch[0][(size_t) i] = 0.5f * (float) std::sin (i * 0.05); a.ch[1][(size_t) i] = -0.25f; }
        const auto path = (std::filesystem::temp_directory_path() / "ss_test.wav").string();
        for (int bits : { 16, 24, 32 })
        {
            CHECK (writeWav (path, a, bits), "write %d", bits);
            AudioBuffer b;
            CHECK (readWav (path, b), "read %d", bits);
            CHECK (b.numFrames() == 4410 && b.numChannels() == 2, "size");
            float err = 0;
            for (int i = 0; i < 4410; ++i) err = std::max (err, std::abs (a.ch[0][(size_t) i] - b.ch[0][(size_t) i]));
            CHECK (err < (bits == 16 ? 1e-4f : 1e-6f), "bits %d err %g", bits, err);
        }
        std::filesystem::remove (path);
        // a 1 kHz tone resampled 44.1k -> 22.05k keeps its frequency and level
        std::vector<float> x (44100);
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float) std::sin (2 * M_PI * 1000.0 * i / sr);
        auto y = resample (x, sr, 22050.0);
        CHECK (std::abs ((long) y.size() - 22050) <= 1, "len %zu", y.size());
        int zc = 0;
        for (size_t i = 1001; i < 21050; ++i) zc += (y[i - 1] < 0) != (y[i] < 0);
        CHECK (std::abs (zc / 2.0 / (20049.0 / 22050.0) - 1000.0) < 5.0, "freq %g", zc / 2.0 / (20049.0 / 22050.0));
        float pk = 0; for (size_t i = 1000; i < 21000; ++i) pk = std::max (pk, std::abs (y[i]));
        CHECK (std::abs (pk - 1.0f) < 0.02f, "peak %g", pk);
        // content above the new Nyquist is removed
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float) std::sin (2 * M_PI * 15000.0 * i / sr);
        y = resample (x, sr, 22050.0);
        pk = 0; for (size_t i = 1000; i < 21000; ++i) pk = std::max (pk, std::abs (y[i]));
        CHECK (pk < 0.01f, "alias %g", pk);
    });

    run ("Bass line: pYIN notes, repeated notes split on onsets", [&] {
        // E1 A1 A1 B1 E2 (41 Hz .. 82 Hz)
        std::vector<SynthNote> line { { 0.2, 0.7, 28 }, { 0.8, 1.2, 33 }, { 1.3, 1.7, 33 }, { 1.8, 2.3, 35 }, { 2.4, 3.0, 40 } };
        auto x = render (line, sr, 3.3, 8);
        const auto set = MonoSettings::bass();
        const auto curve = trackPitch (x, sr, set);
        auto notes = segmentNotes (curve, x, sr, set);
        CHECK (notes.size() == line.size(), "found %zu notes", notes.size());
        for (auto& n : line) CHECK (hasNote (notes, n.pitch, n.start, 0.05), "missing %d at %.2f", n.pitch, n.start);
        for (auto& n : notes) std::printf ("    %d %.3f-%.3f v%.2f\n", n.pitch, n.start, n.end, n.velocity);
    });

    run ("Vocal with vibrato and a scoop: one note, vibrato kept as bend", [&] {
        std::vector<float> x ((size_t) (2.0 * sr), 0.0f);
        addTone (x, sr, 0.3, 1.6, 220.0f, 0.3f, 5, 0.45f, 5.5f);
        const auto set = MonoSettings::vocals();
        auto curve = trackPitch (x, sr, set);
        auto notes = segmentNotes (curve, x, sr, set);
        CHECK (notes.size() == 1, "found %zu notes", notes.size());
        if (! notes.empty())
        {
            CHECK (notes[0].pitch == 57, "pitch %d", notes[0].pitch);
            float mn = 0, mx = 0;
            for (float b : notes[0].bend) { mn = std::min (mn, b); mx = std::max (mx, b); }
            CHECK (mx > 0.3f && mn < -0.3f, "bend range %g..%g", mn, mx);
        }
        // pitch curve follows the vibrato
        int voiced = 0;
        for (float m : curve.midi) voiced += m > 0;
        CHECK (voiced > 110 && voiced < 140, "voiced frames %d", voiced);
    });

    run ("Basic Pitch: polyphonic notes", [&] {
        BasicPitch bp;
        CHECK (bp.loadEmbedded(), "model load");
        // C4 alone, then an E4+G4 dyad, then a C major triad
        std::vector<SynthNote> notes { { 0.2, 0.9, 60 }, { 1.1, 1.9, 64 }, { 1.1, 1.9, 67 }, { 2.1, 3.0, 60 }, { 2.1, 3.0, 64 }, { 2.1, 3.0, 67 } };
        auto x = render (notes, sr, 3.4, 6, 2.0f);   // piano-like harmonic rolloff
        auto out = bp.infer (x, sr, 2);
        CHECK (out.frames() > 280 && out.frames() < 300, "frames %zu", out.frames());
        auto found = BasicPitch::toNotes (out);
        for (auto& n : found) std::printf ("    %d %.3f-%.3f v%.2f\n", n.pitch, n.start, n.end, n.velocity);
        for (auto& n : notes) CHECK (hasNote (found, n.pitch, n.start, 0.08), "missing %d at %.2f", n.pitch, n.start);
        int extra = 0;
        for (auto& n : found) { bool ok = false; for (auto& e : notes) ok = ok || (e.pitch == n.pitch && std::abs (e.start - n.start) < 0.1); extra += ! ok; }
        CHECK (extra <= 2, "%d unexpected notes", extra);
    });

    run ("Drums: kick, snare, closed and open hats", [&] {
        std::mt19937 rng (7);
        std::vector<float> x ((size_t) (5.0 * sr), 0.0f);
        const double beat = 0.5;   // 120 BPM
        int kicks = 0, snares = 0, closed = 0, open = 0;
        for (int b = 0; b < 8; ++b)
        {
            const double t = 0.1 + b * beat;
            if (b % 2 == 0) { addKick (x, sr, t); ++kicks; }
            else { addNoiseHit (x, sr, t, 0.6f, 0.05, 900.0, 190.0f, rng); ++snares; }
            if (b == 7) { addNoiseHit (x, sr, t + beat / 2, 0.3f, 0.12, 7000.0, 0, rng); ++open; }
            else { addNoiseHit (x, sr, t + beat / 2, 0.3f, 0.012, 7000.0, 0, rng); ++closed; }
        }
        auto hits = transcribeDrums (x, sr);
        int k = 0, s = 0, c = 0, o = 0;
        for (auto& h : hits) { k += h.pitch == gm::kick; s += h.pitch == gm::snare; c += h.pitch == gm::closedHat; o += h.pitch == gm::openHat; }
        for (auto& h : hits) std::printf ("    %.3f %s\n", h.start, gmDrumName (h.pitch));
        std::printf ("    kick %d/%d snare %d/%d closed %d/%d open %d/%d\n", k, kicks, s, snares, c, closed, o, open);
        CHECK (k == kicks, "kicks %d", k);
        CHECK (s == snares, "snares %d", s);
        CHECK (c + o == closed + open, "hats %d", c + o);
        CHECK (o >= 1, "open hats %d", o);
        for (auto& h : hits)
        {
            const double d = std::fmod (h.start - 0.1 + 10.0, h.pitch == gm::kick ? 1.0 : 0.25);
            CHECK (std::min (d, (h.pitch == gm::kick ? 1.0 : 0.25) - d) < 0.02, "%s at %.3f", gmDrumName (h.pitch), h.start);
        }
    });

    run ("Tempo from a click track", [&] {
        std::vector<float> x ((size_t) (12 * sr), 0.0f);
        const double bpm = 100.0, period = 60.0 / bpm;
        for (double t = 0.3; t < 11.5; t += period) addKick (x, sr, t, 0.7f);
        const auto te = estimateTempo (x, sr);
        CHECK (std::abs (te.bpm - bpm) < 2.0, "bpm %.2f", te.bpm);
        const double ph = std::fmod (te.offset - 0.3 + 10 * period, period);
        CHECK (ph < 0.03 || ph > period - 0.03, "offset %.3f", te.offset);
    });

    run ("Key and chords with slash bass and smoothing", [&] {
        // C | Am | F | G/B at 120 BPM, 2 beats each... played twice
        const double beat = 0.5;
        std::vector<Note> harm, bass;
        const int chords[4][3] = { { 60, 64, 67 }, { 57, 60, 64 }, { 53, 57, 60 }, { 55, 59, 62 } };
        const int roots[4] = { 36, 33, 29, 35 };   // G/B: bass on B
        for (int rep = 0; rep < 2; ++rep)
            for (int c = 0; c < 4; ++c)
            {
                const double t = (rep * 4 + c) * 4 * beat;
                for (int k = 0; k < 3; ++k) { Note n; n.start = t; n.end = t + 4 * beat - 0.05; n.pitch = chords[c][k]; n.velocity = 0.7f; harm.push_back (n); }
                Note b; b.start = t; b.end = t + 4 * beat - 0.05; b.pitch = roots[c]; b.velocity = 0.8f; bass.push_back (b);
                // a passing note that must not flip the chord
                Note p; p.start = t + 1.5 * beat; p.end = t + 1.75 * beat; p.pitch = 62; p.velocity = 0.3f; harm.push_back (p);
            }
        int root; bool minor;
        detectKey ({ &harm, &bass }, root, minor);
        CHECK (root == 0 && ! minor, "key %s %s", pitchClassName (root), minor ? "minor" : "major");
        auto ch = detectChords ({ &harm }, &bass, 16.0, 120.0, 0.0);
        std::string seq;
        for (auto& c : ch) seq += c.name() + " ";
        std::printf ("    %s\n", seq.c_str());
        CHECK (seq == "C Am F G/B C Am F G/B ", "sequence '%s'", seq.c_str());
    });

    run ("Chord names parse and voice", [&] {
        Chord c;
        CHECK (parseChordName ("F#m7", c) && c.root == 6 && c.name() == "F#m7", "F#m7 -> %s", c.name().c_str());
        CHECK (parseChordName ("C/E", c) && c.bass == 4 && c.name() == "C/E", "C/E");
        CHECK (parseChordName ("Bbmaj7", c) && c.name() == "Bbmaj7", "Bbmaj7");
        CHECK (parseChordName ("N", c) && c.root < 0, "N");
        CHECK (! parseChordName ("H7", c), "H7 rejected");
        parseChordName ("G/B", c);
        auto v = c.voicing (48);
        CHECK (v.size() == 4 && v[0] % 12 == 11 && v[0] < v[1], "voicing");
    });

    run ("MIDI file round trip with drums, bends and chords", [&] {
        Analysis a;
        a.tempo = 96.0; a.duration = 4.0;
        StemResult bassS; bassS.kind = StemKind::Bass; bassS.name = "Bass"; bassS.mode = Transcribe::Mono;
        Note n1; n1.start = 0.5; n1.end = 1.0; n1.pitch = 40; n1.velocity = 0.9f; n1.bend = { 0, 0.5f, 1.0f, 0.5f };
        Note n2; n2.start = 1.25; n2.end = 2.0; n2.pitch = 43; n2.velocity = 0.5f;
        bassS.notes = { n1, n2 };
        StemResult dr; dr.kind = StemKind::Drums; dr.name = "Drums"; dr.mode = Transcribe::Drums;
        Note k; k.start = 0.0; k.end = 0.06; k.pitch = gm::kick; dr.notes = { k };
        a.stems = { bassS, dr };
        Chord c; c.start = 0; c.end = 2; c.root = 0; c.quality = 0;
        a.chords = { c };
        auto bytes = exportAllMidi (a);
        std::vector<std::vector<Note>> tracks; double bpm;
        CHECK (readMidiNotes (bytes, tracks, bpm), "parse");
        CHECK (std::abs (bpm - 96.0) < 0.01, "bpm %g", bpm);
        CHECK (tracks.size() == 5, "tracks %zu", tracks.size());   // conductor, bass, drums, chords, roots
        if (tracks.size() == 5)
        {
            CHECK (tracks[1].size() == 2 && tracks[1][0].pitch == 40 && std::abs (tracks[1][0].start - 0.5) < 0.002 && std::abs (tracks[1][1].end - 2.0) < 0.002, "bass notes");
            CHECK (tracks[2].size() == 1 && tracks[2][0].pitch == 36, "drum note");
            CHECK (tracks[3].size() == 3, "chord notes %zu", tracks[3].size());
        }
    });

    run ("Quantize, transpose and serialise", [&] {
        std::vector<Note> v (2);
        v[0].start = 0.26; v[0].end = 0.5; v[0].pitch = 60;
        v[1].start = 0.51; v[1].end = 0.74; v[1].pitch = 62;
        quantizeNotes (v, 120.0, 0.0, 4, 1.0f);
        CHECK (std::abs (v[0].start - 0.25) < 1e-9 && std::abs (v[1].start - 0.5) < 1e-9, "quantized %g %g", v[0].start, v[1].start);
        transposeNotes (v, 12);
        CHECK (v[0].pitch == 72, "transpose");
        Analysis a; a.key = 42; a.tempo = 133; a.keyRoot = 9; a.keyMinor = true;
        StemResult s; s.kind = StemKind::Other; s.name = "Other"; s.mode = Transcribe::Poly; s.notes = v;
        s.curve.midi = { 0, 60.5f }; s.curve.confidence = { 0, 1 };
        a.stems = { s };
        Chord c; c.root = 2; c.quality = 1; c.start = 1; c.end = 2; a.chords = { c };
        Analysis b;
        CHECK (b.deserialiseData (a.serialiseData()), "deserialise");
        CHECK (b.key == 42 && b.tempo == 133 && b.keyName() == "A minor" && b.stems.size() == 1 && b.stems[0].notes.size() == 2 && b.chords[0].name() == "Dm", "contents");
        std::vector<uint8_t> junk { 1, 2, 3 };
        CHECK (! b.deserialiseData (junk), "junk rejected");
    });

    run ("Pipeline without separation (audio to MIDI) and the cache", [&] {
        BasicPitch bp;
        bp.loadEmbedded();
        std::vector<SynthNote> notes { { 0.2, 0.8, 62 }, { 0.9, 1.5, 65 }, { 1.6, 2.4, 69 } };
        AudioBuffer in; in.sampleRate = 48000.0; in.ch.push_back (render (notes, 48000.0, 2.8, 6, 2.0f));
        PipelineOptions o; o.split = false; o.mixMode = Transcribe::Poly;
        Analysis a; std::string err;
        float lastP = -1; bool monotonic = true;
        const bool ok = runPipeline (in, o, nullptr, bp, a, [&] (float p, const std::string&) { monotonic = monotonic && p >= lastP - 1e-6f; lastP = p; }, nullptr, err);
        CHECK (ok, "pipeline: %s", err.c_str());
        CHECK (monotonic && lastP == 1.0f, "progress");
        CHECK (a.stems.size() == 1 && a.stems[0].kind == StemKind::Mix, "one stem");
        if (! a.stems.empty()) for (auto& n : notes) CHECK (hasNote (a.stems[0].notes, n.pitch, n.start, 0.08), "missing %d", n.pitch);
        const auto dir = (std::filesystem::temp_directory_path() / "ss_cache_test").string();
        AnalysisCache cache (dir);
        CHECK (cache.save (a), "save");
        CHECK (cache.has (a.key), "has");
        Analysis b;
        CHECK (cache.load (a.key, b), "load");
        CHECK (b.stems.size() == 1 && b.stems[0].audio.numFrames() == a.stems[0].audio.numFrames() && b.stems[0].notes.size() == a.stems[0].notes.size(), "cached copy");
        std::filesystem::remove_all (dir);
        // cancelling stops early
        std::atomic<bool> cancel { true };
        CHECK (! runPipeline (in, o, nullptr, bp, a, {}, &cancel, err) && err == "cancelled", "cancel: %s", err.c_str());
    });

    if (const char* model = std::getenv ("STEMSCRIBE_TEST_MODEL"))
    {
        run ("HTDemucs separation (demucs.cpp)", [&] {
            std::string err;
            auto sep = createDemucsSeparator (model, 2, err);
            CHECK (sep != nullptr, "load: %s", err.c_str());
            if (! sep) return;
            // bass tone + drums + a high melody, 10 s
            std::mt19937 rng (3);
            std::vector<float> x ((size_t) (10 * sr), 0.0f);
            for (double t = 0.1; t < 9.5; t += 0.5) addKick (x, sr, t, 0.6f);
            addTone (x, sr, 0.0, 9.8, midiToHz (36), 0.25f, 4);
            addTone (x, sr, 0.0, 9.8, midiToHz (76), 0.1f, 3);
            AudioBuffer in; in.sampleRate = sr; in.ch = { x, x };
            std::vector<AudioBuffer> out;
            CHECK (sep->separate (in, out, {}, nullptr, err), "separate: %s", err.c_str());
            CHECK (out.size() == 4, "stems %zu", out.size());
            if (out.size() != 4) return;
            // the stems add back up to (roughly) the input
            double e = 0, ref = 0;
            for (size_t i = 0; i < x.size(); ++i)
            {
                float s = 0;
                for (auto& o : out) s += o.ch[0][i];
                e += (s - x[i]) * (s - x[i]); ref += x[i] * x[i];
            }
            CHECK (e / ref < 0.1, "reconstruction error %g", e / ref);
            float pk[4];
            for (int k = 0; k < 4; ++k) pk[k] = out[(size_t) k].peak();
            std::printf ("    peaks drums %.2f bass %.2f other %.2f vocals %.2f\n", pk[0], pk[1], pk[2], pk[3]);
            CHECK (pk[0] > 0.2f && pk[1] > 0.1f, "drums and bass found");
        });
    }
    else std::printf ("(HTDemucs test skipped: set STEMSCRIBE_TEST_MODEL to the ggml weights)\n");

    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
