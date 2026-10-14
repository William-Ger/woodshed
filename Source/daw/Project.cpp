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

juce::File Project::defaultProjectsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Woodshed Projects");
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
    static const juce::uint32 colours[] { 0xffd9893b, 0xff4fa3c7, 0xff8bbf5a, 0xffc75b7a, 0xff9a7bd6,
                                          0xffd6c04f, 0xff4fc7a6, 0xffd66b4f };
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

    region.setProperty (ids::length, at - start, &undo);
    region.getParent().addChild (right, region.getParent().indexOf (region) + 1, &undo);
    return right;
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
