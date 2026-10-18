#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <vector>

namespace woodshed::flex
{

/**
    Time Stretch, Pitch Edit and Tempo Detect: the song model's side of it.

    TRACK   flexOn (bool: the track's audio regions play through their time and pitch edits),
            flexMode ("mono", "poly", "rhythmic" or "speed"; "poly" when absent)
    REGION  flexMarkers ("src:dst;..." in order: file sample -> sample from the region's content
                         origin (start - offset). Between markers the audio stretches; past the
                         first and last it keeps the edge segment's speed. Empty: as recorded),
            followTempo (bool: when the tempo map changes, the markers move with their bars and
                         beats, so the audio stretches to stay on them),
            sourceTempo (BPM Tempo Detect found in the file, for display and conforming),
            pitchNotes ("start:end:pitch:shift:drift:vibrato:gain:formant;..." the notes Pitch
                        Edit found: file samples, the detected MIDI pitch, and the edits: shift in
                        semitones (fine pitch included), drift and vibrato as a share of the
                        original (1 = as sung), gain in dB, formant shift in semitones)

    When a track's Time Stretch is on, a region's offset and length are timeline samples from its
    content origin through the markers (the same numbers as before while there are none), so
    trims, splits and copies work on stretched regions unchanged.
*/
namespace ids
{
    inline const juce::Identifier flexOn { "flexOn" }, flexMode { "flexMode" }, flexMarkers { "flexMarkers" },
                                  followTempo { "followTempo" }, sourceTempo { "sourceTempo" }, pitchNotes { "pitchNotes" };
}

/** How a track stretches: a phase vocoder tuned for single voices or for anything, cutting at
    transients without stretching (drums), or tape-style speed (pitch follows). */
enum class Mode { monophonic, polyphonic, rhythmic, speed };

juce::String modeId (Mode);
juce::String modeName (Mode);
Mode modeFromId (const juce::String&);
Mode modeOf (const juce::ValueTree& track);

/** A flex marker: this file sample plays at this sample from the region's content origin. */
struct Marker
{
    double src = 0.0, dst = 0.0;
};

/** The piecewise-linear map from file samples to region time that a region's markers make. */
struct WarpMap
{
    std::vector<Marker> markers;        // in order, both columns strictly increasing

    bool isIdentity() const;
    double toDst (double src) const;
    double toSrc (double dst) const;
    /** Stretch (output over input) of the segment around a file sample. */
    double ratioAt (double src) const;

    static WarpMap decode (const juce::String&);
    juce::String encode() const;
    /** Puts the markers in order and drops any that would run backwards. */
    void tidy();
};

WarpMap warpOf (const juce::ValueTree& region);
void setWarp (juce::ValueTree region, const WarpMap&, juce::UndoManager*);

/** A note Pitch Edit found, and what's been done to it. */
struct PitchNote
{
    double start = 0.0, end = 0.0;      // file samples
    float pitch = 60.0f;                // as sung (median MIDI pitch)
    float shift = 0.0f;                 // semitones, fine pitch included
    float drift = 1.0f, vibrato = 1.0f; // share of the original's slow drift and its vibrato
    float gain = 0.0f;                  // dB
    float formant = 0.0f;               // semitones

    bool isEdited() const;
};

std::vector<PitchNote> notesOf (const juce::ValueTree& region);
void setNotes (juce::ValueTree region, const std::vector<PitchNote>&, juce::UndoManager*);
juce::String encodeNotes (const std::vector<PitchNote>&);
std::vector<PitchNote> decodeNotes (const juce::String&);

/** True when an audio region plays a rendered version (its track's Time Stretch is on and it has
    markers or pitch edits). */
bool isProcessed (const juce::ValueTree& region);

} // namespace woodshed::flex
