#pragma once

#include "Project.h"
#include <map>
#include <optional>

namespace woodshed::daw
{

/**
    Track groups: up to 32, each with a name and colour, stored in the project's `groups`
    property (a track names its group in `group`). A group links what its members do: a fader
    move on one moves the others by the same number of dB, and mute, solo, record-arm and
    selection follow along, each switchable per group.
*/
namespace groups
{
    constexpr int maxGroups = 32;

    struct Group
    {
        int id = 1;
        juce::String name;
        juce::Colour colour;
        bool volume = true, mute = true, solo = true, arm = true, select = true;

        /** Whether this group links a track property (volume, mute, solo or arm). */
        bool links (const juce::Identifier&) const;
    };

    std::vector<Group> get (const Project&);
    /** Replaces the list (one undoable property change). */
    void set (Project&, const std::vector<Group>&);
    /** The group with this id, or null. */
    std::optional<Group> find (const Project&, int id);
    /** A new group with the first free id (1..32), or 0 if all 32 are taken. Undoable. */
    int add (Project&, const juce::String& name = {});
    /** Removes the group and takes its tracks out of it. Undoable. */
    void remove (Project&, int id);
    /** Puts a track in a group (0: none). Undoable. */
    void assign (Project&, juce::ValueTree track, int id);
    /** The group's tracks, in track order. */
    juce::Array<juce::ValueTree> members (const Project&, int id);
    inline int groupOf (const juce::ValueTree& track)      { return (int) track.getProperty (ids::group, 0); }

    //==============================================================================
    /**
        Keeps grouped tracks moving together: listens to the project and, when a member's volume,
        mute, solo or arm changes, makes the same change on the rest of its group (the volume by
        the same dB difference), in the same undo transaction. Undo and redo replay each track's
        own change, so nothing is linked while they run.
    */
    class Linker final : private juce::ValueTree::Listener
    {
    public:
        explicit Linker (Project&);
        ~Linker() override;

    private:
        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override             { remember(); }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override      { remember(); }
        void valueTreeRedirected (juce::ValueTree&) override                               { remember(); }
        void remember();

        Project& project;
        juce::ValueTree state;
        std::map<int, double> volumes;      // each track's last known fader, for relative moves
        bool linking = false;
    };
}

} // namespace woodshed::daw
