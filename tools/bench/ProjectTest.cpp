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
