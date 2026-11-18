// The ReaTune-style view: waveform strip, chord lane, and a piano roll where the detected pitch
// curve is drawn over the note blocks (drum stems get a drum-lane grid instead). Notes can be
// selected, moved, resized, drawn, split, merged, deleted, transposed and quantised; chords can be
// renamed in place.
#pragma once

#include "PluginProcessor.h"
#include "Themes.h"

#include <set>

class RollView : public juce::Component, private juce::Timer, private juce::ScrollBar::Listener
{
public:
    explicit RollView (StemScribeProcessor& p);
    ~RollView() override;

    void analysisChanged();        // new analysis or stem selection: refit, recompute the spectrogram
    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

    // editing commands (also used by the toolbar)
    void deleteSelection();
    void splitSelectionAt (double t);
    void mergeSelection();
    void transposeSelection (int semis);
    void quantizeSelectionOrAll (float strength);
    void selectAll();
    void zoomToFit();

    int gridDivision = 4;          // steps per beat (4 = 1/16 notes); 0 = off
    int scaleSnap = 0;             // index into scaleNames(); 0 = off
    bool showGhosts = true;        // other stems' notes, faint
    static const juce::StringArray& scaleNames();

private:
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double) override;

    ss::StemResult* stem();
    bool isDrumView();
    std::vector<int> drumLanes();

    // geometry
    juce::Rectangle<int> rulerArea() const, chordArea() const, waveArea() const, rollArea() const, keysArea() const;
    float timeToX (double t) const;
    double xToTime (float x) const;
    float pitchToY (float pitch) const;    // top of the row
    float yToPitch (float y) const;
    float rowH() const;
    double snapTime (double t, bool force = false) const;
    int snapPitch (int p) const;
    juce::Rectangle<float> noteRect (const ss::Note& n);
    int hitNote (juce::Point<float> p, bool& onEdge);
    int hitChord (juce::Point<float> p) const;
    void commit (bool chords = false);
    void editChord (int index);
    void showChordMenu (int index, double t);
    void showRollMenu (juce::Point<int> pos);
    void computeSpectrogram();
    void computeWaveform();

    StemScribeProcessor& proc;
    juce::ScrollBar hbar { false }, vbar { true };
    double viewStart = 0.0, pxPerSec = 80.0;
    float lowPitch = 28.0f;        // MIDI note at the bottom of the roll
    float semitonesVisible = 48.0f;
    std::set<int> selection;

    enum class Drag { None, Move, Resize, Rubber, Scrub, Velocity } drag = Drag::None;
    juce::Point<float> dragStart;
    std::vector<ss::Note> dragOrig;
    int dragAnchor = -1;
    juce::Rectangle<float> rubber;
    double hoverTime = 0;

    juce::Image spectrogram;
    int spectroStem = -1;
    uint64_t spectroKey = 0;
    std::vector<float> waveMin, waveMax;   // per 256-sample block
    int waveStem = -1;
    uint64_t waveKey = 0;
    std::unique_ptr<juce::TextEditor> chordEditor;
    int editingChord = -1;
    juce::Atomic<int> spectroJob { 0 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (RollView)
};
