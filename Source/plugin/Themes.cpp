#include "Themes.h"

using namespace juce;

Colour Theme::stemColour (int kind) const
{
    // vocals, drums, bass, other, guitar, piano, mix: hues spread around the theme's accent
    static const float offs[] = { 0.0f, 0.13f, 0.58f, 0.36f, 0.08f, 0.78f, 0.5f };
    const float base = accent.getHue();
    const float sat = jlimit (0.4f, 0.85f, accent.getSaturation());
    const float bri = jlimit (0.6f, 0.95f, accent.getBrightness());
    return Colour::fromHSV (std::fmod (base + offs[jlimit (0, 6, kind)], 1.0f), sat, bri, 1.0f);
}

const std::vector<Theme>& allThemes()
{
    static const std::vector<Theme> t = [] {
        std::vector<Theme> v;
        auto add = [&] (const char* name, uint32 bg, uint32 bg2, uint32 panel, uint32 edge, uint32 text, uint32 dim,
                        uint32 accent, uint32 accent2, uint32 grid, uint32 blackKey, int style, bool spec, bool grain, bool scan)
        {
            v.push_back ({ name, Colour (bg), Colour (bg2), Colour (panel), Colour (edge), Colour (text), Colour (dim),
                           Colour (accent), Colour (accent2), Colour (grid), Colour (blackKey), style, spec, grain, scan });
        };
        //   name             bg          bg2         panel       edge        text        dim         accent      accent2     grid        blackKey
        add ("Studio Night",  0xff10141c, 0xff171d29, 0xff1c2433, 0xff2f3b52, 0xffe7ecf4, 0xff8390a8, 0xff52c7d8, 0xffffb35c, 0xff252e40, 0xff141a25, 0, false, false, false);
        add ("Paper Score",   0xffefe8d8, 0xffe4dcc8, 0xfff8f3e7, 0xffbfb294, 0xff241f17, 0xff7b7160, 0xff2e5e8c, 0xffa8402f, 0xffd9cfb8, 0xffe2d9c3, 1, false, true, false);
        add ("Neon Grid",     0xff07060d, 0xff0f0b1c, 0xff120d22, 0xff3b2266, 0xfff3ebff, 0xff8d7bb0, 0xffff3fb0, 0xff2ef2ff, 0xff1d1534, 0xff0b0817, 2, false, false, true);
        add ("Spectral",      0xff05080a, 0xff0b1216, 0xff0f181d, 0xff23404a, 0xffdff7f2, 0xff7aa39c, 0xff7cf2c4, 0xfffff07a, 0xff15262c, 0xff081014, 3, true, false, false);
        add ("Tape Deck",     0xff2a2420, 0xff3a312a, 0xff463b32, 0xff7d6a57, 0xfff4e9d6, 0xffb9a68c, 0xffe8a33c, 0xffd8553a, 0xff3f352d, 0xff2f2823, 4, false, true, false);
        return v;
    }();
    return t;
}

const Theme& themeByName (const String& name)
{
    for (auto& t : allThemes()) if (t.name == name) return t;
    return allThemes().front();
}

ScribeLookAndFeel::ScribeLookAndFeel() { setTheme (allThemes().front()); }

void ScribeLookAndFeel::setTheme (const Theme& t)
{
    th = &t;
    setColour (ComboBox::backgroundColourId, t.bg2);
    setColour (ComboBox::textColourId, t.text);
    setColour (ComboBox::outlineColourId, t.edge);
    setColour (ComboBox::arrowColourId, t.dim);
    setColour (PopupMenu::backgroundColourId, t.bg2);
    setColour (PopupMenu::textColourId, t.text);
    setColour (PopupMenu::headerTextColourId, t.accent);
    setColour (PopupMenu::highlightedBackgroundColourId, t.accent.withAlpha (0.35f));
    setColour (PopupMenu::highlightedTextColourId, t.text);
    setColour (TextButton::textColourOffId, t.text);
    setColour (TextButton::textColourOnId, t.bg);
    setColour (TextButton::buttonColourId, t.bg2);
    setColour (TextButton::buttonOnColourId, t.accent);
    setColour (ToggleButton::textColourId, t.text);
    setColour (Label::textColourId, t.text);
    setColour (TextEditor::backgroundColourId, t.bg2);
    setColour (TextEditor::textColourId, t.text);
    setColour (TextEditor::outlineColourId, t.edge);
    setColour (TextEditor::focusedOutlineColourId, t.accent);
    setColour (TooltipWindow::backgroundColourId, t.bg2);
    setColour (TooltipWindow::textColourId, t.text);
    setColour (AlertWindow::backgroundColourId, t.bg2);
    setColour (AlertWindow::textColourId, t.text);
    setColour (Slider::thumbColourId, t.accent);
    setColour (Slider::trackColourId, t.accent.withAlpha (0.6f));
    setColour (Slider::backgroundColourId, t.grid);
    setColour (ScrollBar::thumbColourId, t.edge);
    setColour (ProgressBar::foregroundColourId, t.accent);
    setColour (ProgressBar::backgroundColourId, t.bg2);
    setColour (ListBox::backgroundColourId, t.panel);
    setColour (ResizableWindow::backgroundColourId, t.bg);
    setColour (DocumentWindow::textColourId, t.text);
}

Font ScribeLookAndFeel::getComboBoxFont (ComboBox&) { return Font (FontOptions (13.0f)); }
Font ScribeLookAndFeel::getPopupMenuFont() { return Font (FontOptions (14.0f)); }
Font ScribeLookAndFeel::getTextButtonFont (TextButton&, int h) { return Font (FontOptions (jmin (14.0f, h * 0.55f), th->noteStyle == 1 ? Font::plain : Font::bold)); }

void ScribeLookAndFeel::paintGrain (Graphics& g, Rectangle<int> area) const
{
    if (! th->grain) return;
    Random r (1234);
    g.setColour (th->text.withAlpha (0.035f));
    for (int i = 0; i < area.getWidth() * area.getHeight() / 90; ++i)
        g.fillRect ((float) area.getX() + r.nextFloat() * area.getWidth(), (float) area.getY() + r.nextFloat() * area.getHeight(), 1.2f, 1.2f);
}

void ScribeLookAndFeel::drawButtonBackground (Graphics& g, Button& b, const Colour&, bool hi, bool down)
{
    const auto& t = *th;
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();
    Colour fill = on ? t.accent : t.bg2;
    if (hi) fill = fill.brighter (0.08f);
    if (down) fill = fill.darker (0.1f);
    switch (t.noteStyle)
    {
        case 1:   // engraved: square corners, ink outline
            g.setColour (fill); g.fillRect (r);
            g.setColour (t.text.withAlpha (0.7f)); g.drawRect (r, 1.0f);
            break;
        case 2:   // neon: dark fill with a glowing edge
            g.setColour (on ? t.accent.withAlpha (0.25f) : t.bg2); g.fillRoundedRectangle (r, 3.0f);
            for (int i = 3; i >= 1; --i) { g.setColour ((on ? t.accent : t.edge).withAlpha (0.18f * (4 - i))); g.drawRoundedRectangle (r.reduced ((float) i - 1.0f), 3.0f, (float) i); }
            break;
        case 4:   // tape deck: chunky keys with a lit top edge
            g.setColour (fill.darker (0.35f)); g.fillRoundedRectangle (r, 2.0f);
            g.setColour (fill); g.fillRoundedRectangle (r.withTrimmedBottom (down ? 1.0f : 3.0f), 2.0f);
            g.setColour (Colours::white.withAlpha (0.12f)); g.drawLine (r.getX() + 2, r.getY() + 1.5f, r.getRight() - 2, r.getY() + 1.5f);
            break;
        default:
            g.setColour (fill); g.fillRoundedRectangle (r, 5.0f);
            g.setColour (t.edge); g.drawRoundedRectangle (r, 5.0f, 1.0f);
    }
}

void ScribeLookAndFeel::drawToggleButton (Graphics& g, ToggleButton& b, bool hi, bool)
{
    const auto& t = *th;
    auto r = b.getLocalBounds().toFloat();
    auto box = r.removeFromLeft (r.getHeight()).reduced (4.0f);
    g.setColour (b.getToggleState() ? t.accent : t.bg2);
    if (t.noteStyle == 1) g.fillRect (box); else g.fillRoundedRectangle (box, 3.0f);
    g.setColour (hi ? t.accent : t.edge);
    if (t.noteStyle == 1) g.drawRect (box, 1.0f); else g.drawRoundedRectangle (box, 3.0f, 1.0f);
    g.setColour (t.text);
    g.setFont (Font (FontOptions (13.0f)));
    g.drawText (b.getButtonText(), r.withTrimmedLeft (3.0f), Justification::centredLeft);
}

void ScribeLookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                          Slider::SliderStyle, Slider& s)
{
    const auto& t = *th;
    const float cy = (float) y + h * 0.5f;
    const Colour c = s.findColour (Slider::trackColourId);
    g.setColour (t.grid);
    g.fillRoundedRectangle ((float) x, cy - 2.0f, (float) w, 4.0f, 2.0f);
    g.setColour (c);
    g.fillRoundedRectangle ((float) x, cy - 2.0f, pos - (float) x, 4.0f, 2.0f);
    if (t.noteStyle == 2) { g.setColour (c.withAlpha (0.25f)); g.fillRoundedRectangle ((float) x, cy - 4.0f, pos - (float) x, 8.0f, 4.0f); }
    g.setColour (t.noteStyle == 1 ? t.text : c.brighter (0.3f));
    if (t.noteStyle == 4) g.fillRect (pos - 3.0f, cy - 7.0f, 6.0f, 14.0f);
    else g.fillEllipse (pos - 5.0f, cy - 5.0f, 10.0f, 10.0f);
}

void ScribeLookAndFeel::drawComboBox (Graphics& g, int w, int h, bool, int, int, int, int, ComboBox& box)
{
    const auto& t = *th;
    auto r = Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (t.bg2);
    if (t.noteStyle == 1) g.fillRect (r); else g.fillRoundedRectangle (r, 4.0f);
    g.setColour (box.hasKeyboardFocus (false) ? t.accent : t.edge);
    if (t.noteStyle == 1) g.drawRect (r, 1.0f); else g.drawRoundedRectangle (r, 4.0f, 1.0f);
    Path p;
    const float ax = (float) w - 12.0f, ay = h * 0.5f;
    p.addTriangle (ax - 4, ay - 2, ax + 4, ay - 2, ax, ay + 3);
    g.setColour (t.dim);
    g.fillPath (p);
}

void ScribeLookAndFeel::drawProgressBar (Graphics& g, ProgressBar&, int w, int h, double progress, const String& text)
{
    const auto& t = *th;
    auto r = Rectangle<float> (0, 0, (float) w, (float) h);
    g.setColour (t.bg2);
    g.fillRoundedRectangle (r, 3.0f);
    if (progress >= 0 && progress <= 1)
    {
        auto f = r.withWidth (r.getWidth() * (float) progress);
        if (t.noteStyle == 2) { g.setColour (t.accent.withAlpha (0.3f)); g.fillRoundedRectangle (f.expanded (0, 1), 3.0f); }
        g.setColour (t.accent.withAlpha (t.noteStyle == 1 ? 0.5f : 0.85f));
        g.fillRoundedRectangle (f, 3.0f);
    }
    g.setColour (t.edge);
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
    g.setColour (t.text);
    g.setFont (Font (FontOptions (12.5f)));
    g.drawText (text, r.reduced (6, 0), Justification::centred, true);
}
