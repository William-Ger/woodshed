#pragma once

#include "Studio.h"
#include "Theme.h"
#include "Icons.h"

class WoodshedEditor;

namespace woodshed::app
{

/**
    The selected track's guitar rig (the pedalboard, amp and cab), embedded below the tracks.

    One 44 px pane header carries everything about the rig: its power switch, the track's colour
    and name, the track's input, the rig's input gain and level, the preset browser, Save, the
    tuner and Add Pedal (the last few are the editor's own controls, adopted into the header so
    they stay full size). Below it sits the rig's editor in studio mode, scaled down when the
    pane is shorter than the rig needs. The editor is swapped whenever the selected track, its
    rig or the project changes.
*/
class RigPanel final : public juce::Component,
                       private juce::ChangeListener,
                       private juce::ValueTree::Listener,
                       private juce::ApplicationCommandManagerListener,
                       private Studio::ProjectSwapListener,
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
    void projectAboutToBeReplaced() override;
    void timerCallback() override;

    /** Brings the shown editor in line with the selection, the track's rig switch and the engine. */
    void sync();
    void dropEditor();
    void adoptEditorControls();
    WoodshedEditor* rigEditor() const;
    void refreshInputChoice();
    void layOutEditor();
    void setRigOn (bool);
    bool waitingForProjectSwap();

    Studio& studio;
    juce::ValueTree watchedState;              // the project root we listen to
    juce::ValueTree shownTrack;
    int shownTrackId = -1;
    juce::AudioProcessor* shownRig = nullptr;
    std::unique_ptr<juce::AudioProcessorEditor> editor;

    /** A borderless header button that shows an icon (power, tuner). */
    class IconButton final : public juce::Button
    {
    public:
        IconButton (const juce::String& name, icons::Icon i) : juce::Button (name), icon (std::move (i)) {}
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        bool lit = false;             // power: the rig is on
    private:
        icons::Icon icon;
    };

    IconButton powerButton { "Rig power", icons::power() };
    IconButton tunerButton { "Tuner", icons::tuningFork() };
    juce::ComboBox inputChoice;
    juce::TextButton turnOnButton { "Turn the rig on" };
    juce::Rectangle<int> nameArea, gainLabelArea;

    // Opening or starting a project frees every rig; hold off on new editors until it lands.
    bool projectSwapPending = false;
    juce::uint32 lastModalTime = 0;

    static constexpr int headerHeight = 44;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RigPanel)
};

} // namespace woodshed::app
