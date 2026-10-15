#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "TunerPanel.h"

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
    /** Studio mode: a thin continuous bar in the studio's meter colours instead of LED segments. */
    void setSlim (bool shouldBeSlim)                         { slim = shouldBeSlim; repaint(); }

private:
    float level = 0.0f, hold = 0.0f;
    int holdFrames = 0;
    bool slim = false;
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

    /** The enclosure (or amp/cab front) inside this component, which also includes jacks and shadow. */
    juce::Rectangle<float> bodyArea() const;
    juce::Rectangle<float> controlArea() const;

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
    juce::Rectangle<int> spotBounds (int spot) const;        // the cell on the rails
    juce::Rectangle<float> bodyBounds (int spot) const;      // where the enclosure sits in that cell
    juce::Rectangle<int> pedalBounds (int spot) const;       // the PedalView, including jacks and shadow
    int spotAt (juce::Point<int>) const;
    void layoutPedals();

    WoodshedProcessor& processor;
    juce::OwnedArray<PedalView> pedals;
    std::vector<int> spots;          // per slot
    juce::Rectangle<float> boardImage;  // where the board render is drawn
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
    juce::Rectangle<float> ampSlot, cabSlot;
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
    /** Studio mode: the studio's panels, cards and controls instead of the stage look. */
    void setEmbedded (bool);

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
    bool embedded = false;
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

    /** Studio mode: the rig sits in a pane of the studio, which owns the audio interface, the
        master level, full screen and keyboard focus. Hides those controls, stops the editor
        taking focus on its own and lets keys it doesn't use reach the studio. */
    void setEmbedded (bool shouldBeEmbedded);
    bool isEmbedded() const                                  { return embedded; }
    /** Shown beside the wordmark in studio mode. */
    void setTrackName (const juce::String&);

    void toggleTuner();
    bool isTunerShowing() const                              { return tuner.isVisible(); }

    /** In studio mode the pane scales the editor down below this rather than crowding it. */
    static constexpr int embeddedMinWidth = 1000, embeddedMinHeight = 368;

    /** Studio mode: the top bar's controls, which the studio's pane header adopts (reparents)
        so they stay full size while the stage below scales. They still belong to this editor
        and go with it. */
    struct HostControls
    {
        juce::Slider& inputGain;
        woodshed::Meter& inputMeter;
        juce::ComboBox& presets;
        juce::Button& previousPreset;
        juce::Button& nextPreset;
        juce::Button& savePreset;
        juce::Button& addPedal;
    };
    HostControls getHostControls()                           { return { inputGain, inputMeter, presetChoice, prevPresetButton,
                                                                        nextPresetButton, savePresetButton, libraryButton }; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updateLatencyText();
    void startCapture (double seconds);
    void refreshPresetList();
    void stepPreset (int delta);
    void presetChosen (int id);
    void savePresetAs();
    void showLibrary (int targetSpot = -1, const juce::String& category = {});
    void keepControlsFromTakingFocus (juce::Component&);
    void paintEmbeddedFooter (juce::Graphics&);
    int topBarHeight() const                                 { return embedded ? 0 : 72; }
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
                     libraryButton { "+ Add Pedal" }, tunerButton { "Tuner" },
                     prevPresetButton { "<" }, nextPresetButton { ">" };

    woodshed::BoardView board;
    woodshed::RigStack rig;
    woodshed::LibraryPanel library;
    TunerPanel tuner;
    int libraryTargetSpot = -1;

    juce::Array<juce::File> presetFiles;
    juce::String presetCaption;                 // genre, name and description of the loaded preset
    static constexpr int defaultBoardId = 9999, factoryIdBase = 10000;
    juce::File captureDir, captureRequestFile;
    juce::String pendingCaptureName;
    int statusFrames = 0, frame = 0;
    bool embedded = false;
    juce::String trackName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WoodshedEditor)
};
