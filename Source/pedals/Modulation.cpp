#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// Shared helpers for the modulation family.

constexpr float kPi    = juce::MathConstants<float>::pi;
constexpr float kTwoPi = juce::MathConstants<float>::twoPi;
constexpr float kSqrt2 = juce::MathConstants<float>::sqrt2;

inline float wrap01 (float p)               { return p - std::floor (p); }
inline float sinTurns (float p)             { return std::sin (kTwoPi * p); }
inline float cosTurns (float p)             { return std::cos (kTwoPi * p); }

/** Triangle in [-1, 1], phase-aligned with sinTurns (0 at p = 0, rising). */
inline float triTurns (float p)             { return 1.0f - 4.0f * std::abs (wrap01 (p + 0.25f) - 0.5f); }

/** Sine pushed into a square with soft edges (no clicks), in [-1, 1]. */
inline float softSquareTurns (float p)      { return juce::jlimit (-1.0f, 1.0f, 5.0f * sinTurns (p)); }

/** Cheap tanh-like soft clip, exact enough for feedback paths and gentle saturation. */
inline float softClip (float x)
{
    const float c = juce::jlimit (-3.0f, 3.0f, x);
    return c * (27.0f + c * c) / (27.0f + 9.0f * c * c);
}

/** Coefficient for a first-order allpass with -90 degrees at `hz`. */
inline float allpassCoeff (float hz, float sr)
{
    const float t = std::tan (kPi * juce::jlimit (20.0f, sr * 0.45f, hz) / sr);
    return (t - 1.0f) / (t + 1.0f);
}

/** Per-sample one-pole parameter smoother; jumps to the first value it is given after reset(). */
struct Smooth
{
    float value = 0.0f, target = 0.0f, coeff = 1.0f;
    bool primed = false;

    void prepare (double sr, float seconds)     { coeff = 1.0f - std::exp (-1.0f / (seconds * (float) sr)); primed = false; }
    void reset()                                { primed = false; }
    void set (float t)                          { target = t; if (! primed) { value = t; primed = true; } }
    float next()                                { value += coeff * (target - value); return value; }
};

/** Phase-continuous LFO in turns [0, 1); the rate glides, the phase never jumps. */
struct Lfo
{
    float phase = 0.0f, inc = 0.0f, target = 0.0f, coeff = 1.0f;
    bool primed = false;

    void prepare (double sr)                    { coeff = dsp::onePoleCoeff (4.0f, sr); }
    void reset (float startPhase = 0.0f)        { phase = startPhase; primed = false; }

    void setRate (float hz, double sr)
    {
        target = juce::jlimit (0.0f, 0.25f, hz / (float) sr);
        if (! primed) { inc = target; primed = true; }
    }

    float advance()
    {
        inc += coeff * (target - inc);
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
        return phase;
    }
};

/** First-order allpass, H(z) = (a + z^-1) / (1 + a z^-1). */
struct Allpass1
{
    float s = 0.0f;
    float process (float x, float a)            { const float y = a * x + s; s = x - a * y; return y; }
};

struct OnePoleLp
{
    float s = 0.0f;
    float process (float x, float c)            { s += c * (x - s); return s; }
};

/** Smooth random wander in roughly [-1, 1]; realtime-safe LCG, two-pole smoothed. */
struct Wander
{
    uint32_t seed = 1;
    float target = 0.0f, a = 0.0f, b = 0.0f;
    int count = 0;

    void reset (uint32_t newSeed)               { seed = newSeed; target = a = b = 0.0f; count = 0; }

    float next (float coeff, int holdSamples)
    {
        if (--count <= 0)
        {
            count = holdSamples;
            seed = seed * 1664525u + 1013904223u;
            target = (float) (seed >> 8) * (2.0f / 16777216.0f) - 1.0f;
        }

        a += coeff * (target - a);
        b += coeff * (a - b);
        return b;
    }
};

/** Level compensation for a dry/wet crossfade of partly decorrelated signals. */
inline float blendMakeup (float mix, float strength)
{
    const float power = (1.0f - mix) * (1.0f - mix) + mix * mix;
    return std::pow (power, -0.5f * strength);
}

//==============================================================================
// LAGOON: lush stereo chorus, one to three voices spread across the field.
class Chorus final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        for (auto& l : lines) l.setMaxSamples ((int) (sampleRate * 0.07));
        lfo.prepare (sampleRate);
        depth.prepare (sampleRate, 0.05f);
        delayMs.prepare (sampleRate, 0.2f);
        tone.prepare (sampleRate, 0.05f);
        width.prepare (sampleRate, 0.05f);
        mix.prepare (sampleRate, 0.05f);
    }

    void reset() override
    {
        for (auto& l : lines) l.clear();
        for (auto& t : toneLp) t.s = 0.0f;
        lfo.reset();
        for (auto* s : { &depth, &delayMs, &tone, &width, &mix }) s->reset();
    }

    void update() override
    {
        lfo.setRate (knob (0), sampleRate);
        depth.set (knob (1));
        delayMs.set (knob (2));
        voices = 1 + juce::jlimit (0, 2, juce::roundToInt (knob (3)));
        tone.set (dsp::onePoleCoeff (knob (4), sampleRate));
        width.set (knob (5));
        mix.set (knob (6));
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        const float msToSamples = (float) sampleRate * 0.001f;
        const float voiceNorm = 1.0f / std::sqrt ((float) voices);
        const float voiceStep = 1.0f / (float) voices;

        for (int i = 0; i < n; ++i)
        {
            const float inL = left[i], inR = right[i];
            lines[0].push (inL);
            lines[1].push (inR);

            const float p = lfo.advance();
            const float d = depth.next(), base = delayMs.next(), tc = tone.next(), w = width.next(), m = mix.next();
            const float sweep = d * juce::jmin (0.45f * base, 3.2f);
            const float rightOffset = w * 0.5f * voiceStep;

            float wetL = 0.0f, wetR = 0.0f;

            for (int v = 0; v < voices; ++v)
            {
                const float vp = p + (float) v * voiceStep;
                const float centre = base * (1.0f + 0.19f * (float) v);
                const float dev = sweep * (1.0f - 0.15f * (float) v);
                wetL += lines[0].read (juce::jmax (2.0f, (centre + dev * sinTurns (vp)) * msToSamples));
                wetR += lines[1].read (juce::jmax (2.0f, (centre + dev * sinTurns (vp + rightOffset)) * msToSamples));
            }

            wetL = toneLp[0].process (wetL * voiceNorm, tc);
            wetR = toneLp[1].process (wetR * voiceNorm, tc);

            const float makeup = blendMakeup (m, 0.6f);
            left[i]  = makeup * ((1.0f - m) * inL + m * wetL);
            right[i] = makeup * ((1.0f - m) * inR + m * wetR);
        }
    }

private:
    std::array<dsp::DelayBuffer, 2> lines;
    std::array<OnePoleLp, 2> toneLp;
    Lfo lfo;
    Smooth depth, delayMs, tone, width, mix;
    int voices = 2;
};

//==============================================================================
// COMET: flanger with feedback, manual and an optional through-zero sweep.
class Flanger final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.line.setMaxSamples ((int) (sampleRate * 0.03));
            c.lfo.prepare (sampleRate);
            c.depth.prepare (sampleRate, 0.05f);
            c.manual.prepare (sampleRate, 0.08f);
            c.feedback.prepare (sampleRate, 0.03f);
            c.mix.prepare (sampleRate, 0.03f);
            c.zero.prepare (sampleRate, 0.02f);
        }
    }

    void reset() override
    {
        for (size_t ch = 0; ch < channels.size(); ++ch)
        {
            auto& c = channels[ch];
            c.line.clear();
            c.lfo.reset (ch == 0 ? 0.0f : 0.25f);
            for (auto* s : { &c.depth, &c.manual, &c.feedback, &c.mix, &c.zero }) s->reset();
        }
    }

    void update() override
    {
        for (auto& c : channels)
        {
            c.lfo.setRate (knob (0), sampleRate);
            c.depth.set (knob (1));
            c.manual.set (knob (2));
            c.feedback.set (knob (3));
            c.mix.set (knob (4));
            c.zero.set (knob (5) > 0.5f ? 1.0f : 0.0f);
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];
        const float msToSamples = (float) sampleRate * 0.001f;
        constexpr float zeroPointMs = 5.0f;

        for (int i = 0; i < n; ++i)
        {
            const float u = 0.5f + 0.5f * triTurns (c.lfo.advance());
            const float d = c.depth.next(), man = c.manual.next(), fb = c.feedback.next(), m = c.mix.next(), z = c.zero.next();

            // Classic: exponential sweep up from the manual setting. Through-zero: the wet tap
            // sweeps either side of a delayed dry tap, so the two cross and cancel.
            const float classicMs = juce::jmin (20.0f, man * std::exp2 (d * 3.0f * u));
            const float span = 0.2f + d * 4.0f;
            const float tzMs = juce::jlimit (0.1f, 10.0f, zeroPointMs + (man - 1.0f) * 0.6f + span * (2.0f * u - 1.0f));
            const float wetMs = classicMs + z * (tzMs - classicMs);

            const float wet = c.line.read (juce::jmax (2.0f, wetMs * msToSamples));
            const float dryTap = c.line.read (zeroPointMs * msToSamples);
            const float in = x[i];

            c.line.push (in * (1.0f - 0.45f * std::abs (fb)) + fb * softClip (wet));

            const float dry = in + z * (dryTap - in);
            const float wetSigned = wet * (1.0f - 2.0f * z);
            x[i] = blendMakeup (m, 1.0f) * ((1.0f - m) * dry + m * wetSigned);
        }
    }

private:
    struct Channel
    {
        dsp::DelayBuffer line;
        Lfo lfo;
        Smooth depth, manual, feedback, mix, zero;
    };

    std::array<Channel, 2> channels;
};

//==============================================================================
// WHORL: stereo phaser, 4 to 12 stages with feedback and LFO spread between sides.
class Phaser final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        lfo.prepare (sampleRate);
        depth.prepare (sampleRate, 0.05f);
        centre.prepare (sampleRate, 0.08f);
        feedback.prepare (sampleRate, 0.03f);
        spread.prepare (sampleRate, 0.05f);
        mix.prepare (sampleRate, 0.03f);
    }

    void reset() override
    {
        for (auto& c : channels) { for (auto& s : c.stages) s.s = 0.0f; c.fbState = 0.0f; }
        lfo.reset();
        for (auto* s : { &depth, &centre, &feedback, &spread, &mix }) s->reset();
    }

    void update() override
    {
        static constexpr int stageCounts[] { 4, 6, 8, 12 };
        lfo.setRate (knob (0), sampleRate);
        depth.set (knob (1));
        centre.set (std::log2 (knob (2)));
        feedback.set (knob (3));
        const int newStages = stageCounts[juce::jlimit (0, 3, juce::roundToInt (knob (4)))];
        if (newStages != numStages)
            for (auto& c : channels)
                for (size_t s = (size_t) juce::jmin (newStages, numStages); s < c.stages.size(); ++s)
                    c.stages[s].s = 0.0f;
        numStages = newStages;
        spread.set (knob (5));
        mix.set (knob (6));
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        const float sr = (float) sampleRate;
        float* io[] { left, right };

        for (int i = 0; i < n; ++i)
        {
            const float p = lfo.advance();
            const float d = depth.next(), logF = centre.next(), fb = feedback.next(), sp = spread.next(), m = mix.next();
            const float inGain = 1.0f - 0.5f * fb;
            const float makeup = blendMakeup (m, 0.8f);

            for (size_t ch = 0; ch < 2; ++ch)
            {
                auto& c = channels[ch];
                const float ph = ch == 0 ? p : p + 0.5f * sp;
                const float a = allpassCoeff (std::exp2 (logF + d * 2.2f * sinTurns (ph)), sr);

                const float dry = io[ch][i];
                float y = dry * inGain + fb * c.fbState;
                for (int s = 0; s < numStages; ++s)
                    y = c.stages[(size_t) s].process (y, a);
                c.fbState = y;

                io[ch][i] = makeup * ((1.0f - m) * dry + m * y);
            }
        }
    }

private:
    struct Channel
    {
        std::array<Allpass1, 12> stages;
        float fbState = 0.0f;
    };

    std::array<Channel, 2> channels;
    Lfo lfo;
    Smooth depth, centre, feedback, spread, mix;
    int numStages = 4;
};

//==============================================================================
// SHIMMY: amplitude tremolo with sine, triangle, square and a lopsided "bias" wave.
class Tremolo final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.lfo.prepare (sampleRate);
            c.depth.prepare (sampleRate, 0.03f);
            c.makeup.prepare (sampleRate, 0.05f);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.lfo.reset();
            c.depth.reset();
            c.makeup.reset();
        }
        cachedWave = -1;
    }

    void update() override
    {
        wave = juce::jlimit (0, 3, juce::roundToInt (knob (2)));
        const float d = knob (1);

        if (wave != cachedWave || std::abs (d - cachedDepth) > 1.0e-4f)
        {
            // RMS of the gain curve over one cycle, so every wave and depth sits at unity.
            double sum = 0.0;
            constexpr int points = 128;
            for (int k = 0; k < points; ++k)
            {
                const float g = 1.0f - d * shape (wave, ((float) k + 0.5f) / (float) points);
                sum += (double) (g * g);
            }
            cachedMakeup = 1.0f / (float) std::sqrt (juce::jmax (1.0e-3, sum / points));
            cachedWave = wave;
            cachedDepth = d;
        }

        const float out = dsp::dbToGain (knob (3));
        for (auto& c : channels)
        {
            c.lfo.setRate (knob (0), sampleRate);
            c.depth.set (d);
            c.makeup.set (cachedMakeup * out);
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float g = (1.0f - c.depth.next() * shape (wave, c.lfo.advance())) * c.makeup.next();
            float y = x[i] * g;
            if (wave == 3)
                y = softClip (y * 1.4f) * (1.0f / 1.4f);
            x[i] = y;
        }
    }

private:
    /** Amount of cut in [0, 1] for each waveform at phase p. */
    static float shape (int w, float p)
    {
        switch (w)
        {
            case 1:  return 0.5f + 0.5f * triTurns (p);
            case 2:  return 0.5f + 0.5f * softSquareTurns (p);
            case 3:
            {
                // Fast dive, slow recovery, lingering near full volume: a valve bias swing.
                constexpr float k = 0.3f;
                const float skewed = p < k ? 0.5f * p / k : 0.5f + 0.5f * (p - k) / (1.0f - k);
                const float s = 0.5f + 0.5f * sinTurns (skewed);
                return s * s;
            }
            default: return 0.5f + 0.5f * sinTurns (p);
        }
    }

    struct Channel
    {
        Lfo lfo;
        Smooth depth, makeup;
    };

    std::array<Channel, 2> channels;
    int wave = 0, cachedWave = -1;
    float cachedDepth = -1.0f, cachedMakeup = 1.0f;
};

//==============================================================================
// TIDEPOOL: harmonic tremolo; lows and highs pulse in opposite phase around a crossover.
class HarmonicTremolo final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.lfo.prepare (sampleRate);
            c.depth.prepare (sampleRate, 0.03f);
            c.out.prepare (sampleRate, 0.03f);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.split.reset();
            c.lfo.reset();
            c.depth.reset();
            c.out.reset();
        }
    }

    void update() override
    {
        const float d = knob (1);
        const float makeup = 1.0f / std::sqrt ((1.0f - 0.5f * d) * (1.0f - 0.5f * d) + d * d / 8.0f);

        for (auto& c : channels)
        {
            c.lfo.setRate (knob (0), sampleRate);
            c.depth.set (d);
            c.split.setup (knob (2), 0.5f, sampleRate);
            c.out.set (makeup * dsp::dbToGain (knob (3)));
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float s = sinTurns (c.lfo.advance());
            const float d = c.depth.next();
            const float low = c.split.process (x[i]).low;
            const float high = x[i] - low;
            const float gLow  = 1.0f - d * (0.5f + 0.5f * s);
            const float gHigh = 1.0f - d * (0.5f - 0.5f * s);
            x[i] = c.out.next() * (gLow * low + gHigh * high);
        }
    }

private:
    struct Channel
    {
        dsp::Svf split;
        Lfo lfo;
        Smooth depth, out;
    };

    std::array<Channel, 2> channels;
};

//==============================================================================
// QUIVER: pure pitch vibrato; depth is a pitch swing that holds steady across rates.
class Vibrato final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.line.setMaxSamples ((int) (sampleRate * 0.06));
            c.lfo.prepare (sampleRate);
            c.swing.prepare (sampleRate, 0.1f);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.line.clear();
            c.lfo.reset();
            c.swing.reset();
        }
    }

    void update() override
    {
        const float rate = knob (0);
        const float cents = knob (1) * 120.0f;
        // Peak delay swing (seconds) that gives `cents` of peak pitch deviation at this rate.
        const float swingSeconds = juce::jmin (0.02f, (std::exp2 (cents / 1200.0f) - 1.0f) / (kTwoPi * rate));
        wave = juce::roundToInt (knob (2));

        for (auto& c : channels)
        {
            c.lfo.setRate (rate, sampleRate);
            c.swing.set (swingSeconds * (float) sampleRate);
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            c.line.push (x[i]);
            const float p = c.lfo.advance();
            const float w = wave == 1 ? triTurns (p) : sinTurns (p);
            const float a = c.swing.next();
            x[i] = c.line.read (2.0f + a * (1.0f + w));
        }
    }

private:
    struct Channel
    {
        dsp::DelayBuffer line;
        Lfo lfo;
        Smooth swing;
    };

    std::array<Channel, 2> channels;
    int wave = 0;
};

//==============================================================================
// CAROUSEL: rotating-speaker cabinet; horn and drum rotors with their own inertia,
// doppler and amplitude swirl picked up by two mics.
class Rotary final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        hornLine.setMaxSamples ((int) (sampleRate * 0.01));
        drumLine.setMaxSamples ((int) (sampleRate * 0.01));
        crossover.setup (800.0f, 0.5f, sampleRate);
        cabinetCoeff = dsp::onePoleCoeff (7000.0f, sampleRate);
        brightCoeff = dsp::onePoleCoeff (2500.0f, sampleRate);
        balance.prepare (sampleRate, 0.05f);
        drive.prepare (sampleRate, 0.05f);
        micAngle.prepare (sampleRate, 0.05f);
        mix.prepare (sampleRate, 0.03f);
    }

    void reset() override
    {
        hornLine.clear();
        drumLine.clear();
        crossover.reset();
        for (auto& s : bright) s.s = 0.0f;
        for (auto& s : cabinet) s.s = 0.0f;
        hornAngle = 0.0f;
        drumAngle = 0.37f;
        primedSpeed = false;
        for (auto* s : { &balance, &drive, &micAngle, &mix }) s->reset();
    }

    void update() override
    {
        static constexpr float hornSpeeds[] { 0.0f, 0.83f, 6.7f };
        static constexpr float drumSpeeds[] { 0.0f, 0.68f, 5.6f };
        const int sel = juce::jlimit (0, 2, juce::roundToInt (knob (0)));
        hornTarget = hornSpeeds[sel];
        drumTarget = drumSpeeds[sel];

        if (! primedSpeed)
        {
            hornSpeed = hornTarget;
            drumSpeed = drumTarget;
            primedSpeed = true;
        }

        // The light horn spins up in well under a second; the heavy drum takes a few.
        const float ramp = knob (1);
        const float sr = (float) sampleRate;
        hornUp   = 1.0f / (0.55f * ramp * sr);
        hornDown = 1.0f / (0.9f * ramp * sr);
        drumUp   = 1.0f / (2.6f * ramp * sr);
        drumDown = 1.0f / (3.8f * ramp * sr);

        balance.set (knob (2));
        drive.set (knob (3));
        micAngle.set (knob (4) * 0.25f);
        mix.set (knob (5));
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        const float sr = (float) sampleRate;
        const float msToSamples = sr * 0.001f;

        for (int i = 0; i < n; ++i)
        {
            hornSpeed += (hornTarget - hornSpeed) * (hornTarget > hornSpeed ? hornUp : hornDown);
            drumSpeed += (drumTarget - drumSpeed) * (drumTarget > drumSpeed ? drumUp : drumDown);
            hornAngle = wrap01 (hornAngle + hornSpeed / sr);
            drumAngle = wrap01 (drumAngle - drumSpeed / sr);    // the rotors turn opposite ways

            const float dryL = left[i], dryR = right[i];
            const float dr = drive.next();
            const float preGain = 1.0f + 5.0f * dr;
            const float x = softClip (0.5f * (dryL + dryR) * preGain) / (1.0f + 3.2f * dr);

            const float low = crossover.process (x).low;
            hornLine.push (x - low);
            drumLine.push (low);

            const float hc = cosTurns (hornAngle), hs = sinTurns (hornAngle);
            const float dc = cosTurns (drumAngle), ds = sinTurns (drumAngle);
            const float mic = micAngle.next();
            const float mc = cosTurns (mic), ms = sinTurns (mic);

            const float b = balance.next();
            const float hornGain = juce::jmin (1.0f, 2.0f * b);
            const float drumGain = juce::jmin (1.0f, 2.0f * (1.0f - b));
            const float m = mix.next();

            float* out[] { left, right };
            const float dry[] { dryL, dryR };

            for (size_t side = 0; side < 2; ++side)
            {
                // cos of the angle between each rotor and this mic (mics at +/- micAngle).
                const float sgn = side == 0 ? 1.0f : -1.0f;
                const float hFace = hc * mc + sgn * hs * ms;
                const float dFace = dc * mc + sgn * ds * ms;

                float horn = hornLine.read (2.0f + (0.9f - 0.38f * hFace) * msToSamples);
                const float dull = bright[side].process (horn, brightCoeff);
                horn = (dull + (0.35f + 0.65f * (0.5f + 0.5f * hFace)) * (horn - dull)) * (0.58f + 0.42f * hFace);

                const float drum = drumLine.read (2.0f + (0.6f - 0.12f * dFace) * msToSamples) * (0.74f + 0.26f * dFace);

                const float wet = cabinet[side].process (1.2f * (hornGain * horn + drumGain * drum), cabinetCoeff);
                out[side][i] = (1.0f - m) * dry[side] + m * wet;
            }
        }
    }

private:
    dsp::DelayBuffer hornLine, drumLine;
    dsp::Svf crossover;
    std::array<OnePoleLp, 2> bright, cabinet;
    Smooth balance, drive, micAngle, mix;
    float hornAngle = 0.0f, drumAngle = 0.0f, hornSpeed = 0.0f, drumSpeed = 0.0f;
    float hornTarget = 0.0f, drumTarget = 0.0f;
    float hornUp = 0.0f, hornDown = 0.0f, drumUp = 0.0f, drumDown = 0.0f;
    float cabinetCoeff = 0.5f, brightCoeff = 0.3f;
    bool primedSpeed = false;
};

//==============================================================================
// MOTH: photocell-style throb; four staggered allpasses swept by a lamp-and-cell LFO.
class Throb final : public Pedal
{
public:
    void prepared() override
    {
        lampAttack = dsp::onePoleCoeff (40.0f, sampleRate);
        lampRelease = dsp::onePoleCoeff (7.0f, sampleRate);
        for (auto& c : channels)
        {
            c.lfo.prepare (sampleRate);
            c.intensity.prepare (sampleRate, 0.05f);
            c.blend.prepare (sampleRate, 0.03f);
            c.out.prepare (sampleRate, 0.03f);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            for (auto& s : c.stages) s.s = 0.0f;
            c.cell = 0.0f;
            c.lfo.reset();
            for (auto* s : { &c.intensity, &c.blend, &c.out }) s->reset();
        }
    }

    void update() override
    {
        const bool chorus = knob (2) < 0.5f;
        for (auto& c : channels)
        {
            c.lfo.setRate (knob (0), sampleRate);
            c.intensity.set (knob (1));
            c.blend.set (chorus ? 0.5f : 1.0f);
            c.out.set ((chorus ? 1.3f : 1.0f) * dsp::dbToGain (knob (3)));
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        static constexpr float stageRatios[] { 1.0f, 2.6f, 6.5f, 15.0f };
        constexpr float baseHz = 48.0f;
        constexpr float skew = 0.32f;
        auto& c = channels[(size_t) ch];
        const float sr = (float) sampleRate;

        for (int i = 0; i < n; ++i)
        {
            // Lamp: a skewed sine, squared so it flashes briefly; the cell lags it, fast up, slow down.
            const float p = c.lfo.advance();
            const float skewed = p < skew ? 0.5f * p / skew : 0.5f + 0.5f * (p - skew) / (1.0f - skew);
            const float s = 0.5f + 0.5f * sinTurns (skewed);
            const float lamp = s * s;
            c.cell += (lamp > c.cell ? lampAttack : lampRelease) * (lamp - c.cell);

            const float sweep = std::exp2 (c.intensity.next() * 4.0f * c.cell);
            float y = x[i];
            for (size_t st = 0; st < c.stages.size(); ++st)
                y = c.stages[st].process (y, allpassCoeff (baseHz * stageRatios[st] * sweep, sr));

            const float m = c.blend.next();
            x[i] = c.out.next() * ((1.0f - m) * x[i] + m * y);
        }
    }

private:
    struct Channel
    {
        std::array<Allpass1, 4> stages;
        Lfo lfo;
        Smooth intensity, blend, out;
        float cell = 0.0f;
    };

    std::array<Channel, 2> channels;
    float lampAttack = 0.1f, lampRelease = 0.01f;
};

//==============================================================================
// ORBIT: ring modulator with a gliding carrier, LFO or pick-envelope carrier sweep.
class RingMod final : public Pedal
{
public:
    void prepared() override
    {
        envAttack = dsp::onePoleCoeff (80.0f, sampleRate);
        envRelease = dsp::onePoleCoeff (1.5f, sampleRate);
        for (auto& c : channels)
        {
            c.lfo.prepare (sampleRate);
            c.logFreq.prepare (sampleRate, 0.06f);
            c.mix.prepare (sampleRate, 0.03f);
            c.modDepth.prepare (sampleRate, 0.05f);
            c.tone.prepare (sampleRate, 0.05f);
            c.lfoAmt.prepare (sampleRate, 0.03f);
            c.envAmt.prepare (sampleRate, 0.03f);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.carrier = 0.0f;
            c.env = 0.0f;
            c.toneLp.s = 0.0f;
            c.lfo.reset();
            for (auto* s : { &c.logFreq, &c.mix, &c.modDepth, &c.tone, &c.lfoAmt, &c.envAmt }) s->reset();
        }
    }

    void update() override
    {
        const int source = juce::jlimit (0, 2, juce::roundToInt (knob (2)));
        for (auto& c : channels)
        {
            c.logFreq.set (std::log2 (knob (0)));
            c.mix.set (knob (1));
            c.lfoAmt.set (source == 1 ? 1.0f : 0.0f);
            c.envAmt.set (source == 2 ? 1.0f : 0.0f);
            c.modDepth.set (knob (3));
            c.lfo.setRate (knob (4), sampleRate);
            c.tone.set (dsp::onePoleCoeff (knob (5), sampleRate));
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];
        const float invSr = 1.0f / (float) sampleRate;

        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            const float level = std::abs (dry);
            c.env += (level > c.env ? envAttack : envRelease) * (level - c.env);

            const float md = c.modDepth.next();
            const float lfoMod = c.lfoAmt.next() * 2.0f * sinTurns (c.lfo.advance());
            const float envMod = c.envAmt.next() * 3.0f * juce::jmin (1.0f, c.env * 6.0f);
            const float hz = juce::jmin (12000.0f, std::exp2 (c.logFreq.next() + md * (lfoMod + envMod)));

            c.carrier += hz * invSr;
            if (c.carrier >= 1.0f) c.carrier -= 1.0f;

            const float wet = c.toneLp.process (dry * sinTurns (c.carrier) * kSqrt2, c.tone.next());
            const float m = c.mix.next();
            x[i] = blendMakeup (m, 1.0f) * ((1.0f - m) * dry + m * wet);
        }
    }

private:
    struct Channel
    {
        Lfo lfo;
        Smooth logFreq, mix, modDepth, tone, lfoAmt, envAmt;
        OnePoleLp toneLp;
        float carrier = 0.0f, env = 0.0f;
    };

    std::array<Channel, 2> channels;
    float envAttack = 0.1f, envRelease = 0.001f;
};

//==============================================================================
// SWAY: equal-power auto-panner.
class AutoPan final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        lfo.prepare (sampleRate);
        depth.prepare (sampleRate, 0.03f);
        gainCoeff = dsp::onePoleCoeff (120.0f, sampleRate);
    }

    void reset() override
    {
        lfo.reset();
        depth.reset();
        gainL = gainR = 1.0f;
    }

    void update() override
    {
        lfo.setRate (knob (0), sampleRate);
        depth.set (knob (1));
        wave = juce::jlimit (0, 2, juce::roundToInt (knob (2)));
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float p = lfo.advance();
            const float w = wave == 1 ? triTurns (p) : wave == 2 ? softSquareTurns (p) : sinTurns (p);
            const float theta = (depth.next() * w + 1.0f) * (0.25f * kPi);
            gainL += gainCoeff * (kSqrt2 * std::cos (theta) - gainL);
            gainR += gainCoeff * (kSqrt2 * std::sin (theta) - gainR);
            left[i] *= gainL;
            right[i] *= gainR;
        }
    }

private:
    Lfo lfo;
    Smooth depth;
    float gainL = 1.0f, gainR = 1.0f, gainCoeff = 0.1f;
    int wave = 0;
};

//==============================================================================
// TWIN: stereo doubler; two drifting short-delay "takes" either side, or a Haas widener.
class Doubler final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        for (auto& l : lines) l.setMaxSamples ((int) (sampleRate * 0.06));
        for (auto& l : lfos) l.prepare (sampleRate);
        timeMs.prepare (sampleRate, 0.2f);
        drift.prepare (sampleRate, 0.05f);
        side.prepare (sampleRate, 0.05f);
        tone.prepare (sampleRate, 0.05f);
        mix.prepare (sampleRate, 0.03f);
        haas.prepare (sampleRate, 0.03f);
        wanderCoeff = dsp::onePoleCoeff (2.5f, sampleRate);
        wanderHold = (int) (sampleRate * 0.18);
    }

    void reset() override
    {
        for (auto& l : lines) l.clear();
        for (auto& t : toneLp) t.s = 0.0f;
        lfos[0].reset (0.0f);
        lfos[1].reset (0.4f);
        wanders[0].reset (0x1234567u);
        wanders[1].reset (0x89abcdeu);
        for (auto* s : { &timeMs, &drift, &side, &tone, &mix, &haas }) s->reset();
    }

    void update() override
    {
        lfos[0].setRate (0.29f, sampleRate);
        lfos[1].setRate (0.21f, sampleRate);
        timeMs.set (knob (0));
        drift.set (knob (1));
        side.set (knob (2));
        tone.set (dsp::onePoleCoeff (knob (3), sampleRate));
        mix.set (knob (4));
        haas.set (knob (5) > 0.5f ? 1.0f : 0.0f);
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        const float msToSamples = (float) sampleRate * 0.001f;

        for (int i = 0; i < n; ++i)
        {
            const float inL = left[i], inR = right[i];
            lines[0].push (inL);
            lines[1].push (inR);

            const float t = timeMs.next(), dr = drift.next(), w = side.next(), tc = tone.next(), m = mix.next(), h = haas.next();
            const float modA = dr * (0.9f * sinTurns (lfos[0].advance()) + 1.4f * wanders[0].next (wanderCoeff, wanderHold));
            const float modB = dr * (0.9f * sinTurns (lfos[1].advance()) + 1.4f * wanders[1].next (wanderCoeff, wanderHold));

            // Double: two takes at different, drifting times. Haas: one undrifted copy on the right.
            const float tA = t * 0.78f + modA;
            const float tB = t * (1.22f - 0.22f * h) + modB * (1.0f - h);
            const float wA = toneLp[0].process (lines[0].read (juce::jmax (2.0f, tA * msToSamples)), tc);
            const float wB = toneLp[1].process (lines[1].read (juce::jmax (2.0f, tB * msToSamples)), tc);

            float effL = 0.72f * (inL + wA);
            float effR = 0.72f * (inR + wB);
            const float mid = 0.5f * (effL + effR);
            const float sideSig = 0.5f * (effL - effR) * w * 1.4f;
            effL = mid + sideSig;
            effR = mid - sideSig;

            const float haasR = inR + w * (wB - inR);
            effL += h * (inL - effL);
            effR += h * (haasR - effR);

            left[i]  = (1.0f - m) * inL + m * effL;
            right[i] = (1.0f - m) * inR + m * effR;
        }
    }

private:
    std::array<dsp::DelayBuffer, 2> lines;
    std::array<OnePoleLp, 2> toneLp;
    std::array<Lfo, 2> lfos;
    std::array<Wander, 2> wanders;
    Smooth timeMs, drift, side, tone, mix, haas;
    float wanderCoeff = 0.001f;
    int wanderHold = 8000;
};

//==============================================================================
// DRIFT: tape-style wow and flutter with an ageing (darkening, saturating) control.
class Warble final : public Pedal
{
public:
    void prepared() override
    {
        wanderCoeff = dsp::onePoleCoeff (3.0f, sampleRate);
        for (auto& c : channels)
        {
            c.line.setMaxSamples ((int) (sampleRate * 0.03));
            c.wow.prepare (sampleRate);
            c.flutterA.prepare (sampleRate);
            c.flutterB.prepare (sampleRate);
            c.wowDepth.prepare (sampleRate, 0.1f);
            c.flutterDepth.prepare (sampleRate, 0.1f);
            c.age.prepare (sampleRate, 0.05f);
            c.mix.prepare (sampleRate, 0.03f);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.line.clear();
            c.wow.reset (0.0f);
            c.flutterA.reset (0.0f);
            c.flutterB.reset (0.3f);
            c.wander.reset (0x5eedu);
            c.toneLp.s = 0.0f;
            for (auto* s : { &c.wowDepth, &c.flutterDepth, &c.age, &c.mix }) s->reset();
        }
    }

    void update() override
    {
        wanderHold = (int) (sampleRate / juce::jmax (0.1f, knob (2)) * 0.5);
        for (auto& c : channels)
        {
            c.wowDepth.set (knob (0));
            c.flutterDepth.set (knob (1));
            c.wow.setRate (knob (2), sampleRate);
            c.flutterA.setRate (9.3f, sampleRate);
            c.flutterB.setRate (14.1f, sampleRate);
            c.age.set (knob (3));
            c.mix.set (knob (4));
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];
        const float msToSamples = (float) sampleRate * 0.001f;
        const float sr = (float) sampleRate;
        constexpr float wowMaxMs = 4.0f, flutterMaxMs = 0.18f;
        constexpr float centreMs = wowMaxMs + flutterMaxMs + 0.5f;

        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            c.line.push (dry);

            const float wowShape = 0.7f * sinTurns (c.wow.advance()) + 0.45f * c.wander.next (wanderCoeff, wanderHold);
            const float flutterShape = 0.65f * sinTurns (c.flutterA.advance()) + 0.35f * sinTurns (c.flutterB.advance());
            const float delayMs = centreMs + c.wowDepth.next() * wowMaxMs * juce::jlimit (-1.0f, 1.0f, wowShape)
                                           + c.flutterDepth.next() * flutterMaxMs * flutterShape;

            float wet = c.line.read (juce::jmax (2.0f, delayMs * msToSamples));

            const float a = c.age.next();
            const float cutoff = 16000.0f * std::exp2 (-a * 3.8f);
            wet = c.toneLp.process (wet, 1.0f - std::exp (-kTwoPi * cutoff / sr));
            const float driveAmt = 1.0f + 2.0f * a;
            wet = softClip (wet * driveAmt) / driveAmt;

            const float m = c.mix.next();
            x[i] = blendMakeup (m, 0.6f) * ((1.0f - m) * dry + m * wet);
        }
    }

private:
    struct Channel
    {
        dsp::DelayBuffer line;
        Lfo wow, flutterA, flutterB;
        Wander wander;
        OnePoleLp toneLp;
        Smooth wowDepth, flutterDepth, age, mix;
    };

    std::array<Channel, 2> channels;
    float wanderCoeff = 0.001f;
    int wanderHold = 24000;
};

} // namespace

void registerModulationPedals (Registry& r)
{
    r.add<Chorus> ({ "mod.chorus", "LAGOON", "stereo chorus", "Modulation",
                     "Wide, watery chorus with up to three voices spread left to right",
                     juce::Colour (0xff2f7f86), juce::Colour (0xffeaf4ef),
                     { knob ("rate", "Rate", 0.05f, 5.0f, 0.7f, "Hz", 0.5f),
                       knob ("depth", "Depth", 0.0f, 1.0f, 0.5f, "%"),
                       knob ("delay", "Delay", 4.0f, 25.0f, 11.0f, "ms"),
                       choice ("voices", "Voices", { "One", "Two", "Three" }, 1),
                       knob ("tone", "Tone", 1500.0f, 12000.0f, 7000.0f, "Hz", 0.5f),
                       knob ("width", "Width", 0.0f, 1.0f, 0.85f, "%"),
                       knob ("mix", "Mix", 0.0f, 1.0f, 0.5f, "%") } });

    r.add<Flanger> ({ "mod.flanger", "COMET", "flanger", "Modulation",
                      "Jet-plane sweeps, with a through-zero mode that cancels as it crosses",
                      juce::Colour (0xff3b2f63), juce::Colour (0xfff0e6c8),
                      { knob ("rate", "Rate", 0.03f, 5.0f, 0.25f, "Hz", 0.4f),
                        knob ("depth", "Depth", 0.0f, 1.0f, 0.75f, "%"),
                        knob ("manual", "Manual", 0.25f, 5.0f, 0.6f, "ms", 0.5f),
                        knob ("feedback", "Feedback", -0.92f, 0.92f, 0.5f, "%"),
                        knob ("mix", "Mix", 0.0f, 1.0f, 0.5f, "%"),
                        choice ("mode", "Mode", { "Classic", "Thru-zero" }, 0) } });

    r.add<Phaser> ({ "mod.phaser", "WHORL", "phaser", "Modulation",
                     "Swirling stereo phaser from a gentle 4-stage to a deep 12-stage",
                     juce::Colour (0xffd9772b), juce::Colour (0xff1f1a17),
                     { knob ("rate", "Rate", 0.03f, 8.0f, 0.4f, "Hz", 0.4f),
                       knob ("depth", "Depth", 0.0f, 1.0f, 0.8f, "%"),
                       knob ("center", "Center", 150.0f, 3000.0f, 700.0f, "Hz", 0.4f),
                       knob ("feedback", "Feedback", 0.0f, 0.9f, 0.3f, "%"),
                       choice ("stages", "Stages", { "4", "6", "8", "12" }, 0),
                       knob ("spread", "Spread", 0.0f, 1.0f, 0.5f, "%"),
                       knob ("mix", "Mix", 0.0f, 1.0f, 0.5f, "%") } });

    r.add<Tremolo> ({ "mod.tremolo", "SHIMMY", "tremolo", "Modulation",
                      "Classic volume pulse, from smooth sine to choppy square",
                      juce::Colour (0xffa9d8c4), juce::Colour (0xff1d3a33),
                      { knob ("rate", "Rate", 0.5f, 15.0f, 4.5f, "Hz", 0.6f),
                        knob ("depth", "Depth", 0.0f, 1.0f, 0.6f, "%"),
                        choice ("wave", "Wave", { "Sine", "Triangle", "Square", "Bias" }, 0),
                        knob ("level", "Level", -6.0f, 6.0f, 0.0f, "dB") } });

    r.add<HarmonicTremolo> ({ "mod.harmtrem", "TIDEPOOL", "harmonic tremolo", "Modulation",
                              "Lows and highs swell in turn for a rolling, phasey pulse",
                              juce::Colour (0xff1f3550), juce::Colour (0xffe9c46a),
                              { knob ("rate", "Rate", 0.5f, 12.0f, 3.8f, "Hz", 0.6f),
                                knob ("depth", "Depth", 0.0f, 1.0f, 0.7f, "%"),
                                knob ("split", "Split", 200.0f, 2500.0f, 650.0f, "Hz", 0.5f),
                                knob ("level", "Level", -6.0f, 6.0f, 0.0f, "dB") } });

    r.add<Vibrato> ({ "mod.vibrato", "QUIVER", "vibrato", "Modulation",
                      "Pure pitch wobble, from a subtle shimmer to a seasick bend",
                      juce::Colour (0xff7b3b5e), juce::Colour (0xfff6e7ee),
                      { knob ("rate", "Rate", 0.5f, 12.0f, 5.5f, "Hz", 0.6f),
                        knob ("depth", "Depth", 0.0f, 1.0f, 0.35f, "%"),
                        choice ("wave", "Wave", { "Sine", "Triangle" }, 0) } });

    r.add<Rotary> ({ "mod.rotary", "CAROUSEL", "rotary speaker", "Modulation",
                     "Spinning horn and drum that spool up and coast down between speeds",
                     juce::Colour (0xff5a3a22), juce::Colour (0xfff2dfb6),
                     { choice ("speed", "Speed", { "Brake", "Slow", "Fast" }, 1),
                       knob ("ramp", "Ramp", 0.25f, 3.0f, 1.0f, "x", 0.6f),
                       knob ("balance", "Horn", 0.0f, 1.0f, 0.5f, "%"),
                       knob ("drive", "Drive", 0.0f, 1.0f, 0.15f, "%"),
                       knob ("width", "Width", 0.0f, 1.0f, 0.8f, "%"),
                       knob ("mix", "Mix", 0.0f, 1.0f, 1.0f, "%") } });

    r.add<Throb> ({ "mod.throb", "MOTH", "photocell throb", "Modulation",
                    "Lamp-lit, lopsided throb that pulses between chorus and vibrato",
                    juce::Colour (0xff6b6a3a), juce::Colour (0xfff4ecd0),
                    { knob ("speed", "Speed", 0.3f, 12.0f, 1.8f, "Hz", 0.5f),
                      knob ("intensity", "Intensity", 0.0f, 1.0f, 0.75f, "%"),
                      choice ("mode", "Mode", { "Chorus", "Vibrato" }, 0),
                      knob ("level", "Level", -6.0f, 6.0f, 0.0f, "dB") } });

    r.add<RingMod> ({ "mod.ring", "ORBIT", "ring modulator", "Modulation",
                      "Clangorous, bell-like ring modulation with a sweepable carrier",
                      juce::Colour (0xffc9ccd1), juce::Colour (0xff22252b),
                      { knob ("freq", "Freq", 20.0f, 2000.0f, 330.0f, "Hz", 0.35f),
                        knob ("mix", "Mix", 0.0f, 1.0f, 0.5f, "%"),
                        choice ("sweep", "Sweep", { "Off", "LFO", "Envelope" }, 0),
                        knob ("amount", "Amount", 0.0f, 1.0f, 0.4f, "%"),
                        knob ("rate", "Rate", 0.1f, 10.0f, 1.0f, "Hz", 0.5f),
                        knob ("tone", "Tone", 800.0f, 12000.0f, 6000.0f, "Hz", 0.5f) } });

    r.add<AutoPan> ({ "mod.autopan", "SWAY", "auto-pan", "Modulation",
                      "Sends your guitar drifting from speaker to speaker",
                      juce::Colour (0xffe7856f), juce::Colour (0xff2b1a17),
                      { knob ("rate", "Rate", 0.05f, 10.0f, 0.8f, "Hz", 0.5f),
                        knob ("depth", "Depth", 0.0f, 1.0f, 0.75f, "%"),
                        choice ("wave", "Wave", { "Sine", "Triangle", "Square" }, 0) } });

    r.add<Doubler> ({ "mod.doubler", "TWIN", "doubler", "Modulation",
                      "Turns one take into a wide double-tracked pair",
                      juce::Colour (0xffbcd3e6), juce::Colour (0xff1c2b3a),
                      { knob ("time", "Time", 5.0f, 40.0f, 16.0f, "ms"),
                        knob ("drift", "Drift", 0.0f, 1.0f, 0.35f, "%"),
                        knob ("width", "Width", 0.0f, 1.0f, 0.8f, "%"),
                        knob ("tone", "Tone", 1500.0f, 12000.0f, 7500.0f, "Hz", 0.5f),
                        knob ("mix", "Mix", 0.0f, 1.0f, 0.7f, "%"),
                        choice ("mode", "Mode", { "Double", "Haas" }, 0) } });

    r.add<Warble> ({ "mod.warble", "DRIFT", "tape warble", "Modulation",
                     "Seasick wow and flutter from a tired old tape machine",
                     juce::Colour (0xffd6b24a), juce::Colour (0xff2a2410),
                     { knob ("wow", "Wow", 0.0f, 1.0f, 0.35f, "%"),
                       knob ("flutter", "Flutter", 0.0f, 1.0f, 0.2f, "%"),
                       knob ("rate", "Rate", 0.2f, 3.0f, 0.7f, "Hz", 0.6f),
                       knob ("age", "Age", 0.0f, 1.0f, 0.3f, "%"),
                       knob ("mix", "Mix", 0.0f, 1.0f, 1.0f, "%") } });
}

} // namespace woodshed
