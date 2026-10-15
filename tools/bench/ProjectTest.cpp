// woodshed-bench project: checks the multitrack engine without an audio interface.
// A test recording with clicks at known samples is placed on tracks, mixed offline and through
// the live callback, and every click must land on exactly the sample the timeline says.

#include "../../Source/daw/Engine.h"
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
