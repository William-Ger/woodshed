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
             loopOn (bool), loopStart, loopEnd (samples), masterVolume (dB), selectedTrack (id)
      TRACK  id (int, unique), name, colour (ARGB int), volume (dB), pan (-1..1),
             mute, solo, arm, monitor (bool), input (0-based interface channel), rigOn (bool)
        RIG     state (XML string of the track's rig: its board, amp, cab and knobs)
        REGION  id (int, unique), name, file (path relative to the project folder),
                start (timeline sample), offset (sample in the file), length (samples), gain (dB),
                fadeIn, fadeOut (samples; equal-power, so overlapping fades cross-fade)

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
    WOODSHED_ID (fadeIn) WOODSHED_ID (fadeOut)
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

    /** End of the last region, in samples. */
    juce::int64 contentEnd() const;

    int nextId();

private:
    static juce::ValueTree createState (double sampleRate);
    static juce::Colour trackColour (int index);
};

} // namespace woodshed::daw
