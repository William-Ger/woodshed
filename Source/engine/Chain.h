#pragma once

#include "Registry.h"

namespace woodshed
{

/**
    An immutable, ordered list of pedals plus where to read their controls.
    Built on the message thread, handed to the audio thread through an atomic
    pointer, and deleted back on the message thread once the audio thread has
    moved on. Pedal instances are shared between successive chains so delay and
    reverb tails survive edits to the board.
*/
struct Chain
{
    struct Entry
    {
        std::shared_ptr<Pedal> pedal;
        const PedalInfo* info = nullptr;
        const std::atomic<float>* on = nullptr;
        std::array<const std::atomic<float>*, Pedal::maxKnobs> knobs {};   // normalised 0..1
        std::array<juce::NormalisableRange<float>, Pedal::maxKnobs> ranges;
        int numKnobs = 0;
    };

    std::vector<Entry> entries;
    int latencySamples = 0;

    /** Wires an entry to its controls. `knobSources` must hold info.knobs.size() pointers. */
    static Entry makeEntry (std::shared_ptr<Pedal>, const PedalInfo&, const std::atomic<float>* on,
                            const std::atomic<float>* const* knobSources);

    /** Audio thread. Processes the stereo working buffer in place.
        `scratchLeft/Right` must hold numSamples floats. Input is treated as mono (left == right). */
    void process (float* left, float* right, float* scratchLeft, float* scratchRight, int numSamples) const;
};

} // namespace woodshed
