// woodshed-bench mix: the mixing and recording infrastructure, offline. Track Stacks (summing and
// folder), groups and VCAs, plug-in delay compensation, track freeze and Autopunch, each checked
// on real audio: impulses must land on the right sample at the right level.

#include "../../Source/daw/Engine.h"
#include "../../Source/daw/Stacks.h"
#include "../../Source/daw/Groups.h"
#include <iostream>

using namespace woodshed::daw;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 64;
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    failures += ok ? 0 : 1;
}

bool near (float a, float b, float tolerance = 0.002f)     { return std::abs (a - b) <= tolerance; }
float gainOf (double db)                                   { return juce::Decibels::decibelsToGain ((float) db); }

void writeMono (const juce::File& file, const juce::AudioBuffer<float>& buf)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (32)
                                                                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

/** Peaks above `threshold`, as sample indices. */
std::vector<int> impulses (const std::vector<float>& data, float threshold = 0.05f)
{
    std::vector<int> at;
    for (int i = 0; i < (int) data.size(); ++i)
        if (std::abs (data[(size_t) i]) > threshold && (at.empty() || i - at.back() > 50))
            at.push_back (i);
    return at;
}

std::shared_ptr<TrackRuntime> makeRuntime (int id)
{
    auto rt = std::make_shared<TrackRuntime>();
    rt->id = id;
    rt->work.setSize (2, 512);             // room for a bounce's blocks too
    rt->busInput.setSize (2, 512);
    rt->preFader.allocate();
    rt->postFader.allocate();
    rt->gainLeft.reset (rate, 0.0);
    rt->gainRight.reset (rate, 0.0);
    return rt;
}

using Runtimes = std::map<int, std::shared_ptr<TrackRuntime>>;

Runtimes runtimesFor (const Project& project)
{
    Runtimes runtimes;
    for (int i = 0; i < project.numTracks(); ++i)
    {
        const int id = (int) project.track (i)[ids::id];
        runtimes[id] = makeRuntime (id);
    }
    return runtimes;
}

struct Render { std::vector<float> left, right; };

Render render (const Snapshot& snap, juce::int64 length)
{
    Render r { std::vector<float> ((size_t) length), std::vector<float> ((size_t) length) };
    for (juce::int64 pos = 0; pos < length; pos += block)
    {
        Mixer::Block b;
        b.position = pos;
        b.numSamples = (int) juce::jmin ((juce::int64) block, length - pos);
        b.playing = true;
        b.left = r.left.data() + pos;
        b.right = r.right.data() + pos;
        Mixer::render (snap, b);
    }
    return r;
}

Render render (const Project& project, AudioPool& pool, juce::int64 length, Runtimes runtimes = {})
{
    if (runtimes.empty())
        runtimes = runtimesFor (project);
    auto snap = Engine::buildSnapshot (project, pool, runtimes);
    return render (*snap, length);
}

/** A pedal that delays its input by a fixed number of samples and says so. */
class LatentPedal final : public woodshed::Pedal
{
public:
    explicit LatentPedal (int samples) : delay (samples) {}
    void reset() override                               { for (auto& l : lines) l.assign ((size_t) delay + 1, 0.0f); at = { 0, 0 }; }
    int getLatencySamples() const override              { return delay; }
    void processChannel (int channel, float* data, int n) override
    {
        auto& line = lines[(size_t) channel];
        auto& w = at[(size_t) channel];
        for (int i = 0; i < n; ++i)
        {
            line[(size_t) w] = data[i];
            w = (w + 1) % (delay + 1);
            data[i] = line[(size_t) w];
        }
    }

private:
    int delay;
    std::array<std::vector<float>, 2> lines;
    std::array<int, 2> at {};
};

/** A deterministic instrument: a decaying sine per note. Counts its renders, so a frozen track
    can be seen to skip it. */
class SineInstrument final : public woodshed::Instrument
{
public:
    static inline std::atomic<int> renders { 0 };

    void reset() override                               { voices.clear(); }
    void render (float* left, float* right, int n, const woodshed::NoteEvent* events, int numEvents) override
    {
        ++renders;
        int e = 0;
        for (int i = 0; i < n; ++i)
        {
            for (; e < numEvents && events[e].offset <= i; ++e)
                if (events[e].kind == woodshed::NoteEvent::noteOn)
                    voices.push_back ({ 440.0 * std::pow (2.0, (events[e].note - 69) / 12.0), 0.0, events[e].value * 0.5f });
            float v = 0.0f;
            for (auto& voice : voices)
            {
                v += voice.level * (float) std::sin (voice.phase);
                voice.phase += juce::MathConstants<double>::twoPi * voice.hz / sampleRate;
                voice.level *= 0.99995f;
            }
            left[i] += v;
            right[i] += v * 0.5f;       // something stereo, so pan and channels are checked
        }
    }

private:
    struct Voice { double hz, phase; float level; };
    std::vector<Voice> voices;
};

/** Runs the engine's audio callback for `samples`, feeding a constant input, returning the output. */
Render runEngine (Engine& engine, int samples, float input = 0.0f)
{
    Render r { std::vector<float> ((size_t) samples), std::vector<float> ((size_t) samples) };
    std::vector<float> in ((size_t) block, input);
    const float* ins[] { in.data() };
    for (int pos = 0; pos < samples; pos += block)
    {
        float* outs[] { r.left.data() + pos, r.right.data() + pos };
        engine.audioDeviceIOCallbackWithContext (ins, 1, outs, 2, juce::jmin (block, samples - pos), {});
    }
    return r;
}
}

int runMixTest (const juce::StringArray&);
int runMixTest (const juce::StringArray&)
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("woodshed-mix-test");
    folder.deleteRecursively();

    auto project = Project::createIn (folder, rate);
    project->audioFolder().createDirectory();
    {
        juce::AudioBuffer<float> clicks (1, 10000);
        clicks.clear();
        clicks.setSample (0, 1000, 0.5f);
        clicks.setSample (0, 5000, 0.5f);
        writeMono (project->audioFolder().getChildFile ("clicks.wav"), clicks);

        juce::AudioBuffer<float> dc (1, 100000);
        for (int i = 0; i < dc.getNumSamples(); ++i)
            dc.setSample (0, i, 0.5f);
        writeMono (project->audioFolder().getChildFile ("dc.wav"), dc);
    }
    AudioPool pool;

    auto a = project->addTrack ("A");
    auto b = project->addTrack ("B");
    for (auto t : { a, b })
    {
        t.setProperty (ids::rigOn, false, nullptr);
        project->addRegion (t, "Audio/clicks.wav", 0, 0, 10000, "clicks");
    }
    const int idA = (int) a[ids::id], idB = (int) b[ids::id];

    //==============================================================================
    std::cout << "summing stack\n";
    {
        project->undo.beginNewTransaction ("Create Track Stack");
        auto main = stacks::create (*project, { idA, idB }, stacks::Type::summing);
        check (stacks::isSumming (main) && project->state.indexOf (main) == 0 && project->state.indexOf (a) == 1
                 && project->state.indexOf (b) == 2 && (int) a[ids::stack] == (int) main[ids::id],
               "the main goes where the first track was, its members right under it");
        check (stacks::members (*project, main).size() == 2 && stacks::depth (*project, a) == 1, "both tracks are members");

        main.setProperty (ids::volume, -6.0, nullptr);
        auto r = render (*project, pool, 8000);
        check (impulses (r.left) == std::vector<int> { 1000, 5000 } && near (r.left[1000], 1.0f * gainOf (-6.0)),
               "members play through the main's fader (" + juce::String (r.left[1000], 4) + ")");

        main.setProperty (ids::mute, true, nullptr);
        r = render (*project, pool, 8000);
        check (std::abs (r.left[1000]) < 1.0e-6f, "muting the main silences its members");
        main.setProperty (ids::mute, false, nullptr);

        a.setProperty (ids::solo, true, nullptr);
        r = render (*project, pool, 8000);
        check (near (r.left[1000], 0.5f * gainOf (-6.0)), "soloing a member keeps the main playing, the other member silent");
        a.setProperty (ids::solo, false, nullptr);

        // A main with an insert: the members' sum goes through it.
        {
            auto runtimes = runtimesFor (*project);
            auto fx = project->addInsert (main, "test.latent", {});
            runtimes[(int) main[ids::id]]->inserts.push_back ({ (int) fx[ids::id], "test.latent", std::make_shared<LatentPedal> (100) });
            runtimes[(int) main[ids::id]]->inserts.back().pedal->prepare (rate, block);
            r = render (*project, pool, 8000, runtimes);
            check (impulses (r.left) == std::vector<int> { 1100, 5100 }, "the members' sum goes through the main's inserts");
            main.removeChild (fx, nullptr);
        }

        // Nested: a stack of (the stack, C): the inner main plays before the outer.
        project->undo.beginNewTransaction ("New Track");
        auto c = project->addTrack ("C");
        c.setProperty (ids::rigOn, false, nullptr);
        project->addRegion (c, "Audio/clicks.wav", 0, 0, 10000, "clicks");
        project->undo.beginNewTransaction ("Create Track Stack");
        auto outer = stacks::create (*project, { (int) main[ids::id], (int) c[ids::id] }, stacks::Type::summing);
        outer.setProperty (ids::volume, -6.0, nullptr);
        check ((int) main[ids::stack] == (int) outer[ids::id] && stacks::depth (*project, a) == 2
                 && project->state.indexOf (outer) == 0 && stacks::descendants (*project, outer).size() == 4,
               "a stack inside a stack");
        r = render (*project, pool, 8000);
        const float expected = 0.5f * (2.0f * gainOf (-12.0) + gainOf (-6.0));
        check (near (r.left[1000], expected), "a nested stack sums through both mains (" + juce::String (r.left[1000], 4) + ")");

        outer.setProperty (ids::collapsed, true, nullptr);
        check (stacks::isHidden (*project, a) && stacks::isHidden (*project, c) && ! stacks::isHidden (*project, outer),
               "a collapsed stack hides everything inside it");

        // Deleting a main un-stacks its members, and undo brings it all back.
        project->undo.beginNewTransaction ("Delete Track");
        project->removeTrack (outer);
        check (! main.hasProperty (ids::stack) && ! c.hasProperty (ids::stack) && (int) a[ids::stack] == (int) main[ids::id],
               "deleting a main takes its members out of the stack, keeping the inner one");
        project->undo.undo();
        check ((int) c[ids::stack] == (int) project->track (0)[ids::id], "undo puts the main and its members back");
        project->undo.undo();       // the outer stack
        project->undo.undo();       // C
        project->undo.undo();       // the inner stack
        check (project->numTracks() == 2 && ! a.hasProperty (ids::stack) && project->state.indexOf (a) == 0,
               "undoing Create Track Stack restores the flat list");
    }

    //==============================================================================
    std::cout << "folder stack\n";
    {
        project->undo.beginNewTransaction ("Create Track Stack");
        auto main = stacks::create (*project, { idA, idB }, stacks::Type::folder);
        main.setProperty (ids::volume, -6.0, nullptr);
        auto runtimes = runtimesFor (*project);
        auto snap = Engine::buildSnapshot (*project, pool, runtimes);
        check (snap->tracks.size() == 2, "a folder main is not an audio path");
        auto r = render (*snap, 8000);
        check (near (r.left[1000], 1.0f * gainOf (-6.0)), "the folder main's fader scales its members");
        project->undo.undo();
    }

    //==============================================================================
    std::cout << "groups\n";
    {
        groups::Linker linker (*project);
        const int g = groups::add (*project, "Drums");
        groups::assign (*project, a, g);
        groups::assign (*project, b, g);
        check (g == 1 && groups::members (*project, g).size() == 2 && groups::find (*project, g)->name == "Drums", "a group of two");

        groups::assign (*project, b, 0);                    // a different level, set outside the group
        b.setProperty (ids::volume, -6.0, nullptr);
        groups::assign (*project, b, g);

        project->undo.beginNewTransaction ("Volume");
        a.setProperty (ids::volume, 2.0, &project->undo);
        check (near ((float) (double) b[ids::volume], -4.0f, 1.0e-4f), "a fader move moves the group by the same dB (relative)");

        project->undo.beginNewTransaction ("Mute");
        b.setProperty (ids::mute, true, &project->undo);
        check ((bool) a[ids::mute], "mute follows the group");
        project->undo.undo();
        check (! (bool) a[ids::mute] && ! (bool) b[ids::mute], "one undo step takes back the whole group's mute");
        project->undo.redo();
        check ((bool) a[ids::mute] && (bool) b[ids::mute], "and redo brings it back, without linking twice");
        a.setProperty (ids::mute, false, nullptr);

        auto list = groups::get (*project);
        list[0].mute = false;
        groups::set (*project, list);
        b.setProperty (ids::mute, true, nullptr);
        check (! (bool) a[ids::mute], "a group can stop linking mute");
        b.setProperty (ids::mute, false, nullptr);
        a.setProperty (ids::arm, true, nullptr);
        check ((bool) b[ids::arm], "record-arm follows the group");
        a.setProperty (ids::arm, false, nullptr);

        int last = 0;
        for (int i = 0; i < 40; ++i)
            if (auto id = groups::add (*project); id > 0)
                last = id;
        check (last == groups::maxGroups && groups::add (*project) == 0 && (int) groups::get (*project).size() == groups::maxGroups,
               "up to 32 groups");
        groups::remove (*project, g);
        check (groups::groupOf (a) == 0 && ! groups::find (*project, g).has_value(), "removing a group takes its tracks out");
        project->state.removeProperty (ids::groups, nullptr);
        a.setProperty (ids::volume, 0.0, nullptr);
        b.setProperty (ids::volume, 0.0, nullptr);
    }

    //==============================================================================
    std::cout << "VCA\n";
    {
        auto vca = stacks::addVca (*project);
        a.setProperty (ids::vca, vca[ids::id], nullptr);
        b.setProperty (ids::mute, true, nullptr);
        vca.setProperty (ids::volume, -6.0, nullptr);

        auto runtimes = runtimesFor (*project);
        auto snap = Engine::buildSnapshot (*project, pool, runtimes);
        check (std::none_of (snap->tracks.begin(), snap->tracks.end(), [&] (auto& tp) { return tp.runtime->id == (int) vca[ids::id]; }),
               "a VCA is not an audio path");
        auto r = render (*snap, 8000);
        check (near (r.left[1000], 0.5f * gainOf (-6.0)) && stacks::assigned (*project, vca).size() == 1, "the VCA's fader is a dB offset on its track");

        project->setAutomation (vca, "volume", { { 0, -20.0f }, { 10000, 0.0f } });
        r = render (*project, pool, 8000);
        check (near (r.left[1000], 0.5f * gainOf (-18.0)) && near (r.left[5000], 0.5f * gainOf (-10.0)),
               "VCA automation scales its track over time (" + juce::String (juce::Decibels::gainToDecibels (r.left[5000] / 0.5f), 2) + " dB)");

        vca.setProperty (ids::mute, true, nullptr);
        r = render (*project, pool, 8000);
        check (std::abs (r.left[1000]) < 1.0e-6f, "muting the VCA mutes its tracks");

        project->undo.beginNewTransaction ("Delete Track");
        project->removeTrack (vca);
        check (! a.hasProperty (ids::vca), "deleting a VCA frees its tracks");
        b.setProperty (ids::mute, false, nullptr);
    }

    //==============================================================================
    std::cout << "delay compensation\n";
    {
        constexpr int latency = 300;
        auto runtimes = runtimesFor (*project);
        auto fx = project->addInsert (a, "test.latent", {});
        runtimes[idA]->inserts.push_back ({ (int) fx[ids::id], "test.latent", std::make_shared<LatentPedal> (latency) });
        runtimes[idA]->inserts.back().pedal->prepare (rate, block);

        auto snap = Engine::buildSnapshot (*project, pool, runtimes);
        auto r = render (*snap, 8000);
        check (snap->latency == latency, "the mix is as late as its longest path (" + juce::String (snap->latency) + ")");
        check (impulses (r.left) == std::vector<int> { 1000 + latency, 5000 + latency } && near (r.left[1000 + latency], 1.0f, 1.0e-4f),
               "a latent track and a dry one land on the same sample at the master");

        // Without compensation they would be apart: the dry track alone is early by the latency.
        snap->tracks[1].outputDelay = 0;
        for (auto& t : snap->tracks)
            t.runtime->postFader.clear();
        r = render (*snap, 8000);
        check (impulses (r.left).size() == 4, "(uncompensated, the same tracks are two clicks apart)");

        // Through an aux with a latent insert: a dry track sending to it lines up its direct path.
        auto bus = project->addAuxTrack ("Bus");
        runtimes[(int) bus[ids::id]] = makeRuntime ((int) bus[ids::id]);
        auto busFx = project->addInsert (bus, "test.latent", {});
        runtimes[(int) bus[ids::id]]->inserts.push_back ({ (int) busFx[ids::id], "test.latent", std::make_shared<LatentPedal> (200) });
        runtimes[(int) bus[ids::id]]->inserts.back().pedal->prepare (rate, block);
        project->addSend (b, (int) bus[ids::id], 0.0);
        a.setProperty (ids::mute, true, nullptr);
        for (auto& [id, rt] : runtimes)
            rt->postFader.clear();
        snap = Engine::buildSnapshot (*project, pool, runtimes);
        r = render (*snap, 8000);
        check (snap->latency == latency && impulses (r.left) == std::vector<int> { 1000 + latency, 5000 + latency }
                 && near (r.left[1000 + latency], 1.0f, 1.0e-4f),
               "a send through a latent aux lines up with its track's direct path and the other track's");

        // Every path's latency is still counted while a track is muted, so muting doesn't shift time.
        a.setProperty (ids::mute, false, nullptr);
        b.removeChild (Project::sends (b)[0], nullptr);
        project->removeTrack (bus);
        runtimes.erase ((int) bus[ids::id]);

        // A bounce drops the latency, so the file lines up with the timeline.
        for (auto& [id, rt] : runtimes)
        {
            rt->postFader.clear();
            for (auto& ins : rt->inserts)
                ins.pedal->reset();
        }
        snap = Engine::buildSnapshot (*project, pool, runtimes);
        const auto file = folder.getChildFile ("pdc.wav");
        BounceOptions o;
        o.bits = 32;
        Engine::renderToFile (*snap, file, 0, 8000, o, rate);
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        std::vector<float> bounced (8000);
        if (reader != nullptr)
        {
            juce::AudioBuffer<float> buf (2, 8000);
            reader->read (&buf, 0, 8000, 0, true, true);
            std::copy (buf.getReadPointer (0), buf.getReadPointer (0) + 8000, bounced.begin());
        }
        check (reader != nullptr && reader->lengthInSamples == 8000 && impulses (bounced) == std::vector<int> { 1000, 5000 },
               "a bounce takes the compensation out: clicks where the timeline has them");

        a.removeChild (fx, nullptr);
    }

    //==============================================================================
    std::cout << "freeze\n";
    {
        juce::AudioDeviceManager devices;
        Engine engine (devices, {}, [] (const juce::String& type) -> std::unique_ptr<woodshed::Instrument>
                       { return type == "test.sine" ? std::make_unique<SineInstrument>() : nullptr; });
        project->state.setProperty (ids::click, false, nullptr);
        project->state.setProperty (ids::countIn, false, nullptr);
        a.setProperty (ids::mute, true, nullptr);
        b.setProperty (ids::mute, true, nullptr);

        auto keys = project->addInstrumentTrack ("Keys", "test.sine", {}, {});
        keys.setProperty (ids::volume, -3.0, nullptr);
        keys.setProperty (ids::pan, 0.4, nullptr);
        auto region = project->addMidiRegion (keys, 0, 96000, "Keys");
        project->addNote (region, { 60, 100, 0, 960 });
        project->addNote (region, { 67, 90, 960, 960 });
        project->addNote (region, { 64, 80, 1920, 1920 });
        project->addInsert (keys, "dyn.limiter", {});        // a look-ahead limiter: latent

        constexpr int length = 96000;
        engine.prepare (rate, block);
        engine.setProject (project.get());
        engine.play();
        const auto liveLatency = engine.getPlaybackLatency();
        auto live = runEngine (engine, length);
        engine.stop();
        const int liveRenders = SineInstrument::renders.load();

        const auto file = engine.freezeFileFor (keys);
        check (engine.renderFreeze (keys, file) && file.existsAsFile(), "freezing renders the track to the project's Freeze folder");
        keys.setProperty (ids::freezeFile, file.getRelativePathFrom (project->folder), nullptr);
        keys.setProperty (ids::frozen, true, nullptr);

        engine.setProject (nullptr);
        engine.setProject (project.get());
        engine.setPosition (0);
        engine.play();
        SineInstrument::renders = 0;
        const auto frozenLatency = engine.getPlaybackLatency();
        auto frozen = runEngine (engine, length);
        engine.stop();

        float peakLive = 0.0f, worst = 0.0f;
        const int span = length - liveLatency - 4800;
        for (int i = 2400; i < span; ++i)       // past the faders' 30 ms start-up ramp
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto& l = ch == 0 ? live.left : live.right;
                const auto& f = ch == 0 ? frozen.left : frozen.right;
                peakLive = juce::jmax (peakLive, std::abs (l[(size_t) (i + liveLatency)]));
                worst = juce::jmax (worst, std::abs (l[(size_t) (i + liveLatency)] - f[(size_t) (i + frozenLatency)]));
            }
        const float diffDb = juce::Decibels::gainToDecibels (worst / juce::jmax (1.0e-9f, peakLive), -200.0f);
        check (liveRenders > 0 && peakLive > 0.1f && diffDb < -60.0f,
               "the frozen track sounds like the live one, through its fader and pan (" + juce::String (diffDb, 1) + " dB)");
        check (liveLatency > 0 && frozenLatency == 0, "the frozen render already has its latency taken out ("
                                                       + juce::String (liveLatency) + " live)");
        check (SineInstrument::renders.load() == 0, "a frozen track's instrument doesn't run");

        engine.unfreezeTrack (keys);
        check (! (bool) keys[ids::frozen], "unfreeze plays it live again");
        project->undo.undo();
        check ((bool) keys[ids::frozen], "and unfreezing is undoable");
        engine.setProject (nullptr);
        project->removeTrack (keys);
        a.setProperty (ids::mute, false, nullptr);
        b.setProperty (ids::mute, false, nullptr);
    }

    //==============================================================================
    std::cout << "autopunch\n";
    {
        a.setProperty (ids::mute, true, nullptr);
        b.setProperty (ids::mute, true, nullptr);
        auto t = project->addTrack ("Vocal");
        t.setProperty (ids::rigOn, false, nullptr);
        auto old = project->addRegion (t, "Audio/dc.wav", 0, 0, 100000, "old take");
        const auto before = render (*project, pool, 100000);

        project->state.setProperty (ids::punchOn, true, nullptr);
        project->state.setProperty (ids::punchIn, 30000, nullptr);
        project->state.setProperty (ids::punchOut, 60000, nullptr);
        project->state.setProperty (ids::loopOn, false, nullptr);
        t.setProperty (ids::arm, true, nullptr);

        juce::AudioDeviceManager devices;
        Engine engine (devices, {});
        engine.prepare (rate, block);
        engine.setProject (project.get());
        engine.setPosition (0);
        engine.record();
        auto heard = runEngine (engine, 80000, 0.25f);
        engine.stop();
        runEngine (engine, block * 4);
        engine.service();

        check (near (heard.left[10000], 0.5f) && near (heard.left[45000], 0.25f) && near (heard.left[70000], 0.5f),
               "while punching, the old take plays outside the range and the input inside it");

        juce::Array<juce::ValueTree> regions;
        for (auto r : t)
            if (r.hasType (ids::REGION))
                regions.add (r);
        juce::ValueTree take;
        for (auto& r : regions)
            if (r != old && r[ids::file].toString().contains ("take"))
                take = r;
        check (take.isValid() && (juce::int64) take[ids::start] == 30000 - 240 && (juce::int64) take[ids::length] == 30480,
               "the take covers just the punch range (and the crossfades)");
        check (regions.size() == 3 && (juce::int64) old[ids::length] == 30240 && (int) old[ids::fadeOut] == 480,
               "the old region is cut at the punch-in, fading out under the take");

        const auto after = render (*project, pool, 100000);
        bool outsideSame = true;
        for (int i = 0; i < 100000; ++i)
            if ((i < 29760 || i >= 60240) && ! juce::exactlyEqual (before.left[(size_t) i], after.left[(size_t) i]))
                outsideSame = false;
        bool insideTake = true;
        for (int i = 30240; i < 59760; ++i)
            insideTake = insideTake && near (after.left[(size_t) i], 0.25f, 1.0e-5f);
        float worstDip = 1.0f;
        for (int i = 29760; i < 30240; ++i)
        {
            // Equal-power: the two gains' squares sum to one, so the level never dips below the quieter side.
            worstDip = juce::jmin (worstDip, after.left[(size_t) i]);
        }
        check (outsideSame, "outside the punch range the song is bit-identical to before");
        check (insideTake, "inside it, only the new take plays");
        check (worstDip > 0.24f, "the crossfade at the punch-in doesn't dip (" + juce::String (worstDip, 3) + ")");

        project->undo.undo();
        regions.clear();
        for (auto r : t)
            if (r.hasType (ids::REGION))
                regions.add (r);
        check (regions.size() == 1 && (juce::int64) old[ids::length] == 100000, "undo puts the old region back whole");

        // Quick punch: record pressed while playing starts the take where the playhead is.
        project->state.setProperty (ids::punchOn, false, nullptr);
        engine.setPosition (0);
        engine.play();
        runEngine (engine, 20032);
        const auto at = engine.getPosition();
        engine.record();
        runEngine (engine, 9984, 0.25f);
        engine.stop();
        runEngine (engine, block * 4);
        engine.service();
        juce::ValueTree quick;
        for (auto r : t)
            if (r.hasType (ids::REGION) && r != old)
                quick = r;
        check (quick.isValid() && (juce::int64) quick[ids::start] == at && (juce::int64) quick[ids::length] == 9984,
               "quick punch: record during playback starts a take at the playhead (" + juce::String ((juce::int64) quick[ids::start]) + ")");
        engine.setProject (nullptr);
        project->removeTrack (t);
    }

    folder.deleteRecursively();
    std::cout << (failures == 0 ? "all mix checks passed\n" : juce::String (failures) + " mix checks failed\n");
    return failures == 0 ? 0 : 1;
}
