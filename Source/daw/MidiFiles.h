#pragma once

#include "Project.h"

namespace woodshed::daw
{

/** Standard MIDI files in and out. */
namespace midifile
{
    /** One part of an imported file: the notes of one channel of one track. */
    struct Part
    {
        juce::String name;
        int channel = 1;                    // 1..16; 10 is drums in General MIDI
        int program = 0;                    // General MIDI program 0..127 (the first one set)
        std::vector<Note> notes;            // ticks (960 per quarter) from the start of the file
        juce::int64 endTicks = 0;           // where the last note ends
    };

    struct Contents
    {
        std::vector<Part> parts;
        double tempo = 0.0;                 // the first tempo, or 0 if the file has none
        std::vector<TimeGrid::TempoChange> tempoChanges;    // later ones (ticks, 960 per quarter)
        int beatsPerBar = 0, beatUnit = 0;  // the first time signature, or 0
    };

    /** Reads a .mid file; empty parts are left out. False if it isn't a MIDI file. */
    bool read (const juce::File&, Contents&);

    /** Writes these MIDI regions (one MIDI track each, at their song positions) with the
        project's tempo and meter. */
    bool write (const Project&, const juce::Array<juce::ValueTree>& regions, const juce::File&);

    /** Adds `part` as a MIDI region on `track` with its first bar at timeline sample `at`. */
    juce::ValueTree addPart (Project&, juce::ValueTree track, const Part&, juce::int64 at);
}

} // namespace woodshed::daw
