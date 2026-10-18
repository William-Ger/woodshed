// woodshed-bench loopgrid: checks the Loop Grid (clip launching) through the engine's live
// callback, without an audio interface: launches land on the quantize grid to the sample, a
// scene switches every track on one sample, cells replace the arrange while they play, a
// stopped MIDI cell leaves no note hanging, perform recording writes the right regions,
// recording into a cell loops it, and the grid survives a save and load.
// woodshed-bench demo-loopgrid <folder>: a project with a populated grid, for screenshots.

#include "../../Source/loops/LoopGrid.h"
#include "../../Source/instruments/Instrument.h"
#include <iostream>

using namespace woodshed;
using namespace woodshed::daw;
namespace lg = woodshed::loops;

namespace
{
constexpr double rate = 48000.0;
constexpr juce::int64 bar = 96000;          // 120 bpm, 4/4
constexpr int callbackSize = 487;           // doesn't divide a bar, so switches fall mid-block
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << "\n";
    failures += ok ? 0 : 1;
}

juce::String list (const std::vector<juce::int64>& v)
{
    juce::StringArray s;
    for (auto x : v) s.add (juce::String (x));
    return "[" + s.joinIntoString (", ") + "]";
}

/** Samples (offset by `origin`) where |x| first rises into [lo, hi), at least 50 apart. */
std::vector<juce::int64> peaks (const std::vector<float>& data, juce::int64 origin, float lo, float hi = 100.0f)
{
    std::vector<juce::int64> at;
    juce::int64 last = -1000;
    for (size_t i = 0; i < data.size(); ++i)
    {
        const float a = std::abs (data[i]);
        if (a >= lo && a < hi && (juce::int64) i - last > 50)
        {
            at.push_back (origin + (juce::int64) i);
            last = (juce::int64) i;
        }
    }
    return at;
}

double rms (const std::vector<float>& data, size_t from, size_t to)
{
    double sum = 0.0;
    for (size_t i = from; i < to && i < data.size(); ++i)
        sum += (double) data[i] * data[i];
    return std::sqrt (sum / (double) juce::jmax ((size_t) 1, to - from));
}

void writeWav (const juce::File& file, const juce::AudioBuffer<float>& buf)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
    writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

/** A silent file of `length` samples with a click of `level` at each of `at`. */
void writeClicks (const juce::File& file, juce::int64 length, std::initializer_list<juce::int64> at, float level)
{
    juce::AudioBuffer<float> buf (1, (int) length);
    buf.clear();
    for (auto a : at)
        buf.setSample (0, (int) a, level);
    writeWav (file, buf);
}

/** Counts note events and clicks on each note-on, like ProjectTest's click instrument. */
class CountingInstrument final : public Instrument
{
public:
    void reset() override {}
    void render (float* left, float* right, int, const NoteEvent* events, int numEvents) override
    {
        for (int i = 0; i < numEvents; ++i)
        {
            const auto& e = events[i];
            if (e.kind == NoteEvent::noteOn)            { ++ons; held.set ((size_t) e.note); left[e.offset] += 0.3f; right[e.offset] += 0.3f; }
            else if (e.kind == NoteEvent::noteOff)      { ++offs; held.reset ((size_t) e.note); }
            else if (e.kind == NoteEvent::allNotesOff)  { ++allOffs; held.reset(); }
        }
    }
    int ons = 0, offs = 0, allOffs = 0;
    std::bitset<128> held;
};

std::unique_ptr<Instrument> makeInstrument (const juce::String& type)
{
    if (type == "test.count")
        return std::make_unique<CountingInstrument>();
    if (auto* t = InstrumentRegistry::get().find (type))
        return t->create();
    return nullptr;
}

/** An engine (and a Loop Grid) on a project, driven through its live callback. */
struct Live
{
    explicit Live (Project& p)
    {
        engine.prepare (rate, 512);
        engine.setProject (&p);
        grid.service();
        // The faders glide up from silence; let them settle before listening.
        std::vector<float> l (4096), r (4096);
        for (int done = 0; done < 4096; done += 64)
        {
            float* outs[] { l.data() + done, r.data() + done };
            engine.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, 64, {});
        }
    }
    ~Live()     { engine.stop(); engine.setProject (nullptr); }

    /** Runs the callback until the playhead reaches `until` (or `samples` if stopped); the
        output is kept, starting at the playhead's position before. */
    void runTo (juce::int64 until, const std::vector<float>* input = nullptr)
    {
        origin = engine.getPosition();
        left.assign ((size_t) (until - origin), 0.0f);
        right.assign (left.size(), 0.0f);
        for (size_t done = 0; done < left.size();)
        {
            const int n = (int) juce::jmin ((size_t) callbackSize, left.size() - done);
            float* outs[] { left.data() + done, right.data() + done };
            std::vector<float> silence ((size_t) n, 0.0f);
            const float* ins[] { input != nullptr ? input->data() + (size_t) origin + done : silence.data() };
            engine.audioDeviceIOCallbackWithContext (ins, 1, outs, 2, n, {});
            done += (size_t) n;
        }
    }

    juce::AudioDeviceManager devices;
    Engine engine { devices, {}, makeInstrument };
    lg::LoopGrid grid { engine };
    juce::int64 origin = 0;
    std::vector<float> left, right;
};

std::vector<juce::ValueTree> regionsOn (const juce::ValueTree& track, const juce::String& file)
{
    std::vector<juce::ValueTree> list;
    for (auto r : track)
        if (r.hasType (ids::REGION) && r[ids::file].toString() == file)
            list.push_back (r);
    std::sort (list.begin(), list.end(), [] (auto& a, auto& b) { return (juce::int64) a[ids::start] < (juce::int64) b[ids::start]; });
    return list;
}
}

int runLoopGridTest();
int runLoopGridTest()
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("woodshed-loopgrid-test");
    root.deleteRecursively();

    std::cout << "launch quantize grid\n";
    {
        auto p = Project::createIn (root.getChildFile ("grid"), rate);
        const auto& g = p->grid();
        check (lg::nextBoundary (g, 30000, lg::Quantize::none) == 30000, "none: at once");
        check (lg::nextBoundary (g, 30000, lg::Quantize::quarter) == 48000 && lg::nextBoundary (g, 48000, lg::Quantize::quarter) == 48000,
               "1/4: the next beat, or this one when on it");
        check (lg::nextBoundary (g, 30000, lg::Quantize::bar) == bar && lg::nextBoundary (g, bar, lg::Quantize::bar) == bar
               && lg::nextBoundary (g, bar + 1, lg::Quantize::bar) == 2 * bar, "1 bar: the next bar line, or this one when on it");
        check (lg::nextBoundary (g, 100000, lg::Quantize::twoBars) == 2 * bar && lg::nextBoundary (g, 2 * bar + 1, lg::Quantize::twoBars) == 4 * bar,
               "2 bars: bars 1, 3, 5...");
        check (lg::nextBoundary (g, 1, lg::Quantize::fourBars) == 4 * bar && lg::nextBoundary (g, 1, lg::Quantize::eightBars) == 8 * bar,
               "4 and 8 bars");
        p->setMeterChange (2, 3, 4);
        check (lg::nextBoundary (p->grid(), 2 * bar + 1, lg::Quantize::bar) == 2 * bar + 72000, "follows a change to 3/4");
    }

    // A song with an audio track: the arrange has quiet clicks on every beat (+3000), the
    // cell (2 bars) loud ones at 1000 and 97000 into each pass.
    auto project = Project::createIn (root.getChildFile ("song"), rate);
    project->state.setProperty (ids::click, false, nullptr);
    writeClicks (project->audioFolder().getChildFile ("beats.wav"), 8 * bar,
                 { 3000, 27000, 51000, 75000, 99000, 123000, 147000, 171000, 195000, 219000, 243000, 267000, 291000, 315000, 339000,
                   363000, 387000, 411000, 435000, 459000, 483000, 507000, 531000, 555000, 579000 }, 0.25f);
    writeClicks (project->audioFolder().getChildFile ("loop.wav"), 2 * bar, { 1000, 97000 }, 0.5f);
    writeClicks (project->audioFolder().getChildFile ("loopB.wav"), bar, { 1000 }, 0.5f);
    writeClicks (project->audioFolder().getChildFile ("loopA2.wav"), bar, { 2000 }, 0.5f);

    auto a = project->addTrack ("A");
    a.setProperty (ids::rigOn, false, nullptr);
    project->addRegion (a, "Audio/beats.wav", 0, 0, 8 * bar, "beats");
    const auto cellA = lg::addAudioCell (*project, a, 0, "Audio/loop.wav", 0, 2 * bar, 2);

    std::cout << "launch on the bar, replacing the arrange\n";
    {
        Live live (*project);
        live.engine.setPosition (30000);
        live.engine.play();
        live.runTo (40000);
        live.grid.launchCell ((int) a[ids::id], 0);
        live.runTo (300000);

        const auto loud = peaks (live.left, live.origin, 0.4f);
        const auto quiet = peaks (live.left, live.origin, 0.1f, 0.4f);
        check (loud == std::vector<juce::int64> { 97000, 193000, 289000 }, "the cell starts exactly on bar 2 and loops every 2 bars " + list (loud));
        check (quiet == std::vector<juce::int64> { 51000, 75000 }, "the arrange plays until then, and is silent while the cell plays " + list (quiet));

        live.grid.stopTrack ((int) a[ids::id]);
        live.runTo (480000);
        const auto after = peaks (live.left, live.origin, 0.1f, 0.4f);
        const auto loudAfter = peaks (live.left, live.origin, 0.4f);
        check (after == std::vector<juce::int64> { 387000, 411000, 435000, 459000 }, "stop lands on bar 5 and the arrange plays again " + list (after));
        check (loudAfter.empty(), "the cell's click due at 385000 never comes " + list (loudAfter));
        auto status = live.grid.statusOf ((int) a[ids::id], 0);
        check (status.state == lg::LoopGrid::Status::idle && live.grid.playingColumn ((int) a[ids::id]) == -1, "the cell shows as stopped");
    }

    std::cout << "quantize choices and the queued state\n";
    {
        Live live (*project);
        lg::setGridQuantize (*project, lg::Quantize::quarter);
        live.engine.setPosition (10000);
        live.engine.play();
        live.runTo (11000);
        live.grid.launchCell ((int) a[ids::id], 0);
        live.runTo (12000);
        const auto queued = live.grid.statusOf ((int) a[ids::id], 0);
        live.runTo (60000);
        const auto loud = peaks (live.left, live.origin, 0.4f);
        check (queued.state == lg::LoopGrid::Status::queued, "waiting for its beat, the cell shows as queued");
        check (loud == std::vector<juce::int64> { 24000 + 1000 }, "1/4: starts on beat 2 " + list (loud));
        check (live.grid.statusOf ((int) a[ids::id], 0).state == lg::LoopGrid::Status::playing
               && live.grid.statusOf ((int) a[ids::id], 0).progress > 0.0f, "then shows as playing, with progress");

        a.getChildWithName (lg::ids::LOOPGRID).getChild (0).setProperty (lg::ids::quantize, (int) lg::Quantize::fourBars, nullptr);
        lg::setGridQuantize (*project, lg::Quantize::bar);
    }

    std::cout << "a cell's own quantize, and none\n";
    {
        Live live (*project);
        live.engine.setPosition (10000);
        live.engine.play();
        live.runTo (11000);
        live.grid.launchCell ((int) a[ids::id], 0);         // the cell says 4 bars
        live.runTo (4 * bar + 2000);
        check (peaks (live.left, live.origin, 0.4f) == std::vector<juce::int64> { 4 * bar + 1000 }, "4 bars: starts on bar 5");
        a.getChildWithName (lg::ids::LOOPGRID).getChild (0).removeProperty (lg::ids::quantize, nullptr);
    }

    // Track B, and an instrument track C that counts its notes. Column 1 on every track.
    auto b = project->addTrack ("B");
    b.setProperty (ids::rigOn, false, nullptr);
    a.setProperty (ids::pan, -1.0, nullptr);
    b.setProperty (ids::pan, 1.0, nullptr);
    lg::addAudioCell (*project, a, 1, "Audio/loopA2.wav", 0, bar, 1);
    lg::addAudioCell (*project, b, 1, "Audio/loopB.wav", 0, bar, 1);
    auto c = project->addInstrumentTrack ("C", "test.count", "Default", {});
    {
        auto cell = lg::addMidiCell (*project, c, 1, 1);
        project->addNote (lg::contentOf (cell), { 60, 100, 0, 960 });
    }
    lg::setGridQuantize (*project, lg::Quantize::twoBars);

    std::cout << "scene launch\n";
    {
        Live live (*project);
        live.grid.launchCell ((int) a[ids::id], 0);         // A plays column 0 from the start
        live.runTo (150000);
        std::vector<lg::LoopEvent> before;
        live.engine.getLoopPlayer().takeEvents (before);
        live.grid.launchScene (1);
        live.runTo (300000);

        std::vector<lg::LoopEvent> events;
        live.engine.getLoopPlayer().takeEvents (events);
        std::map<int, juce::int64> launched;
        for (auto& e : events)
            if (e.column == 1)
                launched[e.trackId] = e.at;
        check (launched.size() == 3 && launched[(int) a[ids::id]] == 2 * bar && launched[(int) b[ids::id]] == 2 * bar
               && launched[(int) c[ids::id]] == 2 * bar, "A, B and C all switch to scene 2 on bar 3, sample 192000");
        const auto left = peaks (live.left, live.origin, 0.6f);
        const auto right = peaks (live.right, live.origin, 0.6f);
        check (left == std::vector<juce::int64> { 2 * bar + 2000, 3 * bar + 2000 }, "A's new cell (left), looping every bar " + list (left));
        check (right == std::vector<juce::int64> { 2 * bar + 1000, 3 * bar + 1000 }, "B's cell (right) " + list (right));
        auto* inst = dynamic_cast<CountingInstrument*> (live.engine.getInstrument ((int) c[ids::id]));
        check (inst != nullptr && inst->ons == 2, "C's cell played its note on each pass");
    }

    std::cout << "MIDI cells: note-offs and the arrange's notes\n";
    {
        // A long arrange note on C that the cell must cut off, and the cell's own looping note.
        auto region = project->addMidiRegion (c, 0, 8 * bar, "Arrange");
        project->addNote (region, { 48, 100, 0, 8 * 3840 });
        lg::setGridQuantize (*project, lg::Quantize::bar);

        Live live (*project);
        live.engine.setPosition (0);
        live.engine.play();
        live.runTo (50000);
        live.grid.launchCell ((int) c[ids::id], 1);
        live.runTo (bar + 70000);       // the cell's note (a quarter) is off, then on again in the next pass...
        live.runTo (2 * bar + 10000);   // ...and sounding when it stops
        auto* inst = dynamic_cast<CountingInstrument*> (live.engine.getInstrument ((int) c[ids::id]));
        const bool sounding = inst != nullptr && inst->held[60];
        lg::setGridQuantize (*project, lg::Quantize::none);
        live.grid.stopTrack ((int) c[ids::id]);
        live.runTo (3 * bar);
        check (inst != nullptr && inst->allOffs >= 1 && ! inst->held[48], "launching cut the arrange's note");
        check (sounding && inst->held.none() && inst->ons == inst->offs + 1, "stopping the cell let go of its note (" + juce::String (inst != nullptr ? inst->ons : 0)
               + " ons, " + juce::String (inst != nullptr ? inst->offs : 0) + " offs, 1 of them the arrange's)");
        c.removeChild (region, nullptr);
        lg::setGridQuantize (*project, lg::Quantize::bar);
    }

    std::cout << "MIDI cells: no hanging notes in a real instrument\n";
    {
        auto organ = project->addInstrumentTrack ("Organ", "keys.organ", "Default", {});
        for (int i = 0; i < project->numTracks() - 1; ++i)
            project->track (i).setProperty (ids::mute, true, nullptr);
        auto cell = lg::addMidiCell (*project, organ, 0, 1);
        project->addNote (lg::contentOf (cell), { 60, 110, 0, 4 * 960 });       // the whole bar, every pass
        lg::setGridQuantize (*project, lg::Quantize::none);

        Live live (*project);
        live.grid.launchCell ((int) organ[ids::id], 0);
        live.runTo (bar + bar / 2);
        const auto playingLevel = rms (live.left, 100000 - (size_t) live.origin, 140000 - (size_t) live.origin);
        live.grid.stopTrack ((int) organ[ids::id]);
        live.runTo (live.engine.getPosition() + (juce::int64) rate * 2);
        const auto tail = rms (live.left, live.left.size() - 24000, live.left.size());
        check (playingLevel > 0.01, "the organ sounds while the cell plays (rms " + juce::String (playingLevel, 4) + ")");
        check (tail < 1.0e-4, "silence after stopping: no hanging note (rms " + juce::String (tail, 7) + ")");

        organ.getParent().removeChild (organ, nullptr);
        for (int i = 0; i < project->numTracks(); ++i)
            project->track (i).setProperty (ids::mute, false, nullptr);
        lg::setGridQuantize (*project, lg::Quantize::bar);
    }

    std::cout << "perform recording\n";
    {
        a.setProperty (ids::pan, 0.0, nullptr);
        Live live (*project);
        live.grid.setPerformRecording (true);
        live.grid.launchCell ((int) a[ids::id], 0);         // from the start (the transport starts on the bar)
        live.runTo (450000);
        live.grid.stopTrack ((int) a[ids::id]);             // on bar 6, mid-pass
        live.runTo (500000);
        live.grid.service();
        live.grid.setPerformRecording (false);

        const auto made = regionsOn (a, "Audio/loop.wav");
        std::vector<juce::int64> starts, lengths;
        for (auto& r : made)
        {
            starts.push_back ((juce::int64) r[ids::start]);
            lengths.push_back ((juce::int64) r[ids::length]);
        }
        check (starts == std::vector<juce::int64> { 0, 2 * bar, 4 * bar }, "a region per pass " + list (starts));
        check (lengths == std::vector<juce::int64> { 2 * bar, 2 * bar, bar }, "the last one ends where the cell stopped " + list (lengths));
    }
    {
        // The arrange now plays what the grid played.
        Live live (*project);
        live.engine.setPosition (0);
        live.engine.play();
        live.runTo (6 * bar);
        const auto loud = peaks (live.left, live.origin, 0.4f);
        check (loud == std::vector<juce::int64> { 1000, 97000, 193000, 289000, 385000 }, "and the arrange plays it back " + list (loud));
        project->undo.beginNewTransaction();
        for (auto& r : regionsOn (a, "Audio/loop.wav"))
            a.removeChild (r, nullptr);
    }

    std::cout << "recording into a cell\n";
    {
        auto rec = project->addTrack ("Rec");
        rec.setProperty (ids::rigOn, false, nullptr);
        rec.setProperty (ids::arm, true, nullptr);
        for (auto t : { a, b, c })
            t.setProperty (ids::mute, true, nullptr);

        Live live (*project);
        const auto latency = live.engine.getRecordLatencySamples();
        std::vector<float> input ((size_t) (8 * bar), 0.0f);
        input[20000] = 0.5f;                                // during the take
        input[150000] = 0.3f;                               // after it: heard? no, not recorded
        check (live.grid.recordCell ((int) rec[ids::id], 2, 1), "a one-bar take starts");
        live.runTo (10000, &input);
        check (live.grid.statusOf ((int) rec[ids::id], 2).state == lg::LoopGrid::Status::recording, "the cell shows as recording");
        live.runTo (3 * bar, &input);

        const auto loud = peaks (live.left, live.origin, 0.4f);
        check (loud == std::vector<juce::int64> { 20000, bar + 20000 - latency, 2 * bar + 20000 - latency },
               "it hears the input while recording, then loops the take every bar " + list (loud));
        live.grid.service();
        const auto cell = lg::cellAt (rec, 2);
        const auto content = lg::contentOf (cell);
        check (cell.isValid() && lg::barsOf (cell) == 1 && content[ids::file].toString().startsWith ("Audio/Loop")
               && (juce::int64) content[ids::offset] == latency && (juce::int64) content[ids::length] == bar,
               "the take is saved as a one-bar cell (latency " + juce::String (latency) + ")");
        live.runTo (4 * bar, &input);
        check (peaks (live.left, live.origin, 0.4f) == std::vector<juce::int64> { 3 * bar + 20000 - latency }, "it keeps looping");
        check (live.engine.getLoopPlayer().getRecordState() == lg::LoopPlayer::RecordState::committed
               || live.engine.getLoopPlayer().getRecordState() == lg::LoopPlayer::RecordState::idle, "the recorder is done with it");
        rec.setProperty (ids::arm, false, nullptr);
    }
    {
        // The saved cell, played from its file.
        auto rec = project->track (project->numTracks() - 1);
        Live live (*project);
        live.grid.launchCell ((int) rec[ids::id], 2);
        live.runTo (2 * bar);
        const auto latency = (juce::int64) lg::contentOf (lg::cellAt (rec, 2))[ids::offset];
        check (peaks (live.left, live.origin, 0.4f) == std::vector<juce::int64> { 20000 - latency, bar + 20000 - latency }, "the saved cell plays its file");
    }

    std::cout << "recording with interface latency\n";
    {
        // What you hear is `latency` late, so the take loops from `latency` samples into it.
        auto rec = project->track (project->numTracks() - 1);
        Live live (*project);
        constexpr int latency = 300;
        std::vector<float> input ((size_t) (4 * bar), 0.0f);
        input[20000] = 0.5f;
        live.engine.play();
        check (live.engine.getLoopPlayer().startRecording ((int) rec[ids::id], 5, lg::Quantize::bar, 1, false, latency, rate, (double) bar),
               "a take with 300 samples of latency starts");
        live.runTo (3 * bar, &input);
        const auto loud = peaks (live.left, live.origin, 0.4f);
        check (loud == std::vector<juce::int64> { 20000, bar + 20000 - latency, 2 * bar + 20000 - latency }, "heard live, then it loops 300 samples earlier " + list (loud));
        live.grid.service();
        const auto content = lg::contentOf (lg::cellAt (rec, 5));
        check ((juce::int64) content[ids::offset] == latency && (juce::int64) content[ids::length] == bar, "saved with the latency as its offset");
        lg::removeCell (*project, lg::cellAt (rec, 5));
    }

    std::cout << "copy to the grid, and save / load\n";
    {
        auto region = project->addRegion (b, "Audio/loop.wav", 4 * bar, 0, bar + bar / 2, "loop");
        const auto copied = lg::copyRegionToCell (*project, region, b, 3);
        check (copied.isValid() && lg::barsOf (copied) == 2 && (juce::int64) lg::contentOf (copied)[ids::start] == 0, "a region and a half becomes a 2-bar cell");
        check (! lg::copyRegionToCell (*project, region, c, 3).isValid(), "audio doesn't go into an instrument track's grid");
        b.removeChild (region, nullptr);

        const int cellRegionId = (int) lg::contentOf (cellA)[ids::id];
        check (! project->regionById (cellRegionId).isValid() && lg::findCellRegion (*project, cellRegionId).isValid(),
               "cell regions aren't the arrange's");
        check (project->contentEnd() == 8 * bar, "nor do they count towards the song's end");
        check (project->numTracks() == 4 && project->track (project->numTracks() - 1).hasType (ids::TRACK), "the track list is still flat");

        lg::setSceneNames (*project, { "Intro", "Verse", "Chorus", "Break" });
        lg::setGridQuantize (*project, lg::Quantize::fourBars);
        lg::cellAt (a, 0).setProperty (lg::ids::playMode, "oneshot", nullptr);
        project->master();
        check (project->save(), "saves");

        auto loaded = Project::load (project->folder);
        check (loaded != nullptr, "loads");
        if (loaded != nullptr)
        {
            check (lg::sceneNames (loaded->state) == juce::StringArray { "Intro", "Verse", "Chorus", "Break" }
                   && lg::gridQuantize (loaded->state) == lg::Quantize::fourBars, "scenes and quantize round-trip");
            int cells = 0, notes = 0;
            for (int i = 0; i < loaded->numTracks(); ++i)
                for (auto cell : lg::gridOf (loaded->track (i)))
                {
                    ++cells;
                    notes += lg::contentOf (cell).getNumChildren();
                }
            check (cells == 6 && notes == 1, "every cell round-trips (" + juce::String (cells) + " cells)");
            const auto la = loaded->track (0);
            check (lg::isOneShot (lg::cellAt (la, 0)) && lg::barsOf (lg::cellAt (la, 0)) == 2
                   && lg::contentOf (lg::cellAt (la, 0))[ids::file].toString() == "Audio/loop.wav", "a cell's settings round-trip");
            check (loaded->state.getChild (loaded->state.getNumChildren() - 1).hasType (ids::MASTER) && loaded->numTracks() == 4,
                   "MASTER stays last");
        }
    }

    std::cout << "one-shot cells\n";
    {
        for (auto t : { b, c })
            t.setProperty (ids::mute, true, nullptr);
        a.setProperty (ids::mute, false, nullptr);
        Live live (*project);
        live.grid.launchCell ((int) a[ids::id], 0);
        live.runTo (5 * bar);
        const auto loud = peaks (live.left, live.origin, 0.4f);
        const auto quiet = peaks (live.left, live.origin, 0.1f, 0.4f);
        check (loud == std::vector<juce::int64> { 1000, 97000 }, "plays once " + list (loud));
        check (! quiet.empty() && quiet.front() == 2 * bar + 3000, "then the arrange comes back " + list (quiet));
    }

    root.deleteRecursively();
    std::cout << (failures == 0 ? "all loop grid checks passed\n" : juce::String (failures) + " FAILED\n");
    return failures == 0 ? 0 : 1;
}

//==============================================================================
namespace
{
/** Note helper: `step` sixteenths from the cell's start, `len` sixteenths long. */
void notes (Project& p, const juce::ValueTree& cell, std::initializer_list<std::array<int, 4>> list)
{
    auto region = lg::contentOf (cell);
    for (auto& n : list)
        p.addNote (region, { n[0], n[3], (juce::int64) n[1] * 240, (juce::int64) n[2] * 240 });
}

void drumBar (Project& p, const juce::ValueTree& cell, int bars, bool busy, bool halfTime)
{
    auto region = lg::contentOf (cell);
    for (int b = 0; b < bars; ++b)
        for (int s = 0; s < 16; ++s)
        {
            const juce::int64 at = (juce::int64) (b * 16 + s) * 240;
            if (s == 0 || (! halfTime && s == 8) || (busy && s == 10))   p.addNote (region, { 36, 110, at, 120 });
            if ((halfTime ? s == 8 : s % 8 == 4))                         p.addNote (region, { 38, 105, at, 120 });
            if (s % 2 == 0 || busy)                                       p.addNote (region, { 42, s % 4 == 0 ? 90 : 60, at, 60 });
        }
}
}

int writeLoopGridDemoProject (const juce::File& folder);
int writeLoopGridDemoProject (const juce::File& folder)
{
    juce::ScopedJuceInitialiser_GUI init;
    folder.deleteRecursively();
    auto p = Project::createIn (folder, rate);
    p->state.setProperty (ids::tempo, 120.0, nullptr);
    lg::setSceneNames (*p, { "Intro", "Verse", "Chorus", "Break", "Outro", "Scene 6" });

    auto drums = p->addInstrumentTrack ("Drums", "drums.kit", "Studio", {});
    auto bass = p->addInstrumentTrack ("Bass", "bass.electric", "Default", {});
    auto keys = p->addInstrumentTrack ("Keys", "keys.epiano", "Default", {});
    auto pad = p->addInstrumentTrack ("Pad", "synth.pad", "Default", {});
    auto loop = p->addTrack ("Shaker Loop");
    loop.setProperty (ids::rigOn, false, nullptr);

    drumBar (*p, lg::addMidiCell (*p, drums, 0, 1), 1, false, true);
    drumBar (*p, lg::addMidiCell (*p, drums, 1, 2), 2, false, false);
    drumBar (*p, lg::addMidiCell (*p, drums, 2, 2), 2, true, false);
    drumBar (*p, lg::addMidiCell (*p, drums, 4, 1), 1, false, true);

    // Bass: root notes in A minor.
    notes (*p, lg::addMidiCell (*p, bass, 1, 2), { { 45, 0, 3, 100 }, { 45, 4, 2, 90 }, { 52, 6, 2, 90 }, { 45, 8, 3, 100 }, { 48, 12, 4, 95 },
                                                    { 41, 16, 3, 100 }, { 41, 20, 2, 90 }, { 48, 22, 2, 90 }, { 43, 24, 4, 100 }, { 43, 28, 4, 95 } });
    notes (*p, lg::addMidiCell (*p, bass, 2, 2), { { 45, 0, 2, 110 }, { 57, 2, 2, 90 }, { 45, 4, 2, 100 }, { 57, 6, 2, 90 }, { 41, 8, 2, 110 }, { 53, 10, 2, 90 },
                                                    { 41, 12, 2, 100 }, { 53, 14, 2, 90 }, { 48, 16, 2, 110 }, { 60, 18, 2, 90 }, { 43, 24, 2, 110 }, { 55, 26, 2, 90 } });
    notes (*p, lg::addMidiCell (*p, bass, 3, 1), { { 45, 0, 16, 90 } });

    // Keys: chords on the offbeats.
    notes (*p, lg::addMidiCell (*p, keys, 0, 2), { { 57, 0, 8, 80 }, { 60, 0, 8, 80 }, { 64, 0, 8, 80 }, { 53, 16, 8, 80 }, { 57, 16, 8, 80 }, { 60, 16, 8, 80 } });
    notes (*p, lg::addMidiCell (*p, keys, 1, 2), { { 57, 2, 2, 85 }, { 60, 2, 2, 85 }, { 64, 2, 2, 85 }, { 57, 6, 2, 85 }, { 60, 6, 2, 85 }, { 64, 6, 2, 85 },
                                                    { 53, 18, 2, 85 }, { 57, 18, 2, 85 }, { 60, 18, 2, 85 }, { 55, 26, 2, 85 }, { 59, 26, 2, 85 }, { 62, 26, 2, 85 } });
    notes (*p, lg::addMidiCell (*p, keys, 2, 2), { { 69, 0, 4, 95 }, { 72, 4, 4, 95 }, { 76, 8, 4, 95 }, { 72, 12, 4, 95 }, { 65, 16, 4, 95 }, { 69, 20, 4, 95 },
                                                    { 72, 24, 4, 95 }, { 67, 28, 4, 95 } });

    // Pad: long chords.
    notes (*p, lg::addMidiCell (*p, pad, 0, 4), { { 57, 0, 32, 70 }, { 64, 0, 32, 70 }, { 53, 32, 32, 70 }, { 60, 32, 32, 70 } });
    notes (*p, lg::addMidiCell (*p, pad, 2, 4), { { 57, 0, 32, 75 }, { 60, 0, 32, 75 }, { 64, 0, 32, 75 }, { 53, 32, 32, 75 }, { 57, 32, 32, 75 }, { 60, 32, 32, 75 } });
    notes (*p, lg::addMidiCell (*p, pad, 3, 4), { { 50, 0, 64, 65 }, { 57, 0, 64, 65 }, { 62, 0, 64, 65 } });
    lg::cellAt (pad, 3).setProperty (lg::ids::playMode, "oneshot", nullptr);

    // A shaker loop as audio: filtered noise bursts on every 8th note, one bar.
    {
        const int length = (int) bar;
        juce::AudioBuffer<float> buf (1, length);
        buf.clear();
        juce::Random random (7);
        float lp = 0.0f;
        for (int i = 0; i < length; ++i)
        {
            const int sinceHit = i % 12000;
            const float env = std::exp (-(float) sinceHit / ((i / 12000) % 2 == 0 ? 1400.0f : 700.0f));
            const float noise = random.nextFloat() * 2.0f - 1.0f;
            lp += 0.35f * (noise - lp);
            buf.setSample (0, i, (noise - lp) * env * 0.5f);
        }
        writeWav (p->audioFolder().getChildFile ("shaker.wav"), buf);
        for (int column : { 1, 2, 4 })
            lg::addAudioCell (*p, loop, column, "Audio/shaker.wav", 0, length, 1);
    }

    // A little arrange too, so the timeline isn't empty.
    {
        auto region = p->addMidiRegion (keys, 8 * bar, 4 * bar, "Keys");
        p->addNote (region, { 57, 80, 0, 3840 * 2 });
        p->addNote (region, { 60, 80, 0, 3840 * 2 });
        p->addNote (region, { 64, 80, 0, 3840 * 2 });
    }

    p->state.setProperty (ids::selectedTrack, drums[ids::id], nullptr);
    p->undo.clearUndoHistory();
    const bool ok = p->save();
    std::cout << (ok ? "wrote " : "couldn't write ") << p->projectFile().getFullPathName() << "\n";
    return ok ? 0 : 1;
}
