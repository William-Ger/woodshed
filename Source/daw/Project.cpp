#include "Project.h"
#include "../instruments/Instrument.h"
#include "../engine/Registry.h"

namespace woodshed::daw
{

void TimeGrid::finalise()
{
    std::sort (tempoChanges.begin(), tempoChanges.end(), [] (auto& x, auto& y) { return x.tick < y.tick; });
    std::sort (meterChanges.begin(), meterChanges.end(), [] (auto& x, auto& y) { return x.bar < y.bar; });

    changeSamples.clear();
    double sample = 0.0, tick = 0.0, bpm = tempo;
    for (auto& c : tempoChanges)
    {
        sample += (c.tick - tick) * sampleRate * 60.0 / bpm / ticksPerQuarterNote;
        changeSamples.push_back (sample);
        tick = c.tick;
        bpm = c.bpm;
    }

    meters.clear();
    meters.push_back ({ 0, beatsPerBar, beatUnit, 0.0 });
    for (auto& c : meterChanges)
    {
        const auto& last = meters.back();
        if (c.bar <= last.firstBar)
            continue;
        meters.push_back ({ c.bar, c.beatsPerBar, c.beatUnit, last.tick + (c.bar - last.firstBar) * last.ticksPerBar() });
    }
}

double TimeGrid::tickAt (double sample) const
{
    // The last tempo change at or before the sample (before the song: the starting tempo).
    size_t i = 0;
    while (i < changeSamples.size() && changeSamples[i] <= sample)
        ++i;
    const double fromTick = i == 0 ? 0.0 : tempoChanges[i - 1].tick;
    const double fromSample = i == 0 ? 0.0 : changeSamples[i - 1];
    const double bpm = i == 0 ? tempo : tempoChanges[i - 1].bpm;
    return fromTick + (sample - fromSample) * bpm * ticksPerQuarterNote / (sampleRate * 60.0);
}

double TimeGrid::sampleAt (double tick) const
{
    size_t i = 0;
    while (i < tempoChanges.size() && tempoChanges[i].tick <= tick)
        ++i;
    const double fromTick = i == 0 ? 0.0 : tempoChanges[i - 1].tick;
    const double fromSample = i == 0 ? 0.0 : changeSamples[i - 1];
    const double bpm = i == 0 ? tempo : tempoChanges[i - 1].bpm;
    return fromSample + (tick - fromTick) * sampleRate * 60.0 / bpm / ticksPerQuarterNote;
}

double TimeGrid::tempoAt (double sample) const
{
    double bpm = tempo;
    for (size_t i = 0; i < changeSamples.size() && changeSamples[i] <= sample; ++i)
        bpm = tempoChanges[i].bpm;
    return bpm;
}

TimeGrid::Meter TimeGrid::meterAt (double tick) const
{
    if (meters.empty())
        return { 0, beatsPerBar, beatUnit, 0.0 };
    size_t i = 0;
    while (i + 1 < meters.size() && meters[i + 1].tick <= tick)
        ++i;
    return meters[i];
}

double TimeGrid::barStartTick (int bar) const
{
    if (meters.empty())
        return bar * Meter { 0, beatsPerBar, beatUnit, 0.0 }.ticksPerBar();
    size_t i = 0;
    while (i + 1 < meters.size() && meters[i + 1].firstBar <= bar)
        ++i;
    return meters[i].tick + (bar - meters[i].firstBar) * meters[i].ticksPerBar();
}

double TimeGrid::barAt (double tick) const
{
    const auto m = meterAt (tick);
    return m.firstBar + (tick - m.tick) / m.ticksPerBar();
}

juce::String TimeGrid::toBarsBeats (juce::int64 samples) const
{
    const double tick = juce::jmax (0.0, tickAt ((double) samples));
    const auto m = meterAt (tick);
    const double inMeter = tick - m.tick;
    const int barInMeter = (int) std::floor (inMeter / m.ticksPerBar());
    const double inBar = inMeter - barInMeter * m.ticksPerBar();
    const int beat = juce::jlimit (0, m.beatsPerBar - 1, (int) (inBar / m.ticksPerBeat()));
    const int t = juce::jlimit (0, 959, (int) ((inBar - beat * m.ticksPerBeat()) / m.ticksPerBeat() * 960.0));
    return juce::String (m.firstBar + barInMeter + 1) + "." + juce::String (beat + 1) + "." + juce::String (t).paddedLeft ('0', 3);
}

juce::String TimeGrid::toClock (juce::int64 samples) const
{
    const double seconds = juce::jmax (0.0, (double) samples / sampleRate);
    const int minutes = (int) (seconds / 60.0);
    const double rest = seconds - minutes * 60.0;
    return juce::String (minutes) + ":" + juce::String (rest, 3).paddedLeft ('0', 6);
}

//==============================================================================
Project::Project() = default;

juce::ValueTree Project::createState (double sampleRate)
{
    juce::ValueTree s (ids::PROJECT);
    s.setProperty (ids::name, "Untitled", nullptr);
    s.setProperty (ids::sampleRate, sampleRate, nullptr);
    s.setProperty (ids::tempo, 120.0, nullptr);
    s.setProperty (ids::beatsPerBar, 4, nullptr);
    s.setProperty (ids::beatUnit, 4, nullptr);
    s.setProperty (ids::click, true, nullptr);
    s.setProperty (ids::countIn, true, nullptr);
    s.setProperty (ids::loopOn, false, nullptr);
    s.setProperty (ids::loopStart, 0, nullptr);
    s.setProperty (ids::loopEnd, (juce::int64) (sampleRate * 8.0), nullptr);
    s.setProperty (ids::masterVolume, 0.0, nullptr);
    s.setProperty (ids::selectedTrack, -1, nullptr);
    return s;
}

namespace
{
juce::File& projectsFolderOverride()
{
    static juce::File folder;
    return folder;
}
}

juce::File Project::defaultProjectsFolder()
{
    if (projectsFolderOverride() != juce::File())
        return projectsFolderOverride();
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Woodshed Projects");
}

void Project::setDefaultProjectsFolder (const juce::File& folder)
{
    projectsFolderOverride() = folder;
}

std::unique_ptr<Project> Project::createIn (const juce::File& folder, double sampleRate)
{
    auto p = std::make_unique<Project>();
    p->state = createState (sampleRate);
    p->folder = folder;
    p->folder.createDirectory();
    p->audioFolder().createDirectory();
    p->state.setProperty (ids::name, p->folder.getFileName(), nullptr);
    return p;
}

std::unique_ptr<Project> Project::createNew (double sampleRate)
{
    const auto base = defaultProjectsFolder();
    base.createDirectory();
    const auto name = "Untitled " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M");
    auto p = createIn (base.getNonexistentChildFile (name, {}, true), sampleRate);

    auto t = p->addTrack ("Guitar");
    t.setProperty (ids::arm, true, nullptr);
    p->state.setProperty (ids::selectedTrack, t[ids::id], nullptr);
    p->undo.clearUndoHistory();
    return p;
}

std::unique_ptr<Project> Project::load (const juce::File& fileOrFolder)
{
    const auto file = fileOrFolder.isDirectory() ? fileOrFolder.getChildFile ("project.woodshed") : fileOrFolder;
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (ids::PROJECT.toString()))
        return nullptr;

    auto p = std::make_unique<Project>();
    p->state = juce::ValueTree::fromXml (*xml);
    p->folder = file.getParentDirectory();
    p->audioFolder().createDirectory();
    return p;
}

bool Project::save()
{
    folder.createDirectory();
    if (auto xml = state.createXml())
        return xml->writeTo (projectFile());
    return false;
}

bool Project::saveAs (const juce::File& newFolder)
{
    if (newFolder == folder)
        return save();

    if (! newFolder.createDirectory())
        return false;

    // Copy the recordings this project uses; region paths are relative, so they stay valid.
    if (audioFolder().isDirectory() && ! audioFolder().copyDirectoryTo (newFolder.getChildFile ("Audio")))
        return false;

    folder = newFolder;
    state.setProperty (ids::name, newFolder.getFileName(), nullptr);
    return save();
}

const TimeGrid& Project::grid() const
{
    const auto key = state[ids::sampleRate].toString() + "|" + state[ids::tempo].toString() + "|" + state[ids::beatsPerBar].toString()
                   + "|" + state[ids::beatUnit].toString() + "|" + state[ids::tempoChanges].toString() + "|" + state[ids::meterChanges].toString();
    if (key == cachedGridKey)
        return cachedGrid;

    TimeGrid g;
    g.sampleRate = (double) state.getProperty (ids::sampleRate, 48000.0);
    g.tempo = juce::jlimit (20.0, 400.0, (double) state.getProperty (ids::tempo, 120.0));
    g.beatsPerBar = juce::jlimit (1, 16, (int) state.getProperty (ids::beatsPerBar, 4));
    g.beatUnit = juce::jlimit (1, 16, (int) state.getProperty (ids::beatUnit, 4));
    for (auto& part : juce::StringArray::fromTokens (state[ids::tempoChanges].toString(), ";", {}))
        if (part.containsChar (':'))
        {
            const auto tick = part.upToFirstOccurrenceOf (":", false, false).getDoubleValue();
            if (tick > 0.0)
                g.tempoChanges.push_back ({ tick, juce::jlimit (20.0, 400.0, part.fromFirstOccurrenceOf (":", false, false).getDoubleValue()) });
        }
    for (auto& part : juce::StringArray::fromTokens (state[ids::meterChanges].toString(), ";", {}))
        if (part.containsChar (':') && part.containsChar ('/'))
        {
            const int bar = part.upToFirstOccurrenceOf (":", false, false).getIntValue();
            const auto sig = part.fromFirstOccurrenceOf (":", false, false);
            if (bar > 0)
                g.meterChanges.push_back ({ bar, juce::jlimit (1, 16, sig.upToFirstOccurrenceOf ("/", false, false).getIntValue()),
                                            juce::jlimit (1, 16, sig.fromFirstOccurrenceOf ("/", false, false).getIntValue()) });
        }
    g.finalise();
    cachedGrid = g;
    cachedGridKey = key;
    return cachedGrid;
}

void Project::keepMusicalPositions (const TimeGrid& before)
{
    const auto& after = grid();
    auto move = [&] (juce::int64 sample) { return (juce::int64) std::llround (after.sampleAt (before.tickAt ((double) sample))); };

    for (auto t : state)
    {
        if (! t.hasType (ids::TRACK))
            continue;
        for (auto r : t)
        {
            if (r.hasType (ids::REGION))
            {
                const auto start = (juce::int64) r[ids::start];
                const auto length = (juce::int64) r[ids::length];
                const auto newStart = move (start);
                if (isMidiRegion (r))
                {
                    const auto origin = start - (juce::int64) r[ids::offset];
                    r.setProperty (ids::length, juce::jmax ((juce::int64) 1, move (start + length) - newStart), &undo);
                    r.setProperty (ids::offset, newStart - move (origin), &undo);
                }
                if (newStart != start)
                    r.setProperty (ids::start, newStart, &undo);
            }
            else if (r.hasType (ids::AUTOMATION))
            {
                auto points = decodePoints (r[ids::points].toString());
                for (auto& pt : points)
                    pt.time = move (pt.time);
                r.setProperty (ids::points, encodePoints (points), &undo);
            }
        }
    }

    auto markers = getMarkers();
    for (auto& m : markers)
        m.position = move (m.position);
    if (! markers.empty())
        setMarkers (markers);

    for (auto& id : { ids::loopStart, ids::loopEnd })
        if (state.hasProperty (id))
            state.setProperty (id, move ((juce::int64) state[id]), &undo);
}

void Project::setTempoChange (double tick, double bpm)
{
    bpm = juce::jlimit (20.0, 400.0, bpm);
    const auto before = grid();
    if (tick <= 0.0)
    {
        state.setProperty (ids::tempo, bpm, &undo);
        keepMusicalPositions (before);
        return;
    }
    auto changes = grid().tempoChanges;
    changes.erase (std::remove_if (changes.begin(), changes.end(), [tick] (auto& c) { return std::abs (c.tick - tick) < 0.5; }), changes.end());
    changes.push_back ({ tick, bpm });
    std::sort (changes.begin(), changes.end(), [] (auto& a, auto& b) { return a.tick < b.tick; });
    juce::StringArray parts;
    for (auto& c : changes)
        parts.add (juce::String (c.tick, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + ":" + juce::String (c.bpm, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."));
    state.setProperty (ids::tempoChanges, parts.joinIntoString (";"), &undo);
    keepMusicalPositions (before);
}

void Project::removeTempoChange (double tick)
{
    const auto mapBefore = grid();
    auto changes = grid().tempoChanges;
    const auto before = changes.size();
    changes.erase (std::remove_if (changes.begin(), changes.end(), [tick] (auto& c) { return std::abs (c.tick - tick) < 0.5; }), changes.end());
    if (changes.size() == before)
        return;
    juce::StringArray parts;
    for (auto& c : changes)
        parts.add (juce::String (c.tick, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + ":" + juce::String (c.bpm, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."));
    if (parts.isEmpty()) state.removeProperty (ids::tempoChanges, &undo);
    else                 state.setProperty (ids::tempoChanges, parts.joinIntoString (";"), &undo);
    keepMusicalPositions (mapBefore);
}

void Project::setMeterChange (int bar, int beats, int unit)
{
    beats = juce::jlimit (1, 16, beats);
    unit = juce::jlimit (1, 16, unit);
    if (bar <= 0)
    {
        state.setProperty (ids::beatsPerBar, beats, &undo);
        state.setProperty (ids::beatUnit, unit, &undo);
        return;
    }
    auto changes = grid().meterChanges;
    changes.erase (std::remove_if (changes.begin(), changes.end(), [bar] (auto& c) { return c.bar == bar; }), changes.end());
    changes.push_back ({ bar, beats, unit });
    std::sort (changes.begin(), changes.end(), [] (auto& a, auto& b) { return a.bar < b.bar; });
    juce::StringArray parts;
    for (auto& c : changes)
        parts.add (juce::String (c.bar) + ":" + juce::String (c.beatsPerBar) + "/" + juce::String (c.beatUnit));
    state.setProperty (ids::meterChanges, parts.joinIntoString (";"), &undo);
}

void Project::removeMeterChange (int bar)
{
    auto changes = grid().meterChanges;
    const auto before = changes.size();
    changes.erase (std::remove_if (changes.begin(), changes.end(), [bar] (auto& c) { return c.bar == bar; }), changes.end());
    if (changes.size() == before)
        return;
    juce::StringArray parts;
    for (auto& c : changes)
        parts.add (juce::String (c.bar) + ":" + juce::String (c.beatsPerBar) + "/" + juce::String (c.beatUnit));
    if (parts.isEmpty()) state.removeProperty (ids::meterChanges, &undo);
    else                 state.setProperty (ids::meterChanges, parts.joinIntoString (";"), &undo);
}

int Project::nextId()
{
    int highest = 0;
    std::function<void (const juce::ValueTree&)> scan = [&] (const juce::ValueTree& t)
    {
        highest = juce::jmax (highest, (int) t.getProperty (ids::id, 0));
        for (auto c : t) scan (c);
    };
    scan (state);
    return highest + 1;
}

juce::Colour Project::trackColour (int index)
{
    static const juce::uint32 colours[] { 0xffff9f0a, 0xff0a84ff, 0xff32d74b, 0xffff375f, 0xffbf5af2,
                                          0xffffd60a, 0xff64d2ff, 0xffff6b3d };
    return juce::Colour (colours[(size_t) index % std::size (colours)]);
}

juce::ValueTree Project::trackById (int id) const
{
    for (auto t : state)
        if (t.hasType (ids::TRACK) && (int) t[ids::id] == id)
            return t;
    return {};
}

juce::ValueTree Project::addTrack (const juce::String& name, int insertIndex)
{
    juce::ValueTree t (ids::TRACK);
    t.setProperty (ids::id, nextId(), nullptr);
    t.setProperty (ids::name, name, nullptr);
    t.setProperty (ids::colour, (juce::int64) trackColour (numTracks()).getARGB(), nullptr);
    t.setProperty (ids::volume, 0.0, nullptr);
    t.setProperty (ids::pan, 0.0, nullptr);
    t.setProperty (ids::mute, false, nullptr);
    t.setProperty (ids::solo, false, nullptr);
    t.setProperty (ids::arm, false, nullptr);
    t.setProperty (ids::monitor, false, nullptr);
    t.setProperty (ids::input, 0, nullptr);     // interface input 1
    t.setProperty (ids::rigOn, true, nullptr);
    // Tracks stay ahead of the MASTER node.
    if (insertIndex < 0 || insertIndex > numTracks())
        insertIndex = numTracks();
    state.addChild (t, insertIndex, &undo);
    return t;
}

juce::ValueTree Project::master()
{
    auto m = state.getChildWithName (ids::MASTER);
    if (! m.isValid())
    {
        m = juce::ValueTree (ids::MASTER);
        state.addChild (m, -1, nullptr);
    }
    return m;
}

juce::ValueTree Project::addInstrumentTrack (const juce::String& name, const juce::String& instrumentType,
                                             const juce::String& patchName, const std::vector<float>& knobs, int insertIndex)
{
    auto t = addTrack (name, insertIndex);
    t.setProperty (ids::kind, kinds::instrument, &undo);
    t.setProperty (ids::rigOn, false, &undo);
    juce::ValueTree inst (ids::INSTRUMENT);
    inst.setProperty (ids::type, instrumentType, nullptr);
    inst.setProperty (ids::patch, patchName, nullptr);
    inst.setProperty (ids::knobs, encodeValues (knobs), nullptr);
    t.addChild (inst, -1, &undo);
    return t;
}

juce::ValueTree Project::addAuxTrack (const juce::String& name, int insertIndex)
{
    auto t = addTrack (name, insertIndex);
    t.setProperty (ids::kind, kinds::aux, &undo);
    t.setProperty (ids::rigOn, false, &undo);
    return t;
}

juce::ValueTree Project::addSend (juce::ValueTree track, int busId, double levelDb, bool pre)
{
    for (auto s : sends (track))
        if ((int) s[ids::bus] == busId)
        {
            s.setProperty (ids::level, levelDb, &undo);
            s.setProperty (ids::pre, pre, &undo);
            return s;
        }

    juce::ValueTree send (ids::SEND);
    send.setProperty (ids::bus, busId, nullptr);
    send.setProperty (ids::level, levelDb, nullptr);
    send.setProperty (ids::pre, pre, nullptr);
    track.addChild (send, -1, &undo);
    return send;
}

juce::Array<juce::ValueTree> Project::sends (const juce::ValueTree& track)
{
    juce::Array<juce::ValueTree> list;
    for (auto c : track)
        if (c.hasType (ids::SEND))
            list.add (c);
    return list;
}

juce::ValueTree Project::addInsert (juce::ValueTree track, const juce::String& pedalType, const std::vector<float>& knobs, int index)
{
    juce::ValueTree fx (ids::FX);
    fx.setProperty (ids::id, nextId(), nullptr);
    fx.setProperty (ids::type, pedalType, nullptr);
    fx.setProperty (ids::on, true, nullptr);
    fx.setProperty (ids::knobs, encodeValues (knobs), nullptr);

    // `index` counts inserts; place it among the track's children accordingly.
    const auto existing = inserts (track);
    const int at = juce::isPositiveAndBelow (index, existing.size()) ? track.indexOf (existing[index]) : -1;
    track.addChild (fx, at, &undo);
    return fx;
}

juce::Array<juce::ValueTree> Project::inserts (const juce::ValueTree& track)
{
    juce::Array<juce::ValueTree> list;
    for (auto c : track)
        if (c.hasType (ids::FX))
            list.add (c);
    return list;
}

juce::String Project::encodeValues (const std::vector<float>& values)
{
    juce::StringArray parts;
    for (auto v : values)
        parts.add (juce::String (v, 6).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."));
    for (auto& p : parts)
        if (p.isEmpty() || p == "-")
            p = "0";
    return parts.joinIntoString (",");
}

std::vector<float> Project::decodeValues (const juce::String& text)
{
    std::vector<float> values;
    for (auto& part : juce::StringArray::fromTokens (text, ",", {}))
        if (part.trim().isNotEmpty())
            values.push_back (part.getFloatValue());
    return values;
}

void Project::removeTrack (const juce::ValueTree& track)
{
    state.removeChild (track, &undo);
}

juce::ValueTree Project::addRegion (juce::ValueTree track, const juce::String& relativeFile, juce::int64 start,
                                    juce::int64 offset, juce::int64 length, const juce::String& name)
{
    juce::ValueTree r (ids::REGION);
    r.setProperty (ids::id, nextId(), nullptr);
    r.setProperty (ids::name, name, nullptr);
    r.setProperty (ids::file, relativeFile, nullptr);
    r.setProperty (ids::start, start, nullptr);
    r.setProperty (ids::offset, offset, nullptr);
    r.setProperty (ids::length, length, nullptr);
    r.setProperty (ids::gain, 0.0, nullptr);
    track.addChild (r, -1, &undo);
    return r;
}

juce::ValueTree Project::splitRegion (juce::ValueTree region, juce::int64 at)
{
    const auto start = (juce::int64) region[ids::start];
    const auto length = (juce::int64) region[ids::length];
    if (at <= start || at >= start + length)
        return {};

    auto right = region.createCopy();
    right.setProperty (ids::id, nextId(), nullptr);
    right.setProperty (ids::start, at, nullptr);
    right.setProperty (ids::offset, (juce::int64) region[ids::offset] + (at - start), nullptr);
    right.setProperty (ids::length, start + length - at, nullptr);
    right.removeProperty (ids::fadeIn, nullptr);        // the cut is a butt splice: each half
                                                        // keeps only its outer fade
    region.setProperty (ids::length, at - start, &undo);
    region.removeProperty (ids::fadeOut, &undo);
    region.getParent().addChild (right, region.getParent().indexOf (region) + 1, &undo);
    return right;
}

juce::ValueTree Project::regionById (int id) const
{
    for (auto t : state)
        for (auto r : t)
            if (r.hasType (ids::REGION) && (int) r[ids::id] == id)
                return r;
    return {};
}

juce::ValueTree Project::automationNode (const juce::ValueTree& track, const juce::String& param)
{
    for (auto c : track)
        if (c.hasType (ids::AUTOMATION) && c[ids::param].toString() == param)
            return c;
    return {};
}

std::vector<AutoPoint> Project::getAutomation (const juce::ValueTree& track, const juce::String& param)
{
    return decodePoints (automationNode (track, param)[ids::points].toString());
}

void Project::setAutomation (juce::ValueTree track, const juce::String& param, std::vector<AutoPoint> points)
{
    std::stable_sort (points.begin(), points.end(), [] (auto& a, auto& b) { return a.time < b.time; });
    auto node = automationNode (track, param);
    if (points.empty())
    {
        if (node.isValid())
            track.removeChild (node, &undo);
        return;
    }
    if (! node.isValid())
    {
        node = juce::ValueTree (ids::AUTOMATION);
        node.setProperty (ids::param, param, nullptr);
        node.setProperty (ids::points, encodePoints (points), nullptr);
        track.addChild (node, -1, &undo);
        return;
    }
    node.setProperty (ids::points, encodePoints (points), &undo);
}

float Project::evaluate (const std::vector<AutoPoint>& points, juce::int64 time)
{
    jassert (! points.empty());
    if (time <= points.front().time) return points.front().value;
    if (time >= points.back().time)  return points.back().value;
    auto next = std::upper_bound (points.begin(), points.end(), time, [] (juce::int64 t, const AutoPoint& p) { return t < p.time; });
    auto prev = next - 1;
    const auto span = (double) (next->time - prev->time);
    const float x = span > 0.0 ? (float) ((double) (time - prev->time) / span) : 1.0f;
    return prev->value + (next->value - prev->value) * x;
}

juce::String Project::encodePoints (const std::vector<AutoPoint>& points)
{
    juce::StringArray parts;
    for (auto& p : points)
        parts.add (juce::String (p.time) + ":" + juce::String (p.value, 5));
    return parts.joinIntoString (";");
}

std::vector<AutoPoint> Project::decodePoints (const juce::String& text)
{
    std::vector<AutoPoint> points;
    for (auto& part : juce::StringArray::fromTokens (text, ";", {}))
        if (part.containsChar (':'))
            points.push_back ({ part.upToFirstOccurrenceOf (":", false, false).getLargeIntValue(),
                                part.fromFirstOccurrenceOf (":", false, false).getFloatValue() });
    std::stable_sort (points.begin(), points.end(), [] (auto& a, auto& b) { return a.time < b.time; });
    return points;
}

std::vector<Project::AutoParam> Project::automatableParams (const juce::ValueTree& track)
{
    std::vector<AutoParam> list;
    list.push_back ({ "volume", "Volume", -60.0f, 6.0f, (float) (double) track.getProperty (ids::volume, 0.0), "dB" });
    list.push_back ({ "pan", "Pan", -1.0f, 1.0f, (float) (double) track.getProperty (ids::pan, 0.0), {} });

    if (isInstrumentTrack (track))
    {
        const auto inst = track.getChildWithName (ids::INSTRUMENT);
        if (auto* type = woodshed::InstrumentRegistry::get().find (inst[ids::type].toString()))
        {
            const auto values = decodeValues (inst[ids::knobs].toString());
            for (size_t k = 0; k < type->info.knobs.size(); ++k)
            {
                const auto& spec = type->info.knobs[k];
                list.push_back ({ "inst:" + juce::String ((int) k), type->info.name + " " + spec.label, spec.min, spec.max,
                                  k < values.size() ? values[k] : spec.def, spec.unit, spec.skew });
            }
        }
    }

    for (auto send : sends (track))
    {
        const auto bus = track.getParent().getChildWithProperty (ids::id, send[ids::bus]);
        list.push_back ({ "send:" + send[ids::bus].toString(), "Send to " + bus[ids::name].toString(), -60.0f, 6.0f,
                          (float) (double) send.getProperty (ids::level, 0.0), "dB" });
    }

    for (auto fx : inserts (track))
        if (auto* type = woodshed::Registry::get().find (fx[ids::type].toString()))
        {
            const auto values = decodeValues (fx[ids::knobs].toString());
            for (size_t k = 0; k < type->info.knobs.size(); ++k)
            {
                const auto& spec = type->info.knobs[k];
                list.push_back ({ "fx:" + fx[ids::id].toString() + ":" + juce::String ((int) k), type->info.name + " " + spec.label,
                                  spec.min, spec.max, k < values.size() ? values[k] : spec.def, spec.unit, spec.skew });
            }
        }
    return list;
}

juce::ValueTree Project::addTakeFolder (juce::ValueTree track, juce::int64 start, juce::int64 length, const juce::String& name)
{
    juce::ValueTree r (ids::REGION);
    r.setProperty (ids::id, nextId(), nullptr);
    r.setProperty (ids::name, name, nullptr);
    r.setProperty (ids::kind, kinds::folder, nullptr);
    r.setProperty (ids::start, start, nullptr);
    r.setProperty (ids::offset, 0, nullptr);
    r.setProperty (ids::length, juce::jmax ((juce::int64) 1, length), nullptr);
    r.setProperty (ids::gain, 0.0, nullptr);
    track.addChild (r, -1, &undo);
    return r;
}

juce::ValueTree Project::addTake (juce::ValueTree takeFolder, const juce::String& relativeFile, juce::int64 start,
                                  juce::int64 offset, juce::int64 length, const juce::String& name)
{
    juce::ValueTree t (ids::REGION);
    t.setProperty (ids::id, nextId(), nullptr);
    t.setProperty (ids::name, name, nullptr);
    t.setProperty (ids::file, relativeFile, nullptr);
    t.setProperty (ids::start, start, nullptr);
    t.setProperty (ids::offset, offset, nullptr);
    t.setProperty (ids::length, length, nullptr);
    takeFolder.addChild (t, -1, &undo);
    return t;
}

juce::Array<juce::ValueTree> Project::takes (const juce::ValueTree& folder)
{
    juce::Array<juce::ValueTree> list;
    for (auto c : folder)
        if (c.hasType (ids::REGION))
            list.add (c);
    return list;
}

std::vector<Project::CompSegment> Project::getComp (const juce::ValueTree& folder)
{
    const auto count = takes (folder).size();
    const auto from = (juce::int64) folder[ids::offset];
    const auto to = from + (juce::int64) folder[ids::length];
    std::vector<CompSegment> comp;
    for (auto& part : juce::StringArray::fromTokens (folder[ids::comp].toString(), ";", {}))
    {
        auto bits = juce::StringArray::fromTokens (part, ":", {});
        if (bits.size() == 3)
            comp.push_back ({ bits[0].getLargeIntValue(), bits[1].getLargeIntValue(), juce::jlimit (0, juce::jmax (0, count - 1), bits[2].getIntValue()) });
    }

    // Unset (or a folder trimmed wider than its comp): the last take fills the gaps.
    std::vector<CompSegment> full;
    auto cursor = juce::jmin (from, comp.empty() ? from : comp.front().from);
    for (auto& c : comp)
    {
        if (c.from > cursor)
            full.push_back ({ cursor, c.from, juce::jmax (0, count - 1) });
        full.push_back (c);
        cursor = c.to;
    }
    if (cursor < to)
        full.push_back ({ cursor, to, juce::jmax (0, count - 1) });
    return full;
}

void Project::setCompRange (juce::ValueTree takeFolder, juce::int64 from, juce::int64 to, int take)
{
    if (to <= from)
        return;
    std::vector<CompSegment> next;
    for (auto& c : getComp (takeFolder))
    {
        if (c.to <= from || c.from >= to) { next.push_back (c); continue; }
        if (c.from < from) next.push_back ({ c.from, from, c.take });
        if (c.to > to)     next.push_back ({ to, c.to, c.take });
    }
    next.push_back ({ from, to, take });
    std::sort (next.begin(), next.end(), [] (auto& a, auto& b) { return a.from < b.from; });

    // Neighbours on the same take become one segment.
    std::vector<CompSegment> merged;
    for (auto& c : next)
        if (! merged.empty() && merged.back().take == c.take && merged.back().to == c.from)
            merged.back().to = c.to;
        else
            merged.push_back (c);

    juce::StringArray parts;
    for (auto& c : merged)
        parts.add (juce::String (c.from) + ":" + juce::String (c.to) + ":" + juce::String (c.take));
    takeFolder.setProperty (ids::comp, parts.joinIntoString (";"), &undo);
}

juce::Array<int> Project::flattenTakeFolder (juce::ValueTree takeFolder)
{
    juce::Array<int> made;
    auto track = takeFolder.getParent();
    const auto takeList = takes (takeFolder);
    if (! track.isValid() || takeList.isEmpty())
        return made;

    const auto origin = contentOrigin (takeFolder);
    const auto visibleFrom = (juce::int64) takeFolder[ids::start];
    const auto visibleTo = visibleFrom + (juce::int64) takeFolder[ids::length];
    for (auto& c : getComp (takeFolder))
    {
        const auto& take = takeList.getReference (c.take);
        const auto takeFrom = (juce::int64) take[ids::start], takeTo = takeFrom + (juce::int64) take[ids::length];
        const auto from = juce::jmax (c.from, takeFrom, visibleFrom - origin);
        const auto to = juce::jmin (c.to, takeTo, visibleTo - origin);
        if (to <= from)
            continue;
        auto r = addRegion (track, take[ids::file].toString(), origin + from, (juce::int64) take[ids::offset] + (from - takeFrom),
                            to - from, takeFolder[ids::name].toString());
        r.setProperty (ids::gain, takeFolder.getProperty (ids::gain, 0.0), &undo);
        made.add ((int) r[ids::id]);
    }
    track.removeChild (takeFolder, &undo);
    return made;
}

juce::ValueTree Project::addMidiRegion (juce::ValueTree track, juce::int64 start, juce::int64 length, const juce::String& name)
{
    juce::ValueTree r (ids::REGION);
    r.setProperty (ids::id, nextId(), nullptr);
    r.setProperty (ids::name, name, nullptr);
    r.setProperty (ids::kind, kinds::midi, nullptr);
    r.setProperty (ids::start, start, nullptr);
    r.setProperty (ids::offset, 0, nullptr);
    r.setProperty (ids::length, juce::jmax ((juce::int64) 1, length), nullptr);
    r.setProperty (ids::gain, 0.0, nullptr);
    track.addChild (r, -1, &undo);
    return r;
}

juce::ValueTree Project::addNote (juce::ValueTree region, const Note& n)
{
    juce::ValueTree note (ids::NOTE);
    note.setProperty (ids::pitch, juce::jlimit (0, 127, n.pitch), nullptr);
    note.setProperty (ids::velocity, juce::jlimit (1, 127, n.velocity), nullptr);
    note.setProperty (ids::start, n.start, nullptr);
    note.setProperty (ids::length, juce::jmax ((juce::int64) 1, n.length), nullptr);

    // Keep notes in time order so playback and drawing can walk them in one pass.
    int at = region.getNumChildren();
    while (at > 0 && (juce::int64) region.getChild (at - 1)[ids::start] > n.start)
        --at;
    region.addChild (note, at, &undo);
    return note;
}

Note Project::noteFrom (const juce::ValueTree& v)
{
    return { (int) v[ids::pitch], (int) v.getProperty (ids::velocity, 100),
             (juce::int64) v[ids::start], (juce::int64) v.getProperty (ids::length, ticksPerQuarter) };
}

void Project::setNote (juce::ValueTree note, const Note& n)
{
    note.setProperty (ids::pitch, juce::jlimit (0, 127, n.pitch), &undo);
    note.setProperty (ids::velocity, juce::jlimit (1, 127, n.velocity), &undo);
    note.setProperty (ids::length, juce::jmax ((juce::int64) 1, n.length), &undo);

    if ((juce::int64) note[ids::start] != n.start)
    {
        note.setProperty (ids::start, n.start, &undo);

        // Move it back into time order.
        auto region = note.getParent();
        if (region.isValid())
        {
            const int from = region.indexOf (note);
            int to = from;
            while (to > 0 && (juce::int64) region.getChild (to - 1)[ids::start] > n.start) --to;
            while (to < region.getNumChildren() - 1 && (juce::int64) region.getChild (to + 1)[ids::start] < n.start) ++to;
            if (to != from)
                region.moveChild (from, to, &undo);
        }
    }
}

juce::ValueTree Project::copyRegions (const juce::Array<int>& regionIds) const
{
    juce::ValueTree clip (ids::CLIPBOARD);
    clip.setProperty (ids::file, folder.getFullPathName(), nullptr);
    juce::int64 earliest = std::numeric_limits<juce::int64>::max();
    int topTrack = std::numeric_limits<int>::max();

    for (int i = 0; i < numTracks(); ++i)
        for (auto r : track (i))
            if (r.hasType (ids::REGION) && regionIds.contains ((int) r[ids::id]))
            {
                earliest = juce::jmin (earliest, (juce::int64) r[ids::start]);
                topTrack = juce::jmin (topTrack, i);
            }

    for (int i = 0; i < numTracks(); ++i)
        for (auto r : track (i))
            if (r.hasType (ids::REGION) && regionIds.contains ((int) r[ids::id]))
            {
                auto c = r.createCopy();
                c.setProperty (ids::start, (juce::int64) r[ids::start] - earliest, nullptr);
                c.setProperty (ids::lane, i - topTrack, nullptr);
                clip.addChild (c, -1, nullptr);
            }
    return clip;
}

juce::Array<int> Project::pasteRegions (const juce::ValueTree& clipboard, juce::int64 at, int firstTrack)
{
    juce::Array<int> added;
    if (numTracks() == 0)
        return added;

    // From another project: copy each recording in once, under a name that's free here.
    const juce::File source (clipboard[ids::file].toString());
    const bool foreign = source != juce::File() && source != folder;
    std::map<juce::String, juce::String> imported;

    for (auto c : clipboard)
    {
        auto r = c.createCopy();
        if (foreign && r.hasProperty (ids::file))
        {
            const auto relative = r[ids::file].toString();
            auto it = imported.find (relative);
            if (it == imported.end())
            {
                const auto from = source.getChildFile (relative);
                auto to = audioFolder().getNonexistentChildFile (from.getFileNameWithoutExtension(), from.getFileExtension(), false);
                audioFolder().createDirectory();
                const auto copied = from.copyFileTo (to) ? to.getRelativePathFrom (folder).replaceCharacter ('\\', '/') : relative;
                it = imported.emplace (relative, copied).first;
            }
            r.setProperty (ids::file, it->second, nullptr);
        }

        const int target = juce::jlimit (0, numTracks() - 1, firstTrack + (int) r[ids::lane]);
        r.removeProperty (ids::lane, nullptr);
        r.setProperty (ids::id, nextId(), nullptr);
        r.setProperty (ids::start, juce::jmax ((juce::int64) 0, at + (juce::int64) r[ids::start]), nullptr);
        track (target).addChild (r, -1, &undo);
        added.add ((int) r[ids::id]);
    }
    return added;
}

juce::Array<int> Project::repeatRegions (const juce::Array<int>& regionIds)
{
    juce::int64 from = std::numeric_limits<juce::int64>::max(), to = 0;
    int topTrack = std::numeric_limits<int>::max();
    for (int i = 0; i < numTracks(); ++i)
        for (auto r : track (i))
            if (r.hasType (ids::REGION) && regionIds.contains ((int) r[ids::id]))
            {
                from = juce::jmin (from, (juce::int64) r[ids::start]);
                to = juce::jmax (to, (juce::int64) r[ids::start] + (juce::int64) r[ids::length]);
                topTrack = juce::jmin (topTrack, i);
            }

    if (to <= from)
        return {};
    return pasteRegions (copyRegions (regionIds), to, topTrack);
}

void Project::nudgeRegions (const juce::Array<int>& regionIds, juce::int64 delta)
{
    juce::Array<juce::ValueTree> regions;
    juce::int64 earliest = std::numeric_limits<juce::int64>::max();
    for (int id : regionIds)
        if (auto r = regionById (id); r.isValid())
        {
            regions.add (r);
            earliest = juce::jmin (earliest, (juce::int64) r[ids::start]);
        }

    delta = juce::jmax (delta, -earliest);
    if (regions.isEmpty() || delta == 0)
        return;
    for (auto& r : regions)
        r.setProperty (ids::start, (juce::int64) r[ids::start] + delta, &undo);
}

std::vector<Marker> Project::getMarkers() const
{
    std::vector<Marker> list;
    const auto parsed = juce::JSON::parse (state[ids::markers].toString());
    if (auto* items = parsed.getArray())
        for (auto& item : *items)
            list.push_back ({ (int) item["id"], item["name"].toString(), (juce::int64) item["position"] });

    std::stable_sort (list.begin(), list.end(), [] (auto& a, auto& b) { return a.position < b.position; });
    return list;
}

void Project::setMarkers (const std::vector<Marker>& list)
{
    if (list.empty())
    {
        state.removeProperty (ids::markers, &undo);
        return;
    }

    juce::Array<juce::var> items;
    for (auto& m : list)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", m.id);
        o->setProperty ("name", m.name);
        o->setProperty ("position", m.position);
        items.add (juce::var (o));
    }
    state.setProperty (ids::markers, juce::JSON::toString (juce::var (items), true), &undo);
}

int Project::addMarker (juce::int64 at)
{
    auto list = getMarkers();
    for (auto& m : list)
        if (m.position == at)
            return m.id;

    // Logic numbers new markers after the highest "Marker N" so far.
    int highest = 0;
    for (auto& m : list)
        if (m.name.startsWith ("Marker "))
            highest = juce::jmax (highest, m.name.fromFirstOccurrenceOf ("Marker ", false, false).getIntValue());

    // Marker ids live in the JSON, which nextId() doesn't see, so stay above both.
    int id = nextId();
    for (auto& m : list)
        id = juce::jmax (id, m.id + 1);
    list.push_back ({ id, "Marker " + juce::String (highest + 1), juce::jmax ((juce::int64) 0, at) });
    setMarkers (list);
    return id;
}

void Project::moveMarker (int id, juce::int64 to)
{
    auto list = getMarkers();
    for (auto& m : list)
        if (m.id == id && m.position != to)
        {
            m.position = juce::jmax ((juce::int64) 0, to);
            setMarkers (list);
            return;
        }
}

void Project::renameMarker (int id, const juce::String& name)
{
    auto list = getMarkers();
    for (auto& m : list)
        if (m.id == id && m.name != name)
        {
            m.name = name;
            setMarkers (list);
            return;
        }
}

void Project::removeMarker (int id)
{
    auto list = getMarkers();
    const auto before = list.size();
    list.erase (std::remove_if (list.begin(), list.end(), [id] (auto& m) { return m.id == id; }), list.end());
    if (list.size() != before)
        setMarkers (list);
}

juce::int64 Project::nextMarkerAfter (juce::int64 from) const
{
    for (auto& m : getMarkers())
        if (m.position > from)
            return m.position;
    return -1;
}

juce::int64 Project::previousMarkerBefore (juce::int64 from) const
{
    juce::int64 found = -1;
    for (auto& m : getMarkers())
        if (m.position < from)
            found = m.position;
    return found;
}

juce::int64 Project::contentEnd() const
{
    juce::int64 end = 0;
    for (auto t : state)
        for (auto r : t)
            if (r.hasType (ids::REGION))
                end = juce::jmax (end, loopEnd (r));
    return end;
}

juce::int64 Project::loopEnd (const juce::ValueTree& region) const
{
    const auto start = (juce::int64) region[ids::start];
    const auto end = start + (juce::int64) region[ids::length];
    if (! (bool) region[ids::loop])
        return end;

    // The song ends where the last region that doesn't loop ends.
    juce::int64 songEnd = 0;
    for (auto t : state)
        for (auto r : t)
            if (r.hasType (ids::REGION) && ! (bool) r[ids::loop])
                songEnd = juce::jmax (songEnd, (juce::int64) r[ids::start] + (juce::int64) r[ids::length]);

    auto limit = songEnd > end ? songEnd : end + (juce::int64) std::llround (grid().samplesPerBar() * loopLimitBars);
    for (auto other : region.getParent())
        if (other.hasType (ids::REGION) && other != region)
        {
            const auto otherStart = (juce::int64) other[ids::start];
            if (otherStart >= end)
                limit = juce::jmin (limit, otherStart);
        }
    return limit;
}

} // namespace woodshed::daw
