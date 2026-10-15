#include "MixerView.h"

namespace woodshed::app
{

namespace ids = daw::ids;

namespace { constexpr int gutter = 4; }    // between the track strips and the master's bay

MixerView::MixerView (Studio& s) : studio (s)
{
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (false, true, false, true);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    studio.addChangeListener (this);
    attach();
    startTimerHz (60);
}

MixerView::~MixerView()
{
    stopTimer();
    observed.removeListener (this);
    studio.removeChangeListener (this);
}

//==============================================================================
void MixerView::attach()
{
    if (observed == studio.project().state)
        return;

    observed.removeListener (this);
    observed = studio.project().state;
    observed.addListener (this);
    rebuild();
}

void MixerView::rebuild()
{
    const auto scroll = viewport.getViewPositionX();
    strips.clear();

    for (int i = 0; i < studio.project().numTracks(); ++i)
    {
        auto t = studio.project().track (i);
        if (t.hasType (ids::TRACK))
            content.addAndMakeVisible (strips.add (std::make_unique<ChannelStrip> (studio, t, false)));
    }

    master = std::make_unique<ChannelStrip> (studio, observed, true);
    addAndMakeVisible (*master);

    resized();
    viewport.setViewPosition (scroll, 0);
    shownSelection = -2;
    updateSolo();
    updateSelection();
    repaint();
}

void MixerView::updateSelection()
{
    const int id = studio.getSelectedTrackId();
    for (auto* s : strips)
        s->setSelected (s->getTrackId() == id);

    if (id == shownSelection || viewport.getWidth() <= 0)
        return;
    shownSelection = id;

    // Bring a newly selected strip into view.
    for (auto* s : strips)
        if (s->getTrackId() == id)
        {
            const auto shown = viewport.getViewArea();
            if (s->getX() < shown.getX())
                viewport.setViewPosition (s->getX(), 0);
            else if (s->getRight() > shown.getRight())
                viewport.setViewPosition (s->getRight() - shown.getWidth(), 0);
        }
}

void MixerView::updateSolo()
{
    bool anySolo = false;
    for (auto* s : strips)
        anySolo = anySolo || (bool) s->getTree()[ids::solo];
    for (auto* s : strips)
        s->setAnySolo (anySolo);
}

//==============================================================================
void MixerView::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    if (strips.isEmpty())
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (13.0f));
        g.drawText ("No tracks yet. Add one from the Track menu.", viewport.getBounds(), juce::Justification::centred, false);
    }

    // The master sits in its own bay: a hairline and a narrow gap of window colour.
    if (master != nullptr)
    {
        g.setColour (theme::separator);
        g.fillRect (master->getX() - gutter, 0, 1, getHeight());
    }

    g.setColour (theme::separator);
    g.fillRect (0, 0, getWidth(), 1);
}

void MixerView::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (1);
    if (master != nullptr)
        master->setBounds (r.removeFromRight (ChannelStrip::masterWidth));
    r.removeFromRight (gutter);
    viewport.setBounds (r);
    layoutStrips();
}

void MixerView::layoutStrips()
{
    const int width = strips.size() * ChannelStrip::trackWidth;
    const bool scrolls = width > viewport.getWidth();
    const int height = viewport.getHeight() - (scrolls ? viewport.getScrollBarThickness() : 0);
    content.setSize (width, juce::jmax (0, height));

    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (i * ChannelStrip::trackWidth, 0, ChannelStrip::trackWidth, content.getHeight());

    // Keep the master's controls level with the tracks' when a scrollbar takes the bottom.
    if (master != nullptr && scrolls)
        master->setBounds (master->getBounds().withTrimmedBottom (viewport.getScrollBarThickness()));
}

//==============================================================================
void MixerView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    attach();
    updateSelection();
}

void MixerView::timerCallback()
{
    const bool showing = isShowing();
    const double now = juce::Time::getMillisecondCounterHiRes();

    if (! showing)
    {
        wasShowing = false;
        return;
    }

    // Peaks are "since the last call"; whatever piled up while hidden is stale.
    if (! wasShowing)
    {
        wasShowing = true;
        lastTick = now;
        for (auto* s : strips)
            s->discardMeter();
        if (master != nullptr)
            master->discardMeter();
        return;
    }

    const auto dt = (float) juce::jlimit (0.0, 0.25, (now - lastTick) * 0.001);
    lastTick = now;
    for (auto* s : strips)
        s->pollMeter (dt);
    if (master != nullptr)
        master->pollMeter (dt);
}

//==============================================================================
void MixerView::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree == observed)
    {
        if (property == ids::selectedTrack)
            updateSelection();
        else if (property == ids::masterVolume && master != nullptr)
            master->refresh();
        return;
    }

    if (! tree.hasType (ids::TRACK))
        return;

    for (auto* s : strips)
        if (s->getTree() == tree)
            s->refresh();

    if (property == ids::solo)
        updateSolo();
}

void MixerView::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent == observed)
        rebuild();
}

void MixerView::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent == observed)
        rebuild();
}

void MixerView::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent == observed)
        rebuild();
}

void MixerView::valueTreeRedirected (juce::ValueTree&)
{
    rebuild();
}

} // namespace woodshed::app
