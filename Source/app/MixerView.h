#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

/** One channel strip per track plus the master: fader, pan, meters, mute/solo/arm/monitor, input and rig on/off. */
class MixerView final : public juce::Component,
                    private juce::ChangeListener
{
public:
    explicit MixerView (Studio&);
    ~MixerView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    Studio& studio;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerView)
};

} // namespace woodshed::app
