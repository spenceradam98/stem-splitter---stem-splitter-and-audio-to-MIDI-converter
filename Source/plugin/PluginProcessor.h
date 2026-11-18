// stem-scribe processor: captures or loads audio, runs the analysis on a worker thread, then plays
// the stems back in sync with the host (solo / mute / level per stem) and can send the MIDI out live.
// Heavy work never runs on the audio thread: it only reads a prepared PlaybackSet.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "Pipeline.h"

#include <array>
#include <atomic>
#include <memory>

static constexpr int kMaxStems = 7;

class StemScribeProcessor : public juce::AudioProcessor,
                            public juce::ChangeBroadcaster,
                            private juce::AsyncUpdater
{
public:
    StemScribeProcessor();
    ~StemScribeProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- sources (message thread) ----
    juce::String loadAudioFile (const juce::File& f);          // returns an error or ""; starts analysis
    void setCapture (bool armed);                              // records the input while the host plays
    bool isCaptureArmed() const { return captureArmed.load(); }
    double capturedSeconds() const;

    // ---- analysis ----
    struct Settings { bool split = true; ss::Transcribe mixMode = ss::Transcribe::Poly; bool chords = true; bool useHostTempo = true; };
    Settings settings;
    void startAnalysis();
    void cancelAnalysis();
    bool isAnalysing() const;
    float getProgress() const { return progress.load(); }
    juce::String getStatusText() const;
    juce::String getSourceName() const { return sourceName; }
    juce::String modelPath() const;
    void setModelPath (const juce::File& f);
    bool modelFound() const { return ss::demucsAvailable() && juce::File (modelPath()).existsAsFile(); }

    // The current analysis (message thread only). Call analysisEdited() after changing notes or chords.
    ss::Analysis* getAnalysis() { return analysis.get(); }
    void analysisEdited (bool recomputeChords = false);
    void retranscribe (int stem, ss::Transcribe mode);        // runs on the worker thread
    void undo();
    void setAnalysisForTesting (std::unique_ptr<ss::Analysis> a, const juce::String& name);   // screenshot tool
    void redo();
    void pushUndo();                                          // snapshot before an edit
    void dropUndo() { if (! undoStack.empty()) undoStack.pop_back(); }
    bool canUndo() const { return ! undoStack.empty(); }
    bool canRedo() const { return ! redoStack.empty(); }

    // ---- playback ----
    struct StemControl { std::atomic<float> level { 1.0f }; std::atomic<bool> mute { false }, solo { false }, midiOut { true }; };
    std::array<StemControl, kMaxStems> stemControls;
    std::atomic<bool> playStems { true };       // false = pass the input through
    std::atomic<bool> midiOutEnabled { false };
    std::atomic<double> songStart { 0.0 };      // host time (seconds) at which the analysed audio starts
    double getPlayheadSeconds() const { return playhead.load(); }   // song time
    bool isHostPlaying() const { return hostPlaying.load(); }
    void setPreviewPlaying (bool p);
    bool isPreviewPlaying() const { return previewPlaying.load(); }
    void setPreviewPosition (double songSeconds);
    void placeAtPlayhead();                     // align the song start with the host's current position
    double hostPositionSeconds() const { return hostPos.load(); }

    // ---- export (message thread): files land in Documents/stem-scribe/<song>/ ----
    juce::File exportFolder() const;
    juce::File exportStemWav (int stem);
    juce::File exportStemMidi (int stem);
    juce::File exportChordsMidi();
    juce::File exportAllMidi();
    juce::Array<juce::File> exportEverything();

    // ---- UI state ----
    juce::String themeName { "Studio Night" };
    int editorW = 1180, editorH = 760;
    int selectedStem = 0;

private:
    struct PlayAudio;
    struct MidiEv;
    void handleAsyncUpdate() override;
    void installAnalysis (std::unique_ptr<ss::Analysis> a, std::unique_ptr<PlayAudio> audio);
    void rebuildMidi();
    void resampleIfNeeded();
    std::vector<uint8_t> pendingSessionData;      // edits from a restored session, applied after loading
    void finishCapture();

    struct PlayAudio
    {
        double rate = 44100.0, duration = 0.0;
        std::vector<std::vector<std::vector<float>>> stems;   // [stem][ch][sample] at the host rate
    };
    struct MidiEv { double t; int stem; juce::MidiMessage m; };
    std::unique_ptr<PlayAudio> playAudio;
    std::unique_ptr<std::vector<MidiEv>> playMidi;
    juce::CriticalSection playbackLock;                       // the audio thread only try-locks it
    std::unique_ptr<PlayAudio> makePlayAudio (const ss::Analysis& a, double rate) const;

    // results from the worker thread, applied on the message thread
    juce::CriticalSection resultLock;
    std::vector<std::function<void()>> pendingResults;
    std::atomic<bool> captureFinished { false };
    void runJob (std::function<std::function<void()> (std::atomic<bool>& cancel)> job, const juce::String& firstStage);
    void setStage (const juce::String& s, float p);

    class Worker;
    std::unique_ptr<Worker> worker;
    std::unique_ptr<ss::Analysis> analysis;
    std::vector<std::vector<uint8_t>> undoStack, redoStack;
    juce::String sourceName, sourcePath;
    uint64_t pendingKey = 0;          // analysis key restored from the session, loaded from the cache
    ss::BasicPitch basicPitch;
    std::unique_ptr<ss::Separator> separator;
    juce::String separatorPath;
    juce::String lastError;
    std::atomic<float> progress { 0.0f };
    juce::String stage;
    juce::CriticalSection stageLock;
    std::unique_ptr<ss::AnalysisCache> cache;
    std::unique_ptr<juce::PropertiesFile> globalProps;

    // capture (audio thread writes into a buffer prepared on the message thread)
    std::atomic<bool> captureArmed { false }, captureRunning { false };
    juce::AudioBuffer<float> captureBuf;
    std::atomic<int> captureLen { 0 };
    std::atomic<double> captureStartHost { 0.0 }, captureStartPpq { -1.0 }, captureBpm { 0.0 };
    double captureRate = 44100.0;

    // transport
    double sampleRateHz = 44100.0;
    std::atomic<double> playhead { 0.0 }, hostPos { 0.0 };
    std::atomic<bool> hostPlaying { false }, previewPlaying { false };
    std::atomic<double> previewPos { 0.0 };
    double lastSongTime = -1.0;
    bool midiNotesHanging = false;
    std::atomic<double> hostBpm { 0.0 }, hostPpq { 0.0 };

    friend class Worker;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemScribeProcessor)
};
