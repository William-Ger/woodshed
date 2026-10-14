#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// Shared helpers

/** Static EQs recompute coefficients at most once per this many samples while a knob glides. */
constexpr int coeffChunk = 16;

/** Transparent below 1.0, then a smooth knee that never exceeds 2.0. Keeps resonant peaks in check. */
inline float safety (float x)
{
    const float a = std::abs (x);
    return a <= 1.0f ? x : std::copysign (1.0f + std::tanh (a - 1.0f), x);
}

inline float log2Hz (float hz)                  { return std::log2 (juce::jmax (1.0f, hz)); }

/** Resonant low-pass and high-pass outputs peak at about Q, but on broadband guitar the narrow peak adds
    little RMS. A gentle trim keeps loudness steady as Q rises (measured on a DI). */
inline float resonanceTrim (float q)            { return std::pow (juce::jmax (1.0f, q), -0.15f); }

/** Band-pass output scaled so its RMS on broadband input stays put as Q changes: peak gain sqrt(Q). */
inline float bandLevel (const dsp::Svf::Out& o, float q)  { return o.band / std::sqrt (juce::jmax (0.05f, q)); }

/** Transposed direct form II biquad with RBJ cookbook designs. Coefficients are computed in double. */
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

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
        const double inv = 1.0 / A0;
        b0 = (float) (B0 * inv);  b1 = (float) (B1 * inv);  b2 = (float) (B2 * inv);
        a1 = (float) (A1 * inv);  a2 = (float) (A2 * inv);
    }

    struct Omega { double cs, sn; };

    static Omega omega (double hz, double sr)
    {
        const double w = juce::MathConstants<double>::twoPi * juce::jlimit (10.0, sr * 0.45, hz) / sr;
        return { std::cos (w), std::sin (w) };
    }

    static double safeQ (double q)              { return juce::jlimit (0.1, 30.0, q); }

    void peak (double hz, double q, double db, double sr)
    {
        const auto [cs, sn] = omega (hz, sr);
        const double A = std::pow (10.0, db / 40.0), alpha = sn / (2.0 * safeQ (q));
        set (1.0 + alpha * A, -2.0 * cs, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * cs, 1.0 - alpha / A);
    }

    void lowShelf (double hz, double q, double db, double sr)
    {
        const auto [cs, sn] = omega (hz, sr);
        const double A = std::pow (10.0, db / 40.0), alpha = sn / (2.0 * safeQ (q)), s = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1.0) - (A - 1.0) * cs + s), 2.0 * A * ((A - 1.0) - (A + 1.0) * cs), A * ((A + 1.0) - (A - 1.0) * cs - s),
             (A + 1.0) + (A - 1.0) * cs + s, -2.0 * ((A - 1.0) + (A + 1.0) * cs), (A + 1.0) + (A - 1.0) * cs - s);
    }

    void highShelf (double hz, double q, double db, double sr)
    {
        const auto [cs, sn] = omega (hz, sr);
        const double A = std::pow (10.0, db / 40.0), alpha = sn / (2.0 * safeQ (q)), s = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1.0) + (A - 1.0) * cs + s), -2.0 * A * ((A - 1.0) + (A + 1.0) * cs), A * ((A + 1.0) + (A - 1.0) * cs - s),
             (A + 1.0) - (A - 1.0) * cs + s, 2.0 * ((A - 1.0) - (A + 1.0) * cs), (A + 1.0) - (A - 1.0) * cs - s);
    }

    void lowPass (double hz, double q, double sr)
    {
        const auto [cs, sn] = omega (hz, sr);
        const double alpha = sn / (2.0 * safeQ (q));
        set ((1.0 - cs) * 0.5, 1.0 - cs, (1.0 - cs) * 0.5, 1.0 + alpha, -2.0 * cs, 1.0 - alpha);
    }

    void highPass (double hz, double q, double sr)
    {
        const auto [cs, sn] = omega (hz, sr);
        const double alpha = sn / (2.0 * safeQ (q));
        set ((1.0 + cs) * 0.5, -(1.0 + cs), (1.0 + cs) * 0.5, 1.0 + alpha, -2.0 * cs, 1.0 - alpha);
    }
};

/** A bank of linear smoothers for knob-derived values. The first targets after a reset snap instead of gliding. */
template <size_t N>
struct SmoothedSet
{
    std::array<juce::SmoothedValue<float>, N> v;
    bool snap = true;

    void prepare (double sr, double seconds)    { for (auto& s : v) s.reset (sr, seconds); }

    /** Returns true when the values jumped (first block after reset), so callers can recompute at once. */
    bool setTargets (const std::array<float, N>& targets)
    {
        const bool jumped = snap;
        for (size_t i = 0; i < N; ++i)
        {
            if (snap) v[i].setCurrentAndTargetValue (targets[i]);
            else      v[i].setTargetValue (targets[i]);
        }
        snap = false;
        return jumped;
    }

    bool smoothing() const
    {
        return std::any_of (v.begin(), v.end(), [] (const auto& s) { return s.isSmoothing(); });
    }

    void skip (int n)                           { for (auto& s : v) s.skip (n); }
    float operator[] (size_t i) const           { return v[i].getCurrentValue(); }
    float next (size_t i)                       { return v[i].getNextValue(); }
};

/** Fixed-coefficient static EQ scaffolding: per-channel smoothed knobs, coefficients refreshed in chunks. */
template <size_t NumParams, typename Filters>
struct StaticEqChannel
{
    SmoothedSet<NumParams> p;
    Filters f;
};

/** Envelope follower on the rectified signal. */
struct Follower
{
    float env = 0.0f;

    float process (float x, float attack, float release)
    {
        const float a = std::abs (x);
        env += (a > env ? attack : release) * (a - env);
        return env;
    }
};

/** Cheap integer hash to a value in [0, 1). Deterministic, so both channels agree. */
inline float hashToUnit (uint32_t n)
{
    n ^= n >> 16; n *= 0x7feb352dU;
    n ^= n >> 15; n *= 0x846ca68bU;
    n ^= n >> 16;
    return (float) (n >> 8) * (1.0f / 16777216.0f);
}

//==============================================================================
// LADDER: seven fixed peaking bands an octave apart, +-12 dB each.
class GraphicEq final : public Pedal
{
public:
    static constexpr size_t numBands = 7;

    void prepared() override                    { for (auto& c : channels) c.p.prepare (sampleRate, 0.04); }

    void reset() override
    {
        for (auto& c : channels)
        {
            for (auto& b : c.f) b.reset();
            c.p.snap = true;
        }
    }

    void update() override
    {
        std::array<float, numBands> t {};
        for (size_t b = 0; b < numBands; ++b) t[b] = knob ((int) b);

        for (auto& c : channels)
            if (c.p.setTargets (t))
                recompute (c);
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int start = 0; start < n; start += coeffChunk)
        {
            const int len = juce::jmin (coeffChunk, n - start);
            if (c.p.smoothing()) { c.p.skip (len); recompute (c); }

            for (int i = start; i < start + len; ++i)
            {
                float x = data[i];
                for (auto& b : c.f) x = b.process (x);
                data[i] = x;
            }
        }
    }

private:
    using Channel = StaticEqChannel<numBands, std::array<Biquad, numBands>>;

    void recompute (Channel& c) const
    {
        static constexpr float freqs[numBands] { 100.0f, 200.0f, 400.0f, 800.0f, 1600.0f, 3200.0f, 6400.0f };
        for (size_t b = 0; b < numBands; ++b)
            c.f[b].peak (freqs[b], bandQ, c.p[b], sampleRate);
    }

    static constexpr double bandQ = 1.6;    // a little narrower than an octave so neighbours do not pile up
    std::array<Channel, 2> channels;
};

//==============================================================================
// CONTOUR: low shelf, one sweepable bell, high shelf, output.
class ParametricEq final : public Pedal
{
public:
    void prepared() override                    { for (auto& c : channels) c.p.prepare (sampleRate, 0.04); }

    void reset() override
    {
        for (auto& c : channels)
        {
            for (auto& b : c.f) b.reset();
            c.p.snap = true;
        }
    }

    void update() override
    {
        const std::array<float, 8> t { log2Hz (knob (0)), knob (1), log2Hz (knob (2)), knob (3),
                                       std::log2 (knob (4)), log2Hz (knob (5)), knob (6), knob (7) };
        for (auto& c : channels)
            if (c.p.setTargets (t))
                recompute (c);
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int start = 0; start < n; start += coeffChunk)
        {
            const int len = juce::jmin (coeffChunk, n - start);
            if (c.p.smoothing()) { c.p.skip (len); recompute (c); }

            for (int i = start; i < start + len; ++i)
                data[i] = c.f[2].process (c.f[1].process (c.f[0].process (data[i]))) * c.out;
        }
    }

private:
    struct Channel : StaticEqChannel<8, std::array<Biquad, 3>> { float out = 1.0f; };

    void recompute (Channel& c) const
    {
        c.f[0].lowShelf (std::exp2 (c.p[0]), 0.707, c.p[1], sampleRate);
        c.f[1].peak (std::exp2 (c.p[2]), std::exp2 (c.p[4]), c.p[3], sampleRate);
        c.f[2].highShelf (std::exp2 (c.p[5]), 0.707, c.p[6], sampleRate);
        c.out = dsp::dbToGain (c.p[7]);
    }

    std::array<Channel, 2> channels;
};

//==============================================================================
// LEAN: one tilt control pivoting around 800 Hz, plus low cut, high cut and level.
class TiltEq final : public Pedal
{
public:
    void prepared() override                    { for (auto& c : channels) c.p.prepare (sampleRate, 0.04); }

    void reset() override
    {
        for (auto& c : channels)
        {
            for (auto& b : c.f) b.reset();
            c.p.snap = true;
        }
    }

    void update() override
    {
        const std::array<float, 4> t { knob (0), log2Hz (knob (1)), log2Hz (knob (2)), knob (3) };
        for (auto& c : channels)
            if (c.p.setTargets (t))
                recompute (c);
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int start = 0; start < n; start += coeffChunk)
        {
            const int len = juce::jmin (coeffChunk, n - start);
            if (c.p.smoothing()) { c.p.skip (len); recompute (c); }

            for (int i = start; i < start + len; ++i)
            {
                float x = data[i];
                for (auto& b : c.f) x = b.process (x);
                data[i] = x * c.out;
            }
        }
    }

private:
    struct Channel : StaticEqChannel<4, std::array<Biquad, 4>> { float out = 1.0f; };

    void recompute (Channel& c) const
    {
        // Two broad complementary shelves at the pivot: positive tilt lifts treble and trims bass by the same amount.
        constexpr double pivot = 800.0, shelfQ = 0.5;
        c.f[0].lowShelf (pivot, shelfQ, -c.p[0], sampleRate);
        c.f[1].highShelf (pivot, shelfQ, c.p[0], sampleRate);
        c.f[2].highPass (std::exp2 (c.p[1]), 0.707, sampleRate);
        c.f[3].lowPass (std::exp2 (c.p[2]), 0.707, sampleRate);
        c.out = dsp::dbToGain (c.p[3]);
    }

    std::array<Channel, 2> channels;
};

//==============================================================================
// WIRE: steep band limit, a nasal bump in the middle of the band and a little grit.
class Telephone final : public Pedal
{
public:
    void prepared() override                    { for (auto& c : channels) c.p.prepare (sampleRate, 0.04); }

    void reset() override
    {
        for (auto& c : channels)
        {
            for (auto& b : c.f) b.reset();
            c.p.snap = true;
        }
    }

    void update() override
    {
        const std::array<float, 5> t { log2Hz (knob (0)), log2Hz (knob (1)), knob (2), knob (3), knob (4) };
        for (auto& c : channels)
            if (c.p.setTargets (t))
                recompute (c);
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int start = 0; start < n; start += coeffChunk)
        {
            const int len = juce::jmin (coeffChunk, n - start);
            if (c.p.smoothing()) { c.p.skip (len); recompute (c); }

            for (int i = start; i < start + len; ++i)
            {
                float x = data[i];
                for (auto& b : c.f) x = b.process (x);
                // Normalised so a signal at a typical DI's RMS (about 0.06) comes out at the same level at any drive.
                x = std::tanh (x * c.pre) * c.post;
                data[i] = x * c.out;
            }
        }
    }

private:
    struct Channel : StaticEqChannel<5, std::array<Biquad, 5>> { float pre = 1.0f, post = 1.0f, out = 1.0f; };

    void recompute (Channel& c) const
    {
        const double lo = std::exp2 (c.p[0]), hi = juce::jmax (lo * 1.5, (double) std::exp2 (c.p[1]));
        // Fourth-order Butterworth edges (two sections each) for that boxed-in band.
        c.f[0].highPass (lo, 0.5412, sampleRate);
        c.f[1].highPass (lo, 1.3066, sampleRate);
        c.f[2].lowPass (hi, 0.5412, sampleRate);
        c.f[3].lowPass (hi, 1.3066, sampleRate);
        c.f[4].peak (std::sqrt (lo * hi), 1.2, c.p[2], sampleRate);

        constexpr float ref = 0.06f;
        c.pre = dsp::dbToGain (c.p[3] * 30.0f) * 4.0f;
        c.post = ref / std::tanh (ref * c.pre) * std::exp2 (-0.9f * c.p[3]);     // squared-off waves are denser: trim
        // The bump adds loudness, so give some back; raising the low edge strips the guitar's body, so win that back.
        c.out = dsp::dbToGain (c.p[4] - 0.35f * c.p[2]) * std::exp2 (0.78f * (c.p[0] - std::log2 (400.0f))) * makeup;
    }

    // Band-limiting throws away most of a guitar's low end; win it back at the defaults.
    static constexpr float makeup = 3.6f;
    std::array<Channel, 2> channels;
};

//==============================================================================
// YOWL: inductor-style wah. A resonant band-pass sweeps with the position knob, some dry
// low end is kept underneath, and a gentle low-pass takes the fizz off the top.
class Wah final : public Pedal
{
public:
    struct Voicing
    {
        float lowHz, highHz;    // heel and toe resonance
        float qMin, qMax;       // resonance knob range
        float peakGain;         // how far the resonance stands above the dry lows
        float dryLow;           // resonator low-pass output kept under the sweep
        float topHz;            // overall low-pass
        float heelDb, toeDb, midDb; // level makeup across the sweep: the toe loses the guitar's lows, so it needs more
    };

    static constexpr Voicing voicings[] {
        { 350.0f, 2200.0f, 3.5f, 10.0f, 3.2f, 0.30f, 3800.0f,  1.7f,  9.5f, 1.0f },     // vintage: narrow and vocal
        { 300.0f, 2800.0f, 1.8f,  6.0f, 2.6f, 0.45f, 6500.0f, -3.5f,  6.3f, 2.0f },     // modern: wide, more top end
        { 130.0f, 1400.0f, 2.5f,  8.0f, 2.8f, 0.60f, 2600.0f, -3.4f,  3.6f, -0.5f },     // bass: low sweep, full bottom
    };

    void prepared() override
    {
        for (auto& c : channels)
        {
            c.level.reset (sampleRate, 0.03);
            c.q.reset (sampleRate, 0.03);
        }
        posCoeff = dsp::onePoleCoeff (30.0f, sampleRate);
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.svf.reset();
            c.topState = 0.0f;
            c.snap = true;
        }
    }

    void update() override
    {
        voice = voicings[juce::jlimit (0, 2, juce::roundToInt (knob (1)))];
        const float q = juce::jmap (juce::jlimit (0.0f, 1.0f, knob (2)), voice.qMin, voice.qMax);
        const float level = dsp::dbToGain (knob (3));
        const float pos = juce::jlimit (0.0f, 1.0f, knob (0));

        for (auto& c : channels)
        {
            if (c.snap) { c.q.setCurrentAndTargetValue (q); c.level.setCurrentAndTargetValue (level); c.pos = pos; }
            else        { c.q.setTargetValue (q); c.level.setTargetValue (level); }
            c.posTarget = pos;
            c.snap = false;
        }

        topCoeff = dsp::onePoleCoeff (voice.topHz, sampleRate);
        logLow = std::log2 (voice.lowHz);
        logSpan = std::log2 (voice.highHz) - logLow;
        qMid = 0.5f * (voice.qMin + voice.qMax);
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            c.pos += posCoeff * (c.posTarget - c.pos);
            const float q = c.q.getNextValue();
            c.svf.setup (std::exp2 (logLow + logSpan * c.pos), q, sampleRate);

            const float x = data[i];

            // The low-pass and band-pass outputs of one SVF sit in quadrature, so blending them never notches:
            // dry lows below the sweep, the vocal peak on top.
            const auto o = c.svf.process (x);
            const float y = voice.dryLow * o.low + voice.peakGain * o.band * c.svf.k;
            c.topState += topCoeff * (y - c.topState);
            const float makeupDb = voice.heelDb + (voice.toeDb - voice.heelDb) * c.pos + voice.midDb * 4.0f * c.pos * (1.0f - c.pos);
            const float qTrim = std::pow (q / qMid, 0.35f);   // a broad resonance passes more energy
            data[i] = safety (c.topState * qTrim * std::exp2 (makeupDb * dbToLog2) * c.level.getNextValue());
        }
    }

private:
    struct Channel
    {
        dsp::Svf svf;
        juce::SmoothedValue<float> q { 5.0f }, level { 1.0f };
        float pos = 0.5f, posTarget = 0.5f, topState = 0.0f;
        bool snap = true;
    };

    static constexpr float dbToLog2 = 0.16609640f;   // log2(10) / 20
    Voicing voice = voicings[0];
    float posCoeff = 0.0f, topCoeff = 0.0f, logLow = 8.0f, logSpan = 2.0f, qMid = 6.0f;
    std::array<Channel, 2> channels;
};

//==============================================================================
// BLOOM: envelope follower drives a resonant state-variable filter up or down.
class EnvelopeFilter final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels) c.p.prepare (sampleRate, 0.03);
        attack = dsp::onePoleCoeff (1.0f / (juce::MathConstants<float>::twoPi * 0.002f), sampleRate);
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.svf.reset();
            c.follow.env = 0.0f;
            c.p.snap = true;
        }
    }

    void update() override
    {
        sensGain = 4.0f * std::exp2 (juce::jlimit (0.0f, 1.0f, knob (0)) * 6.0f);
        up = juce::roundToInt (knob (3)) == 0;
        bandPass = juce::roundToInt (knob (4)) == 1;
        // Sweeping down rests wide open with the resonance in the treble, which is much hotter on guitar
        // than resting low; these trims (measured on a DI) keep both directions near unity.
        lowTrim = up ? 1.0f : 0.63f;
        bandTrim = up ? 4.5f : 0.85f;
        release = dsp::onePoleCoeff (1000.0f / (juce::MathConstants<float>::twoPi * juce::jmax (10.0f, knob (5))), sampleRate);

        const std::array<float, 5> t { 1.0f + 4.5f * juce::jlimit (0.0f, 1.0f, knob (1)),
                                       juce::jlimit (0.5f, 16.0f, knob (2)),
                                       juce::jlimit (0.0f, 1.0f, knob (6)),
                                       dsp::dbToGain (knob (7)), 0.0f };
        for (auto& c : channels) c.p.setTargets (t);
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float span = c.p.next (0), q = c.p.next (1), mix = c.p.next (2), level = c.p.next (3);
            const float x = data[i];
            const float drive = 1.0f - std::exp (-c.follow.process (x, attack, release) * sensGain);
            const float pos = up ? drive : 1.0f - drive;

            c.svf.setup (baseHz * std::exp2 (span * pos), q, sampleRate);
            const auto o = c.svf.process (x);
            const float wet = bandPass ? bandLevel (o, q) * bandTrim : o.low * resonanceTrim (q) * lowTrim;
            data[i] = safety ((x + mix * (wet - x)) * level);
        }
    }

private:
    struct Channel
    {
        dsp::Svf svf;
        Follower follow;
        SmoothedSet<5> p;
    };

    static constexpr float baseHz = 180.0f;
    float sensGain = 30.0f, attack = 0.5f, release = 0.01f;
    float lowTrim = 1.0f, bandTrim = 4.5f;
    bool up = true, bandPass = false;
    std::array<Channel, 2> channels;
};

//==============================================================================
// TIDE: an LFO sweeps a resonant state-variable filter around a centre frequency.
class LfoFilter final : public Pedal
{
public:
    void prepared() override                    { for (auto& c : channels) c.p.prepare (sampleRate, 0.05); }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.svf.reset();
            c.phase = 0.0f;
            c.cycle = 0;
            c.p.snap = true;
        }
    }

    void update() override
    {
        inc = juce::jlimit (0.01f, 20.0f, knob (0)) / (float) sampleRate;
        mode = juce::jlimit (0, 2, juce::roundToInt (knob (4)));
        shape = juce::jlimit (0, 3, juce::roundToInt (knob (5)));

        const std::array<float, 5> t { 3.0f * juce::jlimit (0.0f, 1.0f, knob (1)), log2Hz (knob (2)),
                                       juce::jlimit (0.5f, 16.0f, knob (3)), juce::jlimit (0.0f, 1.0f, knob (6)),
                                       dsp::dbToGain (knob (7)) };
        for (auto& c : channels) c.p.setTargets (t);
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float depth = c.p.next (0), centre = c.p.next (1), q = c.p.next (2), mix = c.p.next (3), level = c.p.next (4);

            c.phase += inc;
            if (c.phase >= 1.0f) { c.phase -= 1.0f; ++c.cycle; }

            c.svf.setup (std::exp2 (centre + depth * lfo (c)), q, sampleRate);
            const float x = data[i];
            const auto o = c.svf.process (x);
            const float wet = mode == 0 ? o.low * resonanceTrim (q)
                            : mode == 1 ? bandLevel (o, q) * bandMakeup
                                        : o.high * resonanceTrim (q) * highMakeup;
            data[i] = safety ((x + mix * (wet - x)) * level);
        }
    }

private:
    struct Channel
    {
        dsp::Svf svf;
        SmoothedSet<5> p;
        float phase = 0.0f;
        uint32_t cycle = 0;
    };

    /** Bipolar LFO in [-1, 1]. */
    float lfo (const Channel& c) const
    {
        const float ph = c.phase;
        switch (shape)
        {
            case 1:  return 1.0f - 4.0f * std::abs (ph - 0.5f);                                    // triangle
            case 2:  return 2.0f * ph - 1.0f;                                                      // ramp
            case 3:                                                                                // drift: smooth random
            {
                const float a = hashToUnit (c.cycle), b = hashToUnit (c.cycle + 1);
                const float s = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * ph);
                return 2.0f * (a + (b - a) * s) - 1.0f;
            }
            default: return std::sin (juce::MathConstants<float>::twoPi * ph);                    // sine
        }
    }

    static constexpr float bandMakeup = 3.0f, highMakeup = 2.0f;
    float inc = 0.0f;
    int mode = 0, shape = 0;
    std::array<Channel, 2> channels;
};

//==============================================================================
// MOUTH: three formant band-passes morphing through A-E-I-O-U, by hand, LFO or pick attack.
class VowelFilter final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels) c.p.prepare (sampleRate, 0.05);
        vowelCoeff = dsp::onePoleCoeff (25.0f, sampleRate * (1.0 / (double) updateEvery));
        attack = dsp::onePoleCoeff (1.0f / (juce::MathConstants<float>::twoPi * 0.003f), sampleRate);
        release = dsp::onePoleCoeff (1.0f / (juce::MathConstants<float>::twoPi * 0.15f), sampleRate);
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            for (auto& f : c.formants) f.reset();
            c.follow.env = 0.0f;
            c.phase = 0.0f;
            c.counter = 0;
            c.vowel = -1.0f;
            c.p.snap = true;
        }
    }

    void update() override
    {
        control = juce::jlimit (0, 2, juce::roundToInt (knob (1)));
        inc = juce::jlimit (0.05f, 10.0f, knob (2)) / (float) sampleRate;

        const std::array<float, 6> t { juce::jlimit (0.0f, 1.0f, knob (0)), juce::jlimit (0.0f, 1.0f, knob (3)),
                                       juce::jmap (juce::jlimit (0.0f, 1.0f, knob (4)), 2.5f, 0.6f),
                                       std::log2 (juce::jlimit (0.5f, 2.0f, knob (5))),
                                       juce::jlimit (0.0f, 1.0f, knob (6)), dsp::dbToGain (knob (7)) };
        for (auto& c : channels) c.p.setTargets (t);
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float manual = c.p.next (0), depth = c.p.next (1), width = c.p.next (2), shift = c.p.next (3);
            const float mix = c.p.next (4), level = c.p.next (5);
            const float x = data[i];

            c.phase += inc;
            if (c.phase >= 1.0f) c.phase -= 1.0f;
            const float env = c.follow.process (x, attack, release);

            if (c.counter-- <= 0)
            {
                c.counter = updateEvery - 1;

                float v = manual;
                if (control == 1)      v += depth * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * c.phase));
                else if (control == 2) v += depth * (1.0f - std::exp (-env * 12.0f));
                v = v > 1.0f ? 2.0f - v : v;                   // fold back off the end of the vowel row
                v = juce::jlimit (0.0f, 1.0f, v);

                c.vowel = c.vowel < 0.0f ? v : c.vowel + vowelCoeff * (v - c.vowel);
                setFormants (c, width, shift);
            }

            float wet = 0.0f;
            for (size_t f = 0; f < 3; ++f)
                wet += c.amp[f] * c.formants[f].process (x).band * c.formants[f].k;

            // Narrower formants pass less energy; most of that is won back so Reso is a tone control.
            // Shifting the formants up leaves the guitar's strong lows behind; the shift ratio wins that back.
            const float widthComp = std::pow (width * (1.0f / defaultWidth), -0.75f) * std::exp2 (shift);
            data[i] = safety ((x + mix * (wet * widthComp * makeup - x)) * level);
        }
    }

private:
    struct Channel
    {
        std::array<dsp::Svf, 3> formants;
        std::array<float, 3> amp { 1.0f, 0.5f, 0.3f };
        Follower follow;
        SmoothedSet<6> p;
        float phase = 0.0f, vowel = -1.0f;
        int counter = 0;
    };

    struct Vowel { float hz[3]; float amp[3]; };

    void setFormants (Channel& c, float width, float logShift) const
    {
        static constexpr Vowel table[5] {
            // Formant amplitudes carry a per-vowel trim measured on a guitar DI so every vowel lands at about the same loudness.
            { { 650.0f, 1080.0f, 2650.0f }, { 2.45f, 1.22f, 1.10f } },    // A
            { { 400.0f, 1700.0f, 2600.0f }, { 1.33f, 0.37f, 0.40f } },    // E
            { { 290.0f, 1870.0f, 2800.0f }, { 0.71f, 0.14f, 0.10f } },    // I
            { { 400.0f,  800.0f, 2600.0f }, { 1.29f, 0.45f, 0.13f } },    // O
            { { 350.0f,  600.0f, 2700.0f }, { 1.05f, 0.16f, 0.06f } },    // U
        };
        static constexpr float bandwidth[3] { 80.0f, 100.0f, 130.0f };

        const float pos = c.vowel * 4.0f;
        const int a = juce::jlimit (0, 3, (int) pos);
        const float t = pos - (float) a;
        const float shiftRatio = std::exp2 (logShift);

        for (size_t f = 0; f < 3; ++f)
        {
            const float lo = table[a].hz[f], hi = table[a + 1].hz[f];
            const float hz = lo * std::pow (hi / lo, t) * shiftRatio;
            const float q = juce::jlimit (1.0f, 25.0f, hz / (bandwidth[f] * width * shiftRatio));
            c.formants[f].setup (hz, q, sampleRate);
            c.amp[f] = table[a].amp[f] + t * (table[a + 1].amp[f] - table[a].amp[f]);
        }
    }

    static constexpr int updateEvery = 4;
    static constexpr float makeup = 2.4f, defaultWidth = 1.55f;
    float vowelCoeff = 0.1f, attack = 0.5f, release = 0.01f, inc = 0.0f;
    int control = 0;
    std::array<Channel, 2> channels;
};

//==============================================================================
// STAIR: a resonant low-pass that jumps between steps at a free-running rate, with optional glide.
class StepFilter final : public Pedal
{
public:
    StepFilter()
    {
        juce::Random rng (0x57a1);
        for (auto& v : randomSteps) v = rng.nextFloat();
    }

    void prepared() override                    { for (auto& c : channels) c.p.prepare (sampleRate, 0.05); }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.svf.reset();
            c.phase = 0.0f;
            c.step = 0;
            c.logHz = -1.0f;
            c.p.snap = true;
        }
    }

    void update() override
    {
        const float rate = juce::jlimit (0.1f, 30.0f, knob (0));
        inc = rate / (float) sampleRate;
        numSteps = 4 << juce::jlimit (0, 2, juce::roundToInt (knob (1)));
        pattern = juce::jlimit (0, 3, juce::roundToInt (knob (2)));

        const float glideSeconds = 0.0005f + juce::jlimit (0.0f, 1.0f, knob (6)) * 0.6f / rate;
        glide = 1.0f - std::exp (-1.0f / (glideSeconds * (float) sampleRate));

        const std::array<float, 4> t { log2Hz (knob (3)), log2Hz (knob (4)), juce::jlimit (0.5f, 16.0f, knob (5)),
                                       dsp::dbToGain (knob (7)) };
        for (auto& c : channels) c.p.setTargets (t);
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float lo = c.p.next (0), hi = c.p.next (1), q = c.p.next (2), level = c.p.next (3);

            c.phase += inc;
            if (c.phase >= 1.0f) { c.phase -= 1.0f; c.step = (c.step + 1) % numSteps; }
            c.step %= numSteps;

            const float target = lo + (hi - lo) * stepValue (c.step);
            c.logHz = c.logHz < 0.0f ? target : c.logHz + glide * (target - c.logHz);

            c.svf.setup (std::exp2 (c.logHz), q, sampleRate);
            data[i] = safety (c.svf.process (data[i]).low * resonanceTrim (q) * makeup * level);
        }
    }

private:
    struct Channel
    {
        dsp::Svf svf;
        SmoothedSet<4> p;
        float phase = 0.0f, logHz = -1.0f;
        int step = 0;
    };

    float stepValue (int step) const
    {
        const float last = (float) (numSteps - 1);
        switch (pattern)
        {
            case 0:  return (float) step / last;                                            // rise
            case 1:  return 1.0f - (float) step / last;                                     // fall
            case 2:                                                                         // zigzag
            {
                const float half = (float) numSteps * 0.5f;
                const float s = (float) step;
                return s < half ? s / half : 2.0f - s / half;
            }
            default: return randomSteps[(size_t) step];                                     // scatter
        }
    }

    static constexpr float makeup = 1.0f;
    std::array<float, 16> randomSteps {};
    float inc = 0.0f, glide = 1.0f;
    int numSteps = 8, pattern = 3;
    std::array<Channel, 2> channels;
};

} // namespace

//==============================================================================
void registerFilterPedals (Registry& r)
{
    const juce::String hz = "Hz", db = "dB", pct = "%";

    r.add<GraphicEq> ({ "eq.graphic", "LADDER", "7-band graphic eq", "Filter",
                        "Seven octave-spaced bands for carving a sound by eye",
                        juce::Colour (0xff1f6f5c), juce::Colour (0xffeef3e6),
                        { knob ("b100", "100", -12.0f, 12.0f, 0.0f, db),
                          knob ("b200", "200", -12.0f, 12.0f, 0.0f, db),
                          knob ("b400", "400", -12.0f, 12.0f, 0.0f, db),
                          knob ("b800", "800", -12.0f, 12.0f, 0.0f, db),
                          knob ("b1k6", "1.6k", -12.0f, 12.0f, 0.0f, db),
                          knob ("b3k2", "3.2k", -12.0f, 12.0f, 0.0f, db),
                          knob ("b6k4", "6.4k", -12.0f, 12.0f, 0.0f, db) } });

    r.add<ParametricEq> ({ "eq.parametric", "CONTOUR", "parametric eq", "Filter",
                           "Shelves at both ends and a sweepable mid bell for surgical tone work",
                           juce::Colour (0xff9aa0a8), juce::Colour (0xff15181c),
                           { knob ("lowFreq", "Low Freq", 40.0f, 500.0f, 120.0f, hz, 0.5f),
                             knob ("lowGain", "Low", -15.0f, 15.0f, 0.0f, db),
                             knob ("midFreq", "Mid Freq", 150.0f, 6000.0f, 800.0f, hz, 0.35f),
                             knob ("midGain", "Mid", -15.0f, 15.0f, 0.0f, db),
                             knob ("midQ", "Q", 0.3f, 8.0f, 1.0f, {}, 0.4f),
                             knob ("highFreq", "High Freq", 1000.0f, 12000.0f, 4000.0f, hz, 0.45f),
                             knob ("highGain", "High", -15.0f, 15.0f, 0.0f, db),
                             knob ("output", "Output", -12.0f, 12.0f, 0.0f, db) } });

    r.add<TiltEq> ({ "eq.tilt", "LEAN", "tilt eq", "Filter",
                     "One knob leans the whole tone dark or bright, with cuts at either end",
                     juce::Colour (0xffd9a441), juce::Colour (0xff231a0c),
                     { knob ("tilt", "Tilt", -6.0f, 6.0f, 0.0f, db),
                       knob ("lowCut", "Low Cut", 20.0f, 500.0f, 20.0f, hz, 0.4f),
                       knob ("highCut", "High Cut", 1500.0f, 20000.0f, 20000.0f, hz, 0.4f),
                       knob ("level", "Level", -12.0f, 12.0f, 0.0f, db) } });

    r.add<Wah> ({ "filter.wah", "YOWL", "wah", "Filter",
                  "Resonant sweeping wah with vintage, modern and bass voicings",
                  juce::Colour (0xff7a1e2b), juce::Colour (0xfff3e2c7),
                  { knob ("position", "Position", 0.0f, 1.0f, 0.55f, pct),
                    choice ("voice", "Voice", { "Vintage", "Modern", "Bass" }, 0),
                    knob ("resonance", "Resonance", 0.0f, 1.0f, 0.5f, pct),
                    knob ("level", "Level", -12.0f, 12.0f, 0.0f, db) } });

    r.add<EnvelopeFilter> ({ "filter.envelope", "BLOOM", "envelope filter", "Filter",
                             "Pick-driven filter that quacks open on every note",
                             juce::Colour (0xffb7d36b), juce::Colour (0xff1b2410),
                             { knob ("sens", "Sens", 0.0f, 1.0f, 0.5f, pct),
                               knob ("range", "Range", 0.0f, 1.0f, 0.6f, pct),
                               knob ("q", "Q", 0.7f, 12.0f, 4.0f, {}, 0.5f),
                               choice ("direction", "Sweep", { "Up", "Down" }, 0),
                               choice ("mode", "Mode", { "Low-pass", "Band-pass" }, 0),
                               knob ("decay", "Decay", 30.0f, 800.0f, 150.0f, "ms", 0.5f),
                               knob ("mix", "Mix", 0.0f, 1.0f, 1.0f, pct),
                               knob ("level", "Level", -12.0f, 12.0f, 0.0f, db) } });

    r.add<LfoFilter> ({ "filter.lfo", "TIDE", "filter sweep", "Filter",
                        "Resonant filter swept by a slow-to-fast LFO, from gentle swell to throb",
                        juce::Colour (0xff3d5a9e), juce::Colour (0xffe6ecf8),
                        { knob ("rate", "Rate", 0.05f, 10.0f, 0.8f, hz, 0.4f),
                          knob ("depth", "Depth", 0.0f, 1.0f, 0.6f, pct),
                          knob ("centre", "Center", 150.0f, 4000.0f, 900.0f, hz, 0.4f),
                          knob ("resonance", "Reso", 0.7f, 12.0f, 3.0f, {}, 0.5f),
                          choice ("mode", "Mode", { "Low-pass", "Band-pass", "High-pass" }, 0),
                          choice ("shape", "Shape", { "Sine", "Triangle", "Ramp", "Drift" }, 0),
                          knob ("mix", "Mix", 0.0f, 1.0f, 1.0f, pct),
                          knob ("level", "Level", -12.0f, 12.0f, 0.0f, db) } });

    r.add<VowelFilter> ({ "filter.vowel", "MOUTH", "vowel filter", "Filter",
                          "Formant filter that makes the guitar talk through A-E-I-O-U",
                          juce::Colour (0xffe58ba0), juce::Colour (0xff2a0f16),
                          { knob ("vowel", "Vowel", 0.0f, 1.0f, 0.0f, pct),
                            choice ("control", "Control", { "Manual", "LFO", "Envelope" }, 0),
                            knob ("rate", "Rate", 0.1f, 6.0f, 0.8f, hz, 0.5f),
                            knob ("depth", "Depth", 0.0f, 1.0f, 0.6f, pct),
                            knob ("resonance", "Reso", 0.0f, 1.0f, 0.5f, pct),
                            knob ("shift", "Shift", 0.7f, 1.5f, 1.0f, "x"),
                            knob ("mix", "Mix", 0.0f, 1.0f, 1.0f, pct),
                            knob ("level", "Level", -12.0f, 12.0f, 0.0f, db) } });

    r.add<Telephone> ({ "filter.telephone", "WIRE", "lo-fi band limiter", "Filter",
                        "Squeezes the guitar into a narrow, nasal band like an old speaker",
                        juce::Colour (0xff2b2b28), juce::Colour (0xfff0c419),
                        { knob ("low", "Low", 150.0f, 1200.0f, 400.0f, hz, 0.5f),
                          knob ("high", "High", 1200.0f, 8000.0f, 3000.0f, hz, 0.5f),
                          knob ("bump", "Bump", 0.0f, 12.0f, 6.0f, db),
                          knob ("drive", "Drive", 0.0f, 1.0f, 0.3f, pct),
                          knob ("level", "Level", -12.0f, 12.0f, 0.0f, db) } });

    r.add<StepFilter> ({ "filter.step", "STAIR", "step filter", "Filter",
                         "Resonant filter that jumps through a pattern of steps at a free-running rate",
                         juce::Colour (0xff6b4a8f), juce::Colour (0xfff1e8fb),
                         { knob ("rate", "Rate", 0.5f, 20.0f, 4.0f, hz, 0.5f),
                           choice ("steps", "Steps", { "4", "8", "16" }, 1),
                           choice ("pattern", "Pattern", { "Rise", "Fall", "Zigzag", "Scatter" }, 3),
                           knob ("low", "Low", 100.0f, 1000.0f, 300.0f, hz, 0.5f),
                           knob ("high", "High", 600.0f, 6000.0f, 2500.0f, hz, 0.5f),
                           knob ("resonance", "Reso", 0.7f, 12.0f, 5.0f, {}, 0.5f),
                           knob ("glide", "Glide", 0.0f, 1.0f, 0.2f, pct),
                           knob ("level", "Level", -12.0f, 12.0f, 0.0f, db) } });
}

} // namespace woodshed
