#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Pedals.h"

class WoodshedProcessor final : public juce::AudioProcessor
{
public:
    WoodshedProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }

    const juce::String getName() const override            { return JucePlugin_Name; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 4.0; }

    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // Pedal ids, in signal-chain order.
    static inline const juce::StringArray pedalIds { "gate", "drive", "delay", "room" };

    juce::AudioProcessorValueTreeState params;

    // Peak levels since the editor last read them (linear gain).
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };

    // Debug capture: records dry input (ch 0) and processed left output (ch 1).
    void armCapture (double seconds);
    bool captureFinished() const                           { return captureState.load() == CaptureState::done; }
    bool captureRunning() const                            { return captureState.load() == CaptureState::recording; }
    bool writeCapture (const juce::File& file);

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    struct Bypass
    {
        juce::SmoothedValue<float> amount;
        std::atomic<float>* onParam = nullptr;
    };

    woodshed::Gate gate;
    woodshed::Drive drive;
    woodshed::Delay delay;
    woodshed::Room room;
    std::array<Bypass, 4> bypass;

    juce::AudioBuffer<float> mono, scratch, stereo;
    juce::SmoothedValue<float> inputGain, masterGain;
    double currentSampleRate = 44100.0;

    enum class CaptureState { idle, recording, done };
    std::atomic<CaptureState> captureState { CaptureState::idle };
    static constexpr double maxCaptureSeconds = 30.0;
    juce::AudioBuffer<float> captureBuffer;
    int captureLength = 0, capturePos = 0;

    void ensureCapacity (int numSamples);
    float param (const char* id) const                     { return params.getRawParameterValue (id)->load(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WoodshedProcessor)
};
