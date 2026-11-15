#include "PluginEditor.h"
#include "Theme.h"

#ifndef JucePlugin_VersionString
 #define JucePlugin_VersionString "dev"
#endif

static juce::Font ledFont (float size) { return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::bold)); }
static juce::Font bodyFont (float size) { return juce::Font (juce::FontOptions (size)); }

static juce::Colour sevColour (mst::Sev s)
{
    switch (s)
    {
        case mst::Sev::fix: return pal::signalHot;
        case mst::Sev::check: return pal::hazard;
        case mst::Sev::good: return juce::Colour (0xff9aa86a);   // oxidised brass-green: "fine"
        default: return pal::paintDim;
    }
}

// how far off a reading is, as a colour (|z| in standard deviations)
static juce::Colour zColour (float z)
{
    const float a = std::abs (z);
    if (a >= 2.5f) return pal::signalHot;
    if (a >= 1.5f) return pal::hazard;
    return pal::paint;
}

static juce::String mmss (double s)
{
    const int t = (int) s;
    return juce::String (t / 60).paddedLeft ('0', 2) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
}

//==============================================================================
MasterLookAndFeel::MasterLookAndFeel()
{
    setColour (juce::Label::textColourId, pal::paintDim);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1a1411));
    setColour (juce::PopupMenu::textColourId, pal::paint);
    setColour (juce::PopupMenu::headerTextColourId, pal::rustLight);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, pal::rust);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour (juce::TextButton::textColourOffId, pal::paint);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff1a1411));
    setColour (juce::TooltipWindow::textColourId, pal::paint);
    setColour (juce::TooltipWindow::outlineColourId, pal::rust);
    setColour (juce::ScrollBar::thumbColourId, pal::rust);
}

juce::Font MasterLookAndFeel::getTextButtonFont (juce::TextButton&, int h) { return tex::stencil (juce::jmin (15.0f, (float) h * 0.6f)).withExtraKerningFactor (0.06f); }
juce::Font MasterLookAndFeel::getPopupMenuFont() { return tex::plainBold (14.0f); }

void MasterLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool hi, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillRoundedRectangle (r.translated (1.0f, 1.5f), 3.0f);
    g.setGradientFill (juce::ColourGradient (down ? pal::rustDeep : juce::Colour (0xff3a2a20), 0, r.getY(),
                                             down ? juce::Colour (0xff2a120a) : juce::Colour (0xff17110e), 0, r.getBottom(), false));
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (hi ? pal::rustLight : juce::Colour (0xff6a3a20)); g.drawRoundedRectangle (r, 3.0f, 1.2f);
}

// toggles: an indicator lamp + stencilled label on a rusty plate
void MasterLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool hi, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    const auto lit = pal::signalHot;
    g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillRoundedRectangle (r.translated (1.0f, 1.5f), 3.0f);
    g.setGradientFill (juce::ColourGradient (on ? juce::Colour (0xff3a1a10) : juce::Colour (0xff2a221d), 0, r.getY(),
                                             juce::Colour (0xff120d0b), 0, r.getBottom(), false));
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (on ? lit.withAlpha (0.8f) : (hi ? pal::rustLight : juce::Colour (0xff5a3420))); g.drawRoundedRectangle (r, 3.0f, 1.2f);

    const float d = r.getHeight() * 0.42f;
    auto led = juce::Rectangle<float> (d, d).withCentre ({ r.getX() + r.getHeight() * 0.55f, r.getCentreY() });
    if (on)
    {
        juce::Path lp; lp.addEllipse (led);
        juce::DropShadow (lit.withAlpha (0.9f), 10, {}).drawForPath (g, lp);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffff0c0), led.getCentreX(), led.getCentreY(), lit, led.getX(), led.getY(), true));
    }
    else g.setColour (juce::Colour (0xff2a1c16));
    g.fillEllipse (led);
    g.setColour (juce::Colours::black.withAlpha (0.7f)); g.drawEllipse (led, 1.0f);

    g.setColour (on ? pal::paint : pal::paintDim);
    g.setFont (tex::stencil (15.0f).withExtraKerningFactor (0.1f));
    g.drawFittedText (b.getButtonText(), r.withTrimmedLeft (r.getHeight() * 1.0f).withTrimmedRight (6).toNearestInt(), juce::Justification::centredLeft, 1, 0.6f);
}

void MasterLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                                       int thumbStart, int thumbSize, bool over, bool down)
{
    g.setColour (juce::Colour (0xff120d0b)); g.fillRoundedRectangle ((float) x + 2, (float) y, (float) w - 4, (float) h, 3.0f);
    auto t = vertical ? juce::Rectangle<float> ((float) x + 2, (float) thumbStart, (float) w - 4, (float) thumbSize)
                      : juce::Rectangle<float> ((float) thumbStart, (float) y + 2, (float) thumbSize, (float) h - 4);
    g.setColour ((down ? pal::rustLight : over ? pal::rust.brighter (0.2f) : pal::rust).withAlpha (0.9f));
    g.fillRoundedRectangle (t.reduced (0.5f), 3.0f);
}

//==============================================================================
void GenreBar::setGenres (juce::StringArray i, juce::StringArray n)
{
    ids = std::move (i); names = std::move (n);
    repaint();
}

int GenreBar::segmentAt (juce::Point<int> p) const
{
    if (ids.isEmpty()) return -1;
    return juce::jlimit (0, ids.size() - 1, p.x * ids.size() / juce::jmax (1, getWidth()));
}

void GenreBar::mouseDown (const juce::MouseEvent& e)
{
    const int i = segmentAt (e.getPosition());
    if (i >= 0 && onPick) onPick (ids[i]);
}

void GenreBar::mouseMove (const juce::MouseEvent& e) { const int h = segmentAt (e.getPosition()); if (h != hover) { hover = h; repaint(); } }

void GenreBar::paint (juce::Graphics& g)
{
    const int n = ids.size();
    if (n == 0) return;
    const float sw = (float) getWidth() / (float) n;
    for (int i = 0; i < n; ++i)
    {
        auto r = juce::Rectangle<float> ((float) i * sw, 0, sw, (float) getHeight()).reduced (3.0f, 2.0f);
        const bool on = ids[i] == selected, hi = i == hover;
        const bool det = ids[i] == detected;
        g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillRoundedRectangle (r.translated (1.5f, 2.0f), 3.0f);
        if (on)
        {
            juce::Path pth; pth.addRoundedRectangle (r, 3.0f);
            juce::DropShadow (pal::signalHot.withAlpha (0.45f), 12, {}).drawForPath (g, pth);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff8a2a14), r.getCentreX(), r.getY(), juce::Colour (0xff3a0e06), r.getCentreX(), r.getBottom(), false));
        }
        else
            g.setGradientFill (juce::ColourGradient (hi ? juce::Colour (0xff43332a) : juce::Colour (0xff332822), r.getCentreX(), r.getY(),
                                                     juce::Colour (0xff15100d), r.getCentreX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (on ? pal::signalHot : (hi ? pal::rustLight : juce::Colour (0xff5a3420)));
        g.drawRoundedRectangle (r, 3.0f, on ? 1.6f : 1.1f);

        // corner lamp: red = selected, yellow = what the detector hears
        auto lampR = juce::Rectangle<float> (7, 7).withCentre ({ r.getX() + 10, r.getY() + 10 });
        if (det && ! on)
        {
            juce::Path lp; lp.addEllipse (lampR);
            juce::DropShadow (pal::hazard.withAlpha (0.9f), 8, {}).drawForPath (g, lp);
            g.setColour (pal::hazard);
        }
        else g.setColour (on ? juce::Colour (0xffffd0a0) : juce::Colour (0xff2a1c16));
        g.fillEllipse (lampR);
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.drawLine (r.getRight() - 18, r.getBottom() - 3, r.getRight() - 4, r.getBottom() - 10, 1.0f);

        auto tr = r.reduced (6, 2);
        const auto parts = juce::StringArray::fromTokens (names[i], "|", "");
        g.setColour (on ? juce::Colour (0xfffff0d8) : pal::paint.withAlpha (0.85f));
        g.setFont (tex::stencil (19.0f).withExtraKerningFactor (0.06f));
        g.drawFittedText (parts[0], tr.removeFromTop (tr.getHeight() * 0.62f).toNearestInt(), juce::Justification::centredBottom, 1, 0.6f);
        g.setColour (det && ! on ? pal::hazard : (on ? pal::paint : pal::paintDim.withAlpha (0.7f)));
        g.setFont (tex::plainBold (9.0f).withExtraKerningFactor (0.1f));
        g.drawFittedText (det && ids[i] != "auto" ? juce::String ("SOUNDS LIKE THIS") : parts[1], tr.toNearestInt(), juce::Justification::centredTop, 1, 0.6f);
    }
}

//==============================================================================
void SoundsLike::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (pal::screen); g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colour (0xff3a2418)); g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    auto in = r.reduced (10, 6);
    if (! ready)
    {
        g.setColour (pal::paintDim.withAlpha (0.6f)); g.setFont (ledFont (13.0f));
        g.drawText ("WAITING FOR AUDIO...", in, juce::Justification::centredLeft);
        return;
    }
    auto top = in.removeFromTop (in.getHeight() * 0.58f);
    g.setColour (pal::hazard); g.setFont (tex::stencil (24.0f).withExtraKerningFactor (0.06f));
    g.drawFittedText (first.toUpperCase(), top.removeFromLeft (top.getWidth() * 0.62f).toNearestInt(), juce::Justification::centredLeft, 1, 0.6f);
    // confidence bar
    auto bar = top.withSizeKeepingCentre (top.getWidth(), 10.0f);
    g.setColour (juce::Colour (0xff1e1612)); g.fillRoundedRectangle (bar, 2.0f);
    g.setColour (pal::hazard.withAlpha (0.85f)); g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.03f, 1.0f, p1)), 2.0f);
    g.setColour (pal::paint); g.setFont (ledFont (11.0f));
    g.drawText (juce::String (juce::roundToInt (p1 * 100.0f)) + "%", top.withTrimmedBottom (top.getHeight() * 0.55f).toNearestInt(), juce::Justification::topRight);
    g.setColour (pal::paintDim); g.setFont (ledFont (11.5f));
    juce::String line = second.isNotEmpty() ? "NEXT CLOSEST: " + second.toUpperCase() + " " + juce::String (juce::roundToInt (p2 * 100.0f)) + "%" : juce::String();
    if (locked) line = "COMPARING TO " + lockedName.toUpperCase() + " (YOUR PICK)";
    g.drawFittedText (line, in.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
}

//==============================================================================
static constexpr float kChartRange = 9.0f;   // +-dB shown

void TonalChart::buildGrid()
{
    grid = juce::Image (juce::Image::ARGB, getWidth(), getHeight(), true);
    juce::Graphics g (grid);
    auto r = getLocalBounds().toFloat();
    g.setColour (pal::screen); g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colour (0xff3a2418)); g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
}

void TonalChart::paint (juce::Graphics& g)
{
    if (! grid.isValid() || grid.getWidth() != getWidth()) buildGrid();
    g.drawImageAt (grid, 0, 0);
    auto r = getLocalBounds().toFloat().reduced (8.0f);
    auto labels = r.removeFromBottom (30.0f);
    auto axis = r.removeFromLeft (30.0f);
    const float cw = r.getWidth() / (float) mst::kNumBands;
    auto yFor = [&] (float d) { return r.getCentreY() - juce::jlimit (-kChartRange, kChartRange, d) / kChartRange * r.getHeight() * 0.5f; };

    // dB grid
    g.setFont (ledFont (10.0f));
    for (float d : { -6.0f, -3.0f, 0.0f, 3.0f, 6.0f })
    {
        const float y = yFor (d);
        g.setColour (d == 0.0f ? pal::paint.withAlpha (0.35f) : juce::Colour (0x22d6c7a6));
        g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
        g.setColour (pal::paintDim.withAlpha (0.7f));
        g.drawText (d == 0 ? juce::String ("0") : (d > 0 ? "+" : "") + juce::String ((int) d), axis.withY (y - 7).withHeight (14).toNearestInt(), juce::Justification::centredRight);
    }
    g.setColour (pal::paintDim.withAlpha (0.55f)); g.setFont (tex::plainBold (8.5f));
    g.drawText ("MORE", axis.withHeight (12).toNearestInt(), juce::Justification::centredLeft);
    g.drawText ("LESS", axis.withY (r.getBottom() - 12).withHeight (12).toNearestInt(), juce::Justification::centredLeft);

    // genre ranges: +-1 sd (dark rust) and +-2 sd (faint)
    for (int b = 0; b < mst::kNumBands; ++b)
    {
        const float x = r.getX() + cw * (float) b;
        auto col = juce::Rectangle<float> (x, r.getY(), cw, r.getHeight()).reduced (cw * 0.16f, 0);
        const float s = ready || sd[(size_t) b] > 0 ? sd[(size_t) b] : 2.5f;
        g.setColour (pal::rust.withAlpha (0.14f));
        g.fillRect (col.withTop (yFor (2 * s)).withBottom (yFor (-2 * s)));
        g.setColour (pal::rust.withAlpha (0.38f));
        g.fillRect (col.withTop (yFor (s)).withBottom (yFor (-s)));
        g.setColour (pal::rustLight.withAlpha (0.6f));
        g.drawHorizontalLine ((int) yFor (0), col.getX(), col.getRight());

        auto lab = labels.withX (x).withWidth (cw);
        g.setColour (pal::paint); g.setFont (tex::stencil (11.0f).withExtraKerningFactor (0.03f));
        g.drawFittedText (mst::bandDefs()[(size_t) b].label, lab.removeFromTop (15).toNearestInt(), juce::Justification::centred, 1, 0.6f);
        g.setColour (pal::paintDim.withAlpha (0.75f)); g.setFont (tex::plainBold (8.5f));
        g.drawFittedText (mst::bandDefs()[(size_t) b].range, lab.toNearestInt(), juce::Justification::centredTop, 1, 0.6f);
    }

    if (! ready)
    {
        g.setColour (pal::paintDim.withAlpha (0.7f)); g.setFont (ledFont (13.0f));
        g.drawText ("SHADED = WHERE " + genre.toUpperCase() + " RECORDS SIT.  PLAY YOUR MIX.", r.toNearestInt(), juce::Justification::centred);
        return;
    }

    // your mix: a glowing trace across the bands
    juce::Path trace;
    std::array<juce::Point<float>, mst::kNumBands> pts;
    for (int b = 0; b < mst::kNumBands; ++b)
    {
        pts[(size_t) b] = { r.getX() + cw * ((float) b + 0.5f), yFor (dev[(size_t) b]) };
        if (b == 0) trace.startNewSubPath (pts[0]); else trace.lineTo (pts[(size_t) b]);
    }
    g.setColour (pal::signalHot.withAlpha (0.18f)); g.strokePath (trace, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (pal::paint.withAlpha (0.8f)); g.strokePath (trace, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    for (int b = 0; b < mst::kNumBands; ++b)
    {
        const float z = dev[(size_t) b] / juce::jmax (0.1f, sd[(size_t) b]);
        const auto c = zColour (z);
        const auto p = pts[(size_t) b];
        juce::Path dot; dot.addEllipse (p.x - 5, p.y - 5, 10, 10);
        if (std::abs (z) >= 1.5f) juce::DropShadow (c.withAlpha (0.9f), 10, {}).drawForPath (g, dot);
        g.setColour (c); g.fillPath (dot);
        g.setColour (juce::Colours::black.withAlpha (0.7f)); g.strokePath (dot, juce::PathStrokeType (1.0f));
        if (std::abs (dev[(size_t) b]) > kChartRange)   // off the chart: arrow
        {
            juce::Path a; const float dir = dev[(size_t) b] > 0 ? -1.0f : 1.0f;
            a.addTriangle (p.x - 5, p.y + dir * 8, p.x + 5, p.y + dir * 8, p.x, p.y + dir * 15);
            g.fillPath (a);
        }
        g.setColour (c.withAlpha (std::abs (z) >= 1.5f ? 1.0f : 0.75f)); g.setFont (ledFont (10.5f));
        const float ty = juce::jlimit (r.getY() - 6.0f, r.getBottom() - 6.0f, dev[(size_t) b] >= 0 ? p.y - 20 : p.y + 8)
                         + (std::abs (dev[(size_t) b]) > kChartRange ? (dev[(size_t) b] > 0 ? 22.0f : -24.0f) : 0.0f);
        g.drawText ((dev[(size_t) b] >= 0 ? "+" : "") + juce::String (dev[(size_t) b], 1), juce::Rectangle<float> (p.x - cw * 0.5f, ty, cw, 12).toNearestInt(), juce::Justification::centred);
    }
}

//==============================================================================
void Vitals::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (pal::screen); g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colour (0xff3a2418)); g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    r = r.reduced (10, 6);
    const float rh = r.getHeight() / (float) std::max<size_t> (1, rows.size());
    for (auto& row : rows)
    {
        auto line = r.removeFromTop (rh);
        auto lab = line.removeFromLeft (128);
        auto val = line.removeFromRight (104);
        g.setColour (pal::paint); g.setFont (tex::stencil (13.0f).withExtraKerningFactor (0.08f));
        g.drawFittedText (row.label, lab.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);

        auto track = line.reduced (6, 0).withSizeKeepingCentre (line.getWidth() - 12, 8);
        auto xFor = [&] (float v) { return track.getX() + juce::jlimit (0.0f, 1.0f, (v - row.lo) / (row.hi - row.lo)) * track.getWidth(); };
        g.setColour (juce::Colour (0xff1e1612)); g.fillRoundedRectangle (track, 3.0f);
        if (row.hasRange)
        {
            const float a = xFor (row.mean - 2 * row.sd), b = xFor (row.mean + 2 * row.sd);
            const float c = xFor (row.mean - row.sd), d = xFor (row.mean + row.sd);
            g.setColour (pal::rust.withAlpha (0.25f)); g.fillRect (juce::Rectangle<float> (a, track.getY(), b - a, track.getHeight()));
            g.setColour (pal::rust.withAlpha (0.65f)); g.fillRect (juce::Rectangle<float> (c, track.getY(), d - c, track.getHeight()));
            g.setColour (pal::rustLight); g.fillRect (xFor (row.mean) - 0.75f, track.getY() - 2, 1.5f, track.getHeight() + 4);
        }
        else
        {   // a hard limit line (true peak -1 dBTP)
            const float x = xFor (row.mean);
            g.setColour (pal::signal.withAlpha (0.35f)); g.fillRect (juce::Rectangle<float> (x, track.getY(), track.getRight() - x, track.getHeight()));
            g.setColour (pal::signalHot); g.fillRect (x - 0.75f, track.getY() - 3, 1.5f, track.getHeight() + 6);
        }
        // scale ticks
        g.setColour (pal::paintDim.withAlpha (0.35f));
        for (int i = 0; i <= 8; ++i) { const float x = track.getX() + track.getWidth() * (float) i / 8.0f; g.drawVerticalLine ((int) x, track.getBottom() + 2, track.getBottom() + 5); }

        if (row.ready)
        {
            const float x = xFor (row.v);
            const float z = row.hasRange ? (row.v - row.mean) / juce::jmax (1e-3f, row.sd) : (row.warn ? 3.0f : 0.0f);
            const auto c = zColour (z);
            juce::Path needle;
            needle.addTriangle (x - 6, track.getY() - 7, x + 6, track.getY() - 7, x, track.getY() + 3);
            needle.addRectangle (x - 1.0f, track.getY(), 2.0f, track.getHeight() + 3);
            g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillPath (needle, juce::AffineTransform::translation (1, 1.5f));
            g.setColour (c); g.fillPath (needle);
            g.setFont (ledFont (13.0f));
            g.drawText (row.value, val.toNearestInt(), juce::Justification::centredRight);
        }
        else
        {
            g.setColour (pal::paintDim.withAlpha (0.4f)); g.setFont (ledFont (13.0f));
            g.drawText ("--", val.toNearestInt(), juce::Justification::centredRight);
        }
    }
}

//==============================================================================
void ScorePanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (pal::screen); g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colour (0xff3a2418)); g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    r = r.reduced (12, 6);
    auto num = r.removeFromLeft (120);
    const auto col = ! ready ? pal::paintDim.withAlpha (0.4f) : (score >= 80 ? juce::Colour (0xff9aa86a) : score >= 60 ? pal::hazard : pal::signalHot);
    g.setColour (col); g.setFont (tex::stencil (54.0f));
    g.drawText (ready ? juce::String (score) : juce::String ("--"), num.removeFromLeft (80).toNearestInt(), juce::Justification::centredRight);
    g.setColour (pal::paintDim); g.setFont (tex::stencil (16.0f));
    g.drawText ("/100", num.withTrimmedTop (num.getHeight() * 0.45f).toNearestInt(), juce::Justification::centredLeft);

    auto top = r.removeFromTop (r.getHeight() * 0.5f);
    g.setColour (pal::paint); g.setFont (tex::stencil (17.0f).withExtraKerningFactor (0.08f));
    g.drawFittedText ("MIX SCORE VS " + genre.toUpperCase(), top.toNearestInt(), juce::Justification::bottomLeft, 1, 0.6f);
    g.setFont (ledFont (12.0f));
    float x = r.getX();
    auto tally = [&] (int n, const juce::String& what, juce::Colour c)
    {
        g.setColour (c); g.fillEllipse (x, r.getCentreY() - 4, 8, 8);
        g.setColour (pal::paint);
        const auto t = juce::String (n) + " " + what;
        g.drawText (t, juce::Rectangle<float> (x + 12, r.getY(), 80, r.getHeight()).toNearestInt(), juce::Justification::centredLeft);
        x += 24 + (float) t.length() * 7.5f;
    };
    if (ready)
    {
        tally (fixes, "FIX", sevColour (mst::Sev::fix));
        tally (checks, "CHECK", sevColour (mst::Sev::check));
        tally (goods, "GOOD", sevColour (mst::Sev::good));
    }
    else
    {
        g.setColour (pal::paintDim.withAlpha (0.7f));
        g.drawText ("NO ANALYSIS YET", r.toNearestInt(), juce::Justification::centredLeft);
    }
    if (saved)
    {
        g.setColour (pal::hazard.withAlpha (0.9f)); g.setFont (tex::plainBold (8.5f));
        g.drawText ("SAVED WITH PROJECT", getLocalBounds().reduced (8, 5).toFloat().toNearestInt(), juce::Justification::topRight);
    }
}

//==============================================================================
void VerdictList::setItems (const std::vector<mst::Suggestion>& items, int width)
{
    laid.clear();
    const float textW = (float) width - 34.0f;
    int y = 4;
    for (auto& s : items)
    {
        Laid l; l.s = s;
        juce::AttributedString body;
        body.append (s.body, bodyFont (12.5f), pal::paint.withAlpha (0.92f));
        body.setWordWrap (juce::AttributedString::byWord);
        l.body.createLayout (body, textW);
        l.bodyH = (int) std::ceil (l.body.getHeight());
        if (s.plugin.isNotEmpty())
        {
            juce::AttributedString tip;
            tip.append (s.plugin + "  ", tex::stencil (13.0f).withExtraKerningFactor (0.06f), pal::signalHot);
            tip.append (s.tip, tex::plainBold (12.0f), pal::rustLight.brighter (0.25f));
            tip.setWordWrap (juce::AttributedString::byWord);
            l.tip.createLayout (tip, textW - 12.0f);
            l.tipH = (int) std::ceil (l.tip.getHeight()) + 8;
        }
        l.y = y;
        l.h = 8 + 18 + l.bodyH + (l.tipH > 0 ? l.tipH + 4 : 0) + 8;
        y += l.h;
        laid.push_back (std::move (l));
    }
    setSize (width, y + 4);
    repaint();
}

void VerdictList::paint (juce::Graphics& g)
{
    for (auto& l : laid)
    {
        auto r = juce::Rectangle<float> (0, (float) l.y, (float) getWidth(), (float) l.h);
        const auto c = sevColour (l.s.sev);
        // lamp
        auto lamp = juce::Rectangle<float> (10, 10).withCentre ({ 11, r.getY() + 17 });
        juce::Path lp; lp.addEllipse (lamp);
        if (l.s.sev == mst::Sev::fix || l.s.sev == mst::Sev::check) juce::DropShadow (c.withAlpha (0.8f), 8, {}).drawForPath (g, lp);
        g.setColour (c); g.fillPath (lp);
        g.setColour (juce::Colours::black.withAlpha (0.6f)); g.strokePath (lp, juce::PathStrokeType (1.0f));

        auto in = r.withTrimmedLeft (24).withTrimmedRight (6).withTrimmedTop (8);
        auto head = in.removeFromTop (18);
        g.setColour (pal::paintDim.withAlpha (0.7f)); g.setFont (tex::plainBold (8.5f).withExtraKerningFactor (0.08f));
        g.drawText (l.s.area, head.toNearestInt(), juce::Justification::centredRight);
        g.setColour (l.s.sev == mst::Sev::good || l.s.sev == mst::Sev::info ? pal::paint : c);
        g.setFont (tex::stencil (15.0f).withExtraKerningFactor (0.06f));
        g.drawFittedText (l.s.title, head.withTrimmedRight (head.getWidth() * 0.3f).toNearestInt(), juce::Justification::centredLeft, 1, 0.65f);

        l.body.draw (g, in.removeFromTop ((float) l.bodyH));
        if (l.tipH > 0)
        {
            in.removeFromTop (4);
            auto t = in.removeFromTop ((float) l.tipH);
            g.setColour (juce::Colour (0xff24130c)); g.fillRoundedRectangle (t, 3.0f);
            g.setColour (pal::rust.withAlpha (0.7f)); g.fillRect (t.withWidth (3.0f));
            l.tip.draw (g, t.reduced (8, 4));
        }
        g.setColour (juce::Colour (0x30d6c7a6));
        g.drawHorizontalLine ((int) r.getBottom() - 1, 6, (float) getWidth() - 6);
    }
}

//==============================================================================
void PilotLamp::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (4.0f);
    const auto c = r.getCentre();
    const float R = std::min (r.getWidth(), r.getHeight()) * 0.5f;
    const float lit = juce::jlimit (0.0f, 1.0f, 0.2f + heat * 0.8f);

    g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillEllipse (r.translated (2.0f, 3.0f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff7a6a5a), c.x - R * 0.6f, c.y - R * 0.7f, juce::Colour (0xff15100d), c.x + R, c.y + R, true));
    g.fillEllipse (r);
    g.setColour (juce::Colour (0xff0c0807));
    for (int i = 0; i < 40; ++i)
    {
        const float a = (float) i / 40.0f * juce::MathConstants<float>::twoPi;
        g.drawLine (c.x + std::cos (a) * R * 0.9f, c.y + std::sin (a) * R * 0.9f, c.x + std::cos (a) * R, c.y + std::sin (a) * R, 1.0f);
    }
    g.setColour (pal::rust.withAlpha (0.6f)); g.drawEllipse (r.reduced (1.0f), 1.5f);

    const float jr = R * 0.66f;
    const juce::Rectangle<float> jewel (c.x - jr, c.y - jr, jr * 2, jr * 2);
    juce::Path jp; jp.addEllipse (jewel);
    if (lit > 0.4f) juce::DropShadow (juce::Colour (0xffff3020).withAlpha (0.9f * (lit - 0.35f)), (int) (R * 0.9f), {}).drawForPath (g, jp);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffff9a70).interpolatedWith (juce::Colour (0xff5a0a06), 1.0f - lit), c.x - jr * 0.25f, c.y - jr * 0.3f,
                                             juce::Colour (0xff2a0403), c.x + jr, c.y + jr, true));
    g.fillPath (jp);
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (jp);
    g.setColour (juce::Colours::black.withAlpha (0.28f));
    for (int i = 0; i < 8; ++i)
    {
        const float a = (float) i / 8.0f * juce::MathConstants<float>::twoPi + 0.2f;
        g.drawLine (c.x + std::cos (a) * jr * 0.38f, c.y + std::sin (a) * jr * 0.38f, c.x + std::cos (a) * jr, c.y + std::sin (a) * jr, 1.0f);
    }
    juce::Path oct; oct.addPolygon (c, 8, jr * 0.4f, 0.2f);
    g.strokePath (oct, juce::PathStrokeType (1.0f));
    g.setColour (juce::Colours::white.withAlpha (0.35f + 0.3f * lit));
    g.fillEllipse (c.x - jr * 0.55f, c.y - jr * 0.6f, jr * 0.45f, jr * 0.3f);
}

//==============================================================================
MasterDialog::MasterDialog (const juce::String& t, const juce::String& m, juce::StringArray names, std::function<void (int)> cb)
    : title (t), message (m), onClose (std::move (cb))
{
    setWantsKeyboardFocus (true);
    for (int i = 0; i < names.size(); ++i)
    {
        auto* b = buttons.add (new juce::TextButton (names[i]));
        b->onClick = [this, i] { finish (i); };
        addAndMakeVisible (b);
    }
}

void MasterDialog::parentHierarchyChanged()
{
    if (isShowing())
        juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<MasterDialog> (this)] { if (sp != nullptr) sp->grabKeyboardFocus(); });
}

void MasterDialog::finish (int index)
{
    if (onClose == nullptr) return;
    auto cb = std::move (onClose);
    onClose = nullptr;
    cb (index);
}

bool MasterDialog::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::returnKey) { finish (0); return true; }
    if (k == juce::KeyPress::escapeKey) { finish ((int) buttons.size() - 1); return true; }
    return true;
}

void MasterDialog::resized()
{
    panel = getLocalBounds().withSizeKeepingCentre (460, 200);
    auto r = panel.reduced (22);
    auto row = r.removeFromBottom (28);
    for (int i = (int) buttons.size() - 1; i >= 0; --i) { buttons[i]->setBounds (row.removeFromRight (100)); row.removeFromRight (8); }
}

void MasterDialog::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.72f));
    auto p = panel.toFloat();
    g.setColour (juce::Colour (0xff1a1411)); g.fillRect (p);
    g.setGradientFill (juce::ColourGradient (pal::rustDeep.withAlpha (0.5f), p.getX(), p.getY(), juce::Colours::transparentBlack, p.getX(), p.getY() + p.getHeight() * 0.6f, false));
    g.fillRect (p);
    g.setColour (pal::rust); g.drawRect (p, 2.0f);
    g.setColour (juce::Colours::black.withAlpha (0.6f)); g.drawRect (p.reduced (2.0f), 1.0f);
    for (auto pt : { juce::Point<float> (p.getX() + 8, p.getY() + 8), { p.getRight() - 8, p.getY() + 8 }, { p.getX() + 8, p.getBottom() - 8 }, { p.getRight() - 8, p.getBottom() - 8 } })
    {
        g.setColour (juce::Colour (0xff6a5a4c)); g.fillEllipse (pt.x - 3, pt.y - 3, 6, 6);
        g.setColour (pal::rust.withAlpha (0.6f)); g.drawEllipse (pt.x - 3, pt.y - 3, 6, 6, 1.0f);
    }
    auto r = panel.reduced (22);
    g.setColour (pal::signalHot); g.setFont (tex::stencil (19.0f).withExtraKerningFactor (0.1f));
    g.drawText (title, r.removeFromTop (30), juce::Justification::centredLeft);
    g.setColour (pal::paint); g.setFont (tex::plainBold (13.0f));
    g.drawFittedText (message, r.removeFromTop (100), juce::Justification::topLeft, 6);
}

//==============================================================================
MasterEditor::MasterEditor (MasterProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (lamp);
    lamp.setInterceptsMouseClicks (false, false);

    listenButton.setTooltip ("While LISTEN is on, The Master analyses everything that plays through it. Turn it off to freeze the report (you can automate it to analyse just one section).");
    addAndMakeVisible (listenButton);
    listenAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "listen", listenButton);
    resetButton.setTooltip ("Forget everything heard so far and start a fresh analysis.");
    resetButton.onClick = [this] { proc.resetAnalysis(); lastSnap = -1; };
    profilesButton.setTooltip ("Genre profiles: import an updated profile pack, open the profiles folder, or go back to the factory profiles. (Hold SHIFT for the admin tools.)");
    profilesButton.onClick = [this] { showProfilesMenu(); };
    copyButton.setTooltip ("Copy the whole report as text, to paste into notes or send to someone.");
    copyButton.onClick = [this] { copyReport(); };
    for (auto* b : { &resetButton, &profilesButton, &copyButton }) addAndMakeVisible (*b);

    genreBar.setTooltip ("Which genre to compare your mix with. AUTO picks the closest match for you; the yellow lamp shows what your mix sounds like.");
    genreBar.onPick = [this] (const juce::String& id) { proc.setGenreChoice (id); genreBar.setSelected (id); lastSnap = -1; };
    addAndMakeVisible (genreBar);
    for (auto* c : std::initializer_list<juce::Component*> { &soundsLike, &chart, &vitals, &score })
    {
        addAndMakeVisible (*c);
        c->setInterceptsMouseClicks (false, false);
    }
    viewport.setViewedComponent (&verdict, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    refreshGenres();
    setSize (1100, 700);
    refreshNow();
    startTimerHz (15);
}

MasterEditor::~MasterEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void MasterEditor::resized()
{
    plates.clear();
    lamp.setBounds (16, 12, 88, 88);
    logoArea  = { 112, 6, 410, 76 };
    badgeArea = { 548, 14, 260, 86 };
    crestArea = { 556, 19, 76, 76 };
    controlArea = { 820, 14, 264, 86 };
    listenButton.setBounds (830, 36, 98, 30);
    resetButton.setBounds (933, 36, 56, 30);
    profilesButton.setBounds (994, 36, 82, 30);
    statusArea = { 832, 70, 244, 22 };
    stripeArea = { 0, 110, getWidth(), 18 };

    plates.push_back ({ "REFERENCE GENRE", { 16, 138, 712, 86 }, true });
    genreBar.setBounds (26, 162, 692, 54);
    plates.push_back ({ "SOUNDS LIKE", { 738, 138, 346, 86 } });
    soundsLike.setBounds (750, 162, 322, 54);

    plates.push_back ({ "TONAL BALANCE  //  YOUR MIX VS THE GENRE", { 16, 234, 640, 262 } });
    chart.setBounds (28, 258, 616, 228);
    plates.push_back ({ "VITALS", { 16, 506, 640, 178 } });
    vitals.setBounds (28, 530, 616, 144);
    plates.push_back ({ "THE VERDICT", { 666, 234, 418, 450 } });
    copyButton.setBounds (1022, 238, 54, 20);
    score.setBounds (678, 258, 394, 74);
    viewport.setBounds (678, 340, 394, 334);
    verdict.setItems (report.items, viewport.getWidth() - 12);

    buildBackdrop();
}

//==============================================================================
void MasterEditor::refreshGenres()
{
    juce::StringArray ids { "auto" }, names { "AUTO|DETECTS IT" };
    for (auto& g : proc.getLibrary().genres())
    {
        ids.add (g.id);
        names.add (g.name.toUpperCase() + "|" + (g.tracks > 0 ? juce::String (g.tracks) + " REFERENCE SONGS" : juce::String ("FACTORY")));
    }
    genreBar.setGenres (ids, names);
    auto choice = proc.getGenreChoice();
    if (choice != "auto" && proc.getLibrary().indexOf (choice) < 0) choice = "auto";
    genreBar.setSelected (choice);
    lastLib = proc.getLibrary().version.load();
}

void MasterEditor::updateReport()
{
    bool saved = false;
    const auto fx = proc.getDisplayFeatures (saved);
    const auto target = proc.resolveTarget (fx);
    const auto& genres = proc.getLibrary().genres();
    if (target.index < 0 || target.index >= (int) genres.size()) return;
    const auto& gp = genres[(size_t) target.index];
    report = mst::advise (fx, gp);

    // detector
    soundsLike.ready = fx.valid && ! target.guesses.empty();
    soundsLike.locked = ! target.automatic;
    soundsLike.lockedName = gp.name;
    if (soundsLike.ready)
    {
        soundsLike.first = genres[(size_t) target.guesses[0].index].name;
        soundsLike.p1 = target.guesses[0].probability;
        soundsLike.second = target.guesses.size() > 1 ? genres[(size_t) target.guesses[1].index].name : juce::String();
        soundsLike.p2 = target.guesses.size() > 1 ? target.guesses[1].probability : 0.0f;
        genreBar.setDetected (genres[(size_t) target.guesses[0].index].id);
    }
    else genreBar.setDetected ({});
    soundsLike.repaint();

    // chart
    chart.ready = fx.valid;
    chart.genre = gp.name;
    for (int b = 0; b < mst::kNumBands; ++b)
    {
        const auto st = gp.stat (mst::fBand0 + b);
        chart.dev[(size_t) b] = fx.band[(size_t) b] - st.mean;
        chart.sd[(size_t) b] = st.sd;
    }
    chart.repaint();

    // vitals
    auto row = [&] (const juce::String& label, int f, float lo, float hi, float v, const juce::String& value)
    {
        const auto st = gp.stat (f);
        return Vitals::Row { label, value, lo, hi, v, st.mean, st.sd, true, fx.valid, false };
    };
    vitals.rows.clear();
    vitals.rows.push_back (row ("LOUDNESS", mst::fLufs, -20, -4, fx.lufs, juce::String (fx.lufs, 1) + " LUFS"));
    vitals.rows.push_back ({ "TRUE PEAK", juce::String (fx.truePeak, 1) + " dBTP", -10, 2, fx.truePeak, -1.0f, 1.0f, false, fx.valid, fx.truePeak > -1.0f });
    vitals.rows.push_back (row ("PUNCH (PLR)", mst::fPlr, 3, 17, fx.plr, juce::String (fx.plr, 1) + " dB"));
    vitals.rows.push_back (row ("LOUDNESS RANGE", mst::fLra, 0, 16, fx.lra, juce::String (fx.lra, 1) + " LU"));
    vitals.rows.push_back (row ("STEREO WIDTH", mst::fWidth, -24, 0, fx.width, "S " + juce::String (fx.width, 1) + " dB"));
    vitals.rows.push_back (row ("MONO LOW END", mst::fLowCorr, 0, 1, fx.lowCorrelation, juce::String (fx.lowCorrelation, 2)));
    vitals.repaint();

    // verdict
    score.ready = report.ready;
    score.score = report.score; score.fixes = report.fixes; score.checks = report.checks; score.goods = report.goods;
    score.genre = gp.name + (target.automatic ? " (AUTO)" : "");
    score.saved = saved;
    score.repaint();
    const auto pos = viewport.getViewPosition();
    verdict.setItems (report.items, viewport.getWidth() - 12);
    viewport.setViewPosition (pos);

    // status line
    const auto t = mmss (fx.seconds);
    if (saved) statusText = "SAVED REPORT (" + t + ")  //  PLAY TO START A NEW ONE";
    else if (proc.getWorker().getSampleRate() <= 0) statusText = "WAITING FOR THE HOST TO START AUDIO";
    else statusText = juce::String (proc.isListening() ? "LISTENING" : "PAUSED") + "  //  " + t + " ANALYSED";
    repaint (statusArea);
}

void MasterEditor::timerCallback()
{
    if (proc.getLibrary().version.load() != lastLib) { refreshGenres(); lastSnap = -1; }
    const int snap = proc.getWorker().getSnapshotVersion();
    const auto choice = proc.getGenreChoice();
    if (snap != lastSnap || choice != lastGenre)
    {
        lastSnap = snap; lastGenre = choice;
        genreBar.setSelected (proc.getLibrary().indexOf (choice) >= 0 ? choice : juce::String ("auto"));
        updateReport();
    }

    // lamp glows with the music while listening
    const float pk = proc.inPeak.exchange (0.0f);
    const float target = proc.isListening() ? juce::jlimit (0.0f, 1.0f, 0.25f + pk * 0.9f) : 0.0f;
    activity += (target - activity) * 0.3f;
    lamp.heat = activity;
    lamp.repaint();

    auto& rng = juce::Random::getSystemRandom();
    const float before = flicker;
    flicker = rng.nextFloat() < 0.012f ? rng.nextFloat() : flicker * 0.5f;
    if (std::abs (flicker - before) > 0.01f) repaint (logoArea.expanded (6, 8));
}

//==============================================================================
void MasterEditor::showMessage (const juce::String& title, const juce::String& text)
{
    juce::Component::SafePointer<MasterEditor> safe (this);
    dialog = std::make_unique<MasterDialog> (title, text, juce::StringArray { "OK" }, [safe] (int)
    {
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->dialog.reset(); });
    });
    addAndMakeVisible (*dialog);
    dialog->setBounds (getLocalBounds());
}

void MasterEditor::copyReport()
{
    bool saved = false;
    const auto fx = proc.getDisplayFeatures (saved);
    juce::String s;
    s << "THE MASTER - mix report (Castle Danger Music Company)\n";
    s << "Compared with: " << report.genreName << "    Score: " << (report.ready ? juce::String (report.score) : juce::String ("--")) << "/100\n";
    if (fx.valid)
        s << "Loudness " << juce::String (fx.lufs, 1) << " LUFS, true peak " << juce::String (fx.truePeak, 1) << " dBTP, PLR " << juce::String (fx.plr, 1)
          << " dB, LRA " << juce::String (fx.lra, 1) << " LU, width " << juce::String (fx.width, 1) << " dB, low-end correlation " << juce::String (fx.lowCorrelation, 2)
          << ", " << mmss (fx.seconds) << " analysed\n";
    s << "\n";
    for (auto& it : report.items)
    {
        const char* tag = it.sev == mst::Sev::fix ? "[FIX]" : it.sev == mst::Sev::check ? "[CHECK]" : it.sev == mst::Sev::good ? "[GOOD]" : "[NOTE]";
        s << tag << " " << it.title << "\n" << it.body << "\n";
        if (it.plugin.isNotEmpty()) s << "  -> " << it.plugin << ": " << it.tip << "\n";
        s << "\n";
    }
    juce::SystemClipboard::copyTextToClipboard (s);
    showMessage ("REPORT COPIED", "The full report is on your clipboard. Paste it into your notes, an email or a message.");
}

void MasterEditor::showProfilesMenu()
{
    auto& lib = proc.getLibrary();
    const bool admin = juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
    bool saved = false;
    const auto fx = proc.getDisplayFeatures (saved);

    juce::PopupMenu m;
    m.addSectionHeader (lib.describe());
    if (lib.getUserError().isNotEmpty()) m.addItem (99, "Your profile pack has a problem: " + lib.getUserError(), false);
    m.addItem (1, "Import profile pack...");
    m.addItem (2, "Open profiles folder");
    m.addItem (3, "Restore factory profiles", lib.usingUserPack());
    juce::PopupMenu refs;
    if (admin)
    {
        m.addSeparator();
        m.addSectionHeader ("ADMIN");
        for (int i = 0; i < (int) lib.genres().size(); ++i)
        {
            const auto& g = lib.genres()[(size_t) i];
            refs.addItem (100 + i, g.name + "  (" + (g.tracks > 0 ? juce::String (g.tracks) + " songs" : juce::String ("factory seed")) + ")", fx.valid);
        }
        m.addSubMenu ("Add this analysis as a reference song for", refs, fx.valid);
        m.addItem (4, "Export profile pack...");
        m.addItem (5, "Copy analysis numbers (JSON)", fx.valid);
    }

    juce::Component::SafePointer<MasterEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&profilesButton).withParentComponent (this),
        [safe, fx] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& self = *safe;
            auto& lib = self.proc.getLibrary();
            if (r == 1)
            {
                self.chooser = std::make_unique<juce::FileChooser> ("Import a The Master profile pack", juce::File::getSpecialLocation (juce::File::userHomeDirectory), "*.json");
                self.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
                {
                    if (safe == nullptr || fc.getResult() == juce::File()) return;
                    const auto err = safe->proc.getLibrary().importPack (fc.getResult());
                    if (err.isNotEmpty()) safe->showMessage ("IMPORT FAILED", "That file couldn't be used: " + err + ".");
                    else safe->showMessage ("PROFILES UPDATED", "Now using " + safe->proc.getLibrary().describe() + ". Every copy of The Master on this computer uses it.");
                });
            }
            else if (r == 2)
            {
                auto dir = mst::ProfileLibrary::getProfilesFolder();
                dir.createDirectory();
                dir.startAsProcess();
            }
            else if (r == 3)
            {
                const auto err = lib.restoreFactory();
                self.showMessage (err.isEmpty() ? "FACTORY PROFILES" : "COULDN'T RESTORE", err.isEmpty() ? "Back to the profiles that came with the plugin (" + lib.getFactory().packVersion + ")." : err);
            }
            else if (r == 4)
            {
                const auto ver = juce::Time::getCurrentTime().formatted ("%Y.%m.%d");
                self.chooser = std::make_unique<juce::FileChooser> ("Export profile pack", juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile ("the-master-profiles-" + ver + ".json"), "*.json");
                self.chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe, ver] (const juce::FileChooser& fc)
                {
                    if (safe == nullptr || fc.getResult() == juce::File()) return;
                    const auto err = safe->proc.getLibrary().exportPack (fc.getResult(), ver);
                    safe->showMessage (err.isEmpty() ? "PACK EXPORTED" : "EXPORT FAILED", err.isEmpty() ? "Saved " + fc.getResult().getFileName() + ". Upload it for your users, or put it in the repo as Profiles/factory.json for the next release." : err);
                });
            }
            else if (r == 5)
            {
                juce::SystemClipboard::copyTextToClipboard (juce::JSON::toString (fx.toVar(), false));
                self.showMessage ("COPIED", "The measured numbers are on the clipboard as JSON.");
            }
            else if (r >= 100 && r - 100 < (int) lib.genres().size())
            {
                const auto g = lib.genres()[(size_t) (r - 100)];
                const auto err = lib.addReference (g.id, fx);
                if (err.isNotEmpty()) { self.showMessage ("NOT ADDED", err); return; }
                const auto* ng = lib.getUser().find (g.id);
                self.showMessage ("REFERENCE ADDED", mmss (fx.seconds) + " of audio added to " + g.name.toUpperCase() + ". The profile is now built from "
                                  + juce::String (ng != nullptr ? ng->tracks : 1) + " reference song(s). Press RESET, play the next reference, and add it too.");
            }
        });
}

//==============================================================================
void MasterEditor::buildBackdrop()
{
    const float scale = 2.0f;
    const int W = getWidth(), H = getHeight();
    if (W <= 0 || H <= 0) return;
    const int PW = (int) ((float) W * scale), PH = (int) ((float) H * scale);
    juce::Image base (juce::Image::RGB, W, H, false);

    std::vector<juce::Rectangle<float>> pr;
    for (auto& s : plates) pr.push_back (s.bounds.toFloat());
    pr.push_back (badgeArea.toFloat());
    pr.push_back (controlArea.toFloat());
    const auto stripe = stripeArea.toFloat();

    {
        juce::Image::BitmapData bd (base, juce::Image::BitmapData::writeOnly);
        for (int py = 0; py < H; ++py)
        {
            const float y = (float) py;
            for (int px = 0; px < W; ++px)
            {
                const float x = (float) px;
                tex::RGB col;
                int inside = -1; float edge = 1e9f; bool shadow = false;
                for (int i = 0; i < (int) pr.size(); ++i)
                {
                    const auto& p = pr[(size_t) i];
                    if (p.contains (x, y))
                    {
                        inside = i;
                        edge = std::min ({ x - p.getX(), p.getRight() - x, y - p.getY(), p.getBottom() - y });
                        break;
                    }
                    if (p.translated (3.0f, 4.0f).contains (x, y)) shadow = true;
                }
                if (stripe.contains (x, y))
                {
                    const float grainN = tex::fbm (x / 3.0f, y / 3.0f, 3030, 3);
                    const float crease = tex::smoothstep (0.55f, 0.62f, tex::fbm (x / 18.0f, y / 2.0f, 4040, 2));
                    col = tex::mix (tex::rgb (pal::leatherDark), tex::rgb (pal::leather), grainN);
                    col = tex::mul (col, 1.0f - crease * 0.35f);
                    const float wearSpot = tex::smoothstep (0.6f, 0.75f, tex::fbm (x / 40.0f, y / 10.0f, 5050, 3));
                    col = tex::mix (col, tex::rgb (juce::Colour (0xff6a3a2a)), wearSpot * 0.4f);
                    const float t = (y - stripe.getY()) / stripe.getHeight();
                    col = tex::mul (col, 0.75f + 0.35f * std::sin (t * juce::MathConstants<float>::pi));
                }
                else if (inside >= 0)
                {
                    const float d = tex::fbm (x / 30.0f, y / 30.0f, 211, 3);
                    tex::RGB steel = tex::mix (tex::rgb (juce::Colour (0xff1e1a17)), tex::rgb (juce::Colour (0xff332c27)), d);
                    const float u = std::fmod (x + y + 2000.0f, 16.0f) / 16.0f, v = std::fmod (x - y + 2000.0f, 16.0f) / 16.0f;
                    const float tread = (std::abs (u - 0.5f) < 0.08f && std::abs (v - 0.25f) < 0.18f) || (std::abs (v - 0.5f) < 0.08f && std::abs (u - 0.75f) < 0.18f) ? 1.0f : 0.0f;
                    steel = tex::mix (steel, tex::rgb (juce::Colour (0xff4a4038)), tread * 0.55f);
                    const float edgeRust = (1.0f - juce::jlimit (0.0f, 1.0f, edge / 14.0f)) * tex::smoothstep (0.35f, 0.6f, tex::vnoise (x / 6.0f, y / 6.0f, 17));
                    const float bloom = tex::smoothstep (0.6f, 0.75f, tex::fbm (x / 55.0f, y / 55.0f, 404, 3)) * 0.7f;
                    col = tex::mix (steel, tex::rustAt (x, y, 0.15f, 1337), juce::jlimit (0.0f, 1.0f, edgeRust + bloom));
                    col = tex::mul (col, 0.88f);
                }
                else
                {
                    col = tex::rustAt (x, y, 0.1f, 42);
                    if (shadow) col = tex::mul (col, 0.45f);
                }

                const float streak = tex::fbm (x / 9.0f, y / 240.0f, 909, 3);
                const float drip = tex::smoothstep (0.58f, 0.78f, streak) * (0.25f + 0.5f * y / (float) H);
                col = tex::mul (col, 1.0f - drip * 0.6f);
                const float runoff = tex::smoothstep (0.66f, 0.82f, tex::fbm (x / 5.0f, y / 120.0f, 31337, 2));
                col = tex::mix (col, tex::rgb (pal::rustDeep), runoff * 0.35f);
                const float oil = tex::smoothstep (0.7f, 0.8f, tex::fbm (x / 70.0f, y / 50.0f, 555, 3));
                col = tex::mul (col, 1.0f - oil * 0.45f);
                if (tex::hash (px, py, 5) > 0.988f) col = tex::mul (col, 0.45f);
                const float grime = 0.72f + 0.28f * tex::fbm (x / 260.0f, y / 260.0f, 77, 2);
                const float dx = (x / (float) W - 0.5f) * 2.0f, dy = (y / (float) H - 0.5f) * 2.0f;
                const float vig = 1.0f - 0.45f * std::min (1.0f, (dx * dx + dy * dy) * 0.6f);
                col = tex::mul (col, grime * vig);
                bd.setPixelColour (px, py, juce::Colour::fromFloatRGBA (juce::jlimit (0.0f, 1.0f, col.r), juce::jlimit (0.0f, 1.0f, col.g),
                                                                        juce::jlimit (0.0f, 1.0f, col.b), 1.0f));
            }
        }
    }

    backdrop = juce::Image (juce::Image::RGB, PW, PH, false);
    juce::Graphics g (backdrop);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (base, 0, 0, PW, PH, 0, 0, W, H);
    g.addTransform (juce::AffineTransform::scale (scale));
    juce::Random rnd (2026);

    auto rivet = [&] (float cx, float cy, float rad)
    {
        g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillEllipse (cx - rad + 1, cy - rad + 1.5f, rad * 2, rad * 2);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff8a7a68), cx - rad * 0.4f, cy - rad * 0.5f, juce::Colour (0xff2a201a), cx + rad, cy + rad, true));
        g.fillEllipse (cx - rad, cy - rad, rad * 2, rad * 2);
        g.setColour (pal::rust.withAlpha (0.55f)); g.drawEllipse (cx - rad, cy - rad, rad * 2, rad * 2, 1.2f);
        g.setColour (pal::rustDeep.withAlpha (0.35f)); g.fillRect (cx - 0.8f, cy + rad, 1.6f, 3.0f + rnd.nextFloat() * 9.0f);
    };
    auto plateFrame = [&] (juce::Rectangle<float> p, bool hot)
    {
        g.setColour (juce::Colours::white.withAlpha (0.07f)); g.drawLine (p.getX() + 2, p.getY() + 1, p.getRight() - 2, p.getY() + 1, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f)); g.drawRect (p, 1.0f);
        if (hot) { g.setColour (pal::signal.withAlpha (0.45f)); g.drawRect (p.reduced (2.0f), 1.5f); }
        rivet (p.getX() + 6, p.getY() + 6, 3.0f); rivet (p.getRight() - 6, p.getY() + 6, 3.0f);
        rivet (p.getX() + 6, p.getBottom() - 6, 3.0f); rivet (p.getRight() - 6, p.getBottom() - 6, 3.0f);
    };

    for (int i = 0; i < 320; ++i)
    {
        const float x = rnd.nextFloat() * (float) W, y = rnd.nextFloat() * (float) H;
        const float len = 6.0f + rnd.nextFloat() * 70.0f, ang = rnd.nextFloat() * juce::MathConstants<float>::pi;
        g.setColour ((rnd.nextBool() ? juce::Colour (0xffd0c0a8) : juce::Colours::black).withAlpha (0.04f + rnd.nextFloat() * 0.1f));
        g.drawLine (x, y, x + std::cos (ang) * len, y + std::sin (ang) * len, 0.5f + rnd.nextFloat() * 0.6f);
    }

    int seed = 10;
    for (auto& s : plates)
    {
        const auto p = s.bounds.toFloat();
        plateFrame (p, s.hot);
        auto title = tex::distressedText (s.title, s.bounds.getWidth() - 40, 18, tex::stencil (15.0f), s.hot ? pal::signalHot : pal::paint,
                                          juce::Justification::centredLeft, scale, 0.2f, seed++, 0.12f);
        g.setOpacity (1.0f);
        g.drawImage (title, juce::Rectangle<float> (p.getX() + 14, p.getY() + 3, (float) s.bounds.getWidth() - 40, 18.0f));
    }
    for (juce::Component* c : { (juce::Component*) &soundsLike, (juce::Component*) &chart, (juce::Component*) &vitals, (juce::Component*) &score, (juce::Component*) &viewport })
    {
        g.setColour (juce::Colours::black.withAlpha (0.7f)); g.fillRoundedRectangle (c->getBounds().toFloat().expanded (4.0f), 5.0f);
    }
    {   // the verdict list sits in a dark well
        g.setColour (pal::screen); g.fillRoundedRectangle (viewport.getBounds().toFloat(), 4.0f);
    }

    // ANALYSIS plate (LISTEN / RESET / PROFILES)
    {
        const auto ca = controlArea.toFloat();
        plateFrame (ca, false);
        auto t = tex::distressedText ("ANALYSIS", 200, 16, tex::stencil (14.0f), pal::paint, juce::Justification::centredLeft, scale, 0.2f, 7, 0.15f);
        g.setOpacity (1.0f);
        g.drawImage (t, juce::Rectangle<float> (ca.getX() + 12, ca.getY() + 3, 200.0f, 16.0f));
        g.setColour (juce::Colours::black.withAlpha (0.7f)); g.fillRoundedRectangle (statusArea.toFloat().expanded (2.0f), 3.0f);
        g.setColour (pal::screen); g.fillRoundedRectangle (statusArea.toFloat(), 3.0f);
    }

    // leather strap: stitching + chrome studs
    {
        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.drawLine (0, stripe.getY(), (float) W, stripe.getY(), 1.5f);
        g.drawLine (0, stripe.getBottom(), (float) W, stripe.getBottom(), 1.5f);
        juce::Path stitch;
        for (float yy : { stripe.getY() + 3.0f, stripe.getBottom() - 3.0f }) { stitch.startNewSubPath (0, yy); stitch.lineTo ((float) W, yy); }
        juce::Path dashed; const float dl[] = { 5.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, stitch, dl, 2);
        g.setColour (pal::paint.withAlpha (0.45f)); g.fillPath (dashed);
        for (float x = 22.0f; x < (float) W; x += 34.0f)
        {
            const float cy = stripe.getCentreY(), sr = 4.2f;
            g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillEllipse (x - sr + 1.0f, cy - sr + 1.5f, sr * 2, sr * 2);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe8e0d4), x - sr * 0.4f, cy - sr * 0.5f, juce::Colour (0xff3a3430), x + sr, cy + sr, true));
            g.fillEllipse (x - sr, cy - sr, sr * 2, sr * 2);
            if (rnd.nextFloat() < 0.5f) { g.setColour (pal::rust.withAlpha (0.5f)); g.drawEllipse (x - sr, cy - sr, sr * 2, sr * 2, 1.0f); }
            g.setColour (juce::Colours::white.withAlpha (0.7f)); g.fillEllipse (x - sr * 0.45f, cy - sr * 0.55f, sr * 0.6f, sr * 0.45f);
        }
    }

    // CASTLE DANGER MUSIC COMPANY badge with the castle crest
    const auto badge = badgeArea.toFloat();
    plateFrame (badge, false);
    g.setColour (pal::paint.withAlpha (0.22f)); g.drawRect (badge.reduced (5.0f), 1.0f);
    drawCastleCrest (g, crestArea.toFloat());
    const float tx = (float) crestArea.getRight() + 8.0f, tw = badge.getRight() - tx - 10.0f;
    auto l1 = tex::distressedText ("CASTLE DANGER", (int) tw, 28, tex::stencil (25.0f), juce::Colour (0xffeadfc4), juce::Justification::centredLeft, scale, 0.24f, 4242, 0.08f);
    auto l2 = tex::distressedText ("MUSIC COMPANY", (int) tw, 20, tex::stencil (17.0f), pal::rustLight, juce::Justification::centredLeft, scale, 0.24f, 4343, 0.2f);
    auto l3 = tex::distressedText ("WE MAKE MUSIC", (int) tw, 14, tex::plainBold (9.0f), pal::paintDim, juce::Justification::centredLeft, scale, 0.15f, 4444, 0.3f);
    g.setColour (juce::Colours::black);
    g.setOpacity (0.7f); g.drawImage (l1, juce::Rectangle<float> (tx + 1.5f, badge.getY() + 13.0f, tw, 28.0f));
    g.setOpacity (1.0f); g.drawImage (l1, juce::Rectangle<float> (tx, badge.getY() + 11.0f, tw, 28.0f));
    g.setOpacity (0.7f); g.drawImage (l2, juce::Rectangle<float> (tx + 1.5f, badge.getY() + 40.0f, tw, 20.0f));
    g.setOpacity (1.0f); g.drawImage (l2, juce::Rectangle<float> (tx, badge.getY() + 38.0f, tw, 20.0f));
    g.drawImage (l3, juce::Rectangle<float> (tx + 1.0f, badge.getY() + 60.0f, tw, 14.0f));

    auto sub = tex::distressedText ("MIX ANALYSIS & GENRE MATCHING  //  V" + juce::String (JucePlugin_VersionString),
                                    400, 14, tex::plainBold (10.5f), pal::paintDim, juce::Justification::centredLeft, scale, 0.15f, 99, 0.14f);
    g.setOpacity (1.0f);
    g.drawImage (sub, juce::Rectangle<float> (114.0f, 88.0f, 400.0f, 14.0f));

    // the logo: "THE" stamped small over "MASTER" in rusty red, with a scorched analyser trace under it
    {
        const int lw = logoArea.getWidth(), lh = logoArea.getHeight() + 6;
        logoImage = juce::Image (juce::Image::ARGB, (int) ((float) lw * scale), (int) ((float) lh * scale), true);
        juce::Graphics lg (logoImage);
        lg.addTransform (juce::AffineTransform::scale (scale));

        {   // analyser trace: jagged spectrum peaks burning across the bottom
            juce::Path trace;
            juce::Random tr (77);
            const float base = (float) lh - 9.0f;
            trace.startNewSubPath (2.0f, base);
            for (float x = 2.0f; x < (float) lw - 4.0f; x += 5.0f)
            {
                const float env = 0.35f + 0.65f * std::pow (std::sin (x / (float) lw * juce::MathConstants<float>::pi * 1.15f + 0.2f), 2.0f);
                const float spike = tr.nextFloat() < 0.16f ? 1.0f : 0.35f;
                trace.lineTo (x, base - env * spike * (5.0f + tr.nextFloat() * 9.0f));
                trace.lineTo (x + 2.5f, base - tr.nextFloat() * 2.0f);
            }
            lg.setColour (pal::signalHot.withAlpha (0.25f)); lg.strokePath (trace, juce::PathStrokeType (4.0f));
            lg.setColour (pal::signal.withAlpha (0.9f)); lg.strokePath (trace, juce::PathStrokeType (1.2f));
            lg.setColour (pal::hazard.withAlpha (0.5f)); lg.drawLine (2.0f, base + 1.5f, (float) lw - 6.0f, base + 1.5f, 0.8f);
        }

        juce::Image word (juce::Image::ARGB, (int) ((float) lw * scale), (int) ((float) lh * scale), true);
        {
            juce::Graphics wg (word);
            wg.addTransform (juce::AffineTransform::scale (scale));
            auto big = tex::stencil (58.0f).withExtraKerningFactor (0.06f);
            const float theW = 52.0f;
            {
                juce::GlyphArrangement ga; ga.addLineOfText (big, "MASTER", 0, 0);
                const float w = ga.getBoundingBox (0, -1, true).getWidth();
                if (w > (float) lw - theW - 8.0f) big = big.withHeight (58.0f * ((float) lw - theW - 8.0f) / w);
            }
            wg.setColour (pal::paint);
            wg.setFont (tex::stencil (22.0f).withExtraKerningFactor (0.1f));
            wg.drawText ("THE", 0, 10, (int) theW, 24, juce::Justification::centredLeft, false);
            wg.setColour (pal::rustLight);
            wg.fillRect (2.0f, 38.0f, theW - 10.0f, 2.0f);
            wg.setColour (pal::signal);
            wg.setFont (big);
            wg.drawText ("MASTER", (int) theW, 0, lw - (int) theW, 60, juce::Justification::centredLeft, false);
        }
        tex::distress (word, scale, 0.3f, 666);
        lg.drawImage (word, juce::Rectangle<float> (0.0f, 0.0f, (float) lw, (float) lh));
    }
}

void MasterEditor::paint (juce::Graphics& g)
{
    if (backdrop.isValid()) g.drawImage (backdrop, getLocalBounds().toFloat());
    else g.fillAll (pal::ironDark);

    if (logoImage.isValid())
    {
        const float alpha = juce::jlimit (0.45f, 1.0f, 1.0f - flicker * 0.6f);
        const auto la = logoArea.toFloat().withHeight ((float) logoArea.getHeight() + 6.0f);
        g.setColour (juce::Colours::black);
        g.setOpacity (0.65f); g.drawImage (logoImage, la.translated (2.5f, 3.5f));
        g.setOpacity (alpha); g.drawImage (logoImage, la);
        g.setOpacity (1.0f);
    }

    g.setColour (proc.isListening() ? pal::signalHot : pal::paintDim);
    g.setFont (ledFont (10.5f));
    g.drawFittedText (statusText, statusArea.reduced (6, 0), juce::Justification::centredLeft, 1, 0.6f);
}
