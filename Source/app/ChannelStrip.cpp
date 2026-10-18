#include "ChannelStrip.h"
#include "InstrumentPanel.h"
#include "Icons.h"
#include "SpatialPanner.h"

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

    const juce::Colour well        { 0xff111113 };     // grooves, readouts, meter wells
    const juce::Colour ink         { 0xff141416 };     // label on a lit button

    juce::Colour segmentColour (float db)
    {
        return db >= -3.0f ? theme::meterHigh : db >= -12.0f ? theme::meterMid : theme::meterLow;
    }

    /** The strip's small buttons and chips: flat graphite, hairline edge, 1 px light on top. */
    void drawChip (juce::Graphics& g, juce::Rectangle<float> r, bool hover, bool down = false)
    {
        theme::drawRaised (g, r, theme::radiusChip, down ? theme::controlDown : hover ? theme::controlHover : theme::control);
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
    return juce::jlimit (14.0f, 32.0f, (float) getHeight() * 0.12f);
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

    // Scale: short ticks left of the groove, labels to their left; labels that would collide are skipped.
    struct Mark { float db; const char* label; };
    static constexpr Mark marks[] { { 6.0f, "+6" }, { 0.0f, "0" }, { -6.0f, "-6" }, { -12.0f, "-12" }, { -24.0f, "-24" },
                                    { -36.0f, "-36" }, { -48.0f, "-48" }, { -60.0f, "-60" } };
    g.setFont (theme::font (10.0f));
    float lastLabelY = -100.0f;
    const float bottomY = yFor (0.0f);
    const float tickX = (float) scaleWidth - 4.0f;

    for (const auto& m : marks)
    {
        const float y = std::round (yFor (faderPos ((double) m.db))) + 0.5f;
        if (y - lastLabelY < 11.0f || bottomY - y < 11.0f)
            continue;

        const bool unity = m.db == 0.0f;
        g.setColour (unity ? theme::dimText : theme::faintText.withAlpha (0.7f));
        g.fillRect (tickX, y - 0.5f, 3.0f, 1.0f);
        g.setColour (unity ? theme::dimText : theme::faintText);
        g.drawText (m.label, juce::Rectangle<float> (0.0f, y - 6.0f, tickX - 2.0f, 12.0f),
                    juce::Justification::centredRight, false);
        lastLabelY = y;
    }
    g.setColour (theme::faintText.withAlpha (0.7f));
    g.fillRect (tickX, std::round (bottomY), 3.0f, 1.0f);
    g.setColour (theme::faintText);
    g.drawText (juce::CharPointer_UTF8 ("-\xe2\x88\x9e"), juce::Rectangle<float> (0.0f, bottomY - 6.0f, tickX - 2.0f, 12.0f),
                juce::Justification::centredRight, false);

    // The groove: a thin dark slot.
    const auto groove = juce::Rectangle<float> (cx - 1.5f, span.getStart() - 2.0f, 3.0f, span.getLength() + 4.0f);
    g.setColour (well);
    g.fillRoundedRectangle (groove, 1.5f);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (groove.expanded (0.5f), 2.0f, 1.0f);

    // The cap: flat, modern, with a centre line.
    const bool hot = isMouseOverOrDragging();
    const float capH = capHeight(), capW = (float) slotWidth - 2.0f;   // 20 px
    const auto cap = juce::Rectangle<float> (cx - capW * 0.5f, std::round (capY - capH * 0.5f), capW, std::round (capH));

    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (cap.translated (0.0f, 1.5f).expanded (0.5f), 4.0f);
    const auto fill = hot ? juce::Colour (0xff5a5a62) : juce::Colour (0xff4c4c54);
    g.setGradientFill (juce::ColourGradient (fill.brighter (0.06f), 0.0f, cap.getY(), fill.darker (0.12f), 0.0f, cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 3.5f);
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    g.drawHorizontalLine ((int) cap.getY() + 1, cap.getX() + 3.0f, cap.getRight() - 3.0f);
    g.setColour (theme::separator.withAlpha (0.8f));
    g.drawRoundedRectangle (cap.reduced (0.5f), 3.5f, 1.0f);

    g.setColour (hot ? theme::accent : theme::text.withAlpha (0.9f));
    g.fillRect (cap.getX() + 3.0f, std::floor (cap.getCentreY()), capW - 6.0f, 1.0f);
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
    setTooltip ("Pan. Drag up or right, double-click to centre. Right-click for the 3D panner.");
}

bool PanKnob::isSpatial() const   { return (bool) tree[ids::spatial]; }

void PanKnob::showSpatialMenu()
{
    // Spatial audio: the 3D panner instead of the stereo pan.
    juce::PopupMenu m;
    m.addItem ("3D Panner...", [this] { SpatialPanner::show (studio, tree, getScreenBounds()); });
    m.addItem (isSpatial() ? "Use Stereo Pan" : "Use 3D Position", [this]
    {
        studio.project().undo.beginNewTransaction (isSpatial() ? "Stereo Pan" : "3D Position");
        tree.setProperty (ids::spatial, ! isSpatial(), &studio.project().undo);
        repaint();
    });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
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
        const float d = juce::jmin (b.getHeight(), 24.0f), textW = 28.0f;
        const float x = b.getX() + juce::jmax (0.0f, (b.getWidth() - d - 4.0f - textW) * 0.5f);
        knobArea = juce::Rectangle<float> (x, b.getCentreY() - d * 0.5f, d, d);
        textArea = juce::Rectangle<float> (x + d + 4.0f, b.getY(), textW, b.getHeight());
    }
    else
    {
        const float d = juce::jmin (b.getWidth(), b.getHeight() - 14.0f, 32.0f);
        textArea = juce::Rectangle<float> (b.getX(), b.getY() + d + 1.0f, b.getWidth(), 12.0f);
        knobArea = juce::Rectangle<float> (d, d).withCentre ({ b.getCentreX(), b.getY() + d * 0.5f });
    }

    const float v = value();
    const bool hot = isMouseOverOrDragging();
    const auto centre = knobArea.getCentre();

    // A 3D track: a top view with the source's direction instead of the knob.
    if (isSpatial())
    {
        const float r = knobArea.getWidth() * 0.5f - 1.5f;
        g.setColour (well);
        g.fillEllipse (knobArea.reduced (1.5f));
        g.setColour (hot ? theme::accent : theme::accent.withAlpha (0.7f));
        g.drawEllipse (knobArea.reduced (1.5f), 1.5f);
        const float az = (float) (double) tree.getProperty (ids::azimuth, 0.0) * juce::MathConstants<float>::pi / 180.0f;
        const float reach = r * (0.35f + 0.55f * SpatialPanner::radiusFor ((float) (double) tree.getProperty (ids::distance, 1.0)));
        g.setColour (theme::text);
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ centre.x + reach * std::sin (az), centre.y - reach * std::cos (az) }));
        g.setColour (theme::dimText);
        g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre (centre));
        g.setFont (theme::readout (10.0f, theme::Weight::medium));
        g.drawText ("3D", textArea, sideways ? juce::Justification::centredLeft : juce::Justification::centred, false);
        return;
    }

    const float ringR = knobArea.getWidth() * 0.5f - 1.5f;
    const float bodyR = ringR - 4.0f;
    constexpr float sweep = juce::MathConstants<float>::pi * 0.75f;
    const float angle = v * sweep;
    const juce::PathStrokeType stroke (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    // Ring: the whole travel as a quiet track, the offset from centre in the accent.
    juce::Path ring;
    ring.addCentredArc (centre.x, centre.y, ringR, ringR, 0.0f, -sweep, sweep, true);
    g.setColour (well);
    g.strokePath (ring, stroke);

    if (std::abs (v) > 0.005f)
    {
        juce::Path lit;
        lit.addCentredArc (centre.x, centre.y, ringR, ringR, 0.0f, juce::jmin (0.0f, angle), juce::jmax (0.0f, angle), true);
        g.setColour (theme::accent);
        g.strokePath (lit, stroke);
    }

    // Body: a flat graphite disc with a light top edge.
    const auto body = juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillEllipse (body.translated (0.0f, 1.0f).expanded (0.5f));
    const auto fill = hot ? theme::controlHover.brighter (0.08f) : theme::control.brighter (0.06f);
    g.setGradientFill (juce::ColourGradient (fill.brighter (0.08f), 0.0f, body.getY(), fill.darker (0.15f), 0.0f, body.getBottom(), false));
    g.fillEllipse (body);
    g.setColour (theme::separator.withAlpha (0.8f));
    g.drawEllipse (body.reduced (0.5f), 1.0f);

    // Pointer.
    juce::Path pointer;
    pointer.addRoundedRectangle (-0.75f, -bodyR + 2.0f, 1.5f, bodyR * 0.55f, 0.75f);
    g.setColour (theme::text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));

    // Readout.
    const int amount = juce::roundToInt (std::abs (v) * 100.0f);
    const juce::String text = amount == 0 ? juce::String ("C") : (v < 0.0f ? "L" : "R") + juce::String (amount);
    g.setColour (amount == 0 ? theme::faintText : theme::dimText);
    g.setFont (theme::readout (10.0f, theme::Weight::medium));
    g.drawText (text, textArea, sideways ? juce::Justification::centredLeft : juce::Justification::centred, false);
}

void PanKnob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showSpatialMenu();
        return;
    }
    if (isSpatial())
    {
        SpatialPanner::show (studio, tree, getScreenBounds());
        return;
    }
    beginGesture();
    dragStart = value();
    repaint();
}

void PanKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || isSpatial())
        return;
    const auto offset = e.getOffsetFromDragStart();
    const float scale = e.mods.isShiftDown() ? 0.002f : 0.012f;
    setValue (dragStart + (float) (offset.x - offset.y) * scale);
}

void PanKnob::mouseDoubleClick (const juce::MouseEvent&)
{
    if (isSpatial())
        return;
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
    if (isSpatial())
        return;
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
    const auto b = getLocalBounds().toFloat();
    const bool on = (bool) get();
    const bool faint = ! on && implied != nullptr && implied();
    const bool hover = isMouseOver();
    const bool down = isMouseButtonDown();
    const bool led = glyph == Glyph::led;

    if (on && ! led)
    {
        const auto fill = down ? onColour.darker (0.2f) : hover ? onColour.brighter (0.1f) : onColour;
        g.setColour (fill);
        g.fillRoundedRectangle (b, theme::radiusChip);
        g.setColour (juce::Colours::white.withAlpha (0.2f));
        g.drawHorizontalLine ((int) b.getY() + 1, b.getX() + 3.0f, b.getRight() - 3.0f);
    }
    else
    {
        drawChip (g, b, hover, down);
        if (faint)
        {
            g.setColour (onColour.withAlpha (0.55f));
            g.drawRoundedRectangle (b.reduced (0.5f), theme::radiusChip, 1.0f);
        }
    }

    const auto rest = hover ? theme::text : theme::dimText;
    const auto fg = on ? ink : faint ? onColour.withAlpha (0.85f) : rest;

    switch (glyph)
    {
        case Glyph::text:
            g.setColour (fg);
            g.setFont (theme::font (11.0f, theme::Weight::semibold));
            g.drawText (label, b, juce::Justification::centred, false);
            break;

        case Glyph::dot:
        {
            const float d = std::round (juce::jmin (b.getWidth(), b.getHeight()) * 0.4f);
            g.setColour (on ? juce::Colours::white : onColour.withAlpha (hover ? 1.0f : 0.8f));
            g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (b.getCentre()));
            break;
        }

        case Glyph::headphones:
        {
            const float s = std::round (juce::jmin (b.getWidth(), b.getHeight()) * 0.66f);
            icons::draw (g, icons::headphones(), juce::Rectangle<float> (s, s).withCentre (b.getCentre()), fg, 2.0f);
            break;
        }

        case Glyph::led:
        {
            const float d = 6.0f;
            const bool dotOnly = b.getWidth() < 36.0f;      // compact strips: just the light
            auto text = b.reduced (7.0f, 0.0f);
            const auto dot = juce::Rectangle<float> (d, d).withCentre (dotOnly ? b.getCentre()
                                                                               : juce::Point<float> (text.getX() + d * 0.5f, b.getCentreY()));
            if (on)
            {
                g.setColour (onColour.withAlpha (0.25f));
                g.fillEllipse (dot.expanded (2.0f));
                g.setColour (onColour);
                g.fillEllipse (dot);
            }
            else
            {
                g.setColour (theme::faintText);
                g.drawEllipse (dot.reduced (0.5f), 1.0f);
            }
            if (dotOnly)
                break;
            text.removeFromLeft (d + 4.0f);
            g.setColour (on ? theme::text : rest);
            g.setFont (theme::font (11.0f, theme::Weight::medium));
            g.drawText (label, text, juce::Justification::centredLeft, false);
            break;
        }
    }
}

void Toggle::mouseUp (const juce::MouseEvent& e)
{
    repaint();
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
    const bool hover = isMouseOver();
    drawChip (g, b, hover, isMouseButtonDown());

    const int channel = (int) get() + 1;
    const bool roomy = b.getWidth() >= 44.0f;
    auto area = b.reduced (roomy ? 7.0f : 2.0f, 0.0f);

    if (roomy)
    {
        const auto chevron = area.removeFromRight (9.0f);
        icons::draw (g, icons::chevronDown(), juce::Rectangle<float> (9.0f, 9.0f).withCentre (chevron.getCentre()),
                     hover ? theme::text : theme::dimText, 2.2f);
    }

    g.setFont (theme::font (11.0f, theme::Weight::medium));
    g.setColour (hover ? theme::text : theme::text.withAlpha (0.85f));
    g.drawFittedText ((roomy ? "Input " : "In ") + juce::String (channel), area.toNearestInt(),
                      roomy ? juce::Justification::centredLeft : juce::Justification::centred, 1, 0.8f);
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
SendSlot::SendSlot (Studio& s, juce::ValueTree t, int i) : studio (s), track (std::move (t)), index (i)
{
    setTooltip ("Send: this track's signal also goes to an aux (a shared reverb, say). Drag for level, click for options.");
}

juce::ValueTree SendSlot::send() const
{
    return daw::Project::sends (track)[index];
}

void SendSlot::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const bool hover = isMouseOver();
    auto s = send();
    if (! s.isValid())
    {
        if (index != daw::Project::sends (track).size())
            return;
        drawChip (g, b, hover, isMouseButtonDown());
        g.setColour (hover ? theme::text : theme::faintText);
        g.setFont (theme::font (11.0f, theme::Weight::medium));
        g.drawText ("+ Send", b, juce::Justification::centred, false);
        return;
    }

    drawChip (g, b, hover, isMouseButtonDown());
    const double db = (double) s.getProperty (ids::level, 0.0);
    const auto fill = b.reduced (1.0f).withWidth ((b.getWidth() - 2.0f) * faderPos (db));
    g.setColour (theme::accent.withAlpha ((bool) s[ids::pre] ? 0.18f : 0.32f));
    g.fillRoundedRectangle (fill, theme::radiusChip);

    const auto bus = track.getParent().getChildWithProperty (ids::id, s[ids::bus]);
    g.setColour (theme::text);
    g.setFont (theme::font (10.5f, theme::Weight::medium));
    auto area = b.reduced (5.0f, 0.0f);
    g.drawFittedText (((bool) s[ids::pre] ? "pre " : "") + bus[ids::name].toString(), area.removeFromLeft (area.getWidth() * 0.6f).toNearestInt(),
                      juce::Justification::centredLeft, 1, 0.7f);
    g.setFont (theme::readout (9.5f, theme::Weight::medium));
    g.setColour (theme::dimText);
    g.drawText (dbText (db), area, juce::Justification::centredRight, false);
}

void SendSlot::mouseDown (const juce::MouseEvent& e)
{
    dragged = false;
    if (auto s = send(); s.isValid())
    {
        dragStartPos = faderPos ((double) s.getProperty (ids::level, 0.0));
        if (! e.mods.isPopupMenu())
            studio.project().undo.beginNewTransaction ("Send Level");
    }
    repaint();
}

void SendSlot::mouseDrag (const juce::MouseEvent& e)
{
    auto s = send();
    if (! s.isValid() || e.mods.isPopupMenu() || e.getDistanceFromDragStart() < 2)
        return;
    dragged = true;
    const float delta = (float) (e.getDistanceFromDragStartX() - e.getDistanceFromDragStartY()) / (e.mods.isShiftDown() ? 600.0f : 150.0f);
    const auto pos = juce::jlimit (0.0f, 1.0f, dragStartPos + delta);
    s.setProperty (ids::level, pos <= 0.0f ? silenceDb : std::round (faderDb (pos) * 10.0) / 10.0, &studio.project().undo);
    repaint();
}

void SendSlot::mouseUp (const juce::MouseEvent& e)
{
    repaint();
    if (dragged || ! getLocalBounds().contains (e.getPosition()))
        return;
    if (send().isValid()) showSendMenu();
    else if (index == daw::Project::sends (track).size()) showAddMenu();
}

void SendSlot::mouseDoubleClick (const juce::MouseEvent&)
{
    if (auto s = send(); s.isValid())
    {
        studio.project().undo.beginNewTransaction ("Send Level");
        s.setProperty (ids::level, 0.0, &studio.project().undo);
    }
}

void SendSlot::showAddMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("Send to");
    auto& p = studio.project();
    for (int i = 0; i < p.numTracks(); ++i)
    {
        auto t = p.track (i);
        if (! daw::Project::isAuxTrack (t) || t == track)
            continue;
        const int busId = (int) t[ids::id];
        m.addItem (t[ids::name].toString(), [this, busId]
        {
            auto& pr = studio.project();
            pr.undo.beginNewTransaction ("Add Send");
            pr.addSend (track, busId, -10.0);
        });
    }
    m.addSeparator();
    auto addNew = [this] (const juce::String& name, const juce::String& effect)
    {
        return [this, name, effect]
        {
            const auto source = track;
            auto bus = studio.addAuxTrack (name, effect);
            auto& pr = studio.project();
            pr.addSend (source, (int) bus[ids::id], -10.0);     // part of the New Aux step
            studio.selectTrack ((int) source[ids::id]);
        };
    };
    m.addItem ("New Reverb Aux", addNew ("Reverb", "reverb.hall"));
    m.addItem ("New Delay Aux", addNew ("Delay", "delay.digital"));
    m.addItem ("New Empty Aux", addNew ("Aux", {}));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
}

void SendSlot::showSendMenu()
{
    auto s = send();
    juce::PopupMenu m;
    m.addItem ("Pre-Fader", true, (bool) s[ids::pre], [this, s]() mutable
    {
        auto& pr = studio.project();
        pr.undo.beginNewTransaction ("Send Pre/Post");
        s.setProperty (ids::pre, ! (bool) s[ids::pre], &pr.undo);
    });
    juce::PopupMenu to;
    auto& p = studio.project();
    for (int i = 0; i < p.numTracks(); ++i)
    {
        auto t = p.track (i);
        if (! daw::Project::isAuxTrack (t) || t == track)
            continue;
        to.addItem (t[ids::name].toString(), true, (int) s[ids::bus] == (int) t[ids::id], [this, s, busId = (int) t[ids::id]]() mutable
        {
            auto& pr = studio.project();
            pr.undo.beginNewTransaction ("Send Destination");
            s.setProperty (ids::bus, busId, &pr.undo);
        });
    }
    m.addSubMenu ("Send To", to);
    m.addItem ("Show Aux", [this, s] { studio.selectTrack ((int) s[ids::bus]); studio.setPane (Studio::Pane::rig); });
    m.addSeparator();
    m.addItem ("Remove Send", [this, s]
    {
        auto& pr = studio.project();
        pr.undo.beginNewTransaction ("Remove Send");
        track.removeChild (s, &pr.undo);
    });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
}

//==============================================================================
void SlotButton::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const bool hover = isMouseOver();
    drawChip (g, b, hover, isMouseButtonDown());
    const bool on = lit != nullptr && lit();
    g.setFont (theme::font (11.0f, theme::Weight::medium));
    g.setColour (on ? theme::accent : hover ? theme::text : theme::text.withAlpha (0.85f));
    g.drawFittedText (text != nullptr ? text() : juce::String(), b.reduced (6.0f, 0.0f).toNearestInt(),
                      juce::Justification::centred, 1, 0.75f);
}

void SlotButton::mouseUp (const juce::MouseEvent& e)
{
    repaint();
    if (onClick != nullptr && getLocalBounds().contains (e.getPosition()))
        onClick();
}

//==============================================================================
void IconButton::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const bool hover = isMouseOver();
    drawChip (g, b, hover, isMouseButtonDown());

    if (icon == Icon::rig)
    {
        const float s = std::round (juce::jmin (b.getWidth(), b.getHeight()) * 0.62f);
        icons::draw (g, icons::rig(), juce::Rectangle<float> (s, s).withCentre (b.getCentre()),
                     hover ? theme::accent : theme::dimText);
    }
}

void IconButton::mouseUp (const juce::MouseEvent& e)
{
    repaint();
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
    const auto clip = b.removeFromTop (4.0f).reduced (1.0f, 0.0f);
    b.removeFromTop (3.0f);

    // Clip light.
    if (clipped)
    {
        g.setColour (theme::meterHigh.withAlpha (0.3f));
        g.fillRoundedRectangle (clip.expanded (1.0f), 2.0f);
    }
    g.setColour (clipped ? theme::meterHigh : theme::meterHigh.withAlpha (0.16f));
    g.fillRoundedRectangle (clip, 1.5f);

    // The well, with two LED columns.
    theme::drawInset (g, b, 3.0f, well);
    const auto wells = b.reduced (2.0f);
    const float barW = std::floor ((wells.getWidth() - 1.0f) * 0.5f);
    constexpr float pitch = 3.0f;
    const int n = juce::jmax (1, (int) (wells.getHeight() / pitch));
    const float stretch = wells.getHeight() / (float) n;          // segments fill the well exactly

    for (int ch = 0; ch < 2; ++ch)
    {
        const float x = wells.getX() + (float) ch * (wells.getWidth() - barW);
        const float lit = meterPos (level[ch]);
        const int holdSeg = hold[ch] > -60.0f ? juce::jlimit (0, n - 1, (int) (meterPos (hold[ch]) * (float) n)) : -1;

        for (int i = 0; i < n; ++i)
        {
            const float segPos = ((float) i + 0.5f) / (float) n;
            const auto colour = segmentColour (meterDb (segPos));
            const bool on = segPos <= lit;
            const bool peak = i == holdSeg;
            g.setColour (peak ? colour.brighter (0.2f) : on ? colour : colour.withAlpha (0.06f));
            g.fillRect (x, wells.getBottom() - (float) (i + 1) * stretch + 1.0f, barW, stretch - 1.0f);
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
        int gap, chipH, panH, buttonH;

        explicit Metrics (int height)
            : tall (height >= 340), readouts (height >= 230),
              gap (tall ? 8 : 4), chipH (20), panH (tall ? 46 : 22), buttonH (20) {}

        /** Everything above the fader (identical on the master so the faders line up). */
        int controlsHeight() const
        {
            const int io = tall ? chipH * 5 + 16 : chipH;
            const int buttons = tall ? buttonH * 2 + 4 : buttonH;
            return io + gap + panH + gap + buttons;
        }
    };}

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
        rig = std::make_unique<mixer::Toggle> (s, tree, ids::rigOn, "Rig On/Off", "Rig", theme::loop, mixer::Toggle::Glyph::led);
        mute = std::make_unique<mixer::Toggle> (s, tree, ids::mute, "Mute", "M", theme::mute, mixer::Toggle::Glyph::text);
        solo = std::make_unique<mixer::Toggle> (s, tree, ids::solo, "Solo", "S", theme::solo, mixer::Toggle::Glyph::text);
        arm = std::make_unique<mixer::Toggle> (s, tree, ids::arm, "Record Arm", juce::String(), theme::record, mixer::Toggle::Glyph::dot);
        monitor = std::make_unique<mixer::Toggle> (s, tree, ids::monitor, "Input Monitor", juce::String(), theme::monitor, mixer::Toggle::Glyph::headphones);
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

        // Instrument tracks show their instrument where audio tracks show their input and rig.
        instrumentTrack = daw::Project::isInstrumentTrack (tree);
        auxTrack = daw::Project::isAuxTrack (tree);
        for (int i = 0; i < 2; ++i)
        {
            sendSlots[i] = std::make_unique<mixer::SendSlot> (s, tree, i);
            addAndMakeVisible (*sendSlots[i]);
        }
        instrumentSlot = std::make_unique<mixer::SlotButton>();
        instrumentSlot->setTooltip ("This track's instrument: click to edit its sound");
        instrumentSlot->text = [this]
        {
            if (auxTrack)
                return juce::String ("Aux Bus");
            const auto inst = tree.getChildWithName (ids::INSTRUMENT);
            if (inst[ids::type].toString() == "plugin")
                return inst[ids::patch].toString();
            if (auto* type = InstrumentRegistry::get().find (inst[ids::type].toString()))
                return type->info.name;
            return juce::String ("Instrument");
        };
        instrumentSlot->onClick = [this] { select(); studio.setPane (Studio::Pane::rig); };

        fxSlot = std::make_unique<mixer::SlotButton>();
        fxSlot->setTooltip ("Audio FX: effects after the " + juce::String (instrumentTrack ? "instrument" : "rig") + ". Click to edit.");
        fxSlot->text = [this]
        {
            const auto n = daw::Project::inserts (tree).size();
            return n == 0 ? juce::String ("Audio FX") : juce::String (n) + (n == 1 ? " effect" : " effects");
        };
        fxSlot->lit = [this] { return ! daw::Project::inserts (tree).isEmpty(); };
        fxSlot->onClick = [this] { showInserts(); };

        if (auxTrack)
        {
            instrumentSlot->setTooltip ("An aux bus: tracks reach it through their sends. Click to edit its effects.");
            for (auto* c : std::initializer_list<juce::Component*> { instrumentSlot.get(), fxSlot.get(), mute.get(), solo.get(), pan.get() })
                addAndMakeVisible (c);
        }
        else if (instrumentTrack)
            for (auto* c : std::initializer_list<juce::Component*> { instrumentSlot.get(), fxSlot.get(), mute.get(), solo.get(), arm.get(), pan.get() })
                addAndMakeVisible (c);
        else
            for (auto* c : std::initializer_list<juce::Component*> { input.get(), rig.get(), mute.get(), solo.get(), arm.get(),
                                                                     monitor.get(), openRig.get(), pan.get(), fxSlot.get() })
                addAndMakeVisible (c);
    }

    if (master)
    {
        // The Stereo Out's own effects: mastering EQ, compression, a limiter.
        fxSlot = std::make_unique<mixer::SlotButton>();
        fxSlot->setTooltip ("Master effects: on the whole mix, before the master fader. Click to edit.");
        fxSlot->text = [this]
        {
            const auto n = daw::Project::inserts (tree.getChildWithName (ids::MASTER)).size();
            return n == 0 ? juce::String ("Master FX") : juce::String (n) + (n == 1 ? " effect" : " effects");
        };
        fxSlot->lit = [this] { return ! daw::Project::inserts (tree.getChildWithName (ids::MASTER)).isEmpty(); };
        fxSlot->onClick = [this] { showInserts(); };
        addAndMakeVisible (*fxSlot);
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

void ChannelStrip::showInserts()
{
    select();
    if (master)
    {
        auto editor = std::make_unique<InsertsEditor> (studio);
        editor->setTrack (studio.project().master());
        editor->setSize (320, 380);
        juce::CallOutBox::launchAsynchronously (std::move (editor), fxSlot->getScreenBounds(), nullptr);
        return;
    }
    if (instrumentTrack || auxTrack)
    {
        studio.setPane (Studio::Pane::rig);     // the instrument panel has the effects beside it
        return;
    }
    auto editor = std::make_unique<InsertsEditor> (studio);
    editor->setTrack (tree);
    editor->setSize (320, 380);
    juce::CallOutBox::launchAsynchronously (std::move (editor), fxSlot->getScreenBounds(), nullptr);
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

    // Body: flat graphite, raised a touch when selected; a hairline on the right separates strips.
    g.setColour (selected ? theme::panelLight : master ? theme::bar : theme::panel);
    g.fillRect (b);
    g.setColour (theme::highlight);
    g.fillRect (0.0f, 0.0f, b.getWidth(), 1.0f);
    if (selected)
        g.fillRect (0.0f, 0.0f, 1.0f, b.getHeight());
    g.setColour (theme::separator);
    if (master)
        g.fillRect (0.0f, 0.0f, 1.0f, b.getHeight());
    else
        g.fillRect (b.getRight() - 1.0f, 0.0f, 1.0f, b.getHeight());

    // Name plate: a slim colour bar over the name.
    {
        auto plate = nameArea.toFloat();
        const auto bar = plate.removeFromTop (3.0f).reduced (8.0f, 0.0f);
        g.setColour (master ? theme::dimText.withAlpha (0.6f) : trackColour());
        g.fillRoundedRectangle (bar, 1.5f);

        const auto name = master ? juce::String ("Stereo Out") : tree[ids::name].toString();
        g.setColour (selected ? theme::accent : theme::text);
        g.setFont (theme::font (12.0f, selected || master ? theme::Weight::semibold : theme::Weight::medium));
        g.drawFittedText (name, plate.toNearestInt().reduced (6, 0), juce::Justification::centred, 1, 0.85f);
    }

    if (master)
    {
        // The master has no inputs; label its bay instead.
        auto t = topArea.toFloat();
        g.setColour (theme::faintText);
        g.setFont (theme::font (11.0f, theme::Weight::medium));
        g.drawText ("Master", t.removeFromTop (20.0f), juce::Justification::centred, false);
    }

    // Readouts: fader value and peak hold.
    if (! faderReadout.isEmpty())
    {
        const bool clip = meter.isClipped();
        theme::drawInset (g, faderReadout.toFloat(), theme::radiusChip);
        theme::drawInset (g, peakReadout.toFloat(), theme::radiusChip, 
                         juce::Colour (0xff111113).interpolatedWith (theme::meterHigh, clip ? 0.18f : 0.0f));

        g.setFont (theme::readout (11.0f, theme::Weight::medium));
        const double db = (double) tree.getProperty (master ? ids::masterVolume : ids::volume, 0.0);
        g.setColour (theme::text);
        g.drawText (mixer::dbText (db), faderReadout, juce::Justification::centred, false);

        const auto peak = shownPeak.isEmpty() ? juce::String ("-inf") : shownPeak;
        g.setColour (clip ? theme::meterHigh : peak == "-inf" ? theme::faintText : theme::dimText);
        g.drawText (peak, peakReadout, juce::Justification::centred, false);
    }
}

void ChannelStrip::resized()
{
    const Metrics m (getHeight());
    auto r = getLocalBounds().reduced (7, 0);

    r.removeFromTop (m.tall ? 8 : 6);
    r.removeFromBottom (4);
    nameArea = juce::Rectangle<int> (0, r.getBottom() - 22, getWidth(), 22);
    r.removeFromBottom (22 + m.gap);

    // Readouts above the name.
    if (m.readouts)
    {
        auto row = r.removeFromBottom (18);
        faderReadout = row.removeFromLeft ((row.getWidth() - 4) / 2);
        row.removeFromLeft (4);
        peakReadout = row;
        r.removeFromBottom (m.gap);
    }
    else
    {
        faderReadout = peakReadout = {};
    }

    topArea = r.removeFromTop (m.controlsHeight());
    r.removeFromTop (m.gap);

    if (master)
        fxSlot->setBounds (topArea.withTrimmedTop (24).withHeight (m.chipH));

    if (! master)
    {
        auto c = topArea;
        const int ioBottom = c.getY() + (m.tall ? m.chipH * 5 + 16 : m.chipH);
        for (auto& slot : sendSlots)
            slot->setBounds ({});
        if (m.tall)
        {
            // The sends sit in the last two rows of the io block, whatever the track kind.
            sendSlots[1]->setBounds (c.getX(), ioBottom - m.chipH, c.getWidth(), m.chipH);
            sendSlots[0]->setBounds (c.getX(), ioBottom - m.chipH * 2 - 4, c.getWidth(), m.chipH);
        }
        if (instrumentTrack || auxTrack)
        {
            if (m.tall)
            {
                instrumentSlot->setBounds (c.removeFromTop (m.chipH));
                c.removeFromTop (4);
                fxSlot->setBounds (c.removeFromTop (m.chipH));
            }
            else
            {
                auto row = c.removeFromTop (m.chipH);
                fxSlot->setBounds (row.removeFromRight (row.getWidth() / 3));
                row.removeFromRight (3);
                instrumentSlot->setBounds (row);
            }
        }
        else if (m.tall)
        {
            input->setBounds (c.removeFromTop (m.chipH));
            c.removeFromTop (4);
            auto row = c.removeFromTop (m.chipH);
            openRig->setBounds (row.removeFromRight (m.chipH + 2));
            row.removeFromRight (4);
            rig->setBounds (row);
            c.removeFromTop (4);
            fxSlot->setBounds (c.removeFromTop (m.chipH));
        }
        else
        {
            auto row = c.removeFromTop (m.chipH);
            openRig->setBounds (row.removeFromRight (m.chipH));
            row.removeFromRight (3);
            rig->setBounds (row.removeFromRight (m.chipH + 2));
            row.removeFromRight (3);
            input->setBounds (row);
            fxSlot->setBounds ({});
        }
        c.setTop (ioBottom);
        c.removeFromTop (m.gap);
        pan->setBounds (c.removeFromTop (m.panH));
        c.removeFromTop (m.gap);

        mixer::Toggle* buttons[] { mute.get(), solo.get(), arm.get(), monitor.get() };
        if (instrumentTrack || auxTrack)
        {
            auto line = c.removeFromTop (m.buttonH);
            const int count = auxTrack ? 2 : 3;
            const int w = (line.getWidth() - 3 * (count - 1)) / count;
            for (int i = 0; i < count; ++i)
            {
                buttons[i]->setBounds (i < count - 1 ? line.removeFromLeft (w) : line);
                line.removeFromLeft (3);
            }
        }
        else if (m.tall)
        {
            for (int row = 0; row < 2; ++row)
            {
                auto line = c.removeFromTop (m.buttonH);
                const int w = (line.getWidth() - 4) / 2;
                buttons[row * 2]->setBounds (line.removeFromLeft (w));
                line.removeFromLeft (4);
                buttons[row * 2 + 1]->setBounds (line);
                c.removeFromTop (4);
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
    const int meterW = master ? 16 : 12;
    const int faderW = mixer::Fader::scaleWidth + mixer::Fader::slotWidth;
    const int group = faderW + 2 + meterW;
    const int x = r.getX() + (r.getWidth() - group) / 2 - 2;
    fader.setBounds (x, r.getY(), faderW, r.getHeight());
    const auto span = fader.travel();
    const int meterTop = r.getY() + (int) std::floor (span.getStart()) - 9;
    meter.setBounds (x + faderW + 2, meterTop, meterW, (int) std::ceil (span.getLength()) + 11);
}

void ChannelStrip::mouseDown (const juce::MouseEvent& e)
{
    select();
    if (e.eventComponent == this && peakReadout.contains (e.getPosition()))
        meter.resetPeaks();
}

} // namespace woodshed::app
