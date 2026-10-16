#pragma once

#include "Project.h"

namespace woodshed::daw
{

/** A region's notes in time order (ticks from its content origin). */
std::vector<Note> regionNotes (const juce::ValueTree& region);

/** Replaces every NOTE of the region with these (in time order). `undo` may be nullptr, for
    previews during a drag that are put back before the one undoable write at the end. */
void replaceRegionNotes (juce::ValueTree region, const std::vector<Note>&, juce::UndoManager* undo);

/**
    A region's content before a drag started, so the drag can preview its edits without undo and
    make them one undo step at the end: restore() puts the region back exactly (without undo),
    then the caller writes the final result with undo inside its own transaction.
*/
struct RegionSnapshot
{
    juce::ValueTree region;
    std::vector<Note> notes;
    juce::NamedValueSet properties;

    void take (const juce::ValueTree& r);
    bool isActive() const                       { return region.isValid(); }
    void restore();
    void clear()                                { region = {}; notes.clear(); properties.clear(); }
};

/**
    Drummer: a pattern generator in the spirit of Logic's Drummer. Given a style, how busy and
    how hard to play, swing, fills and which kit pieces to use, it writes a groove for N bars
    as General MIDI drum notes (36 kick, 38 snare, 42 closed hat...).

    Grooves are written by hand per style as one bar of 4/4 (16 sixteenths, or 12 triplet
    eighths for swung styles) at three levels of complexity, and adapted to the meter: a bar is
    split into pulses (beats of x/4 and x/2; groups of two and three eighths in x/8, so 6/8 and
    12/8 get dotted-quarter pulses with a triplet feel) and each pulse plays one beat of the
    source bar. Ghost notes, kick pickups, fills and humanizing come from a seeded generator,
    so the same settings and seed always give the same notes.

    A Drummer region is an ordinary MIDI region with its settings in the `drummer` property
    (JSON), so the piano roll can still edit it; changing the settings writes it again.
*/
namespace drummer
{
    inline const juce::Identifier property { "drummer" };

    enum class Style
    {
        rock, pop, funk, hipHop, trap, house, disco, reggae, jazz, bossaNova, metal, shuffle, punk, ballad, afrobeat, lofi
    };
    constexpr int numStyles = 16;
    juce::String styleName (Style);
    /** The style with this name (case-insensitive), or rock. */
    Style styleFromName (const juce::String&);

    /** Which cymbal keeps time: the style's own choice, or forced to the hi-hat or ride. */
    enum class Timekeeper { automatic, hiHat, ride };

    struct Settings
    {
        Style style = Style::rock;
        float complexity = 0.4f;    // 0 simple .. 1 complex
        float loudness = 0.6f;      // 0 soft .. 1 loud
        float swing = 0.0f;         // 0 straight .. 1 full triplet swing (8ths or 16ths, by style)
        float fill = 0.5f;          // 0 none; more: longer and more frequent fills
        float humanize = 0.5f;      // timing and velocity looseness
        bool kickSnare = true, hiHat = true, cymbals = true, toms = true, percussion = false;
        Timekeeper timekeeper = Timekeeper::automatic;
        int bars = 8;
        juce::uint32 seed = 1;

        juce::String toJson() const;
        static Settings fromJson (const juce::String&);
        bool operator== (const Settings&) const;
    };

    /** The style's own swing and whether it plays percussion by default (for picking a style). */
    Settings withStyle (Settings, Style);

    struct Meter
    {
        int beatsPerBar = 4, beatUnit = 4;
        juce::int64 barTicks() const        { return (juce::int64) beatsPerBar * ticksPerQuarter * 4 / juce::jmax (1, beatUnit); }
    };

    /** Bars per phrase (fills go in the last bar of each), or 0 when fills are off. */
    int phraseBars (const Settings&);
    bool isFillBar (const Settings&, int bar);

    /** The groove for settings.bars bars from tick 0, in time order. */
    std::vector<Note> generate (const Settings&, const Meter&);

    /** Writes the settings onto a MIDI region: notes generated from its content origin to its
        end (its length is left alone, so split and trimmed regions stay put), and `drummer` holds
        the JSON, with `bars` set to the region's length. Clears any step pattern. `undo` may be
        nullptr (drag previews). */
    void apply (Project&, juce::ValueTree region, const Settings&, juce::UndoManager* undo);

    bool isDrummerRegion (const juce::ValueTree& region);
    Settings settingsOf (const juce::ValueTree& region);

    /** GM names for the kit pieces the generator uses (for tests and the UI). */
    bool isGeneralMidiDrum (int pitch);
}

} // namespace woodshed::daw
