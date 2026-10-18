#include "Groups.h"

namespace woodshed::daw::groups
{

namespace
{
juce::Colour defaultColour (int id)
{
    static const juce::uint32 colours[] { 0xff4fa3c7, 0xffd9893b, 0xff8bbf5a, 0xffc75b7a, 0xff9a7bd6, 0xffd6c04f, 0xff4fc7a6, 0xffd66b4f };
    return juce::Colour (colours[(size_t) (id - 1) % std::size (colours)]);
}

constexpr double minDb = -100.0, maxDb = 6.0;     // the fader's range
}

bool Group::links (const juce::Identifier& property) const
{
    if (property == ids::volume) return volume;
    if (property == ids::mute)   return mute;
    if (property == ids::solo)   return solo;
    if (property == ids::arm)    return arm;
    return false;
}

std::vector<Group> get (const Project& p)
{
    std::vector<Group> list;
    const auto parsed = juce::JSON::parse (p.state[ids::groups].toString());
    if (auto* array = parsed.getArray())
        for (auto& v : *array)
        {
            Group g;
            g.id = (int) v.getProperty ("id", 0);
            if (g.id < 1 || g.id > maxGroups)
                continue;
            g.name = v.getProperty ("name", "Group " + juce::String (g.id)).toString();
            g.colour = v.hasProperty ("colour") ? juce::Colour ((juce::uint32) (juce::int64) v.getProperty ("colour", 0)) : defaultColour (g.id);
            g.volume = (bool) v.getProperty ("volume", true);
            g.mute = (bool) v.getProperty ("mute", true);
            g.solo = (bool) v.getProperty ("solo", true);
            g.arm = (bool) v.getProperty ("arm", true);
            g.select = (bool) v.getProperty ("select", true);
            list.push_back (g);
        }
    std::sort (list.begin(), list.end(), [] (auto& a, auto& b) { return a.id < b.id; });
    return list;
}

void set (Project& p, const std::vector<Group>& list)
{
    juce::Array<juce::var> array;
    for (auto& g : list)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", g.id);
        o->setProperty ("name", g.name);
        o->setProperty ("colour", (juce::int64) g.colour.getARGB());
        o->setProperty ("volume", g.volume);
        o->setProperty ("mute", g.mute);
        o->setProperty ("solo", g.solo);
        o->setProperty ("arm", g.arm);
        o->setProperty ("select", g.select);
        array.add (juce::var (o));
    }
    if (array.isEmpty())
        p.state.removeProperty (ids::groups, &p.undo);
    else
        p.state.setProperty (ids::groups, juce::JSON::toString (array, true), &p.undo);
}

std::optional<Group> find (const Project& p, int id)
{
    for (auto& g : get (p))
        if (g.id == id)
            return g;
    return std::nullopt;
}

int add (Project& p, const juce::String& name)
{
    auto list = get (p);
    for (int id = 1; id <= maxGroups; ++id)
        if (std::none_of (list.begin(), list.end(), [id] (auto& g) { return g.id == id; }))
        {
            Group g;
            g.id = id;
            g.name = name.isNotEmpty() ? name : "Group " + juce::String (id);
            g.colour = defaultColour (id);
            list.push_back (g);
            set (p, list);
            return id;
        }
    return 0;
}

void remove (Project& p, int id)
{
    for (auto t : members (p, id))
        t.removeProperty (ids::group, &p.undo);
    auto list = get (p);
    list.erase (std::remove_if (list.begin(), list.end(), [id] (auto& g) { return g.id == id; }), list.end());
    set (p, list);
}

void assign (Project& p, juce::ValueTree track, int id)
{
    if (id <= 0)
        track.removeProperty (ids::group, &p.undo);
    else
        track.setProperty (ids::group, id, &p.undo);
}

juce::Array<juce::ValueTree> members (const Project& p, int id)
{
    juce::Array<juce::ValueTree> list;
    if (id <= 0)
        return list;
    for (int i = 0; i < p.numTracks(); ++i)
        if (groupOf (p.track (i)) == id)
            list.add (p.track (i));
    return list;
}

//==============================================================================
Linker::Linker (Project& p) : project (p), state (p.state)
{
    state.addListener (this);
    remember();
}

Linker::~Linker()
{
    state.removeListener (this);
}

void Linker::remember()
{
    volumes.clear();
    for (int i = 0; i < project.numTracks(); ++i)
    {
        auto t = project.track (i);
        volumes[(int) t[ids::id]] = (double) t.getProperty (ids::volume, 0.0);
    }
}

void Linker::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! tree.hasType (ids::TRACK))
        return;

    const int id = (int) tree[ids::id];
    const double was = volumes.count (id) > 0 ? volumes[id] : 0.0;
    if (property == ids::volume)
        volumes[id] = (double) tree.getProperty (ids::volume, 0.0);

    if (linking || project.undo.isPerformingUndoRedo())
        return;
    if (property != ids::volume && property != ids::mute && property != ids::solo && property != ids::arm)
        return;

    const int groupId = groupOf (tree);
    const auto group = groupId > 0 ? find (project, groupId) : std::nullopt;
    if (! group.has_value() || ! group->links (property))
        return;

    const juce::ScopedValueSetter<bool> busy (linking, true);
    const auto value = tree[property];
    const double delta = (double) tree.getProperty (ids::volume, 0.0) - was;

    for (auto other : members (project, groupId))
    {
        if (other == tree)
            continue;
        if (property == ids::volume)
        {
            if (std::abs (delta) < 1.0e-9)
                continue;
            const double now = (double) other.getProperty (ids::volume, 0.0);
            const double next = juce::jlimit (minDb, maxDb, now + delta);
            other.setProperty (ids::volume, next, &project.undo);
            volumes[(int) other[ids::id]] = next;
        }
        else if (other[property] != value)
            other.setProperty (property, value, &project.undo);
    }
}

} // namespace woodshed::daw::groups
