#pragma once

#include "ArrangeView.h"
#include "ControlBar.h"
#include "MixerView.h"
#include "RigPanel.h"
#include "InstrumentPanel.h"
#include "PianoRoll.h"
#include "ScoreView.h"
#include "DrummerEditor.h"
#include "StepSequencer.h"
#include "MusicalTyping.h"
#include "TakesEditor.h"
#include "LoopGridView.h"
#include "SamplerEditor.h"

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
    InstrumentPanel instrument;
    PianoRoll pianoRoll;
    ScoreView scoreView;
    DrummerEditor drummer;
    StepSequencer stepSequencer;
    MusicalTyping typing;
    TakesEditor takesEditor;
    LoopGridView loopGridView;
    SamplerEditor samplerEditor;

    /** The component for the current pane: the rig, or an instrument track's sound in its place. */
    juce::Component* paneComponent();
    void showPaneComponents();
    bool shownInstrument = false;
    juce::TooltipWindow tooltips { this, 600 };

    juce::StretchableLayoutManager layout;
    std::unique_ptr<juce::StretchableLayoutResizerBar> divider;
    Studio::Pane shownPane = Studio::Pane::none;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace woodshed::app
