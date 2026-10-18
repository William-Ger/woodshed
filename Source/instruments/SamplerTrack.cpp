#include "SamplerTrack.h"

namespace woodshed::sampler
{

using namespace daw;

juce::ValueTree addSamplerTrack (Project& p, const juce::String& name, int insertIndex)
{
    auto* type = InstrumentRegistry::get().find (typeId);
    if (type == nullptr)
        return {};
    const auto& info = type->info;
    auto t = p.addInstrumentTrack (name, typeId, info.patches.empty() ? juce::String() : info.patches.front().name,
                                   patchValues (info, info.patches.empty() ? nullptr : &info.patches.front()), insertIndex);
    samplerNode (t.getChildWithName (daw::ids::INSTRUMENT), &p.undo);
    return t;
}

bool isSamplerTrack (const juce::ValueTree& track)
{
    return Project::isInstrumentTrack (track) && track.getChildWithName (daw::ids::INSTRUMENT)[daw::ids::type].toString() == typeId;
}

bool canConvert (const juce::ValueTree& region)
{
    return region.hasType (daw::ids::REGION) && ! Project::isMidiRegion (region) && ! Project::isTakeFolder (region)
        && region[daw::ids::file].toString().isNotEmpty() && (juce::int64) region[daw::ids::length] > 0;
}

juce::ValueTree convertToSamplerTrack (Project& p, juce::ValueTree region, Mode mode)
{
    if (! canConvert (region))
        return {};
    const auto relative = region[daw::ids::file].toString();
    auto src = loadSource (p.resolve (relative));
    if (src == nullptr)
        return {};

    // The region's part of the file. Positions in the file are at its own rate; the timeline's at the project's.
    const double projectRate = p.grid().sampleRate;
    const double toFile = src->rate / projectRate;
    const auto regionStart = (juce::int64) region[daw::ids::start];
    const auto regionLength = (juce::int64) region[daw::ids::length];
    const auto from = juce::jlimit ((juce::int64) 0, src->length(), (juce::int64) region[daw::ids::offset]);
    const auto to = juce::jlimit (from, src->length(), from + (juce::int64) std::llround ((double) regionLength * toFile));
    if (to - from < 64)
        return {};

    const auto sourceTrack = Project::trackOf (region);
    const auto name = region[daw::ids::name].toString().isNotEmpty() ? region[daw::ids::name].toString()
                                                                    : sourceTrack[daw::ids::name].toString();
    auto track = addSamplerTrack (p, name + (mode == Mode::slice ? " Slices" : " Sampler"), p.state.indexOf (sourceTrack) + 1);
    if (! track.isValid())
        return {};

    auto inst = track.getChildWithName (daw::ids::INSTRUMENT);
    auto node = samplerNode (inst, &p.undo);
    loadSample (node, mode, relative, *src, from, to, &p.undo);
    const float gainDb = (float) (double) region.getProperty (daw::ids::gain, 0.0);
    for (auto z : node)
        if (z.hasType (ids::ZONE))
            z.setProperty (ids::gain, gainDb, &p.undo);

    // Slices play to their end whatever the note length; a short release keeps them tight.
    if (mode == Mode::slice)
    {
        auto values = Project::decodeValues (inst[daw::ids::knobs].toString());
        if ((int) values.size() > param::release)
            values[(size_t) param::release] = 30.0f;
        inst.setProperty (daw::ids::knobs, Project::encodeValues (values), &p.undo);
    }

    // The MIDI region that plays it back where the audio was.
    const auto& grid = p.grid();
    auto midi = p.addMidiRegion (track, regionStart, regionLength, name);
    const double originTick = grid.tickAt ((double) regionStart);
    auto tickOf = [&] (juce::int64 fileFrame)
    {
        const auto timeline = (double) regionStart + (double) (fileFrame - from) / toFile;
        return (juce::int64) std::floor (grid.tickAt (timeline) - originTick + 1.0e-6);
    };

    // Notes sit on whole ticks, so each slice starts where its (rounded-down) note lands and ends
    // where the next one starts: the slices still join up, and play back sample-exact.
    if (mode == Mode::slice)
    {
        std::vector<juce::ValueTree> zones;
        for (auto z : node)
            if (z.hasType (ids::ZONE))
                zones.push_back (z);
        for (auto& z : zones)
        {
            const auto on = (juce::int64) std::llround (grid.sampleAt (originTick + (double) tickOf ((juce::int64) z[ids::start])));
            const auto frame = juce::jlimit (from, to - 1, from + (juce::int64) std::llround ((double) (on - regionStart) * toFile));
            z.setProperty (ids::start, frame, &p.undo);
        }
        for (size_t i = 0; i + 1 < zones.size(); ++i)
            zones[i].setProperty (ids::end, zones[i + 1][ids::start], &p.undo);
    }

    for (auto z : node)
    {
        if (! z.hasType (ids::ZONE))
            continue;
        const auto zone = Zone::fromTree (z);
        Note n;
        n.pitch = mode == Mode::slice ? zone.lo : zone.root;
        n.velocity = 127;
        n.start = tickOf (zone.start);
        n.length = juce::jmax ((juce::int64) 1, tickOf (zone.end < 0 ? to : zone.end) - n.start);
        p.addNote (midi, n);
    }

    // Full velocity plays the zone at its own level.
    auto values = Project::decodeValues (inst[daw::ids::knobs].toString());
    if ((int) values.size() > param::velocity)
        values[(size_t) param::velocity] = 0.0f;
    inst.setProperty (daw::ids::knobs, Project::encodeValues (values), &p.undo);

    region.setProperty (daw::ids::mute, true, &p.undo);
    return track;
}

bool loadFileIntoTrack (Project& p, juce::ValueTree track, const juce::File& file, Mode mode)
{
    if (! isSamplerTrack (track))
        return false;
    const auto relative = importIntoProject (file, p.folder);
    if (relative.isEmpty())
        return false;
    auto src = loadSource (p.resolve (relative));
    if (src == nullptr)
        return false;
    auto node = samplerNode (track.getChildWithName (daw::ids::INSTRUMENT), &p.undo);
    loadSample (node, mode, relative, *src, 0, -1, &p.undo);
    return true;
}

} // namespace woodshed::sampler
