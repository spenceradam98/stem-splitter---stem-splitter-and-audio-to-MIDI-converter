#include "RollView.h"

#include "DrumTranscriber.h"
#include "Dsp.h"
#include "MidiFile.h"

using namespace juce;

namespace
{
constexpr int kRulerH = 20, kChordH = 26, kWaveH = 46, kKeysW = 46;

const Theme& themeOf (Component& c)
{
    if (auto* lf = dynamic_cast<ScribeLookAndFeel*> (&c.getLookAndFeel())) return lf->theme();
    return allThemes().front();
}

const std::vector<std::vector<int>>& scaleSteps()
{
    static const std::vector<std::vector<int>> s = {
        {},                                   // off
        { 0, 2, 4, 5, 7, 9, 11 },             // major
        { 0, 2, 3, 5, 7, 8, 10 },             // natural minor
        { 0, 2, 3, 5, 7, 8, 11 },             // harmonic minor
        { 0, 2, 3, 5, 7, 9, 10 },             // dorian
        { 0, 2, 4, 5, 7, 9, 10 },             // mixolydian
        { 0, 2, 4, 7, 9 },                    // major pentatonic
        { 0, 3, 5, 7, 10 },                   // minor pentatonic
        { 0, 3, 5, 6, 7, 10 },                // blues
    };
    return s;
}

bool isBlackKey (int p) { const int pc = ((p % 12) + 12) % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
} // namespace

const StringArray& RollView::scaleNames()
{
    static const StringArray n { "Scale snap: off", "Major", "Natural minor", "Harmonic minor", "Dorian", "Mixolydian", "Major pentatonic", "Minor pentatonic", "Blues" };
    return n;
}

RollView::RollView (StemScribeProcessor& p) : proc (p)
{
    addAndMakeVisible (hbar);
    addAndMakeVisible (vbar);
    hbar.addListener (this);
    vbar.addListener (this);
    hbar.setAutoHide (false);
    setWantsKeyboardFocus (true);
    startTimerHz (30);
}

RollView::~RollView() { hbar.removeListener (this); vbar.removeListener (this); }

ss::StemResult* RollView::stem()
{
    auto* a = proc.getAnalysis();
    if (! a || a->stems.empty()) return nullptr;
    return &a->stems[(size_t) jlimit (0, (int) a->stems.size() - 1, proc.selectedStem)];
}

bool RollView::isDrumView()
{
    auto* s = stem();
    return s && s->mode == ss::Transcribe::Drums;
}

std::vector<int> RollView::drumLanes()
{
    std::set<int> keys { ss::gm::openHat, ss::gm::closedHat, ss::gm::snare, ss::gm::kick };
    if (auto* s = stem()) for (auto& n : s->notes) keys.insert (n.pitch);
    return std::vector<int> (keys.rbegin(), keys.rend());   // highest note at the top
}

// ---- geometry ------------------------------------------------------------------------
Rectangle<int> RollView::rulerArea() const { return { kKeysW, 0, getWidth() - kKeysW - 12, kRulerH }; }
Rectangle<int> RollView::chordArea() const { return { kKeysW, kRulerH, getWidth() - kKeysW - 12, kChordH }; }
Rectangle<int> RollView::waveArea() const { return { kKeysW, kRulerH + kChordH, getWidth() - kKeysW - 12, kWaveH }; }
Rectangle<int> RollView::rollArea() const { const int y = kRulerH + kChordH + kWaveH; return { kKeysW, y, getWidth() - kKeysW - 12, getHeight() - y - 12 }; }
Rectangle<int> RollView::keysArea() const { auto r = rollArea(); return { 0, r.getY(), kKeysW, r.getHeight() }; }

float RollView::timeToX (double t) const { return (float) (kKeysW + (t - viewStart) * pxPerSec); }
double RollView::xToTime (float x) const { return viewStart + (x - kKeysW) / pxPerSec; }
float RollView::rowH() const { return (float) rollArea().getHeight() / semitonesVisible; }

float RollView::pitchToY (float pitch) const
{
    const auto r = rollArea();
    return (float) r.getBottom() - (pitch - lowPitch + 1.0f) * rowH();
}

float RollView::yToPitch (float y) const
{
    const auto r = rollArea();
    return lowPitch + ((float) r.getBottom() - y) / rowH() - 1.0f;
}

double RollView::snapTime (double t, bool force) const
{
    auto* a = const_cast<StemScribeProcessor&> (proc).getAnalysis();
    if (! a || (gridDivision <= 0 && ! force)) return t;
    const double step = 60.0 / jmax (20.0, a->tempo) / jmax (1, gridDivision);
    return a->beatOffset + std::round ((t - a->beatOffset) / step) * step;
}

int RollView::snapPitch (int p) const
{
    if (scaleSnap <= 0) return p;
    auto* a = const_cast<StemScribeProcessor&> (proc).getAnalysis();
    const int root = a ? a->keyRoot : 0;
    const auto& steps = scaleSteps()[(size_t) scaleSnap];
    int best = p, bestD = 99;
    for (int q = p - 6; q <= p + 6; ++q)
    {
        const int pc = ((q - root) % 12 + 12) % 12;
        if (std::find (steps.begin(), steps.end(), pc) != steps.end() && std::abs (q - p) < bestD) { bestD = std::abs (q - p); best = q; }
    }
    return best;
}

Rectangle<float> RollView::noteRect (const ss::Note& n)
{
    const float x0 = timeToX (n.start), x1 = jmax (x0 + 2.0f, timeToX (n.end));
    if (isDrumView())
    {
        const auto lanes = drumLanes();
        const auto r = rollArea();
        const float lh = jmin (34.0f, (float) r.getHeight() / (float) lanes.size());
        const int li = (int) (std::find (lanes.begin(), lanes.end(), n.pitch) - lanes.begin());
        const float y = (float) r.getY() + li * lh;
        return { x0, y + 3, jmax (8.0f, x1 - x0), lh - 6 };
    }
    const float y = pitchToY ((float) n.pitch);
    return { x0, y + 1, x1 - x0, jmax (2.0f, rowH() - 2) };
}

int RollView::hitNote (Point<float> p, bool& onEdge)
{
    onEdge = false;
    auto* s = stem();
    if (! s || ! rollArea().toFloat().contains (p)) return -1;
    for (int i = (int) s->notes.size() - 1; i >= 0; --i)
    {
        const auto r = noteRect (s->notes[(size_t) i]);
        if (r.expanded (1.0f, 0.0f).contains (p))
        {
            onEdge = ! isDrumView() && p.x > r.getRight() - jmin (7.0f, r.getWidth() * 0.35f);
            return i;
        }
    }
    return -1;
}

int RollView::hitChord (Point<float> p) const
{
    auto* a = const_cast<StemScribeProcessor&> (proc).getAnalysis();
    if (! a || ! chordArea().toFloat().contains (p)) return -1;
    const double t = xToTime (p.x);
    for (int i = 0; i < (int) a->chords.size(); ++i)
        if (t >= a->chords[(size_t) i].start && t < a->chords[(size_t) i].end) return i;
    return -1;
}

void RollView::resized()
{
    hbar.setBounds (kKeysW, getHeight() - 12, getWidth() - kKeysW - 12, 12);
    vbar.setBounds (getWidth() - 12, rollArea().getY(), 12, rollArea().getHeight());
    scrollBarMoved (nullptr, 0);
}

void RollView::scrollBarMoved (ScrollBar* bar, double v)
{
    if (bar == &hbar) viewStart = v;
    else if (bar == &vbar) lowPitch = (float) (127.0 - v - semitonesVisible);
    auto* a = proc.getAnalysis();
    const double dur = a ? a->duration : 60.0;
    const double visible = rollArea().getWidth() / pxPerSec;
    hbar.setRangeLimits (0.0, jmax (dur + 2.0, visible));
    hbar.setCurrentRange (viewStart, visible, dontSendNotification);
    vbar.setRangeLimits (0.0, 128.0 - 0.0);
    vbar.setCurrentRange (127.0 - lowPitch - semitonesVisible, semitonesVisible, dontSendNotification);
    vbar.setVisible (! isDrumView());
    repaint();
}

void RollView::zoomToFit()
{
    auto* a = proc.getAnalysis();
    auto* s = stem();
    if (! a) return;
    pxPerSec = jlimit (4.0, 800.0, rollArea().getWidth() / jmax (4.0, jmin (a->duration, 40.0)));
    viewStart = 0.0;
    if (s && ! s->notes.empty())
    {
        int lo = 127, hi = 0;
        for (auto& n : s->notes) { lo = jmin (lo, n.pitch); hi = jmax (hi, n.pitch); }
        semitonesVisible = (float) jlimit (24, 88, hi - lo + 8);
        lowPitch = (float) jlimit (0, 127 - (int) semitonesVisible, (lo + hi) / 2 - (int) semitonesVisible / 2);
    }
    else { semitonesVisible = 48; lowPitch = s && s->kind == ss::StemKind::Bass ? 24.0f : 40.0f; }
    scrollBarMoved (nullptr, 0);
}

void RollView::analysisChanged()
{
    selection.clear();
    if (chordEditor) { chordEditor.reset(); editingChord = -1; }
    zoomToFit();
    computeWaveform();
    if (themeOf (*this).spectrogram) computeSpectrogram();
    repaint();
}

void RollView::computeWaveform()
{
    auto* a = proc.getAnalysis();
    auto* s = stem();
    if (! a || ! s) { waveMin.clear(); waveMax.clear(); return; }
    if (waveStem == proc.selectedStem && waveKey == a->key) return;
    waveStem = proc.selectedStem; waveKey = a->key;
    const auto mono = s->audio.mono();
    const size_t blocks = mono.size() / 256 + 1;
    waveMin.assign (blocks, 0.0f); waveMax.assign (blocks, 0.0f);
    float peak = 1e-6f;
    for (float v : mono) peak = jmax (peak, std::abs (v));
    const float g = 1.0f / peak;   // the strip is normalised per stem
    for (size_t i = 0; i < mono.size(); ++i)
    {
        auto& lo = waveMin[i / 256]; auto& hi = waveMax[i / 256];
        lo = jmin (lo, mono[i] * g); hi = jmax (hi, mono[i] * g);
    }
}

void RollView::computeSpectrogram()
{
    auto* a = proc.getAnalysis();
    auto* s = stem();
    if (! a || ! s) return;
    if (spectroStem == proc.selectedStem && spectroKey == a->key && spectrogram.isValid()) return;
    spectroStem = proc.selectedStem; spectroKey = a->key;
    const int job = ++spectroJob;
    auto mono = std::make_shared<std::vector<float>> (ss::resample (s->audio.mono(), s->audio.sampleRate, 22050.0));
    WeakReference<RollView> safe (this);
    Thread::launch ([safe, mono, job] {
        // log-frequency spectrogram, 4 rows per semitone from MIDI 21 to 108, 10 columns per second
        const int n = 4096, hop = 2205;
        auto mag = ss::stftMagnitude (*mono, n, hop);
        const int rows = 88 * 4, cols = (int) mag.size();
        Image img (Image::ARGB, jmax (1, cols), rows, true);
        float peak = 1e-6f;
        for (auto& f : mag) for (float v : f) peak = jmax (peak, v);
        for (int x = 0; x < cols; ++x)
            for (int y = 0; y < rows; ++y)
            {
                const float midi = 108.0f - y / 4.0f;
                const float bin = ss::midiToHz (midi) * n / 22050.0f;
                const int b = jlimit (1, n / 2 - 1, (int) bin);
                const float frac = bin - (float) b;
                const float v = mag[(size_t) x][(size_t) b] * (1 - frac) + mag[(size_t) x][(size_t) b + 1] * frac;
                const float db = 20.0f * std::log10 (v / peak + 1e-7f);
                const float a = jlimit (0.0f, 1.0f, (db + 70.0f) / 70.0f);
                img.setPixelAt (x, y, Colour::fromFloatRGBA (a, a * a, 0.3f * a + 0.2f * a * a, a * 0.85f));
            }
        MessageManager::callAsync ([safe, img, job] {
            if (auto* v = safe.get()) if (v->spectroJob.get() == job) { v->spectrogram = img; v->repaint(); }
        });
    });
}

void RollView::timerCallback()
{
    static double lastPh = -1;
    const double ph = proc.getPlayheadSeconds();
    if (std::abs (ph - lastPh) > 1e-4)
    {
        // follow the playhead while playing
        const bool rolling = proc.isHostPlaying() || proc.isPreviewPlaying();
        const double visible = rollArea().getWidth() / pxPerSec;
        if (rolling && drag == Drag::None && (ph < viewStart || ph > viewStart + visible * 0.92))
        {
            viewStart = jmax (0.0, ph - visible * 0.08);
            scrollBarMoved (nullptr, 0);
        }
        lastPh = ph;
        repaint();
    }
}

// ---- painting ------------------------------------------------------------------------
void RollView::paint (Graphics& g)
{
    const auto& t = themeOf (*this);
    auto* a = proc.getAnalysis();
    auto* s = stem();
    g.fillAll (t.bg);
    const auto roll = rollArea();
    const bool drums = isDrumView();

    // ---- piano roll background ----
    g.saveState();
    g.reduceClipRegion (roll);
    if (drums)
    {
        const auto lanes = drumLanes();
        const float lh = jmin (34.0f, (float) roll.getHeight() / (float) lanes.size());
        for (size_t i = 0; i < lanes.size(); ++i)
        {
            g.setColour (i % 2 ? t.bg : t.blackKey);
            g.fillRect ((float) roll.getX(), roll.getY() + i * lh, (float) roll.getWidth(), lh);
        }
    }
    else
    {
        for (int p = (int) std::floor (lowPitch); p <= (int) (lowPitch + semitonesVisible) + 1; ++p)
        {
            const float y = pitchToY ((float) p);
            g.setColour (isBlackKey (p) ? t.blackKey : t.bg);
            g.fillRect ((float) roll.getX(), y, (float) roll.getWidth(), rowH());
            if (((p % 12) + 12) % 12 == 0) { g.setColour (t.grid.brighter (0.15f)); g.drawHorizontalLine ((int) (y + rowH()), (float) roll.getX(), (float) roll.getRight()); }
        }
        if (t.spectrogram && spectrogram.isValid() && a)
        {
            // spectrogram columns are 0.1 s, rows 1/4 semitone from MIDI 108 down to 21
            const float x0 = timeToX (0.0), x1 = timeToX (spectrogram.getWidth() * 0.1);
            const float yTop = pitchToY (108.0f) + rowH() * 0.5f, yBot = pitchToY (21.0f) + rowH() * 0.5f;
            g.setOpacity (0.75f);
            g.drawImage (spectrogram, Rectangle<float> (x0, yTop, x1 - x0, yBot - yTop), RectanglePlacement::stretchToFit);
            g.setOpacity (1.0f);
        }
    }
    // beat grid
    if (a)
    {
        const double beat = 60.0 / jmax (20.0, a->tempo);
        const double t0 = xToTime ((float) roll.getX()), t1 = xToTime ((float) roll.getRight());
        const long k0 = (long) std::floor ((t0 - a->beatOffset) / beat), k1 = (long) std::ceil ((t1 - a->beatOffset) / beat);
        const int sub = gridDivision > 0 && beat / gridDivision * pxPerSec > 8 ? gridDivision : 1;
        for (long k = k0; k <= k1; ++k)
            for (int q = 0; q < sub; ++q)
            {
                const float x = timeToX (a->beatOffset + (k + q / (double) sub) * beat);
                const bool bar = q == 0 && ((k % 4) + 4) % 4 == 0;
                g.setColour (bar ? t.grid.brighter (0.35f) : (q == 0 ? t.grid.brighter (0.1f) : t.grid.withAlpha (0.5f)));
                if (t.noteStyle == 1 && q != 0) continue;   // score paper: beats only
                g.drawVerticalLine ((int) x, (float) roll.getY(), (float) roll.getBottom());
            }
    }

    auto drawNote = [&] (const ss::Note& n, Colour c, bool selected, bool ghost) {
        auto r = noteRect (n);
        if (r.getRight() < roll.getX() || r.getX() > roll.getRight()) return;
        const float alpha = ghost ? 0.18f : (0.45f + 0.55f * n.velocity);
        const Colour fill = c.withAlpha (alpha);
        if (drums && ! ghost)
        {
            // a diamond per hit plus a velocity bar
            Path d;
            const float cx = r.getX() + 4, cy = r.getCentreY(), h = r.getHeight() * 0.42f;
            d.addQuadrilateral (cx, cy - h, cx + h * 0.8f, cy, cx, cy + h, cx - h * 0.8f, cy);
            if (t.noteStyle == 2) { g.setColour (c.withAlpha (0.25f)); g.strokePath (d, PathStrokeType (5.0f)); }
            g.setColour (fill); g.fillPath (d);
            g.setColour (selected ? t.text : c.darker (0.4f)); g.strokePath (d, PathStrokeType (selected ? 2.0f : 1.0f));
            g.setColour (c.withAlpha (0.5f));
            g.fillRect (cx - 1.0f, r.getBottom() - 2.0f, 3.0f + 14.0f * n.velocity, 2.0f);
            return;
        }
        switch (t.noteStyle)
        {
            case 1:  g.setColour (fill); g.fillRect (r); g.setColour (t.text.withAlpha (ghost ? 0.1f : 0.85f)); g.drawRect (r, 1.0f); break;
            case 2:
                if (! ghost) { g.setColour (c.withAlpha (0.18f)); g.fillRoundedRectangle (r.expanded (3.0f), 4.0f); }
                g.setColour (fill.withMultipliedAlpha (0.6f)); g.fillRoundedRectangle (r, 2.0f);
                g.setColour (c.brighter (0.4f).withAlpha (ghost ? 0.2f : 1.0f)); g.drawRoundedRectangle (r, 2.0f, 1.2f);
                break;
            case 3:  g.setColour (fill.withMultipliedAlpha (0.35f)); g.fillRect (r); g.setColour (c.withAlpha (ghost ? 0.25f : 1.0f)); g.drawRect (r, 1.5f); break;
            case 4:
                g.setColour (fill); g.fillRect (r);
                g.setColour (Colours::black.withAlpha (ghost ? 0.05f : 0.25f)); g.fillRect (r.withHeight (jmin (3.0f, r.getHeight() * 0.3f)).withY (r.getBottom() - jmin (3.0f, r.getHeight() * 0.3f)));
                break;
            default: g.setColour (fill); g.fillRoundedRectangle (r, jmin (4.0f, r.getHeight() * 0.4f));
        }
        if (selected) { g.setColour (t.text); g.drawRoundedRectangle (r.expanded (1.0f), 3.0f, 1.8f); }
        if (! ghost && r.getWidth() > 30 && r.getHeight() > 10)
        {
            g.setColour ((t.noteStyle == 1 ? t.text : t.bg).withAlpha (0.85f));
            g.setFont (Font (FontOptions (jmin (11.0f, r.getHeight() - 1))));
            g.drawText (String (ss::midiNoteName (n.pitch)), r.reduced (3, 0), Justification::centredLeft, false);
        }
    };

    if (a && showGhosts && ! drums)
        for (size_t i = 0; i < a->stems.size(); ++i)
        {
            if ((int) i == proc.selectedStem || a->stems[i].mode == ss::Transcribe::Drums) continue;
            for (auto& n : a->stems[i].notes) drawNote (n, t.stemColour ((int) a->stems[i].kind), false, true);
        }
    if (s)
    {
        const Colour c = t.stemColour ((int) s->kind);
        for (size_t i = 0; i < s->notes.size(); ++i) drawNote (s->notes[i], c, selection.count ((int) i) > 0, false);
        // detected pitch curve over the notes
        if (! drums && ! s->curve.midi.empty())
        {
            Path curve;
            bool pen = false;
            const double hop = s->curve.hopSeconds;
            const long f0 = jmax (0L, (long) (xToTime ((float) roll.getX()) / hop) - 1);
            const long f1 = jmin ((long) s->curve.midi.size(), (long) (xToTime ((float) roll.getRight()) / hop) + 2);
            const int stepF = jmax (1, (int) (1.0 / (hop * pxPerSec)));
            for (long f = f0; f < f1; f += stepF)
            {
                const float m = s->curve.midi[(size_t) f];
                if (m <= 0) { pen = false; continue; }
                const float x = timeToX (f * hop), y = pitchToY (m) + rowH() * 0.5f;
                if (! pen) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
                pen = true;
            }
            const Colour cc = t.noteStyle == 1 ? t.accent2 : t.text;
            if (t.noteStyle == 2) { g.setColour (t.accent2.withAlpha (0.3f)); g.strokePath (curve, PathStrokeType (4.0f)); }
            g.setColour (cc.withAlpha (0.9f));
            g.strokePath (curve, PathStrokeType (1.4f, PathStrokeType::curved, PathStrokeType::rounded));
        }
    }
    if (t.scanlines)
    {
        g.setColour (Colours::black.withAlpha (0.18f));
        for (int y = roll.getY(); y < roll.getBottom(); y += 3) g.drawHorizontalLine (y, (float) roll.getX(), (float) roll.getRight());
    }
    if (drag == Drag::Rubber) { g.setColour (t.accent.withAlpha (0.15f)); g.fillRect (rubber); g.setColour (t.accent); g.drawRect (rubber, 1.0f); }
    if (! s)
    {
        g.setColour (t.dim);
        g.setFont (Font (FontOptions (15.0f)));
        g.drawFittedText ("The piano roll shows the detected pitch curve over the note blocks once a song is analysed.\n"
                          "Click a note to select it, drag to move, drag its right edge to resize, double-click to draw.",
                          roll.reduced (40), Justification::centred, 4);
    }
    g.restoreState();

    // ---- keyboard / drum names ----
    const auto keys = keysArea();
    g.setColour (t.panel);
    g.fillRect (keys);
    g.saveState();
    g.reduceClipRegion (keys);
    if (drums)
    {
        const auto lanes = drumLanes();
        const float lh = jmin (34.0f, (float) roll.getHeight() / (float) lanes.size());
        g.setFont (Font (FontOptions (10.5f)));
        for (size_t i = 0; i < lanes.size(); ++i)
        {
            g.setColour (t.text);
            g.drawFittedText (ss::gmDrumName (lanes[i]), 2, (int) (roll.getY() + i * lh), kKeysW - 4, (int) lh, Justification::centredLeft, 2);
        }
    }
    else
        for (int p = (int) std::floor (lowPitch); p <= (int) (lowPitch + semitonesVisible) + 1; ++p)
        {
            const float y = pitchToY ((float) p);
            g.setColour (isBlackKey (p) ? t.blackKey.darker (0.3f) : t.text.withAlpha (0.85f));
            g.fillRect (2.0f, y + 0.5f, (float) (isBlackKey (p) ? kKeysW - 16 : kKeysW - 4), rowH() - 1.0f);
            if (((p % 12) + 12) % 12 == 0 && rowH() > 6)
            {
                g.setColour (t.bg);
                g.setFont (Font (FontOptions (jmin (10.0f, rowH()))));
                g.drawText (String (ss::midiNoteName (p)), 4, (int) y, kKeysW - 8, (int) rowH(), Justification::centredRight);
            }
        }
    g.restoreState();

    // ---- ruler ----
    const auto ruler = rulerArea();
    g.setColour (t.panel);
    g.fillRect (ruler);
    g.fillRect (0, 0, kKeysW, roll.getY());
    if (auto* lf = dynamic_cast<ScribeLookAndFeel*> (&getLookAndFeel())) lf->paintGrain (g, { 0, 0, getWidth(), roll.getY() });
    if (a)
    {
        const double beat = 60.0 / jmax (20.0, a->tempo), bar = beat * 4;
        const double t0 = xToTime ((float) ruler.getX()), t1 = xToTime ((float) ruler.getRight());
        const int every = jmax (1, (int) std::ceil (50.0 / (bar * pxPerSec)));
        g.setFont (Font (FontOptions (11.0f)));
        for (long k = (long) std::floor ((t0 - a->beatOffset) / bar); a->beatOffset + k * bar < t1; ++k)
        {
            if (k % every) continue;
            const float x = timeToX (a->beatOffset + k * bar);
            g.setColour (t.dim);
            g.drawVerticalLine ((int) x, (float) ruler.getY() + 8, (float) ruler.getBottom());
            g.setColour (t.text);
            g.drawText (String (k + 1), (int) x + 3, ruler.getY(), 40, ruler.getHeight(), Justification::centredLeft);
        }
    }
    g.setColour (t.dim);
    g.setFont (Font (FontOptions (10.0f, Font::bold)));
    g.drawText ("BAR", 4, ruler.getY(), kKeysW - 6, ruler.getHeight(), Justification::centredLeft);
    g.drawText ("CHORD", 4, chordArea().getY(), kKeysW - 6, kChordH, Justification::centredLeft);
    g.drawText ("WAVE", 4, waveArea().getY(), kKeysW - 6, kWaveH, Justification::centredLeft);

    // ---- chord lane ----
    const auto chordR = chordArea();
    g.setColour (t.bg2);
    g.fillRect (chordR);
    if (a)
    {
        g.saveState();
        g.reduceClipRegion (chordR);
        for (size_t i = 0; i < a->chords.size(); ++i)
        {
            const auto& c = a->chords[i];
            const float x0 = timeToX (c.start), x1 = timeToX (c.end);
            if (x1 < chordR.getX() || x0 > chordR.getRight()) continue;
            auto r = Rectangle<float> (x0 + 1, (float) chordR.getY() + 3, x1 - x0 - 2, (float) chordR.getHeight() - 6);
            const bool none = c.root < 0;
            g.setColour (none ? t.bg2 : t.accent2.withAlpha (t.noteStyle == 1 ? 0.12f : 0.22f));
            if (t.noteStyle == 1) g.fillRect (r); else g.fillRoundedRectangle (r, 3.0f);
            g.setColour (none ? t.dim : t.text);
            g.setFont (Font (FontOptions (13.0f, t.noteStyle == 1 ? Font::italic : Font::bold)));
            if (r.getWidth() > 14) g.drawText (String (c.name()), r.reduced (4, 0), Justification::centredLeft, true);
        }
        g.restoreState();
    }

    // ---- waveform strip ----
    const auto wr = waveArea();
    g.setColour (t.bg);
    g.fillRect (wr);
    if (s && ! waveMax.empty())
    {
        const Colour c = t.stemColour ((int) s->kind);
        g.setColour (c.withAlpha (0.75f));
        const float mid = (float) wr.getCentreY(), h = wr.getHeight() * 0.48f;
        const double blockSecs = 256.0 / s->audio.sampleRate;
        for (int x = wr.getX(); x < wr.getRight(); ++x)
        {
            const double ta = xToTime ((float) x), tb = xToTime ((float) x + 1);
            const long b0 = (long) (ta / blockSecs), b1 = jmax (b0 + 1, (long) (tb / blockSecs));
            float lo = 0, hi = 0;
            for (long b = jmax (0L, b0); b < b1 && b < (long) waveMax.size(); ++b) { lo = jmin (lo, waveMin[(size_t) b]); hi = jmax (hi, waveMax[(size_t) b]); }
            if (hi > lo) g.drawVerticalLine (x, mid - hi * h, mid - lo * h + 1);
        }
    }
    g.setColour (t.edge);
    g.drawHorizontalLine (wr.getBottom() - 1, 0.0f, (float) getWidth());
    g.drawHorizontalLine (chordR.getBottom() - 1, 0.0f, (float) getWidth());

    // ---- playhead ----
    if (a)
    {
        const float x = timeToX (proc.getPlayheadSeconds());
        if (x >= kKeysW && x <= roll.getRight())
        {
            g.setColour (t.accent);
            g.drawLine (x, 0, x, (float) roll.getBottom(), 1.5f);
            Path tri; tri.addTriangle (x - 5, 0, x + 5, 0, x, 7);
            g.fillPath (tri);
        }
    }
}

// ---- editing -------------------------------------------------------------------------
void RollView::commit (bool chords)
{
    auto* s = stem();
    std::vector<ss::Note> sel;
    if (s) for (int i : selection) if (i < (int) s->notes.size()) sel.push_back (s->notes[(size_t) i]);
    proc.analysisEdited (chords);
    selection.clear();
    if ((s = stem()))
        for (auto& n : sel)
            for (int i = 0; i < (int) s->notes.size(); ++i)
            {
                const auto& m = s->notes[(size_t) i];
                if (m.pitch == n.pitch && m.start == n.start && m.end == n.end) { selection.insert (i); break; }
            }
    repaint();
}

void RollView::mouseMove (const MouseEvent& e)
{
    bool edge = false;
    const int hit = hitNote (e.position, edge);
    setMouseCursor (hit >= 0 ? (edge ? MouseCursor::LeftRightResizeCursor : MouseCursor::DraggingHandCursor) : MouseCursor::NormalCursor);
    hoverTime = xToTime (e.position.x);
}

void RollView::mouseDown (const MouseEvent& e)
{
    grabKeyboardFocus();
    if (chordEditor) { chordEditor.reset(); editingChord = -1; }
    dragStart = e.position;
    auto* s = stem();
    if (rulerArea().contains (e.getPosition()) || waveArea().contains (e.getPosition()))
    {
        drag = Drag::Scrub;
        proc.setPreviewPosition (jmax (0.0, xToTime (e.position.x)));
        return;
    }
    if (const int c = hitChord (e.position); c >= 0)
    {
        if (e.mods.isPopupMenu()) showChordMenu (c, xToTime (e.position.x));
        return;
    }
    if (! s || ! rollArea().contains (e.getPosition())) return;
    if (e.mods.isPopupMenu()) { showRollMenu (e.getPosition()); return; }

    bool edge = false;
    const int hit = hitNote (e.position, edge);
    if (hit >= 0)
    {
        if (e.mods.isShiftDown()) { if (! selection.erase (hit)) selection.insert (hit); }
        else if (! selection.count (hit)) { selection.clear(); selection.insert (hit); }
        dragAnchor = hit;
        drag = e.mods.isAltDown() ? Drag::Velocity : (edge ? Drag::Resize : Drag::Move);
        proc.pushUndo();
        dragOrig = s->notes;
    }
    else
    {
        if (! e.mods.isShiftDown()) selection.clear();
        drag = Drag::Rubber;
        rubber = {};
    }
    repaint();
}

void RollView::mouseDrag (const MouseEvent& e)
{
    auto* s = stem();
    const auto d = e.position - dragStart;
    if (drag == Drag::Scrub) { proc.setPreviewPosition (jmax (0.0, xToTime (e.position.x))); repaint(); return; }
    if (! s) return;
    if (drag == Drag::Rubber)
    {
        rubber = Rectangle<float> (dragStart, e.position);
        selection.clear();
        for (int i = 0; i < (int) s->notes.size(); ++i)
            if (rubber.intersects (noteRect (s->notes[(size_t) i]))) selection.insert (i);
        repaint();
        return;
    }
    if (dragAnchor < 0 || dragAnchor >= (int) dragOrig.size()) return;
    const auto& anchor = dragOrig[(size_t) dragAnchor];
    if (drag == Drag::Move)
    {
        double dt = d.x / pxPerSec;
        // snap the anchor's start to the grid, move the others by the same amount
        if (gridDivision > 0 && ! e.mods.isCommandDown()) dt = snapTime (anchor.start + dt) - anchor.start;
        int dp = 0;
        if (! isDrumView()) dp = (int) std::round (-d.y / rowH());
        else
        {
            const auto lanes = drumLanes();
            const auto r = rollArea();
            const float lh = jmin (34.0f, (float) r.getHeight() / (float) lanes.size());
            const int from = (int) (std::find (lanes.begin(), lanes.end(), anchor.pitch) - lanes.begin());
            const int to = jlimit (0, (int) lanes.size() - 1, from + (int) std::round (d.y / lh));
            dp = lanes[(size_t) to] - anchor.pitch;
        }
        for (int i : selection)
        {
            if (i >= (int) s->notes.size()) continue;
            auto& n = s->notes[(size_t) i];
            const auto& o = dragOrig[(size_t) i];
            const double len = o.end - o.start;
            n.start = jmax (0.0, o.start + dt);
            n.end = n.start + len;
            n.pitch = isDrumView() ? jlimit (0, 127, o.pitch + dp) : snapPitch (jlimit (0, 127, o.pitch + dp));
        }
    }
    else if (drag == Drag::Resize)
    {
        double dt = d.x / pxPerSec;
        if (gridDivision > 0 && ! e.mods.isCommandDown()) dt = snapTime (anchor.end + dt) - anchor.end;
        for (int i : selection)
        {
            if (i >= (int) s->notes.size()) continue;
            auto& n = s->notes[(size_t) i];
            n.end = jmax (n.start + 0.02, dragOrig[(size_t) i].end + dt);
        }
    }
    else if (drag == Drag::Velocity)
    {
        for (int i : selection)
            if (i < (int) s->notes.size())
                s->notes[(size_t) i].velocity = jlimit (0.02f, 1.0f, dragOrig[(size_t) i].velocity - d.y / 150.0f);
    }
    repaint();
}

void RollView::mouseUp (const MouseEvent& e)
{
    const auto was = drag;
    drag = Drag::None;
    if ((was == Drag::Move || was == Drag::Resize || was == Drag::Velocity) && e.mouseWasDraggedSinceMouseDown()) commit (true);
    else if (was == Drag::Move || was == Drag::Resize || was == Drag::Velocity) proc.dropUndo();   // a click: no edit
    repaint();
}

void RollView::mouseDoubleClick (const MouseEvent& e)
{
    if (const int c = hitChord (e.position); c >= 0) { editChord (c); return; }
    auto* s = stem();
    auto* a = proc.getAnalysis();
    if (! s || ! a || ! rollArea().contains (e.getPosition())) return;
    bool edge = false;
    if (hitNote (e.position, edge) >= 0) return;
    proc.pushUndo();
    ss::Note n;
    const double beat = 60.0 / jmax (20.0, a->tempo);
    n.start = jmax (0.0, gridDivision > 0 ? snapTime (xToTime (e.position.x) - beat / gridDivision / 2) : xToTime (e.position.x));
    n.velocity = 0.8f;
    if (isDrumView())
    {
        const auto lanes = drumLanes();
        const auto r = rollArea();
        const float lh = jmin (34.0f, (float) r.getHeight() / (float) lanes.size());
        n.pitch = lanes[(size_t) jlimit (0, (int) lanes.size() - 1, (int) ((e.position.y - r.getY()) / lh))];
        n.end = n.start + 0.06;
    }
    else
    {
        n.pitch = snapPitch (jlimit (0, 127, (int) std::floor (yToPitch (e.position.y) + 1.0f)));
        n.end = n.start + beat / jmax (1, gridDivision > 0 ? gridDivision / 2 : 2);
    }
    s->notes.push_back (n);
    selection.clear();
    selection.insert ((int) s->notes.size() - 1);
    commit (true);
}

void RollView::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        const double t = xToTime (e.position.x);
        pxPerSec = jlimit (4.0, 1500.0, pxPerSec * std::pow (1.15, w.deltaY * 6.0));
        viewStart = jmax (0.0, t - (e.position.x - kKeysW) / pxPerSec);
    }
    else if (e.mods.isShiftDown() && ! isDrumView())
        lowPitch = jlimit (0.0f, 127.0f - semitonesVisible, lowPitch + w.deltaY * 12.0f);
    else if (e.mods.isAltDown() && ! isDrumView())
        semitonesVisible = jlimit (12.0f, 96.0f, semitonesVisible * (float) std::pow (1.1, -w.deltaY * 6.0));
    else
    {
        const float dx = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : 0.0f;
        if (dx != 0.0f) viewStart = jmax (0.0, viewStart - dx * 400.0 / pxPerSec);
        else if (! isDrumView()) lowPitch = jlimit (0.0f, 127.0f - semitonesVisible, lowPitch + w.deltaY * 8.0f);
        else viewStart = jmax (0.0, viewStart - w.deltaY * 400.0 / pxPerSec);
    }
    scrollBarMoved (nullptr, 0);
}

bool RollView::keyPressed (const KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (k.isKeyCode (KeyPress::deleteKey) || k.isKeyCode (KeyPress::backspaceKey)) { deleteSelection(); return true; }
    if (mods.isCommandDown() && k.getKeyCode() == 'Z') { if (mods.isShiftDown()) proc.redo(); else proc.undo(); selection.clear(); repaint(); return true; }
    if (mods.isCommandDown() && k.getKeyCode() == 'Y') { proc.redo(); selection.clear(); repaint(); return true; }
    if (mods.isCommandDown() && k.getKeyCode() == 'A') { selectAll(); return true; }
    if (k.getKeyCode() == 'S' && ! mods.isCommandDown()) { splitSelectionAt (hoverTime); return true; }
    if (k.getKeyCode() == 'M' && ! mods.isCommandDown()) { mergeSelection(); return true; }
    if (k.getKeyCode() == 'Q' && ! mods.isCommandDown()) { quantizeSelectionOrAll (mods.isShiftDown() ? 0.5f : 1.0f); return true; }
    if (k.isKeyCode (KeyPress::upKey)) { transposeSelection (mods.isShiftDown() ? 12 : 1); return true; }
    if (k.isKeyCode (KeyPress::downKey)) { transposeSelection (mods.isShiftDown() ? -12 : -1); return true; }
    if (k.isKeyCode (KeyPress::spaceKey)) { proc.setPreviewPlaying (! proc.isPreviewPlaying()); return true; }
    if (k.getKeyCode() == 'F') { zoomToFit(); return true; }
    return false;
}

void RollView::deleteSelection()
{
    auto* s = stem();
    if (! s || selection.empty()) return;
    proc.pushUndo();
    for (auto it = selection.rbegin(); it != selection.rend(); ++it)
        if (*it < (int) s->notes.size()) s->notes.erase (s->notes.begin() + *it);
    selection.clear();
    commit (true);
}

void RollView::splitSelectionAt (double t)
{
    auto* s = stem();
    if (! s) return;
    std::vector<int> targets (selection.begin(), selection.end());
    if (targets.empty())
        for (int i = 0; i < (int) s->notes.size(); ++i) if (s->notes[(size_t) i].start < t && s->notes[(size_t) i].end > t) targets.push_back (i);
    bool any = false;
    std::vector<ss::Note> added;
    for (int i : targets)
    {
        if (i >= (int) s->notes.size()) continue;
        auto& n = s->notes[(size_t) i];
        if (! (t > n.start + 0.02 && t < n.end - 0.02)) continue;
        if (! any) { proc.pushUndo(); any = true; }
        ss::Note b = n;
        const double frac = (t - n.start) / (n.end - n.start);
        const size_t cut = (size_t) (frac * (double) n.bend.size());
        b.start = t;
        if (! n.bend.empty()) { b.bend.assign (n.bend.begin() + (long) cut, n.bend.end()); n.bend.resize (cut); }
        n.end = t;
        added.push_back (b);
    }
    for (auto& b : added) s->notes.push_back (b);
    if (any) commit (true);
}

void RollView::mergeSelection()
{
    auto* s = stem();
    if (! s || selection.size() < 2) return;
    proc.pushUndo();
    std::vector<int> idx (selection.begin(), selection.end());
    auto& first = s->notes[(size_t) idx.front()];
    double end = first.end;
    float vel = first.velocity;
    for (size_t k = 1; k < idx.size(); ++k) { auto& n = s->notes[(size_t) idx[k]]; end = jmax (end, n.end); vel = jmax (vel, n.velocity); }
    first.end = end; first.velocity = vel; first.bend.clear();
    for (size_t k = idx.size() - 1; k >= 1; --k) s->notes.erase (s->notes.begin() + idx[k]);
    selection.clear();
    selection.insert (idx.front());
    commit (true);
}

void RollView::transposeSelection (int semis)
{
    auto* s = stem();
    if (! s || selection.empty() || isDrumView()) return;
    proc.pushUndo();
    for (int i : selection) if (i < (int) s->notes.size()) s->notes[(size_t) i].pitch = jlimit (0, 127, s->notes[(size_t) i].pitch + semis);
    commit (true);
}

void RollView::quantizeSelectionOrAll (float strength)
{
    auto* s = stem();
    auto* a = proc.getAnalysis();
    if (! s || ! a || s->notes.empty()) return;
    proc.pushUndo();
    const int div = gridDivision > 0 ? gridDivision : 4;
    if (selection.empty()) ss::quantizeNotes (s->notes, a->tempo, a->beatOffset, div, strength);
    else
    {
        std::vector<ss::Note> sel;
        for (int i : selection) sel.push_back (s->notes[(size_t) i]);
        ss::quantizeNotes (sel, a->tempo, a->beatOffset, div, strength);
        size_t k = 0;
        for (int i : selection) s->notes[(size_t) i] = sel[k++];
    }
    commit (true);
}

void RollView::selectAll()
{
    selection.clear();
    if (auto* s = stem()) for (int i = 0; i < (int) s->notes.size(); ++i) selection.insert (i);
    repaint();
}

void RollView::editChord (int index)
{
    auto* a = proc.getAnalysis();
    if (! a || index >= (int) a->chords.size()) return;
    const auto& c = a->chords[(size_t) index];
    const auto r = chordArea();
    const float x0 = jmax ((float) r.getX(), timeToX (c.start)), x1 = jmin ((float) r.getRight(), timeToX (c.end));
    chordEditor = std::make_unique<TextEditor>();
    addAndMakeVisible (*chordEditor);
    chordEditor->setBounds ((int) x0, r.getY() + 2, jmax (70, (int) (x1 - x0)), r.getHeight() - 4);
    chordEditor->setText (c.name(), false);
    chordEditor->selectAll();
    chordEditor->grabKeyboardFocus();
    editingChord = index;
    chordEditor->onReturnKey = [this] {
        auto* an = proc.getAnalysis();
        ss::Chord parsed;
        if (an && editingChord >= 0 && editingChord < (int) an->chords.size() && ss::parseChordName (chordEditor->getText().toStdString(), parsed))
        {
            proc.pushUndo();
            auto& c2 = an->chords[(size_t) editingChord];
            c2.root = parsed.root; c2.quality = parsed.quality; c2.bass = parsed.bass;
            proc.analysisEdited (false);
        }
        MessageManager::callAsync ([w = WeakReference<RollView> (this)] { if (auto* v = w.get()) { v->chordEditor.reset(); v->editingChord = -1; v->repaint(); } });
    };
    chordEditor->onEscapeKey = chordEditor->onFocusLost = [this] {
        MessageManager::callAsync ([w = WeakReference<RollView> (this)] { if (auto* v = w.get()) { v->chordEditor.reset(); v->editingChord = -1; v->repaint(); } });
    };
}

void RollView::showChordMenu (int index, double t)
{
    PopupMenu m;
    m.addItem (1, "Rename chord...");
    m.addItem (2, "Split chord here");
    m.addItem (3, "Merge with next chord");
    m.addItem (4, "No chord (N)");
    m.addSeparator();
    m.addItem (5, "Re-detect all chords from the notes");
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (this).withMousePosition(),
        [this, index, t] (int r) {
            auto* a = proc.getAnalysis();
            if (! a || index >= (int) a->chords.size()) return;
            if (r == 1) { editChord (index); return; }
            if (r == 5) { proc.pushUndo(); ss::updateHarmony (*a); proc.analysisEdited (false); return; }
            if (r == 0) return;
            proc.pushUndo();
            auto& c = a->chords[(size_t) index];
            if (r == 2 && t > c.start && t < c.end) { auto b = c; const double cut = snapTime (t, true); c.end = cut; b.start = cut; a->chords.insert (a->chords.begin() + index + 1, b); }
            if (r == 3 && index + 1 < (int) a->chords.size()) { c.end = a->chords[(size_t) index + 1].end; a->chords.erase (a->chords.begin() + index + 1); }
            if (r == 4) c.root = -1;
            proc.analysisEdited (false);
        });
}

void RollView::showRollMenu (Point<int>)
{
    PopupMenu m;
    m.addItem (1, "Delete selected notes", ! selection.empty());
    m.addItem (2, "Split at mouse (S)");
    m.addItem (3, "Merge selected (M)", selection.size() > 1);
    m.addItem (4, "Quantize (Q)");
    m.addItem (5, "Quantize 50% (Shift+Q)");
    m.addSeparator();
    m.addItem (6, "Select all");
    m.addItem (7, "Show other stems' notes", true, showGhosts);
    m.addItem (8, "Zoom to fit (F)");
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (this).withMousePosition(),
        [this, t = hoverTime] (int r) {
            switch (r)
            {
                case 1: deleteSelection(); break;
                case 2: splitSelectionAt (t); break;
                case 3: mergeSelection(); break;
                case 4: quantizeSelectionOrAll (1.0f); break;
                case 5: quantizeSelectionOrAll (0.5f); break;
                case 6: selectAll(); break;
                case 7: showGhosts = ! showGhosts; repaint(); break;
                case 8: zoomToFit(); break;
                default: break;
            }
        });
}
