// stem-scribe command-line tool (also the Phase 0 benchmark).
//   stemscribe_cli song.wav out_dir [--no-split] [--mode poly|mono|drums] [--model file]
//                  [--threads n] [--bpm x] [--no-chords]
// Writes one WAV per stem, one .mid per transcribed stem, all.mid (every stem + chords),
// chords.txt and timings to stdout.
#include "DrumTranscriber.h"
#include "MidiFile.h"
#include "Pipeline.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace ss;

static void writeBytes (const std::filesystem::path& p, const std::vector<uint8_t>& b)
{
    std::ofstream f (p, std::ios::binary);
    f.write ((const char*) b.data(), (std::streamsize) b.size());
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: " << argv[0] << " song.wav out_dir [--no-split] [--mode poly|mono|drums] [--model file] [--threads n] [--bpm x] [--no-chords]\n";
        return 2;
    }
    PipelineOptions o;
    std::string modelPath;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string (argv[++i]) : std::string(); };
        if (a == "--no-split") o.split = false;
        else if (a == "--no-chords") o.chords = false;
        else if (a == "--mode") { const auto m = next(); o.mixMode = m == "mono" ? Transcribe::Mono : m == "drums" ? Transcribe::Drums : Transcribe::Poly; }
        else if (a == "--model") modelPath = next();
        else if (a == "--threads") o.threads = std::atoi (next().c_str());
        else if (a == "--bpm") o.hostBpm = std::atof (next().c_str());
        else { std::cerr << "unknown option " << a << "\n"; return 2; }
    }
    AudioBuffer in;
    std::string err;
    if (! readWav (argv[1], in, &err)) { std::cerr << "cannot read " << argv[1] << ": " << err << "\n"; return 1; }
    std::cout << "input: " << in.seconds() << " s, " << in.sampleRate << " Hz, " << in.numChannels() << " ch\n";

    using clock = std::chrono::steady_clock;
    std::unique_ptr<Separator> sep;
    if (o.split)
    {
        if (modelPath.empty()) modelPath = findDefaultModel();
        const auto t0 = clock::now();
        sep = createDemucsSeparator (modelPath, o.threads, err);
        if (! sep) { std::cerr << "separation unavailable (" << err << "); use --no-split for audio-to-MIDI only\n"; return 1; }
        std::cout << "model loaded in " << std::chrono::duration<double> (clock::now() - t0).count() << " s\n";
    }
    BasicPitch bp;
    if (! bp.loadEmbedded()) { std::cerr << "Basic Pitch model failed to load\n"; return 1; }

    Analysis a;
    std::string lastStage;
    auto stageStart = clock::now();
    const auto t0 = clock::now();
    const bool ok = runPipeline (in, o, sep.get(), bp, a, [&] (float p, const std::string& s) {
        if (s != lastStage)
        {
            const auto now = clock::now();
            if (! lastStage.empty()) std::cout << "  " << lastStage << ": " << std::chrono::duration<double> (now - stageStart).count() << " s\n";
            lastStage = s; stageStart = now;
        }
        (void) p;
    }, nullptr, err);
    if (! ok) { std::cerr << "analysis failed: " << err << "\n"; return 1; }
    const double total = std::chrono::duration<double> (clock::now() - t0).count();
    std::cout << "total " << total << " s (" << in.seconds() / total << "x realtime)\n";
    std::cout << "tempo " << a.tempo << " BPM, key " << a.keyName() << "\n";

    std::filesystem::path out (argv[2]);
    std::filesystem::create_directories (out);
    for (size_t i = 0; i < a.stems.size(); ++i)
    {
        auto& s = a.stems[i];
        if (a.stems.size() > 1) writeWav ((out / (s.name + ".wav")).string(), s.audio, 24);
        if (! s.notes.empty()) writeBytes (out / (s.name + ".mid"), exportStemMidi (a, (int) i));
        std::cout << s.name << ": " << s.notes.size() << " notes";
        if (s.mode == Transcribe::Drums)
        {
            int k = 0, sn = 0, h = 0;
            for (auto& n : s.notes) { k += n.pitch == gm::kick; sn += n.pitch == gm::snare; h += n.pitch == gm::closedHat || n.pitch == gm::openHat; }
            std::cout << " (kick " << k << ", snare " << sn << ", hats " << h << ")";
        }
        std::cout << "\n";
    }
    writeBytes (out / "all.mid", exportAllMidi (a));
    if (! a.chords.empty())
    {
        writeBytes (out / "chords.mid", exportChordMidi (a));
        std::ofstream c (out / "chords.txt");
        for (auto& ch : a.chords) c << ch.start << "\t" << ch.end << "\t" << ch.name() << "\n";
        std::cout << "chords: " << a.chords.size() << " segments\n";
    }
    return 0;
}
