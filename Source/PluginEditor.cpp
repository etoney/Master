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
    setColour (juce::Slider::textBoxTextColourId, pal::paint.withAlpha (0.85f));
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff3a2418));
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0x90000000));
    setColour (juce::Slider::textBoxHighlightColourId, pal::rust.withAlpha (0.6f));
    setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0f0b09));
    setColour (juce::TextEditor::textColourId, pal::paint);
    setColour (juce::TextEditor::highlightColourId, pal::rust.withAlpha (0.6f));
    setColour (juce::CaretComponent::caretColourId, pal::signalHot);

    rustDisc = juce::Image (juce::Image::ARGB, 128, 128, true);
    juce::Image::BitmapData bd (rustDisc, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
        {
            const float n = tex::fbm ((float) x / 22.0f, (float) y / 22.0f, 501, 4);
            const float a = tex::smoothstep (0.5f, 0.7f, n);
            const auto c = tex::rustAt ((float) x * 2.0f, (float) y * 2.0f, 0.3f, 777);
            bd.setPixelColour (x, y, juce::Colour::fromFloatRGBA (c.r, c.g, c.b, a * 0.85f));
        }
}

// marker for what AUTO FIX chose: slider property "auto" holds the value (absent = no marker)
static bool autoPos (juce::Slider& s, double& proportion)
{
    const auto v = s.getProperties()["auto"];
    if (v.isVoid()) return false;
    proportion = s.valueToProportionOfLength ((double) v);
    return true;
}

void MasterLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                          float startAngle, float endAngle, juce::Slider& s)
{
    auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
    const float r = std::min (b.getWidth(), b.getHeight()) * 0.5f;
    const auto c = b.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float thick = juce::jmax (2.5f, r * 0.11f);
    const float alpha = s.isEnabled() ? 1.0f : 0.3f;

    g.setColour (pal::paintDim.withAlpha (0.45f * alpha));
    for (int i = 0; i <= 10; ++i)
    {
        const float a = startAngle + (float) i / 10.0f * (endAngle - startAngle) - juce::MathConstants<float>::halfPi;
        const float r1 = r + 1.0f, r0 = r - (i % 5 == 0 ? 5.0f : 3.0f);
        g.drawLine (c.x + std::cos (a) * r0, c.y + std::sin (a) * r0, c.x + std::cos (a) * r1, c.y + std::sin (a) * r1, i % 5 == 0 ? 1.4f : 0.9f);
    }
    const float ar = r - thick - 4.0f;
    juce::Path track; track.addCentredArc (c.x, c.y, ar, ar, 0, startAngle, endAngle, true);
    g.setColour (juce::Colour (0xff120e0c).withAlpha (alpha)); g.strokePath (track, juce::PathStrokeType (thick + 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (juce::Colour (0xff3a2418).withAlpha (alpha)); g.strokePath (track, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    // bipolar knobs (width, gain) draw their arc from the neutral point
    const double neutral = s.getProperties().contains ("neutral") ? s.valueToProportionOfLength ((double) s.getProperties()["neutral"]) : 0.0;
    const float a0 = startAngle + (float) neutral * (endAngle - startAngle);
    juce::Path val; val.addCentredArc (c.x, c.y, ar, ar, 0, std::min (a0, angle), std::max (a0, angle), true);
    const auto arcCol = (s.isMouseOverOrDragging() ? pal::signalHot : pal::signal).withAlpha (alpha);
    g.setColour (arcCol.withAlpha (0.25f * alpha));
    g.strokePath (val, juce::PathStrokeType (thick + 4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (arcCol);
    g.strokePath (val, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    double ap;
    if (autoPos (s, ap))
    {
        const float aa = startAngle + (float) ap * (endAngle - startAngle) - juce::MathConstants<float>::halfPi;
        juce::Path tri;
        const float rr = r + 2.0f;
        tri.addTriangle (c.x + std::cos (aa) * (rr - 7), c.y + std::sin (aa) * (rr - 7),
                         c.x + std::cos (aa + 0.09f) * (rr + 1), c.y + std::sin (aa + 0.09f) * (rr + 1),
                         c.x + std::cos (aa - 0.09f) * (rr + 1), c.y + std::sin (aa - 0.09f) * (rr + 1));
        g.setColour (pal::hazard.withAlpha (alpha)); g.fillPath (tri);
    }

    const float br = ar - thick * 1.4f;
    if (br < 4.0f) return;
    const juce::Rectangle<float> body (c.x - br, c.y - br, br * 2, br * 2);
    g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillEllipse (body.translated (1.5f, 2.5f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff6a5a4c).withAlpha (alpha), c.x - br * 0.5f, c.y - br * 0.7f,
                                             juce::Colour (0xff17120f), c.x + br * 0.6f, c.y + br, true));
    g.fillEllipse (body);
    {
        juce::Graphics::ScopedSaveState ss (g);
        juce::Path clip; clip.addEllipse (body);
        g.reduceClipRegion (clip);
        const float off = (float) (((juce::uint64) (juce::pointer_sized_int) &s >> 4) % 64);
        g.setOpacity (0.9f * alpha);
        g.drawImage (rustDisc, body.expanded (br * 0.4f).translated (-off * 0.3f, off * 0.2f), juce::RectanglePlacement::stretchToFit);
    }
    g.setColour (juce::Colour (0xff0f0b09).withAlpha (0.8f));
    for (int i = 0; i < 32; ++i)
    {
        const float a = (float) i / 32.0f * juce::MathConstants<float>::twoPi;
        g.drawLine (c.x + std::cos (a) * br * 0.87f, c.y + std::sin (a) * br * 0.87f, c.x + std::cos (a) * br, c.y + std::sin (a) * br, 1.0f);
    }
    g.setColour (juce::Colour (0xff0b0807)); g.drawEllipse (body, 1.2f);
    juce::Path ptr; ptr.addRoundedRectangle (-1.8f, -br * 0.92f, 3.6f, br * 0.6f, 1.2f);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillPath (ptr, juce::AffineTransform::rotation (angle).translated (c.x + 0.8f, c.y + 1.0f));
    g.setColour (pal::paint.withAlpha (alpha));
    g.fillPath (ptr, juce::AffineTransform::rotation (angle).translated (c));
}

// vertical EQ faders: a rusty slot, the move drawn from 0 dB, a stamped cap, the AUTO marker
void MasterLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                          juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearVertical) { LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s); return; }
    const float alpha = s.isEnabled() ? 1.0f : 0.35f;
    const float cx = (float) x + (float) w * 0.5f;
    const float zeroY = (float) (y + h) - (float) s.valueToProportionOfLength (0.0) * (float) h;
    auto slot = juce::Rectangle<float> (cx - 3.0f, (float) y, 6.0f, (float) h);
    g.setColour (juce::Colour (0xff0c0908)); g.fillRoundedRectangle (slot.expanded (1.0f, 4.0f), 3.0f);
    g.setColour (juce::Colour (0xff3a2418)); g.drawRoundedRectangle (slot.expanded (1.0f, 4.0f), 3.0f, 1.0f);
    // ticks every 2 dB
    g.setColour (pal::paintDim.withAlpha (0.35f * alpha));
    for (double d = s.getMinimum(); d <= s.getMaximum() + 1e-6; d += 2.0)
    {
        const float ty = (float) (y + h) - (float) s.valueToProportionOfLength (d) * (float) h;
        const float len = std::abs (d) < 1e-6 ? 12.0f : 6.0f;
        g.drawLine (cx - 7 - len, ty, cx - 7, ty, std::abs (d) < 1e-6 ? 1.4f : 0.8f);
        g.drawLine (cx + 7, ty, cx + 7 + len, ty, std::abs (d) < 1e-6 ? 1.4f : 0.8f);
    }
    const float v = (float) s.getValue();
    const auto col = (std::abs (v) < 0.05f ? pal::paintDim : (v > 0 ? pal::signalHot : pal::signal)).withAlpha (alpha);
    g.setColour (col.withAlpha (0.85f * alpha));
    g.fillRect (juce::Rectangle<float> (cx - 2.0f, std::min (pos, zeroY), 4.0f, std::abs (pos - zeroY)));
    double ap;
    if (autoPos (s, ap))
    {
        const float ay = (float) (y + h) - (float) ap * (float) h;
        juce::Path tri; tri.addTriangle (cx - 15, ay - 5, cx - 15, ay + 5, cx - 8, ay);
        juce::Path tri2; tri2.addTriangle (cx + 15, ay - 5, cx + 15, ay + 5, cx + 8, ay);
        g.setColour (pal::hazard.withAlpha (alpha)); g.fillPath (tri); g.fillPath (tri2);
    }
    auto cap = juce::Rectangle<float> (cx - 13.0f, pos - 7.0f, 26.0f, 14.0f);
    g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillRoundedRectangle (cap.translated (1.0f, 2.0f), 2.5f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff7a6a5a).withAlpha (alpha), cap.getX(), cap.getY(), juce::Colour (0xff221a15), cap.getX(), cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 2.5f);
    g.setColour (s.isMouseOverOrDragging() ? pal::signalHot : juce::Colour (0xff0c0908)); g.drawRoundedRectangle (cap, 2.5f, 1.0f);
    g.setColour (pal::paint.withAlpha (alpha)); g.fillRect (cap.getX() + 4, cap.getCentreY() - 0.75f, cap.getWidth() - 8, 1.5f);
}

juce::Font MasterLookAndFeel::getTextButtonFont (juce::TextButton& b, int h)
{
    if (b.getName() == "autofix") return tex::stencil (26.0f).withExtraKerningFactor (0.12f);
    return tex::stencil (juce::jmin (15.0f, (float) h * 0.6f)).withExtraKerningFactor (0.06f);
}
juce::Font MasterLookAndFeel::getPopupMenuFont() { return tex::plainBold (14.0f); }

void MasterLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool hi, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    if (b.getName() == "autofix")
    {
        juce::Path pth; pth.addRoundedRectangle (r, 4.0f);
        juce::DropShadow (pal::signalHot.withAlpha (hi ? 0.7f : 0.45f), 14, {}).drawForPath (g, pth);
        g.setGradientFill (juce::ColourGradient (down ? juce::Colour (0xff5a1a0c) : juce::Colour (0xffa83218), 0, r.getY(), juce::Colour (0xff3a0e06), 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (hi ? juce::Colour (0xffffc090) : pal::signalHot); g.drawRoundedRectangle (r, 4.0f, 1.8f);
        return;
    }
    if ((bool) b.getProperties()["overridden"])
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillRoundedRectangle (r.translated (1.0f, 1.5f), 3.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff6a5010), 0, r.getY(), juce::Colour (0xff2a1e06), 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (hi ? juce::Colour (0xffffe080) : pal::hazard); g.drawRoundedRectangle (r, 3.0f, 1.3f);
        return;
    }
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
    g.setFont (r.getWidth() < 120.0f ? tex::stencil (12.5f).withExtraKerningFactor (0.03f) : tex::stencil (15.0f).withExtraKerningFactor (0.1f));
    g.drawFittedText (b.getButtonText(), r.withTrimmedLeft (r.getHeight() * 0.95f).withTrimmedRight (4).toNearestInt(), juce::Justification::centredLeft, 1, 0.6f);
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
Selector::Selector (juce::RangedAudioParameter* p, juce::StringArray l, float fs) : labels (std::move (l)), fontSize (fs)
{
    if (p != nullptr)
    {
        att = std::make_unique<juce::ParameterAttachment> (*p, [this] (float v) { index = juce::roundToInt (v); repaint(); if (onChange) onChange (index); });
        att->sendInitialUpdate();
    }
}

int Selector::segmentAt (juce::Point<int> p) const
{
    if (labels.isEmpty()) return -1;
    return juce::jlimit (0, labels.size() - 1, p.x * labels.size() / juce::jmax (1, getWidth()));
}

void Selector::mouseDown (const juce::MouseEvent& e)
{
    const int i = segmentAt (e.getPosition());
    if (i < 0) return;
    if (att != nullptr) att->setValueAsCompleteGesture ((float) i);
    else { index = i; repaint(); if (onChange) onChange (i); }
}

void Selector::mouseMove (const juce::MouseEvent& e) { const int h = segmentAt (e.getPosition()); if (h != hover) { hover = h; repaint(); } }

void Selector::paint (juce::Graphics& g)
{
    const int n = labels.size();
    const float sw = (float) getWidth() / (float) juce::jmax (1, n);
    const float alpha = isEnabled() ? 1.0f : 0.4f;
    for (int i = 0; i < n; ++i)
    {
        auto r = juce::Rectangle<float> ((float) i * sw, 0, sw, (float) getHeight()).reduced (2.5f, 2.0f);
        const bool on = i == index, hi = i == hover;
        g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillRoundedRectangle (r.translated (1.5f, 2.0f), 3.0f);
        if (on)
        {
            juce::Path pth; pth.addRoundedRectangle (r, 3.0f);
            juce::DropShadow (pal::signalHot.withAlpha (0.4f * alpha), 10, {}).drawForPath (g, pth);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff8a2a14).withAlpha (alpha), r.getCentreX(), r.getY(), juce::Colour (0xff3a0e06), r.getCentreX(), r.getBottom(), false));
        }
        else
            g.setGradientFill (juce::ColourGradient (hi ? juce::Colour (0xff43332a) : juce::Colour (0xff332822), r.getCentreX(), r.getY(),
                                                     juce::Colour (0xff15100d), r.getCentreX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour ((on ? pal::signalHot : (hi ? pal::rustLight : juce::Colour (0xff5a3420))).withAlpha (alpha));
        g.drawRoundedRectangle (r, 3.0f, on ? 1.5f : 1.1f);
        g.setColour ((on ? juce::Colour (0xfffff0d8) : pal::paint.withAlpha (0.85f)).withMultipliedAlpha (alpha));
        g.setFont (tex::stencil (fontSize).withExtraKerningFactor (0.08f));
        g.drawFittedText (labels[i], r.reduced (4, 1).toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }
}


//==============================================================================
void StepList::paint (juce::Graphics& g)
{
    // the step you're on, in full: what it means and what to press
    static const char* titles[] = { "LISTEN", "REPORT", "PRESCRIPTION", "APPLY", "REFINE" };
    const int i = juce::jlimit (0, 4, current);
    auto r = getLocalBounds().toFloat().reduced (10.0f, 8.0f);
    g.setColour (pal::paintDim);
    g.setFont (tex::stencil (11.5f).withExtraKerningFactor (0.12f));
    g.drawText ("STEP " + juce::String (i + 1) + " OF 5", r.removeFromTop (16.0f).toNearestInt(), juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xfffff0d8));
    g.setFont (tex::stencil (22.0f).withExtraKerningFactor (0.08f));
    g.drawText (titles[i], r.removeFromTop (30.0f).toNearestInt(), juce::Justification::centredLeft);
    r.removeFromTop (4.0f);
    juce::AttributedString as;
    as.append (details[i], bodyFont (13.0f), pal::paint);
    as.setWordWrap (juce::AttributedString::byWord);
    juce::TextLayout tl; tl.createLayout (as, r.getWidth());
    tl.draw (g, r.withHeight (juce::jmin (r.getHeight(), tl.getHeight())));
}

//==============================================================================
int StepBar::stepAt (juce::Point<int> p) const { return juce::jlimit (0, 4, p.x * 5 / juce::jmax (1, getWidth())); }
void StepBar::mouseMove (const juce::MouseEvent& e) { const int h = stepAt (e.getPosition()); if (h != hover) { hover = h; repaint(); } }
void StepBar::mouseDown (const juce::MouseEvent& e) { const int i = stepAt (e.getPosition()); if (i <= open && onPick) onPick (i); }

void StepBar::paint (juce::Graphics& g)
{
    static const char* titles[] = { "LISTEN", "REPORT", "PRESCRIPTION", "APPLY", "REFINE" };
    const float sw = (float) getWidth() / 5.0f, h = (float) getHeight(), tip = 12.0f;
    for (int i = 0; i < 5; ++i)
    {
        // a chevron: each step points at the next
        const float x0 = (float) i * sw + 2.0f, x1 = (float) (i + 1) * sw - 2.0f;
        juce::Path pth;
        pth.startNewSubPath (x0, 2.0f);
        pth.lineTo (x1 - (i < 4 ? tip : 0.0f), 2.0f);
        if (i < 4) pth.lineTo (x1, h * 0.5f);
        pth.lineTo (x1 - (i < 4 ? tip : 0.0f), h - 2.0f);
        pth.lineTo (x0, h - 2.0f);
        if (i > 0) pth.lineTo (x0 + tip, h * 0.5f);
        pth.closeSubPath();
        const bool done = i < now, here = i == now, locked = i > open, onPage = (i <= 1) == (page == 0), hi = i == hover && ! locked;
        if (here)
        {
            juce::DropShadow (pal::signalHot.withAlpha (0.45f), 10, {}).drawForPath (g, pth);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff8a2a14), 0, 0, juce::Colour (0xff3a0e06), 0, h, false));
        }
        else
            g.setGradientFill (juce::ColourGradient (hi ? juce::Colour (0xff43332a) : juce::Colour (0xff2c221d), 0, 0, juce::Colour (0xff120d0b), 0, h, false));
        g.fillPath (pth);
        g.setColour (here ? pal::signalHot : (onPage && ! locked ? pal::rustLight : juce::Colour (0xff4a2c1c)));
        g.strokePath (pth, juce::PathStrokeType (here ? 1.6f : (onPage && ! locked ? 1.3f : 1.0f)));

        auto r = juce::Rectangle<float> (x0 + (i > 0 ? tip + 4.0f : 8.0f), 0, x1 - x0 - (i > 0 ? tip + 4.0f : 8.0f) - tip, h);
        auto badge = r.removeFromLeft (20.0f).withSizeKeepingCentre (18.0f, 18.0f);
        g.setColour (done ? juce::Colour (0xff9aa86a) : (here ? pal::signalHot : (locked ? juce::Colour (0xff3a2418) : pal::rustDeep)));
        g.fillEllipse (badge);
        g.setColour (done ? juce::Colour (0xff1a1410) : (locked ? pal::paintDim.withAlpha (0.5f) : pal::paint));
        g.setFont (tex::stencil (11.0f));
        g.drawText (done ? juce::String ("OK") : juce::String (i + 1), badge.toNearestInt(), juce::Justification::centred);
        r.removeFromLeft (6.0f);
        g.setColour (here ? juce::Colour (0xfffff0d8) : (locked ? pal::paintDim.withAlpha (0.45f) : pal::paint.withAlpha (done ? 0.75f : 0.9f)));
        g.setFont (tex::stencil (14.5f).withExtraKerningFactor (0.08f));
        g.drawFittedText (titles[i], r.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    }
}

//==============================================================================
static int plugOrder (const juce::String& kind)
{
    // Dead Air right after the amp sim, Flatliner before the levelling, The Servant, then Dominatrix's
    // saturation (bass) or Cutthroat's clip (drums), and Low Blow's duck last
    if (kind == "buzzkill") return 0;
    if (kind == "flatliner") return 1;
    if (kind == "servant") return 2;
    if (kind == "dominatrix") return 3;
    if (kind == "chopshop") return 4;
    if (kind == "lowblow") return 5;
    return 6;
}

void TrackChainList::setRows (std::vector<Row> r, int width)
{
    rows = std::move (r);
    setSize (width, juce::jmax (1, (int) rows.size() * rowH));
    repaint();
}

juce::String TrackChainList::getTooltip()
{
    const int i = getMouseXYRelative().y / rowH;
    return juce::isPositiveAndBelow (i, (int) rows.size()) ? rows[(size_t) i].tip : juce::String();
}

void TrackChainList::paint (juce::Graphics& g)
{
    const auto ok = juce::Colour (0xff9aa86a);
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto& row = rows[i];
        auto r = juce::Rectangle<float> (0, (float) i * rowH, (float) getWidth(), (float) rowH).reduced (0, 2);
        if (i % 2 == 0) { g.setColour (juce::Colours::white.withAlpha (0.03f)); g.fillRoundedRectangle (r, 3.0f); }
        r = r.reduced (4, 1);
        auto top = r.removeFromTop (16);
        top.removeFromLeft (2);
        g.setColour (pal::paintDim); g.setFont (ledFont (10.0f));
        g.drawFittedText (row.role.toUpperCase(), top.removeFromRight (112).toNearestInt(), juce::Justification::centredRight, 1, 0.8f);
        g.setColour (pal::paint); g.setFont (tex::stencil (13.0f).withExtraKerningFactor (0.05f));
        g.drawFittedText (row.track.toUpperCase(), top.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);

        // the chain, each plugin with a lamp: green = linked to The Master, red = add it, yellow = joined late
        auto line = r.withTrimmedLeft (12);
        auto labelOf = [] (const Plug& p, int n) { return juce::String (n) + " " + p.name.toUpperCase() + (p.status == 3 ? " (GONE)" : ""); };
        float fs = 12.0f, total = 0;
        for (; fs >= 9.0f; fs -= 0.5f)
        {
            const auto f = tex::stencil (fs).withExtraKerningFactor (0.04f);
            total = 0;
            for (size_t k = 0; k < row.plugs.size(); ++k)
                total += 11.0f + (float) juce::GlyphArrangement::getStringWidthInt (f, labelOf (row.plugs[k], (int) k + 1)) + (k + 1 < row.plugs.size() ? 18.0f : 0.0f);
            if (total <= line.getWidth()) break;
        }
        const auto f = tex::stencil (fs).withExtraKerningFactor (0.04f);
        g.setFont (f);
        float x = line.getX();
        for (size_t k = 0; k < row.plugs.size(); ++k)
        {
            const auto& p = row.plugs[k];
            const auto label = labelOf (p, (int) k + 1);
            const bool on = p.status == 0 || p.status == 2;   // linked to The Master (2: not planned yet, UPDATE does it)
            const auto col = on ? ok : (p.status == 4 ? pal::paintDim : pal::signalHot);
            auto lamp = juce::Rectangle<float> (7, 7).withCentre ({ x + 3.5f, line.getCentreY() });
            if (on) { juce::Path lp; lp.addEllipse (lamp); juce::DropShadow (ok.withAlpha (0.6f), 5, {}).drawForPath (g, lp); g.setColour (ok); g.fillEllipse (lamp); }
            else { g.setColour (col); g.drawEllipse (lamp.reduced (0.5f), 1.4f); }
            x += 11.0f;
            const float w = (float) juce::GlyphArrangement::getStringWidthInt (f, label) + 2.0f;
            g.setColour (on ? ok.brighter (0.25f) : col);
            g.drawText (label, juce::Rectangle<float> (x, line.getY(), w, line.getHeight()).toNearestInt(), juce::Justification::centredLeft, false);
            x += w;
            if (k + 1 < row.plugs.size())
            {
                g.setColour (pal::rustLight.withAlpha (0.8f)); g.drawText (">", juce::Rectangle<float> (x, line.getY(), 16, line.getHeight()).toNearestInt(), juce::Justification::centred);
                x += 16;
            }
        }
    }
}

ChainView::ChainView()
{
    tracksView.setViewedComponent (&trackList, false);
    tracksView.setScrollBarsShown (true, false);
    tracksView.setScrollBarThickness (8);
    tracksView.setSingleStepSizes (10, TrackChainList::rowH);
    tracksView.getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, pal::rust);
    tracksView.getVerticalScrollBar().setColour (juce::ScrollBar::trackColourId, juce::Colour (0xff1a1410));
    addChildComponent (tracksView);
}

juce::Rectangle<float> ChainView::tracksArea() const
{
    auto r = getLocalBounds().toFloat().reduced (12, 8);
    return r.removeFromRight (r.getWidth() * 0.46f);
}

void ChainView::resized()
{
    tracksView.setBounds (tracksArea().withTrimmedTop (20).toNearestInt());
    trackList.setRows ({}, tracksView.getWidth() - 10);
}

void ChainView::rebuildTracks (const std::vector<mst::ChainPlan::TrackAdd>& adds)
{
    std::vector<TrackChainList::Row> rows;
    auto rowFor = [&] (const juce::String& name) -> TrackChainList::Row&
    {
        const auto key = name.isNotEmpty() ? name : juce::String ("(unnamed track)");
        for (auto& r : rows) if (r.track.equalsIgnoreCase (key)) return r;
        rows.push_back ({ key, {}, {}, {} });
        return rows.back();
    };
    for (auto& t : tracks)
    {
        auto& row = rowFor (t.name);
        TrackChainList::Plug p;
        p.kind = t.kind; p.name = mst::plugDisplayName (t.kind); p.order = plugOrder (t.kind);
        p.status = ! t.linked ? 3 : (t.planned ? 0 : 2);
        row.plugs.push_back (p);
        if (t.kind == "servant") row.role = t.role;
        else if (row.role.isEmpty()) row.role = t.role;
        const auto line = p.name + ": " + (t.linked ? t.summary : juce::String ("not linked any more"));
        row.tip << (row.tip.isEmpty() ? "" : "\n") << line;
    }
    // a Flatliner already on a vocal bus answers "put Flatliner on a lead / backing vocal bus": show the bus, not the advice
    bool leadBus = false, backBus = false, gtrBus = false;
    for (auto& t : tracks)
        if (t.kind == "flatliner" && t.name.containsIgnoreCase ("bus")) (mst::guitarName (t.name) ? gtrBus : (mst::backingName (t.name) ? backBus : leadBus)) = true;
    for (auto& a : adds)
    {
        if (a.kind == "flatliner" && ((a.track == "a lead vocal bus" && leadBus) || (a.track == "a backing vocal bus" && backBus) || (a.track == "a lead guitar bus" && gtrBus))) continue;
        auto& row = rowFor (a.track.isNotEmpty() ? a.track : juce::String ("Guitar and bass tracks"));
        if (a.track.isEmpty()) row.role = "amp noise";
        bool have = false;
        for (auto& p : row.plugs) if (p.kind == a.kind) have = true;
        if (! have) row.plugs.push_back ({ a.kind, mst::plugDisplayName (a.kind), 1, plugOrder (a.kind) });
        row.tip << (row.tip.isEmpty() ? "" : "\n") << "ADD " << mst::plugDisplayName (a.kind) << ": " << a.why;
    }
    // every track with a helper needs The Servant too (it's what tells The Master the track's role and sound)
    int servants = 0;
    for (auto& r : rows)
    {
        bool hasServant = false;
        for (auto& p : r.plugs) if (p.kind == "servant") hasServant = true;
        if (hasServant) { ++servants; continue; }
        if (r.track == "Guitar and bass tracks") continue;
        if (r.track.containsIgnoreCase ("bus")) continue;   // a bus carries tracks that have their own Servants
        r.plugs.push_back ({ "servant", "The Servant", 1, plugOrder ("servant") });
        r.tip << (r.tip.isEmpty() ? "" : "\n") << "ADD The Servant: set its ROLE, so The Master knows what this track is and can balance it.";
    }
    for (auto& r : rows)
        std::stable_sort (r.plugs.begin(), r.plugs.end(), [] (const TrackChainList::Plug& a, const TrackChainList::Plug& b) { return a.order < b.order; });
    // tracks that need something added come first
    std::stable_sort (rows.begin(), rows.end(), [] (const TrackChainList::Row& a, const TrackChainList::Row& b)
    {
        auto needs = [] (const TrackChainList::Row& r) { for (auto& p : r.plugs) if (p.status == 1 || p.status == 3) return true; return false; };
        return needs (a) && ! needs (b);
    });
    // the standing instruction, last: The Servant on every track you want balanced
    rows.push_back ({ servants == 0 ? juce::String ("Your tracks") : juce::String ("Any other track"), "set its ROLE",
                      "Put The Servant on every track you want balanced and set its ROLE (Lead, Rhythm, Bass, Kick...). "
                      "Then press RESET on The Master and play the song, so each one hears its track.",
                      { { "servant", "The Servant", servants == 0 && stage >= 1 ? 1 : 4, 2 } } });
    const int y = tracksView.getViewPositionY();
    trackList.setRows (std::move (rows), tracksView.getWidth() - 10);
    tracksView.setVisible (! trackList.empty());
    tracksView.setViewPosition (0, y);
}

void ChainView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (pal::screen); g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colour (0xff3a2418)); g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    r = r.reduced (12, 8);
    auto right = r.removeFromRight (r.getWidth() * 0.46f);
    r.removeFromRight (14);
    g.setColour (juce::Colour (0x30d6c7a6)); g.drawVerticalLine ((int) right.getX() - 7, right.getY(), right.getBottom());

    // ---- right: the tracks (The Servant)
    {
        auto rr = right;
        g.setColour (pal::rustLight); g.setFont (tex::stencil (13.0f).withExtraKerningFactor (0.08f));
        auto head = rr.removeFromTop (17);
        if (! trackList.empty())
        {
            // key: green lamp = linked to The Master, red ring = add it
            auto key = head.removeFromRight (128); head.removeFromRight (10);
            g.setFont (ledFont (9.0f));
            g.setColour (juce::Colour (0xff9aa86a)); g.fillEllipse (key.getX(), key.getCentreY() - 3, 6, 6);
            g.drawText ("LINKED", key.withTrimmedLeft (9).removeFromLeft (52).toNearestInt(), juce::Justification::centredLeft);
            g.setColour (pal::signalHot); g.drawEllipse (key.getX() + 62, key.getCentreY() - 3, 6, 6, 1.2f);
            g.drawText ("ADD IT", key.withTrimmedLeft (71).toNearestInt(), juce::Justification::centredLeft);
            g.setColour (pal::rustLight); g.setFont (tex::stencil (13.0f).withExtraKerningFactor (0.08f));
        }
        g.drawFittedText ("YOUR TRACKS  //  PLUGINS, IN ORDER", head.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
        if (trackList.empty())
        {
            g.setColour (pal::paintDim); g.setFont (bodyFont (11.5f));
            g.drawFittedText ("No Servants linked. Put The Servant on your tracks and set each one's ROLE: The Master then shares the tone, "
                              "compression and level fixes out to the tracks that cause them.", rr.toNearestInt(), juce::Justification::topLeft, 6, 0.9f);
        }
    }

    // ---- left: the master bus chain
    if (! hasPlan)
    {
        g.setColour (pal::paintDim); g.setFont (bodyFont (12.5f));
        g.drawFittedText ("Play your whole song with nothing on the master bus except The Master (last), then press PRESCRIBE. "
                          "The Master will tell you which Castle Danger plugins to add, and in what order.", r.toNearestInt(), juce::Justification::centredLeft, 5);
        return;
    }
    g.setColour (pal::rustLight); g.setFont (tex::stencil (13.0f).withExtraKerningFactor (0.08f));
    g.drawText ("MASTER BUS", r.removeFromTop (17).toNearestInt(), juce::Justification::centredLeft);
    const int n = (int) steps.size() + 1;
    auto list = r.removeFromTop (juce::jmin (r.getHeight() - (onTracks.isEmpty() ? 0.0f : 40.0f), 38.0f * (float) n));
    const float rh = list.getHeight() / (float) n;
    for (int i = 0; i < n; ++i)
    {
        auto row = list.removeFromTop (rh).reduced (0, 2);
        const bool isMaster = i == n - 1;
        const auto* s = isMaster ? nullptr : &steps[(size_t) i];
        auto badge = row.removeFromLeft (row.getHeight()).reduced (3);
        g.setColour (isMaster ? pal::rust : pal::rustDeep); g.fillEllipse (badge);
        g.setColour (pal::paint); g.setFont (tex::stencil (badge.getHeight() * 0.6f));
        g.drawText (juce::String (i + 1), badge.toNearestInt(), juce::Justification::centred);
        row.removeFromLeft (8);
        juce::Colour col; juce::String txt;
        if (isMaster) { col = juce::Colour (0xff9aa86a); txt = "YOU ARE HERE (LAST)"; }
        else if (! s->linked) { col = pal::signalHot; txt = "NOT FOUND: ADD IT"; }
        else if (s->position < 0) { col = pal::hazard; txt = orderKnown ? "LINKED, NOT IN CHAIN?" : "LINKED: PLAY TO CHECK"; }
        else if (! s->orderOk) { col = pal::signalHot; txt = "LINKED: WRONG SPOT"; }
        else if (s->rebel) { col = pal::signal; txt = "REBEL: HANDS OFF"; }
        else { col = juce::Colour (0xff9aa86a); txt = stage >= 2 ? "LINKED: SET" : "LINKED: IN ORDER"; }
        auto head = row.removeFromTop (row.getHeight() * 0.55f);
        auto stat = head.removeFromRight (150);
        g.setColour (col); g.fillEllipse (stat.getX(), stat.getCentreY() - 4, 8, 8);
        g.setFont (ledFont (10.0f));
        g.drawFittedText (txt, stat.withTrimmedLeft (13).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
        g.setColour (pal::paint); g.setFont (tex::stencil (15.5f).withExtraKerningFactor (0.06f));
        g.drawText (isMaster ? juce::String ("THE MASTER") : s->name.toUpperCase(), head.toNearestInt(), juce::Justification::bottomLeft);
        g.setColour (pal::paintDim); g.setFont (bodyFont (10.5f));
        g.drawFittedText (isMaster ? juce::String ("Measures the result; tone EQ (what the tracks can't fix) + safety limiter.") : s->role, row.toNearestInt(), juce::Justification::topLeft, 1, 0.75f);
    }
    if (! onTracks.isEmpty())
    {
        r.removeFromTop (2);
        g.setColour (pal::paint.withAlpha (0.8f)); g.setFont (bodyFont (10.5f));
        g.drawFittedText (onTracks.joinIntoString ("\n"), r.toNearestInt(), juce::Justification::topLeft, 3, 0.8f);
    }
}

void ChangeLog::setEntries (const std::vector<MasterProcessor::LogEntry>& entries, int width, const juce::String& emptyText)
{
    laid.clear();
    empty = emptyText;
    int y = 4;
    for (auto& e : entries)
    {
        Laid l; l.e = e;
        juce::AttributedString a;
        a.append (e.detail, bodyFont (12.0f), pal::paint.withAlpha (0.9f));
        a.setWordWrap (juce::AttributedString::byWord);
        l.body.createLayout (a, (float) width - 40.0f);
        l.bodyH = (int) std::ceil (l.body.getHeight());
        l.y = y; l.h = 8 + 18 + l.bodyH + 8;
        y += l.h;
        laid.push_back (std::move (l));
    }
    setSize (width, juce::jmax (y + 4, 40));
    repaint();
}

void ChangeLog::paint (juce::Graphics& g)
{
    if (laid.empty())
    {
        g.setColour (pal::paintDim); g.setFont (bodyFont (12.5f));
        g.drawFittedText (empty, getLocalBounds().reduced (10, 8), juce::Justification::topLeft, 3);
        return;
    }
    for (auto& l : laid)
    {
        auto r = juce::Rectangle<float> (0, (float) l.y, (float) getWidth(), (float) l.h);
        const auto c = l.e.confirmed ? juce::Colour (0xff9aa86a) : pal::hazard;
        auto lamp = juce::Rectangle<float> (8, 8).withCentre ({ 12, r.getY() + 17 });
        g.setColour (c); g.fillEllipse (lamp);
        auto in = r.withTrimmedLeft (26).withTrimmedRight (8).withTrimmedTop (8);
        auto head = in.removeFromTop (18);
        g.setColour (pal::signalHot); g.setFont (tex::stencil (13.5f).withExtraKerningFactor (0.08f));
        const auto tag = l.e.plugin.toUpperCase();
        const float tw = (float) juce::GlyphArrangement::getStringWidthInt (tex::stencil (13.5f).withExtraKerningFactor (0.08f), tag) + 12.0f;
        g.drawText (tag, head.removeFromLeft (tw).toNearestInt(), juce::Justification::centredLeft);
        g.setColour (pal::paint); g.setFont (tex::stencil (13.5f).withExtraKerningFactor (0.06f));
        g.drawFittedText (l.e.title, head.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
        l.body.draw (g, in.removeFromTop ((float) l.bodyH));
        g.setColour (juce::Colour (0x30d6c7a6)); g.drawHorizontalLine ((int) r.getBottom() - 1, 6, (float) getWidth() - 6);
    }
}

//==============================================================================
void FixInfo::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (pal::screen); g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colour (0xff3a2418)); g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    r = r.reduced (10, 8);

    // BEFORE -> AFTER
    auto top = r.removeFromTop (54);
    auto scoreCol = [] (int sc) { return sc < 0 ? pal::paintDim.withAlpha (0.4f) : (sc >= 80 ? juce::Colour (0xff9aa86a) : sc >= 60 ? pal::hazard : pal::signalHot); };
    auto half = top.getWidth() * 0.5f;
    auto bA = top.removeFromLeft (half), aA = top;
    for (int k = 0; k < 2; ++k)
    {
        auto a = k == 0 ? bA : aA;
        const int sc = k == 0 ? before : after;
        g.setColour (pal::paintDim); g.setFont (tex::plainBold (9.0f).withExtraKerningFactor (0.1f));
        g.drawText (k == 0 ? "MIX SCORE: YOURS" : "AFTER FIXES", a.removeFromTop (12).toNearestInt(), juce::Justification::centred);
        g.setColour (scoreCol (sc)); g.setFont (tex::stencil (36.0f));
        g.drawText (sc >= 0 ? juce::String (sc) : juce::String ("--"), a.toNearestInt(), juce::Justification::centred);
        if (sc >= 0) { g.setColour (pal::paintDim); g.setFont (tex::stencil (11.0f)); g.drawText ("/100", a.withTrimmedLeft (a.getWidth() * 0.68f).withTrimmedTop (a.getHeight() * 0.45f).toNearestInt(), juce::Justification::centredLeft); }
    }
    g.setColour (pal::paintDim); g.setFont (tex::stencil (22.0f));
    g.drawText (">", juce::Rectangle<float> (bA.getRight() - 10, bA.getY() + 12, 20, 40).toNearestInt(), juce::Justification::centred);

    r.removeFromTop (6);
    g.setColour (pal::paint.withAlpha (0.85f)); g.setFont (bodyFont (11.5f));
    g.drawFittedText (setFrom, r.removeFromTop (30).toNearestInt(), juce::Justification::topLeft, 2, 0.9f);

    // live meters
    g.setFont (ledFont (10.5f));
    auto meter = [&] (const juce::String& name, float v, const juce::String& txt)
    {
        auto row = r.removeFromTop (16);
        g.setColour (pal::paintDim); g.drawText (name, row.removeFromLeft (78).toNearestInt(), juce::Justification::centredLeft);
        auto bar = row.removeFromLeft (row.getWidth() - 52).reduced (0, 5);
        g.setColour (juce::Colour (0xff1e1612)); g.fillRect (bar);
        g.setColour (fixOn ? pal::signal : pal::paintDim.withAlpha (0.4f)); g.fillRect (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, v)));
        g.setColour (pal::paint); g.drawText (txt, row.toNearestInt(), juce::Justification::centredRight);
    };
    if (pluginMode)
        meter ("SAFETY LIM", limGr / 8.0f, "-" + juce::String (limGr, 1) + " dB");
    else
    {
        meter ("LIMITER", limGr / 8.0f, "-" + juce::String (limGr, 1) + " dB");
        meter ("PEAK TAMER", tameGr / 6.0f, "-" + juce::String (tameGr, 1) + " dB");
        meter ("LEVELER", std::abs (levDb) / 6.0f, (levDb >= 0 ? "+" : "") + juce::String (levDb, 1) + " dB");
    }

    if (! cantFix.isEmpty())
    {
        r.removeFromTop (6);
        g.setColour (pal::signalHot); g.setFont (tex::stencil (12.5f).withExtraKerningFactor (0.06f));
        g.drawText ("FIX THESE IN THE MIX", r.removeFromTop (16).toNearestInt(), juce::Justification::centredLeft);
        g.setColour (pal::paint.withAlpha (0.85f)); g.setFont (bodyFont (11.0f));
        g.drawFittedText ("- " + cantFix.joinIntoString ("\n- "), r.toNearestInt(), juce::Justification::topLeft, 8, 0.9f);
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
    g.drawText (juce::String (juce::roundToInt (p1 * 100.0f)) + "% SURE", top.withTrimmedBottom (top.getHeight() * 0.55f).toNearestInt(), juce::Justification::topRight);
    g.setColour (pal::paintDim); g.setFont (ledFont (11.5f));
    juce::String line = second.isNotEmpty() ? "GENRE GUESS  //  NEXT: " + second.toUpperCase() + " " + juce::String (juce::roundToInt (p2 * 100.0f)) + "%" : juce::String();
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
    const bool big = message.length() > 220 || buttons.size() > 2;
    panel = getLocalBounds().withSizeKeepingCentre (big ? 560 : 460, big ? 230 : 200);
    auto r = panel.reduced (22);
    auto row = r.removeFromBottom (28);
    const int bw = buttons.size() > 2 ? 136 : 100;
    for (int i = (int) buttons.size() - 1; i >= 0; --i) { buttons[i]->setBounds (row.removeFromRight (bw)); row.removeFromRight (8); }
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
    g.drawFittedText (message, r.withTrimmedBottom (36), juce::Justification::topLeft, 12);
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
    resetButton.setTooltip ("Start again: forget everything heard so far. With a prescription in place it asks whether to start over from scratch "
                            "(plugins back to neutral) or just measure again.");
    resetButton.onClick = [this]
    {
        if (proc.getChainStage() == 0) { proc.resetAnalysis(); reportAfter = false; reportSel.setIndex (0); lastSnap = -1; setPage (0); return; }
        juce::Component::SafePointer<MasterEditor> safe (this);
        dialog = std::make_unique<MasterDialog> ("RESET",
            "START OVER: every Mix System plugin goes back to neutral and the prescription is cleared, so you begin again at LISTEN.\n\n"
            "MEASURE AGAIN: your settings stay; The Master just forgets what it heard, so the next play-through scores the result (and REFINE works from it).",
            juce::StringArray { "START OVER", "MEASURE AGAIN", "CANCEL" }, [safe] (int i)
            {
                juce::MessageManager::callAsync ([safe, i]
                {
                    if (safe == nullptr) return;
                    safe->dialog.reset();
                    if (i == 0) safe->startOverNow();
                    else if (i == 1) { safe->proc.resetAnalysis(); safe->lastSnap = -1; }
                });
            });
        addAndMakeVisible (*dialog);
        dialog->setBounds (getLocalBounds());
    };
    profilesButton.setTooltip ("Genre profiles: import an updated profile pack, open the profiles folder, or go back to the factory profiles. (Hold SHIFT for the admin tools.)");
    profilesButton.onClick = [this] { showProfilesMenu(); };
    copyButton.setTooltip ("Copy the whole report as text, to paste into notes or send to someone.");
    copyButton.onClick = [this] { copyReport(); };
    diagButton.setTooltip ("Save one text file with the report, the prescription, the change log and every linked plugin's settings and measurements: "
                           "everything needed to fine-tune the system.");
    diagButton.onClick = [this] { exportDiagnostics(); };
    addAndMakeVisible (diagButton);
    for (auto* b : { &resetButton, &profilesButton }) addAndMakeVisible (*b);

    genreBar.setTooltip ("Which genre to compare your mix with. AUTO picks the closest match for you; the yellow lamp shows what your mix sounds like.");
    genreBar.onPick = [this] (const juce::String& id)
    {
        if (id == "__mine" || proc.getLibrary().isCustom (id)) { showMyProfilesMenu(); return; }
        proc.setGenreChoice (id); refreshGenres(); lastSnap = -1;
    };
    addAndMakeVisible (genreBar);
    addAndMakeVisible (soundsLike);
    soundsLike.setInterceptsMouseClicks (false, false);

    stepBar.setTooltip ("The way through: listen to your song, read the report, then The Master prescribes your plugins, sets them and fine-tunes. "
                        "Click any step you've reached to look at it again.");
    stepBar.onPick = [this] (int i)
    {
        if (i <= 1) { setPage (0); return; }
        if (proc.getChainStage() == 0) { runNextButton(); return; }   // PRESCRIPTION before there is one = PRESCRIBE
        setPage (1);
    };
    addAndMakeVisible (stepBar);
    reportSel.setTooltip ("Report on your mix as it comes in, or on what comes out after the fixes.");
    reportSel.onChange = [this] (int i) { reportAfter = i == 1; lastSnap = -1; };
    addAndMakeVisible (reportSel);

    // ---- ANALYZE page
    for (auto* c : std::initializer_list<juce::Component*> { &chart, &vitals, &score })
        c->setInterceptsMouseClicks (false, false);
    viewport.setViewedComponent (&verdict, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    analyzeComps = { &chart, &vitals, &score, &viewport, &copyButton };
    for (auto* c : analyzeComps) addChildComponent (*c);

    // ---- FIX IT page
    autoFixButton.setName ("autofix");
    autoFixButton.setTooltip ("Sets every fix below from what The Master heard, aiming at the genre. Play the whole song first. Press it again after another play-through to fine-tune the loudness.");
    autoFixButton.onClick = [this] { if (proc.isPluginMode()) runChainButton(); else runAutoFix(); };
    fixOnButton.setTooltip ("Turns all the fixes on or off. Flip it to compare with your original mix.");
    fixOnAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "fixOn", fixOnButton);
    matchButton.setTooltip ("Plays the fixed version back at the same loudness as your mix, so you judge the tone and punch, not just 'louder'. Turn it off before you bounce!");
    matchAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "match", matchButton);
    amountKnob = &makeKnob ("amount", "AMOUNT", "How much of the EQ, leveler, peak tamer and width moves to use. 100 = as set.");

    auto& eqS = makeSection (MasterProcessor::secEq, "eqOn", "Tone match EQ on/off.");
    eqS.why.setText ("", juce::dontSendNotification);
    for (int b = 0; b < mst::kNumBands; ++b)
    {
        auto sl = std::make_unique<juce::Slider> (juce::Slider::LinearVertical, juce::Slider::TextBoxBelow);
        sl->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 54, 16);
        sl->setTooltip (juce::String (mst::bandDefs()[(size_t) b].label) + " (" + mst::bandDefs()[(size_t) b].range + " Hz). Yellow arrows: what AUTO FIX chose. Drag to override, double-click for 0 dB.");
        sl->setDoubleClickReturnValue (true, 0.0);
        sl->getProperties().set ("neutral", 0.0);
        eqAtts[(size_t) b] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (p.apvts, "eq" + juce::String (b), *sl);
        sl->textFromValueFunction = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1); };
        addChildComponent (*sl);
        fixComps.push_back (sl.get());
        eqSliders[(size_t) b] = std::move (sl);
    }

    makeSection (MasterProcessor::secDyn, "dynOn", "Leveler + peak tamer on/off.");
    levelerKnob = &makeKnob ("leveler", "LEVELER", "Rides the level slowly: lifts quiet sections toward the song's average, turns sudden loud entrances down. For big level swings between verse and chorus.");
    tameKnob = &makeKnob ("glue", "TAME PEAKS", "Look-ahead limiting of the peaks (usually kick and snare) that stick out of the mix. The threshold follows the music. Lowers PLR so the mix can get louder without sounding squashed.");

    makeSection (MasterProcessor::secStereo, "stereoOn", "Stereo fixes on/off.");
    widthKnob = &makeKnob ("width", "WIDTH", "Width above 120 Hz. 100 = unchanged. The low end is never widened.");
    widthKnob->slider.getProperties().set ("neutral", 100.0);
    bassMonoButton.setTooltip ("Makes everything below 120 Hz mono, so kick and bass sit dead centre.");
    bassMonoAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "bassMono", bassMonoButton);

    makeSection (MasterProcessor::secHum, "humOn", "Hum notches on/off.");
    humSel = std::make_unique<Selector> (p.apvts.getParameter ("humHz"), juce::StringArray { "50 Hz", "60 Hz" }, 15.0f);
    humSel->setTooltip ("Mains hum is 60 Hz in North America, 50 Hz in most of the rest of the world. AUTO FIX picks the one it heard.");

    makeSection (MasterProcessor::secLoud, "loudOn", "Loudness + limiter on/off.");
    gainKnob = &makeKnob ("gain", "GAIN", "How hard the mix is pushed into the true-peak limiter. AUTO FIX aims for the genre's loudness.");
    gainKnob->slider.getProperties().set ("neutral", 0.0);
    ceilSel = std::make_unique<Selector> (p.apvts.getParameter ("ceiling"), juce::StringArray { "-1.0", "-2.0", "-0.3", "-6.0" }, 14.0f);
    ceilSel->setTooltip ("True-peak ceiling. -1.0 dBTP: streaming (Spotify, Apple Music, YouTube). -2.0: extra safe for MP3/AAC. -0.3: CD / download masters. -6.0: headroom for a separate mastering step.");
    outSel = std::make_unique<Selector> (p.apvts.getParameter ("headroom"), juce::StringArray { "-6 dB", "-1 dB" }, 14.0f);
    outSel->setTooltip ("OUTPUT TARGET. -6 dB: peaks held under -6 dB with no loudness push, leaving headroom for a separate mastering step. "
                        "-1 dB: a finished master, as loud as your genre, peaks at -1 dBTP for streaming.");
    outSel->onChange = [this] (int) { proc.outputTargetChanged(); refreshNow(); };
    addChildComponent (*outSel);
    outLabel.setText ("OUTPUT", juce::dontSendNotification);
    outLabel.setFont (tex::stencil (14.0f).withExtraKerningFactor (0.1f));
    outLabel.setColour (juce::Label::textColourId, pal::paint);
    outLabel.setTooltip (outSel->getTooltip());
    addChildComponent (outLabel);

    for (auto* c : std::initializer_list<juce::Component*> { &autoFixButton, &fixOnButton, &matchButton, &bassMonoButton, humSel.get(), ceilSel.get(), &fixInfo })
    {
        addChildComponent (*c);
        fixComps.push_back (c);
    }
    fixInfo.setInterceptsMouseClicks (false, false);
    for (auto& k : knobs) { fixComps.push_back (&k->slider); fixComps.push_back (&k->label); }
    for (auto& sct : sections) { fixComps.push_back (&sct->on); fixComps.push_back (&sct->autoBtn); fixComps.push_back (&sct->why); }

    // ---- MY PLUGINS first; The Master's own built-in fixes are the alternative
    builtInButton.setTooltip ("No Castle Danger plugins on the bus? The Master can fix the mix itself with its own built-in EQ, dynamics, stereo, hum and loudness tools.");
    builtInButton.onClick = [this]
    {
        proc.setPluginMode (! proc.isPluginMode());
        lastSnap = -1; lastChainSig = -1; updateVisibility(); buildBackdrop(); repaint();
    };
    addChildComponent (builtInButton);
    nextButton.setName ("autofix");   // the hot, glowing style: the way forward
    nextButton.setTooltip ("Go to the fix: The Master prescribes your Castle Danger plugins right away, then sets them for you.");
    nextButton.onClick = [this] { runNextButton(); };
    addAndMakeVisible (nextButton);
    stepList.setInterceptsMouseClicks (false, false);
    newRxButton.setTooltip ("Start over after big changes to the mix: every Mix System plugin goes back to neutral, then play the song and press PRESCRIBE. "
                            "(For plugins you've added or removed, the main button says UPDATE: no need to start over.)");
    newRxButton.onClick = [this] { startOverNow(); };
    chainView.setInterceptsMouseClicks (false, true);   // the YOUR TRACKS list scrolls
    logViewport.setViewedComponent (&changeLog, false);
    logViewport.setScrollBarsShown (true, false);
    logViewport.setScrollBarThickness (10);
    chainComps = { &chainView, &logViewport, &newRxButton, &stepList };
    for (auto* c : chainComps) addChildComponent (*c);

    refreshGenres();
    setSize (1100, 744);
    setPage (0);
    refreshNow();
    startTimerHz (15);
}

MasterEditor::~MasterEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

MasterEditor::Knob& MasterEditor::makeKnob (const juce::String& id, const juce::String& text, const juce::String& tip)
{
    auto k = std::make_unique<Knob>();
    k->label.setText (text, juce::dontSendNotification);
    k->label.setJustificationType (juce::Justification::centred);
    k->label.setFont (tex::stencil (14.0f).withExtraKerningFactor (0.08f));
    k->label.setColour (juce::Label::textColourId, pal::paint);
    k->label.setMinimumHorizontalScale (0.6f);
    k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 17);
    k->slider.setMouseDragSensitivity (220);
    k->slider.setTooltip (tip);
    k->att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k->slider);
    auto* prm = proc.apvts.getParameter (id);
    k->slider.setDoubleClickReturnValue (true, prm->convertFrom0to1 (prm->getDefaultValue()));
    addChildComponent (k->slider);
    addChildComponent (k->label);
    knobs.push_back (std::move (k));
    return *knobs.back();
}

void MasterEditor::placeKnob (Knob& k, juce::Rectangle<int> cell)
{
    k.label.setBounds (cell.removeFromTop (18));
    k.slider.setBounds (cell);
}

MasterEditor::FixSection& MasterEditor::makeSection (MasterProcessor::Section id, const juce::String& onParam, const juce::String& tip)
{
    auto sct = std::make_unique<FixSection>();
    sct->id = id;
    sct->on.setTooltip (tip);
    sct->onAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, onParam, sct->on);
    sct->autoBtn.setTooltip ("Yellow = you've changed this section. Click to put it back to what AUTO FIX chose.");
    sct->autoBtn.onClick = [this, id] { proc.revertToAuto (id); };
    sct->why.setFont (bodyFont (11.5f));
    sct->why.setColour (juce::Label::textColourId, pal::paintDim);
    sct->why.setJustificationType (juce::Justification::topLeft);
    sct->why.setMinimumHorizontalScale (1.0f);
    sct->why.setInterceptsMouseClicks (false, false);
    for (auto* c : std::initializer_list<juce::Component*> { &sct->on, &sct->autoBtn, &sct->why }) addChildComponent (*c);
    sections.push_back (std::move (sct));
    return *sections.back();
}

void MasterEditor::setPage (int pg)
{
    page = pg;
    stepBar.page = pg;
    updateSteps();
    updateVisibility();
    buildBackdrop();
    repaint();
}

void MasterEditor::updateVisibility()
{
    if (! proc.isPluginMode()) proc.setPluginMode (true);   // built-in fixes are retired: the plugins do the work
    const bool chainMode = true;
    for (auto* c : analyzeComps) c->setVisible (page == 0);
    for (auto* c : fixComps) c->setVisible (page == 1 && ! chainMode);
    for (auto* c : chainComps) c->setVisible (page == 1 && chainMode);
    reportSel.setVisible (page == 0);
    fixInfo.pluginMode = chainMode;
    builtInButton.setVisible (false);
    diagButton.setBounds (page == 0 ? juce::Rectangle<int> (868, 286, 148, 20) : juce::Rectangle<int> (920, 530, 156, 20));
    autoFixButton.setVisible (false);   // the one button up top does every step now
    outSel->setVisible (page == 1);
    outLabel.setVisible (page == 1);
    fixInfo.setVisible (page == 1);
    newRxButton.setVisible (page == 1 && chainMode && proc.getChainStage() >= 1);
    lastOutdated = proc.chainOutdated();
    updateSteps();
    nextButton.setTooltip (page == 0 ? juce::String ("The next step. After you've played the song: PRESCRIBE, where The Master writes the prescription for your Castle Danger plugins.")
                         : lastOutdated ? "You've added or removed plugins on your tracks since the prescription. UPDATE plans them in from the same analysis"
                                             + juce::String (proc.getChainStage() >= 2 ? " and sends the new settings." : ".")
                                           : juce::String ("Does the next step: PRESCRIBE, then APPLY, then REFINE."));
}

juce::String MasterEditor::chainButtonText() const
{
    const int st = proc.getChainStage();
    if (st == 0) return "PRESCRIBE";
    if (proc.chainOutdated()) return "UPDATE";
    return st == 1 ? "APPLY" : "REFINE";
}

void MasterEditor::startOverNow()
{
    proc.startOver();
    lastChainSig = -1; lastSnap = -1; lastLinkVersion = -1;
    reportAfter = false; reportSel.setIndex (0);
    changeLog.setEntries (proc.getLog(), logViewport.getWidth() - 12, "Nothing changed yet.");
    setPage (0);
    showMessage ("STARTING OVER", "Every Mix System plugin is back to neutral and the prescription is cleared, so The Master hears your mix as it really is. "
                                  "Play the song from the top (the whole song is best), then press PRESCRIBE.");
}

void MasterEditor::updateSteps()
{
    const int stage = proc.getChainStage();
    bool fromSaved = false;
    const auto heard = proc.getDisplayFeatures (fromSaved);
    bool allLinked = stage >= 1;
    if (stage == 1) for (auto& st : proc.chainStatus()) allLinked = allLinked && st.linked;
    const int now = stage == 0 ? (heard.valid && heard.seconds >= 20.0 ? 1 : 0) : stage == 1 ? (allLinked ? 3 : 2) : 4;
    const int open = stage == 0 ? 2 : 4;
    juce::String label;
    if (page == 0) label = stage == 0 && now == 0 ? "PLAY YOUR SONG" : "PRESCRIBE  >";
    else label = chainButtonText();
    if (stepBar.now != now || stepBar.open != open || stepBar.page != page) { stepBar.now = now; stepBar.open = open; stepBar.page = page; stepBar.repaint(); }
    if (nextButton.getButtonText() != label) nextButton.setButtonText (label);
}

void MasterEditor::runNextButton()
{
    if (page == 1) { runChainButton(); return; }
    const int stage = proc.getChainStage();
    bool fromSaved = false;
    const auto heard = proc.getDisplayFeatures (fromSaved);
    if (stage == 0 && ! (heard.valid && heard.seconds >= 20.0))
    {
        showMessage ("PLAY YOUR SONG", "Put The Servant on every track first (it sets its ROLE from the track name; check it), so it listens along. Then press RESET and play your song "
                                       "from the top (the whole song is best). The report fills in as it plays; "
                                       "after 20 seconds this button says PRESCRIBE.");
        return;
    }
    setPage (1);
    if (stage == 0 && ! proc.isWaitingForServants())
    {
        juce::Component::SafePointer<MasterEditor> safe (this);
        proc.requestPrescribe ([safe] (const juce::String& e)
        {
            if (safe == nullptr) return;
            if (e.isNotEmpty()) { safe->showMessage ("NOT YET", e); if (safe->proc.getChainStage() == 0) safe->setPage (0); }
            safe->lastSnap = -1; safe->lastChainSig = -1; safe->updateVisibility();
        });
    }
    refreshNow();
}

void MasterEditor::runChainButton()
{
    juce::String err;
    const int stage = proc.getChainStage();
    if (stage == 0)
    {
        if (proc.isWaitingForServants()) return;
        juce::Component::SafePointer<MasterEditor> safe (this);
        proc.requestPrescribe ([safe] (const juce::String& e)   // the listening Servants finish first
        {
            if (safe == nullptr) return;
            if (e.isNotEmpty()) safe->showMessage ("NOT YET", e);
            safe->lastSnap = -1; safe->lastChainSig = -1;
        });
        return;
    }
    if (proc.chainOutdated())
    {
        if (proc.isWaitingForServants()) return;
        juce::Component::SafePointer<MasterEditor> safe (this);
        proc.requestUpdate ([safe, stage] (const juce::String& e)
        {
            if (safe == nullptr) return;
            if (e.isNotEmpty()) safe->showMessage ("NOT YET", e);
            else if (stage >= 2) { safe->reportAfter = true; safe->reportSel.setIndex (1); }
            safe->lastSnap = -1; safe->lastChainSig = -1; safe->updateVisibility();
        });
        return;
    }
    if (stage == 1) err = proc.applyChain();
    else err = proc.refineChain();
    if (err.isNotEmpty()) { showMessage ("NOT YET", err); return; }
    if (stage >= 1) { reportAfter = true; reportSel.setIndex (1); }
    lastSnap = -1; lastChainSig = -1;
}

void MasterEditor::runAutoFix()
{
    const auto err = proc.applyAutoFix();
    if (err.isNotEmpty()) { showMessage ("NOT YET", err); return; }
    reportAfter = true;
    reportSel.setIndex (1);
    lastSnap = -1;
}

void MasterEditor::resized()
{
    for (auto& b : backdropCache) b = {};
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

    genreBar.setBounds (26, 162, 692, 54);
    soundsLike.setBounds (750, 162, 322, 54);
    tabsArea = { 16, 232, 1068, 42 };
    stepBar.setBounds (24, 237, 800, 32);
    nextButton.setBounds (834, 237, 242, 32);
    reportSel.setBounds (440, 285, 204, 22);
    builtInButton.setBounds (856, 239, 220, 28);
    chainView.setBounds (300, 308, 772, 198);
    logViewport.setBounds (300, 552, 772, 170);
    changeLog.setEntries (proc.getLog(), logViewport.getWidth() - 12, {});
    stepList.setBounds (28, 350, 238, 204);
    newRxButton.setBounds (936, 284, 140, 20);

    // ANALYZE
    chart.setBounds (28, 306, 616, 228);
    vitals.setBounds (28, 578, 616, 144);
    copyButton.setBounds (1022, 286, 54, 20);
    diagButton.setBounds (page == 0 ? juce::Rectangle<int> (868, 286, 148, 20) : juce::Rectangle<int> (920, 530, 156, 20));
    score.setBounds (678, 306, 394, 74);
    viewport.setBounds (678, 388, 394, 334);
    verdict.setItems (report.items, viewport.getWidth() - 12);

    layoutFixPage();
    buildBackdrop();
}

void MasterEditor::layoutFixPage()
{
    // left column: THE CURE
    outLabel.setBounds (34, 309, 66, 24);
    outSel->setBounds (100, 307, 166, 28);
    autoFixButton.setBounds (28, 339, 238, 41);
    fixOnButton.setBounds (28, 382, 238, 30);
    matchButton.setBounds (28, 416, 238, 30);
    placeKnob (*amountKnob, { 28, 452, 238, 110 });
    fixInfo.setBounds (28, 568, 238, 154);

    auto header = [] (juce::Rectangle<int> plate, FixSection& s)
    {
        s.plate = plate;
        if (plate.getWidth() > 400)
        {
            s.on.setBounds (plate.getRight() - 72, plate.getY() + 4, 64, 22);
            s.autoBtn.setBounds (plate.getRight() - 132, plate.getY() + 5, 54, 20);
        }
        else
        {   // small plates: title on top, AUTO + ON underneath
            s.autoBtn.setBounds (plate.getX() + 12, plate.getY() + 26, 60, 22);
            s.on.setBounds (plate.getRight() - 76, plate.getY() + 25, 64, 24);
        }
    };
    // EQ
    const juce::Rectangle<int> eqPlate { 288, 282, 796, 236 };
    header (eqPlate, *sections[0]);
    const float cw = 772.0f / (float) mst::kNumBands;
    for (int b = 0; b < mst::kNumBands; ++b)
        eqSliders[(size_t) b]->setBounds (300 + (int) (cw * (float) b), 330, (int) cw, 150);
    sections[0]->why.setBounds (300, 494, 772, 20);

    // bottom row
    const int y = 528, h = 204, w = 191;
    const std::array<int, 4> xs { 288, 490, 692, 893 };
    header ({ xs[0], y, w, h }, *sections[1]);
    placeKnob (*levelerKnob, { xs[0] + 8, y + 54, 86, 96 });
    placeKnob (*tameKnob, { xs[0] + 97, y + 54, 86, 96 });
    sections[1]->why.setBounds (xs[0] + 10, y + 152, w - 20, 48);

    header ({ xs[1], y, w, h }, *sections[2]);
    placeKnob (*widthKnob, { xs[1] + 6, y + 54, 86, 96 });
    bassMonoButton.setBounds (xs[1] + 94, y + 88, 90, 30);
    sections[2]->why.setBounds (xs[1] + 10, y + 152, w - 20, 48);

    header ({ xs[2], y, w, h }, *sections[3]);
    humSel->setBounds (xs[2] + 12, y + 64, w - 24, 40);
    sections[3]->why.setBounds (xs[2] + 10, y + 114, w - 20, 86);

    header ({ xs[3], y, w, h }, *sections[4]);
    placeKnob (*gainKnob, { xs[3] + 6, y + 54, 86, 96 });
    ceilSel->setBounds (xs[3] + 96, y + 88, 88, 30);
    sections[4]->why.setBounds (xs[3] + 10, y + 152, w - 20, 48);
}

//==============================================================================
void MasterEditor::refreshGenres()
{
    proc.refreshSongProfiles();
    const auto& lib = proc.getLibrary();
    juce::StringArray ids { "auto" }, names { "AUTO|CLOSEST MATCH" };
    for (int i = 0; i < lib.numBuiltIn(); ++i)
    {
        const auto& g = lib.genres()[(size_t) i];
        ids.add (g.id);
        names.add (g.name.toUpperCase() + "|" + (g.tracks > 0 ? juce::String (g.tracks) + " REFERENCE SONGS" : juce::String ("FACTORY")));
    }
    auto choice = proc.getGenreChoice();
    if (choice != "auto" && lib.indexOf (choice) < 0) choice = "auto";
    // the last button: your own profiles (a folder of songs each)
    const int nMine = (int) lib.genres().size() - lib.numBuiltIn();
    if (lib.isCustom (choice))
    {
        const auto& g = lib.genres()[(size_t) lib.indexOf (choice)];
        ids.add (choice);
        names.add (g.name.toUpperCase() + "|MY PROFILE  //  " + juce::String (g.tracks) + (g.tracks == 1 ? " SONG" : " SONGS"));
    }
    else
    {
        ids.add ("__mine");
        names.add ("MY PROFILES|" + (nMine > 0 ? juce::String (nMine) + (nMine == 1 ? " PROFILE" : " PROFILES") : juce::String ("FROM YOUR SONGS")));
    }
    genreBar.setGenres (ids, names);
    genreBar.setSelected (choice);
    lastLib = proc.getLibrary().version.load();
}

void MasterEditor::updateReport()
{
    bool saved = false;
    const auto pre = proc.getDisplayFeatures (saved);
    const auto post = proc.getAfterFeatures();
    // the genre is always judged from your mix as it comes in
    const auto target = proc.resolveTarget (pre);
    const auto& genres = proc.getLibrary().genres();
    if (target.index < 0 || target.index >= (int) genres.size()) return;
    const auto& gp = genres[(size_t) target.index];
    const auto fx = reportAfter ? post : pre;
    if (reportAfter) saved = false;
    report = mst::advise (fx, gp, proc.headroomMode());
    // set-up first: The Servants have to hear the same play-through as The Master, so they go on before you play
    if (proc.getChainStage() == 0 && ! reportAfter)
    {
        int servants = 0;
        for (auto& pr : proc.getPeers()) if (pr.kind == "servant") ++servants;
        mst::Suggestion su;
        su.sev = mst::Sev::info; su.area = "SET-UP";
        if (servants == 0)
        {
            su.title = "PUT THE SERVANT ON EVERY TRACK";
            su.body = "Before you play the song: The Master on the master bus (last), The Servant on every track (each sets its ROLE from the track name: check them). "
                      "The Servants listen along with The Master, so one play-through is enough for the whole prescription.";
            su.plugin = "THE SERVANT"; su.tip = "Insert it on each track and check its ROLE (it reads the track name), then press RESET here and play the song from the top.";
        }
        else
        {
            su.title = juce::String (servants) + (servants == 1 ? " SERVANT LISTENING" : " SERVANTS LISTENING");
            su.body = "Every track with The Servant is being heard along with the mix. Missing one? Add it now, then RESET and play again.";
        }
        report.items.insert (report.items.begin(), su);
    }

    // FIX IT page: scores, the plan's reasons, AUTO markers
    {
        fixInfo.before = pre.valid ? mst::advise (pre, gp, proc.headroomMode()).score : -1;
        fixInfo.after = post.valid && proc.isFixOn() ? mst::advise (post, gp, proc.headroomMode()).score : -1;
        const auto& plan = proc.getPlan();
        fixInfo.cantFix = plan.valid ? plan.cantFix : juce::StringArray();
        if (! plan.valid) fixInfo.setFrom = pre.valid ? "Press AUTO FIX to correct what The Master heard." : "Play the whole song, then press AUTO FIX.";
        else fixInfo.setFrom = "Set from " + mmss (plan.seconds) + " of your mix, aiming at " + plan.genreName.toUpperCase() + "."
                             + (post.valid && proc.isFixOn() ? juce::String() : juce::String ("  Play it again to score the result."));
        fixInfo.repaint();
        const juce::String none = "Press AUTO FIX, or set this section yourself.";
        sections[0]->why.setText (plan.valid ? plan.whyEq : none, juce::dontSendNotification);
        sections[1]->why.setText (plan.valid ? plan.whyDyn : none, juce::dontSendNotification);
        sections[2]->why.setText (plan.valid ? plan.whyStereo : none, juce::dontSendNotification);
        sections[3]->why.setText (plan.valid ? plan.whyHum : none, juce::dontSendNotification);
        sections[4]->why.setText (plan.valid ? plan.whyLoud : none, juce::dontSendNotification);
        auto mark = [&] (juce::Slider& sl, double v) { if (plan.valid) sl.getProperties().set ("auto", v); else sl.getProperties().remove ("auto"); sl.repaint(); };
        for (int b = 0; b < mst::kNumBands; ++b) mark (*eqSliders[(size_t) b], plan.p.eq[(size_t) b]);
        mark (levelerKnob->slider, plan.p.leveler * 100.0);
        mark (tameKnob->slider, plan.p.glue * 100.0);
        mark (widthKnob->slider, plan.p.width * 100.0);
        mark (gainKnob->slider, plan.p.gainDb);

        if (proc.isPluginMode())
        {
            const auto& cp = proc.getChainPlan();
            const int stage = proc.getChainStage();
            fixInfo.before = cp.valid ? mst::advise (cp.before, gp, proc.headroomMode()).score : (pre.valid ? mst::advise (pre, gp, proc.headroomMode()).score : -1);
            fixInfo.after = stage >= 2 && post.valid ? mst::advise (post, gp, proc.headroomMode()).score : -1;
            fixInfo.cantFix = cp.valid ? cp.cantFix : juce::StringArray();
            fixInfo.setFrom = stage == 0 ? (pre.valid ? "Ready: press PRESCRIBE." : "Play the whole song first.")
                            : "Prescription from " + mmss (cp.seconds) + " of your mix, aiming at " + cp.genreName.toUpperCase() + "."
                              + (stage >= 2 && ! post.valid ? juce::String ("  Play it again to score the result.") : juce::String());
            fixInfo.repaint();
        }
    }

    // detector
    soundsLike.ready = pre.valid && ! target.guesses.empty();   // the genre is about your mix, not the fixed output
    soundsLike.locked = ! target.automatic;
    soundsLike.lockedName = gp.name;
    if (soundsLike.ready)
    {
        soundsLike.first = genres[(size_t) target.guesses[0].index].name;
        soundsLike.p1 = target.guesses[0].probability;
        soundsLike.second = target.guesses.size() > 1 ? genres[(size_t) target.guesses[1].index].name : juce::String();
        soundsLike.p2 = target.guesses.size() > 1 ? target.guesses[1].probability : 0.0f;
    {
        const auto det = genres[(size_t) target.guesses[0].index].id;
        genreBar.setDetected (proc.getLibrary().isCustom (det) && det != proc.getGenreChoice() ? juce::String ("__mine") : det);
    }
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
    const auto t = mmss (pre.seconds);
    if (saved) statusText = "SAVED REPORT (" + t + ")  //  PLAY TO START A NEW ONE";
    else if (proc.getWorker().getSampleRate() <= 0) statusText = "WAITING FOR THE HOST TO START AUDIO";
    else statusText = juce::String (proc.isListening() ? "LISTENING" : "PAUSED") + "  //  " + t + " ANALYSED";
    repaint (statusArea);
}

void MasterEditor::timerCallback()
{
    {   // the step bar and the next-step button follow what's happening (a few times a second is plenty)
        static thread_local juce::uint32 lastSteps = 0;
        const auto nowMs = juce::Time::getMillisecondCounter();
        if (nowMs - lastSteps > 250 || lastSnap == -1) { lastSteps = nowMs; updateSteps(); }
    }
    if (proc.getLibrary().version.load() != lastLib) { refreshGenres(); lastSnap = -1; }
    const int snap = proc.getWorker().getSnapshotVersion() * 7919 + proc.getPostWorker().getSnapshotVersion();
    const auto choice = proc.getGenreChoice();
    if (snap != lastSnap || choice != lastGenre)
    {
        lastSnap = snap; lastGenre = choice;
        genreBar.setSelected (proc.getLibrary().indexOf (choice) >= 0 ? choice : juce::String ("auto"));
        updateReport();
    }

    // MY PLUGINS: chain status, the change log, what to do next
    if (page == 1 && proc.isPluginMode())
    {
        const int sig = proc.linkVersion.load() * 31 + proc.getChainStage() * 7 + (int) (juce::Time::getMillisecondCounter() / 700);
        if (sig != lastChainSig)
        {
            lastChainSig = sig;
            const auto& cp = proc.getChainPlan();
            chainView.hasPlan = cp.valid && proc.getChainStage() >= 1;
            chainView.steps = proc.chainStatus();
            chainView.tracks = proc.trackStatus();
            chainView.onTracks.clear();
            for (auto& l : cp.onTracks)   // per-track plugins now live in YOUR TRACKS; keep only general notes here
                if (! (l.startsWith ("DEAD AIR") || l.startsWith ("FLATLINER") || l.startsWith ("LOW BLOW") || l.startsWith ("THE SERVANT")))
                    chainView.onTracks.add (l);
            chainView.stage = proc.getChainStage();
            chainView.rebuildTracks (proc.getChainStage() >= 1 ? cp.adds : std::vector<mst::ChainPlan::TrackAdd>());
            chainView.orderKnown = proc.chainOrderKnown();
            chainView.stage = proc.getChainStage();
            chainView.repaint();
            if (proc.linkVersion.load() != lastLinkVersion)
            {
                lastLinkVersion = proc.linkVersion.load();
                changeLog.setEntries (proc.getLog(), logViewport.getWidth() - 12,
                                      "Nothing changed yet. Every setting The Master sends to your plugins is listed here, with the reason.");
            }
            const int stage = proc.getChainStage();
            bool allLinked = ! chainView.steps.empty(), inOrder = true;
            int linked = 0;
            for (auto& st : chainView.steps) { if (st.linked) ++linked; else allLinked = false; if (! st.orderOk) inOrder = false; }
            bool fromSaved = false;
            const auto heard = proc.getDisplayFeatures (fromSaved);
            const auto t = mmss (heard.seconds);
            juce::StringArray d;
            d.add ("The Servant on every track (check each ROLE), The Master last on the master bus. Then RESET and play your song from the top ("
                   + t + " heard so far). The whole song is best.");
            d.add (t + " heard. Press PRESCRIBE (top right) and The Master picks your plugins and every setting.");
            d.add ("Put the plugins where the lists show: the master bus in that order, and the tracks under YOUR TRACKS. "
                   + juce::String (linked) + " of " + juce::String ((int) chainView.steps.size()) + " bus plugins linked"
                   + (allLinked ? (chainView.orderKnown ? (inOrder ? ", in order." : ". ORDER WRONG: move them to match.") : ". Play a few seconds to check the order.") : juce::String (".")));
            d.add ("Everything is in place. Press APPLY (top right): The Master sets every knob and logs the reason below.");
            d.add ("Done: every change is in the log. Play the song through again, then press REFINE (top right) to fine-tune from what came out. "
                   "Big changes to the mix? START OVER.");
            stepList.details = d;
            stepList.current = stage == 0 ? (heard.seconds >= 20.0 ? 1 : 0) : stage == 1 ? (allLinked ? 3 : 2) : 4;
            const bool outdated = proc.chainOutdated();
            if (outdated) { d.set (stepList.current, "Plugins added or removed on your tracks, or one has now heard the song: press UPDATE (top right). "
                                                    "The Master plans them in from the same analysis."); stepList.details = d; }
            if (outdated != lastOutdated || nextButton.getButtonText() != chainButtonText()) updateVisibility();
            stepList.repaint();
            updateVisibility();
        }
    }

    // FIX IT: which sections you've overridden, live gain-reduction meters
    if (page == 1)
    {
        const bool planOk = proc.getPlan().valid;
        for (auto& sct : sections)
        {
            const bool ov = planOk && proc.isOverridden (sct->id);
            if ((bool) sct->autoBtn.getProperties()["overridden"] != ov) { sct->autoBtn.getProperties().set ("overridden", ov); sct->autoBtn.repaint(); }
            sct->autoBtn.setEnabled (planOk);
        }
        auto& ch = proc.getChain();
        fixInfo.limGr = std::max (ch.getLimiterGr(), fixInfo.limGr * 0.85f);
        fixInfo.tameGr = std::max (ch.getTamerGr(), fixInfo.tameGr * 0.85f);
        fixInfo.levDb = ch.getLevelerDb();
        fixInfo.fixOn = proc.isFixOn();
        fixInfo.repaint();
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
    juce::SystemClipboard::copyTextToClipboard (reportText());
    showMessage ("REPORT COPIED", "The full report is on your clipboard. Paste it into your notes, an email or a message.");
}

juce::String MasterEditor::reportText() const
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
    return s;
}

juce::String MasterEditor::diagnosticsText() const
{
    auto line = [] (const juce::String& t) { return "\n==============================================================================\n" + t + "\n==============================================================================\n"; };
    auto json = [] (const juce::var& v) { return juce::JSON::toString (v, true); };
    auto fmtMap = [] (const std::map<juce::String, float>& m) { juce::StringArray a; for (auto& [k, v] : m) a.add (k + "=" + juce::String (v, 2)); return a.joinIntoString (", "); };
    const auto& cp = proc.getChainPlan();
    const int stage = proc.getChainStage();
    juce::String s;
    s << "THE MASTER - DIAGNOSTICS (Castle Danger Music Company)\n";
    s << "Version " << JucePlugin_VersionString << "   " << juce::Time::getCurrentTime().toString (true, true) << "\n";
    s << "Host: " << juce::PluginHostType().getHostDescription() << "   " << juce::SystemStats::getOperatingSystemName()
      << "   " << juce::String (juce::roundToInt (proc.getSampleRate())) << " Hz\n";
    s << "OUTPUT: " << (proc.headroomMode() ? "-6 dB (pre-master)" : "-1 dB (finished master)") << "   Genre choice: " << proc.getGenreChoice()
      << "   Stage: " << (stage == 0 ? "not prescribed" : stage == 1 ? "prescribed" : "applied")
      << (proc.chainOutdated() ? " (UPDATE pending)" : "") << "\n";

    s << line ("REPORT (" + juce::String (reportAfter ? "AFTER FIXES" : "YOUR MIX") + ")") << reportText();
    {
        bool saved = false;
        const auto fx = proc.getDisplayFeatures (saved);
        s << "Measurements" << (saved ? " (from the saved project)" : "") << ": " << json (fx.toVar()) << "\n";
    }

    s << line ("PRESCRIPTION");
    if (! cp.valid) s << "(none yet)\n";
    else
    {
        s << "Aimed at " << cp.genreName << " from " << mmss (cp.seconds) << " of the mix" << (cp.headroom ? ", OUTPUT -6 dB" : ", OUTPUT -1 dB") << "\n";
        s << "Your mix as it was measured for the prescription (before any fixes): LUFS " << juce::String (cp.before.lufs, 1) << ", true peak "
          << juce::String (cp.before.truePeak, 1) << " dBTP, PLR " << juce::String (cp.before.plr, 1) << " dB, LRA " << juce::String (cp.before.lra, 1)
          << " LU, width " << juce::String (cp.before.width, 1) << " dB\n  " << json (cp.before.toVar()) << "\n\nMASTER BUS\n";
        int i = 1;
        for (auto& st : cp.steps) s << "  " << i++ << " " << st.name << ": " << st.role << "\n     " << fmtMap (st.params) << "\n";
        s << "  " << i << " The Master (last): tone EQ " << (cp.masterEq ? "on" : "off") << " + safety limiter\n\nPLANNED CHANGES\n";
        for (auto& c : cp.changes) s << "  [" << mst::plugDisplayName (c.plugin) << "] " << c.label << " -> " << juce::String (c.to, 2) << " " << c.unit << "   " << c.why << "\n";
        s << "\nTRACK PLANS\n";
        for (auto& t : cp.tracks)
        {
            s << "  " << mst::plugDisplayName (t.kind) << " on " << t.name << " (" << t.role << (t.share >= 0 ? ", " + juce::String (t.share * 100.0f, 1) + "% of the mix" : juce::String()) << "): " << t.summary() << "\n";
            s << "     " << fmtMap (t.params) << "\n";
            for (auto& w : t.why) s << "     - " << w << "\n";
        }
        if (! cp.adds.empty()) { s << "\nASKED TO ADD\n"; for (auto& a : cp.adds) s << "  " << mst::plugDisplayName (a.kind) << " on " << (a.track.isNotEmpty() ? a.track : juce::String ("guitar + bass tracks")) << ": " << a.why << "\n"; }
        if (! cp.onTracks.isEmpty()) s << "\nNOTES\n  " << cp.onTracks.joinIntoString ("\n  ") << "\n";
        if (cp.tracksNote.isNotEmpty() && ! cp.onTracks.contains (cp.tracksNote)) s << "  " << cp.tracksNote << "\n";
        if (! cp.cantFix.isEmpty()) s << "\nCAN'T FIX\n  " << cp.cantFix.joinIntoString ("\n  ") << "\n";
    }

    s << line ("MASTER BUS STATUS");
    for (auto& st : proc.chainStatus())
        s << "  " << st.name << ": " << (st.linked ? "linked" : "NOT FOUND") << ", position " << st.position << (st.orderOk ? ", order ok" : ", ORDER WRONG") << (st.rebel ? ", REBEL (hands off)" : "") << "\n";
    s << "  order known: " << (proc.chainOrderKnown() ? "yes" : "no (play a few seconds)") << "\n";

    s << line ("YOUR TRACKS");
    for (auto& t : proc.trackStatus())
        s << "  " << (t.name.isNotEmpty() ? t.name : juce::String ("(no name)")) << "  [" << mst::plugDisplayName (t.kind) << ", " << t.role << "]  "
          << (t.linked ? "linked" : "NOT LINKED") << (t.planned ? ", planned" : ", not in the plan") << "  " << t.summary << "\n";

    s << line ("WHAT I CHANGED & WHY");
    if (proc.getLog().empty()) s << "(nothing applied yet)\n";
    for (auto& e : proc.getLog())
        s << "  [" << e.plugin << "] " << e.title << (e.confirmed ? "  (confirmed)" : "  (not confirmed)") << "\n     " << e.detail.replace ("\n", "\n     ") << "\n";

    s << line ("EVERY LINKED PLUGIN: SETTINGS AND MEASUREMENTS");
    for (auto& p : proc.getPeers())
        s << "  " << mst::plugDisplayName (p.kind) << " " << p.version << "  id " << p.id << "\n     settings: " << json (p.params)
          << "\n     measured: " << json (p.extra) << "\n";
    return s;
}

void MasterEditor::exportDiagnostics()
{
    const auto name = "The Master diagnostics " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M") + ".txt";
    chooser = std::make_unique<juce::FileChooser> ("Export diagnostics", juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (name), "*.txt");
    juce::Component::SafePointer<MasterEditor> safe (this);
    const auto text = diagnosticsText();
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe, text] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File()) return;
        const bool ok = fc.getResult().replaceWithText (text);
        safe->showMessage (ok ? "DIAGNOSTICS SAVED" : "COULDN'T SAVE",
                           ok ? "Saved " + fc.getResult().getFileName() + ": the report, the prescription, the change log and every track's measurements. "
                                "Send it to Castle Danger Music Company with a note on what sounded wrong."
                              : "Couldn't write the file there. Try your Desktop or Documents folder.");
    });
}

void MasterEditor::showMyProfilesMenu()
{
    proc.getSongFolders().scanNow();
    proc.refreshSongProfiles();
    refreshGenres();
    const auto& lib = proc.getLibrary();
    const auto folders = proc.getSongFolders().getFolders();
    juce::PopupMenu m;
    m.addSectionHeader ("MY PROFILES  //  FROM YOUR SONGS");
    int i = 0;
    for (auto& f : folders)
    {
        const bool ready = f.songs > 0 && lib.indexOf (f.id) >= 0;
        juce::String label = f.name + "  (" + (f.songs > 0 ? juce::String (f.songs) + (f.songs == 1 ? " song" : " songs") : juce::String ("empty: add songs")) + ")";
        if (f.failed > 0) label << "  " << f.failed << " couldn't be read";
        m.addItem (200 + i, label, ready, proc.getGenreChoice() == f.id);
        ++i;
    }
    for (int k = lib.numBuiltIn(); k < (int) lib.genres().size(); ++k)   // from a project, folder not on this computer
    {
        const auto& g = lib.genres()[(size_t) k];
        bool listed = false; for (auto& f : folders) if (f.id == g.id) listed = true;
        if (! listed) m.addItem (400 + k, g.name + "  (saved with this project)", true, proc.getGenreChoice() == g.id);
    }
    if (folders.empty()) m.addItem (99, "No profiles yet. Make one below: any songs you like.", false);
    const auto meas = proc.getSongFolders().getMeasuring();
    if (meas.isNotEmpty()) m.addItem (98, "Measuring " + meas + "...", false);
    m.addSeparator();
    m.addItem (1, "New profile from songs...");
    m.addItem (2, "Open the My Profiles folder");
    if (lib.isCustom (proc.getGenreChoice())) m.addItem (3, "Back to AUTO");

    juce::Component::SafePointer<MasterEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&genreBar).withParentComponent (this),
        [safe, folders] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& self = *safe;
            if (r == 1) self.promptNewSongProfile();
            else if (r == 2) { mst::SongFolders::ensureRoot(); mst::SongFolders::root().startAsProcess(); }
            else if (r == 3) self.proc.setGenreChoice ("auto");
            else if (r >= 200 && r < 400 && r - 200 < (int) folders.size()) self.proc.setGenreChoice (folders[(size_t) (r - 200)].id);
            else if (r >= 400) self.proc.setGenreChoice (self.proc.getLibrary().genres()[(size_t) (r - 400)].id);
            self.refreshGenres();
            self.lastSnap = -1;
        });
}

void MasterEditor::promptNewSongProfile()
{
    nameDialog = std::make_unique<juce::AlertWindow> ("NEW PROFILE FROM SONGS",
        "Name it anything (\"My Band\", \"90s Grunge\", \"Album Sound\"). The Master makes a folder with that name and opens it: "
        "drop in finished songs that sound the way you want your mix to sound. One is enough; three or more is better.",
        juce::MessageBoxIconType::NoIcon, this);
    nameDialog->addTextEditor ("name", "", "Profile name");
    nameDialog->addButton ("CREATE", 1, juce::KeyPress (juce::KeyPress::returnKey));
    nameDialog->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    nameDialog->setColour (juce::AlertWindow::backgroundColourId, juce::Colour (0xff1a1411));
    nameDialog->setColour (juce::AlertWindow::textColourId, pal::paint);
    nameDialog->setColour (juce::AlertWindow::outlineColourId, pal::rust);
    juce::Component::SafePointer<MasterEditor> safe (this);
    nameDialog->enterModalState (true, juce::ModalCallbackFunction::create ([safe] (int result)
    {
        if (safe == nullptr) return;
        auto& self = *safe;
        if (result == 1 && self.nameDialog != nullptr)
        {
            juce::File dir;
            if (self.proc.createSongProfile (self.nameDialog->getTextEditorContents ("name"), dir))
            {
                dir.startAsProcess();
                self.showMessage ("ADD YOUR SONGS", "Drop songs into the \"" + dir.getFileName() + "\" folder that just opened (WAV, AIFF, FLAC, MP3, OGG"
                                  #if JUCE_MAC
                                  ", M4A"
                                  #endif
                                  "). The Master measures them in the background; then pick it under REFERENCE GENRE > MY PROFILES.");
            }
            else self.showMessage ("COULDN'T MAKE IT", "That name can't be used as a folder name. Try another.");
        }
        self.nameDialog.reset();
    }), false);
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
    m.addSeparator();
    m.addItem (6, "New profile from my songs...");
    m.addItem (7, "Open the My Profiles folder");
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
            else if (r == 6) self.promptNewSongProfile();
            else if (r == 7) { mst::SongFolders::ensureRoot(); mst::SongFolders::root().startAsProcess(); }
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
    const int bi = backdropIndex();
    if (backdropCache[(size_t) bi].isValid() && backdropCache[(size_t) bi].getWidth() == (int) (W * scale)) { backdrop = backdropCache[(size_t) bi]; return; }

    plates.clear();
    plates.push_back ({ "REFERENCE GENRE", { 16, 138, 712, 86 }, true });
    plates.push_back ({ "SOUNDS LIKE", { 738, 138, 346, 86 } });
    plates.push_back ({ "", tabsArea });
    if (page == 0)
    {
        plates.push_back ({ "TONAL BALANCE  //  YOUR MIX VS THE GENRE", { 16, 282, 640, 262 } });
        plates.push_back ({ "VITALS", { 16, 554, 640, 178 } });
        plates.push_back ({ "THE VERDICT", { 666, 282, 418, 450 } });
    }
    else if (proc.isPluginMode())
    {
        plates.push_back ({ "THE CURE", { 16, 282, 262, 450 }, true });
        plates.push_back ({ "THE PRESCRIPTION  //  YOUR MASTER BUS CHAIN", { 288, 282, 796, 236 } });
        plates.push_back ({ "WHAT I CHANGED & WHY", { 288, 528, 796, 204 } });
    }
    else
    {
        plates.push_back ({ "THE CURE", { 16, 282, 262, 450 }, true });
        for (auto& sct : sections)
        {
            static const char* names[] = { "TONE MATCH EQ", "DYNAMICS", "STEREO", "HUM", "LOUDNESS" };
            plates.push_back ({ names[(int) sct->id], sct->plate });
        }
    }
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
        if (s.title.isEmpty()) continue;
        auto title = tex::distressedText (s.title, s.bounds.getWidth() - 40, 18, tex::stencil (15.0f), s.hot ? pal::signalHot : pal::paint,
                                          juce::Justification::centredLeft, scale, 0.2f, seed++, 0.12f);
        g.setOpacity (1.0f);
        g.drawImage (title, juce::Rectangle<float> (p.getX() + 14, p.getY() + 3, (float) s.bounds.getWidth() - 40, 18.0f));
    }
    std::vector<juce::Component*> wells { &soundsLike };
    if (page == 0) for (auto* c : { (juce::Component*) &chart, (juce::Component*) &vitals, (juce::Component*) &score, (juce::Component*) &viewport }) wells.push_back (c);
    else { wells.push_back (&fixInfo); if (proc.isPluginMode()) { wells.push_back (&chainView); wells.push_back (&logViewport); } }
    for (auto* c : wells)
    {
        g.setColour (juce::Colours::black.withAlpha (0.7f)); g.fillRoundedRectangle (c->getBounds().toFloat().expanded (4.0f), 5.0f);
    }
    if (page == 0)
    {   // the verdict list sits in a dark well
        g.setColour (pal::screen); g.fillRoundedRectangle (viewport.getBounds().toFloat(), 4.0f);
    }
    else if (proc.isPluginMode())
    {
        g.setColour (pal::screen); g.fillRoundedRectangle (logViewport.getBounds().toFloat(), 4.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillRoundedRectangle (stepList.getBounds().toFloat().expanded (4.0f, 4.0f), 4.0f);
    }
    else
    {   // EQ: stamped band names, dB scale, a dark well behind the faders
        auto well = juce::Rectangle<float> (296.0f, 308.0f, 780.0f, 182.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillRoundedRectangle (well, 5.0f);
        const float cw = 772.0f / (float) mst::kNumBands;
        for (int b = 0; b < mst::kNumBands; ++b)
        {
            auto t = tex::distressedText (mst::bandDefs()[(size_t) b].label, (int) cw, 14, tex::stencil (11.5f), pal::paint, juce::Justification::centred, scale, 0.15f, 70 + b, 0.06f);
            g.setOpacity (1.0f); g.drawImage (t, juce::Rectangle<float> (300.0f + cw * (float) b, 313.0f, cw, 14.0f));
        }
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
    backdropCache[(size_t) bi] = backdrop;
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
