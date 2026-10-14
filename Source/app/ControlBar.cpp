#include "ControlBar.h"

namespace woodshed::app
{

namespace
{
    using daw::ids::tempo;

    constexpr double minTempo = 20.0, maxTempo = 300.0;
    constexpr double minMasterDb = -60.0, maxMasterDb = 6.0;

    const juce::Colour inset { 0xff0b0a09 };

    juce::String formatTempo (double bpm)
    {
        return std::abs (bpm - std::round (bpm)) < 0.005 ? juce::String (juce::roundToInt (bpm))
                                                         : juce::String (bpm, 2);
    }

    juce::String formatRate (double sampleRate)
    {
        auto k = juce::String (sampleRate / 1000.0, 1);
        if (k.endsWith (".0"))
            k = k.dropLastCharacters (2);
        return k + "k";
    }

    juce::String formatDb (double db)
    {
        if (db <= minMasterDb + 0.05)
            return "-inf dB";
        return (db > 0.05 ? "+" : "") + juce::String (db, 1) + " dB";
    }

    void noFocus (juce::Component& c)
    {
        // Keys belong to the main window's shortcuts (Space must never "press" the last button clicked).
        c.setWantsKeyboardFocus (false);
        c.setMouseClickGrabsKeyboardFocus (false);
    }
}

//==============================================================================
ControlBar::IconButton::IconButton (Glyph g, juce::Colour lc, const juce::String& tip)
    : juce::Button (tip), glyph (g), litColour (lc)
{
    setTooltip (tip);
    noFocus (*this);
}

void ControlBar::IconButton::setLit (bool shouldBeLit)
{
    if (lit != shouldBeLit)
    {
        lit = shouldBeLit;
        repaint();
    }
}

void ControlBar::IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto b = getLocalBounds().toFloat().reduced (1.0f);

    if (lit)
    {
        g.setColour (litColour.withAlpha (down ? 0.28f : 0.18f));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (litColour.withAlpha (0.55f));
        g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 1.0f);
    }
    else if (highlighted || down)
    {
        g.setColour (down ? theme::panel : theme::panelLight);
        g.fillRoundedRectangle (b, 4.0f);
    }

    juce::Colour ink = lit ? litColour
                           : (glyph == Glyph::record ? theme::record.withAlpha (highlighted ? 0.95f : 0.72f)
                                                     : theme::text.withAlpha (highlighted ? 1.0f : 0.72f));
    g.setColour (ink);

    const auto c = b.getCentre();
    juce::Path p;

    auto triangle = [&p] (float x, float cy, float w, float h, bool right)
    {
        if (right)
            p.addTriangle (x, cy - h * 0.5f, x, cy + h * 0.5f, x + w, cy);
        else
            p.addTriangle (x + w, cy - h * 0.5f, x + w, cy + h * 0.5f, x, cy);
    };

    switch (glyph)
    {
        case Glyph::play:       triangle (c.x - 5.0f, c.y, 12.0f, 13.0f, true); break;
        case Glyph::stop:       p.addRoundedRectangle (c.x - 5.5f, c.y - 5.5f, 11.0f, 11.0f, 1.5f); break;
        case Glyph::record:     p.addEllipse (c.x - 6.5f, c.y - 6.5f, 13.0f, 13.0f); break;
        case Glyph::toStart:
            p.addRectangle (c.x - 6.5f, c.y - 6.0f, 2.2f, 12.0f);
            triangle (c.x - 3.5f, c.y, 10.0f, 12.0f, false);
            break;
        case Glyph::barBack:
            triangle (c.x - 8.0f, c.y, 8.0f, 11.0f, false);
            triangle (c.x, c.y, 8.0f, 11.0f, false);
            break;
        case Glyph::barForward:
            triangle (c.x - 8.0f, c.y, 8.0f, 11.0f, true);
            triangle (c.x, c.y, 8.0f, 11.0f, true);
            break;
        case Glyph::loop:
        {
            juce::Path ring;
            ring.addRoundedRectangle (c.x - 8.0f, c.y - 5.0f, 16.0f, 10.0f, 4.0f);
            g.strokePath (ring, juce::PathStrokeType (1.8f));
            // Arrowheads riding the ring: right along the top, left along the bottom.
            triangle (c.x - 1.0f, c.y - 5.0f, 5.0f, 6.5f, true);
            triangle (c.x - 4.0f, c.y + 5.0f, 5.0f, 6.5f, false);
            break;
        }
    }

    if (lit && glyph == Glyph::record)
    {
        g.setColour (litColour.withAlpha (0.35f));
        g.fillEllipse (c.x - 9.0f, c.y - 9.0f, 18.0f, 18.0f);
        g.setColour (ink);
    }

    g.fillPath (p);
}

//==============================================================================
ControlBar::PillButton::PillButton (const juce::String& text, bool withLed, const juce::String& tip)
    : juce::Button (text), led (withLed)
{
    setTooltip (tip);
    noFocus (*this);
}

void ControlBar::PillButton::setLit (bool shouldBeLit)
{
    if (lit != shouldBeLit)
    {
        lit = shouldBeLit;
        repaint();
    }
}

void ControlBar::PillButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto b = getLocalBounds().toFloat();

    if (led)
    {
        // A switch printed on the readout: an LED and a label, no box.
        const float d = 6.0f;
        const juce::Rectangle<float> dot (b.getX() + 2.0f, b.getCentreY() - d * 0.5f, d, d);
        if (lit)
        {
            g.setColour (theme::accent.withAlpha (0.3f));
            g.fillEllipse (dot.expanded (2.5f));
            g.setColour (theme::accent);
        }
        else
        {
            g.setColour (theme::outline.brighter (highlighted ? 0.4f : 0.15f));
        }
        g.fillEllipse (dot);

        g.setColour (lit ? theme::text : theme::dimText.withAlpha (highlighted ? 1.0f : 0.8f));
        g.setFont (theme::readout (10.0f));
        g.drawText (getButtonText(), b.withTrimmedLeft (d + 8.0f), juce::Justification::centredLeft, false);
        return;
    }

    b = b.reduced (0.5f);
    if (lit)
    {
        g.setColour (theme::accent.withAlpha (down ? 0.3f : 0.18f));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (theme::accent.withAlpha (0.7f));
    }
    else
    {
        g.setColour (down ? theme::panel : (highlighted ? theme::panelLight : theme::panel));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (theme::outline.brighter (highlighted ? 0.3f : 0.0f));
    }
    g.drawRoundedRectangle (b, 4.0f, 1.0f);

    g.setColour (lit ? theme::accent : theme::text.withAlpha (highlighted ? 1.0f : 0.82f));
    g.setFont (theme::font (12.5f, true));
    g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
}

//==============================================================================
ControlBar::Readout::Readout (ControlBar& o) : owner (o)
{
    addAndMakeVisible (click);
    addAndMakeVisible (countIn);
}

void ControlBar::Readout::resized()
{
    auto r = getLocalBounds().reduced (10, 0);
    const int h = getHeight();

    auto toggles = r.removeFromRight (juce::jmin (92, r.getWidth() / 4));
    click.setBounds (toggles.getX(), h / 2 - 19, toggles.getWidth(), 17);
    countIn.setBounds (toggles.getX(), h / 2 + 2, toggles.getWidth(), 17);
    r.removeFromRight (10);

    signatureArea = r.removeFromRight (56);
    r.removeFromRight (10);
    tempoArea = r.removeFromRight (78);
    r.removeFromRight (10);

    positionArea = r.withHeight (30).withY (4);
    clockArea = r.withHeight (14).withY (32);
}

void ControlBar::Readout::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();

    // A dark inset: shadow along the top edge, a faint lip along the bottom.
    g.setColour (inset);
    g.fillRoundedRectangle (b, 5.0f);
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.drawRoundedRectangle (b.reduced (0.5f), 5.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.045f));
    g.drawHorizontalLine (getHeight() - 1, 6.0f, b.getRight() - 6.0f);

    // Dividers between the fields.
    g.setColour (theme::outline);
    for (auto x : { tempoArea.getX() - 5, signatureArea.getX() - 5, click.getX() - 5 })
        g.fillRect (x, 9, 1, getHeight() - 18);

    g.setColour (countingIn ? theme::record : theme::text);
    g.setFont (theme::readout (25.0f));
    g.drawText (positionText, positionArea, juce::Justification::centredLeft, false);

    g.setColour (countingIn ? theme::record.withAlpha (0.8f) : theme::dimText);
    g.setFont (theme::readout (11.0f));
    g.drawText (clockText, clockArea, juce::Justification::centredLeft, false);

    auto field = [&g] (juce::Rectangle<int> area, const juce::String& value, const juce::String& label, bool hot)
    {
        g.setColour (hot ? theme::accent : theme::text);
        g.setFont (theme::readout (19.0f));
        g.drawText (value, area.withHeight (26).withY (5), juce::Justification::centred, false);
        g.setColour (theme::dimText);
        g.setFont (theme::readout (9.5f));
        g.drawText (label, area.withHeight (13).withY (32), juce::Justification::centred, false);
    };

    if (tempoEditor == nullptr)
        field (tempoArea, tempoText, "BPM", draggingTempo);
    field (signatureArea, signatureText, "TIME", false);
}

void ControlBar::Readout::projectChanged()
{
    const auto& s = owner.studio.project().state;
    const auto t = formatTempo ((double) s.getProperty (tempo, 120.0));
    const auto sig = juce::String ((int) s.getProperty (daw::ids::beatsPerBar, 4)) + "/"
                   + juce::String ((int) s.getProperty (daw::ids::beatUnit, 4));

    if (t != tempoText)       { tempoText = t; repaint (tempoArea); }
    if (sig != signatureText) { signatureText = sig; repaint (signatureArea); }

    click.setLit ((bool) s.getProperty (daw::ids::click, true));
    countIn.setLit ((bool) s.getProperty (daw::ids::countIn, true));
    poll();
}

void ControlBar::Readout::poll()
{
    auto& e = owner.studio.engine();
    const auto grid = owner.studio.project().grid();
    const auto pos = e.getPosition();
    const bool ci = e.isCountingIn();

    juce::String p, c;
    if (ci)
    {
        // Before the take starts: the beats left to wait, and where recording will begin.
        const auto from = e.getRecordStart();
        const int beatsLeft = juce::jmax (1, (int) std::ceil ((double) (from - pos) / grid.samplesPerBeat()));
        p = "COUNT " + juce::String (beatsLeft);
        c = "REC FROM " + grid.toBarsBeats (from);
    }
    else
    {
        p = grid.toBarsBeats (pos);
        c = grid.toClock (pos);
    }

    if (ci != countingIn)
    {
        countingIn = ci;
        repaint (positionArea.getUnion (clockArea));
    }
    if (p != positionText) { positionText = p; repaint (positionArea); }
    if (c != clockText)    { clockText = c; repaint (clockArea); }
}

void ControlBar::Readout::mouseMove (const juce::MouseEvent& e)
{
    if (tempoArea.contains (e.getPosition()))
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    else if (signatureArea.contains (e.getPosition()))
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void ControlBar::Readout::mouseDown (const juce::MouseEvent& e)
{
    if (tempoArea.contains (e.getPosition()) && tempoEditor == nullptr)
    {
        draggingTempo = true;
        tempoTransactionStarted = false;
        dragStartTempo = (double) owner.studio.project().state.getProperty (tempo, 120.0);
        repaint (tempoArea);
    }
    else if (signatureArea.contains (e.getPosition()))
    {
        showSignatureMenu();
    }
}

void ControlBar::Readout::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingTempo)
        return;

    // Up is faster. Shift drags in hundredths; otherwise whole BPM.
    const bool fine = e.mods.isShiftDown();
    const double step = fine ? 0.02 : 0.25;
    double bpm = dragStartTempo - e.getDistanceFromDragStartY() * step;
    bpm = fine ? std::round (bpm * 100.0) / 100.0 : std::round (bpm);
    bpm = juce::jlimit (minTempo, maxTempo, bpm);

    auto& project = owner.studio.project();
    if (std::abs (bpm - (double) project.state.getProperty (tempo, 120.0)) < 1.0e-6)
        return;

    if (! tempoTransactionStarted)
    {
        project.undo.beginNewTransaction ("Change Tempo");
        tempoTransactionStarted = true;
    }
    project.state.setProperty (tempo, bpm, &project.undo);
}

void ControlBar::Readout::mouseUp (const juce::MouseEvent&)
{
    if (draggingTempo)
    {
        draggingTempo = false;
        repaint (tempoArea);
    }
}

void ControlBar::Readout::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (tempoArea.contains (e.getPosition()))
        editTempo();
}

void ControlBar::Readout::editTempo()
{
    tempoEditor = std::make_unique<juce::TextEditor>();
    auto& ed = *tempoEditor;
    ed.setFont (theme::readout (19.0f));
    ed.setJustification (juce::Justification::centred);
    ed.setInputRestrictions (6, "0123456789.");
    ed.setColour (juce::TextEditor::backgroundColourId, theme::panel);
    ed.setColour (juce::TextEditor::textColourId, theme::text);
    ed.setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.35f));
    ed.setColour (juce::TextEditor::outlineColourId, theme::accent);
    ed.setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
    ed.setColour (juce::CaretComponent::caretColourId, theme::accent);
    ed.setText (tempoText, false);
    ed.setBounds (tempoArea.withHeight (26).withY (5));
    addAndMakeVisible (ed);
    ed.selectAll();
    ed.grabKeyboardFocus();
    repaint (tempoArea);

    // Closing happens after the editor's own callback has returned.
    auto finish = [this] (bool commit)
    {
        if (tempoEditor == nullptr)
            return;

        if (commit)
        {
            const double bpm = tempoEditor->getText().getDoubleValue();
            auto& project = owner.studio.project();
            if (bpm >= minTempo && bpm <= maxTempo
                && std::abs (bpm - (double) project.state.getProperty (tempo, 120.0)) > 1.0e-6)
            {
                project.undo.beginNewTransaction ("Set Tempo");
                project.state.setProperty (tempo, bpm, &project.undo);
            }
        }

        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Readout> (this)]
        {
            if (safe != nullptr && safe->tempoEditor != nullptr)
            {
                safe->tempoEditor.reset();
                safe->repaint (safe->tempoArea);
                if (auto* top = safe->getTopLevelComponent())
                    top->grabKeyboardFocus();
            }
        });
    };

    ed.onReturnKey = [finish] { finish (true); };
    ed.onEscapeKey = [finish] { finish (false); };
    ed.onFocusLost = [finish] { finish (true); };
}

void ControlBar::Readout::showSignatureMenu()
{
    static constexpr std::pair<int, int> signatures[] {
        { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 4 }, { 7, 4 },
        { 3, 8 }, { 5, 8 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 }
    };

    const auto& s = owner.studio.project().state;
    const int beats = (int) s.getProperty (daw::ids::beatsPerBar, 4);
    const int unit = (int) s.getProperty (daw::ids::beatUnit, 4);

    juce::PopupMenu m;
    m.addSectionHeader ("Time Signature");
    for (int i = 0; i < (int) std::size (signatures); ++i)
    {
        if (i == 6)
            m.addSeparator();
        const auto [b, u] = signatures[i];
        m.addItem (i + 1, juce::String (b) + "/" + juce::String (u), true, b == beats && u == unit);
    }

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                               .withTargetScreenArea (localAreaToGlobal (signatureArea)),
                     [safe = juce::Component::SafePointer<Readout> (this)] (int result)
                     {
                         if (safe == nullptr || result <= 0)
                             return;
                         const auto [b, u] = signatures[result - 1];
                         auto& project = safe->owner.studio.project();
                         project.undo.beginNewTransaction ("Change Time Signature");
                         project.state.setProperty (daw::ids::beatsPerBar, b, &project.undo);
                         project.state.setProperty (daw::ids::beatUnit, u, &project.undo);
                     });
}

//==============================================================================
void ControlBar::Meter::setLevels (float left, float right)
{
    bool changed = false;
    const float in[2] { left, right };

    for (int ch = 0; ch < 2; ++ch)
    {
        const float level = juce::jmax (in[ch], levels[ch] * 0.82f);
        float hold = holds[ch];
        if (level >= hold)
        {
            hold = level;
            holdTicks[ch] = 30;
        }
        else if (--holdTicks[ch] <= 0)
        {
            hold *= 0.9f;
        }

        changed = changed || std::abs (level - levels[ch]) > 1.0e-4f || std::abs (hold - holds[ch]) > 1.0e-4f;
        levels[ch] = level < 1.0e-4f ? 0.0f : level;
        holds[ch] = hold < 1.0e-4f ? 0.0f : hold;
    }

    if (changed)
        repaint();
}

void ControlBar::Meter::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (inset);
    g.fillRoundedRectangle (b, 3.0f);

    auto toY = [] (float gain, juce::Rectangle<float> bar)
    {
        const float db = juce::Decibels::gainToDecibels (gain, -60.0f);
        const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
        return bar.getBottom() - norm * bar.getHeight();
    };

    const auto inner = b.reduced (3.0f);
    const float w = (inner.getWidth() - 2.0f) * 0.5f;

    for (int ch = 0; ch < 2; ++ch)
    {
        const juce::Rectangle<float> bar (inner.getX() + (float) ch * (w + 2.0f), inner.getY(), w, inner.getHeight());
        g.setColour (theme::outline.withAlpha (0.6f));
        g.fillRect (bar);

        // Green, then yellow above -12 dB, red above 0 dB.
        juce::ColourGradient grad (theme::meterHigh, 0.0f, bar.getY(), theme::meterLow, 0.0f, bar.getBottom(), false);
        grad.addColour (6.0 / 66.0, theme::meterHigh);
        grad.addColour (7.0 / 66.0, theme::meterMid);
        grad.addColour (18.0 / 66.0, theme::meterMid);
        grad.addColour (24.0 / 66.0, theme::meterLow);
        g.setGradientFill (grad);

        const float top = toY (levels[ch], bar);
        g.fillRect (bar.withTop (top));

        if (holds[ch] > 0.0f)
            g.fillRect (bar.withTop (toY (holds[ch], bar)).withHeight (1.5f));
    }

    // 0 dB tick.
    g.setColour (theme::dimText.withAlpha (0.5f));
    g.fillRect (b.getX(), inner.getBottom() - 60.0f / 66.0f * inner.getHeight(), 2.0f, 1.0f);
}

//==============================================================================
void ControlBar::KnobLook::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                             float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (2.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius - 1.5f, radius - 1.5f, 0.0f, startAngle, endAngle, true);
    g.setColour (theme::outline);
    g.strokePath (track, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, radius - 1.5f, radius - 1.5f, 0.0f, startAngle, angle, true);
    g.setColour (theme::accent);
    g.strokePath (value, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float body = radius - 5.5f;
    juce::ColourGradient grad (theme::panelLight.brighter (0.25f), centre.x, centre.y - body,
                               theme::panel.darker (0.3f), centre.x, centre.y + body, false);
    g.setGradientFill (grad);
    g.fillEllipse (juce::Rectangle<float> (body * 2.0f, body * 2.0f).withCentre (centre));
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawEllipse (juce::Rectangle<float> (body * 2.0f, body * 2.0f).withCentre (centre), 1.0f);

    g.setColour (slider.isMouseOverOrDragging() ? theme::accent : theme::text);
    g.drawLine ({ centre.getPointOnCircumference (body * 0.25f, angle), centre.getPointOnCircumference (body * 0.9f, angle) }, 2.0f);
}

//==============================================================================
ControlBar::ControlBar (Studio& s) : studio (s)
{
    for (auto* b : { &toStart, &barBack, &barForward, &play, &stop, &record, &loop })
        addAndMakeVisible (b);
    for (auto* b : { &audio, &rigPane, &mixerPane, &tuner })
        addAndMakeVisible (b);
    addAndMakeVisible (readout);
    addAndMakeVisible (meter);

    toStart.onClick    = [this] { invoke (cmd::returnToStart); };
    barBack.onClick    = [this] { moveByBars (-1); };
    barForward.onClick = [this] { moveByBars (1); };
    play.onClick       = [this] { invoke (cmd::playStop); };
    stop.onClick       = [this]
    {
        auto& e = studio.engine();
        if (e.isPlaying() || e.isRecording())
            e.stop();
        else
            invoke (cmd::returnToStart);
    };
    record.onClick     = [this] { invoke (cmd::record); };
    loop.onClick       = [this] { invoke (cmd::toggleLoop); };

    readout.click.onClick   = [this] { invoke (cmd::toggleClick); };
    readout.countIn.onClick = [this] { invoke (cmd::toggleCountIn); };

    audio.onClick     = [this] { invoke (cmd::audioSettings); };
    rigPane.onClick   = [this] { invoke (cmd::showRig); };
    mixerPane.onClick = [this] { invoke (cmd::showMixer); };
    tuner.onClick     = [this] { invoke (cmd::showTuner); };

    master.setLookAndFeel (&knobLook);
    master.setRange (minMasterDb, maxMasterDb, 0.1);
    master.setSkewFactorFromMidPoint (-12.0);
    master.setDoubleClickReturnValue (true, 0.0);
    master.setTooltip ("Master volume (double-click: 0 dB)");
    noFocus (master);
    master.onDragStart = [this]
    {
        studio.project().undo.beginNewTransaction ("Master Volume");
        knobDragging = true;
    };
    master.onDragEnd = [this] { knobDragging = false; };
    master.onValueChange = [this]
    {
        auto& project = studio.project();
        if (! knobDragging)
            project.undo.beginNewTransaction ("Master Volume");
        project.state.setProperty (daw::ids::masterVolume, master.getValue(), &project.undo);
    };
    addAndMakeVisible (master);

    studio.addChangeListener (this);
    studio.engine().addChangeListener (this);
    studio.deviceManager.addChangeListener (this);

    attachToProject();
    refreshTransport();
    refreshStatus();
    startTimerHz (30);
}

ControlBar::~ControlBar()
{
    stopTimer();
    watched.removeListener (this);
    studio.deviceManager.removeChangeListener (this);
    studio.engine().removeChangeListener (this);
    studio.removeChangeListener (this);
    master.setLookAndFeel (nullptr);
}

//==============================================================================
juce::String ControlBar::projectTitle() const
{
    auto name = studio.project().state[daw::ids::name].toString();
    return name.isNotEmpty() ? name : studio.project().folder.getFileName();
}

juce::Rectangle<int> ControlBar::titleArea() const
{
    return { 16, 0, juce::jmax (0, toStart.getX() - 26), getHeight() };
}

void ControlBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::bar);
    g.setColour (theme::outline);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);

    // Wordmark, as on the rig's top bar, with the project (or the latest message) underneath.
    const auto title = titleArea();
    g.setColour (theme::text);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold).withKerningFactor (0.22f));
    g.drawText ("WOODSHED", title.withY (9).withHeight (26), juce::Justification::centredLeft, false);

    const bool flashing = shownFlash.isNotEmpty();
    g.setColour (flashing ? theme::accent : theme::dimText);
    g.setFont (theme::font (12.0f, flashing));
    g.drawFittedText (flashing ? shownFlash : shownTitle, title.withY (37).withHeight (16),
                      juce::Justification::centredLeft, 1, 0.9f);

    // The transport sits in a slightly raised tray.
    const auto tray = toStart.getBounds().getUnion (loop.getBounds()).expanded (4).toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (tray, 6.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (tray.reduced (0.5f), 6.0f, 1.0f);
    g.fillRect ((float) loop.getX() - 5.0f, tray.getY() + 7.0f, 1.0f, tray.getHeight() - 14.0f);

    if (! statusArea.isEmpty())
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (12.0f));
        g.drawFittedText (statusText, statusArea, juce::Justification::centredRight, 1, 0.85f);
    }

    g.setColour (theme::dimText);
    g.setFont (theme::readout (9.5f));
    g.drawText (formatDb (master.getValue()), masterLabelArea, juce::Justification::centred, false);
}

void ControlBar::resized()
{
    auto r = getLocalBounds();
    const int midY = getHeight() / 2;

    // Left: title, then the transport.
    r.removeFromLeft (16);
    r.removeFromLeft (190);
    r.removeFromLeft (10);

    constexpr int bw = 34, bh = 32, gap = 3;
    int x = r.getX();
    for (auto* b : { &toStart, &barBack, &barForward, &play, &stop, &record })
    {
        b->setBounds (x, midY - bh / 2, bw, bh);
        x += bw + gap;
    }
    x += 8;
    loop.setBounds (x, midY - bh / 2, bw, bh);
    r.setLeft (loop.getRight() + 16);

    // Right, from the edge inwards: meter, master knob, panes, audio, status.
    r.removeFromRight (14);
    meter.setBounds (r.removeFromRight (18).withSizeKeepingCentre (18, 46));
    r.removeFromRight (6);
    auto knob = r.removeFromRight (46);
    master.setBounds (knob.getX() + 3, 3, 40, 40);
    masterLabelArea = { knob.getX() - 6, 44, knob.getWidth() + 12, 14 };
    r.removeFromRight (12);

    auto pill = [&] (PillButton& b, int w)
    {
        b.setBounds (r.removeFromRight (w).withSizeKeepingCentre (w, 26));
    };
    pill (tuner, 54);
    r.removeFromRight (10);
    pill (mixerPane, 58);
    r.removeFromRight (4);
    pill (rigPane, 44);
    r.removeFromRight (10);
    pill (audio, 56);
    r.removeFromRight (10);

    // Centre: the readout takes what it needs; the status line gets the rest, if there's room.
    const int readoutWidth = juce::jmin (440, r.getWidth());
    const int spare = r.getWidth() - readoutWidth;
    statusArea = spare >= 90 ? r.removeFromRight (juce::jmin (150, spare - 12)) : juce::Rectangle<int>();
    readout.setBounds (r.withSizeKeepingCentre (juce::jmin (readoutWidth, r.getWidth()), 50));
}

//==============================================================================
void ControlBar::invoke (juce::CommandID id)
{
    studio.commands.invokeDirectly (id, false);
}

void ControlBar::moveByBars (int bars)
{
    auto& e = studio.engine();
    const auto grid = studio.project().grid();
    const double barLength = grid.samplesPerBar();
    const double here = (double) e.getPosition() / barLength;
    double target = std::floor (here + 1.0e-6);

    if (bars < 0)
    {
        // Mid-bar, go back to the bar's start; at (or, while playing, just past) a barline, the bar before.
        const double slack = e.isPlaying() ? 0.25 : 0.02;
        if (here - target < slack)
            target -= 1.0;
    }
    else
    {
        target += 1.0;
    }

    e.setPosition ((juce::int64) std::llround (juce::jmax (0.0, target) * barLength));
}

void ControlBar::attachToProject()
{
    if (watched != studio.project().state)
    {
        // Holding the old tree keeps it alive, so this is safe after the old project is gone.
        watched.removeListener (this);
        watched = studio.project().state;
        watched.addListener (this);
    }
    refreshFromProject();
}

void ControlBar::refreshFromProject()
{
    const auto& s = studio.project().state;
    loop.setLit ((bool) s.getProperty (daw::ids::loopOn, false));
    master.setValue ((double) s.getProperty (daw::ids::masterVolume, 0.0), juce::dontSendNotification);
    repaint (masterLabelArea);
    readout.projectChanged();

    const auto t = projectTitle();
    if (t != shownTitle)
    {
        shownTitle = t;
        repaint (titleArea());
    }
}

void ControlBar::refreshTransport()
{
    auto& e = studio.engine();
    play.setLit (e.isPlaying());
    // Lit while recording; blinking while it counts in.
    record.setLit (e.isRecording() && (! e.isCountingIn() || (tick / 6) % 2 == 0));

    rigPane.setLit (studio.getPane() == Studio::Pane::rig);
    mixerPane.setLit (studio.getPane() == Studio::Pane::mixer);
}

void ControlBar::refreshStatus()
{
    juce::String text = "No audio device";
    if (auto* device = studio.deviceManager.getCurrentAudioDevice())
    {
        const double rate = device->getCurrentSampleRate();
        const int latency = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
        const double ms = rate > 0.0 ? latency * 1000.0 / rate : 0.0;
        text = juce::String (ms, 1) + " ms" + juce::String::fromUTF8 (" \xc2\xb7 ")
             + juce::String (device->getCurrentBufferSizeSamples()) + " @ " + formatRate (rate);
    }

    if (text != statusText)
    {
        statusText = text;
        repaint (statusArea);
    }
}

//==============================================================================
void ControlBar::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &studio.deviceManager)
    {
        refreshStatus();
        return;
    }

    if (source == &studio)
    {
        attachToProject();

        if (studio.getFlash() != shownFlash)
        {
            shownFlash = studio.getFlash();
            repaint (titleArea());
        }
    }

    refreshTransport();
    readout.poll();
}

void ControlBar::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& id)
{
    if (tree != watched)
        return;     // a track or region

    using namespace daw::ids;
    if (id == tempo || id == beatsPerBar || id == beatUnit || id == click || id == countIn
        || id == loopOn || id == masterVolume || id == name || id == sampleRate)
        refreshFromProject();
}

void ControlBar::timerCallback()
{
    ++tick;
    refreshTransport();
    readout.poll();

    const auto levels = studio.engine().takeMasterLevels();
    meter.setLevels (levels.left, levels.right);

    if (tick % 30 == 0)
        refreshStatus();
}

} // namespace woodshed::app
