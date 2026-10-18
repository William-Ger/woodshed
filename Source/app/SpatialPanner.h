#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

/**
    The 3D panner for one track: the room from above with the listener in the middle (front at
    the top) and the track as a puck you drag around it (direction and distance), an elevation
    slider beside it, and spread and room below. Turning it on replaces the track's pan; the
    project's Stereo / Binaural choice says how it reaches the master. Edits go through undo;
    while automation moves the track, a ring shows where it is now.
*/
class SpatialPanner final : public juce::Component,
                            private juce::ValueTree::Listener,
                            private juce::Timer
{
public:
    SpatialPanner (Studio&, juce::ValueTree track);
    ~SpatialPanner() override;

    /** Opens the panner for `track` in a popover pointing at `target` (screen coordinates). */
    static void show (Studio&, juce::ValueTree track, juce::Rectangle<int> target);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    /** Distance (metres) <-> radius on the top view (0..1), on a log scale. */
    static float radiusFor (float metres);
    static float metresAt (float radius);

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void timerCallback() override;
    void syncControls();
    void setFromPoint (juce::Point<float>, bool keepDistance);
    void set (const juce::Identifier&, double value);
    juce::Rectangle<float> circleArea() const;
    juce::Point<float> pointFor (float azimuth, float metres) const;

    Studio& studio;
    juce::ValueTree track, projectState;
    juce::ToggleButton enabled { "3D position (instead of the pan)" }, binaural { "Binaural (headphones)" };
    juce::Slider elevation, spread, room;
    juce::Label elevationLabel { {}, "Elev." }, spreadLabel { {}, "Spread" }, roomLabel { {}, "Room" };
    juce::Point<float> live { -1.0f, -1.0f };      // where automation has it now, if it's automated

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpatialPanner)
};

} // namespace woodshed::app
