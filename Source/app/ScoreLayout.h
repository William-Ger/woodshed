#pragma once

#include <juce_core/juce_core.h>
#include <vector>

/**
    Music notation layout for the Score editor, with no UI: MIDI notes in, measures of notated
    elements out (chords and rests with their values, dots, ties, beams, stems and accidentals).

    Everything is in song ticks (960 per quarter note, tick 0 = the start of bar 1), so bars line
    up with the arrange ruler. The notes themselves are never changed: the layout quantizes their
    display to a resolution, splits them at bar lines and beats (tying the pieces), fills the gaps
    with rests and spells each pitch in the chosen key.

    One voice per staff: notes starting together share a stem (a chord), and a note that is still
    sounding when the next one starts is cut short for display.
*/
namespace woodshed::score
{
constexpr int quarter = 960, wholeNote = quarter * 4;

enum class Clef { treble, bass, percussion };
enum class StaffMode { automatic, treble, bass, grand };
enum class Accidental { none, sharp, flat, natural };

/** Diatonic staff position: C4 (MIDI 60, middle C) is step 28, D4 29 ... B4 34, C5 35. */
constexpr int middleCStep = 28;

/** A pitch as written: its staff step and its alteration (-1 flat, 0 natural, +1 sharp). */
struct Spelling
{
    int step = middleCStep, alter = 0;
};

/** The key signature's alteration of a letter (0 = C ... 6 = B) in a key given as fifths (-7..7). */
int keyAlter (int letter, int keyFifths);
/** How `pitch` is written in the key: notes of the scale as the key has them, other notes as
    naturals where possible, else sharps in sharp keys (and C) and flats in flat keys. */
Spelling spell (int pitch, int keyFifths);
/** The pitch of a staff step in the key (with the key signature's alteration). */
int pitchForStep (int step, int keyFifths);
/** The next pitch of the key's scale above (direction +1) or below (-1) `pitch`. */
int scaleStep (int pitch, int keyFifths, int direction);
/** A key's name ("C", "F#", "Bb" ...). */
juce::String keyName (int keyFifths);

/** Where General MIDI drums sit on a percussion staff, and whether they take an x notehead. */
struct DrumPosition
{
    int step = 35;
    bool cross = false;
};
DrumPosition drumPosition (int pitch);
/** The drum written at a staff step (for adding notes on a percussion staff). */
int drumPitchForStep (int step);

/** Staff geometry in steps: the top and bottom lines (five lines, two steps apart). */
int topLineStep (Clef);
inline int bottomLineStep (Clef c)      { return topLineStep (c) - 8; }
inline int middleLineStep (Clef c)      { return topLineStep (c) - 4; }

struct InputNote
{
    int pitch = 60;
    juce::int64 start = 0, length = quarter;      // song ticks
    int id = -1;                                  // the caller's handle, passed through to the noteheads
};

struct Options
{
    int beatsPerBar = 4, beatUnit = 4;
    /** Display quantize in ticks: 960, 480, 240, 120, or the triplets 320 and 160. */
    int resolution = 240;
    int keyFifths = 0;
    StaffMode staff = StaffMode::automatic;
    /** A drum kit: one percussion staff with drum positions and x noteheads. */
    bool percussion = false;
    /** Logic's Interpretation: a short note is shown lengthened to the next note, within its beat,
        instead of being followed by a short rest. */
    bool interpret = true;
    /** The range shown, in song ticks; whole bars are laid out around it. `to` < 0 runs through
        the last note. Notes outside it are left out. */
    juce::int64 from = 0, to = -1;
};

struct Notehead
{
    int pitch = 60, step = middleCStep, alter = 0;
    Accidental accidental = Accidental::none;
    bool cross = false;
    bool tieToNext = false, tiedFromPrevious = false;
    int id = -1;
};

/** One chord or rest on a staff. */
struct Element
{
    int staff = 0, bar = 0;                     // bar: index into Layout::bars
    juce::int64 start = 0, duration = 0;        // song ticks (as displayed)
    int base = quarter;                         // the written value: 3840 whole ... 120 thirty-second
    int dots = 0;
    bool tuplet = false;                        // a triplet: three of `base` in the time of two
    bool rest = false;
    bool barRest = false;                       // a whole-bar rest (a centred whole rest in any meter)
    std::vector<Notehead> notes;                // low to high
    bool stemUp = true;
    int beam = -1;                              // index into Layout::beams

    /** Flags (or beams) the value takes: 1 for eighths, 2 for sixteenths, 3 for thirty-seconds. */
    int flags() const;
    bool hasStem() const                        { return ! rest && base < wholeNote; }
};

struct Beam
{
    int staff = 0;
    std::vector<int> elements;                  // indices into Layout::elements, in time order
    bool stemUp = true;
};

struct Bar
{
    juce::int64 start = 0, length = wholeNote;
    int number = 1;
};

struct Layout
{
    std::vector<Clef> staves;                   // top to bottom
    std::vector<Bar> bars;
    std::vector<Element> elements;              // by staff, then time
    std::vector<Beam> beams;
    int keyFifths = 0, beatsPerBar = 4, beatUnit = 4;
    juce::int64 barLength = wholeNote;

    /** The elements of one bar on one staff, in time order (for tests and drawing). */
    std::vector<const Element*> on (int staff, int barIndex) const;
    /** Index of the bar holding a song tick (clamped to the laid-out bars). */
    int barAt (juce::int64 tick) const;
};

Layout layout (const std::vector<InputNote>&, const Options&);

/** For tests: "q", "8.", "h~" (tied on), "r16", "W" (bar rest), chords as "q[C4 E4 G4]". */
juce::String describe (const Layout&, int staff, int barIndex, bool withPitches = false);
juce::String stepName (const Notehead&);

} // namespace woodshed::score
