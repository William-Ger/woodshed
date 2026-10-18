#pragma once

#include "Instrument.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>

/**
    The Sampler: plays audio files across the keyboard in zones.

    Its knobs (envelopes, filter, LFO, glide, voices) work like any instrument's. Its zones and
    samples live in a SAMPLER child of the track's INSTRUMENT node, which the engine hands it
    through Instrument::syncState whenever the project changes:

      SAMPLER  mode ("classic", "oneshot", "slice" or "multi"), sensitivity (0..1, Slice mode's
               transient threshold), gate (bool: Slice mode stops a slice at note-off)
        ZONE   file (path relative to the project folder), name, root (MIDI note), lo, hi (key
               range), velLo, velHi (1..127), start, end (sample frames in the file; end -1 is the
               file's end), loop ("off", "forward" or "alternate"), loopStart, loopEnd, crossfade
               (frames), tune (semitones), fine (cents), gain (dB), pan (-1..1), group (round-robin
               group: matching zones of one group take turns; 0 plays every matching zone)

    Samples are copied into the project's Audio folder when loaded (Convert to Sampler Track
    uses the region's own file), so the project folder carries everything it needs.
*/
namespace woodshed::sampler
{

namespace ids
{
#define WOODSHED_SAMPLER_ID(name) inline const juce::Identifier name { #name };
    WOODSHED_SAMPLER_ID (SAMPLER) WOODSHED_SAMPLER_ID (ZONE)
    WOODSHED_SAMPLER_ID (mode) WOODSHED_SAMPLER_ID (sensitivity) WOODSHED_SAMPLER_ID (gate)
    WOODSHED_SAMPLER_ID (file) WOODSHED_SAMPLER_ID (name) WOODSHED_SAMPLER_ID (root) WOODSHED_SAMPLER_ID (lo) WOODSHED_SAMPLER_ID (hi)
    WOODSHED_SAMPLER_ID (velLo) WOODSHED_SAMPLER_ID (velHi) WOODSHED_SAMPLER_ID (start) WOODSHED_SAMPLER_ID (end)
    WOODSHED_SAMPLER_ID (loop) WOODSHED_SAMPLER_ID (loopStart) WOODSHED_SAMPLER_ID (loopEnd) WOODSHED_SAMPLER_ID (crossfade)
    WOODSHED_SAMPLER_ID (tune) WOODSHED_SAMPLER_ID (fine) WOODSHED_SAMPLER_ID (gain) WOODSHED_SAMPLER_ID (pan) WOODSHED_SAMPLER_ID (group)
#undef WOODSHED_SAMPLER_ID
}

/** The instrument's registry id. */
inline const juce::String typeId { "sampler" };

/** How a loaded sample is laid out, and whether note-off stops it. */
enum class Mode { classic, oneShot, slice, multi };
juce::String modeId (Mode);
juce::String modeName (Mode);
Mode modeFrom (const juce::String& id);

enum class LoopMode { off, forward, alternate };
juce::String loopId (LoopMode);
LoopMode loopFrom (const juce::String& id);

/** Slice mode maps slices to consecutive keys from here (C1 when middle C is C3). */
constexpr int firstSliceNote = 36;
constexpr int maxZones = 128;

/** One ZONE, as plain values. */
struct Zone
{
    juce::String file, name;
    int root = 60, lo = 0, hi = 127, velLo = 1, velHi = 127;
    juce::int64 start = 0, end = -1, loopStart = 0, loopEnd = 0, crossfade = 0;
    LoopMode loop = LoopMode::off;
    float tune = 0.0f, fine = 0.0f, gain = 0.0f, pan = 0.0f;
    int group = 0;

    static Zone fromTree (const juce::ValueTree&);
    juce::ValueTree toTree() const;
};

//==============================================================================
/** A decoded audio file: up to two channels at the file's own rate. Immutable once loaded. */
struct Source
{
    juce::AudioBuffer<float> audio;
    double rate = 48000.0;
    juce::int64 length() const                  { return audio.getNumSamples(); }
};

/** Decodes a file, sharing one copy between every sampler that uses it. Message thread. Null if unreadable. */
std::shared_ptr<const Source> loadSource (const juce::File&);

/** Audio formats the Sampler can load (for drag and drop and file choosers). */
juce::String audioFileWildcard();
bool isAudioFile (const juce::File&);

//==============================================================================
// Analysis (message thread).

/** The sample's pitch over [from, to): the nearest MIDI note and how many cents off it is.
    found is false for unpitched material (drums, noise); note is then 60. */
struct Pitch { int note = 60; float cents = 0.0f; bool found = false; };
Pitch detectPitch (const Source&, juce::int64 from, juce::int64 to);

/** Where the transients are in [from, to), always starting with `from`. Sensitivity 0 finds only
    the strongest hits, 1 nearly every rise in level. */
std::vector<juce::int64> detectSlices (const Source&, juce::int64 from, juce::int64 to, float sensitivity);

//==============================================================================
// Editing the SAMPLER node (message thread; every change through `undo`).

/** The INSTRUMENT node's SAMPLER child, made on first use. */
juce::ValueTree samplerNode (juce::ValueTree instrumentNode, juce::UndoManager*);

/** Lays a sample out for `mode`: Classic one zone across the keyboard at its detected root,
    One Shot one zone that plays the whole sample at its own pitch from C3, Slice a zone per
    transient from C1, Multi adds a zone at its detected root beside the zones already there.
    `relativeFile` is how the ZONE names the file; [from, to) is the part to use (to -1: the end). */
void loadSample (juce::ValueTree sampler, Mode mode, const juce::String& relativeFile, const Source&,
                 juce::int64 from, juce::int64 to, juce::UndoManager*);

/** Slice mode again on the first zone's file and range (after the sensitivity changes). */
void reslice (juce::ValueTree sampler, const Source&, juce::UndoManager*);

/** Copies a file into the project's Audio folder (a unique name if one is taken) and returns its
    path relative to the project folder; a file already in the project is used where it is. */
juce::String importIntoProject (const juce::File& file, const juce::File& projectFolder);

//==============================================================================
/** Info and knobs for the registry. */
InstrumentInfo makeInfo();

/** Knob indices, in InstrumentInfo order. */
namespace param
{
    enum : int { attack, decay, sustain, release, filterType, cutoff, resonance, envAmount,
                 filterAttack, filterDecay, filterSustain, filterRelease, pitchEnv, pitchTime,
                 lfoRate, lfoPitch, lfoFilter, lfoAmp, glide, voiceMode, voices, velocity, level, builtIn };
}

} // namespace woodshed::sampler
