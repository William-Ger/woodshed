#pragma once

#include "../daw/Project.h"

namespace woodshed::loops
{

/**
    The Loop Grid's part of the song: a performance grid of cells (loops) to launch live, rows
    for the tracks and columns for the scenes.

    PROJECT  loopScenes (JSON array of scene names; its length is the number of columns, 8 when
             absent), loopQuantize (int, a Quantize: where launches land, a bar when absent).
             Properties, so PROJECT's children stay the TRACKs then MASTER.
      TRACK
        LOOPGRID  the track's cells (any number, at most one per column)
          CELL    column (0-based scene index), bars (length of one pass), playMode ("loop" or
                  "oneshot"), quantize (int, a Quantize for this cell; absent: the grid's)
            REGION  what the cell plays, with the arrange's REGION schema (audio with file, or
                    kind "midi" with NOTE children), placed from the cell's start: start is
                    usually 0, offset and length as on the arrange, and only the part inside
                    the cell's bars plays. Cell regions are not the arrange's: nothing that
                    walks a track's REGIONs reaches them.

    A cell plays at the song's starting tempo and meter (bars * Project::grid().samplesPerBar()).
*/
namespace ids
{
#define WOODSHED_LOOP_ID(name) inline const juce::Identifier name { #name };
    WOODSHED_LOOP_ID (LOOPGRID) WOODSHED_LOOP_ID (CELL) WOODSHED_LOOP_ID (column) WOODSHED_LOOP_ID (bars)
    WOODSHED_LOOP_ID (playMode) WOODSHED_LOOP_ID (quantize) WOODSHED_LOOP_ID (loopScenes) WOODSHED_LOOP_ID (loopQuantize)
#undef WOODSHED_LOOP_ID
}

/** Where a launch (or stop) lands: at once, on the next beat, or on the next 1, 2, 4 or 8-bar line. */
enum class Quantize : int { none, quarter, bar, twoBars, fourBars, eightBars };
constexpr int numQuantizeChoices = 6;
juce::String quantizeName (Quantize);

/** The first launch point at or after `position` (timeline samples), on the song's tempo map.
    Multi-bar choices count from bar 1, so 4 bars lands on bars 1, 5, 9... */
juce::int64 nextBoundary (const daw::TimeGrid&, juce::int64 position, Quantize);

constexpr int defaultScenes = 8, maxScenes = 64, maxCellBars = 64;

// Scenes --------------------------------------------------------------------------
juce::StringArray sceneNames (const juce::ValueTree& projectState);
int numScenes (const juce::ValueTree& projectState);
void setSceneNames (daw::Project&, const juce::StringArray&);
/** A new column at the end, named "Scene N". */
void addScene (daw::Project&);
/** Removes a column and its cells; the columns after it move left. */
void removeScene (daw::Project&, int column);

Quantize gridQuantize (const juce::ValueTree& projectState);
void setGridQuantize (daw::Project&, Quantize);

// Cells ---------------------------------------------------------------------------
/** The track's LOOPGRID, made (through the undo manager) when `create` is set. */
juce::ValueTree gridOf (daw::Project&, juce::ValueTree track, bool create);
juce::ValueTree gridOf (const juce::ValueTree& track);
juce::ValueTree cellAt (const juce::ValueTree& track, int column);
/** The REGION a cell plays (invalid for an empty cell). */
juce::ValueTree contentOf (const juce::ValueTree& cell);
bool isMidiCell (const juce::ValueTree& cell);
bool isOneShot (const juce::ValueTree& cell);
int barsOf (const juce::ValueTree& cell);
/** The cell's own quantize, else the grid's. */
Quantize quantizeOf (const juce::ValueTree& cell, const juce::ValueTree& projectState);
/** One pass of the cell, in samples. */
juce::int64 cellLength (const daw::TimeGrid&, const juce::ValueTree& cell);
juce::ValueTree trackOfCell (const juce::ValueTree& cell);

/** Puts a copy of an arrange region into the cell at `column` (replacing what was there), as long as the
    region in whole bars. Returns the CELL. One undo step if the caller began a transaction. */
juce::ValueTree copyRegionToCell (daw::Project&, const juce::ValueTree& region, juce::ValueTree track, int column);
/** An empty MIDI cell of `bars` bars. */
juce::ValueTree addMidiCell (daw::Project&, juce::ValueTree track, int column, int bars);
/** An audio cell playing `file` (relative to the project folder) from `offset` for `length` samples. */
juce::ValueTree addAudioCell (daw::Project&, juce::ValueTree track, int column, const juce::String& file,
                              juce::int64 offset, juce::int64 length, int bars);
void removeCell (daw::Project&, juce::ValueTree cell);
/** Moves (or copies) a cell to another track's column; an audio cell only goes to an audio
    track and a MIDI cell to an instrument track. Returns the cell at the target. */
juce::ValueTree moveCell (daw::Project&, juce::ValueTree cell, juce::ValueTree toTrack, int column, bool copy);

/** The cell played over [from, to) as arrange regions on its track (or `onTrack`, of the same
    kind): one per pass (the last trimmed), so the arrange plays what the grid played. Returns
    the new regions' ids. */
juce::Array<int> placeInArrange (daw::Project&, const juce::ValueTree& cell, juce::int64 from, juce::int64 to,
                                 juce::ValueTree onTrack = {});

/** The first column with no cell on the track, or -1. */
int firstEmptyColumn (const juce::ValueTree& track, const juce::ValueTree& projectState);

/** A REGION inside some track's grid, by id (Project::regionById only sees the arrange). */
juce::ValueTree findCellRegion (const daw::Project&, int regionId);

} // namespace woodshed::loops
