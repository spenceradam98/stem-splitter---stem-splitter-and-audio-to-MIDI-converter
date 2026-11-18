#pragma once

#include "PluginProcessor.h"
#include "RollView.h"
#include "StemList.h"
#include "Themes.h"

class StemScribeEditor : public juce::AudioProcessorEditor,
                         public juce::FileDragAndDropTarget,
                         public juce::DragAndDropContainer,
                         private juce::ChangeListener,
                         private juce::Timer
{
public:
    explicit StemScribeEditor (StemScribeProcessor&);
    ~StemScribeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dropHover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dropHover = false; repaint(); }
    void filesDropped (const juce::StringArray&, int, int) override;

    void applyTheme (const juce::String& name);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refreshFromProcessor();
    void showMainMenu();
    void chooseFile();
    void chooseModel();

    StemScribeProcessor& proc;
    ScribeLookAndFeel lnf;
    StemList stems;
    RollView roll;
    juce::TooltipWindow tooltips { this, 600 };

    juce::TextButton loadBtn { "Load" }, captureBtn { "Capture" }, analyseBtn { "Analyse" }, cancelBtn { "Cancel" }, menuBtn { "Menu" };
    juce::ToggleButton splitToggle { "Split stems" };
    juce::ComboBox mixMode, themeBox, gridBox, scaleBox;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::TextButton playBtn { "Play" }, outBtn { "Stems out" }, liveMidiBtn { "Live MIDI" }, placeBtn { "Place at host playhead" },
                     quantBtn { "Quantize" }, undoBtn { "Undo" }, redoBtn { "Redo" }, exportBtn { "Export all" };
    juce::Label posLabel, infoLabel;
    std::unique_ptr<juce::FileChooser> chooser;
    bool dropHover = false;
    int lastStem = -1;
    uint64_t lastKey = 0;
    size_t lastStemCount = 0;
    std::unique_ptr<juce::ResizableCornerComponent> corner;
    juce::ComponentBoundsConstrainer constrainer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemScribeEditor)
};
