// woodshed-bench project: checks the multitrack engine without an audio interface.
// A test recording with clicks at known samples is placed on tracks, mixed offline and through
// the live callback, and every click must land on exactly the sample the timeline says.

#include "../../Source/daw/Engine.h"
#include "../../Source/instruments/Instrument.h"
#include <iostream>

using namespace woodshed::daw;

namespace
{
constexpr double rate = 48000.0;
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << "\n";
    failures += ok ? 0 : 1;
}

/** Peaks above half of `threshold`, as sample indices. */
std::vector<int> clicks (const float* data, int n, float threshold = 0.1f)
{
    std::vector<int> at;
    for (int i = 0; i < n; ++i)
        if (std::abs (data[i]) > threshold && (at.empty() || i - at.back() > 50))
            at.push_back (i);
    return at;
}

void writeClicks (const juce::File& file)
{
    juce::AudioBuffer<float> buf (1, 10000);
    buf.clear();
    buf.setSample (0, 1000, 0.5f);
    buf.setSample (0, 5000, 0.5f);

    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
    writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

/** A test instrument: a click of the note's velocity on every note-on, a negative one on every
    note-off, and nothing else, so the timing of every event is visible in the output. */
class ClickInstrument final : public woodshed::Instrument
{
public:
    void reset() override               { ++resets; }
    void render (float* left, float* right, int, const woodshed::NoteEvent* events, int numEvents) override
    {
        for (int i = 0; i < numEvents; ++i)
        {
            const auto& e = events[i];
            if (e.kind == woodshed::NoteEvent::noteOn)       { left[e.offset] += e.value; right[e.offset] += e.value; lastNote = e.note; }
            else if (e.kind == woodshed::NoteEvent::noteOff) { left[e.offset] -= 0.25f; right[e.offset] -= 0.25f; }
            else if (e.kind == woodshed::NoteEvent::allNotesOff) ++allOffs;
        }
    }
    int resets = 0, allOffs = 0, lastNote = -1;
};

struct Render { std::vector<float> left, right; };

Render offline (Project& project, AudioPool& pool, juce::int64 length)
{
    std::map<int, std::shared_ptr<TrackRuntime>> runtimes;
    for (auto t : project.state)
    {
        if (! t.hasType (ids::TRACK)) continue;
        auto rt = std::make_shared<TrackRuntime>();
        rt->id = (int) t[ids::id];
        rt->work.setSize (2, 64);
        rt->gainLeft.reset (rate, 0.03);
        rt->gainRight.reset (rate, 0.03);
        runtimes[rt->id] = rt;
    }

    auto snap = Engine::buildSnapshot (project, pool, runtimes);
    Render r { std::vector<float> ((size_t) length), std::vector<float> ((size_t) length) };
    for (juce::int64 pos = 0; pos < length; pos += 64)
    {
        Mixer::Block b;
        b.position = pos;
        b.numSamples = (int) juce::jmin ((juce::int64) 64, length - pos);
        b.playing = true;
        b.left = r.left.data() + pos;
        b.right = r.right.data() + pos;
        Mixer::render (*snap, b);
    }
    return r;
}
}

int runProjectTest();
int runProjectTest()
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("woodshed-project-test");
    folder.deleteRecursively();

    auto project = Project::createIn (folder, rate);
    writeClicks (project->audioFolder().getChildFile ("clicks.wav"));
    AudioPool pool;

    auto a = project->addTrack ("A");
    a.setProperty (ids::rigOn, false, nullptr);
    project->addRegion (a, "Audio/clicks.wav", 20000, 500, 6000, "clicks");   // clicks at 20500 and 24500

    std::cout << "region placement\n";
    {
        auto r = offline (*project, pool, 40000);
        const auto left = clicks (r.left.data(), (int) r.left.size());
        check (left == std::vector<int> { 20500, 24500 }, "clicks land at 20500 and 24500");
        check (std::abs (r.left[20500] - 0.5f) < 0.002f && std::abs (r.right[20500] - 0.5f) < 0.002f, "centre pan is unity on both sides");
    }

    std::cout << "trimmed region\n";
    {
        a.getChild (0).setProperty (ids::length, 4000, nullptr);       // now ends at 24000
        auto r = offline (*project, pool, 40000);
        check (clicks (r.left.data(), (int) r.left.size()) == std::vector<int> { 20500 }, "trimmed click is gone");
        a.getChild (0).setProperty (ids::length, 6000, nullptr);
    }

    std::cout << "split\n";
    {
        project->undo.beginNewTransaction ("Split");
        auto right = project->splitRegion (a.getChild (0), 22000);
        check (right.isValid() && (juce::int64) right[ids::offset] == 2500 && (juce::int64) right[ids::length] == 4000, "right half starts 2000 samples into the region");
        auto r = offline (*project, pool, 40000);
        check (clicks (r.left.data(), (int) r.left.size()) == std::vector<int> { 20500, 24500 }, "split region plays the same");
        project->undo.undo();
        check (a.getNumChildren() == 1 && (juce::int64) a.getChild (0)[ids::length] == 6000, "undo puts the region back whole");
    }

    std::cout << "fades\n";
    {
        // A steady 0.5 tone (DC) region, so the gain envelope is the output.
        juce::AudioBuffer<float> dc (1, 20000);
        for (int i = 0; i < dc.getNumSamples(); ++i)
            dc.setSample (0, i, 0.5f);
        const auto file = project->audioFolder().getChildFile ("dc.wav");
        file.deleteFile();
        {
            std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
            juce::WavAudioFormat wav;
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
            writer->writeFromAudioSampleBuffer (dc, 0, dc.getNumSamples());
        }

        auto f = project->addTrack ("Fades");
        f.setProperty (ids::rigOn, false, nullptr);
        a.setProperty (ids::mute, true, nullptr);
        auto region = project->addRegion (f, "Audio/dc.wav", 1000, 0, 10000, "dc");
        region.setProperty (ids::fadeIn, 2000, nullptr);
        region.setProperty (ids::fadeOut, 4000, nullptr);

        auto r = offline (*project, pool, 12000);
        const float halfPower = 0.5f * std::sin (juce::MathConstants<float>::pi * 0.25f);
        check (std::abs (r.left[1000]) < 1.0e-4f, "fade-in starts from silence");
        check (std::abs (r.left[2000] - halfPower) < 0.002f, "half way into the fade-in is at half power");
        check (std::abs (r.left[5000] - 0.5f) < 0.002f, "full level between the fades");
        check (std::abs (r.left[9000] - halfPower) < 0.002f, "half way through the fade-out is at half power");
        check (std::abs (r.left[10999]) < 0.002f, "fade-out ends at silence");

        region.setProperty (ids::fadeIn, 9000, nullptr);       // overlaps the fade-out: gives way to it
        r = offline (*project, pool, 12000);
        check (r.left[6999] > 0.49f, "an overlong fade-in stops where the fade-out begins");

        // Two regions overlapping by 2000 samples with matching fades cross-fade at constant power.
        region.setProperty (ids::fadeIn, 0, nullptr);
        region.setProperty (ids::fadeOut, 2000, nullptr);
        auto g = project->addTrack ("Fades 2");
        g.setProperty (ids::rigOn, false, nullptr);
        project->addRegion (g, "Audio/dc.wav", 9000, 0, 3000, "dc 2").setProperty (ids::fadeIn, 2000, nullptr);
        r = offline (*project, pool, 12000);
        float worst = 0.0f;
        for (int i = 9000; i < 11000; ++i)
        {
            // Equal power: the squares of the two gains sum to one.
            const float out = std::sin ((float) (11000 - 1 - i) / 2000.0f * juce::MathConstants<float>::halfPi);
            const float in = std::sin ((float) (i - 9000) / 2000.0f * juce::MathConstants<float>::halfPi);
            worst = juce::jmax (worst, std::abs (r.left[(size_t) i] - 0.5f * (out + in)));
        }
        check (worst < 0.002f && r.left[10000] > 0.5f, "cross-fade follows both equal-power curves");

        project->undo.beginNewTransaction ("Split");
        auto right = project->splitRegion (region, 5000);
        check (! region.hasProperty (ids::fadeOut) && ! right.hasProperty (ids::fadeIn)
                 && (int) right[ids::fadeOut] == 2000, "a split keeps only each half's outer fade");
        project->undo.undo();
        check ((int) region[ids::fadeOut] == 2000, "undoing the split brings the fade-out back");

        project->removeTrack (g);
        project->removeTrack (f);
        a.setProperty (ids::mute, false, nullptr);
    }

    std::cout << "copy, paste, repeat, nudge, region mute\n";
    {
        auto b = project->addTrack ("B");
        b.setProperty (ids::rigOn, false, nullptr);
        auto c = project->addTrack ("C");
        auto onA = a.getChild (0);                                  // 20000..26000 on track 0
        auto onB = project->addRegion (b, "Audio/clicks.wav", 23000, 0, 2000, "b");
        const juce::Array<int> both { (int) onA[ids::id], (int) onB[ids::id] };

        auto clip = project->copyRegions (both);
        check (clip.getNumChildren() == 2 && (juce::int64) clip.getChild (0)[ids::start] == 0
                 && (juce::int64) clip.getChild (1)[ids::start] == 3000 && (int) clip.getChild (1)[ids::lane] == 1,
               "copy keeps the regions' spacing and track layout");

        project->undo.beginNewTransaction ("Paste");
        auto pasted = project->pasteRegions (clip, 50000, 1);       // onto B and C
        auto p0 = project->regionById (pasted[0]), p1 = project->regionById (pasted[1]);
        check (pasted.size() == 2 && p0.getParent() == b && p1.getParent() == c
                 && (juce::int64) p0[ids::start] == 50000 && (juce::int64) p1[ids::start] == 53000
                 && ! p0.hasProperty (ids::lane) && ! both.contains (pasted[0]),
               "paste lands at the playhead from the selected track down, with new ids");
        project->undo.undo();
        check (! project->regionById (pasted[0]).isValid(), "undo removes the pasted regions");

        project->undo.beginNewTransaction ("Paste");
        pasted = project->pasteRegions (clip, 0, 2);                // below the last track: clamped to C
        check (project->regionById (pasted[1]).getParent() == c, "lanes past the last track stay on the last track");
        project->undo.undo();

        {
            const auto otherFolder = folder.getSiblingFile ("woodshed-project-test-other");
            otherFolder.deleteRecursively();
            auto other = Project::createIn (otherFolder, rate);
            other->addTrack ("X");
            auto fromOther = other->pasteRegions (clip, 0, 0);
            const auto file = other->regionById (fromOther[0])[ids::file].toString();
            check (other->resolve (file).existsAsFile() && other->resolve (file).isAChildOf (other->audioFolder())
                     && other->regionById (fromOther[0])[ids::file] == other->regionById (fromOther[1])[ids::file],
                   "pasting into another project copies the audio in, once");
            otherFolder.deleteRecursively();
        }

        project->undo.beginNewTransaction ("Repeat");
        auto repeated = project->repeatRegions (both);
        auto r0 = project->regionById (repeated[0]), r1 = project->regionById (repeated[1]);
        check (r0.getParent() == a && r1.getParent() == b && (juce::int64) r0[ids::start] == 26000
                 && (juce::int64) r1[ids::start] == 29000, "repeat places a copy straight after the block");
        project->undo.undo();

        project->undo.beginNewTransaction ("Nudge");
        project->nudgeRegions (both, -30000);
        check ((juce::int64) onA[ids::start] == 0 && (juce::int64) onB[ids::start] == 3000, "nudge stops at the song start, as a block");
        project->undo.undo();
        check ((juce::int64) onA[ids::start] == 20000, "undo nudge");

        b.setProperty (ids::mute, true, nullptr);
        onA.setProperty (ids::mute, true, nullptr);
        auto r = offline (*project, pool, 40000);
        check (clicks (r.left.data(), (int) r.left.size()).empty(), "a muted region is silent");
        onA.removeProperty (ids::mute, nullptr);

        project->removeTrack (c);
        project->removeTrack (b);
    }

    std::cout << "markers\n";
    {
        const auto tracksBefore = project->numTracks();
        project->undo.beginNewTransaction ("Create Marker");
        const int verse = project->addMarker (96000);
        project->undo.beginNewTransaction ("Create Marker");
        const int intro = project->addMarker (0);
        auto markers = project->getMarkers();
        check (markers.size() == 2 && markers[0].id == intro && markers[1].id == verse && verse != intro
                 && markers[0].name == "Marker 2" && markers[1].name == "Marker 1",
               "markers come back in timeline order, numbered as they were made");
        check (project->numTracks() == tracksBefore, "markers don't count as tracks");
        check (project->addMarker (96000) == verse && project->getMarkers().size() == 2, "a second marker at the same place is the first one");

        project->undo.beginNewTransaction ("Rename Marker");
        project->renameMarker (verse, "Verse");
        project->undo.beginNewTransaction ("Move Marker");
        project->moveMarker (verse, 48000);
        check (project->nextMarkerAfter (0) == 48000 && project->previousMarkerBefore (48000) == 0
                 && project->nextMarkerAfter (48000) == -1, "next and previous marker");

        project->save();
        auto copy = Project::load (project->folder);
        check (copy != nullptr && copy->getMarkers().size() == 2 && copy->getMarkers()[1].name == "Verse",
               "markers survive save and load");

        project->undo.undo();                                       // move
        project->undo.undo();                                       // rename
        check (project->getMarkers()[1].name == "Marker 1" && project->getMarkers()[1].position == 96000, "undo marker edits");
        project->undo.beginNewTransaction ("Delete Marker");
        project->removeMarker (intro);
        project->removeMarker (verse);
        check (project->getMarkers().empty() && ! project->state.hasProperty (ids::markers), "deleting every marker clears the property");
    }

    std::cout << "pan, mute, solo\n";
    {
        a.setProperty (ids::pan, -1.0, nullptr);
        auto r = offline (*project, pool, 40000);
        check (std::abs (r.right[20500]) < 1.0e-4f && r.left[20500] > 0.6f, "hard left: nothing on the right");
        a.setProperty (ids::pan, 0.0, nullptr);

        auto b = project->addTrack ("B");
        b.setProperty (ids::rigOn, false, nullptr);
        project->addRegion (b, "Audio/clicks.wav", 30000, 0, 10000, "clicks");   // 31000, 35000
        b.setProperty (ids::mute, true, nullptr);
        r = offline (*project, pool, 40000);
        check (clicks (r.left.data(), (int) r.left.size()) == std::vector<int> { 20500, 24500 }, "muted track is silent");

        b.setProperty (ids::mute, false, nullptr);
        b.setProperty (ids::solo, true, nullptr);
        r = offline (*project, pool, 40000);
        check (clicks (r.left.data(), (int) r.left.size()) == std::vector<int> { 31000, 35000 }, "solo silences the other track");
        project->removeTrack (b);
    }

    std::cout << "live waveform while recording\n";
    {
        LivePeaks peaks (rate, 0.01);                               // room for 0.6 s
        std::vector<float> ramp (1000);
        for (size_t i = 0; i < ramp.size(); ++i)
            ramp[i] = (i % 256 == 100) ? -0.5f : 0.1f;
        peaks.add (ramp.data(), (int) ramp.size());
        check (peaks.size() == 1000 / LivePeaks::hop && std::abs (peaks[0] - 0.5f) < 1.0e-6f, "one peak per hop, rectified");

        std::vector<float> lots ((size_t) rate);
        peaks.add (lots.data(), (int) lots.size());
        check (peaks.size() == (int) (rate * 0.6 / LivePeaks::hop), "stops at capacity instead of overrunning");

        juce::AudioDeviceManager devices;
        Engine engine (devices, {});
        project->state.setProperty (ids::countIn, false, nullptr);
        project->state.setProperty (ids::loopOn, false, nullptr);
        engine.prepare (rate, 64);
        engine.setProject (project.get());
        a.setProperty (ids::arm, true, nullptr);
        engine.setPosition (0);
        engine.record();

        std::vector<float> input (64, 0.25f), l (64), r (64);
        const float* ins[] { input.data() };
        for (int block = 0; block < 40; ++block)                    // 2560 samples: 10 peaks
        {
            float* outs[] { l.data(), r.data() };
            engine.audioDeviceIOCallbackWithContext (ins, 1, outs, 2, 64, {});
        }

        const auto* live = engine.getLivePeaks ((int) a[ids::id]);
        check (live != nullptr && live->size() == 10 && std::abs ((*live)[9] - 0.25f) < 1.0e-6f,
               "the armed track's take has its waveform while it records");
        engine.stop();
        check (engine.getLivePeaks ((int) a[ids::id]) == nullptr, "no live waveform once stopped");
        engine.setProject (nullptr);
        a.setProperty (ids::arm, false, nullptr);
    }

    std::cout << "record placement\n";
    {
        juce::int64 start = 0, offset = 0;
        Engine::placeRecording (48000, 300, start, offset);
        check (start == 47700 && offset == 0, "a take moves back by the round-trip latency");
        Engine::placeRecording (100, 300, start, offset);
        check (start == 0 && offset == 200, "a take at the very start trims its first samples instead");
    }

    std::cout << "live transport and loop\n";
    {
        juce::AudioDeviceManager devices;
        Engine engine (devices, {});
        project->state.setProperty (ids::click, false, nullptr);
        project->state.setProperty (ids::loopOn, true, nullptr);
        project->state.setProperty (ids::loopStart, 20000, nullptr);
        project->state.setProperty (ids::loopEnd, 21000, nullptr);
        engine.prepare (rate, 64);
        engine.setProject (project.get());
        // Let the track faders glide up from silence (they start at zero) before playing.
        {
            std::vector<float> l (4096), r (4096);
            for (int done = 0; done < 4096; done += 64)
            {
                float* outs[] { l.data() + done, r.data() + done };
                engine.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, 64, {});
            }
        }

        engine.setPosition (20400);
        engine.play();

        std::vector<float> l (2048), r (2048);
        for (int done = 0; done < 2048; done += 64)
        {
            float* outs[] { l.data() + done, r.data() + done };
            engine.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, 64, {});
        }

        check (clicks (l.data(), 2000) == std::vector<int> { 100, 1100 }, "loop plays the click at 20500 twice, 1000 samples apart");
        check (engine.getPosition() == 20000 + (20400 + 2048 - 21000) % 1000, "playhead wrapped to the loop start ("
                                                                               + juce::String (engine.getPosition()) + ")");

        engine.stop();
        engine.setProject (nullptr);
    }

    std::cout << "armed track monitoring\n";
    {
        a.setProperty (ids::arm, true, nullptr);
        std::map<int, std::shared_ptr<TrackRuntime>> runtimes;
        auto rt = std::make_shared<TrackRuntime>();
        rt->id = (int) a[ids::id];
        rt->work.setSize (2, 64);
        rt->gainLeft.reset (rate, 0.0);
        rt->gainRight.reset (rate, 0.0);
        runtimes[rt->id] = rt;
        auto snap = Engine::buildSnapshot (*project, pool, runtimes);

        std::vector<float> input (64, 0.25f), l (64), r (64);
        const float* ins[] { input.data() };
        auto run = [&] (bool playing, bool recording)
        {
            Mixer::Block b;
            b.position = 20500 - 10;        // the take's click is in this block
            b.numSamples = 64;
            b.playing = playing;
            b.recording = recording;
            b.inputs = ins;
            b.numInputs = 1;
            b.left = l.data();
            b.right = r.data();
            Mixer::render (*snap, b);
        };

        a.setProperty (ids::input, 0, nullptr);
        snap = Engine::buildSnapshot (*project, pool, runtimes);
        run (false, false);
        check (std::abs (l[0] - 0.25f) < 0.01f, "armed and stopped: hears the live input");
        run (true, false);
        check (std::abs (l[10] - 0.5f) < 0.01f && std::abs (l[0]) < 0.01f, "armed and playing: plays the take back");
        run (true, true);
        check (std::abs (l[0] - 0.25f) < 0.01f && std::abs (l[10] - 0.25f) < 0.01f, "armed and recording: hears only the live input");
        a.setProperty (ids::arm, false, nullptr);
        a.setProperty (ids::monitor, true, nullptr);
        snap = Engine::buildSnapshot (*project, pool, runtimes);
        run (true, false);
        check (std::abs (l[0] - 0.25f) < 0.01f && std::abs (l[10] - 0.75f) < 0.01f, "input monitoring while playing: live input plus the take");
        a.setProperty (ids::monitor, false, nullptr);
    }

    std::cout << "instrument tracks and MIDI regions\n";
    {
        for (int i = project->numTracks(); --i >= 0;)
            project->track (i).setProperty (ids::mute, true, nullptr);

        auto keys = project->addInstrumentTrack ("Keys", "test.click", "Default", {});
        keys.setProperty (ids::mute, false, nullptr);
        check (Project::isInstrumentTrack (keys) && keys.getChildWithName (ids::INSTRUMENT)[ids::type] == "test.click",
               "an instrument track remembers its instrument");

        // At 120 bpm a quarter note (960 ticks) is 24000 samples.
        auto region = project->addMidiRegion (keys, 48000, 96000, "Keys");
        project->addNote (region, { 64, 127, 960, 480 });       // 72000 .. 84000
        project->addNote (region, { 60, 127, 0, 480 });         // 48000 .. 60000, sorted first
        check ((int) region.getChild (0)[ids::pitch] == 60, "notes stay in time order");

        std::map<int, std::shared_ptr<TrackRuntime>> runtimes;
        auto rt = std::make_shared<TrackRuntime>();
        rt->id = (int) keys[ids::id];
        rt->work.setSize (2, 64);
        rt->gainLeft.reset (rate, 0.0);
        rt->gainRight.reset (rate, 0.0);
        rt->events.reserve (256);
        auto inst = std::make_shared<ClickInstrument>();
        rt->instrument = inst;
        runtimes[rt->id] = rt;

        auto render = [&] (juce::int64 length)
        {
            auto snap = Engine::buildSnapshot (*project, pool, runtimes);
            std::vector<float> l ((size_t) length), r ((size_t) length);
            rt->expectedPosition = -1;
            for (juce::int64 pos = 0; pos < length; pos += 64)
            {
                Mixer::Block b;
                b.position = pos;
                b.numSamples = 64;
                b.playing = true;
                b.left = l.data() + pos;
                b.right = r.data() + pos;
                Mixer::render (*snap, b);
            }
            return l;
        };

        auto l = render (144000);
        check (clicks (l.data(), (int) l.size()) == std::vector<int> { 48000, 60000, 72000, 84000 },
               "notes start and stop on their exact samples");

        region.setProperty (ids::offset, 24000, nullptr);       // trim the start: the first note is cut away
        region.setProperty (ids::start, 72000, nullptr);
        region.setProperty (ids::length, 24000, nullptr);
        l = render (144000);
        check (clicks (l.data(), (int) l.size()) == std::vector<int> { 72000, 84000 }, "a trimmed region hides the notes outside it");

        region.setProperty (ids::length, 6000, nullptr);
        l = render (144000);
        check (clicks (l.data(), (int) l.size()) == std::vector<int> { 72000, 78000 }, "a note running past the region end stops at it");

        region.setProperty (ids::offset, 0, nullptr);
        region.setProperty (ids::start, 48000, nullptr);
        region.setProperty (ids::length, 96000, nullptr);
        auto right = project->splitRegion (region, 66000);
        l = render (144000);
        check (right.isValid() && Project::isMidiRegion (right) && right.getNumChildren() == 2
               && clicks (l.data(), (int) l.size()) == std::vector<int> { 48000, 60000, 72000, 84000 },
               "a split MIDI region plays every note once");

        // A jump mid-song silences held notes.
        {
            auto snap = Engine::buildSnapshot (*project, pool, runtimes);
            std::vector<float> buf (64);
            rt->expectedPosition = -1;
            rt->notesSounding = false;
            const auto before = inst->allOffs;
            for (juce::int64 pos : { (juce::int64) 47990, (juce::int64) 48054, (juce::int64) 10000 })
            {
                Mixer::Block b;
                b.position = pos;
                b.numSamples = 64;
                b.playing = true;
                b.left = buf.data();
                b.right = buf.data();
                Mixer::render (*snap, b);
            }
            check (inst->allOffs == before + 1, "jumping away while notes sound sends all-notes-off once");
        }

        // Inserts: a LIFT boost at +20 dB, then switched off.
        {
            auto fx = project->addInsert (keys, "dyn.boost", { 20.0f, 0.0f, 0.0f });
            rt->inserts.push_back ({ (int) fx[ids::id], "dyn.boost", woodshed::Registry::get().find ("dyn.boost")->create() });
            rt->inserts.back().pedal->prepare (rate, 64);
            l = render (60000);
            // The boost's tone filters smear a one-sample click, so its peak grows less than 10x.
            check (l[48000] > 2.0f, "an insert processes the instrument (+20 dB: peak " + juce::String (l[48000], 2) + ")");
            fx.setProperty (ids::on, false, nullptr);
            l = render (60000);
            check (std::abs (l[48000] - 1.0f) < 0.01f, "a switched-off insert is bypassed");
            project->state.getChildWithProperty (ids::id, keys[ids::id]).removeChild (fx, nullptr);
            rt->inserts.clear();
        }

        // Automation: volume from -6 dB up to 0 dB, then pan hard left.
        {
            check (Project::evaluate ({ { 0, 0.0f }, { 100, 1.0f } }, 25) > 0.24f && Project::evaluate ({ { 0, 0.0f }, { 100, 1.0f } }, 25) < 0.26f
                   && juce::exactlyEqual (Project::evaluate ({ { 10, 0.5f } }, 0), 0.5f) && juce::exactlyEqual (Project::evaluate ({ { 0, 0.0f }, { 100, 1.0f } }, 500), 1.0f),
                   "an automation line interpolates and holds its ends");
            check (Project::decodePoints (Project::encodePoints ({ { 5, 0.25f }, { 900000, -3.5f } })) == std::vector<AutoPoint> { { 5, 0.25f }, { 900000, -3.5f } },
                   "automation points survive encoding");

            project->setAutomation (keys, "volume", { { 48000, -6.0206f }, { 72000, 0.0f } });
            auto l2 = render (100000);
            check (std::abs (l2[48000] - 0.5f) < 0.02f && std::abs (l2[72000] - 1.0f) < 0.02f,
                   "volume automation sets the level at each note (" + juce::String (l2[48000], 3) + ", " + juce::String (l2[72000], 3) + ")");

            project->setAutomation (keys, "pan", { { 0, -1.0f } });
            auto snap = Engine::buildSnapshot (*project, pool, runtimes);
            std::vector<float> L (64), R (64);
            Mixer::Block b;
            b.position = 72000 - 10;
            b.numSamples = 64;
            b.playing = true;
            b.left = L.data();
            b.right = R.data();
            rt->expectedPosition = -1;
            Mixer::render (*snap, b);
            check (L[10] > 0.9f && std::abs (R[10]) < 1.0e-4f, "pan automation moves the track hard left");

            check (Project::automatableParams (keys).size() >= 2, "a track lists its automatable parameters");
            project->setAutomation (keys, "volume", {});
            project->setAutomation (keys, "pan", {});
            check (! Project::automationNode (keys, "volume").isValid(), "an emptied line is removed");
        }

        // Sends and aux tracks.
        {
            auto bus = project->addAuxTrack ("Reverb");
            auto busRt = std::make_shared<TrackRuntime>();
            busRt->id = (int) bus[ids::id];
            busRt->work.setSize (2, 64);
            busRt->busInput.setSize (2, 64);
            busRt->gainLeft.reset (rate, 0.0);
            busRt->gainRight.reset (rate, 0.0);
            runtimes[busRt->id] = busRt;

            auto send = project->addSend (keys, (int) bus[ids::id], 0.0, false);
            auto l2 = render (60000);
            check (std::abs (l2[48000] - 2.0f) < 0.02f, "a 0 dB send doubles the note through an empty aux (" + juce::String (l2[48000], 3) + ")");

            keys.setProperty (ids::volume, -100.0, nullptr);
            l2 = render (60000);
            check (std::abs (l2[48000]) < 0.001f, "a post-fader send follows the fader down");
            send.setProperty (ids::pre, true, nullptr);
            l2 = render (60000);
            check (std::abs (l2[48000] - 1.0f) < 0.02f, "a pre-fader send ignores the fader");
            keys.setProperty (ids::volume, 0.0, nullptr);
            send.setProperty (ids::pre, false, nullptr);

            project->setAutomation (keys, "send:" + bus[ids::id].toString(), { { 0, -6.0206f } });
            l2 = render (60000);
            check (std::abs (l2[48000] - 1.5f) < 0.02f, "send level automation (" + juce::String (l2[48000], 3) + ")");
            project->setAutomation (keys, "send:" + bus[ids::id].toString(), {});

            bus.setProperty (ids::mute, true, nullptr);
            l2 = render (60000);
            check (std::abs (l2[48000] - 1.0f) < 0.02f, "a muted aux plays nothing");
            project->state.removeChild (bus, nullptr);
            keys.removeChild (send, nullptr);
            runtimes.erase (busRt->id);
        }

        // The master: its node stays after the tracks, and its effects process the mix.
        {
            const int before = project->numTracks();
            auto m = project->master();
            auto added = project->addTrack ("After Master");
            check (project->numTracks() == before + 1 && project->state.getChild (project->state.getNumChildren() - 1).hasType (ids::MASTER)
                   && project->track (before) == added, "new tracks go before the MASTER node, which isn't a track");
            project->state.removeChild (added, nullptr);

            auto fx = project->addInsert (m, "dyn.boost", { 20.0f, 0.0f, 0.0f });
            TrackRuntime masterRt;
            masterRt.inserts.push_back ({ (int) fx[ids::id], "dyn.boost", woodshed::Registry::get().find ("dyn.boost")->create() });
            masterRt.inserts.back().pedal->prepare (rate, 64);
            auto snap = Engine::buildSnapshot (*project, pool, runtimes, &masterRt);
            check (snap->masterInserts.size() == 1, "the master's effects are in the snapshot");
            std::vector<float> L (64), R (64);
            Mixer::Block b;
            b.numSamples = 64;
            b.playing = true;
            b.left = L.data();
            b.right = R.data();
            rt->expectedPosition = -1;
            for (juce::int64 pos = 48000 - 10 - 64 * 40; pos <= 48000 - 10; pos += 64)     // let its gain settle
            {
                b.position = pos;
                Mixer::render (*snap, b);
            }
            check (L[10] > 2.0f, "a master effect processes the mix (" + juce::String (L[10], 2) + ")");
            project->state.removeChild (m, nullptr);
        }

        check (Project::decodeValues (Project::encodeValues ({ 1.0f, -0.5f, 2000.0f, 0.125f })) == std::vector<float> { 1.0f, -0.5f, 2000.0f, 0.125f },
               "knob values survive encoding");

        // Live notes through the engine, recorded into a new MIDI region.
        {
            juce::AudioDeviceManager devices;
            auto made = std::make_shared<int> (0);
            Engine engine (devices, {}, [made] (const juce::String& type) -> std::unique_ptr<woodshed::Instrument>
                           {
                               ++*made;
                               return type == "test.click" ? std::make_unique<ClickInstrument>() : nullptr;
                           });
            project->state.setProperty (ids::click, false, nullptr);
            project->state.setProperty (ids::countIn, false, nullptr);
            project->state.setProperty (ids::loopOn, false, nullptr);
            engine.prepare (rate, 64);
            engine.setProject (project.get());
            check (*made == 1 && engine.getInstrument ((int) keys[ids::id]) != nullptr, "the engine makes the track's instrument");

            engine.setMidiTarget ((int) keys[ids::id]);
            keys.setProperty (ids::arm, true, nullptr);
            engine.setPosition (96000 * 2);
            engine.record();

            std::vector<float> l2 (64), r2 (64);
            auto tick = [&] (int blocks) { for (int i = 0; i < blocks; ++i) { float* outs[] { l2.data(), r2.data() };
                                                                               engine.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, 64, {}); } };
            tick (10);
            engine.sendLiveMidi (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100));
            tick (10);
            engine.sendLiveMidi (juce::MidiMessage::noteOff (1, 67));
            tick (10);
            engine.stop();
            tick (4);
            engine.service();

            juce::ValueTree recorded;
            for (auto r : keys)
                if (r.hasType (ids::REGION) && (juce::int64) r[ids::start] >= 96000 * 2)
                    recorded = r;
            check (recorded.isValid() && Project::isMidiRegion (recorded) && recorded.getNumChildren() == 1
                   && (int) recorded.getChild (0)[ids::pitch] == 67 && (int) recorded.getChild (0)[ids::velocity] == 100,
                   "recording a live note makes a MIDI region holding it");
            check (recorded.isValid() && (juce::int64) recorded[ids::start] == 192000 && (juce::int64) recorded[ids::length] == 96000,
                   "the recorded region spans whole bars");
            engine.setProject (nullptr);
        }
    }

    folder.deleteRecursively();
    std::cout << (failures == 0 ? "all project checks passed\n" : juce::String (failures) + " project checks failed\n");
    return failures == 0 ? 0 : 1;
}

/** woodshed-bench demo <folder>: a small song made from the test DI, for screenshots and UI work. */
int writeDemoProject (const juce::File& folder, const juce::File& di);
int writeDemoProject (const juce::File& folder, const juce::File& di)
{
    juce::ScopedJuceInitialiser_GUI init;
    folder.deleteRecursively();
    auto project = Project::createIn (folder, rate);
    project->state.setProperty (ids::name, "Demo Song", nullptr);
    project->state.setProperty (ids::tempo, 96.0, nullptr);

    if (! di.copyFileTo (project->audioFolder().getChildFile ("Guitar DI.wav")))
    {
        std::cerr << "can't copy " << di.getFullPathName() << "\n";
        return 1;
    }

    AudioPool pool;
    auto audio = pool.get (project->audioFolder().getChildFile ("Guitar DI.wav"));
    const auto diLength = (juce::int64) audio->getNumSamples();
    const auto bar = (juce::int64) project->grid().samplesPerBar();

    struct Part { const char* name; std::vector<std::pair<int, int>> bars; double pan; double volume; };
    const Part parts[] {
        { "Rhythm Left",  { { 0, 4 }, { 4, 4 }, { 8, 4 }, { 12, 4 } }, -0.6, -2.0 },
        { "Rhythm Right", { { 0, 4 }, { 4, 4 }, { 8, 4 }, { 12, 4 } },  0.6, -2.0 },
        { "Lead",         { { 4, 2 }, { 7, 3 }, { 12, 4 } },             0.0,  0.0 },
        { "Ambient",      { { 0, 8 }, { 8, 8 } },                        0.0, -8.0 },
    };

    int n = 0;
    for (auto& part : parts)
    {
        auto t = project->addTrack (part.name);
        t.setProperty (ids::pan, part.pan, nullptr);
        t.setProperty (ids::volume, part.volume, nullptr);

        for (auto [startBar, bars] : part.bars)
        {
            const auto length = juce::jmin (diLength, bars * bar);
            const auto offset = ((juce::int64) (n++ * 37) * 4801) % juce::jmax ((juce::int64) 1, diLength - length);
            project->addRegion (t, "Audio/Guitar DI.wav", startBar * bar, offset, length, part.name);
        }
    }

    project->state.setProperty (ids::selectedTrack, project->track (0)[ids::id], nullptr);
    project->state.setProperty (ids::loopStart, 4 * bar, nullptr);
    project->state.setProperty (ids::loopEnd, 8 * bar, nullptr);
    project->undo.clearUndoHistory();
    const bool ok = project->save();
    std::cout << (ok ? "wrote " : "couldn't write ") << project->projectFile().getFullPathName() << "\n";
    return ok ? 0 : 1;
}

/** woodshed-bench demo-midi <folder>: a short song on software instruments, for screenshots and UI work. */
int writeMidiDemoProject (const juce::File& folder);
int writeMidiDemoProject (const juce::File& folder)
{
    juce::ScopedJuceInitialiser_GUI init;
    folder.deleteRecursively();
    auto project = Project::createIn (folder, rate);
    project->state.setProperty (ids::name, "Instrument Demo", nullptr);
    project->state.setProperty (ids::tempo, 100.0, nullptr);
    const auto grid = project->grid();
    const auto bar = (juce::int64) std::llround (grid.samplesPerBar());
    constexpr int q = ticksPerQuarter;

    auto addInstrument = [&] (const juce::String& name, const juce::String& type, const juce::String& patchName)
    {
        auto* t = woodshed::InstrumentRegistry::get().find (type);
        const woodshed::InstrumentInfo::Patch* patch = nullptr;
        if (t != nullptr)
            for (auto& p : t->info.patches)
                if (p.name == patchName)
                    patch = &p;
        return project->addInstrumentTrack (name, type, patch != nullptr ? patch->name : juce::String(),
                                            t != nullptr ? woodshed::patchValues (t->info, patch) : std::vector<float>());
    };

    // C - Am - F - G, twice.
    const int roots[] { 48, 45, 41, 43 };
    const std::vector<std::vector<int>> chords { { 60, 64, 67 }, { 57, 60, 64 }, { 53, 57, 60 }, { 55, 59, 62 } };

    auto keys = addInstrument ("Keys", "synth.analog", "Classic Poly");
    auto keysRegion = project->addMidiRegion (keys, 0, bar * 8, "Chords");
    for (int b = 0; b < 8; ++b)
        for (int beat = 0; beat < 4; beat += 2)
            for (int pitch : chords[(size_t) (b % 4)])
                project->addNote (keysRegion, { pitch, beat == 0 ? 96 : 76, (juce::int64) (b * 4 + beat) * q, (juce::int64) q * 2 - 40 });

    auto bass = addInstrument ("Bass", "synth.analog", "Sub Bass");
    auto bassRegion = project->addMidiRegion (bass, 0, bar * 8, "Bass");
    for (int b = 0; b < 8; ++b)
        for (int eighth = 0; eighth < 8; ++eighth)
            project->addNote (bassRegion, { roots[b % 4] - 12 + (eighth == 6 ? 12 : 0), eighth % 2 == 0 ? 110 : 80,
                                            (juce::int64) (b * 8 + eighth) * q / 2, (juce::int64) q / 2 - 60 });

    auto lead = addInstrument ("Lead", "synth.analog", "Saw Lead");
    auto leadRegion = project->addMidiRegion (lead, bar * 4, bar * 4, "Melody");
    const int melody[] { 72, 74, 76, 79, 76, 74, 72, 69, 72, 76, 77, 76, 74, 71, 74, 79 };
    for (int i = 0; i < 16; ++i)
        project->addNote (leadRegion, { melody[i], 100, (juce::int64) i * q, (juce::int64) q - 60 });
    if (auto* reverb = woodshed::Registry::get().find ("reverb.hall"))
    {
        std::vector<float> defaults;
        for (auto& k : reverb->info.knobs)
            defaults.push_back (k.def);
        project->addInsert (lead, "reverb.hall", defaults);
    }

    if (woodshed::InstrumentRegistry::get().find ("drums.kit") != nullptr)
    {
        auto drums = addInstrument ("Drums", "drums.kit", {});
        auto beat = project->addMidiRegion (drums, 0, bar * 8, "Beat");
        for (int b = 0; b < 8; ++b)
            for (int s = 0; s < 16; ++s)
            {
                const auto at = (juce::int64) (b * 16 + s) * q / 4;
                if (s == 0 || s == 8 || s == 10) project->addNote (beat, { 36, 115, at, q / 4 });
                if (s == 4 || s == 12)           project->addNote (beat, { 38, 110, at, q / 4 });
                if (s % 2 == 0)                  project->addNote (beat, { 42, s % 4 == 0 ? 90 : 65, at, q / 4 });
            }
    }

    // A shared reverb on an aux, fed by the keys and the lead.
    auto verb = project->addAuxTrack ("Reverb");
    if (auto* hall = woodshed::Registry::get().find ("reverb.hall"))
    {
        std::vector<float> values;
        for (auto& k : hall->info.knobs)
            values.push_back (k.id == "mix" ? k.max : k.def);
        project->addInsert (verb, "reverb.hall", values);
    }
    project->addSend (keys, (int) verb[ids::id], -12.0);
    project->addSend (lead, (int) verb[ids::id], -8.0);

    // Automation: the chords fade in over two bars; the lead's filter opens up.
    project->setAutomation (keys, "volume", { { 0, -30.0f }, { bar * 2, 0.0f } });
    int cutoff = 0;
    if (auto* t = woodshed::InstrumentRegistry::get().find ("synth.analog"))
        for (size_t k = 0; k < t->info.knobs.size(); ++k)
            if (t->info.knobs[k].id == "cutoff")
                cutoff = (int) k;
    const auto cutoffParam = "inst:" + juce::String (cutoff);
    project->setAutomation (lead, cutoffParam, { { bar * 4, 800.0f }, { bar * 6, 6000.0f }, { bar * 8, 2000.0f } });
    lead.setProperty (ids::autoParam, cutoffParam, nullptr);
    project->setAutomation (bass, "pan", { { 0, -0.5f }, { bar * 4, 0.5f }, { bar * 8, -0.5f } });
    bass.setProperty (ids::autoParam, "pan", nullptr);

    project->state.setProperty (ids::selectedTrack, keys[ids::id], nullptr);
    if (! project->save())
        return 1;
    std::cout << "wrote " << project->projectFile().getFullPathName() << "\n";
    return 0;
}

/** woodshed-bench render-project <folder> <out.wav>: the whole song, offline, through each
    track's instrument and inserts (audio tracks play dry: the guitar rig is the app's). */
int renderProject (const juce::File& folder, const juce::File& out);
int renderProject (const juce::File& folder, const juce::File& out)
{
    juce::ScopedJuceInitialiser_GUI init;
    auto project = Project::load (folder);
    if (project == nullptr)
    {
        std::cerr << "can't open " << folder.getFullPathName() << "\n";
        return 1;
    }

    constexpr int block = 512;
    AudioPool pool;
    std::map<int, std::shared_ptr<TrackRuntime>> runtimes;
    for (auto t : project->state)
    {
        if (! t.hasType (ids::TRACK)) continue;
        auto rt = std::make_shared<TrackRuntime>();
        rt->id = (int) t[ids::id];
        rt->work.setSize (2, block);
        rt->gainLeft.reset (rate, 0.03);
        rt->gainRight.reset (rate, 0.03);
        rt->events.reserve (4096);
        if (Project::isInstrumentTrack (t))
            if (auto* type = woodshed::InstrumentRegistry::get().find (t.getChildWithName (ids::INSTRUMENT)[ids::type].toString()))
            {
                rt->instrument = type->create();
                rt->instrument->prepare (rate, block);
            }
        for (auto fx : Project::inserts (t))
            if (auto* type = woodshed::Registry::get().find (fx[ids::type].toString()))
            {
                std::shared_ptr<woodshed::Pedal> pedal = type->create();
                pedal->prepare (rate, block);
                rt->inserts.push_back ({ (int) fx[ids::id], fx[ids::type].toString(), pedal });
            }
        runtimes[rt->id] = rt;
    }

    auto snap = Engine::buildSnapshot (*project, pool, runtimes);
    for (auto& t : snap->tracks)
    {
        t.runtime->gainLeft.setCurrentAndTargetValue (t.audible ? t.gain : 0.0f);
        t.runtime->gainRight.setCurrentAndTargetValue (t.audible ? t.gain : 0.0f);
    }

    const auto length = project->contentEnd() + (juce::int64) (rate * 3.0);
    juce::AudioBuffer<float> buf (2, (int) length);
    buf.clear();
    for (juce::int64 pos = 0; pos < length; pos += block)
    {
        Mixer::Block b;
        b.position = pos;
        b.numSamples = (int) juce::jmin ((juce::int64) block, length - pos);
        b.playing = true;
        b.left = buf.getWritePointer (0, (int) pos);
        b.right = buf.getWritePointer (1, (int) pos);
        Mixer::render (*snap, b);
    }
    buf.applyGain (snap->masterGain);

    out.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (out.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples()))
        return 1;
    std::cout << "wrote " << out.getFullPathName() << "  peak " << juce::Decibels::gainToDecibels (buf.getMagnitude (0, buf.getNumSamples()))
              << " dBFS, rms " << juce::Decibels::gainToDecibels (buf.getRMSLevel (0, 0, buf.getNumSamples())) << " dBFS\n";
    return 0;
}
