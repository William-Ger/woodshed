// woodshed-bench instruments [type ...] [--render dir] [--analyse]
//
// Every software instrument x every patch, at 48 kHz in 64-sample blocks. Pitched
// instruments play a sustained 8-note C-major chord at full velocity, a fast three-octave
// arpeggio with varied velocities, the sustain pedal, pitch bend and the mod wheel; drum
// kits play a groove and then every mapped note on its own.
//
// A patch fails on NaN/inf, any output from no events, a peak above -0.1 dBFS, silence
// when played, anything still sounding 10 s after all-notes-off, a chord loudness outside
// the shared window (so switching sounds doesn't jump in level), instability under random
// knob automation, or a 16-voice chord costing more than the CPU budget per block. The
// registry fails on duplicate type ids, too many knobs, or patches naming unknown knobs.
//
// --analyse prints listening proxies: spectral centroid vs velocity and over time, decay,
// and the centroid swing of any cutoff/tone/bright knob.

#include "../../Source/instruments/Instrument.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>

using namespace woodshed;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 64;
constexpr float peakLimitDb = -0.1f;
constexpr float levelMinDb = -30.0f, levelMaxDb = -8.0f;
constexpr double cpuBudgetUs = 300.0;

using Signal = std::vector<float>;
struct Stereo { Signal l, r; };
struct TimedEvent { int time; NoteEvent ev; };

float toDb (float g)                            { return juce::Decibels::gainToDecibels (g, -200.0f); }
int secs (double s)                             { return (int) (s * rate); }

float peakOf (const Stereo& s, size_t from = 0, size_t to = SIZE_MAX)
{
    float p = 0.0f;
    for (size_t i = from; i < juce::jmin (to, s.l.size()); ++i)
        p = juce::jmax (p, std::abs (s.l[i]), std::abs (s.r[i]));
    return p;
}

float rmsOf (const Stereo& s, size_t from, size_t to)
{
    double sum = 0.0;
    to = juce::jmin (to, s.l.size());
    for (size_t i = from; i < to; ++i) sum += 0.5 * ((double) s.l[i] * s.l[i] + (double) s.r[i] * s.r[i]);
    return (float) std::sqrt (sum / (double) juce::jmax ((size_t) 1, to - from));
}

bool finite (const Stereo& s)
{
    auto ok = [] (float v) { return std::isfinite (v); };
    return std::all_of (s.l.begin(), s.l.end(), ok) && std::all_of (s.r.begin(), s.r.end(), ok);
}

TimedEvent on (double t, int note, float vel)   { NoteEvent e; e.kind = NoteEvent::noteOn;  e.note = note; e.value = vel; return { secs (t), e }; }
TimedEvent off (double t, int note)             { NoteEvent e; e.kind = NoteEvent::noteOff; e.note = note; return { secs (t), e }; }
TimedEvent ctl (double t, NoteEvent::Kind k, float v) { NoteEvent e; e.kind = k; e.value = v; return { secs (t), e }; }

/** Plays timed events through the instrument; perBlock runs before each block's update(). */
Stereo play (Instrument& inst, std::vector<TimedEvent> events, int total, const std::function<void (int)>& perBlock = {})
{
    std::stable_sort (events.begin(), events.end(), [] (const TimedEvent& a, const TimedEvent& b) { return a.time < b.time; });
    Stereo out { Signal ((size_t) total, 0.0f), Signal ((size_t) total, 0.0f) };
    std::vector<NoteEvent> chunk;
    size_t e = 0;
    juce::ScopedNoDenormals noDenormals;

    for (int pos = 0, b = 0; pos < total; pos += block, ++b)
    {
        const int n = juce::jmin (block, total - pos);
        if (perBlock) perBlock (b);
        inst.update();

        chunk.clear();
        while (e < events.size() && events[e].time < pos + n)
        {
            auto ev = events[e++].ev;
            ev.offset = juce::jmax (0, events[e - 1].time - pos);
            chunk.push_back (ev);
        }

        inst.render (out.l.data() + pos, out.r.data() + pos, n, chunk.data(), (int) chunk.size());
    }

    return out;
}

void writeWav (const juce::File& file, const Stereo& s)
{
    file.deleteFile();
    juce::AudioBuffer<float> buf (2, (int) s.l.size());
    buf.copyFrom (0, 0, s.l.data(), (int) s.l.size());
    buf.copyFrom (1, 0, s.r.data(), (int) s.r.size());

    if (auto stream = std::unique_ptr<juce::OutputStream> (file.createOutputStream()))
    {
        juce::WavAudioFormat wav;
        const auto opts = juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (2).withBitsPerSample (24);
        if (auto writer = wav.createWriterFor (stream, opts))
            writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    }
}

/** Spectral centroid (Hz) of a 4096-sample Hann window of the mid signal starting at `from`. */
float centroid (const Stereo& s, size_t from)
{
    constexpr int order = 12, size = 1 << order;
    if (from + (size_t) size > s.l.size()) return 0.0f;
    std::vector<float> data ((size_t) size * 2, 0.0f);
    for (size_t i = 0; i < (size_t) size; ++i)
        data[i] = 0.5f * (s.l[from + i] + s.r[from + i])
                  * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size));

    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (data.data());
    double num = 0.0, den = 0.0;
    for (size_t k = 1; k < (size_t) size / 2; ++k)
    {
        num += (double) k * data[k];
        den += data[k];
    }
    return den > 0.0 ? (float) (num / den * rate / size) : 0.0f;
}

/** Seconds until the level (50 ms RMS) falls 40 dB below its first-50 ms value. */
float decayTime (const Stereo& s, size_t from)
{
    const size_t win = (size_t) secs (0.05);
    const float ref = rmsOf (s, from, from + win);
    if (ref <= 0.0f) return 0.0f;
    for (size_t p = from; p + win < s.l.size(); p += win)
        if (rmsOf (s, p, p + win) < ref * 0.01f)
            return (float) ((double) (p - from) / rate);
    return (float) ((double) (s.l.size() - from) / rate);
}

//==============================================================================
struct Subject
{
    const InstrumentType& type;
    std::unique_ptr<Instrument> inst;
    std::vector<float> values;

    Subject (const InstrumentType& t, const InstrumentInfo::Patch* patch) : type (t), inst (t.create())
    {
        values = patchValues (t.info, patch);
        restart();
    }

    void restart()
    {
        inst->prepare (rate, block);
        applyKnobs();
    }

    void applyKnobs()
    {
        for (size_t k = 0; k < values.size(); ++k)
            inst->setKnob ((int) k, values[k]);
        inst->update();
    }

    bool isDrums() const                        { return type.info.category == "Drums"; }
};

const int drumNotes[] { 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 59 };

std::vector<TimedEvent> chordEvents (std::vector<TimedEvent> ev = {})
{
    for (int n : { 48, 52, 55, 60, 64, 67, 72, 76 })
    {
        ev.push_back (on (0.05, n, 1.0f));
        ev.push_back (off (2.5, n));
    }
    return ev;
}

std::vector<TimedEvent> grooveEvents()
{
    std::vector<TimedEvent> ev;
    const double beat = 60.0 / 110.0, eighth = beat * 0.5;
    for (int bar = 0; bar < 2; ++bar)
    {
        const double t0 = 0.05 + bar * 4 * beat;
        for (int i = 0; i < 8; ++i)
        {
            const double t = t0 + i * eighth;
            ev.push_back (on (t, i == 7 && bar == 1 ? 46 : 42, i % 2 == 0 ? 0.8f : 0.5f));
            if (i == 0 || i == 3 || i == 4) ev.push_back (on (t, 36, i == 3 ? 0.7f : 1.0f));
            if (i == 2 || i == 6)          ev.push_back (on (t, 38, 0.95f));
        }
    }
    const double fill = 0.05 + 8 * beat;
    int k = 0;
    for (int n : { 50, 48, 47, 45, 43, 41 }) ev.push_back (on (fill + (k++) * beat * 0.25, n, 0.9f));
    ev.push_back (on (fill + 6 * beat * 0.25, 49, 1.0f));
    ev.push_back (on (fill + 6 * beat * 0.25, 36, 1.0f));
    return ev;
}

/** Arpeggio C3..C6 in 16ths with varied velocities; sustain pedal over the middle octave,
    then a held note with a bend sweep and the mod wheel. Returns the time of all-notes-off. */
double phraseEvents (std::vector<TimedEvent>& ev)
{
    const int scale[] { 0, 4, 7, 11, 12 };
    const float vels[] { 0.35f, 0.7f, 0.95f, 0.55f, 1.0f, 0.2f };
    double t = 0.05;
    int k = 0;
    for (int oct = 0; oct < 3; ++oct)
    {
        if (oct == 1) ev.push_back (ctl (t, NoteEvent::sustain, 1.0f));
        if (oct == 2) ev.push_back (ctl (t, NoteEvent::sustain, 0.0f));
        for (int s = 0; s < 4; ++s, ++k)
        {
            const int n = 48 + 12 * oct + scale[s];
            ev.push_back (on (t, n, vels[k % 6]));
            ev.push_back (off (t + 0.09, n));
            t += 0.107;
        }
    }
    // Repeated notes (same-key retrigger) and a held, bent note with modulation.
    for (int i = 0; i < 4; ++i) { ev.push_back (on (t, 72, 0.6f + 0.1f * (float) i)); ev.push_back (off (t + 0.05, 72)); t += 0.07; }
    ev.push_back (on (t, 67, 0.8f));
    ev.push_back (ctl (t + 0.1, NoteEvent::modWheel, 1.0f));
    for (int i = 0; i <= 10; ++i) ev.push_back (ctl (t + 0.2 + 0.05 * i, NoteEvent::pitchBend, (float) i / 10.0f));
    for (int i = 0; i <= 10; ++i) ev.push_back (ctl (t + 0.8 + 0.05 * i, NoteEvent::pitchBend, 1.0f - (float) i / 5.0f));
    ev.push_back (ctl (t + 1.5, NoteEvent::pitchBend, 0.0f));
    ev.push_back (ctl (t + 1.5, NoteEvent::modWheel, 0.0f));
    ev.push_back (on (t + 1.6, 36, 1.0f));
    ev.push_back (ctl (t + 1.7, NoteEvent::sustain, 1.0f));
    const double end = t + 2.2;
    ev.push_back (ctl (end, NoteEvent::allNotesOff, 0.0f));
    return end;
}

struct PatchResult
{
    juce::String type, patch;
    float levelDb = 0, peakDb = 0, tailDb = -200;
    double cpu = 0;
    juce::StringArray problems;
};

PatchResult testPatch (const InstrumentType& t, const InstrumentInfo::Patch& patch, const juce::File& renderDir)
{
    PatchResult res;
    res.type = t.info.type;
    res.patch = patch.name;
    auto fail = [&res] (const juce::String& why) { res.problems.addIfNotAlreadyThere (why); };

    Subject s (t, &patch);
    float peak = 0.0f;

    // 1. No events, no output.
    {
        auto out = play (*s.inst, {}, secs (1.0));
        if (! finite (out)) fail ("NaN/inf from silence");
        else if (peakOf (out) > 1.0e-7f) fail ("output with no events (" + juce::String (toDb (peakOf (out)), 1) + " dBFS)");
    }

    // 2. Loudness: the chord (or the groove), RMS over its first second.
    Stereo demo;
    {
        s.restart();
        auto out = s.isDrums() ? play (*s.inst, grooveEvents(), secs (6.0)) : play (*s.inst, chordEvents(), secs (4.5));
        if (! finite (out)) fail ("NaN/inf");
        res.levelDb = toDb (s.isDrums() ? rmsOf (out, 0, (size_t) secs (4.5)) : rmsOf (out, (size_t) secs (0.05), (size_t) secs (1.05)));
        peak = juce::jmax (peak, peakOf (out));
        if (res.levelDb < -60.0f) fail ("silent when played");
        else if (res.levelDb < levelMinDb || res.levelDb > levelMaxDb)
            fail ("level " + juce::String (res.levelDb, 1) + " dBFS (window " + juce::String (levelMinDb, 0) + ".." + juce::String (levelMaxDb, 0) + ")");
        demo = std::move (out);
    }

    // 3. The phrase (or every drum note alone), then 10 s after all-notes-off.
    {
        s.restart();
        std::vector<TimedEvent> ev;
        double end = 0.0;
        if (s.isDrums())
        {
            double tt = 0.05;
            for (int n : drumNotes) { ev.push_back (on (tt, n, 0.5f + 0.5f * (float) ((n * 7) % 5) / 4.0f)); tt += 0.5; }
            end = tt;
            ev.push_back (ctl (end, NoteEvent::allNotesOff, 0.0f));
        }
        else
            end = phraseEvents (ev);

        auto out = play (*s.inst, ev, secs (end + 10.5));
        if (! finite (out)) fail ("NaN/inf in phrase");
        peak = juce::jmax (peak, peakOf (out, 0, (size_t) secs (end)));
        res.tailDb = toDb (peakOf (out, (size_t) secs (end + 10.0)));
        if (res.tailDb > -90.0f) fail ("still sounding 10 s after all-notes-off (" + juce::String (res.tailDb, 0) + " dBFS)");

        if (s.isDrums())
        {
            juce::StringArray quiet;
            for (size_t i = 0; i < std::size (drumNotes); ++i)
                if (peakOf (out, (size_t) secs (0.05 + 0.5 * (double) i), (size_t) secs (0.5 + 0.5 * (double) i)) < 0.003f)
                    quiet.add (juce::String (drumNotes[i]));
            if (! quiet.isEmpty()) fail ("silent notes " + quiet.joinIntoString (","));
        }

        demo.l.insert (demo.l.end(), out.l.begin(), out.l.begin() + secs (end + 2.0));
        demo.r.insert (demo.r.end(), out.r.begin(), out.r.begin() + secs (end + 2.0));
    }

    res.peakDb = toDb (peak);
    if (res.peakDb > peakLimitDb) fail ("peak " + juce::String (res.peakDb, 2) + " dBFS");

    // 4. Random knob automation every block must stay finite and bounded.
    {
        s.restart();
        juce::Random rng (11);
        std::vector<TimedEvent> ev;
        phraseEvents (ev);
        auto out = play (*s.inst, s.isDrums() ? grooveEvents() : ev, secs (4.0), [&] (int)
        {
            for (size_t k = 0; k < t.info.knobs.size(); ++k)
                s.inst->setKnob ((int) k, t.info.knobs[k].toReal (rng.nextFloat()));
        });
        if (! finite (out)) fail ("NaN/inf under knob automation");
        else if (peakOf (out) > 4.0f) fail ("blows up under knob automation (" + juce::String (toDb (peakOf (out)), 1) + " dBFS)");
    }

    // 5. CPU: a 16-voice chord (or 16 drums at once), steady-state blocks.
    {
        s.restart();
        std::vector<NoteEvent> notes;
        for (int i = 0; i < 16; ++i)
        {
            NoteEvent e;
            e.note = s.isDrums() ? drumNotes[(size_t) i] : 36 + i * 3;
            e.value = 0.8f;
            notes.push_back (e);
        }

        float l[block] {}, r[block] {};
        juce::ScopedNoDenormals noDenormals;
        s.inst->render (l, r, block, notes.data(), (int) notes.size());
        for (int i = 0; i < 20; ++i) s.inst->render (l, r, block, nullptr, 0);

        constexpr int blocks = 750;
        const auto t0 = juce::Time::getHighResolutionTicks();
        for (int b = 0; b < blocks; ++b)
        {
            if (s.isDrums() && b % 94 == 0) s.inst->render (l, r, block, notes.data(), (int) notes.size());   // retrigger every ~125 ms
            s.inst->update();
            s.inst->render (l, r, block, nullptr, 0);
        }
        res.cpu = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1.0e6 / blocks;
        if (res.cpu > cpuBudgetUs) fail ("CPU " + juce::String (res.cpu, 0) + " us/block");
    }

    if (renderDir != juce::File())
        writeWav (renderDir.getChildFile (juce::File::createLegalFileName (t.info.type + " - " + patch.name) + ".wav"), demo);

    return res;
}

/** Listening proxies for one instrument's default patch. */
void analyse (const InstrumentType& t)
{
    Subject s (t, t.info.patches.empty() ? nullptr : &t.info.patches.front());
    std::cout << "  " << t.info.type << " (" << (t.info.patches.empty() ? juce::String ("defaults") : t.info.patches.front().name) << ")\n";

    if (s.isDrums())
    {
        for (int n : drumNotes)
        {
            s.restart();
            auto out = play (*s.inst, { on (0.0, n, 1.0f) }, secs (2.0));
            std::cout << "    note " << n << ": peak " << juce::String (toDb (peakOf (out)), 1) << " dB, centroid "
                      << juce::roundToInt (centroid (out, 0)) << " Hz, -40 dB after " << juce::String (decayTime (out, 0), 2) << " s\n";
        }
        return;
    }

    for (int note : { 36, 60, 84 })
    {
        juce::String line = "    note " + juce::String (note) + ":";
        for (float vel : { 0.3f, 0.7f, 1.0f })
        {
            s.restart();
            auto out = play (*s.inst, { on (0.0, note, vel), off (3.0, note) }, secs (4.0));
            line << "  v" << juce::String (vel, 1) << " rms " << juce::String (toDb (rmsOf (out, 0, (size_t) secs (0.3))), 1)
                 << " c0 " << juce::roundToInt (centroid (out, (size_t) secs (0.02)))
                 << " c1.5 " << juce::roundToInt (centroid (out, (size_t) secs (1.5)));
            if (vel > 0.9f)
                line << "  decay(-40dB) " << juce::String (decayTime (out, (size_t) secs (0.01)), 2) << " s"
                     << "  after off " << juce::String (toDb (peakOf (out, (size_t) secs (3.3))), 0) << " dB";
        }
        std::cout << line << "\n";
    }

    for (size_t k = 0; k < t.info.knobs.size(); ++k)
    {
        const auto& spec = t.info.knobs[k];
        if (! (spec.id == "cutoff" || spec.id == "tone" || spec.id == "bright" || spec.id == "brightness"))
            continue;
        juce::String line = "    " + spec.id + ":";
        for (float norm : { 0.0f, 0.5f, 1.0f })
        {
            s.restart();
            s.values[k] = spec.toReal (norm);
            s.applyKnobs();
            auto out = play (*s.inst, { on (0.0, 48, 0.8f), off (1.0, 48) }, secs (1.2));
            line << "  " << spec.format (spec.toReal (norm)) << " -> " << juce::roundToInt (centroid (out, (size_t) secs (0.2))) << " Hz";
        }
        s.values = patchValues (t.info, t.info.patches.empty() ? nullptr : &t.info.patches.front());
        std::cout << line << "\n";
    }
}

} // namespace

int runInstrumentTest (const juce::StringArray& args);
int runInstrumentTest (const juce::StringArray& args)
{
    const auto& registry = InstrumentRegistry::get();
    juce::File renderDir;
    juce::StringArray only;
    bool analysis = false;

    for (int i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--render" && i + 1 < args.size()) renderDir = juce::File::getCurrentWorkingDirectory().getChildFile (args[++i]);
        else if (args[i] == "--analyse")                 analysis = true;
        else                                             only.add (args[i]);
    }

    if (renderDir != juce::File())
        renderDir.createDirectory();

    int failures = 0, tested = 0;

    // Registry-level checks.
    juce::StringArray ids;
    for (auto& t : registry.all())
    {
        juce::StringArray problems;
        if (ids.contains (t.info.type))                                 problems.add ("duplicate type id");
        ids.add (t.info.type);
        if (t.info.knobs.size() > (size_t) Instrument::maxKnobs)        problems.add ("too many knobs (" + juce::String ((int) t.info.knobs.size()) + ")");
        if (! InstrumentRegistry::categories().contains (t.info.category)) problems.add ("unknown category " + t.info.category);
        if (t.info.patches.empty())                                     problems.add ("no patches");

        juce::StringArray knobIds;
        for (auto& k : t.info.knobs)
        {
            if (knobIds.contains (k.id)) problems.add ("duplicate knob id " + k.id);
            knobIds.add (k.id);
        }

        juce::StringArray patchNames;
        for (auto& p : t.info.patches)
        {
            if (patchNames.contains (p.name)) problems.add ("duplicate patch " + p.name);
            patchNames.add (p.name);
            for (auto& [id, value] : p.values)
            {
                const int k = knobIds.indexOf (id);
                if (k < 0) problems.add ("patch " + p.name + " sets unknown knob " + id);
                else
                {
                    const auto r = t.info.knobs[(size_t) k].range();
                    if (value < r.start || value > r.end) problems.add ("patch " + p.name + " sets " + id + " out of range");
                }
            }
        }

        if (! problems.isEmpty())
        {
            std::cout << t.info.type << ": FAIL: " << problems.joinIntoString ("; ") << "\n";
            ++failures;
        }
    }

    std::cout << juce::String ("instrument").paddedRight (' ', 18) << juce::String ("patch").paddedRight (' ', 20)
              << " level   peak   tail  cpu(us)  result\n";

    float lo = 200.0f, hi = -200.0f;
    double worstCpu = 0.0;

    for (auto& t : registry.all())
    {
        if (! only.isEmpty() && ! only.contains (t.info.type))
            continue;

        for (auto& p : t.info.patches)
        {
            const auto r = testPatch (t, p, renderDir);
            ++tested;
            failures += r.problems.isEmpty() ? 0 : 1;
            lo = juce::jmin (lo, r.levelDb);
            hi = juce::jmax (hi, r.levelDb);
            worstCpu = juce::jmax (worstCpu, r.cpu);

            std::cout << r.type.paddedRight (' ', 18) << r.patch.paddedRight (' ', 20)
                      << juce::String (r.levelDb, 1).paddedLeft (' ', 6)
                      << juce::String (r.peakDb, 1).paddedLeft (' ', 7)
                      << juce::String (juce::roundToInt (juce::jmax (r.tailDb, -199.0f))).paddedLeft (' ', 7)
                      << juce::String (r.cpu, 1).paddedLeft (' ', 9) << "  "
                      << (r.problems.isEmpty() ? "PASS" : "FAIL: " + r.problems.joinIntoString ("; ")) << "\n";
        }

        if (analysis)
            analyse (t);
    }

    std::cout << tested << " patches tested, " << failures << " failed; loudness " << juce::String (lo, 1) << " .. "
              << juce::String (hi, 1) << " dBFS RMS, worst CPU " << juce::String (worstCpu, 1) << " us/block\n";
    return failures == 0 ? 0 : 1;
}
