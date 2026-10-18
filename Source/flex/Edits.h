#pragma once

#include "Analysis.h"
#include "../daw/Project.h"

namespace woodshed::flex
{

/**
    The song edits behind Time Stretch, Pitch Edit and Tempo Detect. Each goes through the
    project's undo manager; the caller begins the transaction.
*/

/** The region's markers, with anchors at its visible ends added if it has fewer than two, so
    moving a marker stretches only up to the region's edges. */
WarpMap anchoredWarp (const juce::ValueTree& region);

/** Turns Time Stretch on or off for a track (and sets its mode, if given). */
void setTrackFlex (daw::Project&, juce::ValueTree track, bool on);
void setTrackMode (daw::Project&, juce::ValueTree track, Mode);

/** A marker on file sample `src`, where it plays now (anchors added if needed). Returns its index. */
int addMarker (daw::Project&, juce::ValueTree region, double src);
/** Moves marker `index` to play at region-time `dst` (kept between its neighbours). */
void moveMarker (daw::Project&, juce::ValueTree region, int index, double dst);
void removeMarker (daw::Project&, juce::ValueTree region, int index);
/** Where a marker may go: between its neighbours, with a little room. */
juce::Range<double> markerRange (const WarpMap&, int index, double sampleRate);
/** Back to the audio as recorded: no markers, the region kept where it starts. */
void resetTiming (daw::Project&, juce::ValueTree region);

/** Moves the region's transients onto the nearest `gridTicks` (markers on each, the region's
    ends kept). Turns the track's Time Stretch on. */
void quantizeAudio (daw::Project&, juce::ValueTree region, const std::vector<double>& transients, double gridTicks);

/** Follow tempo: when the tempo map changes, the region's audio stretches with its bars. */
void setFollowTempo (daw::Project&, juce::ValueTree region, bool on);
/** What keepMusicalPositions does to a region that follows the tempo: its start, end, content
    origin and markers all keep their bar and beat. */
void retimeForTempo (juce::ValueTree region, const daw::TimeGrid& before, const daw::TimeGrid& after, juce::UndoManager*);

/** Tempo Detect, adopting: the song's tempo map becomes the region's (one tempo, or a tempo
    per bar if it drifts) and the region moves so its first downbeat lands on a bar line. */
void adoptTempo (daw::Project&, juce::ValueTree region, const TempoEstimate&);
/** Tempo Detect, conforming: a marker on every beat, placed on the song's beats, so the audio
    plays at the song's tempo and follows it from now on. */
void conformToTempo (daw::Project&, juce::ValueTree region, const TempoEstimate&);

/** Scales for Pitch Edit's quantize: semitones above the root that belong. */
enum class Scale { chromatic, major, minor, majorPentatonic, minorPentatonic, blues, dorian, mixolydian };
juce::StringArray scaleNames();
/** The nearest pitch in the scale to `pitch` (MIDI). */
float nearestInScale (float pitch, int root, Scale);
/** Moves each note (those in `which`, or all) to the nearest scale pitch, fine pitch removed. */
void quantizeToScale (std::vector<PitchNote>&, int root, Scale, const std::vector<int>& which = {});

} // namespace woodshed::flex
