#pragma once

#include "Sampler.h"
#include "../daw/Project.h"

/** Sampler tracks in a project: making them, and turning an audio region into one. Message thread;
    every change goes through the project's undo manager (the caller begins the transaction). */
namespace woodshed::sampler
{

/** A new instrument track playing an empty Sampler (its built-in sound) at `insertIndex`. */
juce::ValueTree addSamplerTrack (daw::Project&, const juce::String& name, int insertIndex = -1);

/** True for an instrument track whose instrument is the Sampler. */
bool isSamplerTrack (const juce::ValueTree& track);

/** Can this region become a Sampler track? (A plain audio region with a file.) */
bool canConvert (const juce::ValueTree& region);

/** Convert to Sampler Track: a new instrument track after the region's, with a Sampler holding the
    region's audio (Classic: one zone at its detected root; Slice: a zone per transient from C1)
    and a MIDI region in the same place that plays it back as it was. The audio region is muted
    so the song sounds the same. Returns the new track, or an invalid tree if the region can't be
    converted. */
juce::ValueTree convertToSamplerTrack (daw::Project&, juce::ValueTree audioRegion, Mode mode);

/** Loads a file into a Sampler track's instrument: copied into the project, laid out for `mode`. */
bool loadFileIntoTrack (daw::Project&, juce::ValueTree track, const juce::File&, Mode mode);

} // namespace woodshed::sampler
