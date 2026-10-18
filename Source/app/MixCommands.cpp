#include "MixCommands.h"
#include "../daw/Groups.h"

namespace woodshed::app::mixui
{

namespace ids = daw::ids;

//==============================================================================
void getCommands (juce::Array<juce::CommandID>& c)
{
    c.addArray ({ cmd::createTrackStack, cmd::freezeTrack, cmd::newVca, cmd::toggleAutopunch, cmd::punchToCycle });
}

bool getCommandInfo (Studio& studio, juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    const auto cmdKey = juce::ModifierKeys::commandModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    const auto& s = studio.project().state;
    const auto selected = studio.getSelectedTrack();

    switch (id)
    {
        case cmd::createTrackStack:
            info.setInfo ("Create Track Stack...", "Puts the selected tracks in a folder or summing stack", "Track", 0);
            info.addDefaultKeypress ('d', cmdKey | shift);
            info.setActive (selected.isValid());
            return true;
        case cmd::freezeTrack:
            info.setInfo ("Freeze Track", "Renders the track and plays the render instead, for none of its CPU", "Track", 0);
            info.setTicked ((bool) selected[ids::frozen]);
            info.setActive (selected.isValid() && ! daw::Project::isAuxTrack (selected) && ! studio.engine().isBouncing());
            return true;
        case cmd::newVca:
            info.setInfo ("New VCA", "A fader that scales the tracks assigned to it", "Track", 0);
            return true;
        case cmd::toggleAutopunch:
            info.setInfo ("Autopunch", "Record only between the punch locators", "Transport", 0);
            info.addDefaultKeypress ('p', juce::ModifierKeys::altModifier);
            info.setTicked ((bool) s[ids::punchOn]);
            return true;
        case cmd::punchToCycle:
            info.setInfo ("Set Punch Locators to Cycle", {}, "Transport", 0);
            info.setActive ((juce::int64) s[ids::loopEnd] > (juce::int64) s[ids::loopStart]);
            return true;
        default:
            return false;
    }
}

bool perform (Studio& studio, juce::CommandID id)
{
    switch (id)
    {
        case cmd::createTrackStack: showCreateStackMenu (studio); return true;
        case cmd::freezeTrack:      toggleFreeze (studio, studio.getSelectedTrack()); return true;
        case cmd::newVca:
        {
            auto& p = studio.project();
            p.undo.beginNewTransaction ("New VCA");
            studio.selectTrack ((int) daw::stacks::addVca (p)[ids::id]);
            return true;
        }
        case cmd::toggleAutopunch:  toggleAutopunch (studio); return true;
        case cmd::punchToCycle:     punchToCycle (studio); return true;
        default:                    return false;
    }
}

//==============================================================================
void createTrackStack (Studio& studio, daw::stacks::Type type)
{
    const auto chosen = studio.getSelectedTrackIds();
    if (chosen.isEmpty())
        return;
    auto& p = studio.project();
    p.undo.beginNewTransaction ("Create Track Stack");
    if (auto main = daw::stacks::create (p, chosen, type); main.isValid())
        studio.selectTrack ((int) main[ids::id]);
}

void showCreateStackMenu (Studio& studio)
{
    juce::PopupMenu m;
    m.addSectionHeader ("Create Track Stack");
    m.addItem ("Folder Stack", [&studio] { createTrackStack (studio, daw::stacks::Type::folder); });
    m.addItem ("Summing Stack", [&studio] { createTrackStack (studio, daw::stacks::Type::summing); });
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void toggleFreeze (Studio& studio, juce::ValueTree track)
{
    if (! track.isValid() || daw::Project::isAuxTrack (track))
        return;
    auto& engine = studio.engine();
    if ((bool) track[ids::frozen])
    {
        engine.unfreezeTrack (track);
        studio.flash ("Unfrozen");
        return;
    }
    if (engine.isBouncing())
    {
        studio.flash ("Wait for the bounce to finish");
        return;
    }
    studio.flash ("Freezing " + track[ids::name].toString() + "...");
    engine.freezeTrack (track, [&studio] (bool ok) { studio.flash (ok ? "Frozen" : "Couldn't freeze the track"); });
}

void toggleAutopunch (Studio& studio)
{
    auto& p = studio.project();
    auto& s = p.state;
    p.undo.beginNewTransaction ("Autopunch");
    if (! (bool) s[ids::punchOn] && (juce::int64) s[ids::punchOut] <= (juce::int64) s[ids::punchIn])
    {
        // No range yet: the cycle's, or two bars from the playhead's bar.
        juce::int64 from = s[ids::loopStart], to = s[ids::loopEnd];
        if (to <= from)
        {
            const auto& grid = p.grid();
            const int bar = (int) std::floor (grid.barAt (grid.tickAt ((double) studio.engine().getPosition())));
            from = grid.barStartSample (bar);
            to = grid.barStartSample (bar + 2);
        }
        s.setProperty (ids::punchIn, from, &p.undo);
        s.setProperty (ids::punchOut, to, &p.undo);
    }
    s.setProperty (ids::punchOn, ! (bool) s[ids::punchOn], &p.undo);
}

void punchToCycle (Studio& studio)
{
    auto& p = studio.project();
    auto& s = p.state;
    if ((juce::int64) s[ids::loopEnd] <= (juce::int64) s[ids::loopStart])
        return;
    p.undo.beginNewTransaction ("Set Punch Locators");
    s.setProperty (ids::punchIn, s[ids::loopStart], &p.undo);
    s.setProperty (ids::punchOut, s[ids::loopEnd], &p.undo);
    s.setProperty (ids::punchOn, true, &p.undo);
}

//==============================================================================
juce::PopupMenu groupMenu (Studio& studio, juce::ValueTree track)
{
    // The change goes to every selected track when this one is among them.
    const int trackId = (int) track[ids::id];
    auto targets = [&studio, trackId]
    {
        juce::Array<juce::ValueTree> list;
        auto& p = studio.project();
        const auto chosen = studio.isTrackSelected (trackId) ? studio.getSelectedTrackIds() : juce::Array<int> { trackId };
        for (int id : chosen)
            if (auto t = p.trackById (id); t.isValid() && ! daw::stacks::isVca (t))
                list.add (t);
        return list;
    };
    auto assign = [&studio, targets] (int groupId)
    {
        return [&studio, targets, groupId]
        {
            auto& p = studio.project();
            p.undo.beginNewTransaction ("Track Group");
            for (auto& t : targets())
                daw::groups::assign (p, t, groupId);
        };
    };

    auto& p = studio.project();
    const int current = daw::groups::groupOf (track);
    const auto list = daw::groups::get (p);

    juce::PopupMenu m;
    m.addSectionHeader ("Group");
    m.addItem ("No Group", true, current == 0, assign (0));
    for (auto& g : list)
        m.addItem (juce::String (g.id) + "  " + g.name, true, g.id == current, assign (g.id));
    m.addItem ("New Group", (int) list.size() < daw::groups::maxGroups, false, [&studio, targets]
    {
        auto& pr = studio.project();
        pr.undo.beginNewTransaction ("New Group");
        if (const int id = daw::groups::add (pr); id > 0)
            for (auto& t : targets())
                daw::groups::assign (pr, t, id);
    });

    if (auto group = daw::groups::find (p, current))
    {
        juce::PopupMenu links;
        auto toggle = [&studio, current] (bool daw::groups::Group::* flag)
        {
            return [&studio, current, flag]
            {
                auto& pr = studio.project();
                auto groups = daw::groups::get (pr);
                for (auto& g : groups)
                    if (g.id == current)
                        g.*flag = ! (g.*flag);
                pr.undo.beginNewTransaction ("Group Settings");
                daw::groups::set (pr, groups);
            };
        };
        links.addItem ("Volume", true, group->volume, toggle (&daw::groups::Group::volume));
        links.addItem ("Mute", true, group->mute, toggle (&daw::groups::Group::mute));
        links.addItem ("Solo", true, group->solo, toggle (&daw::groups::Group::solo));
        links.addItem ("Record", true, group->arm, toggle (&daw::groups::Group::arm));
        links.addItem ("Selection", true, group->select, toggle (&daw::groups::Group::select));
        m.addSubMenu ("Link", links);
        m.addItem ("Delete Group", [&studio, current]
        {
            auto& pr = studio.project();
            pr.undo.beginNewTransaction ("Delete Group");
            daw::groups::remove (pr, current);
        });
    }

    if (! daw::stacks::isVca (track))
    {
        auto assignVca = [&studio, targets] (int vcaId)
        {
            return [&studio, targets, vcaId]
            {
                auto& pr = studio.project();
                pr.undo.beginNewTransaction ("Assign VCA");
                for (auto& t : targets())
                    if (vcaId > 0) t.setProperty (ids::vca, vcaId, &pr.undo);
                    else           t.removeProperty (ids::vca, &pr.undo);
            };
        };
        const int currentVca = (int) track.getProperty (ids::vca, -1);
        juce::PopupMenu vcas;
        vcas.addItem ("None", true, currentVca < 0, assignVca (-1));
        for (auto& v : daw::stacks::vcas (p))
            vcas.addItem (v[ids::name].toString(), true, (int) v[ids::id] == currentVca, assignVca ((int) v[ids::id]));
        vcas.addItem ("New VCA", [&studio, targets]
        {
            auto& pr = studio.project();
            pr.undo.beginNewTransaction ("New VCA");
            const auto vca = daw::stacks::addVca (pr);
            for (auto& t : targets())
                t.setProperty (ids::vca, vca[ids::id], &pr.undo);
        });
        m.addSeparator();
        m.addSubMenu ("VCA", vcas);
    }
    return m;
}

void addTrackMenuItems (juce::PopupMenu& m, Studio& studio, juce::ValueTree track)
{
    m.addSeparator();
    m.addItem ("Create Track Stack...", [&studio] { showCreateStackMenu (studio); });
    if (daw::stacks::isMain (track))
    {
        const int id = (int) track[ids::id];
        m.addItem (track[ids::collapsed] ? "Expand Stack" : "Collapse Stack", [&studio, id]
        {
            if (auto t = studio.project().trackById (id); t.isValid())
                t.setProperty (ids::collapsed, ! (bool) t[ids::collapsed], nullptr);
        });
        m.addItem ("Flatten Stack", [&studio, id]
        {
            auto& p = studio.project();
            if (auto t = p.trackById (id); t.isValid())
            {
                p.undo.beginNewTransaction ("Flatten Stack");
                p.removeTrack (t);      // its members stay, out of the stack
            }
        });
    }
    m.addSubMenu ("Group", groupMenu (studio, track));
    if (! daw::Project::isAuxTrack (track))
        m.addItem ((bool) track[ids::frozen] ? "Unfreeze" : "Freeze", ! studio.engine().isBouncing(), (bool) track[ids::frozen],
                   [&studio, track] { toggleFreeze (studio, track); });
}

//==============================================================================
bool blockedByFreeze (Studio& studio, const juce::ValueTree& track)
{
    if (! (bool) track[ids::frozen])
        return false;
    studio.flash (track[ids::name].toString() + " is frozen: unfreeze it to edit");
    return true;
}

bool blockedByFreeze (Studio& studio, const juce::Array<int>& regionIds)
{
    for (int id : regionIds)
    {
        auto r = studio.findRegion (id);
        // A take inside a folder: its track is two levels up.
        auto track = r.getParent();
        while (track.isValid() && ! track.hasType (ids::TRACK))
            track = track.getParent();
        if (blockedByFreeze (studio, track))
            return true;
    }
    return false;
}

//==============================================================================
icons::Icon punchIcon()
{
    // [ • ]: the locators as brackets, the record dot between them.
    juce::Path p;
    p.startNewSubPath (7.5f, 6.5f);  p.lineTo (5.0f, 6.5f);  p.lineTo (5.0f, 17.5f); p.lineTo (7.5f, 17.5f);
    p.startNewSubPath (16.5f, 6.5f); p.lineTo (19.0f, 6.5f); p.lineTo (19.0f, 17.5f); p.lineTo (16.5f, 17.5f);
    p.addEllipse (9.0f, 9.0f, 6.0f, 6.0f);
    return { p, icons::Style::stroke };
}

juce::Colour freezeColour()       { return juce::Colour (0xff7fc8f8); }
juce::Colour punchColour()        { return juce::Colour (0xffff453a); }

void drawSnowflake (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    // Three crossing arms with little forks at their ends.
    const auto c = r.getCentre();
    const float len = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
    juce::Path p;
    for (int i = 0; i < 3; ++i)
    {
        const float a = juce::MathConstants<float>::pi * (float) i / 3.0f + juce::MathConstants<float>::halfPi;
        const juce::Point<float> dir (std::cos (a), std::sin (a));
        p.startNewSubPath (c - dir * len);
        p.lineTo (c + dir * len);
        for (float end : { -1.0f, 1.0f })
        {
            const auto tip = c + dir * (len * 0.62f * end);
            const juce::Point<float> side (-dir.y, dir.x);
            p.startNewSubPath (tip + side * (len * 0.28f) + dir * (len * 0.25f * end));
            p.lineTo (tip);
            p.lineTo (tip - side * (len * 0.28f) + dir * (len * 0.25f * end));
        }
    }
    g.setColour (colour);
    g.strokePath (p, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void drawDisclosure (juce::Graphics& g, juce::Rectangle<float> r, bool open, bool summing, juce::Colour colour)
{
    const auto c = r.getCentre();
    const float s = 4.0f;
    juce::Path p;
    if (open)
        p.addTriangle (c.x - s, c.y - s * 0.6f, c.x + s, c.y - s * 0.6f, c.x, c.y + s * 0.8f);
    else
        p.addTriangle (c.x - s * 0.6f, c.y - s, c.x - s * 0.6f, c.y + s, c.x + s * 0.8f, c.y);
    g.setColour (colour);
    if (summing)
        g.fillPath (p);
    else
        g.strokePath (p, juce::PathStrokeType (1.2f, juce::PathStrokeType::mitered));
}

} // namespace woodshed::app::mixui
