#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "engine/Chain.h"
#include "engine/Presets.h"

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
    //
    // The pedalboard has boardRows x boardColumns spots; each pedal sits in one spot and
    // the signal runs left to right, top row first. The amp and cab are not on the board:
    // they always come last in the chain (pedals -> amp -> cab). Slot order always equals
    // chain order, and getSpots() tells the UI where each board pedal sits.
    static constexpr int boardRows = 2, boardColumns = 5, boardSpots = boardRows * boardColumns;

    juce::StringArray getBoard() const;                     // pedal type per slot, chain order
    std::vector<int> getSpots() const;                      // spot per slot (row * columns + col), -1 for amp/cab
    static bool isRigType (const juce::String& type);       // amp or cab: lives in the stack, not on the board
    static juce::String rigCategory (const juce::String& type);

    int findFreeSpot() const;
    bool addPedal (const juce::String& type, int spot = -1); // amp/cab replace the current one
    void removePedal (int slot);
    void movePedal (int slot, int spot);                    // swaps if the spot is taken
    void loadDefaultBoard();

    static juce::String onParamId (int slot)               { return "s" + juce::String (slot) + "_on"; }
    static juce::String knobParamId (int slot, int knob)   { return "s" + juce::String (slot) + "_p" + juce::String (knob); }

    // User presets are plain state files. Loading one (or a factory preset) changes the
    // board, amp and cab but leaves the input, input gain and master where the player set them.
    static juce::File presetFolder();
    bool savePreset (const juce::File&);
    bool loadPreset (const juce::File&);

    // Factory presets ship inside the app (presets/*.json), in genre order.
    static const std::vector<woodshed::Preset>& factoryPresets();
    static const juce::StringArray& factoryGenres();
    void loadFactoryPreset (const woodshed::Preset&);

    juce::AudioProcessorValueTreeState params;

    // Peak levels since the editor last read them (linear gain).
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };

    // Tuner: while open, the audio thread copies the dry input into a FIFO the tuner reads,
    // and (optionally) mutes the output like a tuner pedal.
    std::atomic<bool> tunerOpen { false }, tunerMutes { true };
    int readTunerInput (float* dest, int maxSamples);       // message thread
    double getCurrentSampleRate() const                    { return currentSampleRate; }

    // Debug capture: records dry input (ch 0) and processed left output (ch 1).
    void armCapture (double seconds);
    bool captureFinished() const                           { return captureState.load() == CaptureState::done; }
    bool captureRunning() const                            { return captureState.load() == CaptureState::recording; }
    bool writeCapture (const juce::File& file);

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    struct Placement { juce::String type; int spot = -1; };

    juce::ValueTree boardTree();
    std::vector<Placement> getPlacements() const;
    /** Puts the board in chain order and moves every slot's knobs and instance along with it.
        `sourceSlots[i]` is the slot the i-th placement currently occupies (-1 for a new pedal). */
    void applyPlacements (std::vector<Placement>, std::vector<int> sourceSlots);
    void rebuildChain();
    void resetSlotValues (int slot, const juce::String& type);
    void holdAllKnobs();
    void replaceStateKeepingLevels (const juce::ValueTree&);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeRedirected (juce::ValueTree&) override;
    void timerCallback() override;

    // Message-thread view of the live pedals, slot by slot.
    std::array<std::shared_ptr<woodshed::Pedal>, maxSlots> instances;
    std::array<juce::String, maxSlots> instanceTypes;

    // Chain handoff: message thread publishes to `pending`, audio thread swaps it into
    // `current` and retires the old one through a FIFO for deletion on the message thread.
    std::atomic<woodshed::Chain*> pending { nullptr };
    woodshed::Chain* current = nullptr;

    // While slots are being shuffled, pedals keep their last knob values until the audio
    // thread is running a chain built after the shuffle (see holdAllKnobs()).
    std::atomic<int> installedGeneration { 0 };
    int publishedGeneration = 0, releaseHoldAtGeneration = -1;
    juce::AbstractFifo retireFifo { 64 };
    std::array<woodshed::Chain*, 64> retired {};
    void freeRetiredChains();

    juce::AbstractFifo tunerFifo { 1 << 15 };
    std::vector<float> tunerRing = std::vector<float> ((size_t) (1 << 15));

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
