#include "ArrangeView.h"
#include "ArrangeHeader.h"
#include "ArrangeLanes.h"
#include "ArrangeRuler.h"
#include "Icons.h"
#include "Movie.h"
#include "../daw/Stacks.h"
#include "MixCommands.h"

namespace woodshed::app
{

namespace ids = daw::ids;

ArrangeView::ArrangeView (Studio& s) : studio (s)
{
    // Menus, scroll bars and the rename editor come from the app's StudioLookAndFeel.
    ruler = std::make_unique<arrange::Ruler> (*this);
    lanes = std::make_unique<arrange::Lanes> (*this);
    addAndMakeVisible (*ruler);
    addAndMakeVisible (*lanes);

    // Clicks on the empty space under the headers reach this component (double-click: new track).
    headerArea.setInterceptsMouseClicks (false, true);
    addAndMakeVisible (headerArea);

    addButton.setTooltip ("New track");
    addButton.onClick = [this] { addTrackAfterSelection(); };
    addAndMakeVisible (addButton);

    for (auto* sb : { &hScroll, &vScroll })
    {
        sb->addListener (this);
        sb->setAutoHide (false);
        addAndMakeVisible (*sb);
    }

    waveforms.addChangeListener (this);
    studio.addChangeListener (this);
    attachToProject();
    startTimerHz (60);
}

ArrangeView::~ArrangeView()
{
    stopTimer();
    movieStrip.reset();
    studio.removeChangeListener (this);
    waveforms.removeChangeListener (this);
    attached.removeListener (this);
    headers.clear();
}

//==============================================================================
void ArrangeView::attachToProject()
{
    if (attachedProject == &studio.project() && attached == studio.project().state)
        return;

    attached.removeListener (this);
    attached = studio.project().state;
    attachedProject = &studio.project();
    attached.addListener (this);

    waveforms.clear();
    scrollY = 0;
    rebuildHeaders();
    zoomToFit();
}

void ArrangeView::rebuildHeaders()
{
    auto& p = project();
    std::vector<std::unique_ptr<arrange::TrackHeader>> next;

    for (int i = 0; i < p.numTracks(); ++i)
    {
        auto t = p.track (i);
        auto it = std::find_if (headers.begin(), headers.end(), [&t] (auto& h) { return h != nullptr && h->getTrack() == t; });
        if (it != headers.end())
        {
            next.push_back (std::move (*it));
        }
        else
        {
            next.push_back (std::make_unique<arrange::TrackHeader> (*this, t));
            headerArea.addAndMakeVisible (*next.back());
        }
    }

    headers = std::move (next);
    updateRows();
    resized();
    repaint();
}

void ArrangeView::layoutHeaders()
{
    for (size_t i = 0; i < headers.size(); ++i)
    {
        const bool shown = i < rowOfTrack.size() && rowOfTrack[i] >= 0;
        headers[i]->setVisible (shown);
        headers[i]->setBounds (0, yForTrack ((int) i), headerArea.getWidth(), trackHeight);
    }
}

void ArrangeView::contentChanged()
{
    updateScrollBars();
    lanes->repaint();
}

//==============================================================================
void ArrangeView::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &waveforms)
    {
        lanes->repaint();
        return;
    }

    // The project may have been replaced; otherwise the selection or panes changed.
    attachToProject();
    for (auto& h : headers)
        h->repaint();
    lanes->repaint();
    ruler->repaint();
}

void ArrangeView::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType (ids::RIG))
        return;

    if (tree == attached)
    {
        if (property == ids::selectedTrack)
            for (auto& h : headers)
                h->repaint();
        ruler->repaint();
        lanes->repaint();
        if (property == ids::tempo || property == ids::beatsPerBar || property == ids::beatUnit
            || property == ids::loopEnd)
            updateScrollBars();
        return;
    }

    if (tree.hasType (ids::REGION))
        contentChanged();
    else if (tree.hasType (ids::TRACK))
    {
        // Track Stacks: collapsing (or a track joining or leaving one) changes the rows.
        if (property == ids::collapsed || property == ids::stack || property == ids::busRole)
        {
            updateRows();
            resized();
            for (auto& h : headers)
                h->repaint();
        }
        lanes->repaint();
        if (property == ids::autoParam || property == ids::group || property == ids::frozen || property == ids::vca)
            for (auto& h : headers)
                h->repaint();
    }
    else if (tree.hasType (ids::NOTE) || tree.hasType (ids::AUTOMATION))
        lanes->repaint();
}

void ArrangeView::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent == attached)
        rebuildHeaders();
    contentChanged();
}

void ArrangeView::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent == attached)
        rebuildHeaders();
    contentChanged();
}

void ArrangeView::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent == attached)
    {
        updateRows();
        layoutHeaders();
        for (auto& h : headers)
            h->repaint();      // track numbers
    }
    lanes->repaint();
}

void ArrangeView::valueTreeRedirected (juce::ValueTree&)
{
    rebuildHeaders();
    contentChanged();
}

//==============================================================================
void ArrangeView::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    // The header column, continuing below the last track.
    auto column = getLocalBounds().removeFromLeft (headerWidth);
    g.setColour (theme::panel);
    g.fillRect (column);

    // Header bar above the track headers, level with the ruler.
    auto corner = column.removeFromTop (rulerHeight);
    g.setColour (theme::bar);
    g.fillRect (corner);

    // The marker row's name, level with the marker lane at the bottom of the ruler.
    auto markerRow = corner.removeFromBottom (arrange::Ruler::markerHeight);
    g.setColour (theme::panel);
    g.fillRect (markerRow);
    g.setColour (theme::separator.withAlpha (0.6f));
    g.fillRect (markerRow.getX(), markerRow.getY(), markerRow.getWidth(), 1);
    g.setColour (theme::dimText);
    g.setFont (theme::font (11.0f, theme::Weight::medium));
    g.drawText ("Marker", markerRow.withTrimmedLeft (14), juce::Justification::centredLeft);

    auto label = corner.withTrimmedLeft (14).withTrimmedRight (40);
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, theme::Weight::semibold));
    const juce::String title ("Tracks");
    g.drawText (title, label, juce::Justification::centredLeft);

    const int n = project().numTracks();
    const int titleWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (theme::font (13.0f, theme::Weight::semibold), title));
    g.setColour (theme::faintText);
    g.setFont (theme::font (12.0f, theme::Weight::medium));
    g.drawText (juce::String (n), label.withTrimmedLeft (titleWidth + 6), juce::Justification::centredLeft);

    // Hairlines: under the header bar and ruler, and between the headers and the lanes.
    g.setColour (theme::separator);
    g.fillRect (0, rulerHeight - 1, getWidth(), 1);
    g.fillRect (headerWidth - 1, 0, 1, getHeight());
}

void ArrangeView::AddButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto r = getLocalBounds().toFloat();
    if (highlighted || down)
    {
        g.setColour (down ? theme::controlDown : theme::controlHover.withAlpha (0.7f));
        g.fillRoundedRectangle (r, theme::radiusControl);
    }
    icons::draw (g, icons::plus(), r.withSizeKeepingCentre (14.0f, 14.0f), highlighted ? theme::text : theme::dimText, 1.8f);
}

void ArrangeView::attachMovie (MovieController* movie)
{
    movieStrip.reset();
    if (movie != nullptr)
    {
        movieStrip = std::make_unique<MovieStrip> (*this, *movie);
        addChildComponent (*movieStrip);
    }
    resized();
}

int ArrangeView::movieStripHeight() const
{
    return movieStrip != nullptr && movieStrip->isWanted() ? MovieStrip::height : 0;
}

void ArrangeView::resized()
{
    auto r = getLocalBounds();
    auto left = r.removeFromLeft (headerWidth);
    auto corner = left.removeFromTop (rulerHeight).withTrimmedBottom (arrange::Ruler::markerHeight);
    addButton.setBounds (corner.removeFromRight (36).withSizeKeepingCentre (24, 24).translated (2, 0));
    const int stripHeight = movieStripHeight();
    left.removeFromTop (stripHeight);
    headerArea.setBounds (left.withTrimmedBottom (scrollBarSize).withTrimmedRight (1));

    auto top = r.removeFromTop (rulerHeight);
    if (movieStrip != nullptr)
    {
        movieStrip->setVisible (stripHeight > 0);
        movieStrip->setBounds (0, r.removeFromTop (stripHeight).getY(), getWidth(), stripHeight);
    }
    hScroll.setBounds (r.removeFromBottom (scrollBarSize));

    const int contentHeight = visibleRows() * trackHeight + trackHeight / 2;
    const bool needV = contentHeight > r.getHeight();
    vScroll.setVisible (needV);
    if (needV)
    {
        vScroll.setBounds (r.removeFromRight (scrollBarSize));
        top.removeFromRight (scrollBarSize);
    }

    ruler->setBounds (top);
    lanes->setBounds (r);

    scrollY = juce::jlimit (0, juce::jmax (0, contentHeight - lanesHeight()), scrollY);

    // Until the user zooms or scrolls, the song stays fitted to the window as it resizes.
    if (fitPending || keepFitted)
        zoomToFit();

    layoutHeaders();
    updateScrollBars();
}

//==============================================================================
int ArrangeView::lanesWidth() const   { return lanes != nullptr ? lanes->getWidth() : 0; }
int ArrangeView::lanesHeight() const  { return lanes != nullptr ? lanes->getHeight() : 0; }

double ArrangeView::getViewLength() const
{
    return juce::jmax (1, lanesWidth()) * samplesPerPixel;
}

int ArrangeView::trackIndexAtY (int y) const
{
    if (y + scrollY < 0)
        return -1;
    const auto row = (size_t) ((y + scrollY) / trackHeight);
    return row < trackAtRow.size() ? trackAtRow[row] : -1;
}

int ArrangeView::yForTrack (int index) const
{
    if (index < 0)
        return index * trackHeight - scrollY;
    if ((size_t) index >= rowOfTrack.size())
        return (visibleRows() + index - (int) rowOfTrack.size()) * trackHeight - scrollY;
    const int row = rowOfTrack[(size_t) index];
    return row >= 0 ? row * trackHeight - scrollY : -1000000;
}

void ArrangeView::updateRows()
{
    auto& p = project();
    rowOfTrack.assign ((size_t) p.numTracks(), -1);
    trackAtRow.clear();
    for (int i = 0; i < p.numTracks(); ++i)
        if (! daw::stacks::isHidden (p, p.track (i)))
        {
            rowOfTrack[(size_t) i] = (int) trackAtRow.size();
            trackAtRow.push_back (i);
        }
}

juce::int64 ArrangeView::snap (double sample, bool enabled) const
{
    if (! enabled)
        return (juce::int64) std::llround (juce::jmax (0.0, sample));

    const auto& grid = project().grid();
    // Snap to beats; when zoomed far out, to the visible grid, up to a bar. In musical time, so
    // it follows tempo and meter changes.
    const double tick = grid.tickAt (juce::jmax (0.0, sample));
    const auto meter = grid.meterAt (tick);
    const double unit = juce::jlimit (1.0, (double) meter.beatsPerBar, gridStepBeats()) * meter.ticksPerBeat();
    const double snapped = meter.tick + std::round ((tick - meter.tick) / unit) * unit;
    return (juce::int64) std::llround (juce::jmax (0.0, grid.sampleAt (snapped)));
}

std::vector<ArrangeView::GridLine> ArrangeView::gridLines (double fromSample, double toSample, double stepBeats) const
{
    std::vector<GridLine> lines;
    const auto& grid = project().grid();
    const int stepBars = stepBeats >= (double) juce::jmax (1, grid.beatsPerBar) ? juce::jmax (1, juce::roundToInt (stepBeats / juce::jmax (1, grid.beatsPerBar))) : 0;
    const int firstBar = juce::jmax (0, (int) std::floor (grid.barAt (grid.tickAt (juce::jmax (0.0, fromSample)))));
    const int lastBar = (int) std::ceil (grid.barAt (grid.tickAt (juce::jmax (0.0, toSample)))) + 1;

    for (int bar = firstBar; bar <= lastBar && lines.size() < 20000; ++bar)
    {
        const double barTick = grid.barStartTick (bar);
        if (stepBars > 0)
        {
            if (bar % stepBars == 0)
                lines.push_back ({ grid.sampleAt (barTick), true, true, bar + 1, 1 });
            continue;
        }
        const auto meter = grid.meterAt (barTick);
        const double subdivisions = std::round ((double) meter.beatsPerBar / stepBeats);
        for (int k = 0; k < (int) subdivisions; ++k)
        {
            const double beat = k * stepBeats;
            const bool isBeat = std::abs (beat - std::round (beat)) < 1.0e-6;
            lines.push_back ({ grid.sampleAt (barTick + beat * meter.ticksPerBeat()), k == 0, isBeat, bar + 1, (int) std::floor (beat + 1.0e-6) + 1 });
        }
    }
    return lines;
}

double ArrangeView::gridStepBeats() const
{
    const auto grid = project().grid();
    const double pixelsPerBeat = grid.samplesPerBeat() / samplesPerPixel;
    const double bar = juce::jmax (1, grid.beatsPerBar);

    for (double step : { 0.25, 0.5, 1.0 })
        if (step * pixelsPerBeat >= 12.0)
            return step;
    for (double bars = 1.0; bars <= 512.0; bars *= 2.0)
        if (bars * bar * pixelsPerBeat >= 12.0)
            return bars * bar;
    return 1024.0 * bar;
}

double ArrangeView::labelStepBeats() const
{
    const auto grid = project().grid();
    const double pixelsPerBeat = grid.samplesPerBeat() / samplesPerPixel;
    const double bar = juce::jmax (1, grid.beatsPerBar);

    if (pixelsPerBeat >= 52.0)
        return 1.0;
    for (double bars = 1.0; bars <= 512.0; bars *= 2.0)
        if (bars * bar * pixelsPerBeat >= 36.0)
            return bars * bar;
    return 1024.0 * bar;
}

//==============================================================================
juce::int64 ArrangeView::timelineLength() const
{
    auto& p = project();
    const auto grid = p.grid();
    auto end = juce::jmax (p.contentEnd(), (juce::int64) p.state[ids::loopEnd], studio.engine().getPosition(),
                           (juce::int64) (grid.samplesPerBar() * 16.0));
    if (studio.engine().isRecording())
        end = juce::jmax (end, studio.engine().getRecordStart() + studio.engine().getRecordedLength());
    return end + (juce::int64) (grid.samplesPerBar() * 8.0);
}

void ArrangeView::updateScrollBars()
{
    const double total = juce::jmax ((double) timelineLength(), viewStart + getViewLength());
    hScroll.setRangeLimits (0.0, total, juce::dontSendNotification);
    hScroll.setCurrentRange (viewStart, getViewLength(), juce::dontSendNotification);

    const int contentHeight = visibleRows() * trackHeight + trackHeight / 2;
    vScroll.setRangeLimits (0.0, (double) juce::jmax (contentHeight, lanesHeight()), juce::dontSendNotification);
    vScroll.setCurrentRange ((double) scrollY, (double) lanesHeight(), juce::dontSendNotification);
}

void ArrangeView::setViewStart (double start)
{
    start = juce::jmax (0.0, start);
    if (juce::exactlyEqual (start, viewStart))
        return;
    viewStart = start;
    updateScrollBars();
    ruler->repaint();
    lanes->repaint();
}

void ArrangeView::setSamplesPerPixel (double spp)
{
    const double rate = project().grid().sampleRate;
    samplesPerPixel = juce::jlimit (2.0, rate * 3600.0 / juce::jmax (100, lanesWidth()), spp);
}

void ArrangeView::scrollBarMoved (juce::ScrollBar* bar, double newRangeStart)
{
    if (bar == &hScroll)
    {
        keepFitted = false;
        setViewStart (newRangeStart);
    }
    else
    {
        scrollY = juce::roundToInt (newRangeStart);
        layoutHeaders();
        lanes->repaint();
    }
}

void ArrangeView::scrollPixels (double dx, double dy)
{
    if (! juce::exactlyEqual (dx, 0.0))
    {
        keepFitted = false;
        setViewStart (viewStart + dx * samplesPerPixel);
    }

    if (! juce::exactlyEqual (dy, 0.0))
    {
        const int contentHeight = visibleRows() * trackHeight + trackHeight / 2;
        const int newY = juce::jlimit (0, juce::jmax (0, contentHeight - lanesHeight()), scrollY + juce::roundToInt (dy));
        if (newY != scrollY)
        {
            scrollY = newY;
            layoutHeaders();
            updateScrollBars();
            lanes->repaint();
        }
    }
}

void ArrangeView::zoomAround (float x, double factor)
{
    if (factor <= 0.0)
        return;
    keepFitted = false;
    const double anchor = sampleAtX (x);
    setSamplesPerPixel (samplesPerPixel / factor);
    viewStart = juce::jmax (0.0, anchor - x * samplesPerPixel);
    updateScrollBars();
    ruler->repaint();
    lanes->repaint();
}

void ArrangeView::zoomBy (double factor)
{
    const float px = xForSample ((double) studio.engine().getPosition());
    zoomAround (px >= 0.0f && px <= (float) lanesWidth() ? px : 0.0f, factor);
}

void ArrangeView::zoomToFit()
{
    const int width = lanesWidth();
    if (width <= 0)
    {
        fitPending = true;
        return;
    }
    fitPending = false;
    keepFitted = true;

    auto& p = project();
    auto end = p.contentEnd();
    if (end <= 0)
        end = (juce::int64) (p.grid().samplesPerBar() * 16.0);

    setSamplesPerPixel ((double) end * 1.04 / width);
    viewStart = 0.0;
    updateScrollBars();
    ruler->repaint();
    lanes->repaint();
}

void ArrangeView::handleWheel (const juce::MouseEvent& e, const juce::MouseWheelDetails& w, float timelineX)
{
    if (e.mods.isCommandDown())
    {
        const float delta = std::abs (w.deltaY) >= std::abs (w.deltaX) ? w.deltaY : w.deltaX;
        zoomAround (timelineX, std::pow (2.0, (double) delta * 2.5));
        return;
    }

    double dx = w.deltaX, dy = w.deltaY;
    if (e.mods.isShiftDown() && juce::exactlyEqual (dx, 0.0))
        std::swap (dx, dy);

    const int contentHeight = visibleRows() * trackHeight + trackHeight / 2;
    if (contentHeight <= lanesHeight() && juce::exactlyEqual (dx, 0.0))
        std::swap (dx, dy);          // nothing to scroll vertically: the wheel scrolls in time

    constexpr double pixelsPerUnit = 260.0;
    scrollPixels (-dx * pixelsPerUnit, -dy * pixelsPerUnit);
}

//==============================================================================
void ArrangeView::timerCallback()
{
    auto& engine = studio.engine();
    const auto position = engine.getPosition();

    // Keep the playhead in view while playing, unless the user is busy with the mouse.
    if (engine.isPlaying() && ! userGesture && ! lanes->isDragging() && ! hScroll.isMouseButtonDown())
    {
        const double length = getViewLength();
        if ((double) position < viewStart || (double) position > viewStart + length * 0.97)
        {
            keepFitted = false;
            setViewStart ((double) position - length * 0.03);
        }
    }

    const bool recordingNow = engine.isRecording();
    if (recordingNow != wasRecording)
    {
        wasRecording = recordingNow;
        lastRecordedLength = -1;
        ruler->repaint();
        lanes->repaint();
    }

    const float px = std::round (xForSample ((double) position));
    if (! juce::exactlyEqual (px, lastPlayheadX))
    {
        ruler->playheadMoved (lastPlayheadX, px);
        lanes->playheadMoved (lastPlayheadX, px);
        lastPlayheadX = px;
    }

    if (recordingNow)
    {
        // The whole take: its live waveform rescales as the playing gets louder.
        const auto start = engine.getRecordStart() - engine.getLiveTakeOffset();
        const auto length = engine.getRecordedLength();
        if (length != lastRecordedLength)
        {
            const float x1 = xForSample ((double) start);
            const float x2 = xForSample ((double) (start + length));
            lanes->repaint (juce::Rectangle<int> ((int) x1 - 8, 0, (int) (x2 - x1) + 16, lanes->getHeight()));
            lastRecordedLength = length;
        }
    }
}

//==============================================================================
void ArrangeView::dragTrackTo (const juce::ValueTree& track, int yInView)
{
    auto& p = project();
    const int current = p.state.indexOf (track);
    if (current < 0)
        return;

    const int y = yInView - lanes->getY() + scrollY;
    const int row = juce::jlimit (0, juce::jmax (0, visibleRows() - 1), y / trackHeight);
    const int target = row < visibleRows() ? trackAtRow[(size_t) row] : p.numTracks() - 1;
    if (target == current)
        return;

    if (! reorderStarted)
    {
        p.undo.beginNewTransaction ("Move Track");
        reorderStarted = true;
    }
    p.state.moveChild (current, target, &p.undo);
}

void ArrangeView::addTrackAfterSelection()
{
    studio.showNewTrackMenu();
}

void ArrangeView::showTrackMenu (juce::ValueTree track)
{
    const int trackId = (int) track[ids::id];
    juce::Component::SafePointer<ArrangeView> safe (this);

    auto withTrack = [safe, trackId] (std::function<void (ArrangeView&, juce::ValueTree)> fn)
    {
        return [safe, trackId, fn]
        {
            if (safe == nullptr)
                return;
            if (auto t = safe->project().trackById (trackId); t.isValid())
                fn (*safe, t);
        };
    };

    juce::PopupMenu colours;
    static const char* const swatchNames[] { "Amber", "Blue", "Green", "Rose", "Violet", "Gold", "Teal", "Coral", "Stone", "Indigo" };
    const auto current = arrange::trackColour (track);
    const auto& swatches = arrange::trackSwatches();
    for (int i = 0; i < swatches.size(); ++i)
    {
        const auto c = swatches[i];
        juce::PopupMenu::Item item (i < (int) std::size (swatchNames) ? swatchNames[i] : "Colour");
        auto dot = std::make_unique<juce::DrawablePath>();
        juce::Path circle;
        circle.addEllipse (0.0f, 0.0f, 14.0f, 14.0f);
        dot->setPath (circle);
        dot->setFill (c);
        item.image = std::move (dot);
        item.isTicked = current.getARGB() == c.getARGB();
        item.action = withTrack ([c] (ArrangeView& v, juce::ValueTree t)
        {
            auto& p = v.project();
            p.undo.beginNewTransaction ("Track Colour");
            t.setProperty (ids::colour, (juce::int64) c.getARGB(), &p.undo);
        });
        colours.addItem (std::move (item));
    }

    juce::PopupMenu m;
    m.addSectionHeader (track[ids::name].toString());
    m.addItem ("Rename", withTrack ([] (ArrangeView& v, juce::ValueTree t)
    {
        for (auto& h : v.headers)
            if (h->getTrack() == t)
                h->startRename();
    }));
    m.addSubMenu ("Colour", colours);

    // Which interface input the track records and monitors.
    juce::PopupMenu inputs;
    int numInputs = 2;
    if (auto* device = studio.deviceManager.getCurrentAudioDevice())
        numInputs = juce::jmax (numInputs, device->getActiveInputChannels().countNumberOfSetBits());
    for (int i = 0; i < numInputs; ++i)
        inputs.addItem ("Input " + juce::String (i + 1), true, (int) track.getProperty (ids::input, 0) == i,
                        withTrack ([i] (ArrangeView& v, juce::ValueTree t)
                        {
                            auto& p = v.project();
                            p.undo.beginNewTransaction ("Track Input");
                            t.setProperty (ids::input, i, &p.undo);
                        }));
    m.addSubMenu ("Input", inputs);
    m.addSeparator();
    m.addItem ("Duplicate", withTrack ([] (ArrangeView& v, juce::ValueTree t)
    {
        v.studio.selectTrack ((int) t[ids::id]);
        v.studio.duplicateSelectedTrack();
    }));
    m.addItem ("Delete", withTrack ([] (ArrangeView& v, juce::ValueTree t)
    {
        v.studio.selectTrack ((int) t[ids::id]);
        v.studio.deleteSelectedTrack();
    }));
    m.addSeparator();
    m.addItem ("New Track", withTrack ([] (ArrangeView& v, juce::ValueTree t)
    {
        v.studio.selectTrack ((int) t[ids::id]);
        v.studio.showNewTrackMenu();
    }));
    mixui::addTrackMenuItems (m, studio, track);       // stacks, groups, VCA, freeze

    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void ArrangeView::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-click in the empty space under the track headers: a new track.
    if (e.x < headerWidth && e.y > rulerHeight)
        addTrackAfterSelection();
}

void ArrangeView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    handleWheel (e, w, 0.0f);
}

} // namespace woodshed::app
