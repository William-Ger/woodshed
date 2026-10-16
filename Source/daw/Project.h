#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>

namespace woodshed::daw
{

/**
    The song, as a ValueTree. Every edit goes through `undo` so it can be undone, and the
    whole tree is the save file (project.woodshed in the project folder, recordings in Audio/).

    PROJECT  (children: the TRACKs in order, then at most one MASTER, always last)
             name, sampleRate, tempo, beatsPerBar, beatUnit, click (bool), countIn (bool),
             loopOn (bool), loopStart, loopEnd (samples), masterVolume (dB), selectedTrack (id),
             markers (JSON array of { id, name, position }; a property, so the children stay tracks)
      TRACK  id (int, unique), name, colour (ARGB int), volume (dB), pan (-1..1),
             mute, solo, arm, monitor (bool), input (0-based interface channel), rigOn (bool),
             kind ("audio" when absent, "instrument", or "aux": a bus that plays the sends it receives
             through its inserts and fader; it has no regions or input)
        RIG        state (XML string of the track's rig: its board, amp, cab and knobs); audio tracks
        INSTRUMENT type (InstrumentRegistry id), patch (name), knobs (comma-separated real values in
                   the instrument's knob order); instrument tracks. Or type "plugin": an Audio Unit or
                   VST3, with plugin (its PluginDescription as XML) and state (base64)
        FX         id, type (pedal Registry id), on (bool), knobs (comma-separated real values in the
                   pedal's knob order); the track's insert effects in signal order, after the rig or
                   instrument. Or type "plugin" with plugin and state, as for INSTRUMENT
        SEND       bus (an aux track's id), level (dB), pre (bool: before the fader and pan); the
                   track's signal, also sent to that aux
        AUTOMATION param ("volume" in dB, "pan" -1..1, "inst:<knob index>" or "fx:<FX id>:<knob index>"
                   in the knob's real units), points ("sample:value;..." in time order). With
                   points, the parameter follows the line between them (and holds the first and
                   last values before and after).
        REGION  id (int, unique), name, start (timeline sample), offset, length (samples), gain (dB),
                fadeIn, fadeOut (samples; equal-power, so overlapping fades cross-fade), mute (bool),
                and either
                  file (path relative to the project folder; offset is the sample in the file), or
                  kind = "midi", with NOTE children: pitch (0..127), velocity (1..127),
                  start, length (ticks, 960 per quarter note, from the region's content origin,
                  which sits at timeline sample start - offset). Only the part of the content
                  inside [start, start + length) plays, so trims, splits and copies treat both
                  kinds of region alike.

      MASTER  the Stereo Out's insert effects: FX children as on a track (mastering EQ, a limiter...)

    All positions and lengths are in samples at the project sample rate, except notes (ticks).
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
    WOODSHED_ID (kind) WOODSHED_ID (INSTRUMENT) WOODSHED_ID (FX) WOODSHED_ID (NOTE) WOODSHED_ID (type) WOODSHED_ID (patch)
    WOODSHED_ID (knobs) WOODSHED_ID (on) WOODSHED_ID (pitch) WOODSHED_ID (velocity)
    WOODSHED_ID (plugin) WOODSHED_ID (MASTER) WOODSHED_ID (SEND) WOODSHED_ID (bus) WOODSHED_ID (level) WOODSHED_ID (pre)
    WOODSHED_ID (AUTOMATION) WOODSHED_ID (param) WOODSHED_ID (points) WOODSHED_ID (autoParam) WOODSHED_ID (showAutomation)
#undef WOODSHED_ID
}

/** Track and region kinds (the `kind` property). */
namespace kinds
{
    inline const juce::String audio { "audio" }, instrument { "instrument" }, midi { "midi" }, aux { "aux" };
}

/** MIDI note positions are in ticks: 960 per quarter note. */
constexpr int ticksPerQuarter = 960;

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

    double samplesPerTick() const               { return sampleRate * 60.0 / tempo / ticksPerQuarter; }
    double toTicks (double samples) const       { return samples / samplesPerTick(); }
    double fromTicks (double ticks) const       { return ticks * samplesPerTick(); }
};

/** A point on an automation line. */
struct AutoPoint
{
    juce::int64 time = 0;
    float value = 0.0f;
    bool operator== (const AutoPoint& o) const      { return time == o.time && juce::exactlyEqual (value, o.value); }
};

/** One note of a MIDI region, as stored (ticks from the region's content origin). */
struct Note
{
    int pitch = 60, velocity = 100;
    juce::int64 start = 0, length = ticksPerQuarter;
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
    int numTracks() const
    {
        const int n = state.getNumChildren();
        return n > 0 && state.getChild (n - 1).hasType (ids::MASTER) ? n - 1 : n;
    }
    /** The Stereo Out's node (its insert effects), made on first use. */
    juce::ValueTree master();
    juce::ValueTree track (int index) const                 { return trackList().getChild (index); }
    juce::ValueTree trackById (int id) const;
    juce::ValueTree addTrack (const juce::String& name, int insertIndex = -1);
    /** A software instrument track playing `instrumentType` with these knob values (real units). */
    juce::ValueTree addInstrumentTrack (const juce::String& name, const juce::String& instrumentType,
                                        const juce::String& patchName, const std::vector<float>& knobs,
                                        int insertIndex = -1);
    static bool isInstrumentTrack (const juce::ValueTree& track) { return track[ids::kind].toString() == kinds::instrument; }
    static bool isAuxTrack (const juce::ValueTree& track)        { return track[ids::kind].toString() == kinds::aux; }
    /** An aux (bus) track: other tracks reach it through sends. */
    juce::ValueTree addAuxTrack (const juce::String& name, int insertIndex = -1);

    // Sends ----------------------------------------------------------------------
    /** A send from `track` to the aux `busId` at `levelDb` (or the existing one, updated). */
    juce::ValueTree addSend (juce::ValueTree track, int busId, double levelDb = 0.0, bool pre = false);
    static juce::Array<juce::ValueTree> sends (const juce::ValueTree& track);

    // Insert effects ------------------------------------------------------------
    /** Adds a pedal from the Registry to the track's inserts (at `index`, or the end). */
    juce::ValueTree addInsert (juce::ValueTree track, const juce::String& pedalType, const std::vector<float>& knobs,
                               int index = -1);
    /** The track's FX children, in signal order. */
    static juce::Array<juce::ValueTree> inserts (const juce::ValueTree& track);

    /** "1,0.5,-3" <-> values. */
    static juce::String encodeValues (const std::vector<float>&);
    static std::vector<float> decodeValues (const juce::String&);
    void removeTrack (const juce::ValueTree& track);
    juce::ValueTree trackList() const                       { return state; }

    // Regions ------------------------------------------------------------------
    juce::ValueTree addRegion (juce::ValueTree track, const juce::String& relativeFile, juce::int64 start,
                               juce::int64 offset, juce::int64 length, const juce::String& name);
    /** Splits at a timeline position; returns the new right-hand region (or invalid if outside). */
    juce::ValueTree splitRegion (juce::ValueTree region, juce::int64 at);
    static juce::ValueTree trackOf (const juce::ValueTree& region) { return region.getParent(); }
    juce::ValueTree regionById (int id) const;

    // Automation -----------------------------------------------------------------
    static juce::ValueTree automationNode (const juce::ValueTree& track, const juce::String& param);
    static std::vector<AutoPoint> getAutomation (const juce::ValueTree& track, const juce::String& param);
    /** Replaces the line (an empty list removes it). One undo step if the caller began a transaction. */
    void setAutomation (juce::ValueTree track, const juce::String& param, std::vector<AutoPoint>);
    /** The value of a line at `time` (points in time order, at least one). */
    static float evaluate (const std::vector<AutoPoint>&, juce::int64 time);
    static juce::String encodePoints (const std::vector<AutoPoint>&);
    static std::vector<AutoPoint> decodePoints (const juce::String&);

    /** Everything on a track that can be automated: param id, display name, range and default. */
    struct AutoParam { juce::String id, name; float min = 0.0f, max = 1.0f, def = 0.0f; juce::String unit; float skew = 1.0f; };
    static std::vector<AutoParam> automatableParams (const juce::ValueTree& track);

    // MIDI regions ---------------------------------------------------------------
    /** An empty MIDI region on an instrument track. */
    juce::ValueTree addMidiRegion (juce::ValueTree track, juce::int64 start, juce::int64 length, const juce::String& name);
    static bool isMidiRegion (const juce::ValueTree& region)   { return region[ids::kind].toString() == kinds::midi; }
    /** Adds a note (ticks from the region's content origin); returns its NOTE. */
    juce::ValueTree addNote (juce::ValueTree region, const Note&);
    static Note noteFrom (const juce::ValueTree& note);
    void setNote (juce::ValueTree note, const Note&);
    /** Timeline sample of the region's content origin (where tick 0 of its notes falls). */
    static juce::int64 contentOrigin (const juce::ValueTree& region)
    {
        return (juce::int64) region[ids::start] - (juce::int64) region[ids::offset];
    }

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
