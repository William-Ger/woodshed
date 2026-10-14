#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

/** The strip across the top: transport, the position readout, tempo and meter, loop/click/count-in, master level, and the pane switches. */
class ControlBar final : public juce::Component,
                    private juce::ChangeListener
{
public:
    explicit ControlBar (Studio&);
    ~ControlBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    Studio& studio;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlBar)
};

} // namespace woodshed::app
