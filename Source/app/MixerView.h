#pragma once

#include "ChannelStrip.h"

namespace woodshed::app
{

/** One channel strip per track plus the master: fader, pan, meters, mute/solo/arm/monitor, input and rig on/off. */
class MixerView final : public juce::Component,
                        private juce::ChangeListener,
                        private juce::ValueTree::Listener,
                        private juce::Timer
{
public:
    explicit MixerView (Studio&);
    ~MixerView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void valueTreeRedirected (juce::ValueTree&) override;

    /** Follows the studio's current project: re-attaches the listener and rebuilds if it changed. */
    void attach();
    void rebuild();
    void layoutStrips();
    void updateSelection();
    void updateSolo();

    Studio& studio;
    juce::ValueTree observed;

    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<ChannelStrip> strips;
    std::unique_ptr<ChannelStrip> master;

    int shownSelection = -1;
    bool wasShowing = false;
    double lastTick = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerView)
};

} // namespace woodshed::app
