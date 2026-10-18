#include "LoopGridView.h"
#include "ArrangeHeader.h"
#include "ArrangeView.h"
#include "Icons.h"

namespace woodshed::app
{

namespace ids = daw::ids;
namespace lg = loops;

LoopGridView::LoopGridView (Studio& s) : studio (s)
{
    setOpaque (true);
    waveforms.addChangeListener (this);
    studio.addChangeListener (this);
    studio.addProjectSwapListener (this);
    studio.dropRegionsOnLoopGrid = [this] (juce::Point<int> at, const juce::Array<int>& regionIds)
    {
        return isShowing() && dropRegions (at, regionIds);
    };
    attach();
    startTimerHz (30);
}

LoopGridView::~LoopGridView()
{
    stopTimer();
    studio.dropRegionsOnLoopGrid = nullptr;
    watched.removeListener (this);
    studio.removeProjectSwapListener (this);
    studio.removeChangeListener (this);
    waveforms.removeChangeListener (this);
}

void LoopGridView::attach()
{
    if (watched == project().state)
        return;
    watched.removeListener (this);
    watched = project().state;
    watched.addListener (this);
    scrollX = scrollY = 0;
    repaint();
}

void LoopGridView::projectAboutToBeReplaced()
{
    watched.removeListener (this);
    watched = {};
    dragCell = {};
    dragging = false;
    waveforms.clear();
}

void LoopGridView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    attach();
    repaint();
}

void LoopGridView::timerCallback()
{
    if (! isShowing())
        return;
    ++tick;

    // Repaint while anything plays, waits or records (the rings move), and once after.
    bool active = engineActive();
    if (active || wasActive)
        repaint();
    wasActive = active;
}

bool LoopGridView::engineActive() const
{
    if (grid().isPerformRecording()
        || studio.engine().getLoopPlayer().getRecordState() != lg::LoopPlayer::RecordState::idle)
        return true;
    auto& p = project();
    for (int i = 0; i < p.numTracks(); ++i)
    {
        const int id = (int) p.track (i)[ids::id];
        if (grid().playingColumn (id) >= 0)
            return true;
        if (auto rt = studio.engine().getRuntime (id); rt != nullptr && rt->loop.shownNext.load() != -2)
            return true;
    }
    return false;
}

//==============================================================================
juce::Rectangle<int> LoopGridView::bodyArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight).withTrimmedBottom (footerHeight);
}

juce::Rectangle<int> LoopGridView::cellsArea() const
{
    return bodyArea().withTrimmedLeft (nameWidth);
}

int LoopGridView::cellWidth() const
{
    const int n = juce::jmax (1, lg::numScenes (project().state));
    return juce::jlimit (92, 150, cellsArea().getWidth() / n);
}

juce::Rectangle<int> LoopGridView::cellRect (int row, int column) const
{
    const auto area = cellsArea();
    const int w = cellWidth();
    return juce::Rectangle<int> (area.getX() + column * w - scrollX, area.getY() + row * rowHeight - scrollY, w, rowHeight).reduced (3);
}

juce::Rectangle<int> LoopGridView::nameRect (int row) const
{
    return { 0, bodyArea().getY() + row * rowHeight - scrollY, nameWidth, rowHeight };
}

juce::Rectangle<int> LoopGridView::stopRect (int row) const
{
    return nameRect (row).removeFromRight (34).withSizeKeepingCentre (22, 22);
}

juce::Rectangle<int> LoopGridView::sceneRect (int column) const
{
    const int w = cellWidth();
    return juce::Rectangle<int> (cellsArea().getX() + column * w - scrollX, getHeight() - footerHeight, w, footerHeight).reduced (3, 5);
}

LoopGridView::Hit LoopGridView::hitAt (juce::Point<int> p) const
{
    Hit h;
    const auto body = bodyArea();
    const int scenes = lg::numScenes (project().state);
    if (p.y >= getHeight() - footerHeight && p.x >= cellsArea().getX())
    {
        for (int c = 0; c < scenes; ++c)
            if (sceneRect (c).contains (p))
                return { Hit::scene, -1, c };
        return h;
    }
    if (! body.contains (p))
        return h;

    const int row = (p.y - body.getY() + scrollY) / rowHeight;
    if (row < 0 || row >= project().numTracks())
        return h;
    if (p.x < nameWidth)
        return { stopRect (row).expanded (4).contains (p) ? Hit::stop : Hit::name, row, -1 };

    const int column = (p.x - cellsArea().getX() + scrollX) / cellWidth();
    if (column >= 0 && column < scenes && cellRect (row, column).expanded (3).contains (p))
        return { Hit::cell, row, column };
    return h;
}

void LoopGridView::clampScroll()
{
    const int contentW = lg::numScenes (project().state) * cellWidth() + 8;
    const int contentH = project().numTracks() * rowHeight + 8;
    scrollX = juce::jlimit (0, juce::jmax (0, contentW - cellsArea().getWidth()), scrollX);
    scrollY = juce::jlimit (0, juce::jmax (0, contentH - bodyArea().getHeight()), scrollY);
}

void LoopGridView::resized()
{
    // Header buttons, right to left.
    auto h = getLocalBounds().removeFromTop (headerHeight).reduced (10, 6);
    quantizeButton = h.removeFromRight (124);
    h.removeFromRight (6);
    performButton = h.removeFromRight (96);
    h.removeFromRight (6);
    stopAllButton = h.removeFromRight (78);
    h.removeFromRight (6);
    addSceneButton = h.removeFromRight (78);
    clampScroll();
}

//==============================================================================
void LoopGridView::drawRing (juce::Graphics& g, juce::Rectangle<float> r, float progress, juce::Colour colour, bool pulse)
{
    const auto c = r.getCentre();
    const float radius = r.getWidth() * 0.5f - 1.5f;
    g.setColour (colour.withAlpha (0.28f));
    g.drawEllipse (r.reduced (1.5f), 2.0f);
    if (pulse)
    {
        g.setColour (colour);
        g.drawEllipse (r.reduced (1.5f), 2.0f);
    }
    else if (progress > 0.0f)
    {
        juce::Path arc;
        arc.addCentredArc (c.x, c.y, radius, radius, 0.0f, 0.0f, juce::MathConstants<float>::twoPi * juce::jlimit (0.0f, 1.0f, progress), true);
        g.setColour (colour);
        g.strokePath (arc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void LoopGridView::drawHeader (juce::Graphics& g)
{
    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (theme::bar);
    g.fillRect (header);
    g.setColour (theme::highlight);
    g.fillRect (header.withHeight (1));
    g.setColour (theme::separator);
    g.fillRect (header.removeFromBottom (1));

    auto title = header.reduced (12, 0);
    icons::draw (g, icons::loopGrid(), title.removeFromLeft (16).toFloat().withSizeKeepingCentre (15.0f, 15.0f), theme::accent);
    title.removeFromLeft (8);
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, theme::Weight::semibold));
    g.drawText ("Loop Grid", title.removeFromLeft (76), juce::Justification::centredLeft);
    g.setColour (theme::faintText);
    g.setFont (theme::font (12.0f));
    g.drawText ("Click a cell to launch it; on an armed track, click an empty cell to record", title.withRight (addSceneButton.getX() - 8),
                juce::Justification::centredLeft, true);

    auto button = [&] (juce::Rectangle<int> r, const juce::String& text, bool lit, juce::Colour litColour, bool isHover)
    {
        theme::drawRaised (g, r.toFloat(), theme::radiusControl, lit ? litColour.withAlpha (0.22f) : isHover ? theme::controlHover : theme::control);
        g.setColour (lit ? litColour : theme::text);
        g.setFont (theme::font (12.0f, theme::Weight::medium));
        g.drawText (text, r, juce::Justification::centred);
    };

    const auto mouse = getMouseXYRelative();
    const bool performing = grid().isPerformRecording();
    button (addSceneButton, "+ Scene", false, theme::accent, addSceneButton.contains (mouse));
    button (stopAllButton, "Stop All", false, theme::accent, stopAllButton.contains (mouse));
    button (performButton, "     Perform", performing, theme::record, performButton.contains (mouse));
    {
        auto dot = performButton.withTrimmedLeft (12).removeFromLeft (10).withSizeKeepingCentre (9, 9).toFloat();
        g.setColour (performing && (tick / 8) % 2 == 0 ? theme::record : theme::record.withAlpha (performing ? 0.45f : 0.8f));
        g.fillEllipse (dot);
    }
    button (quantizeButton, "Quantize: " + lg::quantizeName (lg::gridQuantize (project().state)), false, theme::accent,
            quantizeButton.contains (mouse));
}

void LoopGridView::drawPreview (juce::Graphics& g, const juce::ValueTree& cell, juce::Rectangle<float> area, juce::Colour colour)
{
    const auto content = lg::contentOf (cell);
    const auto pass = (double) juce::jmax ((juce::int64) 1, lg::cellLength (project().grid(), cell));
    const auto& grid = project().grid();
    if (! content.isValid() || area.getWidth() < 4.0f)
        return;

    const auto start = (double) (juce::int64) content[ids::start];
    const auto length = juce::jmin ((double) (juce::int64) content[ids::length], pass - start);

    if (daw::Project::isMidiRegion (content))
    {
        int lo = 127, hi = 0;
        for (auto n : content)
        {
            lo = juce::jmin (lo, (int) n[ids::pitch]);
            hi = juce::jmax (hi, (int) n[ids::pitch]);
        }
        if (hi < lo)
            return;
        lo -= 1; hi += 1;
        const float rowH = juce::jlimit (1.5f, 5.0f, area.getHeight() / (float) (hi - lo + 1));
        const float spanH = rowH * (float) (hi - lo + 1);
        const float top = area.getCentreY() - spanH * 0.5f;
        const auto origin = start - (double) (juce::int64) content[ids::offset];
        g.setColour (colour.interpolatedWith (juce::Colours::white, 0.35f));
        for (auto n : content)
        {
            const auto note = daw::Project::noteFrom (n);
            const auto on = origin + grid.fromTicks ((double) note.start);
            if (on < start || on >= start + length)
                continue;
            const auto off = juce::jmin (start + length, on + grid.fromTicks ((double) note.length));
            const float x = area.getX() + (float) (on / pass) * area.getWidth();
            const float w = juce::jmax (1.5f, (float) ((off - on) / pass) * area.getWidth() - 0.5f);
            g.fillRect (x, top + (float) (hi - note.pitch) * rowH, w, juce::jmax (1.0f, rowH - 0.5f));
        }
        return;
    }

    const auto file = project().resolve (content[ids::file].toString());
    const auto info = waveforms.info (file);
    if (auto* thumb = info.length > 0 ? waveforms.thumbnail (file) : nullptr; thumb != nullptr && length > 0)
    {
        const double rate = info.sampleRate > 0.0 ? info.sampleRate : grid.sampleRate;
        const auto offset = (double) (juce::int64) content[ids::offset];
        auto r = area.withX (area.getX() + (float) (start / pass) * area.getWidth()).withWidth ((float) (length / pass) * area.getWidth());
        const auto peak = thumb->getApproximatePeak();
        g.setColour (colour.interpolatedWith (juce::Colours::white, 0.35f));
        thumb->drawChannel (g, r.toNearestInt(), offset / rate, (offset + length) / rate, 0, peak > 0.0f ? juce::jlimit (1.0f, 8.0f, 0.9f / peak) : 1.0f);
    }
}

void LoopGridView::drawCell (juce::Graphics& g, const juce::ValueTree& track, int row, int column)
{
    const auto r = cellRect (row, column).toFloat();
    const int trackId = (int) track[ids::id];
    const auto cell = lg::cellAt (track, column);
    const auto status = grid().statusOf (trackId, column);
    const auto colour = arrange::trackColour (track);
    const bool isHover = hover.part == Hit::cell && hover.row == row && hover.column == column;
    const bool pulse = (tick / 8) % 2 == 0;
    using S = lg::LoopGrid::Status;

    if (daw::Project::isAuxTrack (track))
        return;

    const bool selected = selectedColumn == column && studio.getSelectedTrackId() == trackId;

    if (! cell.isValid())
    {
        g.setColour (isHover ? theme::panelLight : theme::panel.withAlpha (0.6f));
        g.fillRoundedRectangle (r, theme::radiusChip);
        g.setColour (selected ? theme::accent.withAlpha (0.7f) : theme::outline.withAlpha (0.55f));
        g.drawRoundedRectangle (r.reduced (0.5f), theme::radiusChip, 1.0f);

        const auto mid = r.withSizeKeepingCentre (18.0f, 18.0f);
        if (status.state == S::recording || status.state == S::recordQueued)
        {
            g.setColour (theme::record.withAlpha (status.state == S::recording ? 0.22f : 0.1f));
            g.fillRoundedRectangle (r, theme::radiusChip);
            drawRing (g, mid.expanded (2.0f), status.progress, theme::record, status.state == S::recordQueued && pulse);
            g.setColour (theme::record);
            g.fillEllipse (mid.reduced (5.0f));
        }
        else if ((bool) track[ids::arm])
        {
            g.setColour (theme::record.withAlpha (isHover ? 0.8f : 0.35f));
            g.fillEllipse (mid.reduced (4.0f));
        }
        else if (isHover)
            icons::draw (g, icons::plus(), mid, theme::faintText, 1.6f);
        return;
    }

    const bool playing = status.state == S::playing || status.state == S::stopping || (status.state == S::queued && status.progress > 0.0f);
    g.setColour (colour.withAlpha (playing ? 0.42f : isHover ? 0.28f : 0.2f));
    g.fillRoundedRectangle (r, theme::radiusChip);
    g.setColour (selected ? theme::accent : colour.withAlpha (playing ? 1.0f : 0.55f));
    g.drawRoundedRectangle (r.reduced (0.5f), theme::radiusChip, selected || playing ? 1.5f : 1.0f);

    // The launch ring, top left: a triangle to play, running round while it plays.
    auto inner = r.reduced (6.0f, 4.0f);
    auto top = inner.removeFromTop (16.0f);
    const auto ring = top.removeFromLeft (16.0f);
    const auto ringColour = playing ? colour.interpolatedWith (juce::Colours::white, 0.4f) : theme::dimText;
    if (status.state == S::stopping)
    {
        drawRing (g, ring, status.progress, ringColour, false);
        g.setColour (pulse ? theme::text : ringColour);
        g.fillRect (ring.withSizeKeepingCentre (6.0f, 6.0f));
    }
    else
    {
        drawRing (g, ring, playing ? status.progress : 0.0f, status.state == S::queued ? theme::accent : ringColour,
                  status.state == S::queued && pulse);
        juce::Path tri;
        const auto t = ring.withSizeKeepingCentre (6.0f, 7.0f).translated (0.6f, 0.0f);
        tri.addTriangle (t.getX(), t.getY(), t.getX(), t.getBottom(), t.getRight(), t.getCentreY());
        g.setColour (playing ? juce::Colours::white : status.state == S::queued ? theme::accent : theme::dimText);
        g.fillPath (tri);
    }

    top.removeFromLeft (5.0f);
    const auto content = lg::contentOf (cell);
    g.setColour (playing ? theme::text : theme::dimText);
    g.setFont (theme::font (11.0f, theme::Weight::medium));
    const auto bars = lg::barsOf (cell);
    const juce::String length = lg::isOneShot (cell) ? juce::String (bars) + " bar 1-shot" : juce::String (bars) + (bars == 1 ? " bar" : " bars");
    g.drawText (content[ids::name].toString(), top.withTrimmedRight (40.0f), juce::Justification::centredLeft, true);
    g.setColour (theme::faintText);
    g.setFont (theme::font (10.0f));
    g.drawText (length, top, juce::Justification::centredRight, false);

    inner.removeFromTop (2.0f);
    drawPreview (g, cell, inner, colour);

    // Where the pass is.
    if (playing && status.progress > 0.0f)
    {
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.fillRect (inner.getX() + inner.getWidth() * status.progress, inner.getY(), 1.0f, inner.getHeight());
    }
}

void LoopGridView::drawRow (juce::Graphics& g, int row)
{
    const auto track = project().track (row);
    const int trackId = (int) track[ids::id];
    auto name = nameRect (row);
    const bool selected = studio.getSelectedTrackId() == trackId;

    g.setColour (selected ? theme::panelLight : theme::panel);
    g.fillRect (name);
    g.setColour (theme::separator.withAlpha (0.7f));
    g.fillRect (name.getX(), name.getBottom() - 1, getWidth(), 1);

    const auto colour = arrange::trackColour (track);
    g.setColour (colour);
    g.fillRect (name.removeFromLeft (4).reduced (0, 6));

    auto text = name.reduced (10, 0).withTrimmedRight (30);
    g.setColour (daw::Project::isAuxTrack (track) ? theme::faintText : theme::text);
    g.setFont (theme::font (12.5f, theme::Weight::semibold));
    g.drawText (track[ids::name].toString(), text.removeFromTop (rowHeight / 2 + 4).withTrimmedTop (8), juce::Justification::bottomLeft, true);
    g.setColour (theme::faintText);
    g.setFont (theme::font (11.0f));
    const juce::String kind = daw::Project::isAuxTrack (track) ? "Bus" : daw::Project::isInstrumentTrack (track) ? "MIDI" : "Audio";
    g.drawText ((bool) track[ids::arm] ? kind + "  " + juce::String::fromUTF8 ("\xc2\xb7") + "  armed" : kind, text.withTrimmedTop (2),
                juce::Justification::topLeft, true);

    if (daw::Project::isAuxTrack (track))
        return;

    // The track's stop button: lit while a cell plays, blinking while a stop waits.
    const auto stop = stopRect (row).toFloat();
    const bool playing = grid().playingColumn (trackId) >= 0;
    const bool stopping = grid().isStopQueued (trackId);
    const bool isHover = hover.part == Hit::stop && hover.row == row;
    theme::drawRaised (g, stop, theme::radiusChip, isHover ? theme::controlHover : theme::control);
    g.setColour (stopping ? ((tick / 8) % 2 == 0 ? theme::text : theme::faintText) : playing ? colour : theme::faintText);
    g.fillRoundedRectangle (stop.withSizeKeepingCentre (8.0f, 8.0f), 1.5f);
}

void LoopGridView::drawFooter (juce::Graphics& g)
{
    auto footer = getLocalBounds().removeFromBottom (footerHeight);
    g.setColour (theme::bar);
    g.fillRect (footer);
    g.setColour (theme::separator);
    g.fillRect (footer.withHeight (1));

    g.setColour (theme::dimText);
    g.setFont (theme::font (11.5f, theme::Weight::medium));
    g.drawText ("Scenes", footer.withWidth (nameWidth).reduced (14, 0), juce::Justification::centredLeft);

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (footer.withTrimmedLeft (nameWidth));
    const auto names = lg::sceneNames (project().state);
    for (int c = 0; c < names.size(); ++c)
    {
        const auto r = sceneRect (c);
        const bool isHover = hover.part == Hit::scene && hover.column == c;
        theme::drawRaised (g, r.toFloat(), theme::radiusChip, isHover ? theme::controlHover : theme::control);
        auto inner = r.reduced (8, 0);
        juce::Path tri;
        const auto t = inner.removeFromLeft (9).toFloat().withSizeKeepingCentre (7.0f, 8.0f);
        tri.addTriangle (t.getX(), t.getY(), t.getX(), t.getBottom(), t.getRight(), t.getCentreY());
        g.setColour (isHover ? theme::accent : theme::dimText);
        g.fillPath (tri);
        inner.removeFromLeft (6);
        g.setColour (theme::text);
        g.setFont (theme::font (11.5f, theme::Weight::medium));
        g.drawText (names[c], inner, juce::Justification::centredLeft, true);
    }
}

void LoopGridView::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
    clampScroll();

    auto& p = project();
    const auto body = bodyArea();
    const int scenes = lg::numScenes (p.state);

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (body);

        // Lanes behind the cells, alternating like the arrange.
        for (int row = 0; row < p.numTracks(); ++row)
        {
            const auto lane = juce::Rectangle<int> (nameWidth, body.getY() + row * rowHeight - scrollY, getWidth() - nameWidth, rowHeight);
            if (lane.getBottom() < body.getY() || lane.getY() > body.getBottom())
                continue;
            g.setColour (row % 2 == 0 ? theme::lane : theme::laneAlt);
            g.fillRect (lane);
        }

        {
            juce::Graphics::ScopedSaveState cells (g);
            g.reduceClipRegion (cellsArea());
            for (int row = 0; row < p.numTracks(); ++row)
            {
                if (nameRect (row).getBottom() < body.getY() || nameRect (row).getY() > body.getBottom())
                    continue;
                const auto track = p.track (row);
                for (int c = 0; c < scenes; ++c)
                    if (cellRect (row, c).getRight() >= cellsArea().getX() && cellRect (row, c).getX() <= getWidth())
                        drawCell (g, track, row, c);
            }
        }

        for (int row = 0; row < p.numTracks(); ++row)
            if (nameRect (row).getBottom() >= body.getY() && nameRect (row).getY() <= body.getBottom())
                drawRow (g, row);

        g.setColour (theme::separator);
        g.fillRect (nameWidth - 1, body.getY(), 1, body.getHeight());

        if (p.numTracks() == 0)
        {
            g.setColour (theme::dimText);
            g.setFont (theme::font (13.0f));
            g.drawText ("Add a track to start a Loop Grid.", body, juce::Justification::centred);
        }
    }

    drawHeader (g);
    drawFooter (g);

    // The cell being dragged, under the mouse.
    if (dragging && dragCell.isValid())
    {
        const auto ghost = juce::Rectangle<float> ((float) cellWidth() - 6.0f, (float) rowHeight - 6.0f).withCentre (dragAt.toFloat());
        const auto colour = arrange::trackColour (lg::trackOfCell (dragCell));
        g.setColour (colour.withAlpha (0.35f));
        g.fillRoundedRectangle (ghost, theme::radiusChip);
        g.setColour (colour);
        g.drawRoundedRectangle (ghost, theme::radiusChip, 1.5f);
    }
}

//==============================================================================
void LoopGridView::mouseMove (const juce::MouseEvent& e)
{
    const auto h = hitAt (e.getPosition());
    if (h.part != hover.part || h.row != hover.row || h.column != hover.column)
    {
        hover = h;
        repaint();
    }
    else if (e.y < headerHeight)
        repaint (getLocalBounds().removeFromTop (headerHeight));
}

void LoopGridView::mouseExit (const juce::MouseEvent&)
{
    hover = {};
    repaint();
}

void LoopGridView::mouseDown (const juce::MouseEvent& e)
{
    dragging = false;
    dragCell = {};
    const auto pos = e.getPosition();
    auto& p = project();

    if (pos.y < headerHeight)
    {
        if (quantizeButton.contains (pos))      showQuantizeMenu();
        else if (performButton.contains (pos))  grid().setPerformRecording (! grid().isPerformRecording());
        else if (stopAllButton.contains (pos))  grid().stopAll();
        else if (addSceneButton.contains (pos))
        {
            p.undo.beginNewTransaction ("Add Scene");
            lg::addScene (p);
        }
        repaint();
        return;
    }

    const auto hit = hitAt (pos);
    if (hit.part == Hit::scene)
    {
        if (e.mods.isPopupMenu())
            showSceneMenu (hit.column);
        else
            grid().launchScene (hit.column);
        return;
    }
    if (hit.row < 0)
        return;

    const auto track = p.track (hit.row);
    studio.selectTrack ((int) track[ids::id]);

    if (hit.part == Hit::stop)
    {
        grid().stopTrack ((int) track[ids::id]);
        return;
    }
    if (hit.part != Hit::cell || daw::Project::isAuxTrack (track))
        return;

    selectedColumn = hit.column;
    if (e.mods.isPopupMenu())
    {
        showCellMenu (track, hit.column);
        return;
    }
    dragCell = lg::cellAt (track, hit.column);
    repaint();
}

void LoopGridView::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragCell.isValid() || e.mods.isPopupMenu())
        return;
    if (! dragging && e.getDistanceFromDragStart() < 5)
        return;
    dragging = true;
    dragAt = e.getPosition();
    setMouseCursor (e.mods.isAltDown() ? juce::MouseCursor::CopyingCursor : juce::MouseCursor::DraggingHandCursor);
    repaint();
}

void LoopGridView::mouseUp (const juce::MouseEvent& e)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
    const auto cell = dragCell;
    const bool wasDragging = dragging;
    dragCell = {};
    dragging = false;

    if (e.mods.isPopupMenu())
        return;

    const auto hit = hitAt (e.getMouseDownPosition());
    if (! wasDragging)
    {
        if (hit.part == Hit::cell && hit.row >= 0 && hit.row < project().numTracks() && e.getNumberOfClicks() < 2)
            clickCell (project().track (hit.row), hit.column);
        repaint();
        return;
    }

    if (! cell.isValid())
        return;
    if (getLocalBounds().contains (e.getPosition()))
    {
        const auto target = hitAt (e.getPosition());
        if (target.part == Hit::cell)
        {
            auto& p = project();
            p.undo.beginNewTransaction (e.mods.isAltDown() ? "Copy Cell" : "Move Cell");
            if (lg::moveCell (p, cell, p.track (target.row), target.column, e.mods.isAltDown()).isValid())
                selectedColumn = target.column;
        }
    }
    else
        dropCellOnArrange (cell, e.getScreenPosition());
    repaint();
}

void LoopGridView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto hit = hitAt (e.getPosition());
    if (hit.part == Hit::scene)
    {
        renameScene (hit.column);
        return;
    }
    if (hit.part == Hit::cell)
    {
        const auto track = project().track (hit.row);
        const auto cell = lg::cellAt (track, hit.column);
        if (lg::isMidiCell (cell))
        {
            studio.selectTrack ((int) track[ids::id]);
            studio.openInEditor ((int) lg::contentOf (cell)[ids::id]);
        }
    }
}

void LoopGridView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    float dx = w.deltaX, dy = w.deltaY;
    if (e.mods.isShiftDown() && juce::exactlyEqual (dx, 0.0f))
        std::swap (dx, dy);
    scrollX -= juce::roundToInt (dx * 200.0f);
    scrollY -= juce::roundToInt (dy * 200.0f);
    clampScroll();
    repaint();
}

//==============================================================================
void LoopGridView::clickCell (const juce::ValueTree& track, int column)
{
    const int trackId = (int) track[ids::id];
    const auto cell = lg::cellAt (track, column);
    const auto status = grid().statusOf (trackId, column);
    using S = lg::LoopGrid::Status;

    if (status.state == S::recording)
    {
        grid().finishRecording();
        return;
    }
    if (cell.isValid())
    {
        // A playing cell stops; anything else launches (a waiting one launches again, which
        // changes nothing).
        if (status.state == S::playing)
            grid().stopTrack (trackId);
        else
            grid().launchCell (trackId, column);
        return;
    }
    if ((bool) track[ids::arm])
    {
        if (status.state != S::recordQueued && ! grid().recordCell (trackId, column, 0))
            studio.flash ("Another cell is still recording");
        return;
    }
    studio.flash ("Arm the track (R in its header) to record into an empty cell");
}

void LoopGridView::showCellMenu (const juce::ValueTree& track, int column)
{
    auto& p = project();
    const int trackId = (int) track[ids::id];
    auto cell = lg::cellAt (track, column);
    juce::PopupMenu m;

    if (! cell.isValid())
    {
        juce::PopupMenu record;
        for (int bars : { 1, 2, 4, 8, 16 })
            record.addItem ("Record " + juce::String (bars) + (bars == 1 ? " Bar" : " Bars"), [this, trackId, column, bars]
                            { grid().recordCell (trackId, column, bars); });
        record.addItem ("Record Until Clicked Again", [this, trackId, column] { grid().recordCell (trackId, column, 0); });
        m.addSubMenu ("Record", record, ! daw::Project::isAuxTrack (track));

        if (daw::Project::isInstrumentTrack (track))
        {
            juce::PopupMenu fresh;
            for (int bars : { 1, 2, 4, 8 })
                fresh.addItem (juce::String (bars) + (bars == 1 ? " Bar" : " Bars"), [this, track, column, bars]
                               {
                                   auto& pr = project();
                                   pr.undo.beginNewTransaction ("New MIDI Cell");
                                   const auto made = lg::addMidiCell (pr, track, column, bars);
                                   studio.selectTrack ((int) track[ids::id]);
                                   studio.openInEditor ((int) lg::contentOf (made)[ids::id]);
                               });
            m.addSubMenu ("New MIDI Cell", fresh);
        }

        const auto selection = studio.getSelectedRegions();
        m.addItem ("Copy Selected Region Here", ! selection.isEmpty(), false, [this, selection, track, column]
                   {
                       auto& pr = project();
                       pr.undo.beginNewTransaction ("Copy to Loop Grid");
                       if (! lg::copyRegionToCell (pr, studio.findRegion (selection[0]), track, column).isValid())
                           studio.flash ("Audio goes into audio tracks' cells, MIDI into instrument tracks'");
                   });
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
        return;
    }

    const auto status = grid().statusOf (trackId, column);
    if (status.state == lg::LoopGrid::Status::playing)
        m.addItem ("Stop", [this, trackId] { grid().stopTrack (trackId); });
    else
        m.addItem ("Play", [this, trackId, column] { grid().launchCell (trackId, column); });
    if (lg::isMidiCell (cell))
        m.addItem ("Edit Notes", [this, cell, trackId]
                   {
                       studio.selectTrack (trackId);
                       studio.openInEditor ((int) lg::contentOf (cell)[ids::id]);
                   });
    m.addSeparator();

    auto set = [this] (juce::ValueTree c, const juce::Identifier& id, const juce::var& v, const juce::String& what)
    {
        project().undo.beginNewTransaction (what);
        c.setProperty (id, v, &project().undo);
    };

    juce::PopupMenu mode;
    mode.addItem ("Loop", true, ! lg::isOneShot (cell), [set, cell] { set (cell, lg::ids::playMode, "loop", "Play Mode"); });
    mode.addItem ("One Shot", true, lg::isOneShot (cell), [set, cell] { set (cell, lg::ids::playMode, "oneshot", "Play Mode"); });
    m.addSubMenu ("Play Mode", mode);

    juce::PopupMenu length;
    for (int bars : { 1, 2, 4, 8, 16, 32 })
        length.addItem (juce::String (bars) + (bars == 1 ? " Bar" : " Bars"), true, lg::barsOf (cell) == bars,
                        [set, cell, bars] { set (cell, lg::ids::bars, bars, "Cell Length"); });
    m.addSubMenu ("Length", length);

    juce::PopupMenu quantize;
    quantize.addItem ("Grid (" + lg::quantizeName (lg::gridQuantize (p.state)) + ")", true, ! cell.hasProperty (lg::ids::quantize), [this, cell]
                      {
                          project().undo.beginNewTransaction ("Cell Quantize");
                          juce::ValueTree (cell).removeProperty (lg::ids::quantize, &project().undo);
                      });
    for (int q = 0; q < lg::numQuantizeChoices; ++q)
        quantize.addItem (lg::quantizeName ((lg::Quantize) q), true, cell.hasProperty (lg::ids::quantize) && (int) cell[lg::ids::quantize] == q,
                          [set, cell, q] { set (cell, lg::ids::quantize, q, "Cell Quantize"); });
    m.addSubMenu ("Launch Quantize", quantize);
    m.addSeparator();

    m.addItem ("Add to Arrange at Playhead", [this, cell]
               {
                   auto& pr = project();
                   const auto at = studio.engine().getPosition();
                   pr.undo.beginNewTransaction ("Add Cell to Arrange");
                   studio.setSelectedRegions (lg::placeInArrange (pr, cell, at, at + lg::cellLength (pr.grid(), cell)));
               });
    m.addItem ("Delete Cell", [this, cell]
               {
                   auto& pr = project();
                   pr.undo.beginNewTransaction ("Delete Cell");
                   lg::removeCell (pr, cell);
               });
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void LoopGridView::showSceneMenu (int column)
{
    juce::PopupMenu m;
    m.addItem ("Play Scene", [this, column] { grid().launchScene (column); });
    m.addItem ("Rename...", [this, column] { renameScene (column); });
    m.addSeparator();
    m.addItem ("Add Scene", [this]
               {
                   project().undo.beginNewTransaction ("Add Scene");
                   lg::addScene (project());
               });
    m.addItem ("Delete Scene", lg::numScenes (project().state) > 1, false, [this, column]
               {
                   project().undo.beginNewTransaction ("Delete Scene");
                   lg::removeScene (project(), column);
               });
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void LoopGridView::showQuantizeMenu()
{
    juce::PopupMenu m;
    const auto current = lg::gridQuantize (project().state);
    m.addSectionHeader ("Launches land on");
    for (int q = 0; q < lg::numQuantizeChoices; ++q)
        m.addItem (lg::quantizeName ((lg::Quantize) q), true, (int) current == q, [this, q]
                   {
                       project().undo.beginNewTransaction ("Launch Quantize");
                       lg::setGridQuantize (project(), (lg::Quantize) q);
                   });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                         .withTargetScreenArea (localAreaToGlobal (quantizeButton)));
}

void LoopGridView::renameScene (int column)
{
    auto names = lg::sceneNames (project().state);
    if (! juce::isPositiveAndBelow (column, names.size()))
        return;

    nameEditor = std::make_unique<juce::TextEditor>();
    nameEditor->setFont (theme::font (12.0f, theme::Weight::medium));
    nameEditor->setText (names[column], false);
    nameEditor->selectAll();
    nameEditor->setBounds (sceneRect (column));
    addAndMakeVisible (*nameEditor);
    nameEditor->grabKeyboardFocus();

    juce::Component::SafePointer<LoopGridView> safe (this);
    auto finish = [safe, column] (bool keep)
    {
        if (safe == nullptr || safe->nameEditor == nullptr)
            return;
        const auto text = safe->nameEditor->getText().trim();
        if (keep && text.isNotEmpty())
        {
            auto list = lg::sceneNames (safe->project().state);
            if (juce::isPositiveAndBelow (column, list.size()) && list[column] != text)
            {
                list.set (column, text);
                safe->project().undo.beginNewTransaction ("Rename Scene");
                lg::setSceneNames (safe->project(), list);
            }
        }
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->nameEditor = nullptr; });
    };
    nameEditor->onReturnKey = [finish] { finish (true); };
    nameEditor->onFocusLost = [finish] { finish (true); };
    nameEditor->onEscapeKey = [finish] { finish (false); };
}

//==============================================================================
bool LoopGridView::dropRegions (juce::Point<int> screenPos, const juce::Array<int>& regionIds)
{
    const auto hit = hitAt (getLocalPoint (nullptr, screenPos));
    if (hit.part != Hit::cell || regionIds.isEmpty())
        return false;

    auto& p = project();
    p.undo.beginNewTransaction ("Copy to Loop Grid");
    const auto target = p.track (hit.row);
    int copied = 0;
    if (regionIds.size() == 1)
    {
        const auto region = studio.findRegion (regionIds[0]);
        copied += lg::copyRegionToCell (p, region, target, hit.column).isValid() ? 1 : 0;
    }
    else
    {
        // Several: each into this column on its own track (or the next free one along).
        std::map<int, int> nextColumn;
        for (int id : regionIds)
        {
            const auto region = studio.findRegion (id);
            auto track = daw::Project::trackOf (region);
            auto& column = nextColumn.try_emplace ((int) track[ids::id], hit.column).first->second;
            if (column < lg::numScenes (p.state))
                copied += lg::copyRegionToCell (p, region, track, column++).isValid() ? 1 : 0;
        }
    }
    if (copied == 0)
        studio.flash ("Audio goes into audio tracks' cells, MIDI into instrument tracks'");
    selectedColumn = hit.column;
    repaint();
    return true;
}

void LoopGridView::dropCellOnArrange (const juce::ValueTree& cell, juce::Point<int> screenPos)
{
    // Find the arrange under the mouse, and the track and position there.
    auto* top = getTopLevelComponent();
    auto* under = top != nullptr ? top->getComponentAt (top->getLocalPoint (nullptr, screenPos)) : nullptr;
    auto* view = dynamic_cast<ArrangeView*> (under);
    if (view == nullptr && under != nullptr)
        view = under->findParentComponentOfClass<ArrangeView>();
    if (view == nullptr)
        return;

    const auto local = view->getLocalPoint (nullptr, screenPos);
    if (local.x < ArrangeView::headerWidth || local.y < ArrangeView::rulerHeight)
        return;
    auto& p = project();
    const auto at = view->snap (view->sampleAtX (local.x - ArrangeView::headerWidth));
    const int row = view->trackIndexAtY (local.y - ArrangeView::rulerHeight);
    const auto track = row >= 0 ? p.track (row) : lg::trackOfCell (cell);
    p.undo.beginNewTransaction ("Add Cell to Arrange");
    const auto made = lg::placeInArrange (p, cell, at, at + lg::cellLength (p.grid(), cell), track);
    if (made.isEmpty())
        studio.flash ("Audio cells go on audio tracks, MIDI cells on instrument tracks");
    else
        studio.setSelectedRegions (made);
}

} // namespace woodshed::app
