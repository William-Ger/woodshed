// woodshed-bench mastering: loudness metering against the EBU test cases (Tech 3341 loudness
// and true peak, Tech 3342 loudness range), the mastering processor's limiter, width and EQ,
// and the mastering assistant on a demo song, offline and through the engine.
// woodshed-bench demo-mastering <folder>: that demo song, mastered for streaming, to open in
// the studio's Mastering pane.

#include "../../Source/daw/Engine.h"
#include "../../Source/mastering/ProjectMastering.h"
#include <iostream>

using namespace woodshed;
using namespace woodshed::mastering;
using daw::ids::MASTER;

namespace
{
constexpr double rate = 48000.0;
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << "\n";
    failures += ok ? 0 : 1;
}

juce::String db (float v, int places = 2)          { return std::isfinite (v) ? juce::String (v, places) : juce::String ("-inf"); }

//==============================================================================
// Test signals.

/** Stereo sine sections: { amplitude dBFS, seconds } in a row, the same on both channels. */
juce::AudioBuffer<float> sineSections (double hz, std::initializer_list<std::pair<float, double>> sections, double phase = 0.0)
{
    int total = 0;
    for (auto& s : sections) total += juce::roundToInt (s.second * rate);
    juce::AudioBuffer<float> buf (2, total);
    int at = 0;
    for (auto& [level, seconds] : sections)
    {
        const float a = juce::Decibels::decibelsToGain (level);
        const int n = juce::roundToInt (seconds * rate);
        for (int i = 0; i < n; ++i, ++at)
        {
            const float v = a * (float) std::sin (juce::MathConstants<double>::twoPi * hz * at / rate + phase);
            buf.setSample (0, at, v);
            buf.setSample (1, at, v);
        }
    }
    return buf;
}

LoudnessMeter meterOf (const juce::AudioBuffer<float>& buf)
{
    LoudnessMeter m;
    m.prepare (rate);
    m.process (buf.getReadPointer (0), buf.getReadPointer (1), buf.getNumSamples());
    return m;
}

/** An independent true-peak reference, near-ideal reconstruction: 16x windowed-sinc
    interpolation with 256 taps either side (Kaiser, beta 10). */
float referenceTruePeakDb (const juce::AudioBuffer<float>& buf)
{
    constexpr int over = 16, half = 256;
    std::vector<std::array<float, 2 * half>> table (over);
    for (int p = 0; p < over; ++p)
        for (int j = 0; j < 2 * half; ++j)
        {
            const double t = (double) (j - half + 1) - (double) p / over;          // taps at n - half + 1 .. n + half
            const double sinc = std::abs (t) < 1.0e-12 ? 1.0 : std::sin (juce::MathConstants<double>::pi * t) / (juce::MathConstants<double>::pi * t);
            const double r = t / (half + 1);
            auto bessel0 = [] (double x) { double sum = 1.0, term = 1.0; for (int k = 1; k < 60; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; } return sum; };
            const double w = std::abs (r) < 1.0 ? bessel0 (10.0 * std::sqrt (1.0 - r * r)) / bessel0 (10.0) : 0.0;
            table[(size_t) p][(size_t) j] = (float) (sinc * w);
        }

    float most = 0.0f;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        const float* x = buf.getReadPointer (ch);
        const int n = buf.getNumSamples();
        for (int i = half; i < n - half; ++i)
        {
            // Only bother near loud samples.
            if (std::abs (x[i]) < most * 0.5f)
                continue;
            for (int p = 0; p < over; ++p)
            {
                float sum = 0.0f;
                const float* k = table[(size_t) p].data();
                for (int j = 0; j < 2 * half; ++j)
                    sum += k[j] * x[i - half + 1 + j];
                most = juce::jmax (most, std::abs (sum));
            }
        }
    }
    return juce::Decibels::gainToDecibels (most, -200.0f);
}

/** Loud, peaky programme: rumbling noise, a kick, bright hiss (rolled off toward Nyquist the way
    real recordings are) and full-scale inter-sample tones at fs/4. */
juce::AudioBuffer<float> peakyProgramme (double seconds, float hissLevel = 0.15f, float toneLevel = 0.7f)
{
    const int n = juce::roundToInt (seconds * rate);
    juce::AudioBuffer<float> buf (2, n);
    juce::Random rng (3341);
    float lowL = 0.0f, lowR = 0.0f, hissA = 0.0f, hissB = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / rate;
        lowL += 0.05f * ((rng.nextFloat() * 2.0f - 1.0f) - lowL);
        lowR += 0.05f * ((rng.nextFloat() * 2.0f - 1.0f) - lowR);
        const double beat = std::fmod (t, 0.5);
        const float kick = (float) (0.9 * std::exp (-beat * 12.0) * std::sin (juce::MathConstants<double>::twoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat));
        // fs/4 at 45 degrees: samples at 0.707 of the true peak.
        const float tone = std::fmod (t, 2.0) > 1.5 ? toneLevel * (float) std::sin (juce::MathConstants<double>::halfPi * i + juce::MathConstants<double>::pi / 4.0) : 0.0f;
        hissA += 0.6f * ((rng.nextFloat() * 2.0f - 1.0f) - hissA);
        hissB += 0.6f * (hissA - hissB);
        const float hiss = 2.0f * hissLevel * hissB;
        buf.setSample (0, i, 2.0f * lowL + kick + tone + hiss);
        buf.setSample (1, i, 2.0f * lowR + kick + tone - hiss);
    }
    return buf;
}

//==============================================================================
// The demo song: synthesized stems on audio tracks.

void writeMono (const juce::File& file, const std::vector<float>& data)
{
    juce::AudioBuffer<float> buf (1, (int) data.size());
    buf.copyFrom (0, 0, data.data(), (int) data.size());
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (32)
                                                   .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer != nullptr)
        writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

/** 16 bars at 120 bpm: drums, bass, a dark pad and a lead, mixed a little quiet and dull, the
    way a mix arrives before mastering. */
std::unique_ptr<daw::Project> makeDemoSong (const juce::File& folder)
{
    using namespace daw;
    folder.deleteRecursively();
    auto project = Project::createIn (folder, rate);
    project->state.setProperty (ids::name, "Mastering Demo", nullptr);
    project->state.setProperty (ids::tempo, 120.0, nullptr);
    project->state.setProperty (ids::click, false, nullptr);

    const double beat = 0.5;
    const int bars = 16;
    const int length = juce::roundToInt (bars * 4 * beat * rate);
    const double twoPi = juce::MathConstants<double>::twoPi;
    juce::Random rng (2024);

    std::vector<float> drums ((size_t) length), bass ((size_t) length), pad ((size_t) length), lead ((size_t) length);
    const double roots[] { 55.0, 43.65, 49.0, 41.2 };        // A, F, G, E

    for (int i = 0; i < length; ++i)
    {
        const double t = i / rate;
        const double inBeat = std::fmod (t, beat), inEighth = std::fmod (t, beat / 2.0);
        const int beatIndex = (int) (t / beat), bar = beatIndex / 4;

        // Kick on 1 and 3, snare on 2 and 4, hats on the eighths.
        double d = 0.0;
        if (beatIndex % 2 == 0)
            d += 0.8 * std::exp (-inBeat * 9.0) * std::sin (twoPi * (48.0 * inBeat + 60.0 * (1.0 - std::exp (-inBeat * 25.0)) / 25.0));
        else
            d += std::exp (-inBeat * 18.0) * (0.35 * (rng.nextDouble() * 2.0 - 1.0) + 0.3 * std::sin (twoPi * 185.0 * inBeat));
        d += 0.06 * std::exp (-inEighth * 60.0) * (rng.nextDouble() * 2.0 - 1.0);
        drums[(size_t) i] = (float) d;

        // Bass: eighth notes on the bar's root, a soft-edged saw.
        const double root = roots[bar % 4];
        const double sawPhase = std::fmod (root * t, 1.0);
        bass[(size_t) i] = (float) (0.3 * (1.0 - std::exp (-inEighth * 200.0)) * std::exp (-inEighth * 3.0) * (std::sin (twoPi * root * t) + 0.25 * (2.0 * sawPhase - 1.0)));

        // Pad: a minor triad two octaves up, dark (just the fundamentals and a second harmonic).
        double p = 0.0;
        for (double ratio : { 4.0, 4.0 * 1.1892, 4.0 * 1.4983 })
            p += std::sin (twoPi * root * ratio * t) + 0.2 * std::sin (twoPi * root * ratio * 2.0 * t + 0.3);
        pad[(size_t) i] = (float) (0.06 * p);

        // Lead in the second half: a square-ish line with vibrato.
        if (bar >= 8)
        {
            const int notes[] { 0, 3, 7, 10, 12, 10, 7, 3 };
            const double hz = 440.0 * std::pow (2.0, notes[beatIndex % 8] / 12.0) * (1.0 + 0.004 * std::sin (twoPi * 5.5 * t));
            const double s = std::sin (twoPi * hz * t);
            lead[(size_t) i] = (float) (0.09 * std::tanh (3.0 * s) * (1.0 - std::exp (-inBeat * 80.0)));
        }
    }

    struct Stem { const char* name; std::vector<float>* data; double pan, volume; };
    const Stem stems[] { { "Drums", &drums, 0.0, -3.0 }, { "Bass", &bass, 0.0, -2.0 },
                         { "Pad", &pad, -0.3, -4.0 }, { "Lead", &lead, 0.35, -3.0 } };
    for (auto& stem : stems)
    {
        const auto file = juce::String (stem.name) + ".wav";
        writeMono (project->audioFolder().getChildFile (file), *stem.data);
        auto t = project->addTrack (stem.name);
        t.setProperty (ids::rigOn, false, nullptr);
        t.setProperty (ids::pan, stem.pan, nullptr);
        t.setProperty (ids::volume, stem.volume, nullptr);
        project->addRegion (t, "Audio/" + file, 0, 0, length, stem.name);
    }
    project->state.setProperty (ids::selectedTrack, project->track (0)[ids::id], nullptr);
    project->undo.clearUndoHistory();
    return project;
}

/** The song without the Stereo Out's effects, offline (what the app's bounce renders). */
juce::AudioBuffer<float> renderMix (daw::Project& project, daw::AudioPool& pool)
{
    using namespace daw;
    constexpr int block = 512;
    std::map<int, std::shared_ptr<TrackRuntime>> runtimes;
    for (auto t : project.state)
    {
        if (! t.hasType (ids::TRACK)) continue;
        auto rt = std::make_shared<TrackRuntime>();
        rt->id = (int) t[ids::id];
        rt->work.setSize (2, block);
        rt->gainLeft.reset (rate, 0.03);
        rt->gainRight.reset (rate, 0.03);
        runtimes[rt->id] = rt;
    }
    auto snap = Engine::buildSnapshot (project, pool, runtimes);
    for (auto& t : snap->tracks)
    {
        t.runtime->gainLeft.setCurrentAndTargetValue (t.audible ? t.gain : 0.0f);
        t.runtime->gainRight.setCurrentAndTargetValue (t.audible ? t.gain : 0.0f);
    }
    const auto length = (int) project.contentEnd();
    juce::AudioBuffer<float> buf (2, length);
    buf.clear();
    for (int pos = 0; pos < length; pos += block)
    {
        Mixer::Block b;
        b.position = pos;
        b.numSamples = juce::jmin (block, length - pos);
        b.playing = true;
        b.left = buf.getWritePointer (0, pos);
        b.right = buf.getWritePointer (1, pos);
        Mixer::render (*snap, b);
    }
    buf.applyGain (snap->masterGain);
    return buf;
}

/** The song through the engine's live callback, Stereo Out effects and all. */
juce::AudioBuffer<float> playThroughEngine (daw::Project& project)
{
    using namespace daw;
    constexpr int block = 512;
    juce::AudioDeviceManager devices;
    Engine engine (devices, {});
    engine.prepare (rate, block);
    engine.setProject (&project);
    std::vector<float> l ((size_t) block), r ((size_t) block);
    for (int i = 0; i < 16; ++i)        // let the faders glide up before playing
    {
        float* outs[] { l.data(), r.data() };
        engine.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, block, {});
    }
    engine.setPosition (0);
    engine.play();
    const auto length = (int) project.contentEnd() + block * 4;
    juce::AudioBuffer<float> out (2, length);
    for (int pos = 0; pos + block <= length; pos += block)
    {
        float* outs[] { out.getWritePointer (0, pos), out.getWritePointer (1, pos) };
        engine.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, block, {});
    }
    engine.stop();
    engine.setProject (nullptr);
    return out;
}

float balanceDistance (const Curve& bands)
{
    double sum = 0.0;
    for (int b = 0; b < numBands; ++b)
        sum += juce::square ((double) (bands[(size_t) b] - referenceBalance()[(size_t) b]));
    return (float) std::sqrt (sum / numBands);
}
}

//==============================================================================
int runMasteringTest();
int runMasteringTest()
{
    juce::ScopedJuceInitialiser_GUI init;

    std::cout << "K-weighting\n";
    {
        KWeighting k;
        k.prepare (rate);
        // BS.1770-4 table 1 and 2 (48 kHz).
        const bool shelf = std::abs (k.shelf.b0 - 1.53512485958697) < 1.0e-6 && std::abs (k.shelf.b1 + 2.69169618940638) < 1.0e-6
                        && std::abs (k.shelf.b2 - 1.19839281085285) < 1.0e-6 && std::abs (k.shelf.a1 + 1.69065929318241) < 1.0e-6
                        && std::abs (k.shelf.a2 - 0.73248077421585) < 1.0e-6;
        const bool highPass = std::abs (k.highPass.a1 + 1.99004745483398) < 1.0e-6 && std::abs (k.highPass.a2 - 0.99007225036621) < 1.0e-6;
        check (shelf && highPass, "48 kHz coefficients match BS.1770-4");

        // A 10 kHz sine reads what the K-weighting curve says (the shelf, not just 1 kHz).
        const auto m = meterOf (sineSections (10000.0, { { -23.0f, 10.0 } }));
        const double w = juce::MathConstants<double>::twoPi * 10000.0 / rate;
        auto mag = [w] (double b0, double b1, double b2, double a1, double a2)
        {
            const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
            return std::abs ((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2));
        };
        const double h = mag (1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585)
                       * mag (1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621);
        const double expected = -0.691 + 10.0 * std::log10 (2.0 * 0.5 * juce::square (std::pow (10.0, -23.0 / 20.0) * h));
        check (std::abs (m.integrated() - expected) < 0.1, "10 kHz sine at -23 dBFS reads " + db (m.integrated()) + " LUFS (curve says "
                                                              + juce::String (expected, 2) + ")");
    }

    std::cout << "EBU Tech 3341 loudness (stereo 1 kHz sines; +/-0.1 LU)\n";
    {
        for (float level : { -23.0f, -33.0f })
        {
            const auto m = meterOf (sineSections (1000.0, { { level, 20.0 } }));
            check (std::abs (m.integrated() - level) < 0.1f && std::abs (m.momentary() - level) < 0.1f && std::abs (m.shortTerm() - level) < 0.1f,
                   juce::String (level, 0) + " dBFS: M " + db (m.momentary()) + ", S " + db (m.shortTerm()) + ", I " + db (m.integrated()) + " LUFS");
        }
        struct Case { const char* name; std::initializer_list<std::pair<float, double>> sections; };
        const Case cases[] {
            { "case 3: -36 / -23 / -36 dBFS", { { -36.0f, 10.0 }, { -23.0f, 60.0 }, { -36.0f, 10.0 } } },
            { "case 4: -72 / -36 / -23 / -36 / -72 dBFS", { { -72.0f, 10.0 }, { -36.0f, 10.0 }, { -23.0f, 60.0 }, { -36.0f, 10.0 }, { -72.0f, 10.0 } } },
            { "case 5: -26 / -20 / -26 dBFS", { { -26.0f, 20.0 }, { -20.0f, 20.1 }, { -26.0f, 20.0 } } },
        };
        for (auto& c : cases)
        {
            const auto m = meterOf (sineSections (1000.0, c.sections));
            check (std::abs (m.integrated() - -23.0f) < 0.1f, juce::String (c.name) + ": I " + db (m.integrated()) + " LUFS (gated to -23)");
        }
    }

    std::cout << "EBU Tech 3342 loudness range (+/-1 LU)\n";
    {
        struct Case { const char* name; std::initializer_list<std::pair<float, double>> sections; float lra; };
        const Case cases[] {
            { "case 1: -20 / -30 dBFS", { { -20.0f, 20.0 }, { -30.0f, 20.0 } }, 10.0f },
            { "case 2: -20 / -15 dBFS", { { -20.0f, 20.0 }, { -15.0f, 20.0 } }, 5.0f },
            { "case 3: -40 / -20 dBFS", { { -40.0f, 20.0 }, { -20.0f, 20.0 } }, 20.0f },
            { "case 4: -50 / -35 / -20 / -35 / -50 dBFS", { { -50.0f, 20.0 }, { -35.0f, 20.0 }, { -20.0f, 20.0 }, { -35.0f, 20.0 }, { -50.0f, 20.0 } }, 15.0f },
        };
        for (auto& c : cases)
        {
            const auto m = meterOf (sineSections (1000.0, c.sections));
            check (std::abs (m.loudnessRange() - c.lra) <= 1.0f, juce::String (c.name) + ": LRA " + db (m.loudnessRange(), 1) + " LU (expect "
                                                                 + juce::String (c.lra, 0) + ")");
        }
    }

    std::cout << "true peak (Tech 3341 cases 15-18: 12 kHz at -6 dBFS, +0.2/-0.4 dB)\n";
    {
        for (double degrees : { 0.0, 45.0, 60.0, 67.5 })
        {
            const auto m = meterOf (sineSections (12000.0, { { -6.0f, 1.0 } }, juce::degreesToRadians (degrees)));
            check (m.truePeakDb() <= -5.8f && m.truePeakDb() >= -6.4f,
                   "phase " + juce::String (degrees, 1) + ": true peak " + db (m.truePeakDb()) + " dBTP, sample peak " + db (m.samplePeakDb()) + " dBFS");
        }
        const auto full = meterOf (sineSections (12000.0, { { 0.0f, 1.0 } }, juce::MathConstants<double>::pi / 4.0));
        check (std::abs (full.truePeakDb()) <= 0.2f && std::abs (full.samplePeakDb() + 3.01f) < 0.05f,
               "full-scale fs/4 at 45 degrees: samples " + db (full.samplePeakDb()) + " dBFS, true peak " + db (full.truePeakDb()) + " dBTP");
        auto highTone = sineSections (19000.0, { { -6.0f, 1.0 } }, 0.3);
        for (int ch = 0; ch < 2; ++ch)          // faded in and out: a step would ring
        {
            highTone.applyGainRamp (ch, 0, 480, 0.0f, 1.0f);
            highTone.applyGainRamp (ch, highTone.getNumSamples() - 480, 480, 1.0f, 0.0f);
        }
        const auto high = meterOf (highTone);
        check (high.truePeakDb() <= -5.8f && high.truePeakDb() >= -6.8f, "19 kHz at -6 dBFS: true peak " + db (high.truePeakDb()) + " dBTP");
        const auto low = meterOf (sineSections (997.0, { { -1.0f, 1.0 } }));
        check (std::abs (low.truePeakDb() + 1.0f) < 0.05f, "997 Hz at -1 dBFS: true peak " + db (low.truePeakDb()) + " dBTP");
    }

    std::cout << "limiter\n";
    {
        const auto programme = peakyProgramme (12.0);
        std::cout << "    programme: true peak " << db (meterOf (programme).truePeakDb()) << " dBTP\n";
        for (int c = 0; c < 4; ++c)
            for (float ceiling : { -1.0f, -0.3f })
            {
                std::array<float, knobs::numKnobs> k {};
                k[knobs::character] = (float) c;
                k[knobs::gain] = 9.0f;
                k[knobs::eq] = 0.0f;
                k[knobs::compress] = 0.5f;
                k[knobs::width] = 1.0f;
                k[knobs::monoBass] = 20.0f;
                k[knobs::ceiling] = ceiling;
                const auto out = render (programme, rate, k, {});
                const auto m = meterOf (out);
                // The BS.1770 meter must never see an over. The near-ideal reference also
                // reconstructs the last kilohertz below Nyquist at full level (which a 4x meter's
                // interpolator, and a DAC's, rolls off), so it may read a little higher.
                const auto reference = referenceTruePeakDb (out);
                check (m.truePeakDb() <= ceiling && reference <= ceiling + 0.25f && m.samplePeakDb() <= ceiling,
                       characterNames()[c] + ", +9 dB into " + juce::String (ceiling, 1) + " dBTP: true peak " + db (m.truePeakDb(), 3)
                       + " (16x reference " + db (reference, 3) + "), sample peak " + db (m.samplePeakDb(), 3));
            }

        MasteringProcessor p;
        p.prepare (rate, 512);
        check (p.getLatencySamples() > 0 && p.getLatencySamples() < 128, "lookahead latency " + juce::String (p.getLatencySamples()) + " samples ("
                                                                       + juce::String (p.getLatencySamples() * 1000.0 / rate, 2) + " ms)");

        // Below the ceiling the processor (flat, no compression) is transparent: the input, delayed.
        std::array<float, knobs::numKnobs> k {};
        k[knobs::width] = 1.0f;
        k[knobs::monoBass] = 20.0f;
        k[knobs::ceiling] = -1.0f;
        const auto quiet = sineSections (440.0, { { -12.0f, 1.0 } });
        const auto out = render (quiet, rate, k, {});
        float worst = 0.0f;
        for (int i = 0; i < quiet.getNumSamples(); ++i)
            worst = juce::jmax (worst, std::abs (out.getSample (0, i) - quiet.getSample (0, i)));
        check (worst < 1.0e-4f, "below the ceiling it's bit-transparent (worst difference " + juce::String (worst, 7) + ")");
    }

    std::cout << "width and mono bass\n";
    {
        // 60 Hz side-only (L = -R) and 2 kHz side-only.
        const int n = (int) rate * 2;
        juce::AudioBuffer<float> side (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.2f * (float) (std::sin (juce::MathConstants<double>::twoPi * 60.0 * i / rate) + std::sin (juce::MathConstants<double>::twoPi * 2000.0 * i / rate));
            side.setSample (0, i, v);
            side.setSample (1, i, -v);
        }
        std::array<float, knobs::numKnobs> k {};
        k[knobs::width] = 1.0f;
        k[knobs::monoBass] = 150.0f;
        k[knobs::ceiling] = 0.0f;
        const auto out = render (side, rate, k, {});
        SpectrumAnalyser in (true), after (true);
        in.prepare (rate);
        after.prepare (rate);
        juce::AudioBuffer<float> inSide (2, n), outSide (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float s0 = 0.5f * (side.getSample (0, i) - side.getSample (1, i)), s1 = 0.5f * (out.getSample (0, i) - out.getSample (1, i));
            inSide.setSample (0, i, s0); inSide.setSample (1, i, s0);
            outSide.setSample (0, i, s1); outSide.setSample (1, i, s1);
        }
        in.process (inSide.getReadPointer (0), inSide.getReadPointer (1), n);
        after.process (outSide.getReadPointer (0), outSide.getReadPointer (1), n);
        const float lowDrop = in.bandLevel (50, 70) - after.bandLevel (50, 70);
        const float highDrop = in.bandLevel (1800, 2200) - after.bandLevel (1800, 2200);
        check (lowDrop > 20.0f && std::abs (highDrop) < 0.5f, "Mono Bass 150 Hz: the side at 60 Hz drops " + juce::String (lowDrop, 1)
                                                               + " dB, at 2 kHz " + juce::String (highDrop, 2) + " dB");
        k[knobs::width] = 0.0f;
        const auto mono = render (side, rate, k, {});
        check (mono.getMagnitude (0, 0, n) < 0.01f, "width 0: the side is gone (" + db (juce::Decibels::gainToDecibels (mono.getMagnitude (0, 0, n)), 1) + " dBFS)");
    }

    std::cout << "mastering the demo song\n";
    {
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("woodshed-mastering-test");
        auto project = makeDemoSong (folder);
        daw::AudioPool pool;
        const auto mix = renderMix (*project, pool);
        const auto before = measure (mix, rate);
        std::cout << "    mix: " << db (before.integrated) << " LUFS, LRA " << db (before.range, 1) << " LU, " << db (before.truePeak)
                  << " dBTP; balance off the reference by " << juce::String (balanceDistance (before.bands), 2) << " dB\n";

        Settings settings;
        settings.target = targetPresets()[0];
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto result = master (mix, rate, settings, &before);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        juce::StringArray curve;
        for (auto g : result.curve) curve.add (juce::String (g, 1));
        std::cout << "    gain " << juce::String (result.knobValues[knobs::gain], 2) << " dB, EQ curve [" << curve.joinIntoString (" ")
                  << "] dB, " << juce::String (ms / 1000.0, 2) << " s for " << juce::String (before.seconds, 1) << " s of song\n";
        check (result.ok && std::abs (result.after.integrated - -14.0f) <= 0.5f && result.after.truePeak <= -1.0f,
               "Streaming: " + db (result.after.integrated) + " LUFS, " + db (result.after.truePeak) + " dBTP, LRA " + db (result.after.range, 1) + " LU");
        check (balanceDistance (result.after.bands) < balanceDistance (before.bands),
               "the matching EQ moves the balance toward the reference (" + juce::String (balanceDistance (before.bands), 2) + " -> "
               + juce::String (balanceDistance (result.after.bands), 2) + " dB)");

        // Every target and character, on the same mix.
        for (auto& target : targetPresets())
            for (int c = 0; c < 4; ++c)
            {
                if (target.name == targetPresets()[0].name && c == 0)
                    continue;
                Settings s;
                s.target = target;
                s.character = (Character) c;
                const auto r = master (mix, rate, s, &before);
                check (r.ok && std::abs (r.after.integrated - target.lufs) <= 0.5f && r.after.truePeak <= target.truePeak,
                       target.name + ", " + characterNames()[c] + ": " + db (r.after.integrated) + " LUFS, " + db (r.after.truePeak) + " dBTP");
            }

        // On the project's Stereo Out, played by the engine: the FX node's knobs and curve reach it.
        project->undo.beginNewTransaction ("Mastering");
        project->state.setProperty (daw::ids::masterVolume, -3.0, nullptr);       // applyResult puts it back to 0 dB
        auto fx = applyResult (*project, result, settings.target);
        const auto played = playThroughEngine (*project);
        const auto live = meterOf (played);
        check (std::abs (live.integrated() - -14.0f) <= 0.5f && live.truePeakDb() <= -1.0f && referenceTruePeakDb (played) <= -0.75f,
               "through the engine: " + db (live.integrated()) + " LUFS, " + db (live.truePeakDb()) + " dBTP");
        check (findProcessor (*project) == fx && project->state.getChildWithName (MASTER).indexOf (fx) == project->state.getChildWithName (MASTER).getNumChildren() - 1
               && project->state.getChild (project->state.getNumChildren() - 1).hasType (MASTER), "the processor is the Stereo Out's last insert, MASTER the last child");

        fx.setProperty (daw::ids::on, false, nullptr);
        const auto bypassed = meterOf (playThroughEngine (*project));
        check (std::abs (bypassed.integrated() - before.integrated) < 0.2f, "bypassed (A/B): back to the mix, " + db (bypassed.integrated()) + " LUFS");

        // Reloading keeps it all.
        fx.setProperty (daw::ids::on, true, nullptr);
        project->save();
        auto reloaded = daw::Project::load (folder);
        const auto again = reloaded != nullptr ? meterOf (playThroughEngine (*reloaded)) : LoudnessMeter();
        check (reloaded != nullptr && std::abs (again.integrated() - live.integrated()) < 0.05f,
               "saved and reopened: " + db (again.integrated()) + " LUFS");
        const auto report = reloaded != nullptr ? juce::JSON::parse (findProcessor (*reloaded)[reportId].toString()) : juce::var();
        check (std::abs (measurementFrom (report["after"]).integrated - result.after.integrated) < 0.05f
               && measurementFrom (report["before"]).spectrum.size() == spectrumFrequencies().size(), "the report (before and after) is saved with it");

        project->undo.undo();
        check (! findProcessor (*project).isValid() && std::abs ((double) project->state[daw::ids::masterVolume] - -3.0) < 1.0e-6,
               "undo takes the processor off and the fader back");
        folder.deleteRecursively();
    }

    std::cout << (failures == 0 ? "all mastering checks passed\n" : juce::String (failures) + " mastering checks failed\n");
    return failures == 0 ? 0 : 1;
}

/** woodshed-bench demo-mastering <folder>: the demo song, mastered for streaming. */
int writeMasteringDemoProject (const juce::File& folder);
int writeMasteringDemoProject (const juce::File& folder)
{
    juce::ScopedJuceInitialiser_GUI init;
    auto project = makeDemoSong (folder);
    daw::AudioPool pool;
    const auto mix = renderMix (*project, pool);
    Settings settings;
    settings.target = targetPresets()[0];
    const auto result = master (mix, rate, settings);
    applyResult (*project, result, settings.target);
    project->undo.clearUndoHistory();
    if (! result.ok || ! project->save())
        return 1;
    std::cout << "wrote " << project->projectFile().getFullPathName() << " (" << db (result.before.integrated) << " -> "
              << db (result.after.integrated) << " LUFS, " << db (result.after.truePeak) << " dBTP)\n";
    return 0;
}
