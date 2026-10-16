#include "TakesEditor.h"
#include "ArrangeHeader.h"

namespace woodshed::app
{

namespace ids = daw::ids;

TakesEditor::TakesEditor (Studio& s) : studio (s)
{
    setOpaque (true);
    flattenButton.setTooltip ("Make the comp plain regions (the takes are kept until you save... and undo brings the folder back)");
    flattenButton.onClick = [this]
    {
        if (! folder.isValid())
            return;
        auto& p = studio.project();
        p.undo.beginNewTransaction ("Flatten");
        studio.setSelectedRegions (p.flattenTakeFolder (folder));
    };
    flattenButton.setWantsKeyboardFocus (false);
    addAndMakeVisible (flattenButton);

    waveforms.addChangeListener (this);
    studio.addChangeListener (this);
    studio.addProjectSwapListener (this);
    sync();
    startTimerHz (30);
}

TakesEditor::~TakesEditor()
{
    watched.removeListener (this);
    studio.removeProjectSwapListener (this);
    studio.removeChangeListener (this);
    waveforms.removeChangeListener (this);
}

void TakesEditor::projectAboutToBeReplaced()
{
    watched.removeListener (this);
    watched = {};
    folder = {};
    waveforms.clear();
}

void TakesEditor::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &waveforms)
    {
        repaint();
        return;
    }
    sync();
}

void TakesEditor::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (tree == folder || tree.getParent() == folder)
        repaint();
}

void TakesEditor::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child == folder)
        sync();
}

void TakesEditor::sync()
{
    if (watched != studio.project().state)
    {
        watched.removeListener (this);
        watched = studio.project().state;
        watched.addListener (this);
    }
    auto next = studio.getTakeFolder();
    if (next.isValid() && ! next.getParent().isValid())
        next = {};
    folder = next;
    flattenButton.setEnabled (folder.isValid());
    repaint();
}

juce::Rectangle<int> TakesEditor::lanesBounds() const
{
    return getLocalBounds().withTrimmedTop (headerHeight).withTrimmedLeft (nameWidth).reduced (8, 6);
}

juce::Rectangle<int> TakesEditor::laneArea (int take) const
{
    const auto b = lanesBounds();
    const int count = juce::jmax (1, daw::Project::takes (folder).size());
    const int h = juce::jlimit (24, laneHeight, b.getHeight() / count);
    return { b.getX(), b.getY() + take * h, b.getWidth(), h - 4 };
}

float TakesEditor::xFor (juce::int64 fromOrigin) const
{
    const auto b = lanesBounds();
    const auto from = (double) (juce::int64) folder[ids::offset];
    const auto length = juce::jmax (1.0, (double) (juce::int64) folder[ids::length]);
    return (float) b.getX() + (float) (((double) fromOrigin - from) / length * b.getWidth());
}

juce::int64 TakesEditor::atX (float x) const
{
    const auto b = lanesBounds();
    const auto from = (double) (juce::int64) folder[ids::offset];
    const auto length = juce::jmax (1.0, (double) (juce::int64) folder[ids::length]);
    return (juce::int64) std::llround (from + (double) (x - (float) b.getX()) / juce::jmax (1, b.getWidth()) * length);
}

juce::int64 TakesEditor::snapped (juce::int64 fromOrigin, bool enabled) const
{
    if (! enabled)
        return fromOrigin;
    // To the nearest 1/16 note of the song.
    const auto grid = studio.project().grid();
    const double step = grid.samplesPerBeat() * grid.beatUnit / 16.0;
    const auto origin = (double) daw::Project::contentOrigin (folder);
    return (juce::int64) std::llround (std::round ((origin + (double) fromOrigin) / step) * step - origin);
}

void TakesEditor::resized()
{
    flattenButton.setBounds (getWidth() - 12 - 90, 6, 90, 24);
}

void TakesEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (theme::bar);
    g.fillRect (header);
    g.setColour (theme::highlight);
    g.fillRect (header.withHeight (1));
    g.setColour (theme::separator);
    g.fillRect (header.removeFromBottom (1));

    auto title = header.reduced (12, 0).withTrimmedRight (110);
    if (! folder.isValid())
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (13.0f, theme::Weight::semibold));
        g.drawText ("Takes", title, juce::Justification::centredLeft);
        g.setFont (theme::font (13.0f));
        g.drawText ("Turn on the cycle (C) and record over it: each pass becomes a take here to comp from.",
                    getLocalBounds().withTrimmedTop (headerHeight), juce::Justification::centred);
        return;
    }

    const auto colour = arrange::trackColour (folder.getParent());
    const auto takeList = daw::Project::takes (folder);
    g.setColour (colour);
    g.fillEllipse (title.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat());
    title.removeFromLeft (8);
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, theme::Weight::semibold));
    g.drawText (folder[ids::name].toString() + "  -  " + juce::String (takeList.size()) + " takes   (drag across a take to comp it)",
                title, juce::Justification::centredLeft, true);

    const auto comp = daw::Project::getComp (folder);
    for (int i = 0; i < takeList.size(); ++i)
    {
        const auto lane = laneArea (i);
        const auto& take = takeList.getReference (i);

        // Name: click to use the whole take.
        auto name = juce::Rectangle<int> (8, lane.getY(), nameWidth - 8, lane.getHeight());
        theme::drawRaised (g, name.toFloat(), theme::radiusChip, theme::control);
        g.setColour (theme::text);
        g.setFont (theme::font (12.0f, theme::Weight::semibold));
        g.drawText (take[ids::name].toString(), name, juce::Justification::centred);

        g.setColour (theme::lane);
        g.fillRect (lane);

        // The take's audio over the folder's range.
        const auto takeFrom = (juce::int64) take[ids::start];
        const auto takeTo = takeFrom + (juce::int64) take[ids::length];
        const float x1 = juce::jmax ((float) lane.getX(), xFor (takeFrom)), x2 = juce::jmin ((float) lane.getRight(), xFor (takeTo));
        const auto file = studio.project().resolve (take[ids::file].toString());
        const auto info = waveforms.info (file);
        if (auto* thumb = info.length > 0 ? waveforms.thumbnail (file) : nullptr; thumb != nullptr && x2 > x1)
        {
            const double rate = info.sampleRate > 0.0 ? info.sampleRate : studio.project().grid().sampleRate;
            const auto fileFrom = (double) (juce::int64) take[ids::offset] + (double) (atX (x1) - takeFrom);
            const auto fileTo = fileFrom + (double) (atX (x2) - atX (x1));
            const auto peak = thumb->getApproximatePeak();
            const auto normalise = peak > 0.0f ? juce::jlimit (1.0f, 8.0f, 0.85f / peak) : 1.0f;
            g.setColour (colour.withAlpha (0.35f));
            thumb->drawChannel (g, juce::Rectangle<float> (x1, (float) lane.getY(), x2 - x1, (float) lane.getHeight()).toNearestInt(),
                                fileFrom / rate, fileTo / rate, 0, normalise);
        }

        // Where this take is in the comp: lit.
        for (auto& c : comp)
        {
            if (c.take != i)
                continue;
            const float c1 = juce::jmax (xFor (c.from), x1), c2 = juce::jmin (xFor (c.to), x2);
            if (c2 <= c1)
                continue;
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (juce::Rectangle<float> (c1, (float) lane.getY(), c2 - c1, (float) lane.getHeight()).toNearestInt());
            g.setColour (colour.withAlpha (0.22f));
            g.fillRect (lane);
            if (auto* thumb = info.length > 0 ? waveforms.thumbnail (file) : nullptr; thumb != nullptr)
            {
                const double rate = info.sampleRate > 0.0 ? info.sampleRate : studio.project().grid().sampleRate;
                const auto fileFrom = (double) (juce::int64) take[ids::offset] + (double) (atX (x1) - takeFrom);
                const auto fileTo = fileFrom + (double) (atX (x2) - atX (x1));
                const auto peak = thumb->getApproximatePeak();
                g.setColour (colour.interpolatedWith (juce::Colours::white, 0.45f));
                thumb->drawChannel (g, juce::Rectangle<float> (x1, (float) lane.getY(), x2 - x1, (float) lane.getHeight()).toNearestInt(),
                                    fileFrom / rate, fileTo / rate, 0, peak > 0.0f ? juce::jlimit (1.0f, 8.0f, 0.85f / peak) : 1.0f);
            }
            g.setColour (colour);
            g.drawRect (juce::Rectangle<float> (c1, (float) lane.getY(), c2 - c1, (float) lane.getHeight()), 1.5f);
        }

        // The swipe in progress.
        if (swiping && swipeTake == i)
        {
            const float s1 = xFor (juce::jmin (swipeFrom, swipeTo)), s2 = xFor (juce::jmax (swipeFrom, swipeTo));
            g.setColour (theme::accent.withAlpha (0.25f));
            g.fillRect (juce::Rectangle<float> (s1, (float) lane.getY(), s2 - s1, (float) lane.getHeight()));
        }
    }

    // Playhead.
    const auto position = studio.engine().getPosition() - daw::Project::contentOrigin (folder);
    const float px = xFor (position);
    const auto b = lanesBounds();
    if (px >= (float) b.getX() && px <= (float) b.getRight())
    {
        g.setColour (theme::accent);
        g.fillRect (juce::Rectangle<float> (px, (float) b.getY(), 1.0f, (float) b.getHeight()));
    }
}

void TakesEditor::timerCallback()
{
    if (! folder.isValid() || ! isShowing())
        return;
    const float px = xFor (studio.engine().getPosition() - daw::Project::contentOrigin (folder));
    if (std::abs (px - lastPlayheadX) >= 0.5f)
    {
        repaint (juce::Rectangle<int> ((int) lastPlayheadX - 2, headerHeight, 5, getHeight()));
        repaint (juce::Rectangle<int> ((int) px - 2, headerHeight, 5, getHeight()));
        lastPlayheadX = px;
    }
}

void TakesEditor::mouseDown (const juce::MouseEvent& e)
{
    swiping = false;
    if (! folder.isValid())
        return;
    const auto takeList = daw::Project::takes (folder);
    for (int i = 0; i < takeList.size(); ++i)
    {
        const auto lane = laneArea (i);
        if (e.y < lane.getY() || e.y >= lane.getBottom())
            continue;
        if (e.x < nameWidth)
        {
            // The whole take.
            auto& p = studio.project();
            p.undo.beginNewTransaction ("Choose Take");
            p.setCompRange (folder, (juce::int64) folder[ids::offset], (juce::int64) folder[ids::offset] + (juce::int64) folder[ids::length], i);
            return;
        }
        swiping = true;
        swipeTake = i;
        swipeFrom = swipeTo = snapped (atX ((float) e.x), ! e.mods.isCommandDown());
        repaint();
        return;
    }
}

void TakesEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! swiping)
        return;
    swipeTo = snapped (atX ((float) e.x), ! e.mods.isCommandDown());
    repaint();
}

void TakesEditor::mouseUp (const juce::MouseEvent&)
{
    if (! swiping)
        return;
    swiping = false;
    const auto from = juce::jmin (swipeFrom, swipeTo), to = juce::jmax (swipeFrom, swipeTo);
    if (to > from && folder.isValid())
    {
        auto& p = studio.project();
        p.undo.beginNewTransaction ("Comp");
        p.setCompRange (folder, from, to, swipeTake);
    }
    repaint();
}

} // namespace woodshed::app
