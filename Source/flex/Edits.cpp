#include "Edits.h"

namespace woodshed::flex
{

namespace dids = daw::ids;

namespace
{
juce::int64 origin (const juce::ValueTree& region)
{
    return (juce::int64) region[dids::start] - (juce::int64) region[dids::offset];
}

/** The smallest stretch a piece between two markers may be squeezed to. */
constexpr double minimumPieceSeconds = 0.01;
}

WarpMap anchoredWarp (const juce::ValueTree& region)
{
    auto w = warpOf (region);
    if (w.markers.size() >= 2)
        return w;
    const double from = (double) (juce::int64) region[dids::offset];
    const double to = from + (double) (juce::int64) region[dids::length];
    WarpMap anchored;
    anchored.markers = { { w.toSrc (from), from }, { w.toSrc (to), to } };
    for (auto& m : w.markers)
        if (m.dst > from + 1.0 && m.dst < to - 1.0)
            anchored.markers.push_back (m);
    anchored.tidy();
    return anchored;
}

void setTrackFlex (daw::Project& p, juce::ValueTree track, bool on)
{
    if (on) track.setProperty (ids::flexOn, true, &p.undo);
    else    track.removeProperty (ids::flexOn, &p.undo);
}

void setTrackMode (daw::Project& p, juce::ValueTree track, Mode m)
{
    track.setProperty (ids::flexMode, modeId (m), &p.undo);
    track.setProperty (ids::flexOn, true, &p.undo);
}

int addMarker (daw::Project& p, juce::ValueTree region, double src)
{
    auto w = anchoredWarp (region);
    const double dst = w.toDst (src);
    for (size_t i = 0; i < w.markers.size(); ++i)
        if (std::abs (w.markers[i].src - src) < 2.0)
            return (int) i;
    w.markers.push_back ({ src, dst });
    w.tidy();
    setWarp (region, w, &p.undo);
    setTrackFlex (p, region.getParent(), true);
    for (size_t i = 0; i < w.markers.size(); ++i)
        if (std::abs (w.markers[i].src - src) < 2.0)
            return (int) i;
    return -1;
}

juce::Range<double> markerRange (const WarpMap& w, int index, double sampleRate)
{
    const double room = minimumPieceSeconds * sampleRate;
    const auto n = (int) w.markers.size();
    const double lo = index > 0 ? w.markers[(size_t) index - 1].dst + room : -1.0e12;
    const double hi = index + 1 < n ? w.markers[(size_t) index + 1].dst - room : 1.0e12;
    return { lo, juce::jmax (lo, hi) };
}

void moveMarker (daw::Project& p, juce::ValueTree region, int index, double dst)
{
    auto w = warpOf (region);
    if (! juce::isPositiveAndBelow (index, (int) w.markers.size()))
        return;
    w.markers[(size_t) index].dst = markerRange (w, index, p.grid().sampleRate).clipValue (dst);
    setWarp (region, w, &p.undo);
}

void removeMarker (daw::Project& p, juce::ValueTree region, int index)
{
    auto w = warpOf (region);
    if (! juce::isPositiveAndBelow (index, (int) w.markers.size()))
        return;
    w.markers.erase (w.markers.begin() + index);
    setWarp (region, w, &p.undo);
}

void resetTiming (daw::Project& p, juce::ValueTree region)
{
    // Keep the region's first sample where it is: the file sample there becomes its offset.
    const auto w = warpOf (region);
    const auto from = (juce::int64) region[dids::offset];
    const auto srcFrom = (juce::int64) std::llround (w.toSrc ((double) from));
    const auto srcTo = (juce::int64) std::llround (w.toSrc ((double) (from + (juce::int64) region[dids::length])));
    region.setProperty (dids::offset, juce::jmax ((juce::int64) 0, srcFrom), &p.undo);
    region.setProperty (dids::length, juce::jmax ((juce::int64) 64, srcTo - srcFrom), &p.undo);
    region.removeProperty (ids::flexMarkers, &p.undo);
    region.removeProperty (ids::followTempo, &p.undo);
}

void quantizeAudio (daw::Project& p, juce::ValueTree region, const std::vector<double>& transients, double gridTicks)
{
    if (gridTicks <= 0.0)
        return;
    const auto& grid = p.grid();
    const auto o = (double) origin (region);
    auto w = anchoredWarp (region);
    const double from = w.markers.front().src, to = w.markers.back().src;
    const double room = minimumPieceSeconds * grid.sampleRate;

    // Every transient inside the region, to the nearest grid line.
    std::vector<Marker> wanted;
    for (auto t : transients)
    {
        if (t <= from + room || t >= to - room)
            continue;
        const double tick = grid.tickAt (o + w.toDst (t));
        const double snapped = std::round (tick / gridTicks) * gridTicks;
        wanted.push_back ({ t, grid.sampleAt (snapped) - o });
    }

    // Keep the ends, and only markers that keep everything in order.
    WarpMap q;
    q.markers.push_back (w.markers.front());
    for (auto& m : wanted)
        if (m.dst > q.markers.back().dst + room && m.dst < w.markers.back().dst - room)
            q.markers.push_back (m);
    q.markers.push_back (w.markers.back());
    q.tidy();
    setWarp (region, q, &p.undo);
    setTrackFlex (p, region.getParent(), true);
}

void setFollowTempo (daw::Project& p, juce::ValueTree region, bool on)
{
    if (on)
    {
        region.setProperty (ids::followTempo, true, &p.undo);
        setTrackFlex (p, region.getParent(), true);
    }
    else
    {
        region.removeProperty (ids::followTempo, &p.undo);
    }
}

void retimeForTempo (juce::ValueTree region, const daw::TimeGrid& before, const daw::TimeGrid& after, juce::UndoManager* undo)
{
    auto move = [&] (double sample) { return after.sampleAt (before.tickAt (sample)); };
    const auto start = (juce::int64) region[dids::start];
    const auto length = (juce::int64) region[dids::length];
    const auto oldOrigin = (double) origin (region);

    auto w = anchoredWarp (region);
    if (before.hasChanges() || after.hasChanges())
    {
        // A tempo that changes inside the region bends it: a marker on every beat keeps up.
        const auto base = w;
        const double beatTicks = daw::TimeGrid::ticksPerQuarterNote;
        const double firstTick = std::ceil (before.tickAt ((double) start) / beatTicks) * beatTicks;
        for (double tick = firstTick; before.sampleAt (tick) < (double) (start + length); tick += beatTicks)
        {
            const double dst = before.sampleAt (tick) - oldOrigin;
            w.markers.push_back ({ base.toSrc (dst), dst });
        }
        w.tidy();
    }

    const auto newOrigin = (juce::int64) std::llround (move (oldOrigin));
    const auto newStart = (juce::int64) std::llround (move ((double) start));
    const auto newEnd = (juce::int64) std::llround (move ((double) (start + length)));
    for (auto& m : w.markers)
        m.dst = move (oldOrigin + m.dst) - (double) newOrigin;
    w.tidy();

    region.setProperty (dids::start, newStart, undo);
    region.setProperty (dids::offset, newStart - newOrigin, undo);
    region.setProperty (dids::length, juce::jmax ((juce::int64) 64, newEnd - newStart), undo);
    setWarp (region, w, undo);
}

//==============================================================================
void adoptTempo (daw::Project& p, juce::ValueTree region, const TempoEstimate& t)
{
    if (t.bpm <= 0.0 || t.beats.empty() || ! juce::isPositiveAndBelow (t.firstDownbeat, (int) t.beats.size()))
        return;

    // The region keeps its audio as it is while the tempo changes around it.
    const bool follows = (bool) region[ids::followTempo];
    region.removeProperty (ids::followTempo, &p.undo);

    const auto before = p.grid();
    const auto w = warpOf (region);
    const double downbeat = (double) origin (region) + w.toDst (t.beats[(size_t) t.firstDownbeat]);
    const int bar = juce::jmax (0, (int) std::lround (before.barAt (before.tickAt (downbeat))));

    auto& s = p.state;
    const double first = t.variable && ! t.barTempos.empty() ? t.barTempos.front() : t.bpm;
    s.setProperty (dids::tempo, std::round (juce::jlimit (20.0, 400.0, first) * 100.0) / 100.0, &p.undo);
    juce::StringArray changes;
    if (t.variable)
    {
        const auto meter = before.meterAt (0.0);
        for (size_t k = 1; k < t.barTempos.size(); ++k)
            changes.add (juce::String ((double) (bar + (int) k) * meter.ticksPerBar(), 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
                         + ":" + juce::String (juce::jlimit (20.0, 400.0, t.barTempos[k]), 3).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."));
    }
    if (changes.isEmpty()) s.removeProperty (dids::tempoChanges, &p.undo);
    else                   s.setProperty (dids::tempoChanges, changes.joinIntoString (";"), &p.undo);
    p.keepMusicalPositions (before);

    // Then the region slides so its first downbeat is on the bar line it was nearest.
    const auto& after = p.grid();
    const double movedDownbeat = (double) origin (region) + w.toDst (t.beats[(size_t) t.firstDownbeat]);
    auto shift = (juce::int64) std::llround ((double) after.barStartSample (bar) - movedDownbeat);
    auto start = (juce::int64) region[dids::start] + shift;
    for (int b = bar + 1; start < 0 && b < bar + 64; ++b)
    {
        shift = (juce::int64) std::llround ((double) after.barStartSample (b) - movedDownbeat);
        start = (juce::int64) region[dids::start] + shift;
    }
    region.setProperty (dids::start, juce::jmax ((juce::int64) 0, start), &p.undo);
    region.setProperty (ids::sourceTempo, std::round (t.bpm * 100.0) / 100.0, &p.undo);
    if (follows)
        region.setProperty (ids::followTempo, true, &p.undo);
}

void conformToTempo (daw::Project& p, juce::ValueTree region, const TempoEstimate& t)
{
    if (t.bpm <= 0.0 || t.beats.size() < 2)
        return;
    const auto& grid = p.grid();
    const auto o = (double) origin (region);
    const auto old = warpOf (region);
    const auto from = (juce::int64) region[dids::offset];
    const double srcFrom = old.toSrc ((double) from);
    const double srcTo = old.toSrc ((double) (from + (juce::int64) region[dids::length]));

    // The first downbeat goes to the nearest bar line; every beat after it to the next beat.
    const auto meter = grid.meterAt (grid.tickAt (o));
    const double beatTicks = meter.ticksPerBeat();
    const int d = juce::jlimit (0, (int) t.beats.size() - 1, t.firstDownbeat);
    const double downbeatTick = grid.tickAt (o + old.toDst (t.beats[(size_t) d]));
    const double barTick = std::round ((downbeatTick - meter.tick) / meter.ticksPerBar()) * meter.ticksPerBar() + meter.tick;

    WarpMap w;
    for (size_t k = 0; k < t.beats.size(); ++k)
        w.markers.push_back ({ t.beats[k], grid.sampleAt (barTick + ((double) k - d) * beatTicks) - o });
    w.tidy();
    if (w.markers.size() < 2)
        return;

    // The region keeps the same stretch of the recording, now wherever the beats put it.
    const auto newFrom = (juce::int64) std::llround (w.toDst (srcFrom));
    const auto newTo = (juce::int64) std::llround (w.toDst (srcTo));
    setWarp (region, w, &p.undo);
    region.setProperty (dids::start, juce::jmax ((juce::int64) 0, (juce::int64) std::llround (o) + newFrom), &p.undo);
    region.setProperty (dids::offset, newFrom, &p.undo);
    region.setProperty (dids::length, juce::jmax ((juce::int64) 64, newTo - newFrom), &p.undo);
    region.setProperty (ids::sourceTempo, std::round (t.bpm * 100.0) / 100.0, &p.undo);
    setFollowTempo (p, region, true);
}

//==============================================================================
namespace
{
const std::vector<int>& scaleSteps (Scale s)
{
    static const std::vector<int> chromatic { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, major { 0, 2, 4, 5, 7, 9, 11 },
        minor { 0, 2, 3, 5, 7, 8, 10 }, majorPent { 0, 2, 4, 7, 9 }, minorPent { 0, 3, 5, 7, 10 }, blues { 0, 3, 5, 6, 7, 10 },
        dorian { 0, 2, 3, 5, 7, 9, 10 }, mixolydian { 0, 2, 4, 5, 7, 9, 10 };
    switch (s)
    {
        case Scale::major:           return major;
        case Scale::minor:           return minor;
        case Scale::majorPentatonic: return majorPent;
        case Scale::minorPentatonic: return minorPent;
        case Scale::blues:           return blues;
        case Scale::dorian:          return dorian;
        case Scale::mixolydian:      return mixolydian;
        case Scale::chromatic:
        default:                     return chromatic;
    }
}
}

juce::StringArray scaleNames()
{
    return { "Chromatic", "Major", "Minor", "Major Pentatonic", "Minor Pentatonic", "Blues", "Dorian", "Mixolydian" };
}

float nearestInScale (float pitch, int root, Scale scale)
{
    float best = std::round (pitch);
    float bestDistance = 1000.0f;
    const int base = (int) std::floor (pitch) - 12;
    for (int candidate = base; candidate <= base + 24; ++candidate)
    {
        const int degree = ((candidate - root) % 12 + 12) % 12;
        const auto& steps = scaleSteps (scale);
        if (std::find (steps.begin(), steps.end(), degree) == steps.end())
            continue;
        if (const float d = std::abs ((float) candidate - pitch); d < bestDistance)
        {
            bestDistance = d;
            best = (float) candidate;
        }
    }
    return best;
}

void quantizeToScale (std::vector<PitchNote>& notes, int root, Scale scale, const std::vector<int>& which)
{
    for (int i = 0; i < (int) notes.size(); ++i)
    {
        if (! which.empty() && std::find (which.begin(), which.end(), i) == which.end())
            continue;
        auto& n = notes[(size_t) i];
        n.shift = nearestInScale (n.pitch + n.shift, root, scale) - n.pitch;
    }
}

} // namespace woodshed::flex
