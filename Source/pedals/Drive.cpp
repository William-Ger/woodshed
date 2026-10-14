#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// Shared helpers for the drive family.

using LinearSmooth = juce::SmoothedValue<float>;
using GainSmooth   = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>;

/** Mono oversampler: upsample, run a per-sample function at the high rate, downsample in place. */
template <int Log2Factor>
struct Oversampler
{
    static constexpr int factor = 1 << Log2Factor;

    juce::dsp::Oversampling<float> os { 1, (size_t) Log2Factor, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };

    void prepare (int maxBlock)                 { os.initProcessing ((size_t) maxBlock); }
    void reset()                                { os.reset(); }
    int latency() const                         { return (int) std::ceil (os.getLatencyInSamples()); }

    template <typename Fn>
    void process (float* data, int n, Fn&& perSample)
    {
        float* chans[] { data };
        juce::dsp::AudioBlock<float> block (chans, 1, (size_t) n);
        auto up = os.processSamplesUp (block);
        auto* x = up.getChannelPointer (0);

        for (size_t i = 0; i < up.getNumSamples(); ++i)
            x[i] = perSample (x[i]);

        os.processSamplesDown (block);
    }
};

/** One-pole low/high pass. */
struct OnePole
{
    float s = 0.0f, a = 0.0f;

    void setup (float hz, double sr)            { a = dsp::onePoleCoeff (hz, sr); }
    void reset()                                { s = 0.0f; }
    float lp (float x)                          { s += a * (x - s); return s; }
    float hp (float x)                          { return x - lp (x); }
};

/** RBJ biquad (transposed direct form II); coefficients computed without allocation. */
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;

    void reset()                                { z1 = z2 = 0.0f; }

    float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void set (double B0, double B1, double B2, double A0, double A1, double A2)
    {
        b0 = (float) (B0 / A0); b1 = (float) (B1 / A0); b2 = (float) (B2 / A0);
        a1 = (float) (A1 / A0); a2 = (float) (A2 / A0);
    }

    struct Trig { double cs, alpha; };
    static Trig trig (double sr, double hz, double q)
    {
        const double w = juce::MathConstants<double>::twoPi * juce::jlimit (10.0, sr * 0.49, hz) / sr;
        return { std::cos (w), std::sin (w) / (2.0 * q) };
    }

    void peak (double sr, double hz, double q, double db)
    {
        const double A = std::pow (10.0, db / 40.0);
        const auto t = trig (sr, hz, q);
        set (1 + t.alpha * A, -2 * t.cs, 1 - t.alpha * A, 1 + t.alpha / A, -2 * t.cs, 1 - t.alpha / A);
    }

    void lowShelf (double sr, double hz, double db)
    {
        const double A = std::pow (10.0, db / 40.0);
        const auto t = trig (sr, hz, 0.7071);
        const double sq = 2.0 * std::sqrt (A) * t.alpha;
        set (A * ((A + 1) - (A - 1) * t.cs + sq), 2 * A * ((A - 1) - (A + 1) * t.cs), A * ((A + 1) - (A - 1) * t.cs - sq),
             (A + 1) + (A - 1) * t.cs + sq, -2 * ((A - 1) + (A + 1) * t.cs), (A + 1) + (A - 1) * t.cs - sq);
    }

    void highShelf (double sr, double hz, double db)
    {
        const double A = std::pow (10.0, db / 40.0);
        const auto t = trig (sr, hz, 0.7071);
        const double sq = 2.0 * std::sqrt (A) * t.alpha;
        set (A * ((A + 1) + (A - 1) * t.cs + sq), -2 * A * ((A - 1) + (A + 1) * t.cs), A * ((A + 1) + (A - 1) * t.cs - sq),
             (A + 1) - (A - 1) * t.cs + sq, 2 * ((A - 1) - (A + 1) * t.cs), (A + 1) - (A - 1) * t.cs - sq);
    }

    void lowPass (double sr, double hz, double q)
    {
        const auto t = trig (sr, hz, q);
        set ((1 - t.cs) / 2, 1 - t.cs, (1 - t.cs) / 2, 1 + t.alpha, -2 * t.cs, 1 - t.alpha);
    }

    void highPass (double sr, double hz, double q)
    {
        const auto t = trig (sr, hz, q);
        set ((1 + t.cs) / 2, -(1 + t.cs), (1 + t.cs) / 2, 1 + t.alpha, -2 * t.cs, 1 - t.alpha);
    }
};

/** Rational tanh approximation (C1-continuous at the +-3 clamp); cheap inside oversampled loops. */
inline float fastTanh (float x)
{
    x = juce::jlimit (-3.0f, 3.0f, x);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/** Maps 0..1 exponentially between two frequencies. */
inline float sweep (float t, float lowHz, float highHz)     { return lowHz * std::pow (highHz / lowHz, juce::jlimit (0.0f, 1.0f, t)); }

//==============================================================================
// GRIT: tightened low end, asymmetric soft clipping at 4x oversampling, tone, level.
class Overdrive final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.oversampling.initProcessing ((size_t) maxBlock);
            c.preHighPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass (sampleRate, 140.0f);
            c.dcBlock.coefficients     = juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass (sampleRate, 12.0f);
            c.gain.reset (sampleRate, 0.03);
            c.tone.reset (sampleRate, 0.03);
            c.level.reset (sampleRate, 0.03);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.oversampling.reset();
            c.preHighPass.reset();
            c.dcBlock.reset();
            c.toneState = 0.0f;
        }
    }

    int getLatencySamples() const override      { return (int) std::ceil (channels[0].oversampling.getLatencyInSamples()); }

    void update() override
    {
        for (auto& c : channels)
        {
            c.gain.setTargetValue (dsp::dbToGain (juce::jmap (knob (0), 3.0f, 45.0f)));
            c.tone.setTargetValue (700.0f * std::pow (12.0f, knob (2)));   // ~700 Hz .. 8.4 kHz
            c.level.setTargetValue (dsp::dbToGain (knob (1)));
        }
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
            data[i] = c.preHighPass.processSample (data[i]) * c.gain.getNextValue();

        float* chans[] { data };
        juce::dsp::AudioBlock<float> block (chans, 1, (size_t) n);
        auto up = c.oversampling.processSamplesUp (block);
        auto* x = up.getChannelPointer (0);

        constexpr float bias = 0.18f;
        const float biasOffset = std::tanh (bias);

        for (size_t i = 0; i < up.getNumSamples(); ++i)
            x[i] = std::tanh (x[i] + bias) - biasOffset;

        c.oversampling.processSamplesDown (block);

        for (int i = 0; i < n; ++i)
        {
            const float a = dsp::onePoleCoeff (c.tone.getNextValue(), sampleRate);
            c.toneState += a * (c.dcBlock.processSample (data[i]) - c.toneState);
            data[i] = c.toneState * c.level.getNextValue() * outputTrim;
        }
    }

private:
    struct Channel
    {
        juce::dsp::Oversampling<float> oversampling { 1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
        juce::dsp::IIR::Filter<float> preHighPass, dcBlock;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gain { 1.0f }, tone { 4000.0f };
        juce::SmoothedValue<float> level { 1.0f };
        float toneState = 0.0f;
    };

    std::array<Channel, 2> channels;

    // The clipper saturates near full scale; bring it back to roughly a clean DI's level.
    static constexpr float outputTrim = 0.12f;
};

//==============================================================================
// GILDED: clean-blend drive. A mid-pushed copy goes through a soft, asymmetric
// exponential (germanium-diode style) clipper at 2x; the untouched dry signal is
// blended back underneath, then an active treble shelf acts on the sum.
class BlendDrive final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.os.prepare (maxBlock);
            c.preHp.setup (90.0f, sampleRate);
            c.dcBlock.setup (10.0f, sampleRate);
            c.push.peak (sampleRate, 950.0, 0.7, 5.0);
            c.gain.reset (sampleRate, 0.03);
            c.wet.reset (sampleRate, 0.03);
            c.dry.reset (sampleRate, 0.03);
            c.treble.reset (sampleRate, 0.05);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.os.reset(); c.preHp.reset(); c.dcBlock.reset(); c.push.reset(); c.shelf.reset();
            c.lastTreble = -100.0f;
        }
    }

    int getLatencySamples() const override      { return channels[0].os.latency(); }

    void update() override
    {
        const float blend = knob (3);
        const float level = dsp::dbToGain (knob (2));

        for (auto& c : channels)
        {
            c.gain.setTargetValue (dsp::dbToGain (4.0f + 38.0f * knob (0)));
            c.wet.setTargetValue ((1.0f - blend) * level * wetTrim);
            c.dry.setTargetValue (blend * level * dryTrim);
            c.treble.setTargetValue (-9.0f + 18.0f * knob (1));
        }
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];
        float dry[64];
        auto& tmp = scratch;

        const float trebleDb = c.treble.skip (n);
        if (std::abs (trebleDb - c.lastTreble) > 0.01f)
        {
            c.shelf.highShelf (sampleRate, 1800.0, trebleDb);
            c.lastTreble = trebleDb;
        }

        for (int done = 0; done < n; done += 64)
        {
            const int m = juce::jmin (64, n - done);
            float* x = data + done;

            for (int i = 0; i < m; ++i)
            {
                dry[i] = x[i];
                tmp[(size_t) i] = c.push.process (c.preHp.hp (x[i])) * c.gain.getNextValue();
            }

            c.os.process (tmp.data(), m, [] (float v)
            {
                // Forward-biased diode pair, slightly mismatched: soft knee, asymmetric ceiling.
                return v >= 0.0f ? 1.0f - std::exp (-v)
                                 : -0.8f * (1.0f - std::exp (v * 1.25f));
            });

            for (int i = 0; i < m; ++i)
            {
                const float clipped = c.dcBlock.hp (tmp[(size_t) i]);
                x[i] = c.shelf.process (clipped * c.wet.getNextValue() + dry[i] * c.dry.getNextValue());
            }
        }
    }

private:
    struct Channel
    {
        Oversampler<1> os;
        OnePole preHp, dcBlock;
        Biquad push, shelf;
        GainSmooth gain { 1.0f };
        LinearSmooth wet, dry, treble;
        float lastTreble = -100.0f;
    };

    std::array<Channel, 2> channels;
    std::array<float, 64> scratch {};

    static constexpr float wetTrim = 0.19f;
    static constexpr float dryTrim = 1.0f;
};

//==============================================================================
// EMBER: low-gain, touch-sensitive drive. A gentle algebraic curve (much slower to
// flatten than tanh) whose bias follows your picking: dig in and the stage leans
// over into even-order bloom and compression; play softly and it stays nearly clean.
class TouchDrive final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.os.prepare (maxBlock);
            c.preHp.setup (45.0f, sampleRate);
            c.dcBlock.setup (10.0f, sampleRate);
            c.presence.peak (sampleRate, 750.0, 0.6, 3.0);
            c.env.setup (25.0f, sampleRate);   // detector smoothing
            c.gain.reset (sampleRate, 0.03);
            c.level.reset (sampleRate, 0.03);
            c.body.reset (sampleRate, 0.05);
            c.tone.reset (sampleRate, 0.05);
        }

        envRelease = std::exp (-1.0f / (0.12f * (float) sampleRate));
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.os.reset(); c.preHp.reset(); c.dcBlock.reset(); c.presence.reset(); c.bodyShelf.reset();
            c.env.reset(); c.toneLp.reset(); c.toneLp2.reset();
            c.peakEnv = 0.0f; c.lastBody = -100.0f;
        }
    }

    int getLatencySamples() const override      { return channels[0].os.latency(); }

    void update() override
    {
        for (auto& c : channels)
        {
            c.gain.setTargetValue (dsp::dbToGain (2.0f + 30.0f * knob (0)));
            c.level.setTargetValue (dsp::dbToGain (knob (2)) * outputTrim);
            c.body.setTargetValue (-6.0f + 12.0f * knob (3));
            c.tone.setTargetValue (sweep (knob (1), 1300.0f, 11000.0f));
        }
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        const float bodyDb = c.body.skip (n);
        if (std::abs (bodyDb - c.lastBody) > 0.01f)
        {
            c.bodyShelf.lowShelf (sampleRate, 160.0, bodyDb);
            c.lastBody = bodyDb;
        }

        const float toneHz = c.tone.skip (n);
        c.toneLp.setup (toneHz, sampleRate);
        c.toneLp2.setup (toneHz * 1.6f, sampleRate);

        for (int done = 0; done < n; done += 64)
        {
            const int m = juce::jmin (64, n - done);
            float* x = data + done;

            for (int i = 0; i < m; ++i)
            {
                const float v = c.presence.process (c.bodyShelf.process (c.preHp.hp (x[i]))) * c.gain.getNextValue();
                c.peakEnv = juce::jmax (std::abs (v), c.peakEnv * envRelease);
                x[i] = v;
                bias[(size_t) i] = 0.35f * juce::jmin (1.0f, c.env.lp (c.peakEnv));
            }

            int k = 0;
            c.os.process (x, m, [&] (float v)
            {
                // Bias is held across each base-rate sample's oversampled group.
                const float b = bias[(size_t) (k++ / Oversampler<1>::factor)];
                const float a = v + b;
                return a / (1.0f + std::abs (a)) - b / (1.0f + b);
            });

            for (int i = 0; i < m; ++i)
                x[i] = c.toneLp2.lp (c.toneLp.lp (c.dcBlock.hp (x[i]))) * c.level.getNextValue();
        }
    }

private:
    struct Channel
    {
        Oversampler<1> os;
        OnePole preHp, dcBlock, env, toneLp, toneLp2;
        Biquad presence, bodyShelf;
        GainSmooth gain { 1.0f }, level { 1.0f }, tone { 4000.0f };
        LinearSmooth body;
        float peakEnv = 0.0f, lastBody = -100.0f;
    };

    std::array<Channel, 2> channels;
    std::array<float, 64> bias {};
    float envRelease = 0.999f;

    static constexpr float outputTrim = 0.295f;
};

//==============================================================================
// HORNET: op-amp distortion into hard clipping diodes. The op-amp stage's gain is
// strongly frequency dependent (most of it above ~1.5 kHz) and its bandwidth shrinks
// as gain rises, then rails and a sharp diode pair clip at 4x. The Filter knob is a
// single passive low-pass that darkens as it turns up.
class OpAmpDistortion final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.os.prepare (maxBlock);
            c.inHp.setup (30.0f, sampleRate);
            c.legLow.setup (60.0f, sampleRate);
            c.legHigh.setup (1500.0f, sampleRate);
            c.dcBlock.setup (10.0f, sampleRate);
            c.post.setup (12000.0f, sampleRate);
            c.gain.reset (sampleRate, 0.03);
            c.bandwidth.reset (sampleRate, 0.03);
            c.filter.reset (sampleRate, 0.03);
            c.level.reset (sampleRate, 0.03);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.os.reset(); c.inHp.reset(); c.legLow.reset(); c.legHigh.reset();
            c.gbw.reset(); c.dcBlock.reset(); c.filterLp.reset(); c.post.reset();
        }
    }

    int getLatencySamples() const override      { return channels[0].os.latency(); }

    void update() override
    {
        const float g = dsp::dbToGain (8.0f + 58.0f * knob (0));
        mode = juce::roundToInt (knob (3));

        for (auto& c : channels)
        {
            c.gain.setTargetValue (g);
            c.bandwidth.setTargetValue (juce::jlimit (900.0f, 20000.0f, 650000.0f / g));
            c.filter.setTargetValue (sweep (1.0f - knob (1), 480.0f, 16000.0f));
            c.level.setTargetValue (dsp::dbToGain (knob (2)) * modeTrim[(size_t) juce::jlimit (0, 2, mode)]);
        }
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        c.gbw.setup (c.bandwidth.skip (n), sampleRate);
        c.filterLp.setup (c.filter.skip (n), sampleRate);

        for (int i = 0; i < n; ++i)
        {
            const float x = c.inHp.hp (data[i]);
            // Two feedback legs to ground: a small one from 60 Hz, the main one from 1.5 kHz.
            const float shaped = 0.18f * c.legLow.hp (x) + 0.82f * c.legHigh.hp (x);
            data[i] = c.gbw.lp (x + (c.gain.getNextValue() - 1.0f) * shaped);
        }

        const int m = mode;
        c.os.process (data, n, [m] (float v)
        {
            const float rail = 4.2f * std::tanh (v * (1.0f / 4.2f));

            if (m == 0)   // silicon pair: sharp knee near 0.55
            {
                const float r = std::abs (rail) * (1.0f / 0.55f);
                return rail / std::cbrt (1.0f + r * r * r);
            }

            if (m == 1)   // LEDs: higher threshold, even sharper knee
            {
                const float r = std::abs (rail) * (1.0f / 1.7f);
                const float r2 = r * r;
                return rail / std::sqrt (std::sqrt (1.0f + r2 * r2));
            }

            return rail;  // open: only the op-amp rails
        });

        for (int i = 0; i < n; ++i)
            data[i] = c.post.lp (c.filterLp.lp (c.dcBlock.hp (data[i]))) * c.level.getNextValue();
    }

private:
    struct Channel
    {
        Oversampler<2> os;
        OnePole inHp, legLow, legHigh, gbw, dcBlock, filterLp, post;
        GainSmooth gain { 1.0f }, bandwidth { 10000.0f }, filter { 4000.0f }, level { 1.0f };
    };

    std::array<Channel, 2> channels;
    int mode = 0;

    // Per-clip-mode output trims (silicon / LED / open) for equal loudness.
    static constexpr std::array<float, 3> modeTrim { 0.124f, 0.069f, 0.059f };
};

//==============================================================================
// THICKET: thick multi-stage fuzz. Two cascaded gain stages with diodes in their
// feedback loops give logarithmic (asinh) compression rather than a hard ceiling,
// each band-limited like the coupling caps; then a passive low-pass/high-pass blend
// tone control carves the classic mid scoop. The Mids switch fills it back in.
class ThickFuzz final : public Pedal
{
public:
    void prepared() override
    {
        const double osRate = sampleRate * Oversampler<3>::factor;

        for (auto& c : channels)
        {
            c.os.prepare (maxBlock);
            c.inHp.setup (70.0f, sampleRate);
            c.stage1Hp.setup (110.0f, osRate);
            c.stage1Lp.setup (5200.0f, osRate);
            c.stage2Hp.setup (90.0f, osRate);
            c.stage2Lp.setup (4200.0f, osRate);
            c.dcBlock.setup (10.0f, sampleRate);
            c.toneLp.setup (520.0f, sampleRate);
            c.toneHp.setup (1150.0f, sampleRate);
            c.gain.reset (sampleRate, 0.03);
            c.tone.reset (sampleRate, 0.03);
            c.mids.reset (sampleRate, 0.06);
            c.level.reset (sampleRate, 0.03);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.os.reset(); c.inHp.reset(); c.stage1Hp.reset(); c.stage1Lp.reset(); c.stage2Hp.reset(); c.stage2Lp.reset();
            c.dcBlock.reset(); c.toneLp.reset(); c.toneHp.reset(); c.midPeak.reset();
            c.lastMids = -100.0f;
        }
    }

    int getLatencySamples() const override      { return channels[0].os.latency(); }

    void update() override
    {
        const int mids = juce::roundToInt (knob (3));

        for (auto& c : channels)
        {
            c.gain.setTargetValue (dsp::dbToGain (8.0f + 30.0f * knob (0)));
            c.tone.setTargetValue (knob (1));
            c.mids.setTargetValue (mids == 0 ? 0.0f : (mids == 1 ? 7.0f : 11.0f));
            c.level.setTargetValue (dsp::dbToGain (knob (2)) * outputTrim * midsTrim[(size_t) juce::jlimit (0, 2, mids)]);
        }
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        const float midDb = c.mids.skip (n);
        if (std::abs (midDb - c.lastMids) > 0.01f)
        {
            c.midPeak.peak (sampleRate, 800.0, 0.9, midDb);
            c.lastMids = midDb;
        }

        for (int i = 0; i < n; ++i)
            data[i] = c.inHp.hp (data[i]) * c.gain.getNextValue();

        c.os.process (data, n, [&c] (float v)
        {
            // Stage 1: diodes in feedback -> logarithmic squash.
            float s = c.stage1Lp.lp (c.stage1Hp.hp (v));
            s = std::asinh (s * 3.0f) * (1.0f / 3.0f);

            // Stage 2: fixed high gain, same topology, compresses the compressed signal.
            s = c.stage2Lp.lp (c.stage2Hp.hp (s * 22.0f));
            s = std::asinh (s * 2.0f) * 0.5f;

            // Output transistor runs out of swing.
            return std::tanh (s * 0.45f);
        });

        for (int i = 0; i < n; ++i)
        {
            const float v = c.dcBlock.hp (data[i]);
            const float t = c.tone.getNextValue();
            // Passive blend tone: an LP leg and an HP leg crossfaded; the gap between them is the mid scoop.
            const float toned = (1.0f - t) * c.toneLp.lp (v) + t * 1.15f * c.toneHp.hp (v);
            data[i] = c.midPeak.process (toned) * c.level.getNextValue();
        }
    }

private:
    struct Channel
    {
        Oversampler<3> os;
        OnePole inHp, stage1Hp, stage1Lp, stage2Hp, stage2Lp, dcBlock, toneLp, toneHp;
        Biquad midPeak;
        GainSmooth gain { 1.0f }, level { 1.0f };
        LinearSmooth tone { 0.5f }, mids;
        float lastMids = -100.0f;
    };

    std::array<Channel, 2> channels;
    static constexpr float outputTrim = 0.163f;
    static constexpr std::array<float, 3> midsTrim { 1.0f, 0.85f, 0.74f };   // keep the Mids switch level-neutral
};

//==============================================================================
// DUSTBOWL: vintage two-transistor fuzz. The transistor is modelled as a soft
// cut-off (softplus) feeding a soft saturation, around a bias point you can move:
// a healthy bias gives fat asymmetric fuzz, a starved one leaves small signals in
// cut-off so notes gate and sputter. Its low input impedance dulls the pickup at
// full volume; roll the Guitar knob back and it brightens and cleans up.
class VintageFuzz final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.os.prepare (maxBlock);
            c.dcBlock.setup (12.0f, sampleRate);
            c.post.setup (6500.0f, sampleRate);
            c.inGain.reset (sampleRate, 0.03);
            c.loading.reset (sampleRate, 0.03);
            c.level.reset (sampleRate, 0.03);
            c.bias.reset (sampleRate * Oversampler<3>::factor, 0.05);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.os.reset(); c.dcBlock.reset(); c.post.reset(); c.loadLp.reset();
        }
    }

    int getLatencySamples() const override      { return channels[0].os.latency(); }

    void update() override
    {
        const float guitar = knob (3);
        const float taper = guitar * guitar;                     // audio-taper guitar pot

        for (auto& c : channels)
        {
            c.inGain.setTargetValue (juce::jmax (1.0e-4f, taper) * dsp::dbToGain (14.0f + 30.0f * knob (0)));
            c.loading.setTargetValue (3200.0f + 9000.0f * (1.0f - guitar));
            c.bias.setTargetValue (-0.7f + 1.2f * knob (1));
            c.level.setTargetValue (dsp::dbToGain (knob (2)) * outputTrim);
        }
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        c.loadLp.setup (c.loading.skip (n), sampleRate);

        for (int i = 0; i < n; ++i)
            data[i] = c.loadLp.lp (data[i]) * c.inGain.getNextValue();

        c.os.process (data, n, [&c] (float v)
        {
            const float b = c.bias.getNextValue();
            const float a = v + b;
            // softplus with sharpness 3: exponential cut-off below the bias point, linear above.
            const float conduct = a > 8.0f ? a : std::log1p (std::exp (3.0f * a)) * (1.0f / 3.0f);
            if (std::abs (b - c.lastBias) > 1.0e-6f)
            {
                c.lastBias = b;
                c.restOut = std::tanh (1.6f * std::log1p (std::exp (3.0f * b)) * (1.0f / 3.0f));
            }
            return std::tanh (1.6f * conduct) - c.restOut;
        });

        for (int i = 0; i < n; ++i)
            data[i] = c.post.lp (c.dcBlock.hp (data[i])) * c.level.getNextValue();
    }

private:
    struct Channel
    {
        Oversampler<3> os;
        OnePole dcBlock, post, loadLp;
        GainSmooth inGain { 1.0f }, loading { 3200.0f }, level { 1.0f };
        LinearSmooth bias;
        float lastBias = 1.0e9f, restOut = 0.0f;
    };

    std::array<Channel, 2> channels;
    static constexpr float outputTrim = 0.098f;
};

//==============================================================================
// SERRATE: tight modern metal distortion. Pre-EQ tightens the lows (2nd-order high
// pass) and pushes the upper mids into two cascaded stages: a biased soft stage, then
// a hard-kneed one, band-limited between them. Post: low shelf, sweepable-depth mid
// (scooped by default), high shelf, and a fizz-taming low-pass.
class MetalDistortion final : public Pedal
{
public:
    void prepared() override
    {
        const double osRate = sampleRate * Oversampler<3>::factor;

        for (auto& c : channels)
        {
            c.os.prepare (maxBlock);
            c.prePeak.peak (sampleRate, 1100.0, 0.8, 7.0);
            c.preLp.setup (6500.0f, sampleRate);
            c.interHp.setup (140.0f, osRate);
            c.interLp.setup (6800.0f, osRate);
            c.dcBlock.setup (10.0f, sampleRate);
            c.fizz.lowPass (sampleRate, 7800.0, 0.65);
            c.gain1.reset (sampleRate, 0.03);
            c.gain2.reset (osRate, 0.03);
            c.tight.reset (sampleRate, 0.05);
            c.low.reset (sampleRate, 0.05);
            c.mid.reset (sampleRate, 0.05);
            c.high.reset (sampleRate, 0.05);
            c.level.reset (sampleRate, 0.03);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.os.reset(); c.tightHp.reset(); c.prePeak.reset(); c.preLp.reset(); c.interHp.reset(); c.interLp.reset();
            c.dcBlock.reset(); c.lowShelf.reset(); c.midPeak.reset(); c.highShelf.reset(); c.fizz.reset();
            c.last = { -1.0f, -100.0f, -100.0f, -100.0f };
        }
    }

    int getLatencySamples() const override      { return channels[0].os.latency(); }

    void update() override
    {
        const float g = knob (0);

        for (auto& c : channels)
        {
            c.gain1.setTargetValue (dsp::dbToGain (16.0f + 16.0f * g));
            c.gain2.setTargetValue (dsp::dbToGain (4.0f + 22.0f * g));
            c.tight.setTargetValue (sweep (knob (1), 60.0f, 280.0f));
            c.low.setTargetValue (knob (2));
            c.mid.setTargetValue (knob (3));
            c.high.setTargetValue (knob (4));
            c.level.setTargetValue (dsp::dbToGain (knob (5)) * outputTrim);
        }
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        const std::array<float, 4> now { c.tight.skip (n), c.low.skip (n), c.mid.skip (n), c.high.skip (n) };
        if (std::abs (now[0] - c.last[0]) > 0.05f)  { c.tightHp.highPass (sampleRate, now[0], 0.75);       c.last[0] = now[0]; }
        if (std::abs (now[1] - c.last[1]) > 0.01f)  { c.lowShelf.lowShelf (sampleRate, 110.0, now[1]);     c.last[1] = now[1]; }
        if (std::abs (now[2] - c.last[2]) > 0.01f)  { c.midPeak.peak (sampleRate, 650.0, 0.85, now[2]);    c.last[2] = now[2]; }
        if (std::abs (now[3] - c.last[3]) > 0.01f)  { c.highShelf.highShelf (sampleRate, 3200.0, now[3]);  c.last[3] = now[3]; }

        for (int i = 0; i < n; ++i)
            data[i] = c.preLp.lp (c.prePeak.process (c.tightHp.process (data[i]))) * c.gain1.getNextValue();

        c.os.process (data, n, [&c] (float v)
        {
            constexpr float bias = 0.15f;
            float s = std::tanh (v + bias) - 0.14888f;                 // tanh (bias)
            s = c.interLp.lp (c.interHp.hp (s)) * c.gain2.getNextValue();
            const float s2 = s * s;
            return s / std::sqrt (std::sqrt (1.0f + s2 * s2));          // hard knee, flat top
        });

        for (int i = 0; i < n; ++i)
        {
            float v = c.dcBlock.hp (data[i]);
            v = c.highShelf.process (c.midPeak.process (c.lowShelf.process (v)));
            data[i] = c.fizz.process (v) * c.level.getNextValue();
        }
    }

private:
    struct Channel
    {
        Oversampler<3> os;
        Biquad tightHp, prePeak, lowShelf, midPeak, highShelf, fizz;
        OnePole preLp, interHp, interLp, dcBlock;
        GainSmooth gain1 { 1.0f }, gain2 { 1.0f }, tight { 120.0f }, level { 1.0f };
        LinearSmooth low, mid, high;
        std::array<float, 4> last { -1.0f, -100.0f, -100.0f, -100.0f };
    };

    std::array<Channel, 2> channels;
    static constexpr float outputTrim = 0.052f;
};

//==============================================================================
// GLINT: single-transistor treble booster. A small input cap high-passes the guitar
// before a germanium stage that compresses its positive swing early and its negative
// swing late, so it adds a bright, slightly broken-up edge that pushes an amp.
class TrebleBooster final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.os.prepare (maxBlock);
            c.dcBlock.setup (10.0f, sampleRate);
            c.post.setup (9000.0f, sampleRate);
            c.gain.reset (sampleRate, 0.03);
            c.corner.reset (sampleRate, 0.04);
            c.level.reset (sampleRate, 0.03);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.os.reset(); c.inHp.reset(); c.inHp2.reset(); c.dcBlock.reset(); c.post.reset();
        }
    }

    int getLatencySamples() const override      { return channels[0].os.latency(); }

    void update() override
    {
        const int range = juce::jlimit (0, 2, juce::roundToInt (knob (1)));
        static constexpr std::array<float, 3> corners { 1700.0f, 650.0f, 110.0f };

        for (auto& c : channels)
        {
            c.gain.setTargetValue (dsp::dbToGain (6.0f + 26.0f * knob (0)));
            c.corner.setTargetValue (corners[(size_t) range]);
            c.level.setTargetValue (dsp::dbToGain (knob (2)) * rangeTrim[(size_t) range]);
        }
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        const float corner = c.corner.skip (n);
        c.inHp.setup (corner, sampleRate);
        c.inHp2.setup (60.0f, sampleRate);

        for (int i = 0; i < n; ++i)
            data[i] = c.inHp2.hp (c.inHp.hp (data[i])) * c.gain.getNextValue();

        c.os.process (data, n, [] (float v)
        {
            return v >= 0.0f ? v / (1.0f + 1.4f * v)
                             : std::tanh (v);
        });

        for (int i = 0; i < n; ++i)
            data[i] = c.post.lp (c.dcBlock.hp (data[i])) * c.level.getNextValue();
    }

private:
    struct Channel
    {
        Oversampler<1> os;
        OnePole inHp, inHp2, dcBlock, post;
        GainSmooth gain { 1.0f }, corner { 1700.0f }, level { 1.0f };
    };

    std::array<Channel, 2> channels;

    // Treble / Mid / Full range trims: the narrower the range, the more level the input cap throws away.
    static constexpr std::array<float, 3> rangeTrim { 0.82f, 0.387f, 0.19f };
};

//==============================================================================
// WRAITH: octave-up fuzz. A band-limited, softly driven copy is full-wave rectified
// (a smooth |x| at 8x) which doubles its fundamental, DC-blocked, blended with the
// un-rectified signal and slammed into an asymmetric clipper.
class OctaveFuzz final : public Pedal
{
public:
    void prepared() override
    {
        const double osRate = sampleRate * Oversampler<3>::factor;

        for (auto& c : channels)
        {
            c.os.prepare (maxBlock);
            c.inHp.setup (160.0f, sampleRate);
            c.inLp.setup (2600.0f, sampleRate);
            c.inLp2.setup (3800.0f, sampleRate);
            c.rectDc.setup (25.0f, osRate);
            c.dcBlock.setup (12.0f, sampleRate);
            c.gain1.reset (sampleRate, 0.03);
            c.gain2.reset (osRate, 0.03);
            c.octave.reset (osRate, 0.03);
            c.tone.reset (sampleRate, 0.03);
            c.level.reset (sampleRate, 0.03);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.os.reset(); c.inHp.reset(); c.inLp.reset(); c.inLp2.reset(); c.rectDc.reset(); c.dcBlock.reset(); c.toneLp.reset();
        }
    }

    int getLatencySamples() const override      { return channels[0].os.latency(); }

    void update() override
    {
        for (auto& c : channels)
        {
            c.gain1.setTargetValue (dsp::dbToGain (10.0f + 14.0f * knob (0)));
            c.gain2.setTargetValue (dsp::dbToGain (4.0f + 22.0f * knob (0)));
            c.octave.setTargetValue (knob (1));
            c.tone.setTargetValue (sweep (knob (2), 1100.0f, 9000.0f));
            c.level.setTargetValue (dsp::dbToGain (knob (3)) * outputTrim);
        }
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];
        c.toneLp.setup (c.tone.skip (n), sampleRate);

        // Driver stage: band-limit so the rectifier sees mostly the fundamental (soft-clipped at 8x below).
        for (int i = 0; i < n; ++i)
            data[i] = c.inLp2.lp (c.inLp.lp (c.inHp.hp (data[i]))) * c.gain1.getNextValue();

        c.os.process (data, n, [&c] (float v)
        {
            v = std::tanh (v);                                          // driver soft clip
            const float rect = std::sqrt (v * v + 1.0e-4f) - 0.01f;      // smooth full-wave rectifier
            const float o = c.octave.getNextValue();
            float s = c.rectDc.hp (1.6f * o * rect + (1.0f - o) * v);
            s *= c.gain2.getNextValue();
            // Asymmetric clipper: sharp on top, softer below.
            if (s >= 0.0f)
            {
                const float s3 = s * s * s;
                return s / std::cbrt (1.0f + s3);
            }
            return 0.85f * std::tanh (s * (1.0f / 0.85f));
        });

        for (int i = 0; i < n; ++i)
            data[i] = c.toneLp.lp (c.dcBlock.hp (data[i])) * c.level.getNextValue();
    }

private:
    struct Channel
    {
        Oversampler<3> os;
        OnePole inHp, inLp, inLp2, rectDc, dcBlock, toneLp;
        GainSmooth gain1 { 1.0f }, gain2 { 1.0f }, tone { 4000.0f }, level { 1.0f };
        LinearSmooth octave;
    };

    std::array<Channel, 2> channels;
    static constexpr float outputTrim = 0.081f;
};

//==============================================================================
// CRUMBLE: bitcrusher / lo-fi. Sample-and-hold decimation (no anti-alias filter on
// purpose) and a mid-tread quantizer with a continuous bit depth, then a resonant
// low-pass to tame or shape the hash, and a wet/dry mix.
class Bitcrusher final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.bits.reset (sampleRate, 0.05);
            c.rate.reset (sampleRate, 0.05);
            c.cutoff.reset (sampleRate, 0.05);
            c.mix.reset (sampleRate, 0.03);
            c.level.reset (sampleRate, 0.03);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.phase = 1.0f;
            c.held = 0.0f;
            c.filter.reset();
        }
    }

    void update() override
    {
        for (auto& c : channels)
        {
            c.bits.setTargetValue (knob (0));
            c.rate.setTargetValue (knob (1));
            c.cutoff.setTargetValue (knob (2));
            c.mix.setTargetValue (knob (3));
            c.level.setTargetValue (dsp::dbToGain (knob (4)) * outputTrim);
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        const float steps = std::exp2 (c.bits.skip (n) - 1.0f);
        const float invSteps = 1.0f / steps;
        const float increment = c.rate.skip (n) / (float) sampleRate;
        c.filter.setup (c.cutoff.skip (n), 0.9f, sampleRate);

        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            c.phase += increment;

            if (c.phase >= 1.0f)
            {
                c.phase -= std::floor (c.phase);
                c.held = std::round (juce::jlimit (-1.0f, 1.0f, dry) * steps) * invSteps;
            }

            const float wet = c.filter.process (c.held).low;
            const float m = c.mix.getNextValue();
            x[i] = (wet * m + dry * (1.0f - m)) * c.level.getNextValue();
        }
    }

private:
    struct Channel
    {
        float phase = 1.0f, held = 0.0f;
        dsp::Svf filter;
        LinearSmooth bits { 8.0f }, mix { 1.0f };
        GainSmooth rate { 11000.0f }, cutoff { 6000.0f }, level { 1.0f };
    };

    std::array<Channel, 2> channels;
    static constexpr float outputTrim = 1.0f;
};

//==============================================================================
// SPUTTER: dying-battery fuzz. Supply voltage sets the clipping headroom and a
// cross-over dead zone; the circuit's own current draw sags the supply further while
// you play, so hard notes choke, decays crackle and gate, and quiet ones splutter out.
class BatteryFuzz final : public Pedal
{
public:
    void prepared() override
    {
        const double osRate = sampleRate * Oversampler<2>::factor;

        for (auto& c : channels)
        {
            c.os.prepare (maxBlock);
            c.inHp.setup (90.0f, sampleRate);
            c.dcBlock.setup (12.0f, sampleRate);
            c.gain.reset (sampleRate, 0.03);
            c.battery.reset (osRate, 0.05);
            c.tone.reset (sampleRate, 0.03);
            c.level.reset (sampleRate, 0.03);
        }

        sagAttack = 1.0f - std::exp (-1.0f / (0.006f * (float) osRate));
        sagRelease = 1.0f - std::exp (-1.0f / (0.22f * (float) osRate));
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.os.reset(); c.inHp.reset(); c.dcBlock.reset(); c.toneLp.reset();
            c.sag = 0.0f;
        }
    }

    int getLatencySamples() const override      { return channels[0].os.latency(); }

    void update() override
    {
        for (auto& c : channels)
        {
            c.gain.setTargetValue (dsp::dbToGain (14.0f + 28.0f * knob (0)));
            c.battery.setTargetValue (knob (1));
            c.tone.setTargetValue (sweep (knob (2), 900.0f, 8000.0f));
            c.level.setTargetValue (dsp::dbToGain (knob (3)) * outputTrim);
        }
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];
        c.toneLp.setup (c.tone.skip (n), sampleRate);

        for (int i = 0; i < n; ++i)
            data[i] = c.inHp.hp (data[i]) * c.gain.getNextValue();

        const float att = sagAttack, rel = sagRelease;

        c.os.process (data, n, [&c, att, rel] (float v)
        {
            const float battery = c.battery.getNextValue();
            const float supply = 0.3f + 0.7f * battery;
            const float volts = juce::jmax (0.08f, supply * (1.0f - 0.65f * (1.0f - battery) * c.sag));

            // Starved transistors: a smooth dead zone around zero that widens as the supply drops.
            const float dz = 0.02f + 0.55f * (1.0f - volts);
            const float d = v - dz * fastTanh (v / dz);

            const float y = d >= 0.0f ? volts * fastTanh (d / volts)
                                      : 0.75f * volts * fastTanh (d / (0.75f * volts));

            const float draw = std::abs (y);
            c.sag += (draw > c.sag ? att : rel) * (draw - c.sag);
            return y;
        });

        for (int i = 0; i < n; ++i)
            data[i] = c.toneLp.lp (c.dcBlock.hp (data[i])) * c.level.getNextValue();
    }

private:
    struct Channel
    {
        Oversampler<2> os;
        OnePole inHp, dcBlock, toneLp;
        float sag = 0.0f;
        GainSmooth gain { 1.0f }, tone { 3000.0f }, level { 1.0f };
        LinearSmooth battery { 0.4f };
    };

    std::array<Channel, 2> channels;
    float sagAttack = 0.01f, sagRelease = 0.0001f;
    static constexpr float outputTrim = 0.157f;
};

} // namespace

void registerDrivePedals (Registry& r)
{
    r.add<Overdrive> ({ "drive.overdrive", "GRIT", "overdrive", "Drive",
                        "Warm, mid-forward overdrive that stacks well into an amp",
                        juce::Colour (0xffc4582b), juce::Colour (0xff1d1310),
                        { knob ("drive", "Drive", 0.0f, 1.0f, 0.45f, "%"),
                          knob ("level", "Level", -24.0f, 12.0f, 0.0f, "dB"),
                          knob ("tone", "Tone", 0.0f, 1.0f, 0.55f, "%") } });

    r.add<BlendDrive> ({ "drive.blend", "GILDED", "clean-blend drive", "Drive",
                         "Your clean tone stays intact underneath a soft, glowing layer of grit",
                         juce::Colour (0xffc9a24a), juce::Colour (0xff231a08),
                         { knob ("gain", "Gain", 0.0f, 1.0f, 0.4f, "%"),
                           knob ("treble", "Treble", 0.0f, 1.0f, 0.5f, "%"),
                           knob ("level", "Level", -24.0f, 12.0f, 0.0f, "dB"),
                           knob ("blend", "Clean", 0.0f, 1.0f, 0.45f, "%") } });

    r.add<TouchDrive> ({ "drive.touch", "EMBER", "touch overdrive", "Drive",
                         "Low-gain drive that cleans up when you play soft and growls when you dig in",
                         juce::Colour (0xff2c4f8a), juce::Colour (0xffe9eef8),
                         { knob ("gain", "Gain", 0.0f, 1.0f, 0.4f, "%"),
                           knob ("tone", "Tone", 0.0f, 1.0f, 0.5f, "%"),
                           knob ("level", "Level", -24.0f, 12.0f, 0.0f, "dB"),
                           knob ("body", "Body", 0.0f, 1.0f, 0.5f, "%") } });

    r.add<OpAmpDistortion> ({ "drive.distortion", "HORNET", "distortion", "Drive",
                              "Op-amp grind into hard diodes, with a filter that goes from snarl to wool",
                              juce::Colour (0xff1e1e1e), juce::Colour (0xfff2d03b),
                              { knob ("distortion", "Distortion", 0.0f, 1.0f, 0.5f, "%"),
                                knob ("filter", "Filter", 0.0f, 1.0f, 0.4f, "%"),
                                knob ("volume", "Volume", -24.0f, 12.0f, 0.0f, "dB"),
                                choice ("clip", "Clip", { "Silicon", "LED", "Open" }, 0) } });

    r.add<ThickFuzz> ({ "drive.fuzz", "THICKET", "fuzz", "Drive",
                        "Huge, sustaining wall of fuzz with a scooped, sweepable tone",
                        juce::Colour (0xff3f5a36), juce::Colour (0xfff0ead2),
                        { knob ("sustain", "Sustain", 0.0f, 1.0f, 0.55f, "%"),
                          knob ("tone", "Tone", 0.0f, 1.0f, 0.5f, "%"),
                          knob ("volume", "Volume", -24.0f, 12.0f, 0.0f, "dB"),
                          choice ("mids", "Mids", { "Scoop", "Flat", "Lift" }, 0) } });

    r.add<VintageFuzz> ({ "drive.vintage", "DUSTBOWL", "vintage fuzz", "Drive",
                          "Woolly two-transistor fuzz that cleans up as you roll back the guitar",
                          juce::Colour (0xffb5753a), juce::Colour (0xff20140a),
                          { knob ("fuzz", "Fuzz", 0.0f, 1.0f, 0.7f, "%"),
                            knob ("bias", "Bias", 0.0f, 1.0f, 0.55f, "%"),
                            knob ("volume", "Volume", -24.0f, 12.0f, 0.0f, "dB"),
                            knob ("guitar", "Guitar Vol", 0.0f, 1.0f, 1.0f, "%") } });

    r.add<MetalDistortion> ({ "drive.metal", "SERRATE", "metal distortion", "Drive",
                              "Tight, high-gain chug with a pre-EQ low-end clamp and three-band post EQ",
                              juce::Colour (0xff8c1c22), juce::Colour (0xfff3f0ee),
                              { knob ("gain", "Gain", 0.0f, 1.0f, 0.6f, "%"),
                                knob ("tight", "Tight", 0.0f, 1.0f, 0.5f, "%"),
                                knob ("low", "Low", -12.0f, 12.0f, 2.0f, "dB"),
                                knob ("mid", "Mid", -12.0f, 12.0f, -4.0f, "dB"),
                                knob ("high", "High", -12.0f, 12.0f, 1.0f, "dB"),
                                knob ("level", "Level", -24.0f, 12.0f, 0.0f, "dB") } });

    r.add<TrebleBooster> ({ "drive.treble", "GLINT", "treble booster", "Drive",
                            "Bright germanium push that makes a dark amp bite",
                            juce::Colour (0xffbfc5c9), juce::Colour (0xff3a1c14),
                            { knob ("boost", "Boost", 0.0f, 1.0f, 0.5f, "%"),
                              choice ("range", "Range", { "Treble", "Mid", "Full" }, 0),
                              knob ("level", "Level", -24.0f, 12.0f, 0.0f, "dB") } });

    r.add<OctaveFuzz> ({ "drive.octave", "WRAITH", "octave fuzz", "Drive",
                         "Rectified fuzz with a ghostly octave up, strongest on the neck pickup",
                         juce::Colour (0xff4b2a63), juce::Colour (0xffe4d6f2),
                         { knob ("fuzz", "Fuzz", 0.0f, 1.0f, 0.6f, "%"),
                           knob ("octave", "Octave", 0.0f, 1.0f, 0.75f, "%"),
                           knob ("tone", "Tone", 0.0f, 1.0f, 0.55f, "%"),
                           knob ("level", "Level", -24.0f, 12.0f, 0.0f, "dB") } });

    r.add<Bitcrusher> ({ "drive.crusher", "CRUMBLE", "bitcrusher", "Drive",
                         "Sample-rate and bit-depth reduction from gritty lo-fi to broken game console",
                         juce::Colour (0xff1f8a7a), juce::Colour (0xff0d1f1c),
                         { knob ("bits", "Bits", 2.0f, 16.0f, 8.0f, {}, 0.8f),
                           knob ("rate", "Rate", 300.0f, 48000.0f, 11000.0f, "Hz", 0.3f),
                           knob ("filter", "Filter", 300.0f, 20000.0f, 6500.0f, "Hz", 0.3f),
                           knob ("mix", "Mix", 0.0f, 1.0f, 1.0f, "%"),
                           knob ("level", "Level", -24.0f, 12.0f, 0.0f, "dB") } });

    r.add<BatteryFuzz> ({ "drive.battery", "SPUTTER", "dying-battery fuzz", "Drive",
                          "Starved, sputtering fuzz whose notes choke and crackle as they decay",
                          juce::Colour (0xff6e6a3a), juce::Colour (0xfff5efc8),
                          { knob ("fuzz", "Fuzz", 0.0f, 1.0f, 0.6f, "%"),
                            knob ("battery", "Battery", 0.0f, 1.0f, 0.4f, "%"),
                            knob ("tone", "Tone", 0.0f, 1.0f, 0.5f, "%"),
                            knob ("volume", "Volume", -24.0f, 12.0f, 0.0f, "dB") } });
}

} // namespace woodshed
