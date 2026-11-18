// The stem list on the left: colour, name, solo / mute / level, transcription mode, and drag
// handles that drop the stem's WAV or MIDI straight into the DAW (or the desktop).
#pragma once

#include "PluginProcessor.h"
#include "Themes.h"

class DragChip : public juce::Component, public juce::SettableTooltipClient
{
public:
    DragChip (juce::String label, std::function<juce::Array<juce::File>()> makeFiles);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    std::function<void (const juce::Array<juce::File>&)> onClickFiles;   // a click (no drag) reveals the files

private:
    juce::String label;
    std::function<juce::Array<juce::File>()> makeFiles;
    bool dragging = false;
};

class StemList : public juce::Component
{
public:
    explicit StemList (StemScribeProcessor& p);
    void refresh();                       // rebuild rows after a new analysis
    void paint (juce::Graphics&) override;
    void resized() override;
    std::function<void (int)> onSelect;
    static constexpr int rowHeight = 92;

private:
    struct Row : public juce::Component
    {
        Row (StemList& owner, int index);
        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        StemList& owner;
        int index;
        juce::TextButton solo { "S" }, mute { "M" }, midiOut { "Live" };
        juce::Slider level { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
        juce::ComboBox mode;
        DragChip wav, mid;
    };
    StemScribeProcessor& proc;
    juce::OwnedArray<Row> rows;
    DragChip allMidi;
    int shownKey = -1;
    friend struct Row;
};
