// woodshed-bench sampler: checks the Sampler instrument with real audio numbers.
//
//   pitch       a sine sample played at root +7 and -12 (and a 44.1 kHz file in a 48 kHz song)
//               lands within 2 cents of the right frequency
//   loops       forward loops on whole cycles, crossfaded loops on a broken cycle and alternate
//               loops never jump more than 1.5x the tone's own largest sample-to-sample step
//   one shot    keeps playing after a 10 ms note; Classic stops after its release
//   slices      a synthetic drum loop of 8 hits slices into 8, each within 3 ms of its hit,
//               mapped to C1 upward
//   aliasing    a 15 kHz sine up an octave (30 kHz, above Nyquist) and 18 kHz up a fifth come
//               out more than 60 dB down; 9 kHz up an octave keeps its level
//   round robin two zones in a group take turns
//   save/load   the SAMPLER state (with its sample copied into the project) survives a save and
//               load and plays back the same samples
//   convert     Convert to Sampler Track (Classic and Slice) plays the region back as it was
//   cpu         a 16-voice transposed stereo chord through the filter within budget
//
// woodshed-bench demo-sampler <folder>: a project using the Sampler, for screenshots.

#include "../../Source/instruments/SamplerTrack.h"
#include "../../Source/daw/Engine.h"
#include "../../Source/engine/PitchDetector.h"
#include <iostream>

using namespace woodshed;
using namespace woodshed::sampler;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 64;
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << "\n";
    failures += ok ? 0 : 1;
}

double cpuScale()
{
    const auto scale = juce::SystemStats::getEnvironmentVariable ("WOODSHED_BENCH_CPU_SCALE", "1").getDoubleValue();
    return scale > 0.0 ? scale : 1.0;
}

juce::String fmt (double v, int places = 2)          { return juce::String (v, places); }
float toDb (float g)                                  { return juce::Decibels::gainToDecibels (g, -200.0f); }

bool writeWav (const juce::File& f, const juce::AudioBuffer<float>& audio, double sr)
{
    f.deleteFile();
    f.getParentDirectory().createDirectory();
    std::unique_ptr<juce::OutputStream> stream (f.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (sr)
                                                   .withNumChannels (audio.getNumChannels()).withBitsPerSample (32)
                                                   .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    return writer != nullptr && writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

juce::AudioBuffer<float> sine (double hz, double seconds, double sr = rate, float amp = 0.5f, int channels = 1)
{
    juce::AudioBuffer<float> b (channels, (int) (seconds * sr));
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (c, i, amp * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / sr));
    return b;
}

/** Eight hits, a quarter second apart: kick, hat, snare, hat... Returns their times in samples. */
juce::AudioBuffer<float> drumLoop (std::vector<juce::int64>& hits)
{
    juce::AudioBuffer<float> b (1, (int) (2.0 * rate));
    b.clear();
    juce::Random rng (7);
    for (int h = 0; h < 8; ++h)
    {
        const auto at = (juce::int64) (h * 0.25 * rate);
        hits.push_back (at);
        float hp = 0.0f, prev = 0.0f;
        double phase = 0.0;
        for (int i = 0; i < (int) (0.24 * rate) && at + i < b.getNumSamples(); ++i)
        {
            const double t = i / rate;
            float v = 0.0f;
            if (h % 4 == 0)          // kick: a falling sine with a click
            {
                phase += juce::MathConstants<double>::twoPi * (50.0 + 90.0 * std::exp (-t * 30.0)) / rate;
                v = (float) (0.9 * std::exp (-t * 9.0) * std::sin (phase));
            }
            else if (h % 4 == 2)     // snare: noise and a body
            {
                v = (float) (std::exp (-t * 22.0) * (0.5 * rng.nextFloat() - 0.25 + 0.4 * std::sin (juce::MathConstants<double>::twoPi * 190.0 * t)));
            }
            else                     // hat: bright noise, short and quieter
            {
                const float n = rng.nextFloat() * 2.0f - 1.0f;
                hp = 0.7f * (hp + n - prev);
                prev = n;
                v = (float) (0.3 * std::exp (-t * 60.0)) * hp;
            }
            b.setSample (0, (int) at + i, v);
        }
    }
    return b;
}

struct Sub
{
    std::unique_ptr<Instrument> inst;
    std::vector<float> knobs;

    Sub()
    {
        auto* t = InstrumentRegistry::get().find (typeId);
        inst = t->create();
        knobs = patchValues (t->info, &t->info.patches.front());
        inst->prepare (rate, block);
        apply();
    }

    void set (int k, float v)                   { knobs[(size_t) k] = v; apply(); }
    void apply()
    {
        for (size_t k = 0; k < knobs.size(); ++k) inst->setKnob ((int) k, knobs[k]);
        inst->update();
    }
};

struct Ev { int time; NoteEvent e; };
Ev on (double t, int note, float vel = 1.0f)    { NoteEvent e; e.kind = NoteEvent::noteOn; e.note = note; e.value = vel; return { (int) (t * rate), e }; }
Ev off (double t, int note)                     { NoteEvent e; e.kind = NoteEvent::noteOff; e.note = note; return { (int) (t * rate), e }; }

juce::AudioBuffer<float> play (Instrument& inst, std::vector<Ev> events, double seconds)
{
    std::stable_sort (events.begin(), events.end(), [] (const Ev& a, const Ev& b) { return a.time < b.time; });
    juce::AudioBuffer<float> out (2, (int) (seconds * rate));
    out.clear();
    size_t e = 0;
    std::vector<NoteEvent> now;
    for (int pos = 0; pos < out.getNumSamples(); pos += block)
    {
        const int n = juce::jmin (block, out.getNumSamples() - pos);
        now.clear();
        for (; e < events.size() && events[e].time < pos + n; ++e)
        {
            auto ev = events[e].e;
            ev.offset = juce::jmax (0, events[e].time - pos);
            now.push_back (ev);
        }
        inst.update();
        inst.render (out.getWritePointer (0, pos), out.getWritePointer (1, pos), n, now.data(), (int) now.size());
    }
    return out;
}

float rmsOf (const juce::AudioBuffer<float>& b, double from, double to, int ch = 0)
{
    const int a = juce::jlimit (0, b.getNumSamples(), (int) (from * rate)), z = juce::jlimit (a, b.getNumSamples(), (int) (to * rate));
    return z > a ? b.getRMSLevel (ch, a, z - a) : 0.0f;
}

float frequencyAt (const juce::AudioBuffer<float>& b, double at)
{
    PitchDetector d;
    return d.analyse (b.getReadPointer (0, (int) (at * rate)), PitchDetector::windowSize, rate, 30.0f, 2000.0f).frequency;
}

float maxJump (const juce::AudioBuffer<float>& b, double from, double to)
{
    float m = 0.0f;
    const auto* d = b.getReadPointer (0);
    for (int i = (int) (from * rate) + 1; i < (int) (to * rate); ++i)
        m = juce::jmax (m, std::abs (d[i] - d[i - 1]));
    return m;
}

/** The SAMPLER node of a fresh INSTRUMENT, with one zone. */
juce::ValueTree instrumentWith (const Zone& z, Mode mode = Mode::classic)
{
    juce::ValueTree inst ("INSTRUMENT");
    auto s = samplerNode (inst, nullptr);
    s.setProperty (sampler::ids::mode, modeId (mode), nullptr);
    s.appendChild (z.toTree(), nullptr);
    return inst;
}

/** Plays every track of the project, offline, like a bounce. Left channel. */
std::vector<float> renderSong (daw::Project& project, double seconds)
{
    using namespace woodshed::daw;
    AudioPool pool;
    std::map<int, std::shared_ptr<TrackRuntime>> runtimes;
    for (auto t : project.state)
    {
        if (! t.hasType (daw::ids::TRACK)) continue;
        auto rt = std::make_shared<TrackRuntime>();
        rt->id = (int) t[daw::ids::id];
        rt->work.setSize (2, block);
        rt->gainLeft.reset (rate, 0.03);
        rt->gainRight.reset (rate, 0.03);
        rt->events.reserve (4096);
        if (Project::isInstrumentTrack (t))
            if (auto* type = InstrumentRegistry::get().find (t.getChildWithName (daw::ids::INSTRUMENT)[daw::ids::type].toString()))
            {
                rt->instrument = type->create();
                rt->instrument->prepare (rate, block);
            }
        runtimes[rt->id] = rt;
    }
    auto snap = Engine::buildSnapshot (project, pool, runtimes);
    for (auto& t : snap->tracks)
    {
        t.runtime->gainLeft.setCurrentAndTargetValue (t.audible ? t.gain : 0.0f);
        t.runtime->gainRight.setCurrentAndTargetValue (t.audible ? t.gain : 0.0f);
    }
    const auto length = (juce::int64) (seconds * rate);
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
    return { buf.getReadPointer (0), buf.getReadPointer (0) + length };
}

double correlation (const std::vector<float>& a, const std::vector<float>& b)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < juce::jmin (a.size(), b.size()); ++i)
    {
        ab += (double) a[i] * b[i];
        aa += (double) a[i] * a[i];
        bb += (double) b[i] * b[i];
    }
    return aa > 0.0 && bb > 0.0 ? ab / std::sqrt (aa * bb) : 0.0;
}

double levelRatioDb (const std::vector<float>& a, const std::vector<float>& b)
{
    double aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < juce::jmin (a.size(), b.size()); ++i) { aa += (double) a[i] * a[i]; bb += (double) b[i] * b[i]; }
    return 10.0 * std::log10 ((aa + 1.0e-20) / (bb + 1.0e-20));
}

//==============================================================================
void testRegistry()
{
    std::cout << "registry\n";
    auto* t = InstrumentRegistry::get().find (typeId);
    check (t != nullptr, "the Sampler is registered");
    if (t == nullptr) return;
    check (t->info.knobs.size() == (size_t) param::builtIn + 1 && t->info.knobs.size() <= (size_t) Instrument::maxKnobs,
           juce::String ((int) t->info.knobs.size()) + " knobs, in the param order");
    check (InstrumentRegistry::categories().contains (t->info.category), "its category is a library section");
}

void testPitch (const juce::File& dir)
{
    std::cout << "pitch\n";
    // 440 Hz, looped on whole cycles so it sustains.
    const auto file = dir.getChildFile ("a440.wav");
    writeWav (file, sine (440.0, 2.0), rate);
    Zone z;
    z.file = file.getFullPathName();
    z.root = 69;
    z.loop = LoopMode::forward;
    z.loopStart = 24000;
    z.loopEnd = 48000;
    const auto inst = instrumentWith (z);

    for (auto [note, hz] : { std::pair<int, double> { 69, 440.0 }, { 76, 440.0 * std::exp2 (7.0 / 12.0) }, { 57, 220.0 } })
    {
        Sub s;
        s.inst->syncState (inst, dir);
        const auto out = play (*s.inst, { on (0.0, note) }, 1.5);
        const auto got = frequencyAt (out, 0.6);
        const auto cents = 1200.0 * std::log2 (got / hz);
        check (std::abs (cents) < 2.0, "note " + juce::String (note) + ": " + fmt (got) + " Hz for " + fmt (hz) + " Hz (" + fmt (cents) + " cents)");
    }

    // A 44.1 kHz file in a 48 kHz song still plays at its pitch.
    const auto file44 = dir.getChildFile ("a440-44k.wav");
    writeWav (file44, sine (440.0, 2.0, 44100.0), 44100.0);
    Zone z44;
    z44.file = file44.getFullPathName();
    z44.root = 69;
    Sub s;
    s.inst->syncState (instrumentWith (z44), dir);
    const auto out = play (*s.inst, { on (0.0, 69) }, 1.0);
    const auto got = frequencyAt (out, 0.3);
    check (std::abs (1200.0 * std::log2 (got / 440.0)) < 2.0, "44.1 kHz file at root: " + fmt (got) + " Hz");
}

void testLoops (const juce::File& dir)
{
    std::cout << "loops\n";
    struct Case { const char* what; double hz; juce::int64 ls, le, xf; LoopMode mode; int note; bool mustClick; };
    const Case cases[] {
        { "forward, whole cycles", 480.0, 9600, 19200, 0, LoopMode::forward, 60, false },
        { "forward, whole cycles, +7", 480.0, 9600, 19200, 0, LoopMode::forward, 67, false },
        { "forward, broken cycle, 2000-frame crossfade", 445.0, 10000, 19950, 2000, LoopMode::forward, 60, false },
        { "forward, broken cycle, crossfade, -5", 445.0, 10000, 19950, 2000, LoopMode::forward, 55, false },
        { "alternate, broken cycle", 445.0, 10000, 19950, 0, LoopMode::alternate, 60, false },
        { "forward, broken cycle, no crossfade", 445.0, 10000, 19950, 0, LoopMode::forward, 60, true },
    };
    for (auto& c : cases)
    {
        const auto file = dir.getChildFile ("loop" + juce::String (c.hz) + ".wav");
        writeWav (file, sine (c.hz, 1.0), rate);
        Zone z;
        z.file = file.getFullPathName();
        z.root = 60;
        z.loop = c.mode;
        z.loopStart = c.ls;
        z.loopEnd = c.le;
        z.crossfade = c.xf;
        Sub s;
        s.inst->syncState (instrumentWith (z), dir);
        const auto out = play (*s.inst, { on (0.0, c.note) }, 2.5);
        const double outHz = c.hz * std::exp2 ((c.note - 60) / 12.0);
        const float natural = 0.5f * (float) (juce::MathConstants<double>::twoPi * outHz / rate);
        const float jump = maxJump (out, 0.1, 2.4);
        const float ratio = jump / natural;
        if (c.mustClick)
            check (ratio > 3.0f, juce::String (c.what) + ": jump " + fmt (ratio) + "x (the check can see a click)");
        else
            check (ratio < 1.5f, juce::String (c.what) + ": largest step " + fmt (ratio) + "x the tone's own");
    }
}

void testOneShot (const juce::File& dir)
{
    std::cout << "one shot\n";
    const auto file = dir.getChildFile ("tone300.wav");
    writeWav (file, sine (300.0, 1.0), rate);
    Zone z;
    z.file = file.getFullPathName();
    z.root = 60;

    Sub shot;
    shot.inst->syncState (instrumentWith (z, Mode::oneShot), dir);
    const auto a = play (*shot.inst, { on (0.0, 60), off (0.01, 60) }, 1.3);
    const float during = rmsOf (a, 0.5, 0.9), after = rmsOf (a, 1.05, 1.3);
    check (toDb (during) > -12.0f, "one shot still sounding 0.5 s after a 10 ms note: " + fmt (toDb (during), 1) + " dBFS");
    check (after < 1.0e-6f, "and silent once the sample ends: " + fmt (toDb (after), 1) + " dBFS");

    Sub classic;
    classic.inst->syncState (instrumentWith (z, Mode::classic), dir);
    const auto b = play (*classic.inst, { on (0.0, 60), off (0.01, 60) }, 1.0);
    check (rmsOf (b, 0.5, 0.9) < 1.0e-5f, "Classic stops after its release: " + fmt (toDb (rmsOf (b, 0.5, 0.9)), 1) + " dBFS");
}

void testSlices (const juce::File& dir)
{
    std::cout << "slices\n";
    std::vector<juce::int64> hits;
    Source src;
    src.audio = drumLoop (hits);
    src.rate = rate;

    for (float sens : { 0.5f, 0.2f, 0.8f })
    {
        const auto cuts = detectSlices (src, 0, -1, sens);
        double worst = 0.0;
        if (cuts.size() == hits.size())
            for (size_t i = 0; i < hits.size(); ++i)
                worst = juce::jmax (worst, std::abs ((double) (cuts[i] - hits[i])) / rate * 1000.0);
        check (cuts.size() == hits.size() && worst < 3.0, "sensitivity " + fmt (sens, 1) + ": " + juce::String ((int) cuts.size())
                   + " slices for " + juce::String ((int) hits.size()) + " hits, worst " + fmt (worst, 2) + " ms off");
    }

    // A soft ghost hit only the most sensitive setting finds.
    {
        auto ghost = src;
        for (int i = 0; i < (int) (0.05 * rate); ++i)
            ghost.audio.addSample (0, (int) (0.375 * rate) + i, 0.01f * (float) std::exp (-i / (0.01 * rate)) * (i % 7 < 3 ? 1.0f : -1.0f));
        const auto few = detectSlices (ghost, 0, -1, 0.0f).size(), many = detectSlices (ghost, 0, -1, 1.0f).size();
        check (many > few, "sensitivity 1 finds the ghost note that 0 skips (" + juce::String ((int) few) + " vs " + juce::String ((int) many) + ")");
    }

    // Slice mode on the loop lays the slices on C1 upward.
    const auto file = dir.getChildFile ("loop.wav");
    writeWav (file, src.audio, rate);
    juce::ValueTree inst ("INSTRUMENT");
    auto node = samplerNode (inst, nullptr);
    loadSample (node, Mode::slice, file.getFullPathName(), *loadSource (file), 0, -1, nullptr);
    juce::StringArray keys;
    for (auto c : node) keys.add (c[sampler::ids::lo].toString());
    check (node.getNumChildren() == 8 && keys.joinIntoString (",") == "36,37,38,39,40,41,42,43", "Slice mode zones on keys " + keys.joinIntoString (","));

    // Each slice key plays its own hit: key 38 is the third, the snare.
    Sub s;
    s.inst->syncState (inst, dir);
    const auto out = play (*s.inst, { on (0.0, 38) }, 0.5);
    juce::AudioBuffer<float> snare (1, (int) (0.2 * rate));
    snare.copyFrom (0, 0, src.audio, 0, (int) hits[2], snare.getNumSamples());
    const auto expected = rmsOf (snare, 0.0, 0.2);
    const auto got = rmsOf (out, 0.0, 0.2);
    check (std::abs (toDb (got) - toDb (expected)) < 1.0f, "key 38 plays the snare: " + fmt (toDb (got), 1) + " dB vs " + fmt (toDb (expected), 1) + " dB");
}

void testAliasing (const juce::File& dir)
{
    std::cout << "aliasing\n";
    struct Case { double hz; int semis; bool shouldVanish; };
    for (auto c : { Case { 15000.0, 12, true }, Case { 18000.0, 7, true }, Case { 21000.0, 3, true }, Case { 12000.0, 19, true },
                    Case { 9000.0, 12, false }, Case { 5000.0, 19, false } })
    {
        const auto file = dir.getChildFile ("hf" + juce::String ((int) c.hz) + ".wav");
        writeWav (file, sine (c.hz, 1.0), rate);
        Zone z;
        z.file = file.getFullPathName();
        z.root = 60;
        Sub s;
        s.inst->syncState (instrumentWith (z), dir);
        const auto out = play (*s.inst, { on (0.0, 60 + c.semis) }, 0.3);
        const float ref = 0.5f / std::sqrt (2.0f);
        const float level = toDb (rmsOf (out, 0.05, 0.2) / ref);
        const double outHz = c.hz * std::exp2 (c.semis / 12.0);
        if (c.shouldVanish)
            check (level < -60.0f, fmt (c.hz / 1000.0, 0) + " kHz +" + juce::String (c.semis) + " st (" + fmt (outHz / 1000.0, 1)
                                       + " kHz, above Nyquist): aliases " + fmt (level, 1) + " dB");
        else
            check (std::abs (level) < 1.5f, fmt (c.hz / 1000.0, 0) + " kHz +" + juce::String (c.semis) + " st (" + fmt (outHz / 1000.0, 1)
                                                + " kHz): level " + fmt (level, 2) + " dB");
    }
}

/** How far below the tone everything else is: the residual after a least-squares fit of a sine at `hz`. */
float residualDb (const juce::AudioBuffer<float>& b, double hz, double from, double to)
{
    const int a = (int) (from * rate), z = (int) (to * rate);
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
    for (int i = a; i < z; ++i)
    {
        const double w = juce::MathConstants<double>::twoPi * hz * i / rate, s = std::sin (w), c = std::cos (w), x = b.getSample (0, i);
        ss += s * s; cc += c * c; sc += s * c; xs += x * s; xc += x * c;
    }
    const double det = ss * cc - sc * sc;
    const double ka = (xs * cc - xc * sc) / det, kb = (xc * ss - xs * sc) / det;
    double res = 0.0, tone = 0.0;
    for (int i = a; i < z; ++i)
    {
        const double w = juce::MathConstants<double>::twoPi * hz * i / rate;
        const double fit = ka * std::sin (w) + kb * std::cos (w);
        tone += fit * fit;
        res += (b.getSample (0, i) - fit) * (b.getSample (0, i) - fit);
    }
    return (float) (10.0 * std::log10 ((res + 1.0e-30) / (tone + 1.0e-30)));
}

void testPurity (const juce::File& dir)
{
    std::cout << "purity\n";
    const auto file = dir.getChildFile ("k1.wav");
    writeWav (file, sine (1000.0, 2.0), rate);
    Zone z;
    z.file = file.getFullPathName();
    z.root = 60;
    for (int semis : { 24, 7, 1, -19 })
    {
        Sub s;
        s.inst->syncState (instrumentWith (z), dir);
        const auto out = play (*s.inst, { on (0.0, 60 + semis) }, 0.4);
        const double hz = 1000.0 * std::exp2 (semis / 12.0);
        const float db = residualDb (out, hz, 0.05, 0.35);
        check (db < -60.0f, "1 kHz " + juce::String (semis > 0 ? "+" : "") + juce::String (semis) + " st: everything but the "
                                + fmt (hz, 0) + " Hz tone " + fmt (db, 1) + " dB");
    }
}

void testRoundRobin (const juce::File& dir)
{
    std::cout << "round robin\n";
    const auto a = dir.getChildFile ("rrA.wav"), b = dir.getChildFile ("rrB.wav");
    writeWav (a, sine (300.0, 1.0), rate);
    writeWav (b, sine (500.0, 1.0), rate);
    juce::ValueTree inst ("INSTRUMENT");
    auto node = samplerNode (inst, nullptr);
    for (auto& f : { a, b })
    {
        Zone z;
        z.file = f.getFullPathName();
        z.root = 60;
        z.group = 1;
        node.appendChild (z.toTree(), nullptr);
    }
    Sub s;
    s.inst->syncState (inst, dir);
    juce::StringArray heard;
    for (int i = 0; i < 4; ++i)
    {
        const auto out = play (*s.inst, { on (0.0, 60), off (0.05, 60) }, 0.4);
        heard.add (juce::String (juce::roundToInt (frequencyAt (out, 0.1))));
    }
    check (heard.joinIntoString (" ") == "300 500 300 500", "four hits alternate: " + heard.joinIntoString (" ") + " Hz");
}

void testSaveLoad (const juce::File& dir)
{
    std::cout << "save/load\n";
    using namespace woodshed::daw;
    const auto folder = dir.getChildFile ("Project");
    folder.deleteRecursively();
    auto p = Project::createIn (folder, rate);
    auto track = addSamplerTrack (*p, "Sampler");
    const auto outside = dir.getChildFile ("outside.wav");
    writeWav (outside, sine (330.0, 1.5, rate, 0.5f, 2), rate);
    const bool loaded = loadFileIntoTrack (*p, track, outside, Mode::classic);
    auto node = track.getChildWithName (daw::ids::INSTRUMENT).getChildWithName (sampler::ids::SAMPLER);
    auto zone = node.getChild (0);
    check (loaded && zone.isValid(), "a file loads into the track");
    const auto relative = zone[sampler::ids::file].toString();
    check (relative.startsWith ("Audio/") && folder.getChildFile (relative).existsAsFile(),
           "the sample is copied into the project (" + relative + ")");
    check ((int) zone[sampler::ids::root] == 64, "Classic finds its root: " + zone[sampler::ids::root].toString() + " (E3 = 330 Hz)");
    zone.setProperty (sampler::ids::loop, "alternate", nullptr);
    zone.setProperty (sampler::ids::loopStart, 12000, nullptr);
    zone.setProperty (sampler::ids::loopEnd, 36000, nullptr);
    zone.setProperty (sampler::ids::pan, -0.4, nullptr);
    auto inst = track.getChildWithName (daw::ids::INSTRUMENT);
    auto values = Project::decodeValues (inst[daw::ids::knobs].toString());
    values[(size_t) param::filterType] = 1;
    values[(size_t) param::cutoff] = 1500.0f;
    inst.setProperty (daw::ids::knobs, Project::encodeValues (values), nullptr);
    check (p->save(), "saved");

    auto again = Project::load (folder);
    check (again != nullptr, "loaded");
    if (again == nullptr) return;
    auto inst2 = again->track (0).getChildWithName (daw::ids::INSTRUMENT);
    check (inst2.getChildWithName (sampler::ids::SAMPLER).isEquivalentTo (node), "the SAMPLER node comes back the same");

    auto renderFrom = [&] (const juce::ValueTree& instNode, const juce::File& projectFolder)
    {
        Sub s;
        const auto v = Project::decodeValues (instNode[daw::ids::knobs].toString());
        for (size_t k = 0; k < v.size() && k < s.knobs.size(); ++k) s.knobs[k] = v[k];
        s.apply();
        s.inst->syncState (instNode, projectFolder);
        return play (*s.inst, { on (0.0, 60), off (0.8, 60) }, 1.2);
    };
    const auto x = renderFrom (inst, folder), y = renderFrom (inst2, again->folder);
    float diff = 0.0f;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < x.getNumSamples(); ++i)
            diff = juce::jmax (diff, std::abs (x.getSample (c, i) - y.getSample (c, i)));
    check (diff < 1.0e-6f && rmsOf (x, 0.1, 0.7) > 0.01f, "plays the same samples after loading (max difference " + juce::String (diff) + ")");
    check (rmsOf (x, 0.1, 0.7, 0) > rmsOf (x, 0.1, 0.7, 1) * 1.2f, "and keeps the zone's pan");
}

void testConvert (const juce::File& dir)
{
    std::cout << "convert to sampler track\n";
    using namespace woodshed::daw;
    for (auto mode : { Mode::slice, Mode::classic })
    {
        const auto folder = dir.getChildFile ("Convert" + modeId (mode));
        folder.deleteRecursively();
        auto p = Project::createIn (folder, rate);
        std::vector<juce::int64> hits;
        const auto audio = mode == Mode::slice ? drumLoop (hits) : sine (220.0, 2.0);
        writeWav (folder.getChildFile ("Audio/source.wav"), audio, rate);
        auto t = p->addTrack ("Loop");
        t.setProperty (daw::ids::rigOn, false, nullptr);
        // The region starts half a second in and plays the file from 0.1 s for 1.85 s.
        const auto region = p->addRegion (t, "Audio/source.wav", (juce::int64) (0.5 * rate), (juce::int64) (0.1 * rate),
                                          (juce::int64) (1.85 * rate), "Loop");
        const auto before = renderSong (*p, 3.0);

        auto made = convertToSamplerTrack (*p, region, mode);
        check (made.isValid() && p->numTracks() == 2 && (bool) region[daw::ids::mute], modeName (mode) + ": a new track, the audio region muted");
        if (! made.isValid()) continue;
        const auto midi = made.getChild (made.getNumChildren() - 1);
        const auto zones = made.getChildWithName (daw::ids::INSTRUMENT).getChildWithName (sampler::ids::SAMPLER).getNumChildren();
        check (Project::isMidiRegion (midi) && (juce::int64) midi[daw::ids::start] == (juce::int64) region[daw::ids::start],
               "a MIDI region where the audio was, with " + juce::String (midi.getNumChildren()) + " notes for " + juce::String (zones) + " zones");
        if (mode == Mode::slice)
            check (zones == 8 && midi.getNumChildren() == 8, "eight slices, eight notes");

        const auto after = renderSong (*p, 3.0);
        const auto corr = correlation (before, after);
        if (juce::SystemStats::getEnvironmentVariable ("SAMPLER_DEBUG", {}) == "1")
            for (double t0 = 0.5; t0 < 2.4; t0 += 0.125)
            {
                const auto a0 = (size_t) (t0 * rate), n0 = (size_t) (0.125 * rate);
                std::vector<float> x (before.begin() + (long) a0, before.begin() + (long) (a0 + n0)), y (after.begin() + (long) a0, after.begin() + (long) (a0 + n0));
                std::cout << "    " << t0 << " s: corr " << correlation (x, y) << " level " << levelRatioDb (y, x) << "\n";
            }
        const auto db = levelRatioDb (after, before);
        check (corr > 0.99 && std::abs (db) < 0.5, "plays back as it was: correlation " + fmt (corr, 4) + ", level " + fmt (db, 2) + " dB");
    }
}

void testCpu (const juce::File& dir)
{
    std::cout << "cpu\n";
    const auto file = dir.getChildFile ("cpu.wav");
    juce::AudioBuffer<float> audio (2, (int) (4.0 * rate));
    juce::Random rng (3);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < audio.getNumSamples(); ++i)
            audio.setSample (c, i, 0.2f * (float) std::sin (0.03 * i + c) + 0.05f * (rng.nextFloat() - 0.5f));
    writeWav (file, audio, rate);
    Zone z;
    z.file = file.getFullPathName();
    z.root = 60;
    Sub s;
    s.set (param::filterType, 1);
    s.set (param::cutoff, 3000.0f);
    s.set (param::lfoPitch, 0.2f);
    s.inst->syncState (instrumentWith (z), dir);

    std::vector<NoteEvent> notes;
    for (int i = 0; i < 16; ++i)
    {
        NoteEvent e;
        e.note = 36 + i * 3;
        e.value = 0.8f;
        notes.push_back (e);
    }
    float l[block] {}, r[block] {};
    juce::ScopedNoDenormals noDenormals;
    s.inst->render (l, r, block, notes.data(), (int) notes.size());
    for (int i = 0; i < 20; ++i) s.inst->render (l, r, block, nullptr, 0);
    constexpr int blocks = 1500;
    const auto t0 = juce::Time::getHighResolutionTicks();
    for (int b = 0; b < blocks; ++b)
    {
        s.inst->update();
        s.inst->render (l, r, block, nullptr, 0);
    }
    const auto us = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1.0e6 / blocks;
    const double budget = 300.0 * cpuScale();
    check (us < budget, "16 voices, stereo, filtered, transposed -24..+21 st: " + fmt (us, 1) + " us per 64-sample block (budget " + fmt (budget, 0) + ")");
}
} // namespace

int runSamplerTest (const juce::StringArray& args);
int runSamplerTest (const juce::StringArray&)
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("woodshed-sampler-test");
    dir.deleteRecursively();
    dir.createDirectory();

    testRegistry();
    testPitch (dir);
    testLoops (dir);
    testOneShot (dir);
    testSlices (dir);
    testAliasing (dir);
    testPurity (dir);
    testRoundRobin (dir);
    testSaveLoad (dir);
    testConvert (dir);
    testCpu (dir);

    dir.deleteRecursively();
    std::cout << (failures == 0 ? "sampler: all passed\n" : "sampler: " + juce::String (failures).toStdString() + " failed\n");
    return failures == 0 ? 0 : 1;
}

//==============================================================================
int writeSamplerDemoProject (const juce::File& folder);
int writeSamplerDemoProject (const juce::File& folder)
{
    juce::ScopedJuceInitialiser_GUI init;
    using namespace woodshed::daw;
    folder.deleteRecursively();
    auto p = Project::createIn (folder, rate);
    p->state.setProperty (daw::ids::name, "Sampler Demo", nullptr);
    p->state.setProperty (daw::ids::tempo, 120.0, nullptr);
    const auto bar = (juce::int64) std::llround (p->grid().samplesPerBar());

    // A drum loop as audio, converted to a Slice sampler track that plays the slices in a new order.
    std::vector<juce::int64> hits;
    writeWav (p->audioFolder().getChildFile ("Drum Loop.wav"), drumLoop (hits), rate);
    auto loopTrack = p->addTrack ("Drum Loop");
    loopTrack.setProperty (daw::ids::rigOn, false, nullptr);
    auto region = p->addRegion (loopTrack, "Audio/Drum Loop.wav", 0, 0, (juce::int64) (2.0 * rate), "Drum Loop");
    auto slices = convertToSamplerTrack (*p, region, Mode::slice);
    if (slices.isValid())
    {
        slices.setProperty (daw::ids::volume, -4.0, nullptr);
        auto midi = slices.getChild (slices.getNumChildren() - 1);
        midi.setProperty (daw::ids::length, bar * 4, nullptr);
        midi.removeAllChildren (nullptr);
        const int order[] { 36, 37, 38, 37, 36, 36, 38, 39, 36, 37, 38, 41, 36, 40, 38, 43 };
        for (int i = 0; i < 64; ++i)
            p->addNote (midi, { order[i % 16], i % 4 == 0 ? 120 : 96, (juce::int64) i * 240, 240 });
    }

    // A plucked tone at A2, generated, as a Classic sampler: its root is found, its sustain looped.
    auto keys = addSamplerTrack (*p, "Pluck Sampler");
    keys.setProperty (daw::ids::volume, -9.0, nullptr);
    juce::AudioBuffer<float> tone (2, (int) (2.5 * rate));
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < tone.getNumSamples(); ++i)
        {
            const double t = i / rate;
            double v = 0.0;
            for (int h = 1; h <= 12; ++h)
                v += std::exp (-t * (0.6 + 0.5 * h)) / h * std::sin (juce::MathConstants<double>::twoPi * 220.0 * h * t + c * 0.3 * h);
            tone.setSample (c, i, (float) (0.35 * v * juce::jmin (1.0, t * 500.0)));
        }
    const auto toneFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Pluck A2.wav");
    writeWav (toneFile, tone, rate);
    loadFileIntoTrack (*p, keys, toneFile, Mode::classic);
    toneFile.deleteFile();
    auto zone = keys.getChildWithName (daw::ids::INSTRUMENT).getChildWithName (sampler::ids::SAMPLER).getChild (0);
    if (zone.isValid())
    {
        zone.setProperty (sampler::ids::loop, "forward", nullptr);
        zone.setProperty (sampler::ids::loopStart, (juce::int64) (0.8 * rate), nullptr);
        zone.setProperty (sampler::ids::loopEnd, (juce::int64) (1.6 * rate), nullptr);
        zone.setProperty (sampler::ids::crossfade, (juce::int64) (0.1 * rate), nullptr);
    }
    auto line = p->addMidiRegion (keys, 0, bar * 4, "Line");
    const int notes[] { 0, 3, 5, 7, 10, 7, 5, 3 };
    for (int i = 0; i < 8; ++i)
        p->addNote (line, { 57 + notes[i], 100, (juce::int64) i * 1920, 1800 });
    p->state.setProperty (daw::ids::selectedTrack, keys[daw::ids::id], nullptr);

    if (! p->save())
        return 1;
    std::cout << "wrote " << p->projectFile().getFullPathName() << "\n";
    return 0;
}
