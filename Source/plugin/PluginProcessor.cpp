#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "MidiFile.h"

using namespace juce;

// ------------------------------------------------------------------------------------------
class StemScribeProcessor::Worker : public Thread
{
public:
    explicit Worker (StemScribeProcessor& p) : Thread ("stem-scribe analysis"), owner (p) {}
    ~Worker() override { cancelAndWait(); }

    void start (std::function<std::function<void()> (std::atomic<bool>&)> j)
    {
        cancelAndWait();
        job = std::move (j);
        cancel = false;
        startThread (Thread::Priority::low);
    }
    void cancelAndWait() { cancel = true; stopThread (-1); }
    void requestCancel() { cancel = true; }

    void run() override
    {
        std::function<void()> apply;
        try { apply = job (cancel); }
        catch (const std::exception& e) { const String msg (e.what()); apply = [this, msg] { owner.lastError = msg; }; }
        if (apply)
        {
            const ScopedLock l (owner.resultLock);
            owner.pendingResults.push_back (std::move (apply));
        }
        owner.triggerAsyncUpdate();
    }

    std::atomic<bool> cancel { false };

private:
    StemScribeProcessor& owner;
    std::function<std::function<void()> (std::atomic<bool>&)> job;
};

// ------------------------------------------------------------------------------------------
StemScribeProcessor::StemScribeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true))
{
    basicPitch.loadEmbedded();
    cache = std::make_unique<ss::AnalysisCache> (ss::AnalysisCache::defaultRoot());
    PropertiesFile::Options o;
    o.applicationName = "stem-scribe";
    o.folderName = "YourBrand/stem-scribe";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
    globalProps = std::make_unique<PropertiesFile> (o);
    worker = std::make_unique<Worker> (*this);
}

StemScribeProcessor::~StemScribeProcessor()
{
    cancelPendingUpdate();
    worker.reset();
}

bool StemScribeProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    if (out != AudioChannelSet::stereo() && out != AudioChannelSet::mono()) return false;
    const auto in = l.getMainInputChannelSet();
    return in.isDisabled() || in == AudioChannelSet::stereo() || in == AudioChannelSet::mono();
}

void StemScribeProcessor::prepareToPlay (double sr, int)
{
    sampleRateHz = sr;
    resampleIfNeeded();
}

void StemScribeProcessor::resampleIfNeeded()
{
    const double sr = sampleRateHz;
    bool needs = false;
    {
        const ScopedLock l (playbackLock);
        needs = playAudio && std::abs (playAudio->rate - sr) > 0.5;
    }
    if (needs && analysis && ! isAnalysing())
    {
        // stems are resampled to the host rate in the background
        auto copy = std::make_shared<ss::Analysis>();
        copy->stems = analysis->stems;
        runJob ([this, copy, sr] (std::atomic<bool>&) -> std::function<void()> {
            auto pa = makePlayAudio (*copy, sr);
            return [this, p = std::shared_ptr<PlayAudio> (std::move (pa))]() mutable {
                auto fresh = std::make_unique<PlayAudio> (std::move (*p));
                std::unique_ptr<PlayAudio> old;
                { const ScopedLock l (playbackLock); old = std::move (playAudio); playAudio = std::move (fresh); }
                setStage ({}, 0.0f);
                sendChangeMessage();
            };
        }, "Preparing playback");
    }
}

// ------------------------------------------------------------------------------------------
void StemScribeProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedNoDenormals noDenormals;
    midi.clear();
    const int n = buffer.getNumSamples();
    const int inCh = getTotalNumInputChannels();
    for (int c = inCh; c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);

    bool playing = false;
    double hostTime = 0.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto t = pos->getTimeInSeconds()) hostTime = *t;
            else if (auto s = pos->getTimeInSamples()) hostTime = (double) *s / sampleRateHz;
            if (auto b = pos->getBpm()) hostBpm = *b;
            if (auto q = pos->getPpqPosition()) hostPpq = *q;
        }
    const bool standalone = wrapperType == wrapperType_Standalone;
    hostPlaying = playing;
    hostPos = hostTime;

    // ---- capture ----
    if (captureArmed.load())
    {
        const bool rolling = playing || standalone;
        if (rolling)
        {
            if (! captureRunning.load())
            {
                captureRunning = true;
                captureStartHost = standalone && ! playing ? 0.0 : hostTime;
                captureStartPpq = playing ? hostPpq.load() : -1.0;
                captureBpm = playing ? hostBpm.load() : 0.0;
            }
            const int len = captureLen.load();
            const int room = captureBuf.getNumSamples() - len;
            const int k = jmin (room, n);
            if (k > 0 && inCh > 0)
            {
                for (int c = 0; c < 2; ++c) captureBuf.copyFrom (c, len, buffer, jmin (c, inCh - 1), 0, k);
                captureLen = len + k;
            }
            if (room <= n) { captureArmed = false; captureRunning = false; captureFinished = true; triggerAsyncUpdate(); }
        }
        else if (captureRunning.load())
        {
            captureArmed = false; captureRunning = false; captureFinished = true; triggerAsyncUpdate();
        }
    }

    // ---- song time ----
    const bool preview = previewPlaying.load() && ! playing;
    double songTime = -1.0;
    if (playing) songTime = hostTime - songStart.load();
    else if (preview) songTime = previewPos.load();
    const double blockSecs = n / sampleRateHz;

    const ScopedTryLock tl (playbackLock);
    if (! tl.isLocked()) { if (playStems.load()) buffer.clear(); return; }

    if (playAudio && songTime > -1e8)
        playhead = jmax (0.0, songTime);
    if (preview)
    {
        previewPos = songTime + blockSecs;
        if (playAudio && songTime > playAudio->duration) { previewPlaying = false; previewPos = 0.0; }
    }

    // ---- stems out ----
    if (playStems.load() && playAudio && ! playAudio->stems.empty())
    {
        buffer.clear();
        if (songTime > -blockSecs && (playing || preview))
        {
            bool anySolo = false;
            for (auto& c : stemControls) anySolo = anySolo || c.solo.load();
            const auto& pa = *playAudio;
            const long start = (long) std::llround (songTime * pa.rate);
            for (size_t s = 0; s < pa.stems.size() && s < (size_t) kMaxStems; ++s)
            {
                auto& ctl = stemControls[s];
                if (ctl.mute.load() || (anySolo && ! ctl.solo.load())) continue;
                const float g = ctl.level.load();
                const auto& st = pa.stems[s];
                for (int c = 0; c < buffer.getNumChannels(); ++c)
                {
                    const auto& src = st[(size_t) jmin (c, (int) st.size() - 1)];
                    float* d = buffer.getWritePointer (c);
                    for (int i = 0; i < n; ++i)
                    {
                        const long k = start + i;
                        if (k >= 0 && k < (long) src.size()) d[i] += g * src[(size_t) k];
                    }
                }
            }
        }
    }

    // ---- MIDI out ----
    if (midiOutEnabled.load() && playMidi && (playing || preview) && songTime > -blockSecs)
    {
        const bool contiguous = lastSongTime >= 0 && std::abs (songTime - lastSongTime) < blockSecs * 1.5 + 1e-3;
        if (! contiguous && midiNotesHanging)
        {
            for (int ch = 1; ch <= 16; ++ch) midi.addEvent (MidiMessage::allNotesOff (ch), 0);
            midiNotesHanging = false;
        }
        const double t0 = contiguous ? lastSongTime + blockSecs : songTime, t1 = songTime + blockSecs;
        auto& ev = *playMidi;
        auto it = std::lower_bound (ev.begin(), ev.end(), t0, [] (const MidiEv& e, double t) { return e.t < t; });
        for (; it != ev.end() && it->t < t1; ++it)
        {
            if (! stemControls[(size_t) jlimit (0, kMaxStems - 1, it->stem)].midiOut.load() && it->m.isNoteOn()) continue;
            const int off = jlimit (0, n - 1, (int) ((it->t - songTime) * sampleRateHz));
            midi.addEvent (it->m, off);
            midiNotesHanging = true;
        }
        lastSongTime = songTime;
    }
    else
    {
        if (midiNotesHanging) { for (int ch = 1; ch <= 16; ++ch) midi.addEvent (MidiMessage::allNotesOff (ch), 0); midiNotesHanging = false; }
        lastSongTime = -1.0;
    }
}

// ------------------------------------------------------------------------------------------
void StemScribeProcessor::setStage (const String& s, float p)
{
    const ScopedLock l (stageLock);
    stage = s;
    progress = p;
}

String StemScribeProcessor::getStatusText() const
{
    {
        const ScopedLock l (stageLock);
        if (stage.isNotEmpty()) return stage + "  " + String (roundToInt (progress.load() * 100.0f)) + "%";
    }
    if (captureArmed.load())
        return captureRunning.load() ? "Capturing  " + String (capturedSeconds(), 1) + " s" : "Capture armed: press play in the host";
    if (lastError.isNotEmpty()) return lastError;
    if (analysis)
    {
        String s = String ((int) analysis->stems.size()) + (analysis->stems.size() == 1 ? " track" : " stems") + "  |  "
                 + analysis->keyName() + "  |  " + String (analysis->tempo, 1) + " BPM";
        if (analysis->tempoFromHost) s << " (host)";
        return s;
    }
    return "Drop a song here, or press Capture and play the track";
}

bool StemScribeProcessor::isAnalysing() const
{
    const ScopedLock l (stageLock);
    return stage.isNotEmpty();
}

double StemScribeProcessor::capturedSeconds() const { return captureLen.load() / jmax (1.0, captureRate); }

String StemScribeProcessor::modelPath() const
{
    const auto p = globalProps->getValue ("modelPath");
    if (p.isNotEmpty() && File (p).existsAsFile()) return p;
    return String (ss::findDefaultModel());
}

void StemScribeProcessor::setModelPath (const File& f)
{
    globalProps->setValue ("modelPath", f.getFullPathName());
    globalProps->saveIfNeeded();
    lastError.clear();
    sendChangeMessage();
}

void StemScribeProcessor::runJob (std::function<std::function<void()> (std::atomic<bool>&)> job, const String& firstStage)
{
    lastError.clear();
    setStage (firstStage, 0.0f);
    worker->start (std::move (job));
    sendChangeMessage();
}

void StemScribeProcessor::cancelAnalysis()
{
    worker->requestCancel();
}

void StemScribeProcessor::handleAsyncUpdate()
{
    if (captureFinished.exchange (false)) finishCapture();
    std::vector<std::function<void()>> jobs;
    {
        const ScopedLock l (resultLock);
        jobs.swap (pendingResults);
    }
    for (auto& j : jobs) j();
    if (! jobs.empty()) setStage ({}, 0.0f);   // one job at a time: a result means it finished
    sendChangeMessage();
}

// ------------------------------------------------------------------------------------------
static bool readAudio (const File& f, ss::AudioBuffer& out, String& err)
{
    AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
    if (! r) { err = "Can't read " + f.getFileName() + " (WAV, AIFF, FLAC, Ogg or MP3)"; return false; }
    if (r->lengthInSamples > (int64) (r->sampleRate * 60 * 20)) { err = "Files longer than 20 minutes are not supported"; return false; }
    const int len = (int) r->lengthInSamples, ch = jlimit (1, 2, (int) r->numChannels);
    AudioBuffer<float> b (ch, len);
    r->read (&b, 0, len, 0, true, ch > 1);
    out.sampleRate = r->sampleRate;
    out.setSize (ch, (size_t) len);
    for (int c = 0; c < ch; ++c) std::copy (b.getReadPointer (c), b.getReadPointer (c) + len, out.ch[(size_t) c].begin());
    return true;
}

std::unique_ptr<StemScribeProcessor::PlayAudio> StemScribeProcessor::makePlayAudio (const ss::Analysis& a, double rate) const
{
    auto pa = std::make_unique<PlayAudio>();
    pa->rate = rate;
    for (auto& s : a.stems)
    {
        auto st = std::abs (s.audio.sampleRate - rate) < 0.5 ? s.audio : ss::resample (s.audio, rate);
        pa->duration = jmax (pa->duration, st.seconds());
        pa->stems.push_back (std::move (st.ch));
    }
    return pa;
}

String StemScribeProcessor::loadAudioFile (const File& f)
{
    if (! f.existsAsFile()) return "File not found";
    sourcePath = f.getFullPathName();
    sourceName = f.getFileNameWithoutExtension();
    songStart = 0.0;
    pendingSessionData.clear();
    startAnalysis();
    return {};
}

void StemScribeProcessor::startAnalysis()
{
    if (sourcePath.isEmpty()) { lastError = "Load a file or capture audio first"; sendChangeMessage(); return; }
    const File src (sourcePath);
    const auto opt = settings;
    const double hostRate = sampleRateHz;
    const String model = modelPath();
    double bpm = 0.0, offset = 0.0;
    if (opt.useHostTempo && hostBpm.load() > 0)
    {
        bpm = hostBpm.load();
        // a captured take knows where the beats fell; a loaded file is assumed to start on a beat
        const double ppq = captureStartPpq.load();
        if (ppq >= 0 && sourceName == "Capture") offset = (std::ceil (ppq) - ppq) * 60.0 / bpm;
    }
    auto session = std::make_shared<std::vector<uint8_t>> (pendingSessionData);
    pendingSessionData.clear();

    runJob ([this, src, opt, hostRate, model, bpm, offset, session] (std::atomic<bool>& cancel) -> std::function<void()> {
        ss::AudioBuffer in;
        String err;
        setStage ("Reading audio", 0.0f);
        if (! readAudio (src, in, err)) return [this, err] { lastError = err; };
        const bool wantSplit = opt.split && ss::demucsAvailable() && File (model).existsAsFile();
        auto a = std::make_unique<ss::Analysis>();
        const auto key = ss::analysisKey (in, wantSplit);
        bool loaded = false;
        if (cache->has (key)) { setStage ("Loading cached analysis", 0.1f); loaded = cache->load (key, *a); }
        if (! loaded)
        {
            if (wantSplit && (! separator || separatorPath != model))
            {
                setStage ("Loading the separation model", 0.0f);
                std::string e;
                separator = ss::createDemucsSeparator (model.toStdString(), 0, e);
                separatorPath = separator ? model : String();
                if (! separator) return [this, e] { lastError = String (e); };
            }
            ss::PipelineOptions po;
            po.split = wantSplit;
            po.mixMode = opt.mixMode;
            po.chords = opt.chords;
            po.hostBpm = bpm;
            po.hostBeatOffset = offset;
            std::string e;
            const bool ok = ss::runPipeline (in, po, wantSplit ? separator.get() : nullptr, basicPitch, *a,
                                             [this] (float p, const std::string& s) { setStage (String (s), p); }, &cancel, e);
            if (! ok) return [this, e] { lastError = e == "cancelled" ? String ("Analysis cancelled") : String (e); };
            setStage ("Saving to the cache", 1.0f);
            cache->save (*a);
        }
        if (! session->empty()) a->deserialiseData (*session);   // edits saved with the DAW session win
        setStage ("Preparing playback", 1.0f);
        auto pa = makePlayAudio (*a, hostRate);
        const bool noModel = opt.split && ! wantSplit;
        auto ap = std::shared_ptr<ss::Analysis> (a.release());
        auto pp = std::shared_ptr<PlayAudio> (pa.release());
        return [this, ap, pp, noModel] {
            installAnalysis (std::make_unique<ss::Analysis> (std::move (*ap)), std::make_unique<PlayAudio> (std::move (*pp)));
            if (noModel) lastError = "Separation model not found: transcribed the whole mix (set the model file in the menu)";
        };
    }, "Starting");
}

void StemScribeProcessor::installAnalysis (std::unique_ptr<ss::Analysis> a, std::unique_ptr<PlayAudio> audio)
{
    std::unique_ptr<PlayAudio> oldAudio;
    std::unique_ptr<ss::Analysis> oldA;
    {
        const ScopedLock l (playbackLock);
        oldAudio = std::move (playAudio);
        playAudio = std::move (audio);
    }
    oldA = std::move (analysis);
    analysis = std::move (a);
    undoStack.clear(); redoStack.clear();
    selectedStem = jlimit (0, jmax (0, (int) analysis->stems.size() - 1), selectedStem);
    rebuildMidi();
    setStage ({}, 0.0f);
    resampleIfNeeded();   // the host rate changed while the analysis was running
    sendChangeMessage();
}

void StemScribeProcessor::setAnalysisForTesting (std::unique_ptr<ss::Analysis> a, const String& name)
{
    sourceName = name;
    auto pa = makePlayAudio (*a, sampleRateHz);
    installAnalysis (std::move (a), std::move (pa));
}

void StemScribeProcessor::rebuildMidi()
{
    auto ev = std::make_unique<std::vector<MidiEv>>();
    if (analysis)
        for (size_t s = 0; s < analysis->stems.size(); ++s)
        {
            const auto& st = analysis->stems[s];
            const auto spec = ss::trackForStem (st);
            const int ch = spec.channel + 1;
            for (auto& n : st.notes)
            {
                ev->push_back ({ n.start, (int) s, MidiMessage::noteOn (ch, jlimit (0, 127, n.pitch), (uint8) jlimit (1, 127, roundToInt (n.velocity * 127.0f))) });
                ev->push_back ({ n.end, (int) s, MidiMessage::noteOff (ch, jlimit (0, 127, n.pitch)) });
            }
        }
    std::stable_sort (ev->begin(), ev->end(), [] (const MidiEv& a, const MidiEv& b) { return a.t < b.t; });
    std::unique_ptr<std::vector<MidiEv>> old;
    const ScopedLock l (playbackLock);
    old = std::move (playMidi);
    playMidi = std::move (ev);
}

void StemScribeProcessor::pushUndo()
{
    if (! analysis) return;
    undoStack.push_back (analysis->serialiseData());
    if (undoStack.size() > 60) undoStack.erase (undoStack.begin());
    redoStack.clear();
}

void StemScribeProcessor::undo()
{
    if (! analysis || undoStack.empty()) return;
    redoStack.push_back (analysis->serialiseData());
    analysis->deserialiseData (undoStack.back());
    undoStack.pop_back();
    analysisEdited();
}

void StemScribeProcessor::redo()
{
    if (! analysis || redoStack.empty()) return;
    undoStack.push_back (analysis->serialiseData());
    analysis->deserialiseData (redoStack.back());
    redoStack.pop_back();
    analysisEdited();
}

void StemScribeProcessor::analysisEdited (bool recomputeChords)
{
    if (! analysis) return;
    for (auto& s : analysis->stems) std::sort (s.notes.begin(), s.notes.end());
    if (recomputeChords) ss::updateHarmony (*analysis);
    rebuildMidi();
    cache->saveData (*analysis);
    sendChangeMessage();
}

void StemScribeProcessor::retranscribe (int stem, ss::Transcribe mode)
{
    if (! analysis || stem < 0 || stem >= (int) analysis->stems.size()) return;
    pushUndo();
    auto copy = std::make_shared<ss::StemResult> (analysis->stems[(size_t) stem]);
    copy->mode = mode;
    const auto key = analysis->key;
    const bool chords = settings.chords;
    runJob ([this, copy, stem, key, chords] (std::atomic<bool>& cancel) -> std::function<void()> {
        ss::transcribeStem (*copy, basicPitch, 0, &cancel, [this] (float p) { setStage ("Transcribing", p); });
        if (cancel.load()) return [this] { lastError = "Cancelled"; };
        return [this, copy, stem, key, chords] {
            if (! analysis || analysis->key != key || stem >= (int) analysis->stems.size()) return;
            auto& s = analysis->stems[(size_t) stem];
            s.mode = copy->mode; s.notes = std::move (copy->notes); s.curve = std::move (copy->curve);
            analysisEdited (chords);
        };
    }, "Transcribing");
}

// ------------------------------------------------------------------------------------------
void StemScribeProcessor::setCapture (bool armed)
{
    if (armed)
    {
        if (isAnalysing()) return;
        captureRate = sampleRateHz;
        captureLen = 0;
        captureBuf.setSize (2, (int) (sampleRateHz * 60 * 10), false, true, false);   // up to 10 minutes
        captureRunning = false;
        captureArmed = true;
    }
    else
    {
        const bool wasRunning = captureRunning.load();
        captureArmed = false;
        captureRunning = false;
        if (wasRunning || captureLen.load() > 0) finishCapture();
    }
    sendChangeMessage();
}

void StemScribeProcessor::finishCapture()
{
    const int len = captureLen.exchange (0);
    if (len < (int) (captureRate * 0.5)) { if (len > 0) lastError = "Capture too short"; captureBuf.setSize (0, 0); return; }
    ss::AudioBuffer a;
    a.sampleRate = captureRate;
    a.setSize (2, (size_t) len);
    for (int c = 0; c < 2; ++c) std::copy (captureBuf.getReadPointer (c), captureBuf.getReadPointer (c) + len, a.ch[(size_t) c].begin());
    captureBuf.setSize (0, 0);
    // keep the take in the cache folder so the session can find it again
    const File dir = File (String (cache->root())).getChildFile ("captures");
    dir.createDirectory();
    const File f = dir.getChildFile ("capture-" + String (ss::hexKey (a.hash())) + ".wav");
    ss::writeWav (f.getFullPathName().toStdString(), a, 32);
    sourcePath = f.getFullPathName();
    sourceName = "Capture";
    songStart = captureStartHost.load();
    pendingSessionData.clear();
    startAnalysis();
}

// ------------------------------------------------------------------------------------------
void StemScribeProcessor::setPreviewPlaying (bool p) { previewPlaying = p; sendChangeMessage(); }
void StemScribeProcessor::setPreviewPosition (double s) { previewPos = jmax (0.0, s); playhead = jmax (0.0, s); }
void StemScribeProcessor::placeAtPlayhead() { songStart = hostPos.load(); sendChangeMessage(); }

File StemScribeProcessor::exportFolder() const
{
    auto name = File::createLegalFileName (sourceName.isEmpty() ? String ("Untitled") : sourceName);
    if (sourceName == "Capture" && analysis) name << "-" << String (ss::hexKey (analysis->key)).substring (0, 6);
    auto d = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("stem-scribe").getChildFile (name);
    d.createDirectory();
    return d;
}

static File writeBytes (const File& f, const std::vector<uint8_t>& b)
{
    f.replaceWithData (b.data(), b.size());
    return f;
}

File StemScribeProcessor::exportStemWav (int i)
{
    if (! analysis || i < 0 || i >= (int) analysis->stems.size()) return {};
    const auto& s = analysis->stems[(size_t) i];
    const auto f = exportFolder().getChildFile (sourceName + " - " + String (s.name) + ".wav");
    ss::writeWav (f.getFullPathName().toStdString(), s.audio, 24);
    return f;
}

File StemScribeProcessor::exportStemMidi (int i)
{
    if (! analysis || i < 0 || i >= (int) analysis->stems.size()) return {};
    return writeBytes (exportFolder().getChildFile (sourceName + " - " + String (analysis->stems[(size_t) i].name) + ".mid"), ss::exportStemMidi (*analysis, i));
}

File StemScribeProcessor::exportChordsMidi()
{
    if (! analysis) return {};
    return writeBytes (exportFolder().getChildFile (sourceName + " - Chords.mid"), ss::exportChordMidi (*analysis));
}

File StemScribeProcessor::exportAllMidi()
{
    if (! analysis) return {};
    return writeBytes (exportFolder().getChildFile (sourceName + " - All.mid"), ss::exportAllMidi (*analysis));
}

Array<File> StemScribeProcessor::exportEverything()
{
    Array<File> files;
    if (! analysis) return files;
    for (int i = 0; i < (int) analysis->stems.size(); ++i)
    {
        if (analysis->stems.size() > 1) files.add (exportStemWav (i));
        if (! analysis->stems[(size_t) i].notes.empty()) files.add (exportStemMidi (i));
    }
    if (! analysis->chords.empty()) files.add (exportChordsMidi());
    files.add (exportAllMidi());
    auto txt = exportFolder().getChildFile (sourceName + " - Chords.txt");
    String s;
    s << "Key: " << analysis->keyName() << "\nTempo: " << String (analysis->tempo, 2) << " BPM\n\n";
    for (auto& c : analysis->chords) s << String (c.start, 2) << "\t" << String (c.end, 2) << "\t" << String (c.name()) << "\n";
    txt.replaceWithText (s);
    files.add (txt);
    return files;
}

// ------------------------------------------------------------------------------------------
void StemScribeProcessor::getStateInformation (MemoryBlock& dest)
{
    ValueTree t ("StemScribe");
    t.setProperty ("theme", themeName, nullptr);
    t.setProperty ("w", editorW, nullptr);
    t.setProperty ("h", editorH, nullptr);
    t.setProperty ("stem", selectedStem, nullptr);
    t.setProperty ("source", sourcePath, nullptr);
    t.setProperty ("sourceName", sourceName, nullptr);
    t.setProperty ("songStart", songStart.load(), nullptr);
    t.setProperty ("playStems", playStems.load(), nullptr);
    t.setProperty ("midiOut", midiOutEnabled.load(), nullptr);
    t.setProperty ("split", settings.split, nullptr);
    t.setProperty ("mixMode", (int) settings.mixMode, nullptr);
    t.setProperty ("chords", settings.chords, nullptr);
    t.setProperty ("hostTempo", settings.useHostTempo, nullptr);
    t.setProperty ("capturePpq", captureStartPpq.load(), nullptr);
    for (int i = 0; i < kMaxStems; ++i)
    {
        ValueTree s ("Stem");
        s.setProperty ("level", stemControls[(size_t) i].level.load(), nullptr);
        s.setProperty ("mute", stemControls[(size_t) i].mute.load(), nullptr);
        s.setProperty ("solo", stemControls[(size_t) i].solo.load(), nullptr);
        s.setProperty ("midi", stemControls[(size_t) i].midiOut.load(), nullptr);
        t.appendChild (s, nullptr);
    }
    if (analysis)
    {
        const auto d = analysis->serialiseData();
        t.setProperty ("analysis", Base64::toBase64 (d.data(), d.size()), nullptr);
    }
    else if (! pendingSessionData.empty())
        t.setProperty ("analysis", Base64::toBase64 (pendingSessionData.data(), pendingSessionData.size()), nullptr);
    if (auto xml = t.createXml()) copyXmlToBinary (*xml, dest);
}

void StemScribeProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (! xml) return;
    auto t = ValueTree::fromXml (*xml);
    if (! t.hasType ("StemScribe")) return;
    themeName = t.getProperty ("theme", themeName).toString();
    editorW = t.getProperty ("w", editorW);
    editorH = t.getProperty ("h", editorH);
    selectedStem = t.getProperty ("stem", 0);
    sourcePath = t.getProperty ("source").toString();
    sourceName = t.getProperty ("sourceName").toString();
    songStart = (double) t.getProperty ("songStart", 0.0);
    playStems = (bool) t.getProperty ("playStems", true);
    midiOutEnabled = (bool) t.getProperty ("midiOut", false);
    settings.split = t.getProperty ("split", true);
    settings.mixMode = (ss::Transcribe) (int) t.getProperty ("mixMode", (int) ss::Transcribe::Poly);
    settings.chords = t.getProperty ("chords", true);
    settings.useHostTempo = t.getProperty ("hostTempo", true);
    captureStartPpq = (double) t.getProperty ("capturePpq", -1.0);
    for (int i = 0; i < kMaxStems && i < t.getNumChildren(); ++i)
    {
        auto s = t.getChild (i);
        stemControls[(size_t) i].level = (float) s.getProperty ("level", 1.0f);
        stemControls[(size_t) i].mute = (bool) s.getProperty ("mute", false);
        stemControls[(size_t) i].solo = (bool) s.getProperty ("solo", false);
        stemControls[(size_t) i].midiOut = (bool) s.getProperty ("midi", true);
    }
    pendingSessionData.clear();
    MemoryOutputStream mo;
    if (Base64::convertFromBase64 (mo, t.getProperty ("analysis").toString()))
        pendingSessionData.assign ((const uint8_t*) mo.getData(), (const uint8_t*) mo.getData() + mo.getDataSize());
    // reload the stems (from the cache when possible) and put the saved edits back on top
    if (sourcePath.isNotEmpty() && File (sourcePath).existsAsFile()) startAnalysis();
    else if (sourcePath.isNotEmpty()) lastError = "Source audio missing: " + sourcePath;
    sendChangeMessage();
}

AudioProcessorEditor* StemScribeProcessor::createEditor() { return new StemScribeEditor (*this); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new StemScribeProcessor(); }
