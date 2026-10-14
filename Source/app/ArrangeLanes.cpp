#include "ArrangeLanes.h"
#include "ArrangeHeader.h"
#include "ArrangeView.h"

namespace woodshed::app::arrange
{

namespace ids = daw::ids;

namespace
{
    constexpr float regionInset = 3.0f, nameBarHeight = 15.0f;
    const juce::Colour darkInk { 0xff15130f };

    bool isAudioFile (const juce::String& path)
    {
        return juce::File (path).hasFileExtension ("wav;aif;aiff;flac");
    }

    juce::Array<juce::ValueTree> regionsOf (const juce::ValueTree& track)
    {
        juce::Array<juce::ValueTree> list;
        for (auto r : track)
            if (r.hasType (ids::REGION))
                list.add (r);
        return list;
    }
}

Lanes::Lanes (ArrangeView& v) : view (v)
{
    setOpaque (true);
}

//==============================================================================
juce::Rectangle<float> Lanes::regionRect (int trackIndex, juce::int64 start, juce::int64 length) const
{
    const float x1 = view.xForSample ((double) start);
    const float x2 = view.xForSample ((double) (start + length));
    const float y = (float) view.yForTrack (trackIndex) + regionInset;
    return { x1, y, juce::jmax (1.0f, x2 - x1), (float) ArrangeView::trackHeight - regionInset * 2.0f };
}

Lanes::Hit Lanes::regionAt (juce::Point<int> p) const
{
    Hit hit;
    const int index = view.trackIndexAtY (p.y);
    if (index < 0)
        return hit;

    hit.trackIndex = index;
    auto regions = regionsOf (view.project().track (index));
    for (int i = regions.size(); --i >= 0;)     // the last drawn is on top
    {
        auto& r = regions.getReference (i);
        const auto rect = regionRect (index, (juce::int64) r[ids::start], (juce::int64) r[ids::length]);
        if (! rect.contains (p.toFloat()))
            continue;

        hit.region = r;
        const float grab = juce::jmin (6.0f, rect.getWidth() / 4.0f);
        if ((float) p.x < rect.getX() + grab)            hit.part = Part::leftEdge;
        else if ((float) p.x > rect.getRight() - grab)   hit.part = Part::rightEdge;
        else                                             hit.part = Part::body;
        return hit;
    }
    return hit;
}

juce::int64 Lanes::fileLengthOf (const juce::ValueTree& region)
{
    return view.waveforms.info (view.project().resolve (region[ids::file].toString())).length;
}

juce::int64 Lanes::minimumLength() const
{
    return juce::jmax ((juce::int64) 64, (juce::int64) std::llround (view.getSamplesPerPixel() * 6.0));
}

//==============================================================================
void Lanes::paint (juce::Graphics& g)
{
    auto& p = view.project();
    auto& state = p.state;
    const auto clip = g.getClipBounds();
    const int numTracks = p.numTracks();
    const int contentBottom = view.yForTrack (numTracks);
    const int selectedTrack = view.studio.getSelectedTrackId();

    // Lane backgrounds, alternating; the selected track's lane is tinted with its colour.
    g.fillAll (theme::window);
    for (int i = 0; i < numTracks; ++i)
    {
        const auto row = juce::Rectangle<int> (0, view.yForTrack (i), getWidth(), ArrangeView::trackHeight);
        if (! row.intersects (clip))
            continue;
        auto t = p.track (i);
        g.setColour (i % 2 == 0 ? theme::lane : theme::laneAlt);
        g.fillRect (row);
        if ((int) t[ids::id] == selectedTrack)
        {
            g.setColour (trackColour (t).withAlpha (0.05f));
            g.fillRect (row);
        }
        g.setColour (theme::window.withAlpha (0.6f));
        g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);
    }

    // Loop range, behind the grid.
    if ((bool) state[ids::loopOn])
    {
        const auto loopStart = (juce::int64) state[ids::loopStart], loopEnd = (juce::int64) state[ids::loopEnd];
        if (loopEnd > loopStart)
        {
            const float x1 = view.xForSample ((double) loopStart), x2 = view.xForSample ((double) loopEnd);
            g.setColour (theme::loop.withAlpha (0.045f));
            g.fillRect (juce::Rectangle<float> (x1, 0.0f, x2 - x1, (float) getHeight()));
            g.setColour (theme::loop.withAlpha (0.28f));
            g.fillRect (juce::Rectangle<float> (x1, 0.0f, 1.0f, (float) getHeight()));
            g.fillRect (juce::Rectangle<float> (x2 - 1.0f, 0.0f, 1.0f, (float) getHeight()));
        }
    }

    drawGrid (g, contentBottom);

    // Regions. Ones being moved are drawn at their new place after the rest.
    const auto& selected = view.studio.getSelectedRegions();
    const bool moving = gesture == Gesture::move;
    const bool trimming = gesture == Gesture::trimLeft || gesture == Gesture::trimRight;

    for (int i = 0; i < numTracks; ++i)
    {
        const auto row = juce::Rectangle<int> (0, view.yForTrack (i), getWidth(), ArrangeView::trackHeight);
        if (! row.intersects (clip))
            continue;

        auto t = p.track (i);
        const bool muted = (bool) t[ids::mute];
        for (auto& r : regionsOf (t))
        {
            RegionLook look;
            look.start = (juce::int64) r[ids::start];
            look.offset = (juce::int64) r[ids::offset];
            look.length = (juce::int64) r[ids::length];
            look.selected = selected.contains ((int) r[ids::id]);
            look.muted = muted;

            if (moving && look.selected)
            {
                if (! copying)
                    look.ghost = true;      // an outline where it was
                else
                    look.selected = false;
            }
            else if (trimming && r == grabbed)
            {
                look.start = trimStart;
                look.offset = trimOffset;
                look.length = trimLength;
            }

            drawRegion (g, r, i, look);
        }
    }

    if (moving)
    {
        for (int i = 0; i < numTracks; ++i)
            for (auto& r : regionsOf (p.track (i)))
                if (selected.contains ((int) r[ids::id]))
                {
                    const int target = juce::jlimit (0, numTracks - 1, i + trackDelta);
                    RegionLook look;
                    look.start = (juce::int64) r[ids::start] + moveDelta;
                    look.offset = (juce::int64) r[ids::offset];
                    look.length = (juce::int64) r[ids::length];
                    look.selected = true;
                    look.muted = (bool) p.track (target)[ids::mute];
                    drawRegion (g, r, target, look);
                }
    }

    drawTakes (g);

    // Rubber band.
    if (gesture == Gesture::rubber)
    {
        g.setColour (theme::accent.withAlpha (0.1f));
        g.fillRect (rubber);
        g.setColour (theme::accent.withAlpha (0.7f));
        g.drawRect (rubber, 1);
    }

    // Where a dropped file will land.
    if (dropping)
    {
        const int y = dropTrack >= 0 ? view.yForTrack (dropTrack) : contentBottom;
        const auto row = juce::Rectangle<int> (0, y, getWidth(), ArrangeView::trackHeight);
        g.setColour (theme::accent.withAlpha (0.08f));
        g.fillRect (row);
        g.setColour (theme::accent.withAlpha (0.6f));
        g.drawRect (row, 1);
        const float x = view.xForSample ((double) dropAt);
        g.setColour (theme::accent);
        g.fillRect (juce::Rectangle<float> (x, (float) row.getY(), 2.0f, (float) row.getHeight()));
        g.setFont (theme::font (12.0f, true));
        g.drawText (dropTrack >= 0 ? "Drop to add to " + p.track (dropTrack)[ids::name].toString() : "Drop to add a new track",
                    row.withTrimmedLeft (juce::roundToInt (x) + 8).withWidth (260), juce::Justification::centredLeft);
    }

    drawHint (g, contentBottom);

    // Playhead.
    auto& engine = view.studio.engine();
    const float px = std::round (view.xForSample ((double) engine.getPosition()));
    if (px > -4.0f && px < (float) getWidth() + 4.0f)
    {
        const auto colour = engine.isRecording() ? theme::record : theme::accent;
        g.setColour (colour.withAlpha (0.15f));
        g.fillRect (px - 1.0f, 0.0f, 3.0f, (float) getHeight());
        g.setColour (colour);
        g.fillRect (px, 0.0f, 1.0f, (float) getHeight());
    }
}

void Lanes::drawGrid (juce::Graphics& g, int contentBottom) const
{
    const auto grid = view.project().grid();
    const double spb = grid.samplesPerBeat();
    const double step = view.gridStepBeats();
    const int bpb = juce::jmax (1, grid.beatsPerBar);
    const auto clip = g.getClipBounds();

    const double firstBeat = std::floor (view.sampleAtX ((double) clip.getX()) / spb / step) * step;
    const double lastBeat = view.sampleAtX ((double) clip.getRight()) / spb;
    const float bottom = (float) juce::jmax (0, juce::jmin (getHeight(), contentBottom));

    for (double beat = juce::jmax (0.0, firstBeat); beat <= lastBeat + step; beat += step)
    {
        const float x = std::round (view.xForSample (beat * spb));
        const double inBar = std::fmod (beat, (double) bpb);
        const bool isBar = inBar < 1.0e-6 || bpb - inBar < 1.0e-6;
        const bool isBeat = std::abs (beat - std::round (beat)) < 1.0e-6;

        g.setColour (isBar ? theme::gridBar : (isBeat ? theme::gridBeat : theme::gridBeat.withAlpha (0.55f)));
        g.fillRect (x, 0.0f, 1.0f, bottom);

        // Faint continuation below the last track, so the empty space still reads as a timeline.
        if (isBar && bottom < (float) getHeight())
        {
            g.setColour (theme::gridBeat.withAlpha (0.5f));
            g.fillRect (x, bottom, 1.0f, (float) getHeight() - bottom);
        }
    }
}

void Lanes::drawRegion (juce::Graphics& g, const juce::ValueTree& region, int trackIndex, const RegionLook& look)
{
    const auto rect = regionRect (trackIndex, look.start, look.length);
    const auto clip = g.getClipBounds().toFloat();
    if (rect.getRight() < clip.getX() - 2.0f || rect.getX() > clip.getRight() + 2.0f)
        return;

    auto colour = trackColour (view.project().track (trackIndex));
    if (look.muted)
        colour = colour.withSaturation (0.12f).withMultipliedBrightness (0.7f);

    if (look.ghost)
    {
        g.setColour (colour.withAlpha (0.08f));
        g.fillRoundedRectangle (rect, 4.0f);
        g.setColour (colour.withAlpha (0.4f));
        g.drawRoundedRectangle (rect.reduced (0.5f), 4.0f, 1.0f);
        return;
    }

    // Keep enormous regions to the area being painted, with a margin so cut ends stay out of view.
    const auto body = rect.getIntersection ({ clip.getX() - 8.0f, rect.getY(), clip.getWidth() + 16.0f, rect.getHeight() });

    g.setColour (colour.withMultipliedBrightness (look.selected ? 0.55f : 0.38f).withMultipliedSaturation (0.85f));
    g.fillRoundedRectangle (body, 4.0f);

    // Name bar.
    auto bar = body.withHeight (nameBarHeight);
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (bar.toNearestInt());
        g.setColour (look.selected ? colour.brighter (0.25f) : colour.withMultipliedBrightness (0.85f));
        g.fillRoundedRectangle (body, 4.0f);
    }

    // Name: stays in view when the region starts off the left edge.
    {
        const float textX = juce::jmax (rect.getX(), 0.0f) + 5.0f;
        const float textW = rect.getRight() - textX - 4.0f;
        if (textW > 14.0f)
        {
            g.setColour (darkInk.withAlpha (0.9f));
            g.setFont (theme::font (11.0f, true));
            g.drawText (region[ids::name].toString(), juce::Rectangle<float> (textX, bar.getY(), textW, nameBarHeight),
                        juce::Justification::centredLeft, true);
        }
    }

    // Waveform: only the slice the region plays, and only the part that's on screen.
    const auto file = view.project().resolve (region[ids::file].toString());
    const auto wave = body.withTrimmedTop (nameBarHeight + 1.0f).reduced (0.0f, 2.0f);
    const auto info = view.waveforms.info (file);
    auto* thumb = info.length > 0 ? view.waveforms.thumbnail (file) : nullptr;

    if (thumb != nullptr && wave.getWidth() >= 1.0f && wave.getHeight() > 4.0f)
    {
        const double rate = info.sampleRate > 0.0 ? info.sampleRate : view.project().grid().sampleRate;
        const double sppx = view.getSamplesPerPixel();
        const double fromSample = (double) look.offset + (wave.getX() - rect.getX()) * sppx;
        const double toSample = fromSample + wave.getWidth() * sppx;
        // Quiet DI takes are scaled up for display (by the file's peak), then by the region's gain.
        const auto peak = thumb->getApproximatePeak();
        const auto normalise = peak > 0.0f ? juce::jlimit (1.0f, 8.0f, 0.85f / peak) : 1.0f;
        const auto gain = normalise * juce::jlimit (0.25f, 4.0f, juce::Decibels::decibelsToGain ((float) (double) region.getProperty (ids::gain, 0.0)));

        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (wave.toNearestInt());
        // Centre line, then the wave.
        g.setColour (colour.withAlpha (0.25f));
        g.fillRect (wave.withHeight (1.0f).withCentre (wave.getCentre()));
        g.setColour (look.selected ? colour.interpolatedWith (juce::Colours::white, 0.75f)
                                   : colour.brighter (0.35f).withAlpha (0.9f));
        thumb->drawChannel (g, wave.toNearestInt(), fromSample / rate, toSample / rate, 0, gain);
    }
    else if (wave.getHeight() > 10.0f)
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (11.0f));
        g.drawText (file.existsAsFile() ? "Reading..." : "Missing audio file", wave.reduced (6.0f, 0.0f),
                    juce::Justification::centredLeft, true);
    }

    // Outline.
    if (look.selected)
    {
        g.setColour (theme::text.withAlpha (0.95f));
        g.drawRoundedRectangle (body.reduced (0.75f), 4.0f, 1.5f);
    }
    else
    {
        g.setColour (darkInk.withAlpha (0.6f));
        g.drawRoundedRectangle (body.reduced (0.5f), 4.0f, 1.0f);
    }
}

void Lanes::drawTakes (juce::Graphics& g)
{
    auto& engine = view.studio.engine();
    if (! engine.isRecording())
        return;

    const auto start = engine.getRecordStart();
    const auto length = juce::jmax ((juce::int64) 1, engine.getRecordedLength());
    auto& p = view.project();

    for (int i = 0; i < p.numTracks(); ++i)
    {
        if (! (bool) p.track (i)[ids::arm])
            continue;

        const auto rect = regionRect (i, start, length);
        g.setColour (theme::record.withAlpha (0.28f));
        g.fillRoundedRectangle (rect, 4.0f);
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (rect.withHeight (nameBarHeight).toNearestInt());
            g.setColour (theme::record.withAlpha (0.9f));
            g.fillRoundedRectangle (rect, 4.0f);
        }
        g.setColour (theme::record);
        g.drawRoundedRectangle (rect.reduced (0.5f), 4.0f, 1.0f);
        if (rect.getWidth() > 50.0f)
        {
            g.setColour (juce::Colours::white);
            g.setFont (theme::font (11.0f, true));
            g.drawText ("Recording", rect.withHeight (nameBarHeight).withTrimmedLeft (5.0f), juce::Justification::centredLeft, true);
        }
    }
}

void Lanes::drawHint (juce::Graphics& g, int contentBottom) const
{
    auto& p = view.project();
    if (view.studio.engine().isRecording() || dropping || p.contentEnd() > 0)
        return;

#if JUCE_MAC
    const juce::String newTrackKeys = "Cmd+Option+N";
#else
    const juce::String newTrackKeys = "Ctrl+Alt+N";
#endif

    auto area = getLocalBounds().withTrimmedTop (juce::jmax (0, contentBottom));
    if (area.getHeight() < 70)
        area = getLocalBounds().withTrimmedTop (juce::jmax (0, getHeight() - 90));

    auto box = area.withSizeKeepingCentre (juce::jmin (area.getWidth() - 40, 460), 64);
    g.setColour (theme::text.withAlpha (0.8f));
    g.setFont (theme::font (16.0f, true));
    g.drawText ("Press R to record, or " + newTrackKeys + " for a new track", box.removeFromTop (28),
                juce::Justification::centred, true);
    g.setColour (theme::dimText);
    g.setFont (theme::font (13.0f));
    g.drawText ("You can also drop WAV, AIFF or FLAC files here", box.removeFromTop (22), juce::Justification::centred, true);
}

void Lanes::playheadMoved (float oldX, float newX)
{
    repaint (juce::Rectangle<int> ((int) oldX - 3, 0, 7, getHeight()));
    repaint (juce::Rectangle<int> ((int) newX - 3, 0, 7, getHeight()));
}

//==============================================================================
void Lanes::mouseMove (const juce::MouseEvent& e)
{
    switch (regionAt (e.getPosition()).part)
    {
        case Part::leftEdge:  setMouseCursor (juce::MouseCursor::LeftEdgeResizeCursor); break;
        case Part::rightEdge: setMouseCursor (juce::MouseCursor::RightEdgeResizeCursor); break;
        case Part::body:
        case Part::none:
        default:              setMouseCursor (juce::MouseCursor::NormalCursor); break;
    }
}

void Lanes::mouseDown (const juce::MouseEvent& e)
{
    auto& studio = view.studio;
    const auto hit = regionAt (e.getPosition());
    shiftAtDown = e.mods.isShiftDown();
    downSample = view.sampleAtX (e.x);
    moveDelta = 0;
    trackDelta = 0;
    copying = false;

    if (hit.trackIndex >= 0)
        studio.selectTrack ((int) view.project().track (hit.trackIndex)[ids::id]);

    if (! hit.region.isValid())
    {
        if (e.mods.isPopupMenu())
            return;
        gesture = Gesture::pendingEmpty;
        selectionAtDown = shiftAtDown ? studio.getSelectedRegions() : juce::Array<int>();
        if (! shiftAtDown)
            studio.setSelectedRegions ({});
        return;
    }

    const int id = (int) hit.region[ids::id];
    auto selection = studio.getSelectedRegions();

    if (e.mods.isPopupMenu())
    {
        if (! selection.contains (id))
            studio.setSelectedRegions ({ id });
        showRegionMenu();
        return;
    }

    if (shiftAtDown)
    {
        if (selection.contains (id)) selection.removeFirstMatchingValue (id);
        else                         selection.add (id);
        studio.setSelectedRegions (selection);
    }
    else if (! selection.contains (id))
    {
        studio.setSelectedRegions ({ id });
    }

    grabbed = hit.region;
    grabbedTrack = hit.trackIndex;
    grabbedPart = hit.part;
    grabStart = (juce::int64) grabbed[ids::start];
    grabOffset = (juce::int64) grabbed[ids::offset];
    grabLength = (juce::int64) grabbed[ids::length];
    gesture = Gesture::pendingRegion;
}

void Lanes::mouseDrag (const juce::MouseEvent& e)
{
    auto& p = view.project();
    const bool snapOn = ! e.mods.isCommandDown();

    if (gesture == Gesture::pendingEmpty)
    {
        if (e.getDistanceFromDragStart() < 3)
            return;
        gesture = Gesture::rubber;
        view.setUserGesture (true);
    }

    if (gesture == Gesture::rubber)
    {
        updateRubberBand (e);
        return;
    }

    if (gesture == Gesture::pendingRegion)
    {
        if (e.getDistanceFromDragStart() < 3)
            return;
        // Shift-click that deselected the region: nothing to drag.
        if (! view.studio.getSelectedRegions().contains ((int) grabbed[ids::id]))
        {
            gesture = Gesture::none;
            return;
        }
        gesture = grabbedPart == Part::leftEdge  ? Gesture::trimLeft
                : grabbedPart == Part::rightEdge ? Gesture::trimRight
                                                 : Gesture::move;
        view.setUserGesture (true);
        if (gesture == Gesture::move)
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    const double mouseSample = view.sampleAtX (e.x);

    if (gesture == Gesture::move)
    {
        // Snap the grabbed region's start; everything selected moves by the same amount.
        const auto newStart = view.snap ((double) grabStart + (mouseSample - downSample), snapOn);
        juce::int64 earliest = std::numeric_limits<juce::int64>::max();
        int lowestTrack = std::numeric_limits<int>::max(), highestTrack = -1;
        const auto& selected = view.studio.getSelectedRegions();

        for (int i = 0; i < p.numTracks(); ++i)
            for (auto& r : regionsOf (p.track (i)))
                if (selected.contains ((int) r[ids::id]))
                {
                    earliest = juce::jmin (earliest, (juce::int64) r[ids::start]);
                    lowestTrack = juce::jmin (lowestTrack, i);
                    highestTrack = juce::jmax (highestTrack, i);
                }

        moveDelta = juce::jmax (newStart - grabStart, -earliest);

        int overTrack = view.trackIndexAtY (e.y);
        if (overTrack < 0)
            overTrack = e.y < 0 ? 0 : p.numTracks() - 1;
        trackDelta = juce::jlimit (-lowestTrack, p.numTracks() - 1 - highestTrack, overTrack - grabbedTrack);
        copying = e.mods.isAltDown();
        repaint();
        return;
    }

    const auto grabEnd = grabStart + grabLength;
    const auto minLength = minimumLength();

    if (gesture == Gesture::trimLeft)
    {
        auto newStart = view.snap (mouseSample, snapOn);
        newStart = juce::jmax (newStart, grabStart - grabOffset);           // not before the file starts
        newStart = juce::jmin (newStart, grabEnd - minLength);
        newStart = juce::jmax ((juce::int64) 0, newStart);
        trimStart = newStart;
        trimOffset = grabOffset + (newStart - grabStart);
        trimLength = grabEnd - newStart;
        repaint();
    }
    else if (gesture == Gesture::trimRight)
    {
        auto newEnd = view.snap (mouseSample, snapOn);
        if (const auto fileLength = fileLengthOf (grabbed); fileLength > 0)
            newEnd = juce::jmin (newEnd, grabStart + (fileLength - grabOffset)); // not past the file's end
        newEnd = juce::jmax (newEnd, grabStart + minLength);
        trimStart = grabStart;
        trimOffset = grabOffset;
        trimLength = newEnd - grabStart;
        repaint();
    }
}

void Lanes::mouseUp (const juce::MouseEvent& e)
{
    switch (gesture)
    {
        case Gesture::move:      commitMove (e.mods.isAltDown()); break;
        case Gesture::trimLeft:
        case Gesture::trimRight: commitTrim(); break;
        case Gesture::pendingRegion:
            // A plain click on a region that was part of a bigger selection selects just it.
            if (! shiftAtDown && grabbed.isValid())
                view.studio.setSelectedRegions ({ (int) grabbed[ids::id] });
            break;
        case Gesture::none:
        case Gesture::pendingEmpty:
        case Gesture::rubber:
        default: break;
    }

    gesture = Gesture::none;
    grabbed = {};
    moveDelta = 0;
    trackDelta = 0;
    copying = false;
    view.setUserGesture (false);
    setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
}

void Lanes::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-click below the last track for a new one.
    if (view.trackIndexAtY (e.y) < 0 && ! regionAt (e.getPosition()).region.isValid())
        view.addTrackAfterSelection();
}

void Lanes::commitMove (bool copy)
{
    if (moveDelta == 0 && trackDelta == 0 && ! copy)
        return;

    auto& p = view.project();
    auto& studio = view.studio;
    const auto selected = studio.getSelectedRegions();

    struct Item { juce::ValueTree region; int track; };
    juce::Array<Item> items;
    for (int i = 0; i < p.numTracks(); ++i)
        for (auto& r : regionsOf (p.track (i)))
            if (selected.contains ((int) r[ids::id]))
                items.add ({ r, i });

    if (items.isEmpty())
        return;

    p.undo.beginNewTransaction (copy ? (items.size() > 1 ? "Copy Regions" : "Copy Region")
                                     : (items.size() > 1 ? "Move Regions" : "Move Region"));
    juce::Array<int> newSelection;
    for (auto& item : items)
    {
        auto target = p.track (juce::jlimit (0, p.numTracks() - 1, item.track + trackDelta));
        const auto newStart = (juce::int64) item.region[ids::start] + moveDelta;

        if (copy)
        {
            auto c = item.region.createCopy();
            c.setProperty (ids::id, p.nextId(), nullptr);
            c.setProperty (ids::start, newStart, nullptr);
            target.addChild (c, -1, &p.undo);
            newSelection.add ((int) c[ids::id]);
        }
        else
        {
            item.region.setProperty (ids::start, newStart, &p.undo);
            if (auto parent = item.region.getParent(); parent != target)
            {
                parent.removeChild (item.region, &p.undo);
                target.addChild (item.region, -1, &p.undo);
            }
            newSelection.add ((int) item.region[ids::id]);
        }
    }

    studio.setSelectedRegions (newSelection);
    if (trackDelta != 0)
        studio.selectTrack ((int) p.track (juce::jlimit (0, p.numTracks() - 1, grabbedTrack + trackDelta))[ids::id]);
}

void Lanes::commitTrim()
{
    if (! grabbed.isValid() || (trimStart == grabStart && trimLength == grabLength) || trimLength <= 0)
        return;

    auto& p = view.project();
    p.undo.beginNewTransaction ("Trim Region");
    grabbed.setProperty (ids::start, trimStart, &p.undo);
    grabbed.setProperty (ids::offset, trimOffset, &p.undo);
    grabbed.setProperty (ids::length, trimLength, &p.undo);
}

void Lanes::updateRubberBand (const juce::MouseEvent& e)
{
    rubber = juce::Rectangle<int> (e.getMouseDownPosition(), e.getPosition());
    auto& p = view.project();
    auto selection = selectionAtDown;

    for (int i = 0; i < p.numTracks(); ++i)
        for (auto& r : regionsOf (p.track (i)))
            if (regionRect (i, (juce::int64) r[ids::start], (juce::int64) r[ids::length]).intersects (rubber.toFloat()))
                selection.addIfNotAlreadyThere ((int) r[ids::id]);

    view.studio.setSelectedRegions (selection);
    repaint();
}

void Lanes::showRegionMenu()
{
    auto& studio = view.studio;
    juce::PopupMenu m;
    m.setLookAndFeel (&view.getLookAndFeelForMenus());
    m.addItem ("Split at Playhead", [&studio] { studio.splitSelectedAtPlayhead(); });
    m.addItem ("Loop These Regions", [&studio] { studio.commands.invokeDirectly (cmd::loopToSelection, false); });
    m.addSeparator();
    m.addItem ("Delete", [&studio] { studio.deleteSelectedRegions(); });
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void Lanes::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    view.handleWheel (e, w, (float) e.x);
}

void Lanes::mouseMagnify (const juce::MouseEvent& e, float scale)
{
    view.zoomAround ((float) e.x, (double) scale);
}

//==============================================================================
bool Lanes::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (isAudioFile (f))
            return true;
    return false;
}

void Lanes::dropTarget (int x, int y)
{
    dropTrack = view.trackIndexAtY (y);
    dropAt = view.snap (view.sampleAtX (x));
    repaint();
}

void Lanes::fileDragEnter (const juce::StringArray&, int x, int y)
{
    dropping = true;
    dropTarget (x, y);
}

void Lanes::fileDragMove (const juce::StringArray&, int x, int y)
{
    dropTarget (x, y);
}

void Lanes::fileDragExit (const juce::StringArray&)
{
    dropping = false;
    repaint();
}

void Lanes::filesDropped (const juce::StringArray& files, int x, int y)
{
    dropping = false;
    dropTarget (x, y);

    auto& p = view.project();
    auto& studio = view.studio;
    const auto projectRate = p.grid().sampleRate;
    p.audioFolder().createDirectory();

    p.undo.beginNewTransaction ("Import Audio");
    auto track = dropTrack >= 0 ? p.track (dropTrack) : juce::ValueTree();
    auto at = dropAt;
    juce::Array<int> added;
    juce::String problem;

    for (auto& path : files)
    {
        const juce::File source (path);
        if (! isAudioFile (path))
            continue;

        const auto info = view.waveforms.info (source);
        if (info.length <= 0)
        {
            problem = "Couldn't read " + source.getFileName();
            continue;
        }

        // Copy it into the project so the project stays self-contained.
        auto dest = source;
        if (! source.isAChildOf (p.audioFolder()))
        {
            dest = p.audioFolder().getNonexistentChildFile (source.getFileNameWithoutExtension(), source.getFileExtension(), false);
            if (! source.copyFileTo (dest))
            {
                problem = "Couldn't copy " + source.getFileName() + " into the project";
                continue;
            }
        }

        if (! track.isValid())
            track = p.addTrack (source.getFileNameWithoutExtension());

        const auto relative = dest.getRelativePathFrom (p.folder).replaceCharacter ('\\', '/');
        auto r = p.addRegion (track, relative, at, 0, info.length, source.getFileNameWithoutExtension());
        added.add ((int) r[ids::id]);
        at += info.length;      // several files go end to end

        if (info.sampleRate > 0.0 && std::abs (info.sampleRate - projectRate) > 1.0)
            problem = source.getFileName() + " is " + juce::String (info.sampleRate / 1000.0, 1) + " kHz; the project runs at "
                    + juce::String (projectRate / 1000.0, 1) + " kHz, so it will play at the wrong speed";
    }

    if (track.isValid())
        studio.selectTrack ((int) track[ids::id]);
    if (! added.isEmpty())
        studio.setSelectedRegions (added);

    if (problem.isNotEmpty())
        studio.flash (problem);
    else if (! added.isEmpty())
        studio.flash (added.size() == 1 ? "Imported 1 file" : "Imported " + juce::String (added.size()) + " files");

    repaint();
}

} // namespace woodshed::app::arrange
