#pragma once

#include "ArrangeView.h"
#include "ControlBar.h"
#include "MixerView.h"
#include "RigPanel.h"

namespace woodshed::app
{

/** The studio window's content: control bar, tracks, and the rig or mixer pane below. */
class MainComponent final : public juce::Component,
                            public juce::ApplicationCommandTarget,
                            public juce::MenuBarModel,
                            private juce::ChangeListener
{
public:
    explicit MainComponent (Studio&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override     { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override;
    void menuItemSelected (int, int) override {}

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    Studio& studio;
    ControlBar controlBar;
    ArrangeView arrange;
    MixerView mixer;
    RigPanel rig;

    juce::StretchableLayoutManager layout;
    std::unique_ptr<juce::StretchableLayoutResizerBar> divider;
    Studio::Pane shownPane = Studio::Pane::none;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace woodshed::app
