#include "ArrangeRuler.h"
#include "ArrangeView.h"
#include "MixCommands.h"

namespace woodshed::app::arrange
{

namespace ids = daw::ids;

namespace
{
    /** Logic-like marker colours, in turn. */
    juce::Colour markerColour (int index)
    {
        static const juce::uint32 colours[] { 0xff5e9cec, 0xffe8a33d, 0xff6cc17a, 0xffd96a8b, 0xff9a7be0, 0xff4fbfbf, 0xffd9d35b };
        return juce::Colour (colours[(size_t) index % std::size (colours)]);
    }
}

Ruler::Ruler (ArrangeView& v) : view (v)
{
    setOpaque (true);

    renameEditor.setFont (theme::font (11.0f, theme::Weight::semibold));
    renameEditor.setColour (juce::TextEditor::backgroundColourId, theme::window);
    renameEditor.setColour (juce::TextEditor::textColourId, theme::text);
    renameEditor.setColour (juce::TextEditor::outlineColourId, theme::accent);
    renameEditor.setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
    renameEditor.setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.35f));
    renameEditor.setColour (juce::CaretComponent::caretColourId, theme::accent);
    renameEditor.setIndents (4, 1);
    renameEditor.onReturnKey = [this] { finishRename (true); };
    renameEditor.onEscapeKey = [this] { finishRename (false); };
    renameEditor.onFocusLost = [this] { finishRename (true); };
    addChildComponent (renameEditor);
}

void Ruler::paint (juce::Graphics& g)
{
    auto& state = view.project().state;
    const auto grid = view.project().grid();
    const float width = (float) getWidth();

    g.fillAll (theme::bar);

    // Loop strip: a touch darker than the numbers, with the range as a rounded band.
    const auto strip = getLocalBounds().withTrimmedTop (numbersHeight).withTrimmedBottom (markerHeight + 1).toFloat();
    g.setColour (theme::panel);
    g.fillRect (strip);

    const auto loopStart = (juce::int64) state[ids::loopStart];
    const auto loopEnd = (juce::int64) state[ids::loopEnd];
    if (loopEnd > loopStart)
    {
        const bool on = (bool) state[ids::loopOn];
        const float x1 = view.xForSample ((double) loopStart), x2 = view.xForSample ((double) loopEnd);
        const auto range = juce::Rectangle<float> (x1, strip.getY() + 3.0f, x2 - x1, strip.getHeight() - 6.0f);
        if (range.getRight() > -10.0f && range.getX() < width + 10.0f)
        {
            const float radius = juce::jmin (4.0f, range.getHeight() * 0.5f);
            g.setColour (theme::loop.withAlpha (on ? 0.30f : 0.08f));
            g.fillRoundedRectangle (range, radius);
            g.setColour (theme::loop.withAlpha (on ? 0.85f : 0.30f));
            g.drawRoundedRectangle (range.reduced (0.5f), radius, 1.0f);

            // Handles: solid pills just inside each end.
            if (range.getWidth() > 18.0f)
            {
                g.setColour (on ? theme::loop : theme::loop.withAlpha (0.4f));
                for (float hx : { range.getX() + 3.0f, range.getRight() - 5.0f })
                    g.fillRoundedRectangle (juce::Rectangle<float> (hx, range.getY() + 2.5f, 2.0f, range.getHeight() - 5.0f), 1.0f);
            }
        }
    }

    // Autopunch: the punch range as a red band along the bottom of the strip.
    if ((bool) state[ids::punchOn])
    {
        const auto band = punchBand();
        if (band.getWidth() > 0.0f && band.getRight() > -10.0f && band.getX() < width + 10.0f)
        {
            g.setColour (mixui::punchColour().withAlpha (0.75f));
            g.fillRoundedRectangle (band, 1.5f);
            g.setColour (mixui::punchColour());
            for (float hx : { band.getX(), band.getRight() - 2.0f })
                g.fillRect (juce::Rectangle<float> (hx, band.getY() - 3.0f, 2.0f, band.getHeight() + 3.0f));
        }
    }

    // Bar numbers and ticks.
    const double step = view.gridStepBeats(), labelStep = view.labelStepBeats();
    const int bpb = juce::jmax (1, grid.beatsPerBar);
    const int labelBars = labelStep >= bpb ? juce::jmax (1, juce::roundToInt (labelStep / bpb)) : 0;
    const float base = (float) numbersHeight;

    g.setFont (theme::font (11.0f, theme::Weight::medium));
    for (auto& line : view.gridLines (view.sampleAtX (-2.0), view.sampleAtX (width + 2.0), step))
    {
        const float x = std::round (view.xForSample (line.sample));
        const bool isBar = line.isBar, isBeat = line.isBeat;
        const float tickTop = isBar ? 5.0f : (isBeat ? base - 5.0f : base - 3.0f);

        g.setColour (isBar ? theme::faintText : theme::faintText.withAlpha (isBeat ? 0.55f : 0.35f));
        g.fillRect (x, tickTop, 1.0f, base - tickTop);

        // Labels: every bar (or every few when zoomed out), beats too when zoomed right in.
        const bool labelled = labelBars > 0 ? (isBar && (line.bar - 1) % labelBars == 0) : isBeat;
        if (! labelled)
            continue;

        const int bar = line.bar;
        const int beatInBar = line.beat;
        if (isBar)
        {
            g.setColour (theme::dimText);
            g.drawText (juce::String (bar), juce::Rectangle<float> (x + 4.0f, 3.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
        }
        else if (isBeat)
        {
            g.setColour (theme::faintText);
            g.drawText (juce::String (bar) + "." + juce::String (beatInBar),
                        juce::Rectangle<float> (x + 3.0f, 5.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
        }
    }

    // Tempo and time signature changes: small tags under the bar numbers.
    {
        const auto& map = view.project().grid();
        g.setFont (theme::font (10.0f, theme::Weight::semibold));
        for (auto& c : map.tempoChanges)
        {
            const float x = view.xForSample (map.sampleAt (c.tick));
            if (x < -60.0f || x > width)
                continue;
            g.setColour (theme::accent);
            g.fillRect (juce::Rectangle<float> (x, base - 12.0f, 1.0f, 12.0f));
            g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\x99\xa9 ")) + juce::String (c.bpm, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."),
                        juce::Rectangle<float> (x + 3.0f, base - 13.0f, 60.0f, 12.0f), juce::Justification::centredLeft, false);
        }
        for (auto& c : map.meterChanges)
        {
            const float x = view.xForSample (map.sampleAt (map.barStartTick (c.bar)));
            if (x < -60.0f || x > width)
                continue;
            g.setColour (theme::mute);
            g.drawText (juce::String (c.beatsPerBar) + "/" + juce::String (c.beatUnit),
                        juce::Rectangle<float> (x + 22.0f, 3.0f, 40.0f, 14.0f), juce::Justification::centredLeft, false);
        }
    }

    drawMarkers (g, getLocalBounds().removeFromBottom (markerHeight).withTrimmedBottom (1).toFloat());

    g.setColour (theme::separator.withAlpha (0.6f));
    g.fillRect (0, numbersHeight, getWidth(), 1);
    g.fillRect (0, getHeight() - markerHeight - 1, getWidth(), 1);
    g.setColour (theme::separator);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);

    // Playhead: a small cap in the numbers and a hairline down through the strip.
    const float px = std::round (view.xForSample ((double) view.studio.engine().getPosition()));
    if (px > -8.0f && px < width + 8.0f)
    {
        const auto colour = view.studio.engine().isRecording() ? theme::record : theme::accent;
        g.setColour (colour);
        g.fillRect (px, 6.0f, 1.0f, (float) getHeight() - 6.0f);
        juce::Path cap;
        cap.addTriangle (px - 5.0f, 1.0f, px + 6.0f, 1.0f, px + 0.5f, 8.0f);
        g.fillPath (cap.createPathWithRoundedCorners (1.5f));
    }
}

void Ruler::drawMarkers (juce::Graphics& g, juce::Rectangle<float> lane) const
{
    g.setColour (theme::window);
    g.fillRect (lane);

    // Each marker is a tag running to the next one (Logic's marker track), the last one sized to its name.
    const auto markers = view.project().getMarkers();
    const auto font = theme::font (11.0f, theme::Weight::semibold);
    g.setFont (font);

    for (size_t i = 0; i < markers.size(); ++i)
    {
        const auto& m = markers[i];
        const float x = std::round (view.xForSample ((double) m.position));
        const float nameWidth = juce::GlyphArrangement::getStringWidth (font, m.name) + 14.0f;
        const float end = i + 1 < markers.size() ? std::round (view.xForSample ((double) markers[i + 1].position)) : x + nameWidth;
        if (end < -2.0f || x > (float) getWidth() + 2.0f)
            continue;

        const auto colour = markerColour ((int) i);
        const auto tag = juce::Rectangle<float> (x, lane.getY() + 2.0f, juce::jmax (2.0f, end - x - 1.0f), lane.getHeight() - 3.0f);
        g.setColour (colour.withAlpha (m.id == grabbedMarker ? 0.55f : 0.32f));
        g.fillRoundedRectangle (tag, 3.0f);
        g.setColour (colour);
        g.fillRect (tag.withWidth (2.0f));

        if (m.id != renamingMarker && tag.getWidth() > 16.0f)
        {
            g.setColour (theme::text);
            g.drawText (m.name, tag.withTrimmedLeft (6.0f).withTrimmedRight (3.0f), juce::Justification::centredLeft, true);
        }
    }
}

int Ruler::markerAt (juce::Point<int> p) const
{
    if (! inMarkerLane (p))
        return 0;

    const auto markers = view.project().getMarkers();
    const auto font = theme::font (11.0f, theme::Weight::semibold);
    for (size_t i = markers.size(); i-- > 0;)
    {
        const float x = view.xForSample ((double) markers[i].position);
        const float end = i + 1 < markers.size() ? view.xForSample ((double) markers[i + 1].position)
                                                 : x + juce::GlyphArrangement::getStringWidth (font, markers[i].name) + 14.0f;
        if ((float) p.x >= x - 3.0f && (float) p.x < end)
            return markers[i].id;
    }
    return 0;
}

void Ruler::showMarkerMenu (int markerId)
{
    auto& p = view.project();
    juce::PopupMenu m;
    m.addItem ("Rename", [this, markerId] { startRename (markerId); });
    m.addItem ("Move Marker to Playhead", [this, markerId, &p]
    {
        p.undo.beginNewTransaction ("Move Marker");
        p.moveMarker (markerId, view.studio.engine().getPosition());
    });
    m.addSeparator();
    m.addItem ("Delete Marker", [markerId, &p]
    {
        p.undo.beginNewTransaction ("Delete Marker");
        p.removeMarker (markerId);
    });
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void Ruler::startRename (int markerId)
{
    for (auto& m : view.project().getMarkers())
        if (m.id == markerId)
        {
            renamingMarker = markerId;
            const float x = view.xForSample ((double) m.position);
            renameEditor.setBounds (juce::Rectangle<int> (juce::roundToInt (x), getHeight() - markerHeight, 150, markerHeight - 1));
            renameEditor.setText (m.name, false);
            renameEditor.setVisible (true);
            renameEditor.grabKeyboardFocus();
            renameEditor.selectAll();
            repaint();
            return;
        }
}

void Ruler::finishRename (bool commit)
{
    if (renamingMarker == 0)
        return;

    const int id = std::exchange (renamingMarker, 0);
    renameEditor.setVisible (false);
    const auto name = renameEditor.getText().trim();
    if (commit && name.isNotEmpty())
    {
        auto& p = view.project();
        p.undo.beginNewTransaction ("Rename Marker");
        p.renameMarker (id, name);
    }
    repaint();
}

void Ruler::playheadMoved (float oldX, float newX)
{
    repaint (juce::Rectangle<int> ((int) oldX - 8, 0, 18, getHeight()));
    repaint (juce::Rectangle<int> ((int) newX - 8, 0, 18, getHeight()));
}

//==============================================================================
Ruler::Mode Ruler::loopModeAt (juce::Point<int> p) const
{
    if (p.y < numbersHeight)
        return Mode::scrub;
    if (inMarkerLane (p))
        return markerAt (p) != 0 ? Mode::markerDrag : Mode::markerEmpty;

    auto& state = view.project().state;
    if ((bool) state[ids::punchOn])
    {
        const auto band = punchBand();
        if ((float) p.y >= band.getY() - 4.0f && band.getWidth() > 0.0f)
        {
            const float grab = juce::jmin (6.0f, band.getWidth() / 3.0f);
            if (std::abs ((float) p.x - band.getX()) <= grab)     return Mode::punchStart;
            if (std::abs ((float) p.x - band.getRight()) <= grab) return Mode::punchEnd;
            if ((float) p.x > band.getX() && (float) p.x < band.getRight()) return Mode::punchMove;
        }
    }

    const auto loopStart = (juce::int64) state[ids::loopStart];
    const auto loopEnd = (juce::int64) state[ids::loopEnd];
    if (loopEnd <= loopStart)
        return Mode::loopCreate;

    const float x1 = view.xForSample ((double) loopStart), x2 = view.xForSample ((double) loopEnd);
    const float grab = juce::jmin (6.0f, (x2 - x1) / 3.0f);
    if (std::abs ((float) p.x - x1) <= grab) return Mode::loopStart;
    if (std::abs ((float) p.x - x2) <= grab) return Mode::loopEnd;
    if ((float) p.x > x1 && (float) p.x < x2) return Mode::loopMove;
    return Mode::loopCreate;
}

void Ruler::mouseMove (const juce::MouseEvent& e)
{
    switch (loopModeAt (e.getPosition()))
    {
        case Mode::loopStart:
        case Mode::loopEnd:
        case Mode::punchStart:
        case Mode::punchEnd:  setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case Mode::loopMove:
        case Mode::punchMove:
        case Mode::markerDrag: setMouseCursor (juce::MouseCursor::PointingHandCursor); break;
        case Mode::none:
        case Mode::scrub:
        case Mode::loopCreate:
        case Mode::markerEmpty:
        default:              setMouseCursor (juce::MouseCursor::NormalCursor); break;
    }
}

void Ruler::scrubTo (float x)
{
    view.studio.engine().setPosition ((juce::int64) juce::jmax (0.0, view.sampleAtX (x)));
}

void Ruler::setLoop (juce::int64 start, juce::int64 end, bool turnOn)
{
    auto& p = view.project();
    if (! changed)
    {
        p.undo.beginNewTransaction ("Set Loop");
        changed = true;
    }
    p.state.setProperty (ids::loopStart, start, &p.undo);
    p.state.setProperty (ids::loopEnd, end, &p.undo);
    if (turnOn && ! (bool) p.state[ids::loopOn])
        p.state.setProperty (ids::loopOn, true, &p.undo);
}

juce::Rectangle<float> Ruler::punchBand() const
{
    const auto& state = view.project().state;
    const auto strip = getLocalBounds().withTrimmedTop (numbersHeight).withTrimmedBottom (markerHeight + 1).toFloat();
    const float x1 = view.xForSample ((double) (juce::int64) state[ids::punchIn]);
    const float x2 = view.xForSample ((double) (juce::int64) state[ids::punchOut]);
    return { x1, strip.getBottom() - 4.0f, juce::jmax (0.0f, x2 - x1), 3.0f };
}

void Ruler::setPunch (juce::int64 start, juce::int64 end)
{
    auto& p = view.project();
    if (! changed)
    {
        p.undo.beginNewTransaction ("Set Punch Locators");
        changed = true;
    }
    p.state.setProperty (ids::punchIn, start, &p.undo);
    p.state.setProperty (ids::punchOut, end, &p.undo);
}

void Ruler::mouseDown (const juce::MouseEvent& e)
{
    changed = false;
    mode = loopModeAt (e.getPosition());
    auto& state = view.project().state;
    const bool punch = mode == Mode::punchMove || mode == Mode::punchStart || mode == Mode::punchEnd;
    originalStart = (juce::int64) state[punch ? ids::punchIn : ids::loopStart];
    originalEnd = (juce::int64) state[punch ? ids::punchOut : ids::loopEnd];
    anchor = view.snap (view.sampleAtX (e.x), ! e.mods.isCommandDown());
    view.setUserGesture (true);

    // Right-click on the ruler (not on a marker): tempo and time signature changes here.
    if (e.mods.isPopupMenu() && mode != Mode::markerDrag)
    {
        mode = Mode::none;
        view.setUserGesture (false);
        showTempoMenu (e.getPosition());
        return;
    }

    if (mode == Mode::scrub)
        scrubTo ((float) e.x);

    if (mode == Mode::markerDrag)
    {
        grabbedMarker = markerAt (e.getPosition());
        for (auto& m : view.project().getMarkers())
            if (m.id == grabbedMarker)
                grabbedMarkerPosition = m.position;

        if (e.mods.isPopupMenu())
        {
            mode = Mode::none;
            showMarkerMenu (std::exchange (grabbedMarker, 0));
        }
        repaint();
    }
}

void Ruler::showTempoMenu (juce::Point<int> p)
{
    auto& project = view.project();
    const auto& grid = project.grid();
    const auto at = view.snap (view.sampleAtX (p.x), true);
    const double tick = grid.tickAt ((double) at);
    const int bar = juce::jmax (0, (int) std::floor (grid.barAt (tick) + 0.5));
    const double tempoHere = grid.tempoAt ((double) at);
    const auto meterHere = grid.meterAt (grid.barStartTick (bar));

    // Changes under the mouse, to edit or remove.
    double nearTempoTick = -1.0;
    for (auto& c : grid.tempoChanges)
        if (std::abs (view.xForSample (grid.sampleAt (c.tick)) - (float) p.x) < 8.0f)
            nearTempoTick = c.tick;
    int nearMeterBar = -1;
    for (auto& c : grid.meterChanges)
        if (std::abs (view.xForSample (grid.sampleAt (grid.barStartTick (c.bar))) - (float) p.x) < 8.0f)
            nearMeterBar = c.bar;

    juce::Component::SafePointer<Ruler> safe (this);
    auto ask = [safe] (const juce::String& title, const juce::String& message, const juce::String& initial,
                       std::function<void (const juce::String&)> done)
    {
        auto* w = new juce::AlertWindow (title, message, juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("value", initial);
        w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([w, done, safe] (int result)
        {
            if (result == 1 && safe != nullptr)
                done (w->getTextEditorContents ("value"));
        }), true);
    };

    juce::PopupMenu m;
    m.addSectionHeader ("Tempo and Time Signature");
    m.addItem ("Change Tempo Here (" + grid.toBarsBeats (at) + ")...", [this, ask, tick, tempoHere]
    {
        ask ("Tempo Change", "New tempo from here on, in bpm:", juce::String (tempoHere, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."),
             [this, tick] (const juce::String& text)
             {
                 const auto bpm = text.getDoubleValue();
                 if (bpm < 20.0 || bpm > 400.0)
                     return;
                 auto& pr = view.project();
                 pr.undo.beginNewTransaction ("Tempo Change");
                 pr.setTempoChange (tick, bpm);
             });
    });
    m.addItem ("Change Time Signature at Bar " + juce::String (bar + 1) + "...", [this, ask, bar, meterHere]
    {
        ask ("Time Signature", "New time signature from bar " + juce::String (bar + 1) + " on (for example 3/4 or 6/8):",
             juce::String (meterHere.beatsPerBar) + "/" + juce::String (meterHere.beatUnit),
             [this, bar] (const juce::String& text)
             {
                 const int beats = text.upToFirstOccurrenceOf ("/", false, false).getIntValue();
                 const int unit = text.fromFirstOccurrenceOf ("/", false, false).getIntValue();
                 if (beats < 1 || beats > 16 || (unit != 2 && unit != 4 && unit != 8 && unit != 16))
                     return;
                 auto& pr = view.project();
                 pr.undo.beginNewTransaction ("Time Signature Change");
                 pr.setMeterChange (bar, beats, unit);
             });
    });
    if (nearTempoTick > 0.0 || nearMeterBar > 0)
        m.addSeparator();
    if (nearTempoTick > 0.0)
        m.addItem ("Remove Tempo Change", [this, nearTempoTick]
        {
            auto& pr = view.project();
            pr.undo.beginNewTransaction ("Remove Tempo Change");
            pr.removeTempoChange (nearTempoTick);
        });
    if (nearMeterBar > 0)
        m.addItem ("Remove Time Signature Change", [this, nearMeterBar]
        {
            auto& pr = view.project();
            pr.undo.beginNewTransaction ("Remove Time Signature Change");
            pr.removeMeterChange (nearMeterBar);
        });
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void Ruler::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! inMarkerLane (e.getPosition()))
        return;

    if (const int id = markerAt (e.getPosition()); id != 0)
    {
        startRename (id);
        return;
    }

    auto& p = view.project();
    p.undo.beginNewTransaction ("Create Marker");
    p.addMarker (view.snap (view.sampleAtX (e.x), ! e.mods.isCommandDown()));
}

void Ruler::mouseDrag (const juce::MouseEvent& e)
{
    const bool snapOn = ! e.mods.isCommandDown();
    const auto at = view.snap (view.sampleAtX (e.x), snapOn);
    const auto minLength = snapOn ? (juce::int64) std::llround (view.project().grid().samplesPerBeat())
                                  : (juce::int64) std::llround (view.getSamplesPerPixel() * 4.0);

    switch (mode)
    {
        case Mode::scrub:
            scrubTo ((float) e.x);
            break;

        case Mode::loopCreate:
            if (e.getDistanceFromDragStartX() == 0 || at == anchor)
                break;
            setLoop (juce::jmin (anchor, at), juce::jmax (anchor, at), true);
            break;

        case Mode::loopMove:
        {
            if (! changed && std::abs (e.getDistanceFromDragStartX()) < 3)
                break;
            const auto delta = juce::jmax (-originalStart, at - anchor);
            if (delta != 0 || changed)
                setLoop (originalStart + delta, originalEnd + delta, false);
            break;
        }

        case Mode::loopStart:
            setLoop (juce::jmin (at, originalEnd - minLength), originalEnd, false);
            break;

        case Mode::loopEnd:
            setLoop (originalStart, juce::jmax (at, originalStart + minLength), false);
            break;

        case Mode::punchMove:
        {
            if (! changed && std::abs (e.getDistanceFromDragStartX()) < 3)
                break;
            const auto delta = juce::jmax (-originalStart, at - anchor);
            if (delta != 0 || changed)
                setPunch (originalStart + delta, originalEnd + delta);
            break;
        }

        case Mode::punchStart:
            setPunch (juce::jmin (at, originalEnd - minLength), originalEnd);
            break;

        case Mode::punchEnd:
            setPunch (originalStart, juce::jmax (at, originalStart + minLength));
            break;

        case Mode::markerDrag:
        {
            if (! changed && std::abs (e.getDistanceFromDragStartX()) < 3)
                break;
            auto& p = view.project();
            if (! changed)
            {
                p.undo.beginNewTransaction ("Move Marker");
                changed = true;
            }
            // The marker itself lands on the grid (the grab offset is kept only with snapping off).
            const auto unsnapped = (double) grabbedMarkerPosition + (view.sampleAtX (e.x) - view.sampleAtX (e.getMouseDownX()));
            p.moveMarker (grabbedMarker, view.snap (unsnapped, snapOn));
            break;
        }

        case Mode::markerEmpty:
        case Mode::none:
        default:
            break;
    }
}

void Ruler::mouseUp (const juce::MouseEvent&)
{
    // A click on the range (no drag) switches looping on or off, like the C key.
    if (mode == Mode::loopMove && ! changed)
        view.studio.commands.invokeDirectly (cmd::toggleLoop, false);

    // A click on a marker (no drag) takes the playhead there.
    if (mode == Mode::markerDrag && ! changed && grabbedMarker != 0)
        view.studio.engine().setPosition (grabbedMarkerPosition);
    grabbedMarker = 0;
    repaint();

    mode = Mode::none;
    changed = false;
    view.setUserGesture (false);
}

void Ruler::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    view.handleWheel (e, w, (float) e.x);
}

void Ruler::mouseMagnify (const juce::MouseEvent& e, float scale)
{
    view.zoomAround ((float) e.x, (double) scale);
}

} // namespace woodshed::app::arrange
