#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

namespace woodshed
{

class BoardLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    BoardLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
};

//==============================================================================
/** A knob bound to one slot parameter, displayed in the pedal's real units. */
class Knob final : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const KnobSpec&, juce::Colour ink);
    void resized() override;

    juce::Slider slider;

private:
    void updateValueText();

    KnobSpec spec;
    juce::Label caption, valueText;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
};

//==============================================================================
class Meter final : public juce::Component
{
public:
    void setLevel (float linearPeak);
    void paint (juce::Graphics&) override;

private:
    float level = 0.0f, hold = 0.0f;
    int holdFrames = 0;
};

//==============================================================================
class PedalView final : public juce::Component
{
public:
    PedalView (WoodshedProcessor&, int slot, const juce::String& type);

    static int widthFor (const PedalType*);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override       { repaint(); }
    void mouseExit (const juce::MouseEvent&) override        { repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    void toggle()                                            { footswitch.setToggleState (! footswitch.getToggleState(), juce::sendNotification); }
    int getSlot() const                                      { return slot; }

private:
    WoodshedProcessor& processor;
    const int slot;
    const PedalType* type = nullptr;
    juce::String typeId;

    juce::OwnedArray<Knob> knobs;
    juce::ToggleButton footswitch;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> footswitchAttachment;
    juce::TextButton removeButton { "x" };
    bool dragStarted = false;
};

//==============================================================================
/** The scrolling row of pedals; accepts drops to reorder. */
class BoardContent final : public juce::Component,
                           public juce::DragAndDropTarget
{
public:
    explicit BoardContent (WoodshedProcessor&);

    void rebuild();
    void layoutForHeight (int height);
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    juce::OwnedArray<PedalView> pedals;
    std::function<void()> onAddClicked;

private:
    int insertionIndexFor (int x) const;

    WoodshedProcessor& processor;
    juce::TextButton addButton { "+" };
    int dropIndex = -1;
};

//==============================================================================
/** Full-window overlay for browsing and adding pedals. */
class LibraryPanel final : public juce::Component
{
public:
    explicit LibraryPanel (std::function<void (const juce::String& type)> onPick);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void visibilityChanged() override;

    std::function<void()> onClose;

private:
    class Card;
    void refresh();

    std::function<void (const juce::String&)> onPick;
    juce::TextEditor search;
    juce::OwnedArray<juce::TextButton> categoryButtons;
    juce::String category;
    juce::Viewport viewport;
    juce::Component grid;
    juce::OwnedArray<Card> cards;
    juce::Rectangle<int> panelBounds;
};

} // namespace woodshed

//==============================================================================
class WoodshedEditor final : public juce::AudioProcessorEditor,
                             public juce::DragAndDropContainer,
                             private juce::ChangeListener,
                             private juce::Timer
{
public:
    explicit WoodshedEditor (WoodshedProcessor&);
    ~WoodshedEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updateLatencyText();
    void startCapture (double seconds);
    void refreshPresetList();
    void savePresetAs();
    void showLibrary();

    WoodshedProcessor& woodshedProcessor;
    woodshed::BoardLookAndFeel lookAndFeel;

    juce::ComboBox inputChoice, presetChoice;
    juce::AudioProcessorValueTreeState::ComboBoxAttachment inputAttachment;
    juce::Slider inputGain, master;
    juce::AudioProcessorValueTreeState::SliderAttachment inputGainAttachment, masterAttachment;
    woodshed::Meter inputMeter, outputMeter;
    juce::Label latencyLabel, statusLabel;
    juce::TextButton audioSetupButton { "Audio" }, captureButton { "Capture" }, savePresetButton { "Save" },
                     libraryButton { "+ Add Pedal" };

    juce::Viewport boardViewport;
    woodshed::BoardContent board;
    woodshed::LibraryPanel library;

    juce::Array<juce::File> presetFiles;
    juce::File captureDir, captureRequestFile;
    juce::String pendingCaptureName;
    int statusFrames = 0, frame = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WoodshedEditor)
};
