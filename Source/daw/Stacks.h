#pragma once

#include "Project.h"

namespace woodshed::daw
{

/**
    Track Stacks and VCAs. The project's track list stays flat: a stack is a stack main (an aux
    with busRole "summing" or "folder") followed by its members, which name it in their `stack`
    property. A VCA is an aux with busRole "vca"; the tracks it scales name it in `vca`.

    A summing stack's members play through the main (its inserts, fader and automation) on their
    way to the Stereo Out. A folder stack only groups: the members go straight out, and the main's
    fader (a dB offset) and mute scale them, like a VCA.
*/
namespace stacks
{
    enum class Type { folder, summing };

    inline const juce::String summingRole { "summing" }, folderRole { "folder" }, vcaRole { "vca" };

    inline bool isMain (const juce::ValueTree& t)
    {
        const auto role = t[ids::busRole].toString();
        return Project::isAuxTrack (t) && (role == summingRole || role == folderRole);
    }
    inline bool isSumming (const juce::ValueTree& t)        { return Project::isAuxTrack (t) && t[ids::busRole].toString() == summingRole; }
    inline bool isFolder (const juce::ValueTree& t)         { return Project::isAuxTrack (t) && t[ids::busRole].toString() == folderRole; }
    inline bool isVca (const juce::ValueTree& t)            { return Project::isAuxTrack (t) && t[ids::busRole].toString() == vcaRole; }
    /** An aux that sends can reach (not a stack main or a VCA). */
    inline bool isSendTarget (const juce::ValueTree& t)     { return Project::isAuxTrack (t) && t[ids::busRole].toString().isEmpty(); }

    /** The stack main this track is in, or invalid. */
    juce::ValueTree mainOf (const Project&, const juce::ValueTree& track);
    /** The main's direct members, in track order. */
    juce::Array<juce::ValueTree> members (const Project&, const juce::ValueTree& main);
    /** Members, their members and so on, in track order. */
    juce::Array<juce::ValueTree> descendants (const Project&, const juce::ValueTree& main);
    /** How many stacks this track is nested in (0 for a track in none). */
    int depth (const Project&, const juce::ValueTree& track);
    /** True if a stack it is in (at any level) is collapsed, so the arrange hides it. */
    bool isHidden (const Project&, const juce::ValueTree& track);

    /** A new stack main holding these tracks (and, for a main among them, its members), moved
        together right under it at the first one's place. Through the undo manager: begin a
        transaction first. Returns the main. */
    juce::ValueTree create (Project&, const juce::Array<int>& trackIds, Type, const juce::String& name = {});
    /** Takes every member out of the stack (they stay where they are). Undoable. */
    void unstack (Project&, const juce::ValueTree& main);

    // VCAs --------------------------------------------------------------------
    /** A new VCA (named "VCA N") after the other tracks. Undoable. */
    juce::ValueTree addVca (Project&, const juce::String& name = {});
    /** The VCAs, in track order. */
    juce::Array<juce::ValueTree> vcas (const Project&);
    /** The tracks a VCA (or a folder main) scales. */
    juce::Array<juce::ValueTree> assigned (const Project&, const juce::ValueTree& vca);

    /** Removes everything that points at `track` (members' stack, tracks' vca), before the track
        itself goes. Undoable. */
    void forget (Project&, const juce::ValueTree& track);
}

} // namespace woodshed::daw
