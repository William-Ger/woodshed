#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

/** The selected track's guitar rig (the pedalboard, amp and cab), embedded below the tracks. */
class RigPanel final : public juce::Component,
                    private juce::ChangeListener
{
public:
    explicit RigPanel (Studio&);
    ~RigPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Opens the chromatic tuner over the rig. */
    void showTuner();

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    Studio& studio;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RigPanel)
};

} // namespace woodshed::app
