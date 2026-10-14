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
    juce::Label* createSliderTextBox (juce::Slider&) override;
};

//==============================================================================
class Knob final : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& label);

    void resized() override;

    juce::Slider slider;

private:
    juce::Label caption;
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
struct PedalStyle
{
    juce::String name, subtitle;
    juce::Colour body, ink;
};

class Pedal final : public juce::Component
{
public:
    Pedal (juce::AudioProcessorValueTreeState&, const juce::String& pedalId, PedalStyle,
           std::vector<std::pair<juce::String, juce::String>> knobs, int hotkey);

    void paint (juce::Graphics&) override;
    void resized() override;
    void toggle()                                   { footswitch.setToggleState (! footswitch.getToggleState(), juce::sendNotification); }

private:
    PedalStyle style;
    int hotkey;
    juce::OwnedArray<Knob> knobs;
    juce::ToggleButton footswitch;
    juce::AudioProcessorValueTreeState::ButtonAttachment footswitchAttachment;
};

} // namespace woodshed

//==============================================================================
class WoodshedEditor final : public juce::AudioProcessorEditor,
                             private juce::Timer
{
public:
    explicit WoodshedEditor (WoodshedProcessor&);
    ~WoodshedEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;
    void updateLatencyText();
    void startCapture (double seconds);

    WoodshedProcessor& woodshedProcessor;
    woodshed::BoardLookAndFeel lookAndFeel;

    juce::ComboBox inputChoice;
    juce::AudioProcessorValueTreeState::ComboBoxAttachment inputAttachment;
    woodshed::Knob inputGain, master;
    woodshed::Meter inputMeter, outputMeter;
    juce::Label latencyLabel, statusLabel;
    juce::TextButton audioSetupButton { "Audio Setup" }, captureButton { "Capture 10s" };

    juce::OwnedArray<woodshed::Pedal> pedals;
    juce::File captureDir, captureRequestFile;
    juce::String pendingCaptureName;
    int statusFrames = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WoodshedEditor)
};
