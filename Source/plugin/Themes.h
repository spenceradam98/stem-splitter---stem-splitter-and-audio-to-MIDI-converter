// UI themes. Each theme changes rendering as well as colours: how notes are drawn (rounded,
// flat, glowing, outlined, tape labels), the grid, the background texture, and the Spectral theme
// paints a spectrogram of the selected stem behind the piano roll.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

struct Theme
{
    juce::String name;
    juce::Colour bg, bg2, panel, edge, text, dim, accent, accent2, grid, blackKey;
    int noteStyle = 0;        // 0 rounded, 1 flat score blocks, 2 neon glow, 3 outlined, 4 tape label
    bool spectrogram = false; // spectrogram behind the roll
    bool grain = false;       // paper / tape grain on panels
    bool scanlines = false;   // CRT lines on the roll
    juce::Colour stemColour (int kind) const;   // ss::StemKind index
};

const std::vector<Theme>& allThemes();
const Theme& themeByName (const juce::String& name);

class ScribeLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ScribeLookAndFeel();
    void setTheme (const Theme& t);
    const Theme& theme() const { return *th; }

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void drawProgressBar (juce::Graphics&, juce::ProgressBar&, int w, int h, double progress, const juce::String& text) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override;

    void paintGrain (juce::Graphics&, juce::Rectangle<int> area) const;

private:
    const Theme* th = nullptr;
};
