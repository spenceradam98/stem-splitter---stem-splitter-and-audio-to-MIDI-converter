#include "PluginEditor.h"

using namespace juce;

StemScribeEditor::StemScribeEditor (StemScribeProcessor& p)
    : AudioProcessorEditor (p), proc (p), stems (p), roll (p)
{
    setLookAndFeel (&lnf);
    lnf.setTheme (themeByName (proc.themeName));

    addAndMakeVisible (stems);
    addAndMakeVisible (roll);
    stems.onSelect = [this] (int) { roll.analysisChanged(); };

    for (auto* b : { &loadBtn, &captureBtn, &analyseBtn, &cancelBtn, &menuBtn, &playBtn, &outBtn, &liveMidiBtn, &placeBtn,
                     &quantBtn, &undoBtn, &redoBtn, &exportBtn })
        addAndMakeVisible (*b);
    addAndMakeVisible (splitToggle);
    addAndMakeVisible (mixMode);
    addAndMakeVisible (progressBar);
    addAndMakeVisible (themeBox);
    addAndMakeVisible (gridBox);
    addAndMakeVisible (scaleBox);
    addAndMakeVisible (posLabel);
    addAndMakeVisible (infoLabel);
    progressBar.setPercentageDisplay (false);

    loadBtn.setTooltip ("Load a song (WAV, AIFF, FLAC, Ogg, MP3). You can also drop a file on the window.");
    loadBtn.onClick = [this] { chooseFile(); };
    captureBtn.setClickingTogglesState (true);
    captureBtn.setTooltip ("Record the track this plugin sits on: arm, play the section in the host, then stop. stem-scribe analyses what it heard and lines the stems up with the timeline.");
    captureBtn.onClick = [this] { proc.setCapture (captureBtn.getToggleState()); };
    analyseBtn.setTooltip ("Run the analysis again with the current settings");
    analyseBtn.onClick = [this] { proc.startAnalysis(); };
    cancelBtn.onClick = [this] { proc.cancelAnalysis(); };
    menuBtn.onClick = [this] { showMainMenu(); };

    splitToggle.setToggleState (proc.settings.split, dontSendNotification);
    splitToggle.setTooltip ("Split into vocals, drums, bass and other first (HTDemucs). Off = plain audio-to-MIDI on the whole file.");
    splitToggle.onClick = [this] { proc.settings.split = splitToggle.getToggleState(); refreshFromProcessor(); };
    mixMode.addItem ("Poly (chords, keys)", 1);
    mixMode.addItem ("Mono (melody, bass)", 2);
    mixMode.addItem ("Drums", 3);
    mixMode.setSelectedId (proc.settings.mixMode == ss::Transcribe::Mono ? 2 : proc.settings.mixMode == ss::Transcribe::Drums ? 3 : 1, dontSendNotification);
    mixMode.setTooltip ("How to transcribe the whole file when not splitting");
    mixMode.onChange = [this] {
        const int id = mixMode.getSelectedId();
        proc.settings.mixMode = id == 2 ? ss::Transcribe::Mono : id == 3 ? ss::Transcribe::Drums : ss::Transcribe::Poly;
    };

    for (auto& t : allThemes()) themeBox.addItem (t.name, themeBox.getNumItems() + 1);
    themeBox.setText (proc.themeName, dontSendNotification);
    themeBox.onChange = [this] { applyTheme (themeBox.getText()); };

    gridBox.addItem ("Grid: off", 1);
    gridBox.addItem ("Grid: 1/4", 2);
    gridBox.addItem ("Grid: 1/8", 3);
    gridBox.addItem ("Grid: 1/16", 4);
    gridBox.addItem ("Grid: 1/32", 5);
    gridBox.addItem ("Grid: 1/8 triplet", 6);
    gridBox.setSelectedId (4, dontSendNotification);
    gridBox.onChange = [this] {
        static const int div[] = { 0, 1, 2, 4, 8, 3 };
        roll.gridDivision = div[jlimit (0, 5, gridBox.getSelectedId() - 1)];
        roll.repaint();
    };
    for (int i = 0; i < RollView::scaleNames().size(); ++i) scaleBox.addItem (RollView::scaleNames()[i], i + 1);
    scaleBox.setSelectedId (1, dontSendNotification);
    scaleBox.setTooltip ("Snap moved and drawn notes to a scale on the detected key");
    scaleBox.onChange = [this] { roll.scaleSnap = scaleBox.getSelectedId() - 1; };

    playBtn.setTooltip ("Preview the stems (space). In a DAW, the stems also play with the host transport.");
    playBtn.onClick = [this] { proc.setPreviewPlaying (! proc.isPreviewPlaying()); };
    outBtn.setClickingTogglesState (true);
    outBtn.setToggleState (proc.playStems.load(), dontSendNotification);
    outBtn.setTooltip ("On: the plugin outputs the stems (with solo / mute / level). Off: the input passes through.");
    outBtn.onClick = [this] { proc.playStems = outBtn.getToggleState(); };
    liveMidiBtn.setClickingTogglesState (true);
    liveMidiBtn.setToggleState (proc.midiOutEnabled.load(), dontSendNotification);
    liveMidiBtn.setTooltip ("Send the MIDI out of the plugin while the host plays (REAPER, Cubase, Bitwig, FL Studio). Choose stems with each stem's Live button.");
    liveMidiBtn.onClick = [this] { proc.midiOutEnabled = liveMidiBtn.getToggleState(); };
    placeBtn.setTooltip ("Line the song's start up with the host's current playhead position");
    placeBtn.onClick = [this] { proc.placeAtPlayhead(); };
    quantBtn.setTooltip ("Quantize the selected notes (or all notes of the stem) to the grid. Shift+Q in the roll = 50%.");
    quantBtn.onClick = [this] { roll.quantizeSelectionOrAll (1.0f); };
    undoBtn.onClick = [this] { proc.undo(); roll.repaint(); };
    redoBtn.onClick = [this] { proc.redo(); roll.repaint(); };
    exportBtn.setTooltip ("Write every stem's WAV and MIDI, the chord tracks and a chord sheet to Documents/stem-scribe");
    exportBtn.onClick = [this] {
        auto files = proc.exportEverything();
        if (! files.isEmpty()) files[0].revealToUser();
    };
    infoLabel.setJustificationType (Justification::centredRight);
    posLabel.setJustificationType (Justification::centredLeft);

    proc.addChangeListener (this);
    constrainer.setMinimumSize (860, 560);
    constrainer.setMaximumSize (3000, 2000);
    setConstrainer (&constrainer);
    setResizable (true, true);
    setSize (jlimit (860, 3000, proc.editorW), jlimit (560, 2000, proc.editorH));
    refreshFromProcessor();
    roll.analysisChanged();
    startTimerHz (15);
}

StemScribeEditor::~StemScribeEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void StemScribeEditor::applyTheme (const String& name)
{
    proc.themeName = name;
    lnf.setTheme (themeByName (name));
    sendLookAndFeelChange();
    roll.analysisChanged();
    repaint();
}

void StemScribeEditor::paint (Graphics& g)
{
    const auto& t = lnf.theme();
    g.fillAll (t.bg);
    auto top = getLocalBounds().removeFromTop (48);
    g.setColour (t.panel);
    g.fillRect (top);
    lnf.paintGrain (g, top);
    g.setColour (t.edge);
    g.drawHorizontalLine (top.getBottom() - 1, 0.0f, (float) getWidth());
    // wordmark
    g.setColour (t.accent);
    g.setFont (Font (FontOptions (21.0f, t.noteStyle == 1 ? Font::italic : Font::bold)));
    if (t.noteStyle == 2) { g.setColour (t.accent.withAlpha (0.35f)); g.drawText ("stem-scribe", 15, 1, 150, 46, Justification::centredLeft); g.setColour (t.accent); }
    g.drawText ("stem-scribe", 14, 0, 150, 46, Justification::centredLeft);
    auto bottom = getLocalBounds().removeFromBottom (44);
    g.setColour (t.panel);
    g.fillRect (bottom);
    g.setColour (t.edge);
    g.drawHorizontalLine (bottom.getY(), 0.0f, (float) getWidth());
    if (dropHover)
    {
        g.setColour (t.accent.withAlpha (0.15f));
        g.fillRect (getLocalBounds());
        g.setColour (t.accent);
        g.drawRect (getLocalBounds(), 3);
        g.setFont (Font (FontOptions (22.0f, Font::bold)));
        g.drawText ("Drop to analyse", getLocalBounds(), Justification::centred);
    }
}

void StemScribeEditor::resized()
{
    proc.editorW = getWidth();
    proc.editorH = getHeight();
    auto r = getLocalBounds();
    auto top = r.removeFromTop (48).reduced (8, 9);
    top.removeFromLeft (150);
    loadBtn.setBounds (top.removeFromLeft (64)); top.removeFromLeft (6);
    captureBtn.setBounds (top.removeFromLeft (80)); top.removeFromLeft (6);
    analyseBtn.setBounds (top.removeFromLeft (76)); top.removeFromLeft (10);
    splitToggle.setBounds (top.removeFromLeft (104));
    mixMode.setBounds (top.removeFromLeft (150)); top.removeFromLeft (10);
    menuBtn.setBounds (top.removeFromRight (64)); top.removeFromRight (6);
    themeBox.setBounds (top.removeFromRight (130)); top.removeFromRight (10);
    cancelBtn.setBounds (top.removeFromRight (70)); top.removeFromRight (6);
    progressBar.setBounds (top);

    auto bottom = r.removeFromBottom (44).reduced (8, 8);
    playBtn.setBounds (bottom.removeFromLeft (64)); bottom.removeFromLeft (6);
    posLabel.setBounds (bottom.removeFromLeft (92));
    outBtn.setBounds (bottom.removeFromLeft (88)); bottom.removeFromLeft (6);
    liveMidiBtn.setBounds (bottom.removeFromLeft (84)); bottom.removeFromLeft (6);
    placeBtn.setBounds (bottom.removeFromLeft (170)); bottom.removeFromLeft (12);
    exportBtn.setBounds (bottom.removeFromRight (90)); bottom.removeFromRight (6);
    redoBtn.setBounds (bottom.removeFromRight (54)); bottom.removeFromRight (4);
    undoBtn.setBounds (bottom.removeFromRight (54)); bottom.removeFromRight (10);
    quantBtn.setBounds (bottom.removeFromRight (78)); bottom.removeFromRight (6);
    scaleBox.setBounds (bottom.removeFromRight (150)); bottom.removeFromRight (6);
    gridBox.setBounds (bottom.removeFromRight (118)); bottom.removeFromRight (6);
    infoLabel.setBounds (bottom);

    stems.setBounds (r.removeFromLeft (290));
    roll.setBounds (r);
}

bool StemScribeEditor::isInterestedInFileDrag (const StringArray& files)
{
    for (auto& f : files)
        if (File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3")) return true;
    return false;
}

void StemScribeEditor::filesDropped (const StringArray& files, int, int)
{
    dropHover = false;
    for (auto& f : files)
        if (File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3")) { proc.loadAudioFile (File (f)); break; }
    refreshFromProcessor();
    repaint();
}

void StemScribeEditor::chooseFile()
{
    chooser = std::make_unique<FileChooser> ("Load a song", File::getSpecialLocation (File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc) {
        if (fc.getResult().existsAsFile()) proc.loadAudioFile (fc.getResult());
        refreshFromProcessor();
    });
}

void StemScribeEditor::chooseModel()
{
    chooser = std::make_unique<FileChooser> ("Find the separation model (" + String (ss::defaultModelFileName()) + ")", File(), "*.bin");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc) {
        if (fc.getResult().existsAsFile()) proc.setModelPath (fc.getResult());
        refreshFromProcessor();
    });
}

void StemScribeEditor::showMainMenu()
{
    PopupMenu m;
    m.addSectionHeader ("Analysis");
    m.addItem (1, "Detect chords", true, proc.settings.chords);
    m.addItem (2, "Use the host's tempo", true, proc.settings.useHostTempo);
    m.addItem (3, "Re-detect key and chords from the current notes", proc.getAnalysis() != nullptr);
    m.addSeparator();
    m.addSectionHeader ("Separation model");
    m.addItem (10, proc.modelFound() ? "Model: " + File (proc.modelPath()).getFileName() : String ("Model not found: locate it..."), false);
    m.addItem (11, "Choose model file...");
    m.addSeparator();
    m.addSectionHeader ("Files");
    m.addItem (20, "Show export folder", proc.getAnalysis() != nullptr);
    m.addItem (21, "Show analysis cache folder");
    m.addSeparator();
    m.addItem (30, "Keyboard shortcuts...");
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&menuBtn), [this] (int r) {
        switch (r)
        {
            case 1: proc.settings.chords = ! proc.settings.chords; break;
            case 2: proc.settings.useHostTempo = ! proc.settings.useHostTempo; break;
            case 3: if (auto* a = proc.getAnalysis()) { proc.pushUndo(); ss::updateHarmony (*a); proc.analysisEdited (false); } break;
            case 11: chooseModel(); break;
            case 20: proc.exportFolder().revealToUser(); break;
            case 21: File (String (ss::AnalysisCache::defaultRoot())).revealToUser(); break;
            case 30:
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "stem-scribe shortcuts",
                    "Click a note to select (Shift adds), drag to move, drag the right edge to resize, Alt-drag for velocity.\n"
                    "Double-click to draw a note, double-click a chord to rename it, right-click for more.\n"
                    "Delete: remove   S: split at mouse   M: merge   Q: quantize (Shift: 50%)\n"
                    "Up / Down: transpose (Shift: octave)   Cmd/Ctrl+Z: undo   Cmd/Ctrl+A: select all\n"
                    "Space: play / stop preview   F: zoom to fit   Ctrl+wheel: zoom   Shift+wheel: scroll pitch");
                break;
            default: break;
        }
        refreshFromProcessor();
    });
}

void StemScribeEditor::changeListenerCallback (ChangeBroadcaster*) { refreshFromProcessor(); }

void StemScribeEditor::refreshFromProcessor()
{
    const bool busy = proc.isAnalysing();
    analyseBtn.setEnabled (! busy && proc.getSourceName().isNotEmpty());
    cancelBtn.setEnabled (busy);
    loadBtn.setEnabled (! busy);
    captureBtn.setEnabled (! busy || proc.isCaptureArmed());
    captureBtn.setToggleState (proc.isCaptureArmed(), dontSendNotification);
    mixMode.setVisible (! proc.settings.split);
    splitToggle.setToggleState (proc.settings.split, dontSendNotification);
    auto* a = proc.getAnalysis();
    for (auto* b : { &quantBtn, &exportBtn, &playBtn, &placeBtn }) b->setEnabled (a != nullptr);
    undoBtn.setEnabled (proc.canUndo());
    redoBtn.setEnabled (proc.canRedo());
    const uint64_t key = a ? a->key : 0;
    const size_t count = a ? a->stems.size() : 0;
    stems.refresh();
    if (key != lastKey || count != lastStemCount || proc.selectedStem != lastStem)
    {
        lastKey = key; lastStemCount = count; lastStem = proc.selectedStem;
        roll.analysisChanged();
    }
    if (a)
        infoLabel.setText (String (a->keyName()) + "   " + String (a->tempo, 1) + " BPM" + (a->separatorName.empty() ? String() : "   " + String (a->separatorName)), dontSendNotification);
    else infoLabel.setText (proc.modelFound() ? String() : String ("Separation model not found (Menu)"), dontSendNotification);
    roll.repaint();
}

void StemScribeEditor::timerCallback()
{
    const bool busy = proc.isAnalysing();
    progressValue = busy ? (double) proc.getProgress() : (proc.isCaptureArmed() ? -1.0 : 0.0);
    progressBar.setTextToDisplay (proc.getStatusText());
    if (cancelBtn.isEnabled() != busy) refreshFromProcessor();
    if (captureBtn.getToggleState() != proc.isCaptureArmed()) refreshFromProcessor();
    playBtn.setButtonText (proc.isPreviewPlaying() ? "Stop" : "Play");
    const double ph = proc.getPlayheadSeconds();
    const int m = (int) (ph / 60.0);
    posLabel.setText (String (m) + ":" + String (ph - m * 60.0, 1).paddedLeft ('0', 4), dontSendNotification);
}
