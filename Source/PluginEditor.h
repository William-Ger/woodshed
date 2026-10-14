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
    PedalView (WoodshedProcessor&, int slot, const juce::String& type, bool draggable);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    void toggle()                                            { footswitch.setToggleState (! footswitch.getToggleState(), juce::sendNotification); }
    int getSlot() const                                      { return slot; }
    bool isBeingDragged() const                              { return dragging; }

    std::function<void (PedalView&)> onDragMove, onDragEnd;

private:
    WoodshedProcessor& processor;
    const int slot;
    const PedalType* type = nullptr;
    juce::String typeId;
    const bool draggable;

    juce::OwnedArray<Knob> knobs;
    juce::ToggleButton footswitch;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> footswitchAttachment;
    juce::TextButton removeButton { "x" };
    juce::ComponentDragger dragger;
    bool dragging = false;
};

//==============================================================================
/** The 5 x 2 pedalboard: rails, spots, patch cables. Pedals snap to spots when dropped. */
class BoardView final : public juce::Component
{
public:
    explicit BoardView (WoodshedProcessor&);

    void rebuild();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    juce::Array<PedalView*> pedalsInChainOrder() const;
    std::function<void (int spot)> onEmptySpotClicked;

private:
    juce::Rectangle<int> spotBounds (int spot) const;
    juce::Rectangle<int> pedalBounds (int spot) const        { return spotBounds (spot).reduced (5, 10); }
    int spotAt (juce::Point<int>) const;
    void layoutPedals();

    WoodshedProcessor& processor;
    juce::OwnedArray<PedalView> pedals;
    std::vector<int> spots;          // per slot
    juce::Rectangle<int> deck;       // the area inside the frame
    int dragTarget = -1, hoverSpot = -1;
};

//==============================================================================
/** The amp head sitting on its cab, beside the board. */
class RigStack final : public juce::Component
{
public:
    explicit RigStack (WoodshedProcessor&);

    void rebuild();
    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void (const juce::String& category)> onChoose;
    PedalView* getAmp() const                                { return amp.get(); }
    PedalView* getCab() const                                { return cab.get(); }

private:
    WoodshedProcessor& processor;
    std::unique_ptr<PedalView> amp, cab;
    juce::TextButton chooseAmp { "Choose amp" }, chooseCab { "Choose cab" },
                     swapAmp { "Change" }, swapCab { "Change" };
};

//==============================================================================
/** Full-window overlay for browsing and adding pedals. */
class LibraryPanel final : public juce::Component
{
public:
    explicit LibraryPanel (std::function<void (const juce::String& type)> onPick);

    void showCategory (const juce::String& category);

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
    void showLibrary (int targetSpot = -1, const juce::String& category = {});
    void pickFromLibrary (const juce::String& type);
    void flash (const juce::String& message);

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

    woodshed::BoardView board;
    woodshed::RigStack rig;
    woodshed::LibraryPanel library;
    int libraryTargetSpot = -1;

    juce::Array<juce::File> presetFiles;
    juce::File captureDir, captureRequestFile;
    juce::String pendingCaptureName;
    int statusFrames = 0, frame = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WoodshedEditor)
};
