#pragma once

#include "Studio.h"
#include "../flex/Flex.h"

namespace woodshed::app
{
class ArrangeView;
}

namespace woodshed::app::arrange
{

/**
    Time Stretch in the arrange lanes. On a track with Time Stretch on, each audio region shows
    its transients as faint ticks and its flex markers as lines with a handle. In the upper half
    of the region's waveform: click to put a marker on the nearest transient (or where you click),
    drag a marker to move it (the audio between its neighbours stretches to follow), Alt-click a
    marker to take it out. A stretched region draws the audio it actually plays.
*/
class FlexLane
{
public:
    explicit FlexLane (ArrangeView&);

    /** Draws the waveform of a region that plays a render; false if it doesn't (draw the file). */
    bool drawWave (juce::Graphics&, const juce::ValueTree& region, juce::Rectangle<float> rect, juce::Rectangle<float> wave,
                   juce::Colour, juce::int64 lookStart, juce::int64 lookOffset);
    /** The markers and transients, over a region on a Time Stretch track. */
    void drawMarkers (juce::Graphics&, const juce::ValueTree& region, juce::Rectangle<float> rect, juce::Rectangle<float> wave,
                      juce::int64 lookStart, juce::int64 lookOffset);

    /** True if the press is a flex gesture (and it's been taken). */
    bool mouseDown (const juce::MouseEvent&, const juce::ValueTree& region, juce::Rectangle<float> rect, float nameBarHeight);
    bool isDragging() const                                 { return dragIndex >= 0; }
    void mouseDrag (const juce::MouseEvent&);
    void mouseUp (const juce::MouseEvent&);
    /** The mouse cursor over a region (a marker handle shows a resize cursor). */
    bool wantsCursor (juce::Point<int>, const juce::ValueTree& region, juce::Rectangle<float> rect, float nameBarHeight) const;

    /** How long a region can be at most: where the end of its file plays. */
    juce::int64 playedLength (const juce::ValueTree& region, juce::int64 fileLength) const;

    /** Region menu items for Time Stretch and Pitch Edit. */
    void addMenuItems (juce::PopupMenu&, const juce::ValueTree& region);

private:
    std::vector<double> transientsOf (const juce::ValueTree& region) const;
    int markerAt (const juce::ValueTree& region, float x, juce::int64 start, juce::int64 offset) const;
    float xForRegionTime (double dst, juce::int64 start, juce::int64 offset) const;

    ArrangeView& view;
    juce::ValueTree dragRegion;
    int dragIndex = -1;
    double dragFrom = 0.0, dragDst = 0.0;
    flex::WarpMap dragWarp;
};

} // namespace woodshed::app::arrange
