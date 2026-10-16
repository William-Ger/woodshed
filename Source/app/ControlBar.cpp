#include "ControlBar.h"
#include "WindowStyle.h"

namespace woodshed::app
{

namespace
{
    using daw::ids::tempo;

    constexpr double minTempo = 20.0, maxTempo = 300.0;
    constexpr double minMasterDb = -60.0, maxMasterDb = 6.0;

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
        return k + " kHz";
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
ControlBar::IconButton::IconButton (icons::Icon i, juce::Colour lc, const juce::String& tip, float size)
    : juce::Button (tip), icon (std::move (i)), litColour (lc), iconSize (size)
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
    const auto b = getLocalBounds().toFloat();
    const float radius = juce::jmin (theme::radiusControl, b.getHeight() * 0.5f);

    if (lit)
    {
        g.setColour (litColour.withAlpha (litFill + (down ? 0.12f : highlighted ? 0.06f : 0.0f)));
        g.fillRoundedRectangle (b, radius);
    }
    else if (highlighted || down)
    {
        g.setColour (juce::Colours::white.withAlpha (down ? 0.12f : 0.07f));
        g.fillRoundedRectangle (b, radius);
    }

    const bool isRecord = litColour == theme::record;
    juce::Colour ink = lit ? litColour
                           : isRecord ? theme::record.withAlpha (highlighted ? 1.0f : 0.8f)
                                      : theme::text.withAlpha (highlighted ? 0.95f : 0.72f);
    if (down && ! lit)
        ink = ink.withMultipliedAlpha (0.8f);

    // Stroke icons keep a ~1.5 px line whatever size they're drawn at.
    const auto box = b.withSizeKeepingCentre (iconSize, iconSize);
    icons::draw (g, icon, box, ink, 1.5f * 24.0f / iconSize);
}

//==============================================================================
ControlBar::Segment::Segment (const juce::String& text, icons::Icon i, const juce::String& tip)
    : juce::Button (text), icon (std::move (i))
{
    setTooltip (tip);
    noFocus (*this);
}

void ControlBar::Segment::setSelected (bool shouldBeSelected)
{
    if (selected != shouldBeSelected)
    {
        selected = shouldBeSelected;
        repaint();
    }
}

void ControlBar::Segment::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (2.0f);
    if (selected)
        theme::drawRaised (g, b, theme::radiusControl - 1.5f, down ? theme::control : theme::controlHover);
    else if (highlighted || down)
    {
        g.setColour (juce::Colours::white.withAlpha (down ? 0.08f : 0.045f));
        g.fillRoundedRectangle (b, theme::radiusControl - 1.5f);
    }

    const auto font = theme::font (12.5f, theme::Weight::medium);
    const float textW = juce::GlyphArrangement::getStringWidth (font, getButtonText());
    const float iconW = 15.0f, gap = 6.0f;
    const float x = b.getCentreX() - (iconW + gap + textW) * 0.5f;

    const auto ink = selected ? theme::text : theme::dimText.withAlpha (highlighted ? 1.0f : 0.9f);
    icons::draw (g, icon, { x, b.getCentreY() - iconW * 0.5f, iconW, iconW }, selected ? theme::accent : ink);
    g.setColour (ink);
    g.setFont (font);
    g.drawText (getButtonText(), juce::Rectangle<float> (x + iconW + gap, b.getY(), textW + 2.0f, b.getHeight()),
                juce::Justification::centredLeft, false);
}

//==============================================================================
ControlBar::Readout::Readout (ControlBar& o) : owner (o)
{
    for (auto* b : { &click, &countIn })
    {
        b->setLitFill (0.0f);     // in the readout an amber glyph alone means "on"
        addAndMakeVisible (b);
    }
}

void ControlBar::Readout::resized()
{
    auto r = getLocalBounds();
    const int h = getHeight();

    r.removeFromLeft (12);
    r.removeFromRight (6);

    // Click and count-in sit at the right end, as two small icon toggles.
    constexpr int toggle = 26;
    countIn.setBounds (r.removeFromRight (toggle).withSizeKeepingCentre (toggle, toggle));
    r.removeFromRight (2);
    click.setBounds (r.removeFromRight (toggle).withSizeKeepingCentre (toggle, toggle));
    r.removeFromRight (8);

    signatureArea = r.removeFromRight (48).withSizeKeepingCentre (48, h - 8);
    r.removeFromRight (4);
    tempoArea = r.removeFromRight (60).withSizeKeepingCentre (60, h - 8);
    r.removeFromRight (8);

    positionArea = r.withY (h / 2 - 15).withHeight (20);
    clockArea = r.withY (h / 2 + 4).withHeight (12);
}

void ControlBar::Readout::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    theme::drawInset (g, b, theme::radiusPanel);

    // Hover wells for the fields you can drag or click.
    if (hover != Hover::none || draggingTempo)
    {
        const auto area = (draggingTempo || hover == Hover::tempo) ? tempoArea : signatureArea;
        g.setColour (juce::Colours::white.withAlpha (draggingTempo ? 0.08f : 0.05f));
        g.fillRoundedRectangle (area.toFloat(), theme::radiusChip);
    }

    // Hairline dividers between the fields.
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    for (auto x : { tempoArea.getX() - 4, click.getX() - 4 })
        g.fillRect (x, 10, 1, getHeight() - 20);

    g.setColour (countingIn ? theme::record : theme::text);
    g.setFont (theme::readout (17.0f));
    g.drawText (positionText, positionArea, juce::Justification::centredLeft, false);

    g.setColour (countingIn ? theme::record.withAlpha (0.8f) : theme::faintText.brighter (0.25f));
    g.setFont (theme::readout (10.5f, theme::Weight::medium));
    g.drawText (clockText, clockArea.withRight (tempoArea.getX() - 6), juce::Justification::centredLeft, false);

    auto field = [&g, this] (juce::Rectangle<int> area, const juce::String& value, const juce::String& label, bool hot)
    {
        const int h = getHeight();
        g.setColour (hot ? theme::accent : theme::text);
        g.setFont (theme::readout (14.0f));
        g.drawText (value, area.withY (h / 2 - 14).withHeight (18), juce::Justification::centred, false);
        g.setColour (theme::faintText.brighter (0.25f));
        g.setFont (theme::font (10.5f, theme::Weight::medium));
        g.drawText (label, area.withY (h / 2 + 4).withHeight (12), juce::Justification::centred, false);
    };

    if (tempoEditor == nullptr)
        field (tempoArea, tempoText, "bpm", draggingTempo);
    field (signatureArea, signatureText, "time", false);
}

void ControlBar::Readout::projectChanged()
{
    const auto& s = owner.studio.project().state;
    // The tempo where the playhead is (the song's tempo unless it changes along the way).
    const auto& songGrid = owner.studio.project().grid();
    const auto t = formatTempo (songGrid.hasChanges() ? songGrid.tempoAt ((double) owner.studio.engine().getPosition())
                                                      : (double) s.getProperty (tempo, 120.0));
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
        p = "Count " + juce::String (beatsLeft);
        c = "Rec at " + grid.toBarsBeats (from);
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
    const auto p = e.getPosition();
    const auto h = tempoArea.contains (p) ? Hover::tempo : signatureArea.contains (p) ? Hover::signature : Hover::none;
    setMouseCursor (h == Hover::tempo ? juce::MouseCursor::UpDownResizeCursor
                                      : h == Hover::signature ? juce::MouseCursor::PointingHandCursor
                                                              : juce::MouseCursor::NormalCursor);
    if (h != hover)
    {
        hover = h;
        repaint();
    }
}

void ControlBar::Readout::mouseExit (const juce::MouseEvent&)
{
    if (hover != Hover::none)
    {
        hover = Hover::none;
        repaint();
    }
}

void ControlBar::Readout::mouseDown (const juce::MouseEvent& e)
{
    if (tempoArea.contains (e.getPosition()) && tempoEditor == nullptr)
    {
        draggingTempo = true;
        tempoTransactionStarted = false;
        dragStartTempo = (double) owner.studio.project().state.getProperty (tempo, 120.0);
        dragStartGrid = owner.studio.project().grid();
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
    // Just the tempo while dragging; regions go back on their bars once, at the end, so a long
    // drag doesn't round their positions over and over.
    project.state.setProperty (tempo, bpm, &project.undo);
}

void ControlBar::Readout::mouseUp (const juce::MouseEvent&)
{
    if (draggingTempo)
    {
        draggingTempo = false;
        if (tempoTransactionStarted)
            owner.studio.project().keepMusicalPositions (dragStartGrid);
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
    ed.setFont (theme::readout (14.0f));
    ed.setJustification (juce::Justification::centred);
    ed.setInputRestrictions (6, "0123456789.");
    ed.setIndents (2, 1);
    ed.setColour (juce::TextEditor::backgroundColourId, theme::control);
    ed.setColour (juce::TextEditor::textColourId, theme::text);
    ed.setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.35f));
    ed.setColour (juce::TextEditor::outlineColourId, theme::accent);
    ed.setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
    ed.setColour (juce::CaretComponent::caretColourId, theme::accent);
    ed.setText (tempoText, false);
    ed.setBounds (tempoArea.reduced (2, 4));
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
                project.setTempoChange (0.0, bpm);
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
    // Two slim bars, left over right, scaled -60..+6 dB; tinted yellow above -12 dB and red above 0.
    const auto b = getLocalBounds().toFloat();
    const float barH = (b.getHeight() - 1.0f) * 0.5f;
    const float zeroX = b.getX() + b.getWidth() * 60.0f / 66.0f;

    auto toX = [&b] (float gain)
    {
        const float db = juce::Decibels::gainToDecibels (gain, -60.0f);
        return b.getX() + juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f) * b.getWidth();
    };

    juce::ColourGradient grad (theme::meterLow, b.getX(), 0.0f, theme::meterHigh, b.getRight(), 0.0f, false);
    grad.addColour (42.0 / 66.0, theme::meterLow);
    grad.addColour (48.0 / 66.0, theme::meterMid);
    grad.addColour (59.0 / 66.0, theme::meterMid);
    grad.addColour (60.0 / 66.0, theme::meterHigh);

    for (int ch = 0; ch < 2; ++ch)
    {
        const juce::Rectangle<float> bar (b.getX(), b.getY() + (float) ch * (barH + 1.0f), b.getWidth(), barH);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (bar, barH * 0.5f);

        g.setGradientFill (grad);
        if (levels[ch] > 0.0f)
            g.fillRoundedRectangle (bar.withRight (juce::jmax (bar.getX() + barH, toX (levels[ch]))), barH * 0.5f);
        if (holds[ch] > 0.0f)
            g.fillRect (bar.withX (toX (holds[ch]) - 1.5f).withWidth (1.5f));
    }

    // 0 dB tick.
    g.setColour (theme::dimText.withAlpha (0.35f));
    g.fillRect (zeroX, b.getY(), 1.0f, b.getHeight());
}

//==============================================================================
int ControlBar::MasterLook::getSliderThumbRadius (juce::Slider&)    { return 6; }

void ControlBar::MasterLook::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                               juce::Slider::SliderStyle, juce::Slider& s)
{
    // A slim groove with a small round thumb, sized to sit above the meter.
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat();
    const auto groove = bounds.withSizeKeepingCentre (bounds.getWidth(), 3.0f);
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (groove, 1.5f);
    g.setColour (theme::text.withAlpha (s.isMouseOverOrDragging() ? 0.7f : 0.5f));
    g.fillRoundedRectangle (groove.withRight (pos), 1.5f);

    const auto thumb = juce::Rectangle<float> (11.0f, 11.0f).withCentre ({ pos, bounds.getCentreY() });
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillEllipse (thumb.translated (0.0f, 0.75f).expanded (0.5f));
    g.setColour (s.isMouseButtonDown() ? juce::Colour (0xffd0d0d6) : juce::Colour (0xfff2f2f5));
    g.fillEllipse (thumb);
}

//==============================================================================
ControlBar::ControlBar (Studio& s) : studio (s)
{
    for (auto* b : { &toStart, &barBack, &barForward, &play, &stop, &record, &loop })
        addAndMakeVisible (b);
    addAndMakeVisible (readout);
    for (auto* b : { &rigPane, &mixerPane })
        addAndMakeVisible (b);
    for (auto* b : { &tuner, &settings })
        addAndMakeVisible (b);
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

    rigPane.onClick   = [this] { invoke (cmd::showRig); };
    mixerPane.onClick = [this] { invoke (cmd::showMixer); };
    tuner.onClick     = [this] { invoke (cmd::showTuner); };
    settings.onClick  = [this] { invoke (cmd::audioSettings); };

    master.setLookAndFeel (&masterLook);
    master.setRange (minMasterDb, maxMasterDb, 0.1);
    master.setSkewFactorFromMidPoint (-12.0);
    master.setDoubleClickReturnValue (true, 0.0);
    master.setColour (juce::Slider::trackColourId, theme::text.withAlpha (0.55f));
    master.setSliderSnapsToMousePosition (false);
    master.setMouseCursor (juce::MouseCursor::PointingHandCursor);
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
        master.setTooltip ("Master volume: " + formatDb (master.getValue()) + " (double-click: 0 dB)");
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

int ControlBar::leftInset() const
{
    // Under a unified title bar the window buttons float over our top-left corner (but not in full screen).
    if (auto* top = getTopLevelComponent(); top != nullptr && top != this)
        if (auto* peer = top->getPeer())
            if (hasUnifiedTitleBar (*top) && ! peer->isFullScreen())
                return theme::trafficLightInset;
    return 16;
}

void ControlBar::parentHierarchyChanged()
{
    resized();
}

void ControlBar::mouseDown (const juce::MouseEvent& e)
{
    // Empty toolbar space is title bar: drag the window, double-click to zoom.
    startWindowDrag (*this, e);
}

juce::String ControlBar::getTooltip()
{
    const auto pos = getMouseXYRelative();
    if (titleBounds.contains (pos) && pos.y >= getHeight() / 2)
        return deviceDetail;
    return {};
}

void ControlBar::paint (juce::Graphics& g)
{
    g.setGradientFill (juce::ColourGradient::vertical (theme::barTop, 0.0f, theme::bar, (float) getHeight()));
    g.fillAll();
    g.setColour (theme::separator);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);

    // The project leads; the audio status (or the latest message) sits underneath.
    if (! titleBounds.isEmpty())
    {
        const int midY = getHeight() / 2;
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, theme::Weight::semibold));
        g.drawText (shownTitle, titleBounds.withY (midY - 17).withHeight (17), juce::Justification::centredLeft, true);

        const bool flashing = shownFlash.isNotEmpty();
        g.setColour (flashing ? theme::accent : theme::dimText);
        g.setFont (theme::font (11.0f, flashing ? theme::Weight::medium : theme::Weight::regular));
        g.drawText (flashing ? shownFlash : statusText, titleBounds.withY (midY + 1).withHeight (14),
                    juce::Justification::centredLeft, true);
    }

    // The Rig / Mixer switch's track.
    if (! segmentTrack.isEmpty())
    {
        const auto t = segmentTrack.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        g.fillRoundedRectangle (t, theme::radiusControl + 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawRoundedRectangle (t.reduced (0.5f), theme::radiusControl + 1.0f, 1.0f);
    }

    if (! speakerArea.isEmpty())
        icons::draw (g, icons::speaker(), speakerArea.toFloat(), theme::dimText);
}

void ControlBar::resized()
{
    layoutInset = leftInset();
    const int w = getWidth(), h = getHeight(), midY = h / 2;
    constexpr int ctl = 28, margin = 12;

    auto place = [midY] (juce::Component& c, int x, int width, int height = ctl)
    {
        c.setBounds (x, midY - height / 2, width, height);
    };

    // Right, from the edge inwards: master level and meter, settings, tuner, the pane switch.
    int right = w - margin - 4;
    const bool roomy = w >= 1180;
    const int masterW = roomy ? 92 : 72;
    master.setBounds (right - masterW, midY - 11, masterW, 14);
    meter.setBounds (master.getX() + 6, midY + 5, masterW - 12, 5);
    right = master.getX() - 4;
    speakerArea = { right - 16, midY - 10, 16, 16 };
    right = speakerArea.getX() - 14;

    place (settings, right - ctl, ctl);
    right = settings.getX() - 2;
    place (tuner, right - ctl, ctl);
    right = tuner.getX() - 12;

    const auto segFont = theme::font (12.5f, theme::Weight::medium);
    auto segWidth = [&segFont] (const juce::Button& b)
    {
        return juce::roundToInt (juce::GlyphArrangement::getStringWidth (segFont, b.getButtonText())) + 15 + 6 + 24;
    };
    const int mixW = segWidth (mixerPane), rigW = segWidth (rigPane);
    place (mixerPane, right - mixW, mixW);
    place (rigPane, mixerPane.getX() - rigW, rigW);
    segmentTrack = rigPane.getBounds().getUnion (mixerPane.getBounds());
    right = segmentTrack.getX() - 16;

    // Centre: transport then readout, centred on the window when there's room.
    const int left = layoutInset;
    constexpr int bw = ctl, gap = 2, minTitle = 110;
    int available = right - (left + minTitle + 16);

    bool showBarButtons = true;
    int transportW = 7 * bw + 5 * gap + 9;
    int readoutW = Readout::preferredWidth;
    if (transportW + 12 + readoutW > available)
        readoutW = juce::jmax (Readout::minimumWidth, available - transportW - 12);
    if (transportW + 12 + readoutW > available)
    {
        showBarButtons = false;
        transportW -= 2 * (bw + gap);
    }

    const int groupW = transportW + 12 + readoutW;
    int x = (w - groupW) / 2;
    x = juce::jmin (x, right - groupW);
    x = juce::jmax (x, left + minTitle + 16);

    barBack.setVisible (showBarButtons);
    barForward.setVisible (showBarButtons);
    for (auto* b : { &toStart, &barBack, &barForward, &play, &stop, &record })
    {
        if (! b->isVisible())
            continue;
        place (*b, x, bw);
        x += bw + gap;
    }
    x += 7;
    place (loop, x, bw);
    x = loop.getRight() + 12;
    readout.setBounds (x, midY - 20, readoutW, 40);

    titleBounds = { left, 0, juce::jmax (0, toStart.getX() - 16 - left), h };
    repaint();
}

//==============================================================================
void ControlBar::invoke (juce::CommandID id)
{
    studio.commands.invokeDirectly (id, false);
}

void ControlBar::moveByBars (int bars)
{
    auto& e = studio.engine();
    const auto& grid = studio.project().grid();
    const double here = grid.barAt (grid.tickAt ((double) e.getPosition()));
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

    e.setPosition (grid.barStartSample ((int) juce::jmax (0.0, target)));
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
    master.setTooltip ("Master volume: " + formatDb (master.getValue()) + " (double-click: 0 dB)");
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

    rigPane.setSelected (studio.getPane() == Studio::Pane::rig);
    mixerPane.setSelected (studio.getPane() == Studio::Pane::mixer);
}

void ControlBar::refreshStatus()
{
    juce::String text = "No audio device", detail = "Choose an audio interface in Settings";
    if (auto* device = studio.deviceManager.getCurrentAudioDevice())
    {
        const double rate = device->getCurrentSampleRate();
        const int latency = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
        const double ms = rate > 0.0 ? latency * 1000.0 / rate : 0.0;
        const auto dot = juce::String::fromUTF8 (" \xc2\xb7 ");
        text = formatRate (rate) + dot + juce::String (ms, 1) + " ms";
        detail = device->getName() + dot + juce::String (device->getCurrentBufferSizeSamples()) + " samples at "
               + formatRate (rate) + dot + juce::String (ms, 1) + " ms round trip";
    }

    deviceDetail = detail;
    if (text != statusText)
    {
        statusText = text;
        repaint (titleArea());
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
    if ((tick < 90 || tick % 15 == 0) && leftInset() != layoutInset)
        resized();     // the window gained (or lost, in full screen) its unified title bar
    refreshTransport();
    readout.poll();

    const auto levels = studio.engine().takeMasterLevels();
    meter.setLevels (levels.left, levels.right);

    if (tick % 30 == 0)
        refreshStatus();
}

} // namespace woodshed::app
