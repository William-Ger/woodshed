// woodshed-bench: offline checks for every pedal in the registry.
//
//   woodshed-bench list
//   woodshed-bench knobs [type ...]
//   woodshed-bench run [type ...] [--wav guitar.wav] [--render outDir]
//   woodshed-bench presets [file.json ...] [--wav guitar.wav] [--render outDir]
//   woodshed-bench project      (multitrack engine checks; see ProjectTest.cpp)
//   woodshed-bench score        (Score editor notation layout checks; see ScoreTest.cpp)
//   woodshed-bench instruments [type ...] [--render dir] [--analyse]   (see InstrumentTest.cpp)
//   woodshed-bench pitchcorrect [--wav guitar.wav] [--render outDir]   (PLUMB accuracy in cents)
//   woodshed-bench sampler      (the Sampler instrument; see SamplerTest.cpp)
//
// Each pedal runs alone at 48 kHz in 64-sample blocks, through the same Chain
// code the app uses. A pedal fails if it produces NaN/inf, makes noise from
// silence, changes the level of a guitar DI by more than 3 dB at default
// settings, blows up at any knob extreme or under random knob automation, or
// costs more than 100 us per 64-sample stereo block.
//
// `presets` builds each factory preset's whole rig (board, amp, cab) and plays the
// guitar DI through it. A preset fails if its file is malformed, it produces NaN/inf,
// peaks above 0 dBFS, lands outside the shared loudness window (so switching presets
// doesn't jump in volume), or the whole rig costs more than 600 us per block.

#include "../../Source/engine/Chain.h"
#include "../../Source/engine/PitchDetector.h"
#include "../../Source/engine/Presets.h"
#include <deque>
#include <juce_audio_formats/juce_audio_formats.h>

using namespace woodshed;

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 64;
constexpr float levelTolerance = 3.0f;
constexpr float silenceLimitDb = -80.0f;
constexpr float extremePeakLimit = 4.0f;    // +12 dBFS before the output safety clip
constexpr double cpuBudgetUs = 100.0;

/** Shared CI machines are slower and noisier than a desk: WOODSHED_BENCH_CPU_SCALE widens the
    CPU budgets (the CI sets it). */
double cpuScale()
{
    const auto scale = juce::SystemStats::getEnvironmentVariable ("WOODSHED_BENCH_CPU_SCALE", "1").getDoubleValue();
    return scale > 0.0 ? scale : 1.0;
}

using Signal = std::vector<float>;

float toDb (float g)                        { return juce::Decibels::gainToDecibels (g, -200.0f); }

float rms (const Signal& s, size_t from = 0)
{
    double sum = 0.0;
    for (size_t i = from; i < s.size(); ++i) sum += (double) s[i] * s[i];
    return (float) std::sqrt (sum / (double) juce::jmax ((size_t) 1, s.size() - from));
}

float peak (const Signal& s, size_t from = 0)
{
    float p = 0.0f;
    for (size_t i = from; i < s.size(); ++i) p = juce::jmax (p, std::abs (s[i]));
    return p;
}

bool allFinite (const Signal& s)
{
    return std::all_of (s.begin(), s.end(), [] (float v) { return std::isfinite (v); });
}

Signal loadGuitar (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    if (auto reader = std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (file)))
    {
        if (std::abs (reader->sampleRate - sampleRate) > 1.0)
        {
            std::cerr << "warning: " << file.getFullPathName() << " is not 48 kHz; using synthetic guitar\n";
            return {};
        }

        juce::AudioBuffer<float> buf ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&buf, 0, (int) reader->lengthInSamples, 0, true, true);
        return Signal (buf.getReadPointer (0), buf.getReadPointer (0) + buf.getNumSamples());
    }

    return {};
}

/** Karplus-Strong plucks around a DI's level, used when no recording is available. */
Signal syntheticGuitar()
{
    Signal out ((size_t) (sampleRate * 8.0), 0.0f);
    juce::Random rng (42);
    const float notes[] { 82.4f, 110.0f, 146.8f, 196.0f, 246.9f, 329.6f, 220.0f, 164.8f };

    for (int n = 0; n < 8; ++n)
    {
        const int start = (int) (n * sampleRate * 0.9);
        const int period = (int) (sampleRate / notes[n]);
        Signal line ((size_t) period);
        for (auto& v : line) v = rng.nextFloat() * 2.0f - 1.0f;

        for (int i = 0, idx = 0; i < (int) (sampleRate * 2.0) && start + i < (int) out.size(); ++i)
        {
            const int next = (idx + 1) % period;
            const float v = line[(size_t) idx];
            line[(size_t) idx] = 0.996f * 0.5f * (v + line[(size_t) next]);
            out[(size_t) (start + i)] += v * 0.25f;
            idx = next;
        }
    }

    return out;
}

Signal sine (float hz, float db, double seconds)
{
    Signal s ((size_t) (sampleRate * seconds));
    const float a = juce::Decibels::decibelsToGain (db);
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = a * std::sin (juce::MathConstants<float>::twoPi * hz * (float) i / (float) sampleRate);
    return s;
}

//==============================================================================
struct Rig
{
    const PedalType& type;
    std::shared_ptr<Pedal> pedal;
    std::atomic<float> on { 1.0f };
    std::array<std::atomic<float>, Pedal::maxKnobs> knobs;
    Chain chain;

    explicit Rig (const PedalType& t) : type (t)
    {
        std::array<const std::atomic<float>*, Pedal::maxKnobs> sources {};
        for (size_t k = 0; k < knobs.size(); ++k)
        {
            knobs[k] = k < t.info.knobs.size() ? t.info.knobs[k].defaultNorm() : 0.5f;
            sources[k] = &knobs[k];
        }

        pedal = t.create();
        chain.entries.push_back (Chain::makeEntry (pedal, t.info, &on, sources.data()));
        restart();
    }

    void restart()
    {
        pedal->prepare (sampleRate, blockSize);
        pedal->engaged.reset (sampleRate, 0.015);
        pedal->engaged.setCurrentAndTargetValue (1.0f);
    }

    void setDefaults()
    {
        for (size_t k = 0; k < type.info.knobs.size(); ++k)
            knobs[k] = type.info.knobs[k].defaultNorm();
    }

    /** Runs a mono signal through the chain; returns left and right. */
    std::pair<Signal, Signal> run (const Signal& in, std::function<void (int block)> perBlock = {})
    {
        Signal l (in), r (in.size());
        float scratchL[blockSize], scratchR[blockSize];

        for (size_t pos = 0, block = 0; pos < in.size(); pos += blockSize, ++block)
        {
            if (perBlock) perBlock ((int) block);
            const int n = (int) juce::jmin ((size_t) blockSize, in.size() - pos);
            chain.process (l.data() + pos, r.data() + pos, scratchL, scratchR, n);
        }

        return { std::move (l), std::move (r) };
    }

    double cpuMicrosPerBlock (const Signal& in)
    {
        restart();
        Signal l (in), r (in);
        const int blocks = (int) (in.size() / blockSize);

        juce::ScopedNoDenormals noDenormals;
        const auto t0 = juce::Time::getHighResolutionTicks();

        for (int b = 0; b < blocks; ++b)
        {
            for (size_t k = 0; k < type.info.knobs.size(); ++k)
                pedal->setKnob ((int) k, type.info.knobs[k].def);

            pedal->update();
            float* L = l.data() + b * blockSize;
            float* R = r.data() + b * blockSize;

            if (pedal->producesStereo())
                pedal->processStereo (L, R, blockSize);
            else
            {
                pedal->processChannel (0, L, blockSize);
                pedal->processChannel (1, R, blockSize);
            }
        }

        const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
        return seconds * 1.0e6 / blocks;
    }
};

void writeWav (const juce::File& file, const Signal& l, const Signal& r)
{
    file.deleteFile();
    juce::AudioBuffer<float> buf (2, (int) l.size());
    buf.copyFrom (0, 0, l.data(), (int) l.size());
    buf.copyFrom (1, 0, r.data(), (int) r.size());

    if (auto stream = std::unique_ptr<juce::OutputStream> (file.createOutputStream()))
    {
        juce::WavAudioFormat wav;
        const auto opts = juce::AudioFormatWriterOptions{}.withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (24);
        if (auto writer = wav.createWriterFor (stream, opts))
            writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    }
}

struct Result
{
    juce::String type, name;
    float levelDelta = 0, outPeakDb = 0, silenceDb = 0, worstExtremeDb = 0;
    double cpu = 0;
    int latency = 0;
    juce::StringArray problems;
};

Result test (const PedalType& t, const Signal& guitar, const juce::File& renderDir)
{
    Result res;
    res.type = t.info.type;
    res.name = t.info.name;
    Rig rig (t);
    auto fail = [&res] (const juce::String& why) { res.problems.add (why); };

    // 1. Silence in, silence out.
    {
        rig.restart();
        auto [l, r] = rig.run (Signal ((size_t) (sampleRate * 3.0), 0.0f));
        const size_t tail = (size_t) (sampleRate * 2.0);
        res.silenceDb = toDb (juce::jmax (peak (l, tail), peak (r, tail)));
        if (! allFinite (l) || ! allFinite (r)) fail ("NaN/inf from silence");
        else if (res.silenceDb > silenceLimitDb) fail ("noise from silence (" + juce::String (res.silenceDb, 1) + " dBFS)");
    }

    // 2. Guitar at defaults: level match and no overs.
    {
        rig.restart();
        auto [l, r] = rig.run (guitar);
        const float inRms = rms (guitar);
        const float outRms = 0.5f * (rms (l) + rms (r));
        res.levelDelta = toDb (outRms) - toDb (inRms);
        res.outPeakDb = toDb (juce::jmax (peak (l), peak (r)));

        if (! allFinite (l) || ! allFinite (r)) fail ("NaN/inf on guitar");
        if (std::abs (res.levelDelta) > levelTolerance) fail ("level " + juce::String (res.levelDelta, 1) + " dB at defaults");
        if (res.outPeakDb > -0.1f) fail ("clips at defaults (peak " + juce::String (res.outPeakDb, 1) + " dBFS)");

        if (renderDir != juce::File())
            writeWav (renderDir.getChildFile (t.info.type + ".wav"), l, r);
    }

    // 3. Sine at -20 dBFS.
    {
        rig.restart();
        auto [l, r] = rig.run (sine (220.0f, -20.0f, 2.0));
        if (! allFinite (l) || ! allFinite (r)) fail ("NaN/inf on sine");
    }

    // 4. Every knob at both extremes (others at default).
    const Signal shortGuitar (guitar.begin(), guitar.begin() + (long) juce::jmin (guitar.size(), (size_t) (sampleRate * 3.0)));
    float worst = 0.0f;

    for (size_t k = 0; k < t.info.knobs.size(); ++k)
    {
        for (float v : { 0.0f, 1.0f })
        {
            rig.setDefaults();
            rig.knobs[k] = v;
            rig.restart();
            auto [l, r] = rig.run (shortGuitar);
            const float p = juce::jmax (peak (l), peak (r));
            worst = juce::jmax (worst, p);

            if (! allFinite (l) || ! allFinite (r))
                fail ("NaN/inf with " + t.info.knobs[k].label + " at " + (v > 0.5f ? "max" : "min"));
            else if (p > extremePeakLimit)
                fail ("blows up with " + t.info.knobs[k].label + " at " + (v > 0.5f ? "max" : "min")
                      + " (" + juce::String (toDb (p), 1) + " dBFS)");
        }
    }

    // 5. Random knob automation every block.
    {
        rig.setDefaults();
        rig.restart();
        juce::Random rng (7);
        auto [l, r] = rig.run (shortGuitar, [&] (int)
        {
            for (size_t k = 0; k < t.info.knobs.size(); ++k)
                rig.knobs[k] = rng.nextFloat();
        });

        if (! allFinite (l) || ! allFinite (r)) fail ("NaN/inf under knob automation");
        worst = juce::jmax (worst, peak (l), peak (r));
        if (juce::jmax (peak (l), peak (r)) > extremePeakLimit) fail ("blows up under knob automation");
    }

    res.worstExtremeDb = toDb (worst);

    // 6. CPU, stereo, defaults.
    rig.setDefaults();
    res.cpu = rig.cpuMicrosPerBlock (guitar);
    if (res.cpu > cpuBudgetUs * cpuScale()) fail ("CPU " + juce::String (res.cpu, 1) + " us/block (budget " + juce::String (cpuBudgetUs, 0) + ")");

    res.latency = rig.pedal->getLatencySamples();
    return res;
}


//==============================================================================
// Whole-preset checks.
constexpr float presetLevelMin = -3.0f, presetLevelMax = 3.0f;     // output RMS vs the DI, dB
constexpr float presetPeakLimit = 1.0f;                            // 0 dBFS, before the safety clip
constexpr double presetCpuBudgetUs = 600.0;

struct BoardRig
{
    struct Slot
    {
        std::shared_ptr<Pedal> pedal;
        std::atomic<float> on { 1.0f };
        std::array<std::atomic<float>, Pedal::maxKnobs> knobs;
        bool startsOn = true;
    };

    std::deque<Slot> slots;
    Chain chain;

    explicit BoardRig (const Preset& preset)
    {
        for (auto& p : preset.pedals)
        {
            const auto& t = *Registry::get().find (p.type);
            auto& s = slots.emplace_back();
            s.startsOn = p.on;
            s.on = p.on ? 1.0f : 0.0f;

            std::array<const std::atomic<float>*, Pedal::maxKnobs> sources {};
            for (size_t k = 0; k < s.knobs.size(); ++k)
            {
                s.knobs[k] = p.knobs[k];
                sources[k] = &s.knobs[k];
            }

            s.pedal = t.create();
            chain.entries.push_back (Chain::makeEntry (s.pedal, t.info, &s.on, sources.data()));
        }

        restart();
    }

    void restart()
    {
        for (auto& s : slots)
        {
            s.pedal->prepare (sampleRate, blockSize);
            s.pedal->engaged.reset (sampleRate, 0.015);
            s.pedal->engaged.setCurrentAndTargetValue (s.startsOn ? 1.0f : 0.0f);
        }
    }

    std::pair<Signal, Signal> run (const Signal& in, double& microsPerBlock)
    {
        Signal l (in), r (in.size());
        float scratchL[blockSize], scratchR[blockSize];
        juce::ScopedNoDenormals noDenormals;
        const auto t0 = juce::Time::getHighResolutionTicks();
        int blocks = 0;

        for (size_t pos = 0; pos < in.size(); pos += blockSize, ++blocks)
        {
            const int n = (int) juce::jmin ((size_t) blockSize, in.size() - pos);
            chain.process (l.data() + pos, r.data() + pos, scratchL, scratchR, n);
        }

        microsPerBlock = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) * 1.0e6 / juce::jmax (1, blocks);
        return { std::move (l), std::move (r) };
    }
};

int runPresets (const juce::Array<juce::File>& files, const Signal& guitar, const juce::File& renderDir)
{
    juce::StringArray seen;
    int failures = 0, tested = 0;

    // The DI, then six seconds of silence so tails can be checked.
    Signal input (guitar);
    input.resize (guitar.size() + (size_t) (sampleRate * 6.0), 0.0f);
    const float inRms = rms (guitar);

    std::cout << juce::String ("genre").paddedRight (' ', 20) << juce::String ("preset").paddedRight (' ', 26)
              << "level  peak   tail  cpu(us)  result\n";

    for (auto& file : files)
    {
        juce::StringArray errors;
        const auto presets = parsePresets (file.loadFileAsString(), errors);

        for (auto& e : errors)
        {
            std::cout << file.getFileName() << ": FAIL: " << e << "\n";
            ++failures;
        }

        for (auto& p : presets)
        {
            ++tested;
            juce::StringArray problems;

            if (seen.contains (p.name, true)) problems.add ("duplicate name");
            seen.add (p.name);

            BoardRig rig (p);
            double cpu = 0.0;
            auto [l, r] = rig.run (input, cpu);

            Signal playedL (l.begin(), l.begin() + (long) guitar.size()), playedR (r.begin(), r.begin() + (long) guitar.size());
            const float level = toDb (0.5f * (rms (playedL) + rms (playedR))) - toDb (inRms);
            const float pk = juce::jmax (peak (l), peak (r));
            const size_t lastSecond = input.size() - (size_t) sampleRate;
            const float tail = toDb (juce::jmax (peak (l, lastSecond), peak (r, lastSecond)));

            if (! allFinite (l) || ! allFinite (r))                    problems.add ("NaN/inf");
            if (pk > presetPeakLimit)                                  problems.add ("peaks at " + juce::String (toDb (pk), 1) + " dBFS");
            if (level < presetLevelMin || level > presetLevelMax)      problems.add ("level " + juce::String (level, 1) + " dB (window "
                                                                                     + juce::String (presetLevelMin, 0) + ".." + juce::String (presetLevelMax, 0) + ")");
            if (cpu > presetCpuBudgetUs * cpuScale())                               problems.add ("CPU " + juce::String (cpu, 0) + " us/block");

            failures += problems.isEmpty() ? 0 : 1;

            std::cout << p.genre.paddedRight (' ', 20) << p.name.paddedRight (' ', 26)
                      << juce::String (level, 1).paddedLeft (' ', 5)
                      << juce::String (toDb (pk), 1).paddedLeft (' ', 6)
                      << juce::String (juce::roundToInt (juce::jmax (tail, -99.0f))).paddedLeft (' ', 7)
                      << juce::String (juce::roundToInt (cpu)).paddedLeft (' ', 9) << "  "
                      << (problems.isEmpty() ? "PASS" : "FAIL: " + problems.joinIntoString ("; ")) << "\n";

            if (renderDir != juce::File())
                writeWav (renderDir.getChildFile (juce::File::createLegalFileName (p.genre + " - " + p.name) + ".wav"), l, r);
        }
    }

    std::cout << tested << " presets tested, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}

//==============================================================================
// pitchcorrect: accuracy checks for PLUMB (pitch.correct) on synthetic sung notes.
namespace pc
{
/** The vocal tract of an "ah": three formant peaks over a floor, as a gain at f Hz. */
float vowelEnvelope (float f)
{
    const float formants[][2] { { 730.0f, 90.0f }, { 1090.0f, 110.0f }, { 2440.0f, 170.0f } };
    const float gains[] { 1.0f, 0.5f, 0.25f };
    float env = 0.08f;
    for (int j = 0; j < 3; ++j)
    {
        const float d = (f - formants[j][0]) / formants[j][1];
        env += gains[j] / (1.0f + d * d);
    }
    return env;
}

/** A sung vowel: a falling harmonic source through vowelEnvelope (its formants scaled by
    `formantScale`), plus a little breath. `hzAt` gives the pitch at time t. */
Signal vowel (std::function<double (double)> hzAt, double seconds, double formantScale = 1.0, float db = -14.0f)
{
    Signal s ((size_t) (sampleRate * seconds));
    juce::Random rng (11);
    const float amp = juce::Decibels::decibelsToGain (db);
    double phase = 0.0;

    for (size_t i = 0; i < s.size(); ++i)
    {
        const double t = (double) i / sampleRate;
        const double hz = hzAt (t);
        phase += hz / sampleRate;
        phase -= std::floor (phase);

        float v = 0.0f;
        for (int k = 1; k * hz < 5000.0; ++k)
        {
            v += vowelEnvelope ((float) (k * hz / formantScale)) / (float) k * std::sin (juce::MathConstants<float>::twoPi * (float) k * (float) phase);
        }

        const float fade = (float) juce::jmin (1.0, t / 0.02, (seconds - t) / 0.02);
        s[i] = amp * fade * (v + 0.003f * (rng.nextFloat() * 2.0f - 1.0f));
    }

    return s;
}

double cents (double hz, double ref)                { return 1200.0 * std::log2 (hz / ref); }
double noteHz (int midiNote)                        { return 440.0 * std::exp2 ((midiNote - 69) / 12.0); }

/** Pitch frames (Hz, 0 when unvoiced) over [from, to) seconds, 43 ms windows every 5 ms. */
std::vector<float> frames (const Signal& s, double from, double to)
{
    static PitchDetector detector (2048);
    std::vector<float> out;
    for (size_t pos = (size_t) (from * sampleRate); pos + 2048 <= juce::jmin (s.size(), (size_t) (to * sampleRate)); pos += 240)
    {
        const auto r = detector.analyse (s.data() + pos, 2048, sampleRate, 60.0f, 1500.0f);
        out.push_back (r.clarity > 0.8f ? r.frequency : 0.0f);
    }
    return out;
}

struct Stats { double mean = 0, maxDev = 0, spread = 0; int voiced = 0; };

/** Mean cents vs ref, worst frame deviation from ref, and peak-to-peak spread. */
Stats measure (const Signal& s, double from, double to, double ref)
{
    Stats st;
    double lo = 1.0e9, hi = -1.0e9;
    for (float hz : frames (s, from, to))
    {
        if (hz <= 0.0f) continue;
        const double c = cents (hz, ref);
        st.mean += c;
        st.maxDev = juce::jmax (st.maxDev, std::abs (c));
        lo = juce::jmin (lo, c);
        hi = juce::jmax (hi, c);
        ++st.voiced;
    }
    st.mean /= juce::jmax (1, st.voiced);
    st.spread = st.voiced > 0 ? hi - lo : 0.0;
    return st;
}

/** How far the harmonic levels of a steady note at `hz` (0.4..1.9 s) are from the vowel
    model with formants scaled by `formantScale`: dB RMS, after removing the overall gain. */
double harmonicDistance (const Signal& s, double hz, double formantScale)
{
    constexpr int order = 13, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) size * 2), mag ((size_t) size / 2, 0.0f);
    for (size_t pos = (size_t) (0.4 * sampleRate); pos + size <= (size_t) (1.9 * sampleRate); pos += size / 2)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = s[pos + (size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (size_t k = 0; k < mag.size(); ++k) mag[k] += buf[k];
    }

    std::vector<double> diff;
    for (int k = 1; k * hz < 4000.0; ++k)
    {
        const int bin = (int) std::lround (k * hz * size / sampleRate);
        float m = 0.0f;
        for (int b = bin - 3; b <= bin + 3; ++b) m = juce::jmax (m, mag[(size_t) b]);
        const double model = vowelEnvelope ((float) (k * hz / formantScale)) / (double) k;
        diff.push_back (20.0 * std::log10 (m + 1.0e-12) - 20.0 * std::log10 (model));
    }

    double mean = 0.0, sq = 0.0;
    for (double d : diff) mean += d;
    mean /= (double) diff.size();
    for (double d : diff) sq += (d - mean) * (d - mean);
    return std::sqrt (sq / (double) diff.size());
}

struct Corrector
{
    Rig rig;
    explicit Corrector (const PedalType& t) : rig (t) {}

    Corrector& set (const juce::String& id, float real)
    {
        for (size_t k = 0; k < rig.type.info.knobs.size(); ++k)
            if (rig.type.info.knobs[k].id == id)
                rig.knobs[k] = rig.type.info.knobs[k].toNorm (real);
        return *this;
    }

    Signal process (const Signal& in)
    {
        rig.restart();
        return rig.run (in).first;
    }
};

int scaleIndex (const PedalType& t, const juce::String& name)
{
    for (auto& k : t.info.knobs)
        if (k.id == "scale") return k.choices.indexOf (name);
    return -1;
}

void render (const juce::File& dir, const juce::String& name, const Signal& in, const Signal& out)
{
    if (dir == juce::File()) return;
    writeWav (dir.getChildFile (name + "-before.wav"), in, in);
    writeWav (dir.getChildFile (name + "-after.wav"), out, out);
}

int run (const Signal& guitar, const juce::File& renderDir)
{
    const auto* type = Registry::get().find ("pitch.correct");
    if (type == nullptr) { std::cerr << "pitch.correct is not registered\n"; return 1; }

    int failures = 0;
    const double a4 = 440.0;
    auto report = [&] (const juce::String& what, const Stats& s, double tolerance)
    {
        const bool ok = s.voiced > 10 && std::abs (s.mean) <= tolerance;
        failures += ok ? 0 : 1;
        std::cout << what.paddedRight (' ', 52) << " mean " << juce::String (s.mean, 2).paddedLeft (' ', 7)
                  << " c  worst frame " << juce::String (s.maxDev, 1).paddedLeft (' ', 6)
                  << " c  spread " << juce::String (s.spread, 1).paddedLeft (' ', 6) << " c  "
                  << (ok ? "PASS" : "FAIL") << "\n";
    };

    {
        Corrector c (*type);
        std::cout << "latency " << c.rig.pedal->getLatencySamples() << " samples ("
                  << juce::String (c.rig.pedal->getLatencySamples() * 1000.0 / sampleRate, 2) << " ms)\n";
    }

    // 1. A4 + 35 cents with +/-20 cent, 5.5 Hz vibrato.
    const auto sharpVibrato = vowel ([] (double t) { return 440.0 * std::exp2 ((35.0 + 20.0 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * t)) / 1200.0); }, 3.0);
    report ("input: A4+35c, vibrato +/-20c", measure (sharpVibrato, 0.3, 2.9, a4), 1.0e9);

    for (float response : { 0.0f, 20.0f })
    {
        Corrector c (*type);
        c.set ("response", response);
        const auto out = c.process (sharpVibrato);
        report ("chromatic, response " + juce::String (response, 0) + " ms -> A4", measure (out, 0.3, 2.9, a4), 5.0);
        if (response == 0.0f) render (renderDir, "pc-vibrato-hard", sharpVibrato, out);
    }

    {
        Corrector c (*type);
        c.set ("response", 120.0f).set ("humanize", 1.0f);
        const auto out = c.process (sharpVibrato);
        const auto s = measure (out, 1.0, 2.9, a4);
        report ("chromatic, response 120 ms, humanize 100% -> A4", s, 5.0);
        std::cout << "    (vibrato kept: spread " << juce::String (s.spread, 1) << " c of the input's ~40)\n";
        render (renderDir, "pc-vibrato-humanized", sharpVibrato, out);
    }

    // 2. Scale discrimination: A4 + 58 cents is Bb4 in Chromatic, A4 in C Major.
    const auto a4plus58 = vowel ([] (double) { return 440.0 * std::exp2 (58.0 / 1200.0); }, 2.0);
    {
        Corrector c (*type);
        c.set ("response", 0.0f);
        report ("A4+58c, chromatic -> Bb4", measure (c.process (a4plus58), 0.3, 1.9, noteHz (70)), 5.0);
    }
    {
        Corrector c (*type);
        c.set ("response", 0.0f).set ("scale", (float) scaleIndex (*type, "Major"));
        report ("A4+58c, C major -> A4", measure (c.process (a4plus58), 0.3, 1.9, a4), 5.0);
    }

    // 3. F4 + 20 cents in A minor pentatonic (A C D E G): nearest is E4.
    {
        const auto f4 = vowel ([] (double) { return noteHz (65) * std::exp2 (20.0 / 1200.0); }, 2.0);
        Corrector c (*type);
        c.set ("response", 0.0f).set ("key", 9.0f).set ("scale", (float) scaleIndex (*type, "Pent Minor"));
        report ("F4+20c, A minor pentatonic -> E4", measure (c.process (f4), 0.3, 1.9, noteHz (64)), 5.0);
    }

    // 4. Ref pitch: A4 = 432 Hz moves the target.
    {
        const auto in = vowel ([] (double) { return 437.0; }, 2.0);
        Corrector c (*type);
        c.set ("response", 0.0f).set ("ref", 432.0f);
        report ("437 Hz with A4 = 432 Hz -> 432 Hz", measure (c.process (in), 0.3, 1.9, 432.0), 5.0);
    }

    // 5. Glide A4 -> E5 over 3 s in C major, fast response: should step A B C D E.
    {
        const auto glide = vowel ([] (double t) { return 440.0 * std::exp2 (juce::jlimit (0.0, 7.0, (t - 0.25) * 7.0 / 3.0) / 12.0); }, 3.5);
        Corrector c (*type);
        c.set ("response", 0.0f).set ("scale", (float) scaleIndex (*type, "Major"));
        const auto out = c.process (glide);

        auto onNotes = [] (const Signal& s, juce::StringArray& visited)
        {
            const char* names[] { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
            int on = 0, total = 0;
            for (float hz : frames (s, 0.1, 3.4))
            {
                if (hz <= 0.0f) continue;
                ++total;
                const double m = 69.0 + 12.0 * std::log2 (hz / 440.0);
                const int n = (int) std::lround (m);
                const bool scaleNote = std::abs (m - n) * 100.0 <= 10.0 && juce::String ("101011010101").substring (n % 12, n % 12 + 1) == "1";
                if (scaleNote)
                {
                    ++on;
                    const juce::String name (names[n % 12]);
                    if (visited.isEmpty() || visited[visited.size() - 1] != name) visited.add (name);
                }
            }
            return 100.0 * on / juce::jmax (1, total);
        };

        juce::StringArray inNotes, outNotes;
        const double inPct = onNotes (glide, inNotes), outPct = onNotes (out, outNotes);
        const bool ok = outPct >= 75.0 && outNotes.joinIntoString (" ") == "A B C D E";
        failures += ok ? 0 : 1;
        std::cout << juce::String ("glide A4->E5, C major, response 0").paddedRight (' ', 52)
                  << " frames on a scale note (+/-10c): in " << juce::roundToInt (inPct) << "%, out " << juce::roundToInt (outPct)
                  << "%  steps: " << outNotes.joinIntoString (" ") << "  " << (ok ? "PASS" : "FAIL") << "\n";

        // Clicks: the largest sample-to-sample jump, relative to the input's.
        auto maxStep = [] (const Signal& s) { float m = 0.0f; for (size_t i = 1; i < s.size(); ++i) m = juce::jmax (m, std::abs (s[i] - s[i - 1])); return m; };
        const float stepRatio = maxStep (out) / maxStep (glide);
        failures += stepRatio < 1.5f ? 0 : 1;
        std::cout << juce::String ("glide: largest sample step, out / in").paddedRight (' ', 52) << " " << juce::String (stepRatio, 2)
                  << "  " << (stepRatio < 1.5f ? "PASS" : "FAIL") << "\n";
        render (renderDir, "pc-glide-cmajor", glide, out);
    }

    // 6. Low and high voices, with the matching Range.
    {
        const auto low = vowel ([] (double) { return noteHz (43) * std::exp2 (30.0 / 1200.0); }, 2.0);
        Corrector c (*type);
        c.set ("response", 0.0f);
        report ("G2+30c (98 Hz), range Voice -> G2", measure (c.process (low), 0.3, 1.9, noteHz (43)), 5.0);

        const auto lower = vowel ([] (double) { return noteHz (36) * std::exp2 (-25.0 / 1200.0); }, 2.0);
        Corrector d (*type);
        d.set ("response", 0.0f).set ("range", 0.0f);
        report ("C2-25c (65 Hz), range Low -> C2", measure (d.process (lower), 0.3, 1.9, noteHz (36)), 5.0);

        const auto high = vowel ([] (double) { return noteHz (84) * std::exp2 (-40.0 / 1200.0); }, 2.0);
        Corrector e (*type);
        e.set ("response", 0.0f).set ("range", 2.0f);
        report ("C6-40c (1037 Hz), range High -> C6", measure (e.process (high), 0.3, 1.9, noteHz (84)), 5.0);
    }

    // 7. Formants stay put: A2+140c (a low voice, so its harmonics trace the vowel's
    //    envelope finely) pulled down 1.4 semitones to A2 in A minor pentatonic.
    //    Measure each output harmonic and compare the levels with two models: the same
    //    vocal tract at the new pitch (formants kept), and the input resampled (formants
    //    moved down 8%, the chipmunk/darth effect). The output should match the first.
    {
        const double inHz = noteHz (45) * std::exp2 (140.0 / 1200.0), outHz = noteHz (45), r = outHz / inHz;
        const auto in = vowel ([inHz] (double) { return inHz; }, 2.0);
        Corrector c (*type);
        c.set ("response", 0.0f).set ("key", 9.0f).set ("scale", (float) scaleIndex (*type, "Pent Minor"));
        const auto out = c.process (in);
        report ("A2+140c, A minor pentatonic -> A2 (-1.4 st)", measure (out, 0.3, 1.9, outHz), 5.0);

        // Calibrate the measure on the two ideal answers, synthesised directly.
        const double refKept = harmonicDistance (vowel ([outHz] (double) { return outHz; }, 2.0), outHz, 1.0);
        const double refMoved = harmonicDistance (vowel ([outHz] (double) { return outHz; }, 2.0, r), outHz, 1.0);
        std::cout << juce::String ("    (measure on ideal answers: kept / resampled)").paddedRight (' ', 52) << " "
                  << juce::String (refKept, 1) << " dB / " << juce::String (refMoved, 1) << " dB rms from formants-kept\n";

        const double kept = harmonicDistance (out, outHz, 1.0), moved = harmonicDistance (out, outHz, r);
        const bool ok = kept < moved && kept < 0.5 * refMoved;
        failures += ok ? 0 : 1;
        std::cout << juce::String ("    harmonic levels vs formants-kept / resampled").paddedRight (' ', 52) << " "
                  << juce::String (kept, 1) << " dB / " << juce::String (moved, 1) << " dB rms  " << (ok ? "PASS" : "FAIL") << "\n";
        render (renderDir, "pc-formant-down", in, out);
    }

    // 8. Stereo: channel 1 replays channel 0's decisions, so identical inputs come out identical.
    {
        auto pedal = type->create();
        for (size_t k = 0; k < type->info.knobs.size(); ++k)
            pedal->setKnob ((int) k, type->info.knobs[k].def);
        pedal->prepare (sampleRate, blockSize);
        pedal->update();
        Signal l (sharpVibrato), r (sharpVibrato);
        for (size_t pos = 0; pos + blockSize <= l.size(); pos += blockSize)
        {
            pedal->processChannel (0, l.data() + pos, blockSize);
            pedal->processChannel (1, r.data() + pos, blockSize);
        }
        float diff = 0.0f;
        for (size_t i = 0; i < l.size(); ++i) diff = juce::jmax (diff, std::abs (l[i] - r[i]));
        failures += diff < 1.0e-6f ? 0 : 1;
        std::cout << juce::String ("stereo: |left - right| for identical inputs").paddedRight (' ', 52) << " "
                  << juce::String (toDb (diff), 1) << " dB  " << (diff < 1.0e-6f ? "PASS" : "FAIL") << "\n";
    }

    // 9. Unvoiced: a sibilant between two sung notes passes as the plain delayed input.
    {
        const auto note1 = vowel ([] (double) { return 440.0 * std::exp2 (30.0 / 1200.0); }, 0.6);
        Signal in (note1);
        juce::Random rng (3);
        dsp::Svf hp;
        hp.setup (4000.0f, 0.7f, sampleRate);
        const size_t hissStart = in.size(), hissLen = (size_t) (0.4 * sampleRate);
        for (size_t i = 0; i < hissLen; ++i)
            in.push_back (0.05f * hp.process (rng.nextFloat() * 2.0f - 1.0f).high);
        in.insert (in.end(), note1.begin(), note1.end());

        Corrector c (*type);
        const auto out = c.process (in);
        const int lat = c.rig.pedal->getLatencySamples();
        double err = 0.0, ref = 0.0;
        for (size_t i = hissStart + 4800; i < hissStart + hissLen - 2400; ++i)
        {
            const double d = out[i + (size_t) lat] - in[i];
            err += d * d;
            ref += (double) in[i] * in[i];
        }
        const float db = (float) (10.0 * std::log10 (err / ref + 1.0e-20));
        failures += db < -60.0f ? 0 : 1;
        std::cout << juce::String ("sibilant between notes: output vs delayed input").paddedRight (' ', 52) << " "
                  << juce::String (db, 1) << " dB  " << (db < -60.0f ? "PASS" : "FAIL") << "\n";
        render (renderDir, "pc-sibilant", in, out);
    }

    // 10. Guitar DI renders for listening: chromatic and A minor pentatonic, hard and natural.
    if (renderDir != juce::File())
    {
        Corrector hard (*type);
        hard.set ("response", 0.0f);
        render (renderDir, "pc-guitar-chromatic-hard", guitar, hard.process (guitar));

        Corrector pent (*type);
        pent.set ("response", 0.0f).set ("key", 9.0f).set ("scale", (float) scaleIndex (*type, "Pent Minor"));
        render (renderDir, "pc-guitar-aminpent-hard", guitar, pent.process (guitar));

        Corrector natural (*type);
        natural.set ("response", 80.0f).set ("humanize", 0.6f);
        render (renderDir, "pc-guitar-chromatic-natural", guitar, natural.process (guitar));
        std::cout << "renders in " << renderDir.getFullPathName() << "\n";
    }

    std::cout << (failures == 0 ? "PASS" : "FAIL: " + juce::String (failures) + " check(s)") << "\n";
    return failures == 0 ? 0 : 1;
}
} // namespace pc

void printKnobs (const juce::StringArray& only)
{
    for (auto& t : Registry::get().all())
    {
        if (! only.isEmpty() && ! only.contains (t.info.type))
            continue;

        std::cout << t.info.type << "  " << t.info.name << "  [" << t.info.category << "]  " << t.info.subtitle
                  << " - " << t.info.description << "\n";

        for (auto& k : t.info.knobs)
        {
            std::cout << "    " << k.id.paddedRight (' ', 12) << k.label.paddedRight (' ', 14);
            if (k.isChoice())
                std::cout << "choice: " << k.choices.joinIntoString (" | ") << "  (default " << k.choices[(int) k.def] << ")\n";
            else
                std::cout << k.min << " .. " << k.max << (k.unit.isEmpty() ? "" : " " + k.unit)
                          << "  (default " << k.def << ")" << (k.unit == "%" ? "  [% knobs: 0..1]" : "") << "\n";
        }
    }
}

} // namespace

int runProjectTest();
int runScoreTest();
int renderScoreSamples (const juce::File&);
int runInstrumentTest (const juce::StringArray& args);
int writeMidiDemoProject (const juce::File&);
int runDrummerTest (const juce::StringArray&);
int writeDrummerDemoProject (const juce::File&);
int renderProject (const juce::File&, const juce::File&);
int writeDemoProject (const juce::File& folder, const juce::File& di);
int runSamplerTest (const juce::StringArray&);          // SamplerTest.cpp
int writeSamplerDemoProject (const juce::File&);

int main (int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add (argv[i]);

    const auto& registry = Registry::get();

    if (args.isEmpty() || args[0] == "list")
    {
        for (auto& t : registry.all())
            std::cout << t.info.category.paddedRight (' ', 12) << t.info.type.paddedRight (' ', 26)
                      << t.info.name.paddedRight (' ', 14) << t.info.subtitle << "\n";
        std::cout << registry.all().size() << " pedals\n";
        return 0;
    }

    if (args[0] == "tuner")
    {
        // Pitch detector accuracy: pure and harmonic-rich tones across the guitar range.
        PitchDetector detector;
        float worstCents = 0.0f;

        for (float hz : { 30.87f, 41.2f, 82.41f, 110.0f, 146.83f, 196.0f, 246.94f, 329.63f, 440.0f, 659.26f, 987.77f })
        {
            for (int harmonic = 0; harmonic < 2; ++harmonic)
            {
                Signal s ((size_t) PitchDetector::windowSize);
                for (size_t i = 0; i < s.size(); ++i)
                {
                    const float ph = juce::MathConstants<float>::twoPi * hz * (float) i / (float) sampleRate;
                    s[i] = harmonic == 0 ? 0.3f * std::sin (ph)
                                         : 0.1f * std::sin (ph) + 0.25f * std::sin (2 * ph) + 0.15f * std::sin (3 * ph) + 0.1f * std::sin (5 * ph);
                }

                const auto r = detector.analyse (s.data(), sampleRate);
                const float cents = r.frequency > 0 ? 1200.0f * std::log2 (r.frequency / hz) : 9999.0f;
                worstCents = juce::jmax (worstCents, std::abs (cents));
                std::cout << juce::String (hz, 2).paddedLeft (' ', 8) << (harmonic ? " rich " : " sine ")
                          << juce::String (r.frequency, 2).paddedLeft (' ', 9) << " Hz  "
                          << juce::String (cents, 2).paddedLeft (' ', 7) << " cents  clarity " << juce::String (r.clarity, 2) << "\n";
            }
        }

        std::cout << "worst error " << juce::String (worstCents, 2) << " cents\n";
        return worstCents < 1.0f ? 0 : 1;
    }

    if (args[0] == "project")
        return runProjectTest();

    if (args[0] == "score")
        return runScoreTest();

    if (args[0] == "score-render" && args.size() > 1)
        return renderScoreSamples (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));

    if (args[0] == "instruments")
    {
        args.remove (0);
        return runInstrumentTest (args);
    }

    if (args[0] == "render-project" && args.size() > 2)
        return renderProject (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]),
                              juce::File::getCurrentWorkingDirectory().getChildFile (args[2]));

    if (args[0] == "drummer")
    {
        args.remove (0);
        return runDrummerTest (args);
    }

    if (args[0] == "sampler")          return runSamplerTest (args);
    if (args[0] == "demo-sampler" && args.size() > 1) return writeSamplerDemoProject (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));

    if (args[0] == "demo-drummer" && args.size() > 1)
        return writeDrummerDemoProject (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));

    if (args[0] == "demo-midi" && args.size() > 1)
        return writeMidiDemoProject (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));

    if (args[0] == "demo" && args.size() > 1)
        return writeDemoProject (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]),
                                 juce::File::getCurrentWorkingDirectory().getChildFile ("tools/testdata/guitar-di-48k.wav"));

    if (args[0] == "knobs")
    {
        args.remove (0);
        printKnobs (args);
        return 0;
    }

    if (args[0] != "run" && args[0] != "presets" && args[0] != "pitchcorrect")
    {
        std::cerr << "usage: woodshed-bench list | knobs [type ...] | run [type ...] [--wav file] [--render dir]"
                     " | presets [file.json ...] [--wav file] [--render dir]"
                     " | instruments [type ...] [--render dir] [--analyse] | project | score | score-render dir | drummer"
                     " | pitchcorrect [--wav file] [--render dir]"
                     " | demo-midi <folder> | demo-drummer <folder> | render-project <folder> <out.wav>"
                     " | sampler | demo-sampler <folder>\n";
        return 2;
    }

    juce::File wavFile = juce::File::getCurrentWorkingDirectory().getChildFile ("tools/testdata/guitar-di-48k.wav");
    juce::File renderDir;
    juce::StringArray only;

    for (int i = 1; i < args.size(); ++i)
    {
        if (args[i] == "--wav" && i + 1 < args.size())         wavFile = juce::File::getCurrentWorkingDirectory().getChildFile (args[++i]);
        else if (args[i] == "--render" && i + 1 < args.size()) renderDir = juce::File::getCurrentWorkingDirectory().getChildFile (args[++i]);
        else                                                   only.add (args[i]);
    }

    if (renderDir != juce::File())
        renderDir.createDirectory();

    auto guitar = wavFile.existsAsFile() ? loadGuitar (wavFile) : Signal();
    if (guitar.empty())
    {
        std::cerr << "note: no guitar recording at " << wavFile.getFullPathName() << ", using synthetic plucks\n";
        guitar = syntheticGuitar();
    }

    if (args[0] == "pitchcorrect")
        return pc::run (guitar, renderDir);

    if (args[0] == "presets")
    {
        juce::Array<juce::File> files;
        for (auto& o : only)
            files.add (juce::File::getCurrentWorkingDirectory().getChildFile (o));

        if (files.isEmpty())
        {
            files = juce::File::getCurrentWorkingDirectory().getChildFile ("presets").findChildFiles (juce::File::findFiles, false, "*.json");
            files.sort();
        }

        return runPresets (files, guitar, renderDir);
    }

    std::cout << juce::String ("type").paddedRight (' ', 26) << "level  peak  silence  worst   cpu(us)  lat  result\n";
    int failures = 0, tested = 0;

    for (auto& t : registry.all())
    {
        if (! only.isEmpty() && ! only.contains (t.info.type))
            continue;

        const auto r = test (t, guitar, renderDir);
        ++tested;
        failures += r.problems.isEmpty() ? 0 : 1;

        std::cout << r.type.paddedRight (' ', 26)
                  << juce::String (r.levelDelta, 1).paddedLeft (' ', 5)
                  << juce::String (r.outPeakDb, 1).paddedLeft (' ', 6)
                  << juce::String (r.silenceDb, 0).paddedLeft (' ', 9)
                  << juce::String (r.worstExtremeDb, 1).paddedLeft (' ', 7)
                  << juce::String (r.cpu, 1).paddedLeft (' ', 10)
                  << juce::String (r.latency).paddedLeft (' ', 5) << "  "
                  << (r.problems.isEmpty() ? "PASS" : "FAIL: " + r.problems.joinIntoString ("; ")) << "\n";
    }

    std::cout << tested << " tested, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
