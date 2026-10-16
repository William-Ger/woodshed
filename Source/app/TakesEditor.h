#pragma once

#include "Studio.h"
#include "Theme.h"
#include "ArrangeWaveforms.h"

namespace woodshed::app
{

/**
    Comping a take folder (Logic's quick swipe comping): every take of the folder in its own lane,
    the part of each that's in the comp lit up. Drag across a take to put that stretch of it in
    the comp; click a take's name to use all of it. Flatten turns the comp into plain regions.
*/
class TakesEditor final : public juce::Component,
                          private juce::ChangeListener,
                          private juce::ValueTree::Listener,
                          private Studio::ProjectSwapListener,
                          private juce::Timer
{
public:
    explicit TakesEditor (Studio&);
    ~TakesEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void projectAboutToBeReplaced() override;
    void timerCallback() override;

    void sync();
    juce::Rectangle<int> laneArea (int take) const;
    juce::Rectangle<int> lanesBounds() const;
    float xFor (juce::int64 fromOrigin) const;
    juce::int64 atX (float x) const;
    juce::int64 snapped (juce::int64 fromOrigin, bool enabled) const;

    Studio& studio;
    juce::ValueTree watched, folder;
    arrange::WaveformCache waveforms;
    juce::TextButton flattenButton { "Flatten" };

    int swipeTake = -1;
    juce::int64 swipeFrom = 0, swipeTo = 0;
    bool swiping = false;
    float lastPlayheadX = -1.0f;

    static constexpr int headerHeight = 36, nameWidth = 96, laneHeight = 56;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TakesEditor)
};

} // namespace woodshed::app
