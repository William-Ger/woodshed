#pragma once

#include "Registry.h"

namespace woodshed
{

/**
    Factory presets live as JSON files in presets/, one file per genre:

        { "genre": "Blues",
          "presets": [
            { "name": "Front Porch",
              "description": "Warm edge-of-breakup with a slap of room.",
              "amp":   { "type": "amp.head",    "knobs": { "gain": 4.5, "voice": "Crunch" } },
              "cab":   { "type": "cab.speaker", "knobs": { "mic": 0.3 } },
              "board": [ { "type": "drive.overdrive", "spot": 0, "on": true, "knobs": { "drive": 3 } } ] } ] }

    Knob values are in the knob's real units (what `woodshed-bench knobs` prints); "%" knobs
    run 0..1. Selectors take the choice label or its index. Knobs left out keep their
    defaults. Spots are 0-9: top row 0-4 left to right, then the bottom row 5-9.
*/
struct PresetPedal
{
    juce::String type;
    int spot = -1;                                  // -1 for the amp and cab
    bool on = true;
    std::array<float, Pedal::maxKnobs> knobs {};    // normalised 0..1
};

struct Preset
{
    juce::String name, genre, description;
    std::vector<PresetPedal> pedals;                // board pedals, then the amp, then the cab
};

/** Parses one genre file. Anything wrong (unknown pedal or knob, value out of range,
    clashing spots...) is added to `errors` as "preset name: problem". */
std::vector<Preset> parsePresets (const juce::String& json, juce::StringArray& errors);

} // namespace woodshed
