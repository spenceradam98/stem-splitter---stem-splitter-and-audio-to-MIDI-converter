#include "StemList.h"

using namespace juce;

static const Theme& themeOf (Component& c)
{
    if (auto* lf = dynamic_cast<ScribeLookAndFeel*> (&c.getLookAndFeel())) return lf->theme();
    return allThemes().front();
}

DragChip::DragChip (String l, std::function<Array<File>()> f) : label (std::move (l)), makeFiles (std::move (f))
{
    setMouseCursor (MouseCursor::DraggingHandCursor);
}

void DragChip::paint (Graphics& g)
{
    const auto& t = themeOf (*this);
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool hi = isMouseOver() || dragging;
    g.setColour (hi ? t.accent2.withAlpha (0.3f) : t.bg2);
    g.fillRoundedRectangle (r, t.noteStyle == 1 ? 0.0f : 4.0f);
    g.setColour (hi ? t.accent2 : t.edge);
    g.drawRoundedRectangle (r, t.noteStyle == 1 ? 0.0f : 4.0f, 1.0f);
    // grip dots
    g.setColour (t.dim);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 2; ++j) g.fillEllipse (r.getX() + 5 + j * 4, r.getCentreY() - 5 + i * 4, 2, 2);
    g.setColour (t.text);
    g.setFont (Font (FontOptions (12.0f, Font::bold)));
    g.drawText (label, r.withTrimmedLeft (14), Justification::centred);
}

void DragChip::mouseDown (const MouseEvent&) { dragging = false; }

void DragChip::mouseDrag (const MouseEvent& e)
{
    if (dragging || e.getDistanceFromDragStart() < 6) return;
    dragging = true;
    repaint();
    auto files = makeFiles();
    StringArray paths;
    for (auto& f : files) if (f.existsAsFile()) paths.add (f.getFullPathName());
    if (! paths.isEmpty())
        DragAndDropContainer::performExternalDragDropOfFiles (paths, false, this, [this] { dragging = false; repaint(); });
    else dragging = false;
}

void DragChip::mouseUp (const MouseEvent& e)
{
    if (! dragging && ! e.mouseWasDraggedSinceMouseDown())
    {
        auto files = makeFiles();
        if (onClickFiles) onClickFiles (files);
    }
    dragging = false;
    repaint();
}

// ------------------------------------------------------------------------------------------
StemList::StemList (StemScribeProcessor& p)
    : proc (p),
      allMidi ("All MIDI", [this] { Array<File> f; if (auto x = proc.exportAllMidi(); x.existsAsFile()) f.add (x); return f; })
{
    allMidi.setTooltip ("Drag every transcribed stem plus the chord tracks into the DAW as one multi-track .mid");
    allMidi.onClickFiles = [] (const Array<File>& f) { if (! f.isEmpty()) f[0].revealToUser(); };
    addAndMakeVisible (allMidi);
}

StemList::Row::Row (StemList& o, int i)
    : owner (o), index (i),
      wav ("WAV", [&o, i] { Array<File> f; if (auto x = o.proc.exportStemWav (i); x.existsAsFile()) f.add (x); return f; }),
      mid ("MIDI", [&o, i] { Array<File> f; if (auto x = o.proc.exportStemMidi (i); x.existsAsFile()) f.add (x); return f; })
{
    auto& ctl = owner.proc.stemControls[(size_t) jlimit (0, kMaxStems - 1, i)];
    for (auto* b : { &solo, &mute, &midiOut }) { b->setClickingTogglesState (true); addAndMakeVisible (*b); }
    solo.setToggleState (ctl.solo.load(), dontSendNotification);
    mute.setToggleState (ctl.mute.load(), dontSendNotification);
    midiOut.setToggleState (ctl.midiOut.load(), dontSendNotification);
    solo.onClick = [this, &ctl] { ctl.solo = solo.getToggleState(); };
    mute.onClick = [this, &ctl] { ctl.mute = mute.getToggleState(); };
    midiOut.onClick = [this, &ctl] { ctl.midiOut = midiOut.getToggleState(); };
    solo.setTooltip ("Solo"); mute.setTooltip ("Mute");
    midiOut.setTooltip ("Send this stem's notes out as live MIDI while the host plays (hosts that allow plugin MIDI output)");
    level.setRange (0.0, 1.5, 0.01);
    level.setValue (ctl.level.load(), dontSendNotification);
    level.setDoubleClickReturnValue (true, 1.0);
    level.onValueChange = [this, &ctl] { ctl.level = (float) level.getValue(); };
    level.setTooltip ("Playback level");
    addAndMakeVisible (level);
    mode.addItem ("No MIDI", 1);
    mode.addItem ("Mono (melody, bass)", 2);
    mode.addItem ("Poly (chords, keys)", 3);
    mode.addItem ("Drums", 4);
    if (auto* a = owner.proc.getAnalysis(); a && i < (int) a->stems.size()) mode.setSelectedId ((int) a->stems[(size_t) i].mode + 1, dontSendNotification);
    mode.onChange = [this] {
        const auto m = (ss::Transcribe) (mode.getSelectedId() - 1);
        auto* a = owner.proc.getAnalysis();
        if (a && index < (int) a->stems.size() && a->stems[(size_t) index].mode != m) owner.proc.retranscribe (index, m);
    };
    mode.setTooltip ("How this stem is turned into MIDI");
    addAndMakeVisible (mode);
    wav.setTooltip ("Drag the stem audio into the DAW (24-bit WAV). Click to show the file.");
    mid.setTooltip ("Drag this stem's MIDI into the DAW. Click to show the file.");
    wav.onClickFiles = mid.onClickFiles = [] (const Array<File>& f) { if (! f.isEmpty()) f[0].revealToUser(); };
    addAndMakeVisible (wav);
    addAndMakeVisible (mid);
}

void StemList::Row::paint (Graphics& g)
{
    const auto& t = themeOf (*this);
    auto* a = owner.proc.getAnalysis();
    if (! a || index >= (int) a->stems.size()) return;
    const auto& s = a->stems[(size_t) index];
    auto r = getLocalBounds().toFloat().reduced (4.0f, 3.0f);
    const bool sel = owner.proc.selectedStem == index;
    const Colour c = t.stemColour ((int) s.kind);
    g.setColour (sel ? t.panel.brighter (0.06f) : t.panel);
    if (t.noteStyle == 1) g.fillRect (r); else g.fillRoundedRectangle (r, 6.0f);
    if (sel) { g.setColour (c.withAlpha (0.8f)); if (t.noteStyle == 1) g.drawRect (r, 1.5f); else g.drawRoundedRectangle (r, 6.0f, 1.5f); }
    if (t.noteStyle == 4)   // tape label strip
    {
        g.setColour (Colour (0xfff1e6c8).withAlpha (0.9f));
        g.fillRect (r.getX() + 10, r.getY() + 5, 96.0f, 18.0f);
    }
    g.setColour (c);
    g.fillRect (r.getX(), r.getY() + 4, 4.0f, r.getHeight() - 8);
    if (t.noteStyle == 2) { g.setColour (c.withAlpha (0.25f)); g.fillRect (r.getX(), r.getY() + 4, 9.0f, r.getHeight() - 8); }
    g.setColour (t.noteStyle == 4 ? Colour (0xff2a2420) : t.text);
    g.setFont (Font (FontOptions (15.0f, Font::bold)));
    g.drawText (s.name, (int) r.getX() + 14, (int) r.getY() + 4, 100, 20, Justification::centredLeft);
    g.setColour (t.dim);
    g.setFont (Font (FontOptions (11.5f)));
    String info = String ((int) s.notes.size()) + (s.mode == ss::Transcribe::Drums ? " hits" : " notes");
    if (s.mode == ss::Transcribe::Off) info = "audio only";
    g.drawText (info, (int) r.getX() + 110, (int) r.getY() + 4, (int) r.getWidth() - 116, 20, Justification::centredRight);
}

void StemList::Row::resized()
{
    auto r = getLocalBounds().reduced (12, 6).withTrimmedLeft (4);
    r.removeFromTop (22);
    auto row1 = r.removeFromTop (26);
    solo.setBounds (row1.removeFromLeft (26).reduced (1));
    mute.setBounds (row1.removeFromLeft (26).reduced (1));
    row1.removeFromLeft (4);
    midiOut.setBounds (row1.removeFromRight (64).reduced (1));
    level.setBounds (row1.withTrimmedRight (4));
    r.removeFromTop (4);
    auto row2 = r.removeFromTop (26);
    mode.setBounds (row2.removeFromLeft (jmax (90, row2.getWidth() - 116)).reduced (1));
    row2.removeFromLeft (4);
    wav.setBounds (row2.removeFromLeft (56).reduced (1));
    mid.setBounds (row2.reduced (1));
}

void StemList::Row::mouseDown (const MouseEvent&)
{
    owner.proc.selectedStem = index;
    if (owner.onSelect) owner.onSelect (index);
    owner.repaint();
    for (auto* r : owner.rows) r->repaint();
}

void StemList::refresh()
{
    auto* a = proc.getAnalysis();
    const int n = a ? (int) a->stems.size() : 0;
    const int key = a ? (int) (a->key & 0x7fffffff) + n * 7 : -1;
    if (key == shownKey && rows.size() == n)
    {
        for (int i = 0; i < n; ++i)
        {
            rows[i]->mode.setSelectedId ((int) a->stems[(size_t) i].mode + 1, dontSendNotification);
            rows[i]->repaint();
        }
        return;
    }
    shownKey = key;
    rows.clear();
    for (int i = 0; i < n; ++i) addAndMakeVisible (rows.add (new Row (*this, i)));
    resized();
    repaint();
}

void StemList::paint (Graphics& g)
{
    const auto& t = themeOf (*this);
    g.setColour (t.bg2);
    g.fillRect (getLocalBounds());
    if (auto* lf = dynamic_cast<ScribeLookAndFeel*> (&getLookAndFeel())) lf->paintGrain (g, getLocalBounds());
    g.setColour (t.dim);
    g.setFont (Font (FontOptions (11.0f, Font::bold)));
    g.drawText ("STEMS", 14, 6, 100, 16, Justification::centredLeft);
    if (rows.isEmpty())
    {
        g.setColour (t.dim);
        g.setFont (Font (FontOptions (13.0f)));
        g.drawFittedText ("No analysis yet.\n\nDrop a song on the window, use Load, or press Capture and play the track in the host.",
                          getLocalBounds().reduced (16).withTrimmedTop (30).removeFromTop (120), Justification::topLeft, 6);
    }
}

void StemList::resized()
{
    auto r = getLocalBounds().withTrimmedTop (24);
    for (auto* row : rows) row->setBounds (r.removeFromTop (rowHeight));
    allMidi.setVisible (! rows.isEmpty());
    allMidi.setBounds (r.removeFromTop (40).reduced (14, 6));
}
