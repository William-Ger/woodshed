#include "ChannelStrip.h"

namespace woodshed::app
{

namespace mixer
{
namespace
{
    namespace ids = daw::ids;

    // Console taper: dB against fader travel, top down. Below the last point the fader is off.
    struct Anchor { float db, pos; };
    constexpr Anchor taper[] { { 6.0f, 1.0f }, { 0.0f, 0.78f }, { -6.0f, 0.62f }, { -12.0f, 0.47f }, { -24.0f, 0.29f },
                               { -36.0f, 0.17f }, { -48.0f, 0.08f }, { -60.0f, 0.03f }, { -90.0f, 0.005f } };
    constexpr int numAnchors = (int) std::size (taper);

    const juce::Colour inset       { 0xff0c0b0a };     // grooves, readouts, meter wells
    const juce::Colour edgeDark    { 0xff0a0908 };
    const juce::Colour capTop      { 0xff5a554d };
    const juce::Colour capBottom   { 0xff2b2825 };
    const juce::Colour knobTop     { 0xff3d3934 };
    const juce::Colour knobBottom  { 0xff1f1d1a };
    const juce::Colour ink         { 0xff141311 };     // label on a lit button

    juce::Colour segmentColour (float db)
    {
        return db >= -3.0f ? theme::meterHigh : db >= -12.0f ? theme::meterMid : theme::meterLow;
    }

    void drawInset (juce::Graphics& g, juce::Rectangle<float> r, float corner = 3.0f)
    {
        g.setColour (inset);
        g.fillRoundedRectangle (r, corner);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.drawHorizontalLine ((int) r.getBottom(), r.getX() + corner, r.getRight() - corner);
    }
}

float faderPos (double dbIn)
{
    const auto db = (float) dbIn;
    if (db <= -96.0f)
        return 0.0f;
    if (db >= taper[0].db)
        return 1.0f;

    for (int i = 0; i < numAnchors - 1; ++i)
    {
        const auto& hi = taper[i];
        const auto& lo = taper[i + 1];
        if (db >= lo.db)
            return lo.pos + (db - lo.db) / (hi.db - lo.db) * (hi.pos - lo.pos);
    }
    return taper[numAnchors - 1].pos;
}

double faderDb (float pos)
{
    if (pos <= 0.004f)
        return silenceDb;
    if (pos >= 1.0f)
        return taper[0].db;

    for (int i = 0; i < numAnchors - 1; ++i)
    {
        const auto& hi = taper[i];
        const auto& lo = taper[i + 1];
        if (pos >= lo.pos)
            return (double) (lo.db + (pos - lo.pos) / (hi.pos - lo.pos) * (hi.db - lo.db));
    }
    return (double) taper[numAnchors - 1].db;
}

float meterPos (float db)      { return db <= -90.0f ? 0.0f : faderPos ((double) db); }
float meterDb (float pos)      { return (float) faderDb (pos); }

juce::String dbText (double db)
{
    if (db <= -96.0)
        return "-inf";
    const auto rounded = std::round (db * 10.0) / 10.0;
    if (std::abs (rounded) < 0.05)
        return "0.0";
    return (rounded > 0.0 ? "+" : "") + juce::String (rounded, 1);
}

//==============================================================================
Bound::Bound (Studio& s, juce::ValueTree t, const juce::Identifier& p, const juce::String& name)
    : studio (s), tree (std::move (t)), prop (p), undoName (name)
{
}

void Bound::beginGesture()
{
    studio.project().undo.beginNewTransaction (undoName);
}

void Bound::beginWheelGesture()
{
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastWheel > 600)
        beginGesture();
    lastWheel = now;
}

void Bound::set (const juce::var& v)
{
    if (tree[prop] != v)
        tree.setProperty (prop, v, &studio.project().undo);
}

//==============================================================================
Fader::Fader (Studio& s, juce::ValueTree t, const juce::Identifier& p, juce::Colour c)
    : Bound (s, std::move (t), p, p == ids::masterVolume ? "Master Volume" : "Track Volume"), tint (c)
{
    setTooltip ("Volume. Drag (Shift for fine), double-click for 0 dB.");
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
}

double Fader::value() const   { return (double) tree.getProperty (prop, 0.0); }

float Fader::capHeight() const
{
    return juce::jlimit (16.0f, 38.0f, (float) getHeight() * 0.16f);
}

juce::Range<float> Fader::travel() const
{
    const float half = capHeight() * 0.5f;
    return { half + 3.0f, juce::jmax (half + 4.0f, (float) getHeight() - half - 3.0f) };
}

void Fader::paint (juce::Graphics& g)
{
    const auto span = travel();
    const auto yFor = [span] (float pos) { return span.getEnd() - pos * span.getLength(); };
    const float cx = (float) scaleWidth + (float) slotWidth * 0.5f;
    const float pos = faderPos (value());
    const float capY = yFor (pos);

    // Scale: ticks against the slot, labels on the left; labels that would collide are skipped.
    struct Mark { float db; const char* label; };
    static constexpr Mark marks[] { { 6.0f, "+6" }, { 0.0f, "0" }, { -6.0f, "-6" }, { -12.0f, "-12" }, { -24.0f, "-24" },
                                    { -36.0f, "-36" }, { -48.0f, "-48" }, { -60.0f, "-60" } };
    g.setFont (theme::font (9.0f));
    float lastLabelY = -100.0f;
    const float bottomY = yFor (0.0f);

    for (const auto& m : marks)
    {
        const float y = yFor (faderPos ((double) m.db));
        const bool unity = m.db == 0.0f;
        g.setColour (unity ? theme::text.withAlpha (0.55f) : theme::dimText.withAlpha (0.45f));
        if (unity)
        {
            g.fillRect (cx - 12.0f, y - 0.5f, 24.0f, 1.0f);
        }
        else
        {
            g.fillRect (cx - 12.0f, y - 0.5f, 7.0f, 1.0f);
            g.fillRect (cx + 5.0f, y - 0.5f, 7.0f, 1.0f);
        }

        if (y - lastLabelY >= 9.0f && bottomY - y >= 9.0f)
        {
            g.setColour (unity ? theme::text.withAlpha (0.8f) : theme::dimText);
            g.drawText (m.label, juce::Rectangle<float> (0.0f, y - 5.0f, (float) scaleWidth - 1.0f, 10.0f),
                        juce::Justification::centredRight, false);
            lastLabelY = y;
        }
    }
    g.setColour (theme::dimText.withAlpha (0.45f));
    g.fillRect (cx - 12.0f, bottomY - 0.5f, 7.0f, 1.0f);
    g.setColour (theme::dimText);
    g.drawText (juce::CharPointer_UTF8 ("-\xe2\x88\x9e"), juce::Rectangle<float> (0.0f, bottomY - 5.0f, (float) scaleWidth - 1.0f, 10.0f),
                juce::Justification::centredRight, false);

    // The groove, with the level below the cap in the track's colour.
    auto groove = juce::Rectangle<float> (cx - 2.5f, span.getStart() - 4.0f, 5.0f, span.getLength() + 8.0f);
    drawInset (g, groove, 2.5f);
    if (pos > 0.0f)
    {
        g.setColour (tint.withAlpha (0.55f));
        g.fillRoundedRectangle (juce::Rectangle<float> (cx - 1.0f, capY, 2.0f, bottomY - capY + 2.0f), 1.0f);
    }

    // The cap.
    const float capH = capHeight(), capW = (float) slotWidth - 4.0f;
    auto cap = juce::Rectangle<float> (cx - capW * 0.5f, capY - capH * 0.5f, capW, capH);
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (cap.translated (0.0f, 3.0f).expanded (1.0f, 0.5f), 3.5f);

    const bool hot = isMouseOverOrDragging();
    g.setGradientFill (juce::ColourGradient (hot ? capTop.brighter (0.15f) : capTop, cap.getX(), cap.getY(),
                                             capBottom, cap.getX(), cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 3.0f);

    // Ridges either side of the line, like a moulded cap.
    const int ridges = capH >= 26.0f ? 3 : 2;
    const float step = (capH * 0.5f - 4.0f) / (float) (ridges + 1);
    for (int i = 1; i <= ridges; ++i)
        for (float sign : { -1.0f, 1.0f })
        {
            const float y = capY + sign * ((float) i * step + 1.5f);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRect (cap.getX() + 3.0f, y, capW - 6.0f, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.fillRect (cap.getX() + 3.0f, y + 1.0f, capW - 6.0f, 1.0f);
        }

    g.setColour (juce::Colours::white.withAlpha (0.18f));
    g.drawHorizontalLine ((int) cap.getY() + 1, cap.getX() + 2.0f, cap.getRight() - 2.0f);
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.drawRoundedRectangle (cap, 3.0f, 1.0f);

    g.setColour (theme::text);
    g.fillRect (cap.getX() + 1.0f, capY - 1.0f, capW - 2.0f, 2.0f);
}

void Fader::setPos (float p, bool detent)
{
    auto db = faderDb (juce::jlimit (0.0f, 1.0f, p));
    if (db > silenceDb)
    {
        if (detent && std::abs (db) < 0.4)
            db = 0.0;
        db = std::round (db * 10.0) / 10.0;
    }
    set (db);
}

void Fader::mouseDown (const juce::MouseEvent& e)
{
    beginGesture();
    if (e.mods.isAltDown())
    {
        set (0.0);
        return;
    }
    dragStartPos = faderPos (value());
    lastDragY = e.y;
    repaint();
}

void Fader::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isAltDown())
        return;
    fine = e.mods.isShiftDown();
    const float delta = (float) (lastDragY - e.y) / juce::jmax (1.0f, travel().getLength());
    lastDragY = e.y;
    dragStartPos = juce::jlimit (0.0f, 1.0f, dragStartPos + delta * (fine ? 0.15f : 1.0f));
    setPos (dragStartPos, ! fine);
}

void Fader::mouseDoubleClick (const juce::MouseEvent&)
{
    beginGesture();
    set (0.0);
}

void Fader::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (std::abs (w.deltaX) > std::abs (w.deltaY))
    {
        Component::mouseWheelMove (e, w);     // sideways swipes scroll the mixer
        return;
    }
    beginWheelGesture();
    const float dir = w.isReversed ? -1.0f : 1.0f;
    setPos (faderPos (value()) + w.deltaY * dir * 0.2f, false);
}

//==============================================================================
PanKnob::PanKnob (Studio& s, juce::ValueTree t)
    : Bound (s, std::move (t), ids::pan, "Pan")
{
    setTooltip ("Pan. Drag up or right, double-click to centre.");
}

float PanKnob::value() const      { return juce::jlimit (-1.0f, 1.0f, (float) (double) tree.getProperty (prop, 0.0)); }

void PanKnob::setValue (float v)
{
    v = juce::jlimit (-1.0f, 1.0f, std::round (v * 100.0f) / 100.0f);
    set ((double) v);
}

void PanKnob::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const bool sideways = b.getWidth() > b.getHeight() * 1.8f;

    juce::Rectangle<float> knobArea, textArea;
    if (sideways)
    {
        const float d = b.getHeight(), textW = 30.0f;
        const float x = b.getX() + juce::jmax (0.0f, (b.getWidth() - d - 4.0f - textW) * 0.5f);
        knobArea = juce::Rectangle<float> (x, b.getY(), d, d);
        textArea = juce::Rectangle<float> (x + d + 4.0f, b.getY(), textW, d);
    }
    else
    {
        textArea = b.withTrimmedTop (b.getHeight() - 12.0f);
        const float d = juce::jmin (b.getWidth(), b.getHeight() - 12.0f);
        knobArea = juce::Rectangle<float> (d, d).withCentre ({ b.getCentreX(), b.getY() + d * 0.5f });
    }

    const float v = value();
    const auto centre = knobArea.getCentre();
    const float ringR = knobArea.getWidth() * 0.5f - 1.0f;
    const float bodyR = ringR - 3.5f;
    constexpr float sweep = juce::MathConstants<float>::pi * 0.75f;
    const float angle = v * sweep;

    // Ring: the whole travel faint, the offset from centre lit.
    juce::Path ring;
    ring.addCentredArc (centre.x, centre.y, ringR - 1.0f, ringR - 1.0f, 0.0f, -sweep, sweep, true);
    g.setColour (inset);
    g.strokePath (ring, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (std::abs (v) > 0.005f)
    {
        juce::Path lit;
        lit.addCentredArc (centre.x, centre.y, ringR - 1.0f, ringR - 1.0f, 0.0f, juce::jmin (0.0f, angle), juce::jmax (0.0f, angle), true);
        g.setColour (theme::accent);
        g.strokePath (lit, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.setColour (theme::dimText.withAlpha (0.6f));
    g.fillRect (centre.x - 0.5f, centre.y - ringR - 1.5f, 1.0f, 2.0f);

    // Body.
    auto body = juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillEllipse (body.translated (0.0f, 1.5f));
    g.setGradientFill (juce::ColourGradient (isMouseOverOrDragging() ? knobTop.brighter (0.15f) : knobTop,
                                             body.getX(), body.getY(), knobBottom, body.getX(), body.getBottom(), false));
    g.fillEllipse (body);
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.drawEllipse (body, 1.0f);

    // Pointer.
    juce::Path pointer;
    pointer.addRoundedRectangle (-1.0f, -bodyR + 1.5f, 2.0f, bodyR * 0.6f, 1.0f);
    g.setColour (theme::text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));

    // Readout.
    const int amount = juce::roundToInt (std::abs (v) * 100.0f);
    const juce::String text = amount == 0 ? juce::String ("C") : (v < 0.0f ? "L" : "R") + juce::String (amount);
    g.setColour (amount == 0 ? theme::dimText : theme::text);
    g.setFont (theme::font (10.0f));
    g.drawText (text, textArea, sideways ? juce::Justification::centredLeft : juce::Justification::centred, false);
}

void PanKnob::mouseDown (const juce::MouseEvent&)
{
    beginGesture();
    dragStart = value();
    repaint();
}

void PanKnob::mouseDrag (const juce::MouseEvent& e)
{
    const auto offset = e.getOffsetFromDragStart();
    const float scale = e.mods.isShiftDown() ? 0.002f : 0.012f;
    setValue (dragStart + (float) (offset.x - offset.y) * scale);
}

void PanKnob::mouseDoubleClick (const juce::MouseEvent&)
{
    beginGesture();
    setValue (0.0f);
}

void PanKnob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (std::abs (w.deltaX) > std::abs (w.deltaY))
    {
        Component::mouseWheelMove (e, w);
        return;
    }
    beginWheelGesture();
    setValue (value() + w.deltaY * (w.isReversed ? -1.0f : 1.0f) * 0.5f);
}

//==============================================================================
Toggle::Toggle (Studio& s, juce::ValueTree t, const juce::Identifier& p, const juce::String& undo,
                const juce::String& text, juce::Colour on, Glyph gl)
    : Bound (s, std::move (t), p, undo), label (text), onColour (on), glyph (gl)
{
}

void Toggle::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const bool on = (bool) get();
    const bool faint = ! on && implied != nullptr && implied();
    const bool hover = isMouseOver();

    if (on && glyph != Glyph::led)
    {
        g.setGradientFill (juce::ColourGradient (onColour.brighter (0.15f), b.getX(), b.getY(),
                                                 onColour.darker (0.25f), b.getX(), b.getBottom(), false));
        g.fillRoundedRectangle (b, 3.0f);
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawHorizontalLine ((int) b.getY() + 1, b.getX() + 2.0f, b.getRight() - 2.0f);
    }
    else
    {
        g.setGradientFill (juce::ColourGradient (theme::panelLight.brighter (hover ? 0.12f : 0.04f), b.getX(), b.getY(),
                                                 theme::panel, b.getX(), b.getBottom(), false));
        g.fillRoundedRectangle (b, 3.0f);
    }
    g.setColour (faint ? onColour.withAlpha (0.7f) : juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (b, 3.0f, 1.0f);

    const auto fg = on ? ink : faint ? onColour.withAlpha (0.8f) : theme::dimText.brighter (hover ? 0.3f : 0.0f);

    switch (glyph)
    {
        case Glyph::text:
            g.setColour (fg);
            g.setFont (theme::font (11.0f, true));
            g.drawText (label, b, juce::Justification::centred, false);
            break;

        case Glyph::dot:
        {
            const float d = juce::jmin (b.getWidth(), b.getHeight()) * 0.45f;
            g.setColour (on ? ink : onColour.withAlpha (hover ? 0.95f : 0.75f));
            g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (b.getCentre()));
            break;
        }

        case Glyph::headphones:
        {
            const float s = juce::jmin (b.getWidth(), b.getHeight()) * 0.62f;
            const auto c = b.getCentre();
            juce::Path band;
            band.addCentredArc (c.x, c.y + s * 0.08f, s * 0.42f, s * 0.42f, 0.0f,
                                -juce::MathConstants<float>::halfPi * 1.1f, juce::MathConstants<float>::halfPi * 1.1f, true);
            g.setColour (fg);
            g.strokePath (band, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            const float cupW = s * 0.22f, cupH = s * 0.38f;
            g.fillRoundedRectangle (c.x - s * 0.5f, c.y + s * 0.08f, cupW, cupH, 1.2f);
            g.fillRoundedRectangle (c.x + s * 0.5f - cupW, c.y + s * 0.08f, cupW, cupH, 1.2f);
            break;
        }

        case Glyph::led:
        {
            const float d = 6.0f;
            auto text = b.reduced (b.getWidth() < 44.0f ? 4.0f : 5.0f, 0.0f);
            const auto led = juce::Rectangle<float> (d, d).withCentre ({ text.getX() + d * 0.5f, b.getCentreY() });
            if (on)
            {
                g.setColour (onColour.withAlpha (0.3f));
                g.fillEllipse (led.expanded (2.5f));
            }
            g.setColour (on ? onColour : onColour.darker (2.2f));
            g.fillEllipse (led);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawEllipse (led, 0.8f);
            text.removeFromLeft (d + 3.0f);
            g.setColour (on ? theme::text : theme::dimText);
            g.setFont (theme::font (10.0f, true));
            g.drawText (label, text, juce::Justification::centred, false);
            break;
        }
    }
}

void Toggle::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mouseWasClicked() || ! getLocalBounds().contains (e.getPosition()))
        return;
    beginGesture();
    set (! (bool) get());
}

//==============================================================================
InputSelector::InputSelector (Studio& s, juce::ValueTree t)
    : Bound (s, std::move (t), ids::input, "Track Input")
{
    setTooltip ("The interface input this track records and monitors");
}

int InputSelector::numInputs() const
{
    int n = 0;
    if (auto* device = studio.deviceManager.getCurrentAudioDevice())
        n = device->getInputChannelNames().size();
    return juce::jmax (2, n, (int) get() + 1);
}

void InputSelector::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    drawInset (g, b);

    const int channel = (int) get() + 1;
    const bool roomy = b.getWidth() >= 40.0f;
    auto area = b.reduced (roomy ? 5.0f : 2.0f, 0.0f);

    if (roomy)
    {
        const auto arrowArea = area.removeFromRight (7.0f);
        juce::Path arrow;
        arrow.addTriangle (0.0f, 0.0f, 6.0f, 0.0f, 3.0f, 3.5f);
        g.setColour (isMouseOver() ? theme::text : theme::dimText);
        g.fillPath (arrow, juce::AffineTransform::translation (arrowArea.getX(), arrowArea.getCentreY() - 1.5f));
    }

    g.setFont (theme::readout (10.0f));
    g.setColour (isMouseOver() ? theme::accent.brighter (0.2f) : theme::accent);
    g.drawText ((roomy ? "IN " : "IN") + juce::String (channel), area,
                roomy ? juce::Justification::centredLeft : juce::Justification::centred, false);
}

void InputSelector::mouseDown (const juce::MouseEvent&)
{
    const int current = (int) get();
    juce::PopupMenu menu;
    menu.addSectionHeader ("Input");
    for (int i = 0; i < numInputs(); ++i)
        menu.addItem (i + 1, "Input " + juce::String (i + 1), true, i == current);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (90),
                        [safe = juce::Component::SafePointer<InputSelector> (this)] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;
                            safe->beginGesture();
                            safe->set (result - 1);
                            safe->repaint();
                        });
}

//==============================================================================
void IconButton::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const bool hover = isMouseOver();
    g.setGradientFill (juce::ColourGradient (theme::panelLight.brighter (hover ? 0.12f : 0.04f), b.getX(), b.getY(),
                                             theme::panel, b.getX(), b.getBottom(), false));
    g.fillRoundedRectangle (b, 3.0f);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (b, 3.0f, 1.0f);

    if (icon == Icon::rig)
    {
        // Three little sliders: "edit settings".
        const auto c = b.getCentre();
        const float h = juce::jmin (b.getHeight(), b.getWidth()) * 0.55f;
        const float knobs[] { 0.3f, 0.7f, 0.45f };
        g.setColour (hover ? theme::accent : theme::dimText.brighter (0.2f));
        for (int i = 0; i < 3; ++i)
        {
            const float x = c.x + (float) (i - 1) * 3.5f;
            g.fillRect (x - 0.5f, c.y - h * 0.5f, 1.0f, h);
            g.fillRect (x - 1.5f, c.y - h * 0.5f + knobs[i] * h - 1.0f, 3.0f, 2.0f);
        }
    }
}

void IconButton::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && getLocalBounds().contains (e.getPosition()) && onClick != nullptr)
        onClick();
}

//==============================================================================
Meter::Meter()
{
    setTooltip ("Peak level. Click to clear the clip light.");
}

void Meter::push (float left, float right, float dt)
{
    const bool wasActive = level[0] > -95.0f || level[1] > -95.0f || hold[0] > -95.0f || hold[1] > -95.0f;
    const float peaks[] { left, right };

    for (int ch = 0; ch < 2; ++ch)
    {
        if (peaks[ch] >= 1.0f)
            clipped = true;

        const float db = juce::Decibels::gainToDecibels (peaks[ch], -100.0f);
        level[ch] = juce::jmax (db, level[ch] - 26.0f * dt, -100.0f);

        if (db >= hold[ch])
        {
            hold[ch] = db;
            holdAge[ch] = 0.0f;
        }
        else if ((holdAge[ch] += dt) > 1.5f)
        {
            hold[ch] = juce::jmax (-100.0f, hold[ch] - 20.0f * dt);
        }
    }

    const bool active = level[0] > -95.0f || level[1] > -95.0f || hold[0] > -95.0f || hold[1] > -95.0f;
    if (active || wasActive)
        repaint();
}

void Meter::resetPeaks()
{
    clipped = false;
    hold[0] = hold[1] = -100.0f;
    repaint();
    if (onReset != nullptr)
        onReset();
}

void Meter::mouseDown (const juce::MouseEvent&)
{
    resetPeaks();
}

void Meter::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    const auto led = b.removeFromTop (4.0f);
    b.removeFromTop (2.0f);

    g.setColour (clipped ? theme::meterHigh : theme::meterHigh.darker (2.4f));
    g.fillRoundedRectangle (led, 1.0f);
    if (clipped)
    {
        g.setColour (theme::meterHigh.withAlpha (0.25f));
        g.fillRoundedRectangle (led.expanded (1.5f), 2.0f);
    }

    drawInset (g, b, 2.0f);
    const auto wells = b.reduced (1.0f);
    const float barW = (wells.getWidth() - 1.0f) * 0.5f;
    constexpr float pitch = 3.0f;
    const int n = juce::jmax (1, (int) (wells.getHeight() / pitch));
    const float stretch = wells.getHeight() / (float) n;          // segments fill the well exactly

    for (int ch = 0; ch < 2; ++ch)
    {
        const float x = wells.getX() + (float) ch * (barW + 1.0f);
        const float lit = meterPos (level[ch]);
        const int holdSeg = hold[ch] > -60.0f ? juce::jlimit (0, n - 1, (int) (meterPos (hold[ch]) * (float) n)) : -1;

        for (int i = 0; i < n; ++i)
        {
            const float segPos = ((float) i + 0.5f) / (float) n;
            const auto colour = segmentColour (meterDb (segPos));
            const bool on = segPos <= lit || i == holdSeg;
            g.setColour (on ? colour : colour.withAlpha (0.08f));
            g.fillRect (x, wells.getBottom() - (float) (i + 1) * stretch + 0.5f, barW, stretch - 1.0f);
        }
    }
}

} // namespace mixer

//==============================================================================
namespace
{
    namespace ids = daw::ids;

    /** Row sizes; the pane can be anything from about 160 px to most of the window. */
    struct Metrics
    {
        bool tall, readouts;
        int gap, panH, buttonH;

        explicit Metrics (int height)
            : tall (height >= 340), readouts (height >= 230),
              gap (tall ? 5 : 3), panH (tall ? 40 : 24), buttonH (tall ? 18 : 17) {}

        /** Everything between the name and the fader (identical on the master so the faders line up). */
        int controlsHeight() const
        {
            const int io = tall ? 18 + 4 + 18 : 18;
            const int buttons = tall ? buttonH * 2 + 3 : buttonH;
            return io + gap + panH + gap + buttons + gap + 2;
        }
    };
}

ChannelStrip::ChannelStrip (Studio& s, juce::ValueTree t, bool isMaster)
    : studio (s), tree (std::move (t)), master (isMaster),
      fader (s, tree, isMaster ? ids::masterVolume : ids::volume, isMaster ? theme::accent : trackColour())
{
    addAndMakeVisible (fader);
    addAndMakeVisible (meter);
    meter.onReset = [this] { shownPeak = {}; repaint (peakReadout); };

    if (! master)
    {
        input = std::make_unique<mixer::InputSelector> (s, tree);
        rig = std::make_unique<mixer::Toggle> (s, tree, ids::rigOn, "Rig On/Off", "RIG", theme::loop, mixer::Toggle::Glyph::led);
        mute = std::make_unique<mixer::Toggle> (s, tree, ids::mute, "Mute", "M", theme::mute, mixer::Toggle::Glyph::text);
        solo = std::make_unique<mixer::Toggle> (s, tree, ids::solo, "Solo", "S", theme::solo, mixer::Toggle::Glyph::text);
        arm = std::make_unique<mixer::Toggle> (s, tree, ids::arm, "Record Arm", juce::String(), theme::record, mixer::Toggle::Glyph::dot);
        monitor = std::make_unique<mixer::Toggle> (s, tree, ids::monitor, "Input Monitor", juce::String(), theme::accent, mixer::Toggle::Glyph::headphones);
        openRig = std::make_unique<mixer::IconButton> (mixer::IconButton::Icon::rig);
        pan = std::make_unique<mixer::PanKnob> (s, tree);

        rig->setTooltip ("Rig on/off: play this track through its pedalboard, amp and cab");
        mute->setTooltip ("Mute");
        solo->setTooltip ("Solo");
        arm->setTooltip ("Arm for recording");
        monitor->setTooltip ("Monitor the input through the rig");
        openRig->setTooltip ("Edit this track's rig");
        mute->implied = [this] { return anySolo && ! (bool) tree[ids::solo]; };

        openRig->onClick = [this]
        {
            select();
            studio.setPane (Studio::Pane::rig);
        };

        for (auto* c : std::initializer_list<juce::Component*> { input.get(), rig.get(), mute.get(), solo.get(), arm.get(),
                                                                 monitor.get(), openRig.get(), pan.get() })
            addAndMakeVisible (c);
    }

    // Any click on the strip selects its track.
    addMouseListener (this, true);
}

ChannelStrip::~ChannelStrip()
{
    removeMouseListener (this);
}

juce::Colour ChannelStrip::trackColour() const
{
    return master ? theme::accent : juce::Colour ((juce::uint32) (int) tree.getProperty (ids::colour, (int) 0xff8d857a));
}

void ChannelStrip::select()
{
    if (! master)
        studio.selectTrack (getTrackId());
}

void ChannelStrip::setSelected (bool s)
{
    if (selected != s)
    {
        selected = s;
        repaint();
    }
}

void ChannelStrip::setAnySolo (bool s)
{
    if (anySolo != s)
    {
        anySolo = s;
        if (mute != nullptr)
            mute->repaint();
    }
}

void ChannelStrip::refresh()
{
    fader.setTint (trackColour());
    repaint();
    for (auto* c : getChildren())
        c->repaint();
}

void ChannelStrip::pollMeter (float dt)
{
    auto levels = master ? studio.engine().takeMasterLevels() : studio.engine().takeTrackLevels (getTrackId());
    meter.push (levels.left, levels.right, dt);

    const float peak = meter.getPeakHoldDb();
    const auto text = peak > -60.0f ? mixer::dbText ((double) peak) : juce::String ("-inf");
    if (text != shownPeak || meter.isClipped() != shownClip)
    {
        shownPeak = text;
        shownClip = meter.isClipped();
        repaint (peakReadout);
    }
}

void ChannelStrip::discardMeter()
{
    if (master)
        studio.engine().takeMasterLevels();
    else
        studio.engine().takeTrackLevels (getTrackId());
}

void ChannelStrip::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const auto colour = trackColour();

    // Body.
    const auto base = master ? theme::panel.brighter (0.04f) : theme::panel;
    g.setGradientFill (juce::ColourGradient (selected ? theme::panelLight.brighter (0.05f) : base.brighter (0.03f), 0.0f, 0.0f,
                                             selected ? theme::panelLight.darker (0.15f) : base.darker (0.12f), 0.0f, b.getBottom(), false));
    g.fillRect (b);

    g.setColour (juce::Colours::white.withAlpha (0.035f));
    g.fillRect (0.0f, 0.0f, 1.0f, b.getHeight());
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRect (b.getRight() - 1.0f, 0.0f, 1.0f, b.getHeight());

    // Colour cap and name.
    g.setColour (colour);
    g.fillRect (0.0f, 0.0f, b.getWidth(), 4.0f);
    g.setColour (colour.withAlpha (selected ? 0.3f : 0.16f));
    g.fillRect (nameArea.toFloat());

    g.setColour (selected ? theme::accent : theme::text);
    g.setFont (theme::font (12.0f, true));
    const auto name = master ? juce::String ("MASTER") : tree[ids::name].toString();
    g.drawFittedText (name, nameArea.reduced (5, 0), juce::Justification::centred, 1, 0.8f);

    if (master)
    {
        g.setColour (theme::dimText.withAlpha (0.8f));
        g.setFont (theme::font (10.0f, true));
        auto t = topArea.toFloat();
        g.drawText ("STEREO OUT", t.removeFromTop (18.0f), juce::Justification::centred, false);
        g.setColour (theme::outline);
        g.fillRect (t.getX() + 10.0f, t.getY() + 2.0f, t.getWidth() - 20.0f, 1.0f);
    }

    // Readouts: fader value and peak hold.
    if (! faderReadout.isEmpty())
    {
        g.setFont (theme::readout (10.0f));
        for (auto r : { faderReadout, peakReadout })
        {
            auto rf = r.toFloat();
            g.setColour (juce::Colour (0xff0c0b0a));
            g.fillRoundedRectangle (rf, 2.5f);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.drawRoundedRectangle (rf.reduced (0.5f), 2.5f, 1.0f);
        }

        const double db = (double) tree.getProperty (master ? ids::masterVolume : ids::volume, 0.0);
        g.setColour (theme::accent);
        g.drawText (mixer::dbText (db), faderReadout, juce::Justification::centred, false);

        g.setColour (meter.isClipped() ? theme::meterHigh : shownPeak == "-inf" || shownPeak.isEmpty() ? theme::dimText : theme::text);
        g.drawText (shownPeak.isEmpty() ? juce::String ("-inf") : shownPeak, peakReadout, juce::Justification::centred, false);
    }

    if (selected)
    {
        g.setColour (theme::accent.withAlpha (0.85f));
        g.drawRect (b.withTrimmedTop (4.0f), 1.0f);
    }
}

void ChannelStrip::resized()
{
    const Metrics m (getHeight());
    auto r = getLocalBounds().reduced (5, 0);
    r.removeFromTop (4);
    nameArea = juce::Rectangle<int> (0, 4, getWidth(), 20);
    r.removeFromTop (20 + m.gap);

    // Readouts at the bottom.
    r.removeFromBottom (4);
    if (m.readouts)
    {
        auto row = r.removeFromBottom (15);
        faderReadout = row.removeFromLeft ((row.getWidth() - 3) / 2);
        row.removeFromLeft (3);
        peakReadout = row;
        r.removeFromBottom (m.gap);
    }
    else
    {
        faderReadout = peakReadout = {};
    }

    topArea = r.removeFromTop (m.controlsHeight());

    if (! master)
    {
        auto c = topArea;
        if (m.tall)
        {
            input->setBounds (c.removeFromTop (18));
            c.removeFromTop (4);
            auto row = c.removeFromTop (18);
            openRig->setBounds (row.removeFromRight (20));
            row.removeFromRight (3);
            rig->setBounds (row);
        }
        else
        {
            auto row = c.removeFromTop (18);
            openRig->setBounds (row.removeFromRight (16));
            row.removeFromRight (3);
            rig->setBounds (row.removeFromRight (36));
            row.removeFromRight (3);
            input->setBounds (row);
        }
        c.removeFromTop (m.gap);
        pan->setBounds (c.removeFromTop (m.panH));
        c.removeFromTop (m.gap);

        mixer::Toggle* buttons[] { mute.get(), solo.get(), arm.get(), monitor.get() };
        if (m.tall)
        {
            for (int row = 0; row < 2; ++row)
            {
                auto line = c.removeFromTop (m.buttonH);
                const int w = (line.getWidth() - 3) / 2;
                buttons[row * 2]->setBounds (line.removeFromLeft (w));
                line.removeFromLeft (3);
                buttons[row * 2 + 1]->setBounds (line);
                c.removeFromTop (3);
            }
        }
        else
        {
            auto line = c.removeFromTop (m.buttonH);
            const int w = (line.getWidth() - 9) / 4;
            for (int i = 0; i < 4; ++i)
            {
                buttons[i]->setBounds (i < 3 ? line.removeFromLeft (w) : line);
                line.removeFromLeft (3);
            }
        }
    }

    // Fader and meter, centred as a group, the meter matching the fader's travel.
    const int meterW = master ? 18 : 14;
    const int faderW = mixer::Fader::scaleWidth + mixer::Fader::slotWidth;
    const int group = faderW + 4 + meterW;
    const int x = r.getX() + (r.getWidth() - group) / 2 - 2;
    fader.setBounds (x, r.getY(), faderW, r.getHeight());
    const auto span = fader.travel();
    const int meterTop = r.getY() + (int) std::floor (span.getStart()) - 7;
    meter.setBounds (x + faderW + 4, meterTop, meterW, (int) std::ceil (span.getLength()) + 8);
}

void ChannelStrip::mouseDown (const juce::MouseEvent& e)
{
    select();
    if (e.eventComponent == this && peakReadout.contains (e.getPosition()))
        meter.resetPeaks();
}

} // namespace woodshed::app
