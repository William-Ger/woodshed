#pragma once

#include "Studio.h"
#include "../flex/Analysis.h"

namespace woodshed::app::flexui
{

/**
    Time Stretch, Pitch Edit and Tempo Detect in the menus: the commands (cmd block 0x3100),
    what MainComponent asks about them, and what happens when audio is imported.

    Settings: "importTempo" ("ask" (default), "always" or "never"): when audio goes into an
    empty song, whether it takes the file's tempo; "conformImports" (bool, on by default): in a
    song that already has a tempo, a file with a clear beat is conformed to it (it follows the
    tempo from then on).
*/
void getAllCommands (juce::Array<juce::CommandID>&);
/** False if the command isn't one of these. */
bool getCommandInfo (Studio&, juce::CommandID, juce::ApplicationCommandInfo&);
bool perform (Studio&, juce::CommandID);
/** The Time Stretch / Tempo menu items, for the Edit menu. */
void addMenuItems (juce::PopupMenu&, juce::ApplicationCommandManager*);

/** The audio regions among the selection (or the selected track's region under the playhead). */
juce::Array<juce::ValueTree> selectedAudioRegions (Studio&);
/** The tempo found in a region's file (cached). */
std::shared_ptr<const woodshed::flex::TempoEstimate> tempoOf (Studio&, const juce::ValueTree& region);

/** After files were dropped into the song: offers the file's tempo to an empty song, or
    conforms the region to the song's tempo, per the settings. */
void audioImported (Studio&, const juce::Array<int>& regionIds, bool songWasEmpty);

} // namespace woodshed::app::flexui
