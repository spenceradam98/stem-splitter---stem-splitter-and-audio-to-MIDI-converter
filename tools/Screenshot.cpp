// Renders the editor in every theme to PNG files (docs/screenshot.png and docs/screenshots/).
// The stems are synthesised (vocal line, drums, bass, chords) and run through the real
// transcription, tempo and chord code, so the roll shows genuine analysis output.
// Build with -DSTEMSCRIBE_BUILD_SCREENSHOT=ON; run: stemscribe_screenshot <docs dir> (needs a display, e.g. Xvfb).
#include "../Source/plugin/PluginProcessor.h"
#include "../Source/plugin/PluginEditor.h"

#include "Dsp.h"
#include "Harmony.h"

#include <random>

static void tone (std::vector<float>& x, double sr, double a, double b, float midi, float amp, int harm, float vib = 0, float rolloff = 1.5f)
{
    double ph = 0;
    for (long i = (long) (a * sr); i < (long) (b * sr) && i < (long) x.size(); ++i)
    {
        const double t = i / sr - a;
        const double f = ss::midiToHz (midi) * std::pow (2.0, vib * std::sin (2 * M_PI * 5.2 * t) * std::min (1.0, t * 2) / 12.0);
        ph += 2 * M_PI * f / sr;
        double s = 0;
        for (int h = 1; h <= harm; ++h) s += std::sin (h * ph) / std::pow ((double) h, (double) rolloff);
        const double env = std::min (1.0, t / 0.015) * std::min (1.0, (b - a - t) / 0.03);
        x[(size_t) i] += (float) (amp * env * s);
    }
}

static std::unique_ptr<ss::Analysis> makeSong()
{
    const double sr = 44100.0, beat = 0.5, dur = 16.0;   // 120 BPM, 8 bars
    const size_t N = (size_t) (dur * sr);
    std::vector<float> voc (N), drm (N), bas (N), oth (N);
    std::mt19937 rng (5);
    std::uniform_real_distribution<float> u (-1, 1);
    const int prog[4][3] = { { 57, 60, 64 }, { 53, 57, 60 }, { 48, 52, 55 }, { 55, 59, 62 } };   // Am F C G
    const int roots[4] = { 33, 29, 36, 31 };
    for (int bar = 0; bar < 8; ++bar)
    {
        const double t = bar * 4 * beat;
        for (int k = 0; k < 3; ++k) tone (oth, sr, t, t + 4 * beat - 0.02, (float) prog[bar % 4][k], 0.07f, 6, 0, 2.0f);
        for (int q = 0; q < 4; ++q) tone (bas, sr, t + q * beat, t + q * beat + beat * (q == 3 ? 0.45 : 0.9), (float) (roots[bar % 4] + (q == 3 ? 7 : 0)), 0.25f, 6);
        for (int q = 0; q < 8; ++q)
        {
            const double h = t + q * beat / 2;
            if (q % 4 == 0) { double p = 0; for (long i = 0; i < (long) (0.25 * sr); ++i) { const long k = (long) (h * sr) + i; if (k >= (long) N) break; p += 2 * M_PI * (50 + 90 * std::exp (-i / sr * 30)) / sr; drm[(size_t) k] += 0.8f * (float) (std::sin (p) * std::exp (-i / sr * 12)); } }
            if (q % 4 == 2) for (long i = 0; i < (long) (0.2 * sr); ++i) { const long k = (long) (h * sr) + i; if (k < (long) N) drm[(size_t) k] += 0.45f * (u (rng) * 0.7f + 0.4f * (float) std::sin (2 * M_PI * 190 * i / sr)) * (float) std::exp (-i / sr * 22); }
            float prev = 0, hp = 0;
            for (long i = 0; i < (long) (0.06 * sr); ++i) { const long k = (long) (h * sr) + i; if (k >= (long) N) break; const float n = u (rng); hp = 0.4f * (hp + n - prev); prev = n; drm[(size_t) k] += 0.25f * hp * (float) std::exp (-i / sr * 70); }
        }
    }
    // a vocal line with vibrato and a scoop
    const float mel[] = { 69, 72, 71, 69, 67, 69, 64, 65, 67, 69, 71, 72, 74, 72, 71, 69 };
    for (int i = 0; i < 16; ++i)
    {
        const double a = i * 2 * beat + 0.05, b = a + (i % 4 == 3 ? 1.8 : 0.85);
        tone (voc, sr, a, b, mel[i], 0.12f, 8, i % 4 == 3 ? 0.45f : 0.12f);
    }
    auto a = std::make_unique<ss::Analysis>();
    a->duration = dur;
    a->separatorName = "HTDemucs 4-stem";
    const std::pair<ss::StemKind, std::vector<float>*> list[] = { { ss::StemKind::Vocals, &voc }, { ss::StemKind::Drums, &drm }, { ss::StemKind::Bass, &bas }, { ss::StemKind::Other, &oth } };
    ss::BasicPitch bp;
    bp.loadEmbedded();
    for (auto& [k, x] : list)
    {
        ss::StemResult s;
        s.kind = k; s.name = ss::stemKindName (k); s.mode = ss::defaultTranscribeFor (k);
        s.audio.sampleRate = sr; s.audio.ch = { *x, *x };
        ss::transcribeStem (s, bp, 0);
        a->stems.push_back (std::move (s));
    }
    const auto te = ss::estimateTempo (drm, sr);
    a->tempo = te.bpm; a->beatOffset = te.offset;
    ss::updateHarmony (*a);
    a->key = 0x5c4e;
    return a;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File docs (argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]) : juce::File::getCurrentWorkingDirectory());
    docs.getChildFile ("screenshots").createDirectory();
    auto song = makeSong();
    std::printf ("tempo %.1f, key %s, chords %zu\n", song->tempo, song->keyName().c_str(), song->chords.size());
    for (auto& s : song->stems) std::printf ("  %s: %zu notes\n", s.name.c_str(), s.notes.size());
    int idx = 0;
    for (auto& theme : allThemes())
    {
        StemScribeProcessor proc;
        proc.prepareToPlay (44100.0, 512);
        proc.themeName = theme.name;
        proc.selectedStem = idx == 1 ? 1 : (idx == 2 ? 3 : (idx == 4 ? 2 : 0));
        proc.setAnalysisForTesting (std::make_unique<ss::Analysis> (*song), "Demo song");
        proc.setPreviewPosition (5.3);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        ed->setSize (1280, 780);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (theme.spectrogram ? 2500 : 400);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat png;
        const auto name = "theme-" + theme.name.toLowerCase().replaceCharacter (' ', '-') + ".png";
        for (auto f : { docs.getChildFile ("screenshots").getChildFile (name), idx == 0 ? docs.getChildFile ("screenshot.png") : juce::File() })
        {
            if (f == juce::File()) continue;
            f.deleteFile();
            juce::FileOutputStream os (f);
            png.writeImageToStream (img, os);
            std::printf ("wrote %s\n", f.getFullPathName().toRawUTF8());
        }
        ++idx;
    }
    return 0;
}
