#include "ArrangeView.h"
#include "ArrangeHeader.h"
#include "ArrangeLanes.h"
#include "ArrangeRuler.h"

namespace woodshed::app
{

namespace ids = daw::ids;

ArrangeView::ArrangeView (Studio& s) : studio (s)
{
    // Themed menus, scrollbars, buttons and the rename editor.
    lnf.setColour (juce::PopupMenu::backgroundColourId, theme::panel);
    lnf.setColour (juce::PopupMenu::textColourId, theme::text);
    lnf.setColour (juce::PopupMenu::headerTextColourId, theme::dimText);
    lnf.setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::accent);
    lnf.setColour (juce::PopupMenu::highlightedTextColourId, juce::Colour (0xff15130f));
    lnf.setColour (juce::ScrollBar::thumbColourId, theme::panelLight.brighter (0.25f));
    lnf.setColour (juce::ScrollBar::trackColourId, theme::bar);
    lnf.setColour (juce::TextButton::buttonColourId, theme::panelLight);
    lnf.setColour (juce::TextButton::textColourOffId, theme::text);
    lnf.setColour (juce::ComboBox::outlineColourId, theme::outline);
    lnf.setColour (juce::TextEditor::backgroundColourId, theme::window);
    lnf.setColour (juce::TextEditor::textColourId, theme::text);
    lnf.setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.35f));
    lnf.setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
    setLookAndFeel (&lnf);

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
    studio.removeChangeListener (this);
    waveforms.removeChangeListener (this);
    attached.removeListener (this);
    headers.clear();
    setLookAndFeel (nullptr);
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
    resized();
    repaint();
}

void ArrangeView::layoutHeaders()
{
    for (size_t i = 0; i < headers.size(); ++i)
        headers[i]->setBounds (0, yForTrack ((int) i), headerArea.getWidth(), trackHeight);
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

    // Corner above the headers.
    auto corner = getLocalBounds().removeFromLeft (headerWidth).removeFromTop (rulerHeight);
    g.setColour (theme::bar);
    g.fillRect (corner);
    g.setColour (theme::dimText);
    g.setFont (theme::font (11.0f, true));
    const int n = project().numTracks();
    g.drawText (n == 1 ? "1 TRACK" : juce::String (n) + " TRACKS", corner.withTrimmedLeft (14).withTrimmedRight (40),
                juce::Justification::centredLeft);

    g.setColour (theme::outline);
    g.fillRect (corner.getX(), corner.getBottom() - 1, corner.getWidth(), 1);
    g.fillRect (headerWidth - 1, 0, 1, getHeight());

    // Bottom-left, beside the horizontal scrollbar.
    g.setColour (theme::bar);
    g.fillRect (0, getHeight() - scrollBarSize, headerWidth, scrollBarSize);
}

void ArrangeView::resized()
{
    auto r = getLocalBounds();
    auto left = r.removeFromLeft (headerWidth);
    auto corner = left.removeFromTop (rulerHeight);
    addButton.setBounds (corner.removeFromRight (36).withSizeKeepingCentre (24, 22));
    headerArea.setBounds (left.withTrimmedBottom (scrollBarSize).withTrimmedRight (1));

    auto top = r.removeFromTop (rulerHeight);
    hScroll.setBounds (r.removeFromBottom (scrollBarSize));

    const int contentHeight = project().numTracks() * trackHeight + trackHeight / 2;
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
    const int index = (y + scrollY) / trackHeight;
    return index < project().numTracks() ? index : -1;
}

juce::int64 ArrangeView::snap (double sample, bool enabled) const
{
    if (! enabled)
        return (juce::int64) std::llround (juce::jmax (0.0, sample));

    const auto grid = project().grid();
    // Snap to beats; when zoomed far out, to the visible grid, up to a bar.
    const double step = juce::jlimit (1.0, (double) juce::jmax (1, grid.beatsPerBar), gridStepBeats()) * grid.samplesPerBeat();
    return (juce::int64) std::llround (juce::jmax (0.0, std::round (sample / step) * step));
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

    const int contentHeight = project().numTracks() * trackHeight + trackHeight / 2;
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
        const int contentHeight = project().numTracks() * trackHeight + trackHeight / 2;
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

    const int contentHeight = project().numTracks() * trackHeight + trackHeight / 2;
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
        const auto start = engine.getRecordStart();
        const auto length = engine.getRecordedLength();
        if (length != lastRecordedLength)
        {
            const float x1 = xForSample ((double) (start + juce::jmax ((juce::int64) 0, lastRecordedLength)));
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
    const int target = juce::jlimit (0, p.numTracks() - 1, y / trackHeight);
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
    studio.addTrack();
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
    m.setLookAndFeel (&lnf);
    m.addSectionHeader (track[ids::name].toString());
    m.addItem ("Rename", withTrack ([] (ArrangeView& v, juce::ValueTree t)
    {
        for (auto& h : v.headers)
            if (h->getTrack() == t)
                h->startRename();
    }));
    m.addSubMenu ("Colour", colours);
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
        v.studio.addTrack();
    }));

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
