#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

/**
    The selected track's guitar rig (the pedalboard, amp and cab), embedded below the tracks.

    A slim header names the track and switches its rig on or off; below it sits the rig's own
    editor in studio mode, scaled down when the pane is shorter than the rig needs. The editor
    is swapped whenever the selected track, its rig or the project changes.
*/
class RigPanel final : public juce::Component,
                       private juce::ChangeListener,
                       private juce::ValueTree::Listener,
                       private juce::ApplicationCommandManagerListener,
                       private juce::Timer
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
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void applicationCommandInvoked (const juce::ApplicationCommandTarget::InvocationInfo&) override;
    void applicationCommandListChanged() override {}
    void timerCallback() override;

    /** Brings the shown editor in line with the selection, the track's rig switch and the engine. */
    void sync();
    void dropEditor();
    void layOutEditor();
    void setRigOn (bool);
    bool waitingForProjectSwap();

    Studio& studio;
    juce::ValueTree watchedState;              // the project root we listen to
    juce::ValueTree shownTrack;
    int shownTrackId = -1;
    juce::AudioProcessor* shownRig = nullptr;
    std::unique_ptr<juce::AudioProcessorEditor> editor;

    juce::ToggleButton rigSwitch { "Rig on" };
    juce::TextButton turnOnButton { "Turn the rig on" };

    // Opening or starting a project frees every rig; hold off on new editors until it lands.
    bool projectSwapPending = false;
    juce::uint32 lastModalTime = 0;

    static constexpr int headerHeight = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RigPanel)
};

} // namespace woodshed::app
