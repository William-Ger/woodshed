#include "Stacks.h"

namespace woodshed::daw::stacks
{

juce::ValueTree mainOf (const Project& p, const juce::ValueTree& track)
{
    if (! track.hasProperty (ids::stack))
        return {};
    auto main = p.trackById ((int) track[ids::stack]);
    return isMain (main) && main != track ? main : juce::ValueTree();
}

juce::Array<juce::ValueTree> members (const Project& p, const juce::ValueTree& main)
{
    juce::Array<juce::ValueTree> list;
    if (! isMain (main))
        return list;
    for (int i = 0; i < p.numTracks(); ++i)
        if (auto t = p.track (i); t != main && t.hasProperty (ids::stack) && (int) t[ids::stack] == (int) main[ids::id])
            list.add (t);
    return list;
}

juce::Array<juce::ValueTree> descendants (const Project& p, const juce::ValueTree& main)
{
    juce::Array<juce::ValueTree> list;
    for (int i = 0; i < p.numTracks(); ++i)
    {
        auto t = p.track (i);
        // Walk up from each track; a guard stops a (corrupt) cycle.
        auto up = mainOf (p, t);
        for (int guard = 0; up.isValid() && guard < 32; ++guard, up = mainOf (p, up))
            if (up == main)
            {
                list.add (t);
                break;
            }
    }
    return list;
}

int depth (const Project& p, const juce::ValueTree& track)
{
    int d = 0;
    for (auto up = mainOf (p, track); up.isValid() && d < 32; up = mainOf (p, up))
        ++d;
    return d;
}

bool isHidden (const Project& p, const juce::ValueTree& track)
{
    int guard = 0;
    for (auto up = mainOf (p, track); up.isValid() && guard < 32; up = mainOf (p, up), ++guard)
        if ((bool) up[ids::collapsed])
            return true;
    return false;
}

juce::ValueTree create (Project& p, const juce::Array<int>& trackIds, Type type, const juce::String& name)
{
    // The tracks, with any stack main's members coming along, in track order.
    juce::Array<juce::ValueTree> chosen, moving;
    for (int id : trackIds)
        if (auto t = p.trackById (id); t.isValid() && ! chosen.contains (t))
            chosen.add (t);
    if (chosen.isEmpty())
        return {};

    for (int i = 0; i < p.numTracks(); ++i)
    {
        auto t = p.track (i);
        bool in = chosen.contains (t);
        for (auto up = mainOf (p, t); ! in && up.isValid(); up = mainOf (p, up))
            in = chosen.contains (up);
        if (in)
            moving.add (t);
    }

    // The top level of the selection: tracks whose stack isn't itself coming along.
    juce::Array<juce::ValueTree> top;
    for (auto& t : moving)
        if (! moving.contains (mainOf (p, t)))
            top.add (t);

    const auto& first = moving.getReference (0);
    const auto outer = mainOf (p, first);
    const int at = p.state.indexOf (first);

    auto main = p.addAuxTrack (name.isNotEmpty() ? name : type == Type::summing ? juce::String ("Summing Stack")
                                                                                 : juce::String ("Folder Stack"), at);
    main.setProperty (ids::busRole, type == Type::summing ? summingRole : folderRole, &p.undo);
    main.setProperty (ids::colour, first[ids::colour], &p.undo);
    // Nested: the new stack sits where its first track was, inside the same stack if they all were.
    bool sameOuter = true;
    for (auto& t : top)
        sameOuter = sameOuter && mainOf (p, t) == outer;
    if (outer.isValid() && sameOuter)
        main.setProperty (ids::stack, outer[ids::id], &p.undo);

    for (auto& t : top)
        t.setProperty (ids::stack, main[ids::id], &p.undo);

    // Members sit together right under the main, in their old order.
    int target = p.state.indexOf (main) + 1;
    for (auto& t : moving)
    {
        const int from = p.state.indexOf (t);
        if (from != target)
            p.state.moveChild (from, target, &p.undo);
        ++target;
    }
    return main;
}

void unstack (Project& p, const juce::ValueTree& main)
{
    const auto outer = mainOf (p, main);
    for (auto t : members (p, main))
    {
        if (outer.isValid())
            t.setProperty (ids::stack, outer[ids::id], &p.undo);
        else
            t.removeProperty (ids::stack, &p.undo);
    }
}

juce::ValueTree addVca (Project& p, const juce::String& name)
{
    auto t = p.addAuxTrack (name.isNotEmpty() ? name : "VCA " + juce::String (vcas (p).size() + 1));
    t.setProperty (ids::busRole, vcaRole, &p.undo);
    return t;
}

juce::Array<juce::ValueTree> vcas (const Project& p)
{
    juce::Array<juce::ValueTree> list;
    for (int i = 0; i < p.numTracks(); ++i)
        if (isVca (p.track (i)))
            list.add (p.track (i));
    return list;
}

juce::Array<juce::ValueTree> assigned (const Project& p, const juce::ValueTree& vca)
{
    if (isFolder (vca))
        return descendants (p, vca);

    juce::Array<juce::ValueTree> list;
    if (! isVca (vca))
        return list;
    for (int i = 0; i < p.numTracks(); ++i)
        if (auto t = p.track (i); t.hasProperty (ids::vca) && (int) t[ids::vca] == (int) vca[ids::id])
            list.add (t);
    return list;
}

void forget (Project& p, const juce::ValueTree& track)
{
    if (isMain (track))
        unstack (p, track);
    if (isVca (track))
        for (auto t : assigned (p, track))
            t.removeProperty (ids::vca, &p.undo);
}

} // namespace woodshed::daw::stacks
