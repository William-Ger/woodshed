#pragma once

#include "Project.h"

namespace woodshed::daw
{

/**
    A step-sequencer pattern (Logic's Step Sequencer): rows of notes, columns of equal steps,
    each step off or on at a velocity. The pattern repeats from the region's content origin to
    its end, so the region's notes are always generated from it; the pattern itself is kept on
    the region in the `steps` property (JSON) so it can be edited again.
*/
struct StepPattern
{
    inline static const juce::Identifier property { "steps" };
    static constexpr int maxSteps = 64;

    struct Row
    {
        int pitch = 36;
        bool mute = false, solo = false;
        std::vector<int> velocity;      // one per step; 0 is off
    };

    int numSteps = 16;
    int stepTicks = ticksPerQuarter / 4;
    std::vector<Row> rows;

    juce::int64 lengthTicks() const                 { return (juce::int64) numSteps * stepTicks; }
    bool isOn (int row, int step) const             { return velocityAt (row, step) > 0; }
    int velocityAt (int row, int step) const;
    void set (int row, int step, int velocity);
    /** Changes the number of steps, keeping the ones that remain. */
    void setNumSteps (int);
    void clear();
    bool isEmpty() const;
    /** Whether the row sounds, given every row's mute and solo. */
    bool isAudible (int row) const;

    /** The notes of the pattern repeated from tick 0 up to `endTick`. */
    std::vector<Note> toNotes (juce::int64 endTick) const;
    /** A pattern with these rows, read from notes (each note on its nearest step, within the first
        `numSteps` steps). */
    static StepPattern fromNotes (const std::vector<Note>&, const std::vector<int>& rowPitches, int stepTicks, int numSteps);

    juce::String toJson() const;
    /** False if the text isn't a pattern. */
    static bool fromJson (const juce::String&, StepPattern&);
    bool operator== (const StepPattern&) const;

    /** A musical random pattern from the seed: a groove for drum rows, a phrase in the rows'
        scale otherwise. `density` 0..1. */
    void randomize (juce::uint32 seed, bool drums, float density = 0.5f);

    // Rows ------------------------------------------------------------------------
    /** The kit pieces shown for a drum track, top to bottom. */
    static std::vector<int> drumRows();
    /** "Kick", "Snare", "Closed Hat"... (General MIDI), or the note name. */
    static juce::String rowName (int pitch, bool drums);

    enum class Scale { chromatic, major, minor, pentatonic };
    static juce::StringArray scaleNames();
    /** Rows in this scale from `lowest` up (C-based), top row highest, `count` of them. */
    static std::vector<int> scaleRows (Scale, int lowest, int count);
    /** Keeps each row's steps where the pitch is still a row. */
    void setRows (const std::vector<int>& pitches);

    /** Writes the pattern onto a MIDI region: notes from the content origin to the region's end,
        and the JSON. `undo` may be nullptr (drag previews). */
    void applyTo (Project&, juce::ValueTree region, juce::UndoManager* undo) const;
};

} // namespace woodshed::daw
