// woodshed-bench flex: Time Stretch, Pitch Edit and Tempo Detect (Source/flex) — stretch timing,
// pitch accuracy, formants, tempo and note detection, saving, the engine and the CPU it takes.
// woodshed-bench demo-flex <folder>: a project with a stretched drum loop and a pitch-edited
// melody, to open in the studio or render with render-project.

#include "../../Source/flex/Edits.h"
#include "../../Source/flex/Renderer.h"
#include "../../Source/daw/Engine.h"
#include "../../Source/engine/PitchDetector.h"
#include <iostream>

using namespace woodshed;
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

double cpuScale()
{
    const auto scale = juce::SystemStats::getEnvironmentVariable ("WOODSHED_BENCH_CPU_SCALE", "1").getDoubleValue();
    return scale > 0.0 ? scale : 1.0;
}

using Buffer = std::shared_ptr<const juce::AudioBuffer<float>>;

Buffer makeBuffer (const std::vector<float>& v)
{
    auto b = std::make_shared<juce::AudioBuffer<float>> (1, (int) v.size());
    for (size_t i = 0; i < v.size(); ++i)
        b->setSample (0, (int) i, v[i]);
    return b;
}

/** A drum hit: a kick (pitched-down sine thump), a snare (noise and tone) or a hat (bright noise). */
void addHit (std::vector<float>& v, double at, int kind, float level, juce::Random& random)
{
    const int start = (int) std::llround (at);
    const int length = (int) (rate * (kind == 0 ? 0.25 : kind == 1 ? 0.15 : 0.05));
    double phase = 0.0;
    float lp = 0.0f, prevNoise = 0.0f;
    for (int i = 0; i < length && start + i < (int) v.size(); ++i)
    {
        if (start + i < 0) continue;
        const float t = (float) i / (float) rate;
        float s = 0.0f;
        if (kind == 0)
        {
            const double f = 50.0 + 120.0 * std::exp (-t * 30.0);
            phase += juce::MathConstants<double>::twoPi * f / rate;
            s = (float) std::sin (phase) * std::exp (-t * 14.0f);
        }
        else
        {
            const float noise = random.nextFloat() * 2.0f - 1.0f;
            if (kind == 1)
            {
                lp += 0.3f * (noise - lp);
                phase += juce::MathConstants<double>::twoPi * 190.0 / rate;
                s = (0.7f * lp + 0.4f * (float) std::sin (phase)) * std::exp (-t * 25.0f);
            }
            else
            {
                s = (noise - prevNoise) * 0.5f * std::exp (-t * 80.0f);
                prevNoise = noise;
            }
        }
        v[(size_t) (start + i)] += level * s;
    }
}

/** A bar-accented click track or a drum loop at `bpm`, starting with a downbeat at `firstBeat`. */
std::vector<float> beatLoop (double bpm, int bars, bool drums, double firstBeat = 0.0, double endBpm = 0.0)
{
    juce::Random random (1234);
    std::vector<double> beats;
    double t = firstBeat;
    const int count = bars * 4;
    for (int k = 0; k < count; ++k)
    {
        beats.push_back (t);
        const double now = endBpm > 0.0 ? bpm + (endBpm - bpm) * k / (double) count : bpm;
        t += rate * 60.0 / now;
    }
    std::vector<float> v ((size_t) (t + rate * 0.5), 0.0f);
    for (int k = 0; k < count; ++k)
    {
        const double at = beats[(size_t) k];
        if (! drums)
        {
            // A click: a short burst of a high tone, louder and lower on the downbeat.
            const bool down = k % 4 == 0;
            const double f = down ? 1500.0 : 1000.0;
            for (int i = 0; i < (int) (rate * 0.015); ++i)
                v[(size_t) at + (size_t) i] += (down ? 0.8f : 0.45f) * (float) std::sin (juce::MathConstants<double>::twoPi * f * i / rate)
                                               * std::exp (-(float) i / (float) (rate * 0.004));
            if (down)
                addHit (v, at, 0, 0.3f, random);
            continue;
        }
        if (k % 2 == 0) addHit (v, at, 0, k % 4 == 0 ? 0.9f : 0.7f, random);
        else            addHit (v, at, 1, 0.6f, random);
        addHit (v, at, 2, 0.25f, random);
        addHit (v, at + (beats.size() > (size_t) k + 1 ? (beats[(size_t) k + 1] - at) * 0.5 : rate * 30.0 / bpm), 2, 0.2f, random);
    }
    return v;
}

/** Hits (alternating kick and snare) at the given positions, with a held organ chord under them. */
std::vector<float> hitsWithPad (const std::vector<double>& hits, double seconds)
{
    juce::Random random (99);
    std::vector<float> v ((size_t) (seconds * rate), 0.0f);
    for (size_t i = 0; i < v.size(); ++i)
    {
        const double t = (double) i / rate;
        const float fade = (float) juce::jmin (1.0, juce::jmin (t, seconds - t) / 0.05);
        v[i] = fade * 0.04f * (float) (std::sin (juce::MathConstants<double>::twoPi * 220.0 * t) + std::sin (juce::MathConstants<double>::twoPi * 277.18 * t)
                                + std::sin (juce::MathConstants<double>::twoPi * 329.63 * t));
    }
    for (size_t k = 0; k < hits.size(); ++k)
        addHit (v, hits[k], (int) (k % 2), 0.8f, random);
    return v;
}

std::vector<float> sine (double hz, double seconds, float level = 0.4f)
{
    std::vector<float> v ((size_t) (seconds * rate));
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = level * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (double) i / rate);
    return v;
}

/** The pitch (Hz) of the middle `window` samples around `centre`. */
double pitchOf (const float* data, int numSamples, int centre)
{
    PitchDetector detector;
    std::vector<float> w ((size_t) PitchDetector::windowSize, 0.0f);
    for (int i = 0; i < PitchDetector::windowSize; ++i)
        if (const int k = centre - PitchDetector::windowSize / 2 + i; k >= 0 && k < numSamples)
            w[(size_t) i] = data[k];
    return detector.analyse (w.data(), rate).frequency;
}

double cents (double hz, double target)        { return 1200.0 * std::log2 (hz / target); }

/** The worst distance (ms) from each expected transient to the nearest found one, and the
    number expected that had none within 20 ms. */
std::pair<double, int> transientError (const std::vector<double>& found, const std::vector<double>& expected)
{
    double worst = 0.0;
    int missing = 0;
    for (auto e : expected)
    {
        double best = 1.0e9;
        for (auto f : found)
            best = juce::jmin (best, std::abs (f - e));
        if (best > 0.02 * rate) ++missing;
        else worst = juce::jmax (worst, best);
    }
    return { worst * 1000.0 / rate, missing };
}

/** Where the attack nearest `at` (within 25 ms) starts: the first moment the level of the
    signal's slope reaches half its peak there. Measures where a hit really landed. */
double attackNear (const float* x, int n, double at)
{
    constexpr int window = 48;
    const int from = juce::jmax (window + 1, (int) (at - 0.025 * rate)), to = juce::jmin (n, (int) (at + 0.025 * rate));
    if (to - from < window * 2)
        return -1.0e9;
    std::vector<float> level ((size_t) (to - from));
    for (int k = from; k < to; ++k)
    {
        double e = 0.0;
        for (int j = k - window; j < k; ++j) { const double d = x[j] - 0.95 * x[j - 1]; e += d * d; }
        level[(size_t) (k - from)] = (float) std::sqrt (e / window);
    }
    const float peak = *std::max_element (level.begin(), level.end());
    const float quiet = *std::min_element (level.begin(), level.begin() + (long) ((at - 0.005 * rate - from) > 0 ? (at - 0.005 * rate - from) : 1));
    for (size_t k = 0; k < level.size(); ++k)
        if (level[k] >= quiet + 0.5f * (peak - quiet))
            return from + (double) k - window / 4.0;
    return -1.0e9;
}

/** The worst distance (ms) between where each hit should start and where its attack is. */
double attackError (const juce::AudioBuffer<float>& audio, const std::vector<double>& expected)
{
    double worst = 0.0;
    for (auto e : expected)
        if (e >= 0.03 * rate)
            worst = juce::jmax (worst, std::abs (attackNear (audio.getReadPointer (0), audio.getNumSamples(), e) - e));
    return worst * 1000.0 / rate;
}

double attackError (const std::vector<float>& audio, const std::vector<double>& expected)
{
    double worst = 0.0;
    for (auto e : expected)
    {
        if (e < 0.03 * rate)
            continue;       // too close to the start to see where it begins
        worst = juce::jmax (worst, std::abs (attackNear (audio.data(), (int) audio.size(), e) - e));
        if (std::getenv ("FLEX_DEBUG") != nullptr)
            std::cout << "      at " << e / rate * 1000.0 << " ms: " << (attackNear (audio.data(), (int) audio.size(), e) - e) / rate * 1000.0 << " ms\n";
    }
    return worst * 1000.0 / rate;
}

flex::RenderRequest request (const Buffer& audio, flex::Mode mode, flex::WarpMap warp)
{
    flex::RenderRequest rq;
    rq.source = audio;
    rq.sampleRate = rate;
    rq.mode = mode;
    rq.warp = std::move (warp);
    rq.transients = std::make_shared<const std::vector<double>> (flex::detectTransients (audio->getReadPointer (0), audio->getNumSamples(), rate));
    return rq;
}

double rms (const float* x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += (double) x[i] * x[i];
    return n > 0 ? std::sqrt (s / n) : 0.0;
}

/** The first formant (Hz): the peak of the spectrum, smoothed over 300 Hz so the harmonics
    blur into their envelope, between 300 and 1100 Hz. Averaged over a few windows. */
double firstFormant (const float* data, int numSamples, int centre)
{
    constexpr int order = 13, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> spectrum ((size_t) size / 2, 0.0);
    for (int shift = -2; shift <= 2; ++shift)
    {
        std::vector<float> w ((size_t) size * 2, 0.0f);
        for (int i = 0; i < size; ++i)
            if (const int k = centre + shift * 2400 - size / 2 + i; k >= 0 && k < numSamples)
                w[(size_t) i] = data[k] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / size));
        fft.performFrequencyOnlyForwardTransform (w.data(), true);
        for (int k = 0; k < size / 2; ++k)
            spectrum[(size_t) k] += w[(size_t) k] * w[(size_t) k];
    }
    const double binHz = rate / size;
    const int radius = (int) (150.0 / binHz);
    double best = 0.0, bestHz = 0.0;
    for (int k = (int) (300.0 / binHz); k < (int) (1100.0 / binHz); ++k)
    {
        double s = 0.0;
        for (int j = k - radius; j <= k + radius; ++j)
            s += spectrum[(size_t) j];
        if (s > best) { best = s; bestHz = k * binHz; }
    }
    return bestHz;
}

/** A vowel: a 140 Hz pulse train through resonances at 700 and 1200 Hz ("ah"). */
std::vector<float> vowel (double f0, double seconds)
{
    std::vector<float> v ((size_t) (seconds * rate), 0.0f);
    juce::dsp::IIR::Filter<float> f1, f2;
    f1.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (rate, 700.0, 6.0);
    f2.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (rate, 1200.0, 7.0);
    double phase = 0.0;
    for (size_t i = 0; i < v.size(); ++i)
    {
        phase += f0 / rate;
        float pulse = 0.0f;
        if (phase >= 1.0) { phase -= 1.0; pulse = 1.0f; }
        v[i] = 1.5f * (f1.processSample (pulse) + 0.6f * f2.processSample (pulse));
    }
    return v;
}

/** A sung-like melody: each note with a scoop into it, vibrato, harmonics and a breath between. */
struct Sung { std::vector<float> audio; std::vector<flex::PitchNote> notes; };

Sung melody()
{
    const std::vector<int> pitches { 60, 62, 64, 67, 64, 60 };
    const double noteSeconds = 0.45, gapSeconds = 0.06;
    Sung s;
    s.audio.assign ((size_t) ((noteSeconds + gapSeconds) * (double) pitches.size() * rate + rate * 0.3), 0.0f);
    juce::Random random (7);
    double phase = 0.0;
    int at = (int) (0.1 * rate);
    for (auto midi : pitches)
    {
        const int length = (int) (noteSeconds * rate);
        for (int i = 0; i < length; ++i)
        {
            const double t = i / rate;
            const double scoop = -0.6 * std::exp (-t / 0.03);
            const double vibrato = 0.3 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * t) * juce::jmin (1.0, t / 0.15);
            const double hz = 440.0 * std::pow (2.0, (midi + scoop + vibrato - 69.0) / 12.0);
            phase += hz / rate;
            const double env = juce::jmin (1.0, t / 0.02) * juce::jmin (1.0, (noteSeconds - t) / 0.03);
            const double p = juce::MathConstants<double>::twoPi * phase;
            s.audio[(size_t) (at + i)] = (float) (env * 0.3 * (std::sin (p) + 0.5 * std::sin (2 * p) + 0.25 * std::sin (3 * p) + 0.1 * std::sin (4 * p)))
                                       + 0.002f * (random.nextFloat() - 0.5f);
        }
        flex::PitchNote n;
        n.start = at;
        n.end = at + length;
        n.pitch = (float) midi;
        s.notes.push_back (n);
        at += length + (int) (gapSeconds * rate);
    }
    return s;
}

void writeWav (const juce::File& file, const std::vector<float>& v)
{
    juce::AudioBuffer<float> buf (1, (int) v.size());
    for (size_t i = 0; i < v.size(); ++i) buf.setSample (0, (int) i, v[i]);
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
    writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

std::vector<float> offline (Project& project, AudioPool& pool, juce::int64 length)
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
    std::vector<float> left ((size_t) length), right ((size_t) length);
    for (juce::int64 pos = 0; pos < length; pos += 64)
    {
        Mixer::Block b;
        b.position = pos;
        b.numSamples = (int) juce::jmin ((juce::int64) 64, length - pos);
        b.playing = true;
        b.left = left.data() + pos;
        b.right = right.data() + pos;
        Mixer::render (*snap, b);
    }
    return left;
}

//==============================================================================
void testStretch()
{
    std::cout << "time stretch\n";
    // Hits every 0.4 s (and one off the beat) over a held chord: 4 s.
    std::vector<double> hits;
    for (int k = 0; k < 9; ++k) hits.push_back ((0.2 + 0.4 * k) * rate);
    hits.push_back (2.55 * rate);
    std::sort (hits.begin(), hits.end());
    const auto source = makeBuffer (hitsWithPad (hits, 4.0));
    const int n = source->getNumSamples();

    const auto foundInSource = flex::detectTransients (source->getReadPointer (0), n, rate);
    const auto [sourceErr, sourceMissing] = transientError (foundInSource, hits);
    check (sourceMissing == 0 && sourceErr < 5.0, "transient detection finds every hit within 5 ms (worst " + juce::String (sourceErr, 2)
                                                  + " ms, " + juce::String ((int) foundInSource.size()) + " found for " + juce::String ((int) hits.size()) + ")");

    // Where each hit's attack is in the source; stretched, it should be exactly there times the ratio.
    std::vector<double> attacks;
    for (auto h : hits) attacks.push_back (attackNear (source->getReadPointer (0), n, h));

    for (auto mode : { flex::Mode::monophonic, flex::Mode::polyphonic, flex::Mode::rhythmic, flex::Mode::speed })
    {
        for (double ratio : { 0.8, 1.25, 1.5 })
        {
            flex::WarpMap w;
            w.markers = { { 0.0, 0.0 }, { (double) n, n * ratio } };
            const auto out = flex::render (request (source, mode, w));
            const auto expectedLength = (int) std::ceil (n * ratio);
            std::vector<double> expected;
            for (auto a : attacks) expected.push_back (a * ratio);
            const double err = attackError (*out, expected);
            const double level = juce::Decibels::gainToDecibels (rms (out->getReadPointer (0), out->getNumSamples()) / rms (source->getReadPointer (0), n));
            check (std::abs (out->getNumSamples() - expectedLength) <= 1 && err < 5.0 && std::abs (level) < (mode == flex::Mode::rhythmic ? 4.0 : 2.0),
                   flex::modeName (mode).paddedRight (' ', 19) + " x" + juce::String (ratio, 2) + ": length " + juce::String (out->getNumSamples())
                   + " (want " + juce::String (expectedLength) + "), every hit within " + juce::String (err, 2) + " ms, level "
                   + juce::String (level, 1) + " dB");
        }
    }

    // Markers that stretch one part and squeeze another: each hit lands where its marker says.
    {
        flex::WarpMap w;
        w.markers = { { 0.0, 0.0 }, { hits[3], hits[3] * 1.3 }, { hits[7], hits[3] * 1.3 + (hits[7] - hits[3]) * 0.8 }, { (double) n, 0.0 } };
        w.markers.back().dst = w.markers[2].dst + (n - hits[7]);
        const auto out = flex::render (request (source, flex::Mode::polyphonic, w));
        std::vector<double> expected;
        for (auto a : attacks) expected.push_back (w.toDst (a));
        const double err = attackError (*out, expected);
        check (err < 5.0, "markers: x1.3 then x0.8 then as recorded, every hit within 5 ms (worst " + juce::String (err, 2) + " ms)");

        // Away from the markers' pieces, as recorded means bit for bit.
        const int from = (int) std::ceil (w.markers[2].dst) + 2000, srcFrom = (int) std::llround (w.toSrc (from));
        float worst = 0.0f;
        for (int i = 0; i < 20000 && from + i < out->getNumSamples() && srcFrom + i < n; ++i)
            worst = juce::jmax (worst, std::abs (out->getSample (0, from + i) - source->getSample (0, srcFrom + i)));
        check (worst < 1.0e-6f, "the part after the last marker plays as recorded (max difference " + juce::String (worst, 8) + ")");
    }

    // The guitar DI: a real recording stretched both ways stays clean and as loud.
    const auto diFile = juce::File::getCurrentWorkingDirectory().getChildFile ("tools/testdata/guitar-di-48k.wav");
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    if (std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (diFile)); reader != nullptr)
    {
        auto di = std::make_shared<juce::AudioBuffer<float>> (1, (int) reader->lengthInSamples);
        reader->read (di.get(), 0, di->getNumSamples(), 0, true, false);
        const auto diTransients = flex::detectTransients (di->getReadPointer (0), di->getNumSamples(), rate);
        check (diTransients.size() >= 3, "guitar DI: " + juce::String ((int) diTransients.size()) + " transients (picked notes) found");
        for (double ratio : { 0.85, 1.2 })
        {
            flex::WarpMap w;
            w.markers = { { 0.0, 0.0 }, { (double) di->getNumSamples(), di->getNumSamples() * ratio } };
            const auto out = flex::render (request (di, flex::Mode::polyphonic, w));
            bool finite = true;
            float peak = 0.0f;
            for (int i = 0; i < out->getNumSamples(); ++i)
            {
                finite = finite && std::isfinite (out->getSample (0, i));
                peak = juce::jmax (peak, std::abs (out->getSample (0, i)));
            }
            const double level = juce::Decibels::gainToDecibels (rms (out->getReadPointer (0), out->getNumSamples()) / rms (di->getReadPointer (0), di->getNumSamples()));
            // Legato notes have soft attacks: most (not all) must be found right where they belong.
            const auto found = flex::detectTransients (out->getReadPointer (0), out->getNumSamples(), rate);
            int close = 0, near = 0;
            for (auto t : diTransients)
            {
                double best = 1.0e9;
                for (auto f : found) best = juce::jmin (best, std::abs (f - t * ratio));
                close += best < 0.005 * rate ? 1 : 0;
                near += best < 0.02 * rate ? 1 : 0;
            }
            const int count = (int) diTransients.size();
            check (finite && peak < 1.5f * di->getMagnitude (0, 0, di->getNumSamples()) + 0.05f && std::abs (level) < 2.0
                       && close * 2 >= count && near * 4 >= count * 3,
                   "guitar DI x" + juce::String (ratio, 2) + ": level " + juce::String (level, 2) + " dB, of " + juce::String (count)
                   + " picked notes " + juce::String (close) + " start within 5 ms and " + juce::String (near) + " within 20 ms of where they should");
        }
    }
    else
    {
        check (false, "tools/testdata/guitar-di-48k.wav readable (run from the repository root)");
    }
}

void testPitch()
{
    std::cout << "pitch edit\n";
    for (double hz : { 220.0, 330.0, 523.25 })
    {
        const auto source = makeBuffer (sine (hz, 2.0));
        flex::PitchNote note;
        note.start = 0;
        note.end = source->getNumSamples();
        note.pitch = (float) (69.0 + 12.0 * std::log2 (hz / 440.0));
        note.shift = 3.0f;
        auto rq = request (source, flex::Mode::monophonic, {});
        rq.notes = { note };
        rq.pitch = std::make_shared<const flex::PitchTrack> (flex::trackPitch (source->getReadPointer (0), source->getNumSamples(), rate));
        const auto out = flex::render (rq);
        const double target = hz * std::pow (2.0, 3.0 / 12.0);
        double worst = 0.0;
        for (double at : { 0.6, 1.0, 1.4 })
            worst = juce::jmax (worst, std::abs (cents (pitchOf (out->getReadPointer (0), out->getNumSamples(), (int) (at * rate)), target)));
        const double level = juce::Decibels::gainToDecibels (rms (out->getReadPointer (0) + 24000, 48000) / rms (source->getReadPointer (0) + 24000, 48000));
        check (worst < 5.0 && out->getNumSamples() == source->getNumSamples() && std::abs (level) < 2.0,
               "sine " + juce::String (hz, 0) + " Hz +3 semitones: " + juce::String (worst, 2) + " cents off at worst, level " + juce::String (level, 2) + " dB");
    }

    // Formants: a vowel up five semitones keeps its resonances where they were; plain speed-up moves them.
    {
        const auto source = makeBuffer (vowel (140.0, 2.0));
        flex::PitchNote note;
        note.start = 0;
        note.end = source->getNumSamples();
        note.pitch = (float) (69.0 + 12.0 * std::log2 (140.0 / 440.0));
        note.shift = 5.0f;
        auto rq = request (source, flex::Mode::monophonic, {});
        rq.notes = { note };
        rq.pitch = std::make_shared<const flex::PitchTrack> (flex::trackPitch (source->getReadPointer (0), source->getNumSamples(), rate));
        const auto kept = flex::render (rq);
        flex::WarpMap faster;
        const double speed = std::pow (2.0, 5.0 / 12.0);
        faster.markers = { { 0.0, 0.0 }, { (double) source->getNumSamples(), source->getNumSamples() / speed } };
        const auto tape = flex::renderVarispeed (*source, faster);

        const double original = firstFormant (source->getReadPointer (0), source->getNumSamples(), 48000);
        const double preserved = firstFormant (kept->getReadPointer (0), kept->getNumSamples(), 48000);
        const double moved = firstFormant (tape->getReadPointer (0), tape->getNumSamples(), 30000);
        const double f0 = pitchOf (kept->getReadPointer (0), kept->getNumSamples(), 48000);
        check (std::abs (preserved / original - 1.0) < 0.1 && moved / original > 1.2 && std::abs (cents (f0, 140.0 * speed)) < 10.0,
               "formants: vowel +5 semitones, first formant " + juce::String (original, 0) + " -> " + juce::String (preserved, 0)
               + " Hz (tape speed-up: " + juce::String (moved, 0) + " Hz), pitch " + juce::String (cents (f0, 140.0 * speed), 1) + " cents off");

        auto shifted = rq;
        shifted.notes[0].formant = 4.0f;
        const auto up = flex::render (shifted);
        const double raised = firstFormant (up->getReadPointer (0), up->getNumSamples(), 48000);
        check (raised / preserved > 1.15, "formant shift +4 semitones raises the first formant (" + juce::String (preserved, 0) + " -> " + juce::String (raised, 0) + " Hz)");
    }

    // Note detection on a sung-like melody, then editing one note.
    {
        const auto sung = melody();
        const auto source = makeBuffer (sung.audio);
        const auto track = flex::trackPitch (source->getReadPointer (0), source->getNumSamples(), rate);
        const auto notes = flex::detectNotes (track, rate);
        bool match = notes.size() == sung.notes.size();
        double worstPitch = 0.0, worstEdge = 0.0;
        for (size_t i = 0; match && i < notes.size(); ++i)
        {
            worstPitch = juce::jmax (worstPitch, (double) std::abs (notes[i].pitch - sung.notes[i].pitch));
            worstEdge = juce::jmax (worstEdge, std::abs (notes[i].start - sung.notes[i].start) * 1000.0 / rate,
                                    std::abs (notes[i].end - sung.notes[i].end) * 1000.0 / rate);
        }
        check (match && worstPitch < 0.3 && worstEdge < 50.0,
               "melody: " + juce::String ((int) notes.size()) + " notes found (sung " + juce::String ((int) sung.notes.size()) + "), pitches within "
               + juce::String (worstPitch, 2) + " semitones, edges within " + juce::String (worstEdge, 0) + " ms");

        if (match)
        {
            auto edited = notes;
            edited[2].shift = 2.0f;     // E4 -> F#4
            edited[2].vibrato = 0.0f;   // and held straight
            edited[2].drift = 0.0f;
            auto rq = request (source, flex::Mode::monophonic, {});
            rq.notes = edited;
            rq.pitch = std::make_shared<const flex::PitchTrack> (track);
            const auto out = flex::render (rq);
            const int mid = (int) (0.5 * (edited[2].start + edited[2].end) + 0.08 * rate);
            const double target = 440.0 * std::pow (2.0, (edited[2].pitch + 2.0 - 69.0) / 12.0);
            const double got = pitchOf (out->getReadPointer (0), out->getNumSamples(), mid);
            const double untouched = pitchOf (out->getReadPointer (0), out->getNumSamples(), (int) (0.5 * (edited[0].start + edited[0].end)));
            const double before = pitchOf (source->getReadPointer (0), source->getNumSamples(), (int) (0.5 * (edited[0].start + edited[0].end)));
            check (std::abs (cents (got, target)) < 15.0 && std::abs (cents (untouched, before)) < 5.0,
                   "edited note +2 semitones, vibrato off: " + juce::String (cents (got, target), 1) + " cents off; the first note unchanged ("
                   + juce::String (cents (untouched, before), 1) + " cents)");

            std::vector<flex::PitchNote> scaled = notes;
            for (auto& n : scaled) n.shift = 0.4f;
            flex::quantizeToScale (scaled, 0, flex::Scale::major);
            bool onScale = true;
            for (auto& n : scaled)
            {
                const float result = n.pitch + n.shift;
                onScale = onScale && std::abs (result - std::round (result)) < 0.001f;
                const int degree = ((int) std::round (result) % 12 + 12) % 12;
                onScale = onScale && degree != 1 && degree != 3 && degree != 6 && degree != 8 && degree != 10;
            }
            check (onScale, "quantize to C major puts every note on a scale step");
        }
    }
}

void testTempo()
{
    std::cout << "tempo detect\n";
    for (bool drums : { false, true })
        for (double bpm : { 90.0, 120.0, 140.0 })
        {
            const double first = 0.37 * rate;
            const auto loop = beatLoop (bpm, 8, drums, first);
            const auto t = flex::detectTempo (loop.data(), (int) loop.size(), rate);
            if (std::getenv ("FLEX_DEBUG") != nullptr)
                for (size_t k = 0; k < juce::jmin ((size_t) 9, t.beats.size()); ++k)
                    std::cout << "      beat " << (t.beats[k] - first) / rate * 1000.0 << " ms" << ((int) k == t.firstDownbeat ? " (downbeat)" : "") << "\n";
            const double barLength = rate * 240.0 / bpm;
            const double phase = juce::isPositiveAndBelow (t.firstDownbeat, (int) t.beats.size())
                               ? std::fmod (t.beats[(size_t) t.firstDownbeat] - first + barLength * 1000.0, barLength) : barLength * 0.5;
            const bool downbeat = juce::jmin (phase, barLength - phase) < 0.01 * rate;
            check (std::abs (t.bpm - bpm) < 1.0 && downbeat && ! t.variable,
                   juce::String (drums ? "drum loop " : "click     ") + juce::String (bpm, 0) + " bpm: found " + juce::String (t.bpm, 2)
                   + " bpm, downbeat " + (downbeat ? "on the bar" : "missed") + ", confidence " + juce::String (t.confidence, 2));
        }

    // A drummer without a click, 110 speeding up to 130: a tempo for every bar.
    {
        const auto loop = beatLoop (110.0, 8, true, 0.2 * rate, 130.0);
        const auto t = flex::detectTempo (loop.data(), (int) loop.size(), rate);
        const bool rising = t.barTempos.size() >= 6 && t.barTempos.back() > t.barTempos.front() + 10.0;
        check (t.variable && rising, "rising tempo 110 -> 130: variable, bars from " + juce::String (t.barTempos.empty() ? 0.0 : t.barTempos.front(), 1)
                                     + " to " + juce::String (t.barTempos.empty() ? 0.0 : t.barTempos.back(), 1) + " bpm");
    }
}

void testProject()
{
    std::cout << "project\n";
    juce::ScopedJuceInitialiser_GUI init;
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("woodshed-flex-test");
    folder.deleteRecursively();
    auto project = Project::createIn (folder, rate);
    project->audioFolder().createDirectory();

    // A 90 bpm drum loop on a 120 bpm song.
    const auto loop = beatLoop (90.0, 4, true, 0.0);
    writeWav (project->audioFolder().getChildFile ("loop90.wav"), loop);
    auto track = project->addTrack ("Drums");
    auto region = project->addRegion (track, "Audio/loop90.wav", 0, 0, (juce::int64) loop.size(), "Loop");

    AudioPool pool;
    flex::Renderer renderer;
    pool.regionAudio = [&renderer] (const juce::ValueTree& r, const juce::File& f, const AudioPool::Audio& a, bool)
    {
        return renderer.audioFor (r, f, a, rate, true);
    };

    // Tempo Detect, conforming: the loop's beats land on the song's.
    const auto audio = pool.get (project->resolve ("Audio/loop90.wav"));
    const auto estimate = flex::detectTempo (audio->getReadPointer (0), audio->getNumSamples(), rate);
    project->undo.beginNewTransaction ("Conform");
    flex::setTrackMode (*project, track, flex::Mode::rhythmic);
    flex::conformToTempo (*project, region, estimate);
    {
        const auto length = (juce::int64) region[ids::start] + (juce::int64) region[ids::length];
        const auto mix = offline (*project, pool, length);
        std::vector<double> beats;
        for (int k = 0; k < 16; ++k) beats.push_back ((double) project->grid().fromBeats (k));
        const double err = attackError (mix, beats);
        check (err < 5.0 && std::abs ((double) length - 16.0 * rate * 0.5) < rate * 0.6,
               "conform 90 -> 120 bpm (rhythmic): every beat on the song's beat within " + juce::String (err, 2) + " ms, region "
               + juce::String ((double) length / rate, 2) + " s");
    }

    // The song's tempo changes; the region follows it.
    project->undo.beginNewTransaction ("Tempo");
    project->setTempoChange (0.0, 100.0);
    {
        const auto length = (juce::int64) region[ids::start] + (juce::int64) region[ids::length];
        const auto mix = offline (*project, pool, length);
        std::vector<double> beats;
        for (int k = 0; k < 16; ++k) beats.push_back ((double) project->grid().fromBeats (k));
        const double err = attackError (mix, beats);
        check (err < 5.0, "song to 100 bpm: the region follows, every beat within " + juce::String (err, 2) + " ms");
    }

    // Undo puts it back as it was at 120.
    project->undo.undo();
    check (std::abs ((double) project->state[ids::tempo] - 120.0) < 1.0e-9 && (bool) region[flex::ids::followTempo], "undo the tempo change: 120 bpm again, still following");

    // A flex marker moved by hand: the hit under it moves there.
    {
        const auto loopFile = project->audioFolder().getChildFile ("hits.wav");
        std::vector<double> hits { 0.25 * rate, 0.75 * rate, 1.25 * rate, 1.75 * rate };
        const auto loopAudio = hitsWithPad (hits, 2.0);
        writeWav (loopFile, loopAudio);
        auto t2 = project->addTrack ("Hits");
        auto r2 = project->addRegion (t2, "Audio/hits.wav", (juce::int64) (4.0 * rate), 0, (juce::int64) (2.0 * rate), "Hits");
        flex::setTrackMode (*project, t2, flex::Mode::polyphonic);
        const int index = flex::addMarker (*project, r2, hits[2]);
        flex::moveMarker (*project, r2, index, hits[2] + 0.1 * rate);
        // Mute the drums so only these hits are heard.
        track.setProperty (ids::mute, true, nullptr);
        const auto mix = offline (*project, pool, (juce::int64) (6.0 * rate));
        std::vector<double> expected;
        for (auto h : hits)
            expected.push_back (4.0 * rate + flex::warpOf (r2).toDst (attackNear (loopAudio.data(), (int) loopAudio.size(), h)));
        const double err = attackError (mix, expected);
        check (err < 5.0 && flex::warpOf (r2).markers.size() == 3,
               "drag a marker 100 ms later: that hit plays 100 ms later, the region's ends stay (worst " + juce::String (err, 2) + " ms)");

        // Quantize: a hit 30 ms late moves onto its 16th.
        auto r3 = project->addRegion (t2, "Audio/hits.wav", (juce::int64) (8.0 * rate + 0.03 * rate), 0, (juce::int64) (2.0 * rate), "Late");
        const auto& grid = project->grid();
        const auto transients = renderer.analysis().transients (project->resolve ("Audio/hits.wav").getFullPathName(), pool.get (project->resolve ("Audio/hits.wav")), rate);
        flex::quantizeAudio (*project, r3, *transients, ticksPerQuarter / 4.0);
        t2.setProperty (ids::mute, false, nullptr);
        r2.setProperty (ids::mute, true, nullptr);
        const auto qmix = offline (*project, pool, (juce::int64) (10.5 * rate));
        std::vector<double> want;
        for (auto h : hits)
        {
            const double late = 8.0 * rate + 0.03 * rate + h;
            const double tick = std::round (grid.tickAt (late) / (ticksPerQuarter / 4.0)) * (ticksPerQuarter / 4.0);
            want.push_back (grid.sampleAt (tick));
        }
        const double qerr = attackError (qmix, want);
        check (qerr < 5.0, "quantize audio: hits 30 ms late land on the 16ths within " + juce::String (qerr, 2) + " ms");
    }

    // Pitch edits and every setting survive saving.
    {
        auto notes = std::vector<flex::PitchNote> { { 100.0, 20000.0, 60.0f, 2.5f, 0.5f, 0.0f, -3.0f, 1.5f } };
        flex::setNotes (region, notes, nullptr);
        region.setProperty (flex::ids::sourceTempo, estimate.bpm, nullptr);
        const auto markers = region[flex::ids::flexMarkers].toString();
        check (project->save(), "project saves");
        auto loaded = Project::load (folder);
        const auto lt = loaded != nullptr ? loaded->track (0) : juce::ValueTree();
        const auto lr = lt.getChild (0);
        const auto ln = flex::notesOf (lr);
        const bool same = lt.isValid() && (bool) lt[flex::ids::flexOn] && flex::modeOf (lt) == flex::Mode::rhythmic
                       && lr[flex::ids::flexMarkers].toString() == markers && (bool) lr[flex::ids::followTempo]
                       && ln.size() == 1 && std::abs (ln[0].shift - 2.5f) < 1.0e-3f && std::abs (ln[0].drift - 0.5f) < 1.0e-3f
                       && std::abs (ln[0].gain + 3.0f) < 1.0e-3f && std::abs (ln[0].formant - 1.5f) < 1.0e-3f
                       && std::abs ((double) lr[flex::ids::sourceTempo] - estimate.bpm) < 0.01;
        check (same, "round trip: Time Stretch on, rhythmic, " + juce::String ((int) flex::warpOf (lr).markers.size())
                     + " markers, follow tempo and the pitch edits all come back");
        if (loaded != nullptr)
        {
            AudioPool pool2;
            flex::Renderer renderer2;
            pool2.regionAudio = [&renderer2] (const juce::ValueTree& r, const juce::File& f, const AudioPool::Audio& a, bool)
            {
                return renderer2.audioFor (r, f, a, rate, true);
            };
            flex::setNotes (region, {}, nullptr);
            flex::setNotes (loaded->track (0).getChild (0), {}, nullptr);
            loaded->track (0).setProperty (ids::mute, false, nullptr);
            track.setProperty (ids::mute, false, nullptr);
            const auto a = offline (*project, pool, (juce::int64) (4.0 * rate));
            loaded->track (1).setProperty (ids::mute, true, nullptr);
            for (int i = 0; i < project->numTracks(); ++i)
                project->track (i).setProperty (ids::mute, i != 0, nullptr);
            for (int i = 0; i < loaded->numTracks(); ++i)
                loaded->track (i).setProperty (ids::mute, i != 0, nullptr);
            const auto b1 = offline (*project, pool, (juce::int64) (4.0 * rate));
            const auto b2 = offline (*loaded, pool2, (juce::int64) (4.0 * rate));
            float worst = 0.0f;
            for (size_t i = 0; i < b1.size(); ++i) worst = juce::jmax (worst, std::abs (b1[i] - b2[i]));
            check (worst < 1.0e-6f && rms (b1.data(), (int) b1.size()) > 0.01, "the loaded project plays the same stretched audio (max difference " + juce::String (worst, 8) + ")");
        }
    }

    // Tempo Detect, adopting: an empty song takes the loop's tempo, and its downbeat sits on a bar.
    {
        auto p2 = Project::createIn (folder.getChildFile ("adopt"), rate);
        p2->audioFolder().createDirectory();
        const auto l140 = beatLoop (140.0, 4, true, 0.21 * rate);
        writeWav (p2->audioFolder().getChildFile ("loop140.wav"), l140);
        auto t = p2->addTrack ("Loop");
        auto r = p2->addRegion (t, "Audio/loop140.wav", (juce::int64) (2.0 * rate), 0, (juce::int64) l140.size(), "Loop");
        const auto est = flex::detectTempo (l140.data(), (int) l140.size(), rate);
        p2->undo.beginNewTransaction ("Adopt");
        flex::adoptTempo (*p2, r, est);
        const double downbeat = (double) (juce::int64) r[ids::start] + 0.21 * rate;
        const auto& g = p2->grid();
        const double bar = g.barAt (g.tickAt (downbeat));
        check (std::abs ((double) p2->state[ids::tempo] - 140.0) < 1.0 && std::abs (bar - std::round (bar)) * g.samplesPerBar() < 0.005 * rate,
               "adopt: the song is now " + juce::String ((double) p2->state[ids::tempo], 2) + " bpm and the loop's downbeat is on bar "
               + juce::String (std::round (bar) + 1, 0) + " (off by " + juce::String (std::abs (bar - std::round (bar)) * g.samplesPerBar() * 1000.0 / rate, 2) + " ms)");
    }

    // The live engine: the renderer plays a stand-in, then the proper render when it's done.
    {
        flex::Renderer live;
        auto r = project->track (0).getChild (0);
        const auto file = project->resolve (r[ids::file].toString());
        live.clear();
        auto first = live.audioFor (r, file, pool.get (file), rate, false);
        live.waitForAll();
        auto second = live.audioFor (r, file, pool.get (file), rate, false);
        check (first != nullptr && second != nullptr && first != second && live.pending() == 0
               && std::abs (first->getNumSamples() - second->getNumSamples()) <= 1,
               "live: a varispeed stand-in at once (" + juce::String (first != nullptr ? first->getNumSamples() : 0) + " samples), the render after");
    }

    folder.deleteRecursively();
}

void testCpu()
{
    std::cout << "cpu\n";
    std::vector<double> hits;
    for (int k = 0; k < 40; ++k) hits.push_back ((0.1 + 0.25 * k) * rate);
    const auto source = makeBuffer (hitsWithPad (hits, 10.0));
    const double seconds = source->getNumSamples() / rate;

    struct Case { const char* name; flex::Mode mode; double ratio; bool pitch; double budget; };
    for (auto c : { Case { "polyphonic x1.2", flex::Mode::polyphonic, 1.2, false, 8.0 },
                    Case { "monophonic x0.9", flex::Mode::monophonic, 0.9, false, 8.0 },
                    Case { "pitch edit +3", flex::Mode::monophonic, 1.0, true, 4.0 },
                    Case { "speed x1.3", flex::Mode::speed, 1.3, false, 50.0 } })
    {
        flex::WarpMap w;
        w.markers = { { 0.0, 0.0 }, { (double) source->getNumSamples(), source->getNumSamples() * c.ratio } };
        auto rq = request (source, c.mode, w);
        if (c.pitch)
        {
            flex::PitchNote n;
            n.end = source->getNumSamples();
            n.shift = 3.0f;
            rq.notes = { n };
            rq.pitch = std::make_shared<const flex::PitchTrack>();
        }
        const auto started = juce::Time::getMillisecondCounterHiRes();
        flex::render (rq);
        const double took = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
        const double speed = seconds / juce::jmax (1.0e-6, took);
        check (speed * cpuScale() >= c.budget, juce::String (c.name).paddedRight (' ', 17) + juce::String (speed, 1) + "x real time (budget "
                                               + juce::String (c.budget, 0) + "x)");
    }

    const auto started = juce::Time::getMillisecondCounterHiRes();
    flex::detectTransients (source->getReadPointer (0), source->getNumSamples(), rate);
    flex::detectTempo (source->getReadPointer (0), source->getNumSamples(), rate);
    flex::trackPitch (source->getReadPointer (0), source->getNumSamples(), rate);
    const double speed = seconds / juce::jmax (1.0e-6, (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0);
    check (speed * cpuScale() >= 10.0, "analysis (transients, tempo, pitch) " + juce::String (speed, 1) + "x real time (budget 10x)");
}
}

//==============================================================================
int runFlexTest();
int runFlexTest()
{
    failures = 0;
    testStretch();
    testPitch();
    testTempo();
    testProject();
    testCpu();
    std::cout << (failures == 0 ? "flex: all passed\n" : "flex: " + std::to_string (failures) + " FAILED\n");
    return failures == 0 ? 0 : 1;
}

/** A project to look at: a 90 bpm drum loop conformed to 120, and a pitch-edited melody. */
int writeFlexDemoProject (const juce::File& folder);
int writeFlexDemoProject (const juce::File& folder)
{
    juce::ScopedJuceInitialiser_GUI init;
    folder.deleteRecursively();
    auto project = Project::createIn (folder, rate);
    project->audioFolder().createDirectory();

    const auto loop = beatLoop (90.0, 4, true, 0.0);
    writeWav (project->audioFolder().getChildFile ("Drum Loop 90.wav"), loop);
    auto drums = project->addTrack ("Drums");
    auto loopRegion = project->addRegion (drums, "Audio/Drum Loop 90.wav", 0, 0, (juce::int64) loop.size(), "Drum Loop");
    flex::setTrackMode (*project, drums, flex::Mode::rhythmic);
    flex::conformToTempo (*project, loopRegion, flex::detectTempo (loop.data(), (int) loop.size(), rate));

    const auto sung = melody();
    writeWav (project->audioFolder().getChildFile ("Melody.wav"), sung.audio);
    auto voice = project->addTrack ("Voice");
    auto melodyRegion = project->addRegion (voice, "Audio/Melody.wav", project->grid().barStartSample (1), 0, (juce::int64) sung.audio.size(), "Melody");
    flex::setTrackMode (*project, voice, flex::Mode::monophonic);
    auto notes = flex::detectNotes (flex::trackPitch (sung.audio.data(), (int) sung.audio.size(), rate), rate);
    if (notes.size() > 3)
    {
        notes[1].shift = -1.0f;
        notes[3].vibrato = 0.0f;
        notes[3].shift = 2.0f;
    }
    flex::setNotes (melodyRegion, notes, nullptr);
    project->state.setProperty (ids::selectedTrack, voice[ids::id], nullptr);

    const bool ok = project->save();
    std::cout << (ok ? "wrote " : "couldn't write ") << folder.getFullPathName() << " (region " << (int) melodyRegion[ids::id]
              << " is the melody: WOODSHED_TEST_REGION=" << (int) melodyRegion[ids::id] << " WOODSHED_TEST_PANE=pitchEdit)\n";
    return ok ? 0 : 1;
}
