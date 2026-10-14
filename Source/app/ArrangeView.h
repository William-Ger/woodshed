#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

/** Tracks down the left, the timeline to the right: ruler, regions with waveforms, playhead, loop range. Editing (select, move, trim, split, delete) goes through the project's undo manager. */
class ArrangeView final : public juce::Component,
                    private juce::ChangeListener
{
public:
    explicit ArrangeView (Studio&);
    ~ArrangeView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Horizontal zoom, keeping the playhead where it is on screen. */
    void zoomBy (double factor);
    /** Fits the whole song (or 16 bars, if it's empty) in the window. */
    void zoomToFit();

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    Studio& studio;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrangeView)
};

} // namespace woodshed::app
