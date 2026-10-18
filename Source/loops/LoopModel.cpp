#include "LoopModel.h"

namespace woodshed::loops
{

namespace dids = daw::ids;

juce::String quantizeName (Quantize q)
{
    switch (q)
    {
        case Quantize::none:      return "None";
        case Quantize::quarter:   return "1/4";
        case Quantize::bar:       return "1 Bar";
        case Quantize::twoBars:   return "2 Bars";
        case Quantize::fourBars:  return "4 Bars";
        case Quantize::eightBars: return "8 Bars";
        default:                  return {};
    }
}

juce::int64 nextBoundary (const daw::TimeGrid& grid, juce::int64 position, Quantize q)
{
    if (q == Quantize::none)
        return position;

    const double tick = grid.tickAt ((double) juce::jmax ((juce::int64) 0, position));
    if (q == Quantize::quarter)
    {
        const auto meter = grid.meterAt (tick);
        auto beat = std::floor ((tick - meter.tick) / meter.ticksPerBeat());
        auto at = (juce::int64) std::llround (grid.sampleAt (meter.tick + beat * meter.ticksPerBeat()));
        if (at < position)
            at = (juce::int64) std::llround (grid.sampleAt (meter.tick + (beat + 1.0) * meter.ticksPerBeat()));
        return at;
    }

    const int every = q == Quantize::bar ? 1 : q == Quantize::twoBars ? 2 : q == Quantize::fourBars ? 4 : 8;
    auto bar = (int) std::floor (grid.barAt (tick));
    if (grid.barStartSample (bar) < position)
        ++bar;
    bar = (bar + every - 1) / every * every;
    return grid.barStartSample (bar);
}

//==============================================================================
juce::StringArray sceneNames (const juce::ValueTree& s)
{
    juce::StringArray names;
    const auto parsed = juce::JSON::parse (s[ids::loopScenes].toString());
    if (auto* items = parsed.getArray())
        for (auto& item : *items)
            names.add (item.toString());
    if (names.isEmpty())
        for (int i = 0; i < defaultScenes; ++i)
            names.add ("Scene " + juce::String (i + 1));
    return names;
}

int numScenes (const juce::ValueTree& s)
{
    return sceneNames (s).size();
}

void setSceneNames (daw::Project& p, const juce::StringArray& names)
{
    juce::Array<juce::var> items;
    for (auto& n : names)
        items.add (n);
    p.state.setProperty (ids::loopScenes, juce::JSON::toString (juce::var (items), true), &p.undo);
}

void addScene (daw::Project& p)
{
    auto names = sceneNames (p.state);
    if (names.size() >= maxScenes)
        return;
    names.add ("Scene " + juce::String (names.size() + 1));
    setSceneNames (p, names);
}

void removeScene (daw::Project& p, int column)
{
    auto names = sceneNames (p.state);
    if (! juce::isPositiveAndBelow (column, names.size()) || names.size() <= 1)
        return;

    for (int i = 0; i < p.numTracks(); ++i)
    {
        auto grid = gridOf (p.track (i));
        for (int c = grid.getNumChildren(); --c >= 0;)
        {
            auto cell = grid.getChild (c);
            const int col = (int) cell[ids::column];
            if (col == column)
                grid.removeChild (cell, &p.undo);
            else if (col > column)
                cell.setProperty (ids::column, col - 1, &p.undo);
        }
    }
    names.remove (column);
    setSceneNames (p, names);
}

Quantize gridQuantize (const juce::ValueTree& s)
{
    return (Quantize) juce::jlimit (0, numQuantizeChoices - 1, (int) s.getProperty (ids::loopQuantize, (int) Quantize::bar));
}

void setGridQuantize (daw::Project& p, Quantize q)
{
    p.state.setProperty (ids::loopQuantize, (int) q, &p.undo);
}

//==============================================================================
juce::ValueTree gridOf (const juce::ValueTree& track)
{
    return track.getChildWithName (ids::LOOPGRID);
}

juce::ValueTree gridOf (daw::Project& p, juce::ValueTree track, bool create)
{
    auto grid = track.getChildWithName (ids::LOOPGRID);
    if (! grid.isValid() && create && track.isValid())
    {
        grid = juce::ValueTree (ids::LOOPGRID);
        track.addChild (grid, -1, &p.undo);
    }
    return grid;
}

juce::ValueTree cellAt (const juce::ValueTree& track, int column)
{
    for (auto cell : gridOf (track))
        if (cell.hasType (ids::CELL) && (int) cell[ids::column] == column)
            return cell;
    return {};
}

juce::ValueTree contentOf (const juce::ValueTree& cell)
{
    return cell.getChildWithName (dids::REGION);
}

bool isMidiCell (const juce::ValueTree& cell)       { return daw::Project::isMidiRegion (contentOf (cell)); }
bool isOneShot (const juce::ValueTree& cell)        { return cell[ids::playMode].toString() == "oneshot"; }
int barsOf (const juce::ValueTree& cell)            { return juce::jlimit (1, maxCellBars, (int) cell.getProperty (ids::bars, 1)); }

Quantize quantizeOf (const juce::ValueTree& cell, const juce::ValueTree& projectState)
{
    if (cell.hasProperty (ids::quantize))
        return (Quantize) juce::jlimit (0, numQuantizeChoices - 1, (int) cell[ids::quantize]);
    return gridQuantize (projectState);
}

juce::int64 cellLength (const daw::TimeGrid& grid, const juce::ValueTree& cell)
{
    return (juce::int64) std::llround (barsOf (cell) * grid.samplesPerBar());
}

juce::ValueTree trackOfCell (const juce::ValueTree& cell)
{
    return cell.getParent().getParent();
}

namespace
{
juce::ValueTree makeCell (daw::Project& p, juce::ValueTree track, int column, int bars, juce::ValueTree content)
{
    if (auto old = cellAt (track, column); old.isValid())
        old.getParent().removeChild (old, &p.undo);

    juce::ValueTree cell (ids::CELL);
    cell.setProperty (ids::column, column, nullptr);
    cell.setProperty (ids::bars, juce::jlimit (1, maxCellBars, bars), nullptr);
    cell.setProperty (ids::playMode, "loop", nullptr);
    content.setProperty (dids::id, p.nextId(), nullptr);
    content.setProperty (dids::start, 0, nullptr);
    content.removeProperty (dids::loop, nullptr);
    content.removeProperty (dids::mute, nullptr);
    cell.addChild (content, -1, nullptr);
    gridOf (p, track, true).addChild (cell, -1, &p.undo);
    return cell;
}

/** New ids for every REGION in a copied tree (NOTEs don't have any). */
void renumber (daw::Project& p, juce::ValueTree tree)
{
    if (tree.hasType (dids::REGION))
        tree.setProperty (dids::id, p.nextId(), nullptr);
    for (auto c : tree)
        renumber (p, c);
}
}

juce::ValueTree copyRegionToCell (daw::Project& p, const juce::ValueTree& region, juce::ValueTree track, int column)
{
    if (! region.hasType (dids::REGION) || daw::Project::isTakeFolder (region) || ! track.isValid())
        return {};
    if (daw::Project::isMidiRegion (region) != daw::Project::isInstrumentTrack (track) || daw::Project::isAuxTrack (track))
        return {};

    const auto length = (juce::int64) region[dids::length];
    const int bars = (int) std::ceil ((double) length / p.grid().samplesPerBar() - 1.0e-6);
    return makeCell (p, track, column, bars, region.createCopy());
}

juce::ValueTree addMidiCell (daw::Project& p, juce::ValueTree track, int column, int bars)
{
    juce::ValueTree r (dids::REGION);
    r.setProperty (dids::name, track[dids::name], nullptr);
    r.setProperty (dids::kind, daw::kinds::midi, nullptr);
    r.setProperty (dids::offset, 0, nullptr);
    r.setProperty (dids::length, (juce::int64) std::llround (bars * p.grid().samplesPerBar()), nullptr);
    r.setProperty (dids::gain, 0.0, nullptr);
    return makeCell (p, track, column, bars, r);
}

juce::ValueTree addAudioCell (daw::Project& p, juce::ValueTree track, int column, const juce::String& file,
                              juce::int64 offset, juce::int64 length, int bars)
{
    juce::ValueTree r (dids::REGION);
    r.setProperty (dids::name, track[dids::name], nullptr);
    r.setProperty (dids::file, file, nullptr);
    r.setProperty (dids::offset, offset, nullptr);
    r.setProperty (dids::length, length, nullptr);
    r.setProperty (dids::gain, 0.0, nullptr);
    return makeCell (p, track, column, bars, r);
}

void removeCell (daw::Project& p, juce::ValueTree cell)
{
    if (cell.getParent().isValid())
        cell.getParent().removeChild (cell, &p.undo);
}

juce::ValueTree moveCell (daw::Project& p, juce::ValueTree cell, juce::ValueTree toTrack, int column, bool copy)
{
    const auto from = trackOfCell (cell);
    if (! cell.isValid() || ! toTrack.isValid() || daw::Project::isAuxTrack (toTrack)
        || isMidiCell (cell) != daw::Project::isInstrumentTrack (toTrack))
        return {};
    if (from == toTrack && (int) cell[ids::column] == column)
        return cell;

    auto moved = cell.createCopy();
    renumber (p, moved);
    moved.setProperty (ids::column, column, nullptr);
    if (! copy)
        removeCell (p, cell);
    if (auto old = cellAt (toTrack, column); old.isValid())
        removeCell (p, old);
    gridOf (p, toTrack, true).addChild (moved, -1, &p.undo);
    return moved;
}

juce::Array<int> placeInArrange (daw::Project& p, const juce::ValueTree& cell, juce::int64 from, juce::int64 to)
{
    juce::Array<int> made;
    auto track = trackOfCell (cell);
    const auto pass = cellLength (p.grid(), cell);
    if (! track.isValid() || pass <= 0 || to <= from)
        return made;

    for (auto passStart = from; passStart < to; passStart += pass)
    {
        const auto passEnd = juce::jmin (passStart + pass, to);
        for (auto r : cell)
        {
            if (! r.hasType (dids::REGION))
                continue;
            const auto start = passStart + (juce::int64) r[dids::start];
            const auto length = juce::jmin ((juce::int64) r[dids::length], passEnd - start);
            if (length <= 0)
                continue;
            auto copy = r.createCopy();
            copy.setProperty (dids::id, p.nextId(), nullptr);
            copy.setProperty (dids::start, start, nullptr);
            copy.setProperty (dids::length, length, nullptr);
            track.addChild (copy, -1, &p.undo);
            made.add ((int) copy[dids::id]);
        }
        if (isOneShot (cell))
            break;
    }
    return made;
}

int firstEmptyColumn (const juce::ValueTree& track, const juce::ValueTree& projectState)
{
    for (int c = 0; c < numScenes (projectState); ++c)
        if (! cellAt (track, c).isValid())
            return c;
    return -1;
}

juce::ValueTree findCellRegion (const daw::Project& p, int regionId)
{
    for (int i = 0; i < p.numTracks(); ++i)
        for (auto cell : gridOf (p.track (i)))
            for (auto r : cell)
                if (r.hasType (dids::REGION) && (int) r[dids::id] == regionId)
                    return r;
    return {};
}

} // namespace woodshed::loops
