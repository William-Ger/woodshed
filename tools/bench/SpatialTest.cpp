// woodshed-bench spatial: checks the 3D panner (binaural cues, stereo collapse, automation),
// the surround bounce (speaker energies, the multichannel WAV header) and the movie sync logic.
// woodshed-bench demo-spatial <folder>: a project with a source circling the listener, to open
// in the studio or render with render-project.

#include "../../Source/daw/Engine.h"
#include "../../Source/spatial/Spatial.h"
#include "../../Source/video/MovieSync.h"
#include <juce_dsp/juce_dsp.h>
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

juce::String db (double v, int places = 1)      { return juce::String (v, places) + " dB"; }

struct Stereo { std::vector<float> left, right; };

/** The panner's binaural (or stereo) output for `input` on both channels, at a fixed position. */
Stereo pan (const std::vector<float>& input, spatial::Position where, spatial::Monitoring mode = spatial::Monitoring::binaural)
{
    spatial::Panner p;
    p.prepare (rate);
    Stereo s { input, input };
    for (size_t at = 0; at < input.size(); at += 256)
    {
        const int n = (int) juce::jmin ((size_t) 256, input.size() - at);
        p.setTarget (where);
        p.render (s.left.data() + at, s.right.data() + at, n, mode);
    }
    return s;
}

std::vector<float> impulse (int length, int at = 200)
{
    std::vector<float> v ((size_t) length, 0.0f);
    v[(size_t) at] = 1.0f;
    return v;
}

std::vector<float> noise (int length, juce::int64 seed = 7)
{
    juce::Random r (seed);
    std::vector<float> v ((size_t) length);
    for (auto& x : v)
        x = r.nextFloat() * 2.0f - 1.0f;
    return v;
}

double energy (const float* x, int n)
{
    double e = 0.0;
    for (int i = 0; i < n; ++i)
        e += (double) x[i] * x[i];
    return e;
}

double energyDb (const std::vector<float>& x)    { return 10.0 * std::log10 (energy (x.data(), (int) x.size()) + 1.0e-30); }

/** The lag (samples) at which `b` best matches `a`: positive when `b` comes later. */
int lagOf (const std::vector<float>& a, const std::vector<float>& b, int maxLag)
{
    int best = 0;
    double bestValue = -1.0e30;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double sum = 0.0;
        for (int i = maxLag; i < (int) a.size() - maxLag; ++i)
            sum += (double) a[(size_t) i] * b[(size_t) (i + lag)];
        if (sum > bestValue)
        {
            bestValue = sum;
            best = lag;
        }
    }
    return best;
}

/** The first sample above a tenth of the signal's peak. */
int onset (const std::vector<float>& x)
{
    float peak = 0.0f;
    for (auto v : x) peak = juce::jmax (peak, std::abs (v));
    for (size_t i = 0; i < x.size(); ++i)
        if (std::abs (x[i]) > peak * 0.1f)
            return (int) i;
    return -1;
}

/** Average power spectrum (Hann-windowed 4096-point frames, half overlapped). */
std::vector<double> spectrum (const std::vector<float>& x)
{
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> power ((size_t) size / 2, 0.0);
    std::vector<float> frame ((size_t) size * 2);
    int frames = 0;
    for (size_t start = 0; start + size <= x.size(); start += size / 2)
    {
        std::fill (frame.begin(), frame.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            frame[(size_t) i] = x[start + (size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size));
        fft.performFrequencyOnlyForwardTransform (frame.data());
        for (int k = 0; k < size / 2; ++k)
            power[(size_t) k] += (double) frame[(size_t) k] * frame[(size_t) k];
        ++frames;
    }
    for (auto& p : power) p /= juce::jmax (1, frames);
    return power;
}

double bandDb (const std::vector<double>& power, double fromHz, double toHz)
{
    const double binHz = rate / (2.0 * (double) power.size());
    double sum = 1.0e-30;
    for (size_t k = (size_t) (fromHz / binHz); k < power.size() && (double) k * binHz < toHz; ++k)
        sum += power[k];
    return 10.0 * std::log10 (sum);
}

/** The magnitude response (dB) of an impulse response, zero-padded to 16384 points. */
std::vector<double> response (const std::vector<float>& ir)
{
    constexpr int order = 14, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> frame ((size_t) size * 2, 0.0f);
    for (size_t i = 0; i < ir.size() && i < (size_t) size; ++i)
        frame[i] = ir[i];
    fft.performFrequencyOnlyForwardTransform (frame.data());
    std::vector<double> out ((size_t) size / 2);
    for (int k = 0; k < size / 2; ++k)
        out[(size_t) k] = 20.0 * std::log10 ((double) frame[(size_t) k] + 1.0e-12);
    return out;
}

double frequencyOfMinimum (const std::vector<double>& responseDb, double fromHz, double toHz)
{
    const double binHz = rate / (2.0 * (double) responseDb.size());
    size_t best = (size_t) (fromHz / binHz);
    for (size_t k = best; k < responseDb.size() && (double) k * binHz < toHz; ++k)
        if (responseDb[k] < responseDb[best])
            best = k;
    return (double) best * binHz;
}

void writeWav (const juce::File& file, const std::vector<float>& samples)
{
    juce::AudioBuffer<float> buf (1, (int) samples.size());
    buf.copyFrom (0, 0, samples.data(), (int) samples.size());
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (32)
                                                   .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

/** Renders a project offline through the engine's mixer, with prepared panners. */
Stereo renderProject (Project& project, AudioPool& pool, juce::int64 length, int block = 64)
{
    std::map<int, std::shared_ptr<TrackRuntime>> runtimes;
    for (auto t : project.state)
    {
        if (! t.hasType (ids::TRACK)) continue;
        auto rt = std::make_shared<TrackRuntime>();
        rt->id = (int) t[ids::id];
        rt->work.setSize (2, block);
        rt->busInput.setSize (2, block);
        rt->gainLeft.reset (rate, 0.03);
        rt->gainRight.reset (rate, 0.03);
        rt->panner = std::make_unique<spatial::Panner>();
        rt->panner->prepare (rate);
        runtimes[rt->id] = rt;
    }
    auto snap = Engine::buildSnapshot (project, pool, runtimes);
    for (auto& t : snap->tracks)
    {
        t.runtime->gainLeft.setCurrentAndTargetValue (t.gain);
        t.runtime->gainRight.setCurrentAndTargetValue (t.gain);
    }
    Stereo r { std::vector<float> ((size_t) length), std::vector<float> ((size_t) length) };
    for (juce::int64 pos = 0; pos < length; pos += block)
    {
        Mixer::Block b;
        b.position = pos;
        b.numSamples = (int) juce::jmin ((juce::int64) block, length - pos);
        b.playing = true;
        b.left = r.left.data() + pos;
        b.right = r.right.data() + pos;
        Mixer::render (*snap, b);
    }
    return r;
}

double maxSecondDifference (const std::vector<float>& x, size_t from, size_t to)
{
    double worst = 0.0;
    for (size_t i = juce::jmax ((size_t) 2, from); i < to && i < x.size(); ++i)
        worst = juce::jmax (worst, (double) std::abs (x[i] - 2.0f * x[i - 1] + x[i - 2]));
    return worst;
}

double maxStep (const std::vector<float>& x, size_t from, size_t to)
{
    double worst = 0.0;
    for (size_t i = juce::jmax ((size_t) 1, from); i < to && i < x.size(); ++i)
        worst = juce::jmax (worst, (double) std::abs (x[i] - x[i - 1]));
    return worst;
}

/** The worst of `measure` over 10 ms windows, each relative to that window's RMS. */
double worstRelative (const std::vector<float>& x, size_t from, size_t to, double (*measure) (const std::vector<float>&, size_t, size_t))
{
    constexpr size_t window = 480;
    double worst = 0.0;
    for (size_t at = from; at + window <= to; at += window)
    {
        const double rms = std::sqrt (energy (x.data() + at, (int) window) / (double) window);
        if (rms > 1.0e-6)
            worst = juce::jmax (worst, measure (x, at, at + window) / rms);
    }
    return worst;
}

/** A bounce on a fresh engine (the bench runs no message loop, so an engine bounces once). */
bool bounce (Project& project, const juce::File& file, juce::int64 length, const BounceOptions& options)
{
    juce::AudioDeviceManager devices;
    Engine engine (devices, {});
    engine.prepare (rate, 512);
    engine.setProject (&project);
    engine.startBounce (file, 0, length, options, {});
    for (int i = 0; i < 4000 && engine.getBounceProgress() < 1.0f; ++i)
        juce::Thread::sleep (5);
    juce::Thread::sleep (100);
    engine.setProject (nullptr);
    return file.existsAsFile();
}

bool readFile (const juce::File& f, juce::AudioBuffer<float>& buf, juce::AudioChannelSet* layout = nullptr)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    if (reader == nullptr)
        return false;
    if (layout != nullptr)
        *layout = reader->getChannelLayout();
    buf.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
    return reader->read (&buf, 0, buf.getNumSamples(), 0, true, true);
}

/** Each channel's share of the total energy. */
std::vector<double> shares (const juce::AudioBuffer<float>& buf)
{
    std::vector<double> e ((size_t) buf.getNumChannels());
    double total = 1.0e-30;
    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        total += (e[(size_t) ch] = energy (buf.getReadPointer (ch), buf.getNumSamples()));
    for (auto& v : e) v /= total;
    return e;
}

juce::String describeShares (const std::vector<double>& s, const spatial::SpeakerLayout& layout)
{
    juce::String out;
    for (size_t i = 0; i < s.size() && i < (size_t) layout.size(); ++i)
        if (s[i] > 0.005)
            out << layout.speakers[i].label << " " << juce::roundToInt (s[i] * 100.0) << "% ";
    return out.trim();
}

/** The WAV's fmt chunk: format tag, channel count and (extensible) channel mask. */
struct WavHeader { int format = 0, channels = 0; juce::uint32 mask = 0; bool ok = false; };
WavHeader readHeader (const juce::File& f)
{
    WavHeader h;
    juce::MemoryBlock data;
    if (! f.loadFileAsData (data) || data.getSize() < 12)
        return h;
    auto* bytes = static_cast<const juce::uint8*> (data.getData());
    if (std::memcmp (bytes, "RIFF", 4) != 0 || std::memcmp (bytes + 8, "WAVE", 4) != 0)
        return h;
    for (size_t at = 12; at + 8 <= data.getSize();)
    {
        const auto size = juce::ByteOrder::littleEndianInt (bytes + at + 4);
        if (std::memcmp (bytes + at, "fmt ", 4) == 0 && at + 8 + 24 <= data.getSize())
        {
            const auto* fmt = bytes + at + 8;
            h.format = juce::ByteOrder::littleEndianShort (fmt);
            h.channels = juce::ByteOrder::littleEndianShort (fmt + 2);
            if (h.format == 0xfffe && size >= 40)
                h.mask = juce::ByteOrder::littleEndianInt (fmt + 20);
            h.ok = true;
            return h;
        }
        at += 8 + size + (size & 1);
    }
    return h;
}

/** A project with one track playing `samples`, set up by `setup`. */
std::unique_ptr<Project> oneTrack (const juce::File& folder, const std::vector<float>& samples,
                                   std::function<void (juce::ValueTree)> setup)
{
    folder.deleteRecursively();
    auto project = Project::createIn (folder, rate);
    project->audioFolder().createDirectory();
    writeWav (project->audioFolder().getChildFile ("source.wav"), samples);
    auto t = project->addTrack ("Source");
    t.setProperty (ids::rigOn, false, nullptr);
    project->addRegion (t, "Audio/source.wav", 0, 0, (juce::int64) samples.size(), "Source");
    setup (t);
    return project;
}
}

int runSpatialTest();
int runSpatialTest()
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("woodshed-spatial-test");
    temp.deleteRecursively();
    temp.createDirectory();

    //==========================================================================
    std::cout << "interaural time difference (Woodworth)\n";
    for (float az : { -90.0f, -60.0f, -30.0f, 30.0f, 90.0f })
    {
        const auto ir = pan (impulse (2048), { az, 0.0f, 1.0f, 0.0f });
        const double expected = spatial::woodworthItd (az, 0.0) * rate;     // positive: right ear first
        const int lag = lagOf (ir.left, ir.right, 60);                      // positive: right ear later
        const int onsetGap = onset (ir.right) - onset (ir.left);
        const double near = energyDb (az < 0 ? ir.left : ir.right), far = energyDb (az < 0 ? ir.right : ir.left);
        check (std::abs (-lag - expected) <= 2.0 && std::abs (-onsetGap - expected) <= 2.0 && near - far > 3.0,
               "azimuth " + juce::String (az, 0) + ": Woodworth " + juce::String (expected, 1) + " samples, measured "
               + juce::String (-lag) + " (correlation) / " + juce::String (-onsetGap) + " (onset); near ear "
               + db (near - far) + " louder");
    }
    {
        const auto left = pan (impulse (2048), { -90.0f, 0.0f, 1.0f, 0.0f });
        check (onset (left.left) < onset (left.right) && energyDb (left.left) > energyDb (left.right),
               "a source on the left arrives earlier and louder in the left ear");
        const auto front = pan (impulse (2048), { 0.0f, 0.0f, 1.0f, 0.0f });
        check (onset (front.left) == onset (front.right) && std::abs (energyDb (front.left) - energyDb (front.right)) < 0.01,
               "straight ahead: no time or level difference");
    }

    //==========================================================================
    std::cout << "front/back and elevation change the spectrum\n";
    {
        const auto src = noise (96000);
        const auto front = pan (src, { 30.0f, 0.0f, 1.0f, 0.0f }), back = pan (src, { 150.0f, 0.0f, 1.0f, 0.0f });
        const auto fs = spectrum (front.left), bs = spectrum (back.left);
        const double low = bandDb (bs, 150.0, 1500.0) - bandDb (fs, 150.0, 1500.0);
        const double high = bandDb (bs, 5000.0, 14000.0) - bandDb (fs, 5000.0, 14000.0);
        const int lagFront = lagOf (front.left, front.right, 60), lagBack = lagOf (back.left, back.right, 60);
        check (std::abs (low) < 1.5 && high < -4.0 && lagFront == lagBack,
               "30 vs 150 degrees: same time difference (" + juce::String (lagFront) + " / " + juce::String (lagBack)
               + "), lows " + db (low) + ", highs " + db (high) + " behind");

        double previous = 0.0;
        bool rising = true;
        juce::String notches;
        for (float el : { -40.0f, 0.0f, 40.0f, 80.0f })
        {
            const auto ir = pan (impulse (4096), { 0.0f, el, 1.0f, 0.0f });
            const double hz = frequencyOfMinimum (response (ir.left), 3500.0, 16000.0);
            rising = rising && hz > previous + 300.0;
            previous = hz;
            notches << juce::String (el, 0) << " deg: " << juce::String (hz / 1000.0, 2) << " kHz  ";
        }
        check (rising, "the pinna notch climbs with elevation: " + notches.trim());

        const auto up = pan (src, { 0.0f, 40.0f, 1.0f, 0.0f }), down = pan (src, { 0.0f, -40.0f, 1.0f, 0.0f });
        const auto us = spectrum (up.left), ds = spectrum (down.left);
        const double at5k = bandDb (us, 4800.0, 5800.0) - bandDb (ds, 4800.0, 5800.0);
        const double at9k = bandDb (us, 8300.0, 9500.0) - bandDb (ds, 8300.0, 9500.0);
        check (at5k > 2.0 && at9k < -2.0, "up vs down (+-40 degrees): 4.8-5.8 kHz " + db (at5k) + ", 8.3-9.5 kHz " + db (at9k));
    }

    //==========================================================================
    // The fold-down M = (L + R) / 2 against the two ears summed without interference
    // ((|L|^2 + |R|^2) / 4): the ears' time difference makes a comb, which mustn't take more than
    // 6 dB out of any octave band (a polarity or delay mistake would take 20 or more).
    std::cout << "mono compatibility (L+R fold-down vs the ears' power sum, octave bands 63 Hz - 16 kHz)\n";
    {
        const auto src = noise (192000, 11);
        for (float az : { 0.0f, 30.0f, 60.0f, 90.0f, 135.0f, 180.0f })
        {
            const auto b = pan (src, { az, 0.0f, 1.0f, 0.0f });
            std::vector<float> mono (src.size());
            for (size_t i = 0; i < mono.size(); ++i)
                mono[i] = 0.5f * (b.left[i] + b.right[i]);
            const auto ms = spectrum (mono), ls = spectrum (b.left), rs = spectrum (b.right);
            double worst = 1.0e9, worstHz = 0.0;
            for (double hz = 63.0; hz <= 16000.0; hz *= 2.0)
            {
                const double lo = hz / std::sqrt (2.0), hi = hz * std::sqrt (2.0);
                const double ears = 10.0 * std::log10 ((std::pow (10.0, bandDb (ls, lo, hi) / 10.0) + std::pow (10.0, bandDb (rs, lo, hi) / 10.0)) / 4.0);
                const double d = bandDb (ms, lo, hi) - ears;
                if (d < worst) { worst = d; worstHz = hz; }
            }
            check (worst > -6.0, "azimuth " + juce::String (az, 0) + ": worst octave " + db (worst) + " at "
                                 + juce::String (worstHz, 0) + " Hz; fold-down " + db (energyDb (mono) - energyDb (src)) + " against the source");
        }

        // Stereo monitoring of the same 3D track sums back to the source exactly in the middle.
        const auto s = pan (src, { 0.0f, 0.0f, 1.0f, 0.0f }, spatial::Monitoring::stereo);
        double err = 0.0;
        for (size_t i = 2000; i < src.size(); ++i)
            err = juce::jmax (err, (double) std::abs (0.5f * (s.left[i] + s.right[i]) - src[i]));
        check (err < 1.0e-4, "stereo monitoring, centred: the fold-down is the source (max error " + juce::String (err, 6) + ")");
    }

    //==========================================================================
    std::cout << "distance\n";
    {
        const auto src = noise (96000, 3);
        const auto near = pan (src, { 0.0f, 0.0f, 1.0f, 0.0f }), far = pan (src, { 0.0f, 0.0f, 10.0f, 0.0f });
        const auto ns = spectrum (near.left), fs = spectrum (far.left);
        const double low = bandDb (fs, 100.0, 1000.0) - bandDb (ns, 100.0, 1000.0);
        const double high = bandDb (fs, 10000.0, 16000.0) - bandDb (ns, 10000.0, 16000.0);
        check (std::abs (low + 20.0) < 1.0 && high < low - 1.5,
               "10 m vs 1 m: lows " + db (low) + " (inverse distance: -20 dB), highs " + db (high) + " (air absorption)");

        auto withRoom = pan (impulse (4096), { 0.0f, 0.0f, 3.0f, 0.0f, 1.0f });
        const auto dry = pan (impulse (4096), { 0.0f, 0.0f, 3.0f, 0.0f, 0.0f });
        const double tail = energy (withRoom.left.data() + 400, 3000) - energy (dry.left.data() + 400, 3000);
        check (tail > 0.0 && energyDb (withRoom.left) - energyDb (dry.left) < 6.0,
               "early reflections add a small tail (" + db (energyDb (withRoom.left) - energyDb (dry.left)) + ")");
    }

    //==========================================================================
    std::cout << "automation through the engine: smooth, no zipper\n";
    {
        const int length = (int) rate * 2;
        std::vector<float> sine ((size_t) length);
        for (int i = 0; i < length; ++i)
            sine[(size_t) i] = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) rate);

        AudioPool pool;
        auto project = oneTrack (temp.getChildFile ("automation"), sine, [] (juce::ValueTree t)
        {
            t.setProperty (ids::spatial, true, nullptr);
            t.setProperty (ids::azimuth, -90.0, nullptr);
        });
        project->state.setProperty (ids::spatialOutput, "binaural", nullptr);
        const auto still = renderProject (*project, pool, length);

        auto track = project->track (0);
        project->setAutomation (track, "spatial:azimuth", { { 0, -90.0f }, { (juce::int64) rate, 90.0f } });
        project->setAutomation (track, "spatial:distance", { { 0, 1.0f }, { (juce::int64) rate, 4.0f } });
        project->setAutomation (track, "spatial:elevation", { { 0, -30.0f }, { (juce::int64) rate, 60.0f } });
        const auto moving = renderProject (*project, pool, length);

        // A zipper is a jump the sine doesn't make on its own: the largest second difference
        // (and step) in each 10 ms, relative to that stretch's RMS, against a still source's.
        const auto from = (size_t) 4800, to = (size_t) rate;     // after the fade-in edge, while it moves
        const double ratio = worstRelative (moving.right, from, to, maxSecondDifference) / worstRelative (still.right, from, to, maxSecondDifference);
        const double stepRatio = worstRelative (moving.right, from, to, maxStep) / worstRelative (still.right, from, to, maxStep);
        const double leftRatio = worstRelative (moving.left, from, to, maxSecondDifference) / worstRelative (still.left, from, to, maxSecondDifference);
        check (ratio < 1.25 && leftRatio < 1.25 && stepRatio < 1.25,
               "azimuth -90 to 90, elevation -30 to 60, distance 1 to 4 m over 1 s: worst second difference "
               + juce::String (juce::jmax (ratio, leftRatio), 3) + "x a still source's, worst step " + juce::String (stepRatio, 3) + "x");

        // Afterwards it's where the automation left it: right, 60 degrees up (30 degrees from the
        // median plane). On this 220 Hz tone the head-shadow filters add phase to the time
        // difference, as a real head does: low-frequency ITDs run longer than Woodworth's
        // high-frequency figure, up to Kuhn's 3a/c sin(angle).
        std::vector<float> endL (moving.left.begin() + (long) rate + 9600, moving.left.end()), endR (moving.right.begin() + (long) rate + 9600, moving.right.end());
        const int lag = lagOf (endL, endR, 60);
        const double woodworth = spatial::woodworthItd (90.0, 60.0) * rate;
        const double kuhn = 3.0 * spatial::headRadius / spatial::speedOfSound * std::sin (juce::MathConstants<double>::pi / 6.0) * rate;
        check (-lag >= woodworth - 1.0 && -lag <= kuhn + 1.5, "after the move the right ear leads by " + juce::String (-lag)
               + " samples at 220 Hz (Woodworth " + juce::String (woodworth, 1) + ", low-frequency limit " + juce::String (kuhn, 1) + ")");

        const auto params = Project::automatableParams (track);
        int spatialParams = 0;
        for (auto& p : params) spatialParams += p.id.startsWith ("spatial:") ? 1 : 0;
        check (spatialParams == 4, "a 3D track offers azimuth, elevation, distance and spread to automate");

        // Stereo monitoring of the same track: amplitude panning, still unity at the centre.
        project->setAutomation (track, "spatial:azimuth", {});
        project->setAutomation (track, "spatial:distance", {});
        project->setAutomation (track, "spatial:elevation", {});
        track.setProperty (ids::azimuth, 0.0, nullptr);
        project->state.setProperty (ids::spatialOutput, "stereo", nullptr);
        const auto centred = renderProject (*project, pool, length);
        check (std::abs (energyDb (centred.left) - energyDb (sine)) < 0.2 && std::abs (energyDb (centred.right) - energyDb (sine)) < 0.2,
               "stereo monitoring, centred: unity in both channels like the pan knob ("
               + db (energyDb (centred.left) - energyDb (sine), 2) + ")");
    }

    //==========================================================================
    std::cout << "surround bounce\n";
    {
        const auto& five = spatial::SpeakerLayout::fivePointOne();
        const auto& seven = spatial::SpeakerLayout::sevenPointOnePointFour();

        // VBAP gains straight from the layouts.
        std::array<float, 16> g {};
        five.gains (-30.0f, 0.0f, g.data());
        check (std::abs (g[0] - 1.0f) < 1.0e-4f && g[1] + g[2] + g[4] + g[5] < 1.0e-4f, "5.1: a source at -30 degrees is all in L");
        five.gains (-15.0f, 0.0f, g.data());
        check (std::abs (g[0] - g[2]) < 1.0e-3f && std::abs (g[0] * g[0] + g[2] * g[2] - 1.0f) < 1.0e-3f,
               "5.1: halfway between L and C, equal and at unit power (" + juce::String (g[0], 3) + ")");
        float lfe = 0.0f;
        for (float az = -180.0f; az < 180.0f; az += 7.0f)
        {
            seven.gains (az, 20.0f, g.data());
            float power = 0.0f;
            for (int ch = 0; ch < seven.size(); ++ch) power += g[(size_t) ch] * g[(size_t) ch];
            lfe = juce::jmax (lfe, std::abs (power - 1.0f) + g[3]);
        }
        check (lfe < 1.0e-3f, "7.1.4: unit power all the way round, nothing in the LFE");

        const juce::int64 length = 48000;
        const auto src = noise ((int) length, 5);
        BounceOptions o;
        o.surround = "5.1";
        o.bits = 32;

        struct Case { float az, el; int expect; const char* where; };
        for (auto c : { Case { -30.0f, 0.0f, 0, "front-left" }, Case { 110.0f, 0.0f, 5, "right surround" }, Case { 0.0f, 0.0f, 2, "centre" } })
        {
            auto project = oneTrack (temp.getChildFile ("five"), src, [c] (juce::ValueTree t)
            {
                t.setProperty (ids::spatial, true, nullptr);
                t.setProperty (ids::azimuth, (double) c.az, nullptr);
                t.setProperty (ids::elevation, (double) c.el, nullptr);
            });
            const auto file = temp.getChildFile (juce::String ("five-") + c.where + ".wav");
            juce::AudioBuffer<float> buf;
            const bool ok = bounce (*project, file, length, o) && readFile (file, buf);
            const auto s = ok ? shares (buf) : std::vector<double> (6, 0.0);
            check (ok && buf.getNumChannels() == 6 && s[(size_t) c.expect] > 0.95,
                   juce::String ("5.1, a source ") + c.where + ": " + describeShares (s, five));
        }

        // A plain stereo track (no 3D) panned hard left stays in L; centred, it's L and R, no C.
        {
            auto project = oneTrack (temp.getChildFile ("stereo"), src, [] (juce::ValueTree t) { t.setProperty (ids::pan, 0.0, nullptr); });
            const auto file = temp.getChildFile ("five-stereo.wav");
            juce::AudioBuffer<float> buf;
            const bool ok = bounce (*project, file, length, o) && readFile (file, buf);
            const auto s = ok ? shares (buf) : std::vector<double> (6, 0.0);
            check (ok && std::abs (s[0] - 0.5) < 0.02 && std::abs (s[1] - 0.5) < 0.02 && s[2] < 0.001,
                   "5.1, a centred stereo track: " + describeShares (s, five));
            const double level = ok ? 10.0 * std::log10 (energy (buf.getReadPointer (0), buf.getNumSamples()) / energy (src.data(), (int) length)) : -99.0;
            check (std::abs (level) < 0.3, "  ...at the stereo pan's level in L (" + db (level, 2) + ")");
        }

        o.surround = "7.1.4";
        for (auto c : { Case { -45.0f, 45.0f, 8, "top front left" }, Case { 135.0f, 0.0f, 5, "right rear" },
                        Case { 90.0f, 0.0f, 7, "right side" } })
        {
            auto project = oneTrack (temp.getChildFile ("seven"), src, [c] (juce::ValueTree t)
            {
                t.setProperty (ids::spatial, true, nullptr);
                t.setProperty (ids::azimuth, (double) c.az, nullptr);
                t.setProperty (ids::elevation, (double) c.el, nullptr);
            });
            const auto file = temp.getChildFile (juce::String ("seven-") + c.where + ".wav");
            juce::AudioBuffer<float> buf;
            juce::AudioChannelSet layout;
            const bool ok = bounce (*project, file, length, o) && readFile (file, buf, &layout);
            const auto s = ok ? shares (buf) : std::vector<double> (12, 0.0);
            check (ok && buf.getNumChannels() == 12 && s[(size_t) c.expect] > 0.95,
                   juce::String ("7.1.4, a source ") + c.where + ": " + describeShares (s, seven));

            if (c.expect == 8)
            {
                const auto h = readHeader (file);
                check (h.ok && h.format == 0xfffe && h.channels == 12 && h.mask == 0x2d63f,
                       "7.1.4 WAV header: WAVE_FORMAT_EXTENSIBLE, " + juce::String (h.channels) + " channels, mask 0x"
                       + juce::String::toHexString ((int) h.mask) + " (L R C LFE Lrs Rrs Lss Rss Ltf Rtf Ltr Rtr)");
                check (layout == seven.channels, "  ...and it reads back as the 7.1.4 layout (" + layout.getSpeakerArrangementAsString() + ")");
            }
        }
    }

    //==========================================================================
    std::cout << "movie sync (the logic; playback itself needs a screen)\n";
    {
        video::MovieSync sync;
        video::MovieSync::Player player;
        player.open = true;
        player.duration = 120.0;

        // A player whose clock runs 0.3% fast, polled at 30 Hz while the song plays a minute.
        double worst = 0.0, transport = 10.0;
        int seeks = 0;
        for (int tick = 0; tick < 30 * 60; ++tick)
        {
            const auto a = sync.update (true, transport, player);
            if (a.seek) { player.position = a.seekTo; ++seeks; }
            if (a.play) player.playing = true;
            player.speed = a.speed;
            if (tick > 30)
                worst = juce::jmax (worst, std::abs (player.position - transport));
            player.position += (1.0 / 30.0) * player.speed * 1.003;
            transport += 1.0 / 30.0;
        }
        check (worst < 0.02 && seeks == 1, "a 0.3% fast player stays within " + juce::String (worst * 1000.0, 1)
                                           + " ms of the playhead with " + juce::String (seeks) + " seek (the start)");

        auto a = sync.update (true, 30.0, player);     // a locate while playing
        check (a.seek && std::abs (a.seekTo - 30.0) < 1.0e-9, "a locate seeks the movie");
        a = sync.update (false, 70.0, player);
        check (a.stop, "stopping the song stops the movie");
        player.playing = false;
        player.position = 70.0;
        a = sync.update (false, -2.0, player);
        check (a.seek && juce::exactlyEqual (a.seekTo, 0.0) && ! a.play, "before the movie starts it holds the first frame");
        check (video::MovieSync::timecode (3723.5, 25.0) == "01:02:03:12" && video::MovieSync::timecode (-1.0, 30.0) == "-00:00:01:00",
               "timecode: " + video::MovieSync::timecode (3723.5, 25.0) + ", " + video::MovieSync::timecode (-1.0, 30.0));
    }

    temp.deleteRecursively();
    std::cout << (failures == 0 ? "spatial: all passed\n" : "spatial: " + juce::String (failures) + " failed\n");
    return failures == 0 ? 0 : 1;
}

/** A song with one source that circles the listener, rises and falls, for listening on headphones. */
int writeSpatialDemoProject (const juce::File& folder);
int writeSpatialDemoProject (const juce::File& folder)
{
    juce::ScopedJuceInitialiser_GUI init;
    const int length = (int) rate * 16;
    std::vector<float> tone ((size_t) length);
    juce::Random r (1);
    float lp = 0.0f;
    for (int i = 0; i < length; ++i)
    {
        // Brushed noise pulses on the beat over a quiet chord, so both time and level cues are audible.
        const float beat = std::fmod ((float) i / (float) rate * 2.0f, 1.0f);
        lp += 0.3f * (r.nextFloat() * 2.0f - 1.0f - lp);
        const float t = (float) i / (float) rate;
        tone[(size_t) i] = 0.4f * lp * std::exp (-beat * 8.0f)
                         + 0.08f * (std::sin (juce::MathConstants<float>::twoPi * 220.0f * t) + std::sin (juce::MathConstants<float>::twoPi * 277.2f * t));
    }

    auto project = oneTrack (folder, tone, [] (juce::ValueTree t)
    {
        t.setProperty (ids::name, "Circling", nullptr);
        t.setProperty (ids::spatial, true, nullptr);
        t.setProperty (ids::distance, 2.0, nullptr);
        t.setProperty (ids::room, 0.4, nullptr);
    });
    auto track = project->track (0);
    std::vector<AutoPoint> az, el;
    for (int s = 0; s <= 16; ++s)
    {
        az.push_back ({ (juce::int64) (s * rate), -180.0f + 22.5f * (float) s });
        el.push_back ({ (juce::int64) (s * rate), 40.0f * std::sin ((float) s * 0.8f) });
    }
    az.back().value = 179.0f;
    project->setAutomation (track, "spatial:azimuth", az);
    project->setAutomation (track, "spatial:elevation", el);
    track.setProperty (ids::autoParam, "spatial:azimuth", nullptr);
    project->state.setProperty (ids::spatialOutput, "binaural", nullptr);
    project->state.setProperty (ids::selectedTrack, track[ids::id], nullptr);
    const bool ok = project->save();
    std::cout << (ok ? "wrote " : "couldn't write ") << project->projectFile().getFullPathName() << "\n";
    return ok ? 0 : 1;
}
