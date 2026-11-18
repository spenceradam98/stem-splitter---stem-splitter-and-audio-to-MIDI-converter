// Plugin smoke test (local, needs no display): loads a WAV through the processor without the
// separation model, waits for the analysis, then plays it through processBlock with a fake host
// transport and checks that audio and MIDI come out, that state round-trips, and that capture works.
//   stemscribe_smoketest song.wav
#include "../Source/plugin/PluginProcessor.h"

struct FakeHead : juce::AudioPlayHead
{
    double time = 0, bpm = 120; bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing);
        p.setTimeInSeconds (time);
        p.setTimeInSamples ((juce::int64) (time * 48000.0));
        p.setBpm (bpm);
        p.setPpqPosition (time * bpm / 60.0);
        return p;
    }
};

static bool waitIdle (StemScribeProcessor& p, int ms)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        if (! p.isAnalysing() && p.getAnalysis()) return true;
    }
    return false;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::printf ("usage: stemscribe_smoketest song.wav\n"); return 2; }
    int fails = 0;
    auto check = [&] (bool c, const char* what) { std::printf ("%s %s\n", c ? "ok  " : "FAIL", what); fails += ! c; };

    StemScribeProcessor p;
    p.prepareToPlay (48000.0, 512);
    p.settings.split = false;
    p.settings.mixMode = ss::Transcribe::Poly;
    check (p.loadAudioFile (juce::File (argv[1])).isEmpty(), "load file");
    check (waitIdle (p, 120000), "analysis finished");
    auto* a = p.getAnalysis();
    check (a && a->stems.size() == 1 && ! a->stems[0].notes.empty(), "notes found");
    std::printf ("     status: %s\n", p.getStatusText().toRawUTF8());

    FakeHead head;
    p.setPlayHead (&head);
    p.midiOutEnabled = true;
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    float peak = 0; int noteOns = 0;
    for (int b = 0; b < 48000 * 6 / 512; ++b)
    {
        buf.clear();
        p.processBlock (buf, midi);
        peak = juce::jmax (peak, buf.getMagnitude (0, 512));
        for (const auto m : midi) noteOns += m.getMessage().isNoteOn();
        head.time += 512 / 48000.0;
    }
    check (peak > 0.05f, "stems play with the host transport");
    check (noteOns > 0, "live MIDI out");
    p.stemControls[0].mute = true;
    buf.clear(); p.processBlock (buf, midi);
    check (buf.getMagnitude (0, 512) == 0.0f, "mute");
    p.stemControls[0].mute = false;

    // state round trip (with an edit)
    p.pushUndo();
    a->stems[0].notes.erase (a->stems[0].notes.begin());
    p.analysisEdited();
    const size_t n = a->stems[0].notes.size();
    juce::MemoryBlock state;
    p.getStateInformation (state);
    StemScribeProcessor q;
    q.prepareToPlay (48000.0, 512);
    q.setStateInformation (state.getData(), (int) state.getSize());
    check (waitIdle (q, 60000), "restored session reloads (from the cache)");
    check (q.getAnalysis() && q.getAnalysis()->stems[0].notes.size() == n, "edits restored with the session");
    p.undo();
    check (p.getAnalysis()->stems[0].notes.size() == n + 1, "undo");

    // exports
    auto files = p.exportEverything();
    bool allExist = files.size() >= 3;
    for (auto& f : files) allExist = allExist && f.existsAsFile() && f.getSize() > 0;
    check (allExist, "export everything");
    for (auto& f : files) f.deleteFile();
    p.exportFolder().deleteRecursively();

    // capture 3 s of a tone from the host
    p.setCapture (true);
    head.time = 10.0;
    for (int b = 0; b < 48000 * 3 / 512; ++b)
    {
        for (int i = 0; i < 512; ++i) { const float v = 0.3f * (float) std::sin (2 * M_PI * 220.0 * (b * 512 + i) / 48000.0); buf.setSample (0, i, v); buf.setSample (1, i, v); }
        p.processBlock (buf, midi);
        head.time += 512 / 48000.0;
    }
    head.playing = false;
    p.processBlock (buf, midi);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    check (waitIdle (p, 60000) && p.getSourceName() == "Capture" && std::abs (p.songStart.load() - 10.0) < 0.02, "capture from the host timeline");
    if (auto* c = p.getAnalysis()) { bool has57 = false; for (auto& nn : c->stems[0].notes) has57 = has57 || nn.pitch == 57; check (has57, "captured A3 transcribed"); }
    p.setPlayHead (nullptr);
    std::printf ("%s\n", fails ? "SMOKE TEST FAILED" : "smoke test passed");
    return fails ? 1 : 0;
}
