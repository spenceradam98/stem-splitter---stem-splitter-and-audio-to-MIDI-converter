#include "Pipeline.h"

#include "DrumTranscriber.h"
#include "Harmony.h"
#include "MonoTranscriber.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace ss
{
namespace fs = std::filesystem;

std::string hexKey (uint64_t key)
{
    char b[17];
    std::snprintf (b, sizeof (b), "%016llx", (unsigned long long) key);
    return b;
}

uint64_t analysisKey (const AudioBuffer& input, bool split)
{
    return input.hash() ^ (split ? 0x9e3779b97f4a7c15ull : 0ull);   // split and unsplit analyses are cached apart
}

void transcribeStem (StemResult& s, const BasicPitch& bp, int threads, const std::atomic<bool>* cancel,
                     const std::function<void (float)>& progress)
{
    s.notes.clear();
    s.curve = PitchCurve();
    const auto mono = s.audio.mono();
    const double sr = s.audio.sampleRate;
    switch (s.mode)
    {
        case Transcribe::Off: break;
        case Transcribe::Mono:
        {
            const auto set = s.kind == StemKind::Bass ? MonoSettings::bass() : MonoSettings::vocals();
            s.curve = trackPitch (mono, sr, set, cancel);
            if (cancel && cancel->load()) return;
            s.notes = segmentNotes (s.curve, mono, sr, set);
            break;
        }
        case Transcribe::Poly:
        {
            PolySettings ps;
            if (s.kind == StemKind::Bass) { ps.maxHz = 500.0f; }
            auto out = bp.infer (mono, sr, threads, progress, cancel);
            if (cancel && cancel->load()) return;
            s.notes = BasicPitch::toNotes (std::move (out), ps);
            break;
        }
        case Transcribe::Drums: s.notes = transcribeDrums (mono, sr); break;
    }
    if (progress) progress (1.0f);
}

void updateHarmony (Analysis& a)
{
    std::vector<const std::vector<Note>*> harm, keySets;
    const std::vector<Note>* bass = nullptr;
    for (auto& s : a.stems)
    {
        if (s.mode == Transcribe::Drums || s.mode == Transcribe::Off || s.kind == StemKind::Drums) continue;
        keySets.push_back (&s.notes);
        if (s.kind == StemKind::Bass) bass = &s.notes;
        else if (s.kind != StemKind::Vocals || a.stems.size() == 1) harm.push_back (&s.notes);
    }
    if (harm.empty()) for (auto& s : a.stems) if (s.kind == StemKind::Vocals) harm.push_back (&s.notes);
    detectKey (keySets, a.keyRoot, a.keyMinor);
    a.chords = detectChords (harm, bass, a.duration, a.tempo, a.beatOffset);
}

bool runPipeline (const AudioBuffer& inputRaw, const PipelineOptions& o, Separator* sep, const BasicPitch& bp,
                  Analysis& out, const ProgressFn& progress, const std::atomic<bool>* cancel, std::string& error)
{
    auto report = [&] (float p, const std::string& s) { if (progress) progress (std::clamp (p, 0.0f, 1.0f), s); };
    auto cancelled = [&] { if (cancel && cancel->load()) { error = "cancelled"; return true; } return false; };
    Analysis a;
    report (0.0f, "Preparing audio");
    AudioBuffer input = inputRaw.toStereo();
    if (std::abs (input.sampleRate - 44100.0) > 0.5) input = resample (input, 44100.0);
    a.sampleRate = 44100.0;
    a.duration = input.seconds();
    if (input.numFrames() < 4410) { error = "the audio is too short to analyse"; return false; }

    const bool split = o.split && sep && sep->isReady();
    a.key = analysisKey (inputRaw, split);
    const float sepShare = split ? 0.8f : 0.0f;
    if (split)
    {
        std::vector<AudioBuffer> stems;
        a.separatorName = sep->name();
        if (! sep->separate (input, stems, [&] (float p, const std::string& s) { report (p * sepShare, s); }, cancel, error)) return false;
        const auto kinds = sep->stemKinds();
        // present stems in a musician-friendly order: vocals, drums, bass, other, guitar, piano
        for (StemKind k : { StemKind::Vocals, StemKind::Drums, StemKind::Bass, StemKind::Other, StemKind::Guitar, StemKind::Piano })
            for (size_t i = 0; i < kinds.size() && i < stems.size(); ++i)
                if (kinds[i] == k)
                {
                    StemResult r;
                    r.kind = k; r.name = stemKindName (k); r.audio = std::move (stems[i]); r.mode = defaultTranscribeFor (k);
                    a.stems.push_back (std::move (r));
                }
    }
    else
    {
        if (o.split && ! split) a.separatorName = "none";
        StemResult r;
        r.kind = StemKind::Mix; r.name = "Mix"; r.audio = input; r.mode = o.mixMode;
        a.stems.push_back (std::move (r));
    }
    if (cancelled()) return false;

    // tempo: the host's if known, else from the drums (or the mix)
    if (o.hostBpm > 0) { a.tempo = o.hostBpm; a.beatOffset = o.hostBeatOffset; a.tempoFromHost = true; }
    else
    {
        report (sepShare, "Finding the tempo");
        const int d = a.findStem (StemKind::Drums);
        const auto te = estimateTempo (d >= 0 ? a.stems[(size_t) d].audio.mono() : input.mono(), 44100.0);
        a.tempo = te.bpm; a.beatOffset = te.offset;
    }

    if (o.transcribe)
    {
        const float span = (1.0f - sepShare) * 0.95f;
        const float per = span / (float) a.stems.size();
        for (size_t i = 0; i < a.stems.size(); ++i)
        {
            auto& s = a.stems[i];
            const float base = sepShare + 0.02f + per * (float) i;
            const std::string what = "Transcribing " + s.name;
            report (base, what);
            transcribeStem (s, bp, o.threads, cancel, [&] (float p) { report (base + per * p, what); });
            if (cancelled()) return false;
        }
        if (o.chords) { report (0.98f, "Detecting key and chords"); updateHarmony (a); }
    }
    report (1.0f, "Done");
    out = std::move (a);
    return true;
}

// ------------------------------------------------------------------------------------------
AnalysisCache::AnalysisCache (std::string rootDir) : dir (std::move (rootDir)) {}

std::string AnalysisCache::defaultRoot()
{
#if defined(_WIN32)
    if (const char* la = std::getenv ("LOCALAPPDATA")) return std::string (la) + "\\stem-scribe\\cache";
    return "stem-scribe-cache";
#elif defined(__APPLE__)
    if (const char* h = std::getenv ("HOME")) return std::string (h) + "/Library/Caches/stem-scribe";
    return "/tmp/stem-scribe-cache";
#else
    if (const char* x = std::getenv ("XDG_CACHE_HOME")) return std::string (x) + "/stem-scribe";
    if (const char* h = std::getenv ("HOME")) return std::string (h) + "/.cache/stem-scribe";
    return "/tmp/stem-scribe-cache";
#endif
}

std::string AnalysisCache::folderFor (uint64_t key) const { return (fs::path (dir) / hexKey (key)).string(); }

std::string AnalysisCache::stemPath (uint64_t key, const StemResult& s) const
{
    return (fs::path (folderFor (key)) / (std::string (stemKindName (s.kind)) + ".wav")).string();
}

bool AnalysisCache::has (uint64_t key) const
{
    std::error_code ec;
    return fs::is_regular_file (fs::path (folderFor (key)) / "analysis.bin", ec);
}

bool AnalysisCache::saveData (const Analysis& a) const
{
    std::error_code ec;
    fs::create_directories (folderFor (a.key), ec);
    const auto bytes = a.serialiseData();
    const auto path = fs::path (folderFor (a.key)) / "analysis.bin";
    const auto tmp = fs::path (path.string() + ".tmp");
    {
        std::ofstream f (tmp, std::ios::binary);
        if (! f) return false;
        f.write ((const char*) bytes.data(), (std::streamsize) bytes.size());
        if (! f) return false;
    }
    fs::rename (tmp, path, ec);
    return ! ec;
}

bool AnalysisCache::save (const Analysis& a) const
{
    std::error_code ec;
    fs::create_directories (folderFor (a.key), ec);
    for (auto& s : a.stems)
        if (! writeWav (stemPath (a.key, s), s.audio, 32)) return false;
    return saveData (a);
}

bool AnalysisCache::load (uint64_t key, Analysis& out) const
{
    std::ifstream f (fs::path (folderFor (key)) / "analysis.bin", std::ios::binary);
    if (! f) return false;
    std::vector<uint8_t> bytes ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    Analysis a;
    if (! a.deserialiseData (bytes)) return false;
    for (auto& s : a.stems)
        if (! readWav (stemPath (key, s), s.audio)) return false;
    out = std::move (a);
    return true;
}
} // namespace ss
