// woodshed-bench: offline checks for every pedal in the registry.
//
//   woodshed-bench list
//   woodshed-bench knobs [type ...]
//   woodshed-bench run [type ...] [--wav guitar.wav] [--render outDir]
//   woodshed-bench presets [file.json ...] [--wav guitar.wav] [--render outDir]
//   woodshed-bench project      (multitrack engine checks; see ProjectTest.cpp)
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
    if (res.cpu > cpuBudgetUs) fail ("CPU " + juce::String (res.cpu, 1) + " us/block (budget " + juce::String (cpuBudgetUs, 0) + ")");

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
            if (cpu > presetCpuBudgetUs)                               problems.add ("CPU " + juce::String (cpu, 0) + " us/block");

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
int writeDemoProject (const juce::File& folder, const juce::File& di);

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

    if (args[0] == "demo" && args.size() > 1)
        return writeDemoProject (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]),
                                 juce::File::getCurrentWorkingDirectory().getChildFile ("tools/testdata/guitar-di-48k.wav"));

    if (args[0] == "knobs")
    {
        args.remove (0);
        printKnobs (args);
        return 0;
    }

    if (args[0] != "run" && args[0] != "presets")
    {
        std::cerr << "usage: woodshed-bench list | knobs [type ...] | run [type ...] [--wav file] [--render dir]"
                     " | presets [file.json ...] [--wav file] [--render dir]\n";
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
