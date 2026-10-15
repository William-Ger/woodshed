#include "Project.h"

namespace woodshed::daw
{

juce::String TimeGrid::toBarsBeats (juce::int64 samples) const
{
    const double beats = juce::jmax (0.0, toBeats (samples));
    const int bar = (int) (beats / beatsPerBar);
    const double inBar = beats - bar * beatsPerBar;
    const int beat = (int) inBar;
    const int tick = juce::jlimit (0, 959, (int) ((inBar - beat) * 960.0));
    return juce::String (bar + 1) + "." + juce::String (beat + 1) + "." + juce::String (tick).paddedLeft ('0', 3);
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

TimeGrid Project::grid() const
{
    TimeGrid g;
    g.sampleRate = (double) state.getProperty (ids::sampleRate, 48000.0);
    g.tempo = juce::jlimit (20.0, 400.0, (double) state.getProperty (ids::tempo, 120.0));
    g.beatsPerBar = juce::jlimit (1, 16, (int) state.getProperty (ids::beatsPerBar, 4));
    g.beatUnit = juce::jlimit (1, 16, (int) state.getProperty (ids::beatUnit, 4));
    return g;
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
    t.setProperty (ids::input, 0, nullptr);     // interface input 1, where William's guitar is plugged in
    t.setProperty (ids::rigOn, true, nullptr);
    state.addChild (t, insertIndex, &undo);
    return t;
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
        if (foreign)
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
                end = juce::jmax (end, (juce::int64) r[ids::start] + (juce::int64) r[ids::length]);
    return end;
}

} // namespace woodshed::daw
