#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>

namespace woodshed::daw
{

/**
    The song, as a ValueTree. Every edit goes through `undo` so it can be undone, and the
    whole tree is the save file (project.woodshed in the project folder, recordings in Audio/).

    PROJECT  name, sampleRate, tempo, beatsPerBar, beatUnit, click (bool), countIn (bool),
             loopOn (bool), loopStart, loopEnd (samples), masterVolume (dB), selectedTrack (id),
             markers (JSON array of { id, name, position }; a property, so the children stay tracks)
      TRACK  id (int, unique), name, colour (ARGB int), volume (dB), pan (-1..1),
             mute, solo, arm, monitor (bool), input (0-based interface channel), rigOn (bool)
        RIG     state (XML string of the track's rig: its board, amp, cab and knobs)
        REGION  id (int, unique), name, file (path relative to the project folder),
                start (timeline sample), offset (sample in the file), length (samples), gain (dB),
                fadeIn, fadeOut (samples; equal-power, so overlapping fades cross-fade), mute (bool)

    All positions and lengths are in samples at the project sample rate.
*/
namespace ids
{
#define WOODSHED_ID(name) inline const juce::Identifier name { #name };
    WOODSHED_ID (PROJECT) WOODSHED_ID (TRACK) WOODSHED_ID (RIG) WOODSHED_ID (REGION)
    WOODSHED_ID (name) WOODSHED_ID (sampleRate) WOODSHED_ID (tempo) WOODSHED_ID (beatsPerBar) WOODSHED_ID (beatUnit)
    WOODSHED_ID (click) WOODSHED_ID (countIn) WOODSHED_ID (loopOn) WOODSHED_ID (loopStart) WOODSHED_ID (loopEnd)
    WOODSHED_ID (masterVolume) WOODSHED_ID (selectedTrack)
    WOODSHED_ID (id) WOODSHED_ID (colour) WOODSHED_ID (volume) WOODSHED_ID (pan) WOODSHED_ID (mute) WOODSHED_ID (solo)
    WOODSHED_ID (arm) WOODSHED_ID (monitor) WOODSHED_ID (input) WOODSHED_ID (rigOn) WOODSHED_ID (state)
    WOODSHED_ID (file) WOODSHED_ID (start) WOODSHED_ID (offset) WOODSHED_ID (length) WOODSHED_ID (gain)
    WOODSHED_ID (fadeIn) WOODSHED_ID (fadeOut) WOODSHED_ID (CLIPBOARD) WOODSHED_ID (lane) WOODSHED_ID (markers)
#undef WOODSHED_ID
}

/** Musical time helpers for a constant tempo and meter. */
struct TimeGrid
{
    double sampleRate = 48000.0, tempo = 120.0;
    int beatsPerBar = 4, beatUnit = 4;

    double samplesPerBeat() const               { return sampleRate * 60.0 / tempo * 4.0 / beatUnit; }
    double samplesPerBar() const                { return samplesPerBeat() * beatsPerBar; }
    double toBeats (juce::int64 samples) const  { return (double) samples / samplesPerBeat(); }
    juce::int64 fromBeats (double beats) const  { return (juce::int64) std::llround (beats * samplesPerBeat()); }

    /** "bar.beat.tick" (1-based bar and beat, ticks out of 960), e.g. "5.2.480". */
    juce::String toBarsBeats (juce::int64 samples) const;
    /** "m:ss.mmm". */
    juce::String toClock (juce::int64 samples) const;
};

/** A named point on the timeline (Logic's marker track). */
struct Marker
{
    int id = 0;
    juce::String name;
    juce::int64 position = 0;
};

class Project
{
public:
    Project();

    juce::ValueTree state;
    juce::UndoManager undo;

    /** Where project.woodshed and Audio/ live. Every project has a folder from the start,
        so recordings always have somewhere to go. */
    juce::File folder;
    juce::File audioFolder() const                          { return folder.getChildFile ("Audio"); }
    juce::File projectFile() const                          { return folder.getChildFile ("project.woodshed"); }
    juce::File resolve (const juce::String& relativePath) const { return folder.getChildFile (relativePath); }

    /** A fresh project in a new folder under the default projects location. */
    static std::unique_ptr<Project> createNew (double sampleRate);
    /** An empty project (no tracks) in the given folder. */
    static std::unique_ptr<Project> createIn (const juce::File& folder, double sampleRate);
    static std::unique_ptr<Project> load (const juce::File& projectFileOrFolder);
    static juce::File defaultProjectsFolder();
    /** Where new projects go (Settings); empty restores Documents/Woodshed Projects. */
    static void setDefaultProjectsFolder (const juce::File&);

    bool save();
    /** Copies the project (and its audio) to a new folder and switches to it. */
    bool saveAs (const juce::File& newFolder);

    TimeGrid grid() const;

    // Tracks -------------------------------------------------------------------
    int numTracks() const                                   { return trackList().getNumChildren(); }
    juce::ValueTree track (int index) const                 { return trackList().getChild (index); }
    juce::ValueTree trackById (int id) const;
    juce::ValueTree addTrack (const juce::String& name, int insertIndex = -1);
    void removeTrack (const juce::ValueTree& track);
    juce::ValueTree trackList() const                       { return state; }

    // Regions ------------------------------------------------------------------
    juce::ValueTree addRegion (juce::ValueTree track, const juce::String& relativeFile, juce::int64 start,
                               juce::int64 offset, juce::int64 length, const juce::String& name);
    /** Splits at a timeline position; returns the new right-hand region (or invalid if outside). */
    juce::ValueTree splitRegion (juce::ValueTree region, juce::int64 at);
    static juce::ValueTree trackOf (const juce::ValueTree& region) { return region.getParent(); }
    juce::ValueTree regionById (int id) const;

    /** Copies of these regions for the clipboard: each one's start is relative to the earliest and
        its `lane` is its track's index relative to the topmost track among them. The clipboard
        remembers this project's folder so another project can paste it. */
    juce::ValueTree copyRegions (const juce::Array<int>& regionIds) const;
    /** Adds the clipboard's regions with their earliest start at `at`, the topmost on `firstTrack`
        and the rest on the tracks below it (the last track if there are none). Regions copied
        from another project bring their audio into this one's Audio folder. One undo step if
        the caller began a transaction. Returns the new regions' ids. */
    juce::Array<int> pasteRegions (const juce::ValueTree& clipboard, juce::int64 at, int firstTrack);
    /** Logic's Repeat: a copy of the regions straight after them, as a block. Returns the copies' ids. */
    juce::Array<int> repeatRegions (const juce::Array<int>& regionIds);
    /** Moves the regions by `delta` samples, as a block that stops at the song start. */
    void nudgeRegions (const juce::Array<int>& regionIds, juce::int64 delta);

    // Markers -------------------------------------------------------------------
    /** In timeline order. */
    std::vector<Marker> getMarkers() const;
    /** A new marker ("Marker N") at `at`, or the one already there. Returns its id. */
    int addMarker (juce::int64 at);
    void moveMarker (int id, juce::int64 to);
    void renameMarker (int id, const juce::String& name);
    void removeMarker (int id);
    /** The first marker after / the last one before `from`, or -1. */
    juce::int64 nextMarkerAfter (juce::int64 from) const;
    juce::int64 previousMarkerBefore (juce::int64 from) const;

    /** End of the last region, in samples. */
    juce::int64 contentEnd() const;

    int nextId();

private:
    static juce::ValueTree createState (double sampleRate);
    static juce::Colour trackColour (int index);
    void setMarkers (const std::vector<Marker>&);
};

} // namespace woodshed::daw
