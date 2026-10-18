#pragma once

#include "Studio.h"
#include "../daw/Stacks.h"
#include "Icons.h"

namespace woodshed::app::mixui
{

/** The studio's mixing and recording commands: Track Stacks, groups and VCAs, freeze and
    Autopunch, with the menus and small drawings the views share. */

// Commands (cmd::createTrackStack ...): MainComponent forwards its command calls here.
void getCommands (juce::Array<juce::CommandID>&);
/** False if `id` isn't one of these. */
bool getCommandInfo (Studio&, juce::CommandID, juce::ApplicationCommandInfo&);
bool perform (Studio&, juce::CommandID);

/** A stack of the selected tracks (one undo step); selects its main. */
void createTrackStack (Studio&, daw::stacks::Type);
/** "Create Track Stack...": the choice of a folder or a summing stack, at the mouse. */
void showCreateStackMenu (Studio&);
/** Freezes the track (rendering it in the background), or unfreezes it. */
void toggleFreeze (Studio&, juce::ValueTree track);
/** Turns Autopunch on or off; with no punch range yet it takes the cycle's, or two bars at the playhead. */
void toggleAutopunch (Studio&);
/** Sets the punch locators to the cycle range. */
void punchToCycle (Studio&);

/** The track's group (and its links) and VCA, for the header badge, the mixer and the track menu. */
juce::PopupMenu groupMenu (Studio&, juce::ValueTree track);
/** Track Stack, group, VCA and freeze items for the arrange's track menu. */
void addTrackMenuItems (juce::PopupMenu&, Studio&, juce::ValueTree track);

/** True (with a note in the status line) if any of these regions sits on a frozen track: a
    frozen track's regions can't be edited until it's unfrozen. */
bool blockedByFreeze (Studio&, const juce::Array<int>& regionIds);
bool blockedByFreeze (Studio&, const juce::ValueTree& track);

// Drawing ---------------------------------------------------------------------------
/** Autopunch's toolbar icon: a range between two locators, with a record dot in it. */
icons::Icon punchIcon();
juce::Colour freezeColour();
juce::Colour punchColour();
void drawSnowflake (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
/** A stack main's disclosure triangle (pointing down when open); a summing stack's is filled. */
void drawDisclosure (juce::Graphics&, juce::Rectangle<float>, bool open, bool summing, juce::Colour);

} // namespace woodshed::app::mixui
