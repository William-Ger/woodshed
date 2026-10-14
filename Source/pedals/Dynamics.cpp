#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// Shared helpers for the dynamics family.

/** One-pole coefficient for a time constant in milliseconds (0 = instant). */
inline float msCoeff (float ms, double sr)
{
    if (ms <= 0.0f) return 1.0f;
    return 1.0f - std::exp (-1.0f / (ms * 0.001f * (float) sr));
}

inline float gainToDbFast (float g)                     { return 20.0f * std::log10 (juce::jmax (g, 1.0e-6f)); }
inline float dbToGainFast (float db)                    { return std::exp (db * 0.11512925f); }   // ln(10)/20

/** Attack/release envelope follower on a rectified signal. */
struct EnvFollower
{
    float env = 0.0f, att = 1.0f, rel = 1.0f;

    void setup (float attackMs, float releaseMs, double sr)    { att = msCoeff (attackMs, sr); rel = msCoeff (releaseMs, sr); }
    float process (float rectified)
    {
        env += (rectified > env ? att : rel) * (rectified - env);
        return env;
    }
};

using LinearSmooth = juce::SmoothedValue<float>;

/** Supply-rail style safety: linear up to 2.0 (+6 dBFS), then a smooth knee that never exceeds 3.0. */
inline float railClip (float v)
{
    const float a = std::abs (v);
    if (a <= 2.0f) return v;
    const float over = a - 2.0f;
    return std::copysign (2.0f + over / (1.0f + over), v);
}

//==============================================================================
// HUSH: noise gate with hysteresis and a short hold, so sustained notes don't chatter.
class NoiseGate final : public Pedal
{
public:
    void reset() override                       { for (auto& s : state) s = {}; }

    void prepared() override
    {
        envRelease = std::exp (-1.0f / (0.010f * (float) sampleRate));
        openCoeff = 1.0f - std::exp (-1.0f / (0.0008f * (float) sampleRate));
        holdSamples = (int) (0.015 * sampleRate);
    }

    void update() override
    {
        openThreshold = dsp::dbToGain (knob (0));
        closeThreshold = openThreshold * 0.5f;    // 6 dB of hysteresis
        closeCoeff = 1.0f - std::exp (-1.0f / (knob (1) * 0.001f * (float) sampleRate));
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& s = state[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            s.env = juce::jmax (std::abs (x[i]), s.env * envRelease);

            if (s.env > openThreshold)       { s.open = true; s.hold = holdSamples; }
            else if (s.env < closeThreshold && --s.hold <= 0) s.open = false;

            const float target = s.open ? 1.0f : 0.0f;
            s.gain += (s.open ? openCoeff : closeCoeff) * (target - s.gain);
            x[i] *= s.gain;
        }
    }

private:
    struct State { float env = 0.0f, gain = 0.0f; bool open = false; int hold = 0; };
    std::array<State, 2> state;
    float envRelease = 0.0f, openCoeff = 0.0f, closeCoeff = 0.0f;
    float openThreshold = 0.0f, closeThreshold = 0.0f;
    int holdSamples = 0;
};

//==============================================================================
// MOLASSES: optical-style compressor. RMS-ish detector, wide soft knee, and a
// two-stage release whose slow tail grows the longer the cell has been "lit",
// so it breathes like a light-dependent resistor rather than pumping.
class Compressor final : public Pedal
{
public:
    void prepared() override
    {
        detCoeff = msCoeff (4.0f, sampleRate);
        fastRelease = msCoeff (70.0f, sampleRate);
        slowRelease = msCoeff (900.0f, sampleRate);
        memoryCoeff = msCoeff (600.0f, sampleRate);

        for (auto& c : channels)
        {
            c.makeup.reset (sampleRate, 0.05);
            c.blend.reset (sampleRate, 0.03);
            c.threshold.reset (sampleRate, 0.05);
            c.ratio.reset (sampleRate, 0.05);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.power = 0.0f;
            c.grDb = 0.0f;
            c.memory = 0.0f;
        }
    }

    void update() override
    {
        const float sustain = knob (0);
        attackCoeff = msCoeff (knob (1), sampleRate);

        const float thresholdDb = -12.0f - 30.0f * sustain;
        const float ratio = 1.5f + 8.5f * sustain * sustain;

        // Auto makeup: about half the static gain reduction a typical note (-20 dBFS RMS)
        // would see, which keeps the compressor level with bypass across the Sustain range.
        const float makeupDb = juce::jmin (14.0f, 0.55f * staticGr (-20.0f, thresholdDb, ratio));

        for (auto& c : channels)
        {
            c.threshold.setTargetValue (thresholdDb);
            c.ratio.setTargetValue (ratio);
            c.makeup.setTargetValue (dsp::dbToGain (makeupDb + knob (2)));
            c.blend.setTargetValue (knob (3));
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            c.power += detCoeff * (dry * dry - c.power);
            const float levelDb = 10.0f * std::log10 (c.power + 1.0e-12f);

            const float target = staticGr (levelDb, c.threshold.getNextValue(), c.ratio.getNextValue());

            if (target > c.grDb)
                c.grDb += attackCoeff * (target - c.grDb);
            else
            {
                // Mostly fast release after a short squeeze, slow "glow" after a long one.
                const float m = juce::jlimit (0.0f, 1.0f, c.memory * (1.0f / 8.0f));
                c.grDb += (fastRelease + m * (slowRelease - fastRelease)) * (target - c.grDb);
            }

            c.memory += memoryCoeff * (c.grDb - c.memory);

            const float wet = dry * dbToGainFast (-c.grDb) * c.makeup.getNextValue();
            const float b = c.blend.getNextValue();
            x[i] = railClip (wet * b + dry * (1.0f - b));
        }
    }

private:
    /** Gain reduction in dB (positive) for a level, with a 14 dB soft knee. */
    static float staticGr (float levelDb, float thresholdDb, float ratio)
    {
        constexpr float knee = 14.0f;
        const float over = levelDb - thresholdDb;
        const float slope = 1.0f - 1.0f / ratio;

        if (over <= -0.5f * knee) return 0.0f;
        if (over >= 0.5f * knee)  return slope * over;

        const float t = over + 0.5f * knee;
        return slope * t * t / (2.0f * knee);
    }

    struct Channel
    {
        float power = 0.0f, grDb = 0.0f, memory = 0.0f;
        LinearSmooth threshold { -27.0f }, ratio { 4.0f }, blend { 1.0f };
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> makeup { 1.0f };
    };

    std::array<Channel, 2> channels;
    float detCoeff = 0.0f, attackCoeff = 0.0f, fastRelease = 0.0f, slowRelease = 0.0f, memoryCoeff = 0.0f;
};

//==============================================================================
// CEILING: look-ahead peak limiter. A 1.5 ms look-ahead with a held, double-smoothed
// gain curve, then a hard safety clamp at the ceiling for anything that slips past.
class Limiter final : public Pedal
{
public:
    void prepared() override
    {
        lookahead = juce::jmax (4, (int) std::round (0.0015 * sampleRate));
        smoothCoeff = msCoeff (0.0015f * 1000.0f / 3.0f, sampleRate);

        for (auto& c : channels)
        {
            c.delay.setMaxSamples (lookahead + 8);
            c.drive.reset (sampleRate, 0.03);
            c.ceiling.reset (sampleRate, 0.03);
            c.level.reset (sampleRate, 0.03);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.delay.clear();
            c.held = c.s1 = c.s2 = 1.0f;
            c.holdCount = 0;
        }
    }

    int getLatencySamples() const override      { return lookahead; }

    void update() override
    {
        releaseCoeff = msCoeff (knob (2), sampleRate);

        for (auto& c : channels)
        {
            c.drive.setTargetValue (dsp::dbToGain (knob (0)));
            c.ceiling.setTargetValue (dsp::dbToGain (knob (1)));
            c.level.setTargetValue (dsp::dbToGain (knob (3)));
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float in = x[i] * c.drive.getNextValue();
            const float ceil = c.ceiling.getNextValue();
            const float a = std::abs (in);
            const float need = a > ceil ? ceil / a : 1.0f;

            // Hold the deepest required gain for the look-ahead window, then release.
            if (need <= c.held)                  { c.held = need; c.holdCount = lookahead; }
            else
            {
                if (c.holdCount > 0) --c.holdCount;
                if (c.holdCount == 0) c.held += releaseCoeff * (need - c.held);
            }

            // Two cascaded instant-attack/smoothed stages give a click-free curve that
            // has (nearly) arrived by the time the delayed peak comes out.
            c.s1 = juce::jmin (c.held, c.s1 + smoothCoeff * (c.held - c.s1));
            c.s2 = juce::jmin (c.s1, c.s2 + smoothCoeff * (c.s1 - c.s2));

            c.delay.push (in);
            const float delayed = c.delay.data[(size_t) ((c.delay.writePos - 1 - lookahead) & c.delay.mask)];
            const float y = juce::jlimit (-ceil, ceil, delayed * c.s2);
            x[i] = y * c.level.getNextValue();
        }
    }

private:
    struct Channel
    {
        dsp::DelayBuffer delay;
        float held = 1.0f, s1 = 1.0f, s2 = 1.0f;
        int holdCount = 0;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> drive { 1.0f }, ceiling { 0.25f }, level { 1.0f };
    };

    std::array<Channel, 2> channels;
    int lookahead = 72;
    float smoothCoeff = 0.5f, releaseCoeff = 0.01f;
};

//==============================================================================
// LIFT: clean boost with a gentle tilt EQ around 700 Hz and a voice switch.
// Rails-style soft ceiling only engages far above normal levels.
class CleanBoost final : public Pedal
{
public:
    void prepared() override
    {
        tiltCoeff = dsp::onePoleCoeff (700.0f, sampleRate);
        fatCoeff = dsp::onePoleCoeff (140.0f, sampleRate);

        for (auto& c : channels)
        {
            c.lowGain.reset (sampleRate, 0.04);
            c.highGain.reset (sampleRate, 0.04);
            c.fat.reset (sampleRate, 0.04);
            c.tight.reset (sampleRate, 0.04);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
            c.tiltLp = c.fatLp = 0.0f;
    }

    void update() override
    {
        const float boost = dsp::dbToGain (knob (0));
        const float tiltDb = 4.0f * knob (1);
        const int voice = juce::roundToInt (knob (2));

        for (auto& c : channels)
        {
            c.lowGain.setTargetValue (boost * dsp::dbToGain (-tiltDb));
            c.highGain.setTargetValue (boost * dsp::dbToGain (tiltDb));
            c.fat.setTargetValue (voice == 1 ? 0.41f : 0.0f);      // ~ +3 dB low shelf
            c.tight.setTargetValue (voice == 2 ? 0.6f : 0.0f);     // ~ -8 dB below 140 Hz
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            float v = x[i];

            c.fatLp += fatCoeff * (v - c.fatLp);
            v += c.fatLp * (c.fat.getNextValue() - c.tight.getNextValue());

            c.tiltLp += tiltCoeff * (v - c.tiltLp);
            v = c.tiltLp * c.lowGain.getNextValue() + (v - c.tiltLp) * c.highGain.getNextValue();

            x[i] = railClip (v);
        }
    }

private:

    struct Channel
    {
        float tiltLp = 0.0f, fatLp = 0.0f;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> lowGain { 1.0f }, highGain { 1.0f };
        LinearSmooth fat, tight;
    };

    std::array<Channel, 2> channels;
    float tiltCoeff = 0.1f, fatCoeff = 0.02f;
};

//==============================================================================
// TIDE: auto-swell. A fast/slow envelope pair spots each pick attack a few ms ahead
// of the (delayed) audio, ducks the volume to silence just before the pick lands and
// fades it back in, so every note blooms like a bowed string.
class AutoSwell final : public Pedal
{
public:
    void prepared() override
    {
        lookahead = juce::jmax (4, (int) std::round (0.003 * sampleRate));

        for (auto& c : channels)
        {
            c.fast.setup (0.3f, 25.0f, sampleRate);
            c.slow.setup (40.0f, 250.0f, sampleRate);
            c.delay.setMaxSamples (lookahead + 8);
            c.level.reset (sampleRate, 0.03);
        }

        gateAttack = msCoeff (1.0f, sampleRate);
        gateRelease = msCoeff (80.0f, sampleRate);
        duckCoeff = msCoeff (0.6f, sampleRate);
        rearmSamples = (int) (0.06 * sampleRate);
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.fast.env = c.slow.env = 0.0f;
            c.delay.clear();
            c.phase = 1.0f;
            c.duck = 1.0f;
            c.gate = 0.0f;
            c.rearm = 0;
            c.ducking = false;
        }
    }

    int getLatencySamples() const override      { return lookahead; }

    void update() override
    {
        threshold = dsp::dbToGain (knob (0));
        phaseStep = 1.0f / juce::jmax (1.0f, knob (1) * 0.001f * (float) sampleRate);
        curve = juce::roundToInt (knob (2));

        for (auto& c : channels)
            c.level.setTargetValue (dsp::dbToGain (knob (3)));
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float a = std::abs (x[i]);
            const float f = c.fast.process (a);
            const float s = c.slow.process (a);

            if (c.rearm > 0) --c.rearm;

            // A new pick: fast envelope above threshold and jumping well above the slow one.
            if (c.rearm == 0 && f > threshold && f > 2.2f * s + 1.0e-5f)
            {
                c.ducking = true;
                c.rearm = rearmSamples;
            }

            float swell;
            if (c.ducking)
            {
                // Pull the current swell down to silence inside the look-ahead window.
                c.duck += duckCoeff * (0.0f - c.duck);
                swell = c.duck * shape (c.phase);
                if (c.duck < 0.002f)
                {
                    c.ducking = false;
                    c.duck = 1.0f;
                    c.phase = 0.0f;
                    swell = 0.0f;
                }
            }
            else
            {
                c.phase = juce::jmin (1.0f, c.phase + phaseStep);
                swell = shape (c.phase);
            }

            // Between notes the gate closes so the next note also starts from silence.
            const float gateTarget = f > threshold * 0.5f ? 1.0f : 0.0f;
            c.gate += (gateTarget > c.gate ? gateAttack : gateRelease) * (gateTarget - c.gate);

            c.delay.push (x[i]);
            const float delayed = c.delay.data[(size_t) ((c.delay.writePos - 1 - lookahead) & c.delay.mask)];
            x[i] = delayed * swell * c.gate * c.level.getNextValue() * makeup;
        }
    }

private:
    float shape (float p) const
    {
        switch (curve)
        {
            case 0:  return p;                                          // linear
            case 2:  return p * p * p;                                  // slow bloom
            default: return p * p * (3.0f - 2.0f * p);                  // smooth S
        }
    }

    struct Channel
    {
        EnvFollower fast, slow;
        dsp::DelayBuffer delay;
        float phase = 1.0f, duck = 1.0f, gate = 0.0f;
        int rearm = 0;
        bool ducking = false;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> level { 1.0f };
    };

    std::array<Channel, 2> channels;
    float threshold = 0.003f, phaseStep = 0.0001f, gateAttack = 0.1f, gateRelease = 0.001f, duckCoeff = 0.03f;
    int curve = 1, rearmSamples = 2880, lookahead = 144;

    // Swelled notes lose their loudest part; this brings a swelled phrase back to bypass loudness.
    static constexpr float makeup = 1.9f;
};

//==============================================================================
// CHISEL: transient shaper. Two envelope pairs: one differs only in attack speed
// (measures the pick), one differs only in release speed (measures the ring-out).
class TransientShaper final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.attackFast.setup (0.5f, 120.0f, sampleRate);
            c.attackSlow.setup (20.0f, 120.0f, sampleRate);
            c.sustainFast.setup (1.0f, 40.0f, sampleRate);
            c.sustainSlow.setup (1.0f, 400.0f, sampleRate);
            c.attackAmt.reset (sampleRate, 0.03);
            c.sustainAmt.reset (sampleRate, 0.03);
            c.level.reset (sampleRate, 0.03);
        }

        gainSmooth = msCoeff (1.0f, sampleRate);
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.attackFast.env = c.attackSlow.env = c.sustainFast.env = c.sustainSlow.env = 0.0f;
            c.gainDb = 0.0f;
        }
    }

    void update() override
    {
        for (auto& c : channels)
        {
            c.attackAmt.setTargetValue (knob (0));
            c.sustainAmt.setTargetValue (knob (1));
            c.level.setTargetValue (dsp::dbToGain (knob (2)));
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float a = std::abs (x[i]) + 1.0e-6f;
            const float atkDiff = gainToDbFast (c.attackFast.process (a)) - gainToDbFast (c.attackSlow.process (a));
            const float susDiff = gainToDbFast (c.sustainSlow.process (a)) - gainToDbFast (c.sustainFast.process (a));

            const float target = juce::jlimit (-15.0f, 15.0f,
                                               c.attackAmt.getNextValue() * juce::jlimit (0.0f, 15.0f, atkDiff)
                                             + c.sustainAmt.getNextValue() * 0.8f * juce::jlimit (0.0f, 15.0f, susDiff));
            c.gainDb += gainSmooth * (target - c.gainDb);
            x[i] = railClip (x[i] * dbToGainFast (c.gainDb) * c.level.getNextValue());
        }
    }

private:
    struct Channel
    {
        EnvFollower attackFast, attackSlow, sustainFast, sustainSlow;
        float gainDb = 0.0f;
        LinearSmooth attackAmt, sustainAmt;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> level { 1.0f };
    };

    std::array<Channel, 2> channels;
    float gainSmooth = 0.02f;
};

} // namespace

void registerDynamicsPedals (Registry& r)
{
    r.add<NoiseGate> ({ "dyn.gate", "HUSH", "noise gate", "Dynamics",
                        "Silences hum and hiss between notes",
                        juce::Colour (0xff3e4c55), juce::Colour (0xffe8eee9),
                        { knob ("threshold", "Threshold", -90.0f, -20.0f, -62.0f, "dB"),
                          knob ("release", "Release", 10.0f, 500.0f, 90.0f, "ms", 0.5f) } });

    r.add<Compressor> ({ "dyn.compressor", "MOLASSES", "compressor", "Dynamics",
                         "Slow-glowing optical squeeze for country snap and endless sustain",
                         juce::Colour (0xff7a4a1e), juce::Colour (0xfff6e3c4),
                         { knob ("sustain", "Sustain", 0.0f, 1.0f, 0.5f, "%"),
                           knob ("attack", "Attack", 0.5f, 60.0f, 8.0f, "ms", 0.5f),
                           knob ("level", "Level", -18.0f, 8.0f, 0.0f, "dB"),
                           knob ("blend", "Blend", 0.0f, 1.0f, 1.0f, "%") } });

    r.add<Limiter> ({ "dyn.limiter", "CEILING", "limiter", "Dynamics",
                      "Look-ahead brickwall that pins your peaks without pumping",
                      juce::Colour (0xffd8dde0), juce::Colour (0xff1c2a36),
                      { knob ("drive", "Drive", 0.0f, 24.0f, 3.0f, "dB"),
                        knob ("ceiling", "Ceiling", -30.0f, 0.0f, -14.0f, "dB"),
                        knob ("release", "Release", 5.0f, 800.0f, 80.0f, "ms", 0.5f),
                        knob ("level", "Level", -12.0f, 6.0f, -1.5f, "dB") } });

    r.add<CleanBoost> ({ "dyn.boost", "LIFT", "clean boost", "Dynamics",
                         "Up to 20 dB of clean push with a gentle bright/dark tilt",
                         juce::Colour (0xfff2c230), juce::Colour (0xff2a2208),
                         { knob ("boost", "Boost", 0.0f, 20.0f, 1.0f, "dB"),
                           knob ("tilt", "Tilt", -1.0f, 1.0f, 0.15f, "%"),
                           choice ("voice", "Voice", { "Flat", "Fat", "Tight" }, 0) } });

    r.add<AutoSwell> ({ "dyn.swell", "TIDE", "auto-swell", "Dynamics",
                        "Fades every picked note in from silence for bowed, violin-like swells",
                        juce::Colour (0xff2f6f73), juce::Colour (0xffe6f2ee),
                        { knob ("sense", "Sense", -66.0f, -24.0f, -48.0f, "dB"),
                          knob ("rise", "Rise", 20.0f, 2000.0f, 220.0f, "ms", 0.45f),
                          choice ("curve", "Curve", { "Linear", "Smooth", "Bloom" }, 1),
                          knob ("level", "Level", -12.0f, 6.0f, 0.0f, "dB") } });

    r.add<TransientShaper> ({ "dyn.transient", "CHISEL", "transient shaper", "Dynamics",
                              "Sharpens or softens the pick and stretches or trims the ring-out",
                              juce::Colour (0xff4a3b6b), juce::Colour (0xffeee6ff),
                              { knob ("attack", "Attack", -1.0f, 1.0f, 0.25f, "%"),
                                knob ("sustain", "Sustain", -1.0f, 1.0f, 0.0f, "%"),
                                knob ("level", "Level", -12.0f, 12.0f, 0.0f, "dB") } });
}

} // namespace woodshed
