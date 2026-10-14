#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "engine/Chain.h"

/**
    The board is a list of up to maxSlots pedals. Slot i always holds the i-th
    pedal in the chain (occupied slots are kept contiguous from 0). Each slot has
    host-visible parameters "sN_on" and "sN_p0".."sN_p7" (normalised 0..1); which
    pedal type lives in a slot is stored in the state tree, not as a parameter, so
    adding new pedal types never shifts anyone's saved presets.
*/
class WoodshedProcessor final : public juce::AudioProcessor,
                                public juce::ChangeBroadcaster,
                                private juce::ValueTree::Listener,
                                private juce::Timer
{
public:
    static constexpr int maxSlots = 16;

    WoodshedProcessor();
    ~WoodshedProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }

    const juce::String getName() const override            { return JucePlugin_Name; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 8.0; }

    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // Board editing (message thread). Changes are broadcast via ChangeBroadcaster.
    juce::StringArray getBoard() const;
    void insertPedal (const juce::String& type, int position);
    void removePedal (int position);
    void movePedal (int from, int to);
    void setBoard (const juce::StringArray& types);         // resets every knob to its default
    void loadDefaultBoard();

    static juce::String onParamId (int slot)               { return "s" + juce::String (slot) + "_on"; }
    static juce::String knobParamId (int slot, int knob)   { return "s" + juce::String (slot) + "_p" + juce::String (knob); }

    // Presets are plain state files.
    static juce::File presetFolder();
    bool savePreset (const juce::File&);
    bool loadPreset (const juce::File&);

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

    juce::ValueTree boardTree();
    void writeBoard (const juce::StringArray& types);
    void rebuildChain();
    void copySlotValues (int from, int to);
    void resetSlotValues (int slot, const juce::String& type);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeRedirected (juce::ValueTree&) override   { rebuildChain(); }
    void timerCallback() override;

    // Message-thread view of the live pedals, slot by slot.
    std::array<std::shared_ptr<woodshed::Pedal>, maxSlots> instances;
    std::array<juce::String, maxSlots> instanceTypes;

    // Chain handoff: message thread publishes to `pending`, audio thread swaps it into
    // `current` and retires the old one through a FIFO for deletion on the message thread.
    std::atomic<woodshed::Chain*> pending { nullptr };
    woodshed::Chain* current = nullptr;
    juce::AbstractFifo retireFifo { 64 };
    std::array<woodshed::Chain*, 64> retired {};
    void freeRetiredChains();

    juce::AudioBuffer<float> work, scratch;
    juce::SmoothedValue<float> inputGain, masterGain;
    double currentSampleRate = 48000.0;
    int preparedBlockSize = 512;

    enum class CaptureState { idle, recording, done };
    std::atomic<CaptureState> captureState { CaptureState::idle };
    static constexpr double maxCaptureSeconds = 30.0;
    juce::AudioBuffer<float> captureBuffer;
    int captureLength = 0, capturePos = 0;

    float param (const char* id) const                     { return params.getRawParameterValue (id)->load(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WoodshedProcessor)
};
