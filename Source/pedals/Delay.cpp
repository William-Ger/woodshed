#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// ECHO: analog-flavoured delay; repeats darken as they feed back.
class AnalogDelay final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.line.setMaxSamples ((int) (sampleRate * 2.1));
            c.time.reset (sampleRate, 0.12);
            c.feedback.reset (sampleRate, 0.03);
            c.mix.reset (sampleRate, 0.03);
        }

        damping = dsp::onePoleCoeff (3800.0f, sampleRate);
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.line.clear();
            c.lowPass = 0.0f;
        }
    }

    void update() override
    {
        for (auto& c : channels)
        {
            c.time.setTargetValue (juce::jmax (2.0f, knob (0) * 0.001f * (float) sampleRate));
            c.feedback.setTargetValue (knob (1));
            c.mix.setTargetValue (knob (2));
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            const float wet = c.line.read (c.time.getNextValue());
            c.lowPass += damping * (wet - c.lowPass);
            c.line.push (dry + c.feedback.getNextValue() * c.lowPass);
            x[i] = dry + c.mix.getNextValue() * wet;
        }
    }

private:
    struct Channel
    {
        dsp::DelayBuffer line;
        juce::SmoothedValue<float> time, feedback, mix;
        float lowPass = 0.0f;
    };

    std::array<Channel, 2> channels;
    float damping = 0.3f;
};

//==============================================================================
// Small helpers for the delay family.

constexpr float pi = juce::MathConstants<float>::pi;
constexpr float twoPi = juce::MathConstants<float>::twoPi;

/** Linear below half the ceiling, then a smooth rational knee up to the ceiling. Slope is continuous. */
inline float softLimit (float x, float ceiling = 1.6f)
{
    const float knee = 0.5f * ceiling;
    const float a = std::abs (x);
    if (a <= knee) return x;
    const float over = (a - knee) / (ceiling - knee);
    return std::copysign (knee + (ceiling - knee) * over / (1.0f + over), x);
}

/** Pade tanh, clamped so it stays monotonic. */
inline float fastTanh (float x)
{
    x = juce::jlimit (-3.0f, 3.0f, x);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline float msToSamples (float ms, double sr)            { return ms * 0.001f * (float) sr; }

struct OnePoleLP
{
    float z = 0.0f, a = 1.0f;
    void setCutoff (float hz, double sr)                    { a = dsp::onePoleCoeff (juce::jlimit (5.0f, (float) sr * 0.45f, hz), sr); }
    void reset()                                            { z = 0.0f; }
    float process (float x)                                 { z += a * (x - z); return z; }
};

struct OnePoleHP
{
    OnePoleLP lp;
    void setCutoff (float hz, double sr)                    { lp.setCutoff (hz, sr); }
    void reset()                                            { lp.reset(); }
    float process (float x)                                 { return x - lp.process (x); }
};

/** Exponential parameter glide, snapped on the first update after reset(). */
struct Glide
{
    float value = 0.0f, target = 0.0f, coeff = 1.0f;
    bool snap = true;

    void setTime (float seconds, double sr)                 { coeff = 1.0f - std::exp (-1.0f / (seconds * (float) sr)); }
    void reset()                                            { snap = true; }
    void set (float t)                                      { target = t; if (snap) { value = t; snap = false; } }
    float next()                                            { value += coeff * (target - value); return value; }
};

/** Cheap deterministic noise in [-1, 1). */
struct Noise
{
    uint32_t state = 0x9e3779b9u;
    float next()
    {
        state = state * 1664525u + 1013904223u;
        return (float) (state >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
};

/** Sine oscillator as a rotating phasor (no per-sample trig). */
struct Lfo
{
    float c = 1.0f, s = 0.0f, rc = 1.0f, rs = 0.0f;

    void setPhase (float radians)                           { c = std::cos (radians); s = std::sin (radians); }
    void setRate (float hz, double sr)
    {
        const float w = twoPi * hz / (float) sr;
        rc = std::cos (w); rs = std::sin (w);
        const float mag = 1.0f / std::sqrt (c * c + s * s);   // renormalise once per block
        c *= mag; s *= mag;
    }
    float next()
    {
        const float nc = c * rc - s * rs;
        s = c * rs + s * rc;
        c = nc;
        return s;
    }
};

/** Pairs a delay line with its loop filters: low-pass tone, DC/low cut, soft limiter on the write. */
struct EchoLine
{
    dsp::DelayBuffer line;
    OnePoleLP tone;
    OnePoleHP lowCut;

    void reset()                                            { line.clear(); tone.reset(); lowCut.reset(); }
    float read (float delay) const                          { return line.read (juce::jmax (2.0f, delay)); }
    void write (float x)                                    { line.push (softLimit (lowCut.process (tone.process (x)))); }
};

//==============================================================================
// GLASS: clean digital delay with a note-division selector relative to Time.
class DigitalDelay final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.echo.line.setMaxSamples ((int) (sampleRate * 2.05));
            c.time.setTime (0.08f, sampleRate);
            c.feedback.setTime (0.02f, sampleRate);
            c.mix.setTime (0.02f, sampleRate);
        }
    }

    void reset() override
    {
        for (auto& c : channels) { c.echo.reset(); c.time.reset(); c.feedback.reset(); c.mix.reset(); }
    }

    void update() override
    {
        static constexpr float divisions[] { 1.0f, 0.75f, 0.5f, 1.0f / 3.0f };
        const float div = divisions[juce::jlimit (0, 3, (int) knob (3))];
        for (auto& c : channels)
        {
            c.time.set (msToSamples (knob (0) * div, sampleRate) - 1.0f);
            c.echo.tone.setCutoff (knob (4), sampleRate);
            c.echo.lowCut.setCutoff (40.0f, sampleRate);
            c.feedback.set (knob (1));
            c.mix.set (knob (2));
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];
        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            const float wet = c.echo.read (c.time.next());
            c.echo.write (dry + c.feedback.next() * wet);
            x[i] = dry + c.mix.next() * wet;
        }
    }

private:
    struct Channel { EchoLine echo; Glide time, feedback, mix; };
    std::array<Channel, 2> channels;
};

//==============================================================================
// SPOOL: tape echo. Up to three playback heads, wow & flutter, saturating record path, worn-head tone.
class TapeEcho final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.line.setMaxSamples ((int) (sampleRate * 1.1));
            c.time.setTime (0.25f, sampleRate);   // slow motor: pitch glide when Time moves
            c.fbG.setTime (0.02f, sampleRate);
            c.wetG.setTime (0.02f, sampleRate);
        }
        wowRate = 0.55f; flutterRate = 6.3f;
    }

    void reset() override
    {
        for (size_t ch = 0; ch < channels.size(); ++ch)
        {
            auto& c = channels[ch];
            c.line.clear(); c.tone.reset(); c.lowCut.reset(); c.drift = 0.0f; c.time.reset(); c.fbG.reset(); c.wetG.reset();
            c.wow.setPhase (ch == 0 ? 0.0f : 1.9f);
            c.flutter.setPhase (ch == 0 ? 0.0f : 0.7f);
            c.noise.state = ch == 0 ? 0x1234567u : 0x7654321u;
            for (auto& g : c.headGain) g.reset();
        }
    }

    void update() override
    {
        // Heads sit at 1/3, 2/3 and 3/3 of Time.
        static constexpr int heads[][3] { { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 1 }, { 1, 0, 1 }, { 1, 1, 1 } };
        const auto& sel = heads[juce::jlimit (0, 4, (int) knob (6))];
        const int count = sel[0] + sel[1] + sel[2];

        const float age = knob (5);
        driveGain = 0.7f + 2.6f * knob (4);
        makeup = 1.0f + 0.6f * knob (4);
        feedback = knob (1);
        mix = knob (2);
        const float wf = knob (3);
        wowDepth = wf * 0.0055f * (float) sampleRate / (twoPi * wowRate) * 0.6f;
        driftDepth = wf * 0.0055f * (float) sampleRate / (twoPi * 1.0f) * 0.4f;
        flutterDepth = wf * 0.0012f * (float) sampleRate / (twoPi * flutterRate);

        for (auto& c : channels)
        {
            c.time.set (msToSamples (knob (0), sampleRate));
            c.tone.setCutoff (9000.0f * std::pow (0.25f, age), sampleRate);     // 9 kHz fresh -> 2.25 kHz worn
            c.lowCut.setCutoff (50.0f + 130.0f * age, sampleRate);
            c.wow.setRate (wowRate, sampleRate);
            c.flutter.setRate (flutterRate, sampleRate);
            for (int h = 0; h < 3; ++h)
            {
                c.headGain[(size_t) h].setTime (0.02f, sampleRate);
                c.headGain[(size_t) h].set (sel[h] ? 1.0f / (float) count : 0.0f);
            }
        }
        outNorm = std::sqrt ((float) count);
        driftCoeff = dsp::onePoleCoeff (0.8f, sampleRate);
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];
        c.fbG.set (feedback);
        c.wetG.set (mix * makeup * outNorm);

        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            c.drift += driftCoeff * (c.noise.next() - c.drift);
            const float speed = wowDepth * c.wow.next() + driftDepth * 60.0f * c.drift + flutterDepth * c.flutter.next();
            const float t = c.time.next();

            float play = 0.0f;
            for (int h = 0; h < 3; ++h)
            {
                const float frac = (float) (h + 1) / 3.0f;
                play += c.headGain[(size_t) h].next() * c.line.read (juce::jmax (2.0f, frac * (t + speed) - 1.0f));
            }

            // Record path: saturating, small-signal unity, worn-head filtering.
            const float rec = c.lowCut.process (c.tone.process (dry + c.fbG.next() * play));
            c.line.push (fastTanh (driveGain * rec) / driveGain);

            x[i] = dry + c.wetG.next() * play;
        }
    }

private:
    struct Channel
    {
        dsp::DelayBuffer line;
        Glide time, fbG, wetG;
        std::array<Glide, 3> headGain;
        OnePoleLP tone;
        OnePoleHP lowCut;
        Lfo wow, flutter;
        Noise noise;
        float drift = 0.0f;
    };

    std::array<Channel, 2> channels;
    float driveGain = 1.0f, makeup = 1.0f, feedback = 0.0f, mix = 0.0f, outNorm = 1.0f;
    float wowRate = 0.55f, flutterRate = 6.3f, wowDepth = 0.0f, driftDepth = 0.0f, flutterDepth = 0.0f, driftCoeff = 0.0f;
};

//==============================================================================
// VOLLEY: stereo ping-pong. Repeats bounce left, right, left...
class PingPongDelay final : public Pedal
{
public:
    bool producesStereo() const override                    { return true; }

    void prepared() override
    {
        for (auto& e : echoes) e.line.setMaxSamples ((int) (sampleRate * 1.55));
        time.setTime (0.08f, sampleRate);
        feedback.reset (sampleRate, 0.03);
        mix.reset (sampleRate, 0.03);
        width.reset (sampleRate, 0.03);
    }

    void reset() override
    {
        for (auto& e : echoes) e.reset();
        time.reset();
        feedback.setCurrentAndTargetValue (knob (1));
        mix.setCurrentAndTargetValue (knob (2));
        width.setCurrentAndTargetValue (knob (4));
    }

    void update() override
    {
        time.set (msToSamples (knob (0), sampleRate) - 1.0f);
        feedback.setTargetValue (knob (1));
        mix.setTargetValue (knob (2));
        width.setTargetValue (knob (4));
        for (auto& e : echoes)
        {
            e.tone.setCutoff (knob (3), sampleRate);
            e.lowCut.setCutoff (60.0f, sampleRate);
        }
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float d = time.next();
            const float eL = echoes[0].read (d);
            const float eR = echoes[1].read (d);
            const float in = 0.5f * (left[i] + right[i]);

            echoes[0].write (in + feedback.getNextValue() * eR);
            echoes[1].write (eL);

            const float w = width.getNextValue();
            const float a = 0.5f * (1.0f + w), b = 0.5f * (1.0f - w);
            const float m = mix.getNextValue() * 1.25f;   // repeats are split across two sides
            left[i]  += m * (a * eL + b * eR);
            right[i] += m * (b * eL + a * eR);
        }
    }

private:
    std::array<EchoLine, 2> echoes;
    Glide time;
    juce::SmoothedValue<float> feedback, mix, width;
};

//==============================================================================
// MIRAGE: reverse delay. Each chunk of the last Time is played backwards once; consecutive
// chunks are joined with short equal-power crossfades, so there are no clicks or doubled notes.
class ReverseDelay final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.echo.line.setMaxSamples ((int) (sampleRate * 3.2));
            c.chunk.setTime (0.1f, sampleRate);
            c.fbG.setTime (0.02f, sampleRate);
            c.wetG.setTime (0.02f, sampleRate);
        }
        maxDelay = (float) (sampleRate * 3.1);
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.echo.reset(); c.chunk.reset(); c.fbG.reset(); c.wetG.reset();
            c.current = 0; c.age = 0.0f; c.fadeLen = 1.0f;
            c.delay = { 2.0f, 2.0f };
        }
    }

    void update() override
    {
        feedback = knob (1);
        mix = knob (2);
        for (auto& c : channels)
        {
            c.chunk.set (msToSamples (knob (0), sampleRate));
            c.echo.tone.setCutoff (knob (3), sampleRate);
            c.echo.lowCut.setCutoff (50.0f, sampleRate);
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];
        c.fbG.set (feedback);
        c.wetG.set (mix);

        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            const float len = juce::jmax (256.0f, c.chunk.next());
            const float fade = 0.2f * len;

            // Time for the next chunk: start a fresh reader at the newest sample and fade over to it.
            if (c.age >= len - fade && c.age >= c.fadeLen)
            {
                c.current ^= 1;
                c.delay[(size_t) c.current] = 2.0f;
                c.age = 0.0f;
                c.fadeLen = fade;
            }

            const float g = juce::jmin (1.0f, c.age / c.fadeLen);
            const auto cur = (size_t) c.current, old = cur ^ 1;
            float wet = c.echo.read (c.delay[cur]);
            if (g < 1.0f)
                wet = std::sin (0.5f * pi * g) * wet + std::cos (0.5f * pi * g) * c.echo.read (c.delay[old]);

            c.delay[cur] = juce::jmin (maxDelay, c.delay[cur] + 2.0f);
            c.delay[old] = juce::jmin (maxDelay, c.delay[old] + 2.0f);
            c.age += 1.0f;

            c.echo.write (dry + c.fbG.next() * wet);
            x[i] = dry + c.wetG.next() * wet;
        }
    }

private:
    struct Channel
    {
        EchoLine echo;
        Glide chunk, fbG, wetG;
        int current = 0;
        float age = 0.0f, fadeLen = 1.0f;
        std::array<float, 2> delay { 2.0f, 2.0f };
    };

    std::array<Channel, 2> channels;
    float feedback = 0.0f, mix = 0.0f, maxDelay = 1000.0f;
};

//==============================================================================
// CADENCE: rhythmic multi-tap. Taps land on a pattern of beat fractions; the last tap feeds back.
class MultiTapDelay final : public Pedal
{
public:
    static constexpr int maxTaps = 5;
    struct Pattern { int count; float beats[maxTaps]; float gains[maxTaps]; };

    static const Pattern& pattern (int index)
    {
        static const Pattern patterns[] {
            { 3, { 1.0f, 2.0f, 3.0f },                         { 0.85f, 0.62f, 0.45f } },          // Quarters
            { 3, { 0.75f, 1.5f, 2.25f },                       { 0.85f, 0.62f, 0.45f } },          // Dotted
            { 3, { 0.5f, 0.75f, 1.0f },                        { 0.62f, 0.5f, 0.85f } },           // Gallop
            { 3, { 1.0f / 3.0f, 2.0f / 3.0f, 1.0f },           { 0.55f, 0.5f, 0.85f } },           // Triplet
            { 4, { 2.0f / 3.0f, 1.0f, 5.0f / 3.0f, 2.0f },     { 0.6f, 0.8f, 0.5f, 0.65f } },      // Swing
            { 5, { 0.25f, 0.625f, 1.125f, 1.75f, 2.5f },       { 0.5f, 0.65f, 0.45f, 0.55f, 0.4f } } // Scatter
        };
        return patterns[juce::jlimit (0, 5, index)];
    }

    bool producesStereo() const override                    { return true; }

    void prepared() override
    {
        echo.line.setMaxSamples ((int) (sampleRate * 3.1));
        beat.setTime (0.08f, sampleRate);
        fadeStep = 1.0f / (0.015f * (float) sampleRate);
        feedback.reset (sampleRate, 0.03);
        mix.reset (sampleRate, 0.03);
        spread.reset (sampleRate, 0.03);
    }

    void reset() override
    {
        echo.reset(); beat.reset();
        current = target = juce::jlimit (0, 5, (int) knob (1));
        fade = 1.0f;
        feedback.setCurrentAndTargetValue (knob (2));
        mix.setCurrentAndTargetValue (knob (3));
        spread.setCurrentAndTargetValue (knob (5));
    }

    void update() override
    {
        beat.set (msToSamples (knob (0), sampleRate));
        target = juce::jlimit (0, 5, (int) knob (1));
        feedback.setTargetValue (knob (2));
        mix.setTargetValue (knob (3));
        spread.setTargetValue (knob (5));
        echo.tone.setCutoff (knob (4), sampleRate);
        echo.lowCut.setCutoff (60.0f, sampleRate);
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            // Pattern changes dip the taps out and back in rather than jumping.
            if (current != target)
            {
                fade -= fadeStep;
                if (fade <= 0.0f) { fade = 0.0f; current = target; }
            }
            else
            {
                fade = juce::jmin (1.0f, fade + fadeStep);
            }

            const auto& pat = pattern (current);
            const float b = beat.next();
            const float sp = spread.getNextValue();
            float sumL = 0.0f, sumR = 0.0f, last = 0.0f;

            for (int t = 0; t < pat.count; ++t)
            {
                const float v = echo.read (pat.beats[t] * b - 1.0f);
                const float pan = (t % 2 == 0 ? -sp : sp);
                sumL += pat.gains[t] * v * juce::jmin (1.0f, 1.0f - pan);
                sumR += pat.gains[t] * v * juce::jmin (1.0f, 1.0f + pan);
                last = v;
            }

            const float in = 0.5f * (left[i] + right[i]);
            echo.write (in + feedback.getNextValue() * fade * last);

            const float m = mix.getNextValue() * fade * 0.8f;
            left[i]  += m * sumL;
            right[i] += m * sumR;
        }
    }

private:
    EchoLine echo;
    Glide beat;
    int current = 0, target = 0;
    float fade = 1.0f, fadeStep = 0.001f;
    juce::SmoothedValue<float> feedback, mix, spread;
};

//==============================================================================
// RICOCHET: slapback. One short, bright-but-thin repeat with a little tape grit.
class Slapback final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.line.setMaxSamples ((int) (sampleRate * 0.25));
            c.time.setTime (0.06f, sampleRate);
            c.fbG.setTime (0.02f, sampleRate);
            c.wetG.setTime (0.02f, sampleRate);
        }
    }

    void reset() override
    {
        for (size_t ch = 0; ch < channels.size(); ++ch)
        {
            auto& c = channels[ch];
            c.line.clear(); c.tone.reset(); c.lowCut.reset(); c.time.reset(); c.fbG.reset(); c.wetG.reset();
            c.wobble.setPhase (ch == 0 ? 0.0f : 1.3f);
        }
    }

    void update() override
    {
        level = knob (1);
        feedback = knob (4);
        grit = 1.0f + 5.0f * knob (3);
        for (auto& c : channels)
        {
            c.time.set (msToSamples (knob (0), sampleRate) - 1.0f);
            c.tone.setCutoff (knob (2), sampleRate);
            c.lowCut.setCutoff (140.0f, sampleRate);
            c.wobble.setRate (0.9f, sampleRate);
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];
        c.fbG.set (feedback);
        c.wetG.set (level * (0.85f + 0.15f * grit));
        const float wobbleDepth = 0.0006f * (float) sampleRate / (twoPi * 0.9f);

        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            const float slap = c.line.read (juce::jmax (2.0f, c.time.next() + wobbleDepth * c.wobble.next()));
            const float rec = c.lowCut.process (c.tone.process (dry + c.fbG.next() * slap));
            c.line.push (fastTanh (grit * rec) / grit);
            x[i] = dry + c.wetG.next() * slap;
        }
    }

private:
    struct Channel { dsp::DelayBuffer line; Glide time, fbG, wetG; OnePoleLP tone; OnePoleHP lowCut; Lfo wobble; };
    std::array<Channel, 2> channels;
    float level = 0.5f, feedback = 0.0f, grit = 1.0f;
};

//==============================================================================
// RIPTIDE: modulated delay. The repeats drift in pitch like a slow chorus and smear further each pass.
class ModulatedDelay final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.echo.line.setMaxSamples ((int) (sampleRate * 1.25));
            c.time.setTime (0.08f, sampleRate);
            c.fbG.setTime (0.02f, sampleRate);
            c.wetG.setTime (0.02f, sampleRate);
        }
    }

    void reset() override
    {
        for (size_t ch = 0; ch < channels.size(); ++ch)
        {
            auto& c = channels[ch];
            c.echo.reset(); c.time.reset(); c.depth.reset(); c.fbG.reset(); c.wetG.reset();
            c.lfo.setPhase (ch == 0 ? 0.0f : pi * 0.5f);
        }
    }

    void update() override
    {
        feedback = knob (1);
        mix = knob (2);
        const float rate = knob (3);
        const float depthSamples = knob (4) * msToSamples (2.6f, sampleRate) * juce::jmin (1.0f, 1.0f / rate);
        for (auto& c : channels)
        {
            c.time.set (msToSamples (knob (0), sampleRate) - 1.0f);
            c.depth.setTime (0.05f, sampleRate);
            c.depth.set (depthSamples);
            c.lfo.setRate (rate, sampleRate);
            c.echo.tone.setCutoff (knob (5), sampleRate);
            c.echo.lowCut.setCutoff (50.0f, sampleRate);
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];
        c.fbG.set (feedback);
        c.wetG.set (mix);

        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            const float dep = c.depth.next();
            const float wet = c.echo.read (c.time.next() + dep * (1.0f + c.lfo.next()));
            c.echo.write (dry + c.fbG.next() * wet);
            x[i] = dry + c.wetG.next() * wet;
        }
    }

private:
    struct Channel { EchoLine echo; Glide time, depth, fbG, wetG; Lfo lfo; };
    std::array<Channel, 2> channels;
    float feedback = 0.0f, mix = 0.0f;
};

//==============================================================================
// CORRODE: lo-fi delay. Every pass is band-limited, sample-rate reduced and bit-truncated (no added noise).
class LoFiDelay final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.line.setMaxSamples ((int) (sampleRate * 1.6));
            c.time.setTime (0.1f, sampleRate);
            c.fbG.setTime (0.02f, sampleRate);
            c.wetG.setTime (0.02f, sampleRate);
        }
    }

    void reset() override
    {
        for (size_t ch = 0; ch < channels.size(); ++ch)
        {
            auto& c = channels[ch];
            c.line.clear(); c.time.reset(); c.fbG.reset(); c.wetG.reset(); c.lowCut.reset(); c.antiAlias.reset(); c.post.reset(); c.drift = 0.0f;
            c.held = 0.0f; c.holdPhase = 1.0f;
            c.noise.state = ch == 0 ? 0xabcdef1u : 0x1fedcbau;
            c.warble.setPhase (ch == 0 ? 0.0f : 2.1f);
        }
    }

    void update() override
    {
        feedback = knob (1);
        mix = knob (2);
        const float bits = 16.0f - 12.0f * knob (3);
        step = std::pow (2.0f, 1.0f - bits);
        holdInc = juce::jlimit (0.0f, 1.0f, knob (4) / (float) sampleRate);
        const float band = knob (5);
        const float warble = knob (6);
        warbleDepth = warble * 0.004f * (float) sampleRate / (twoPi * 0.7f);
        driftDepth = warble * 0.003f * (float) sampleRate / (twoPi * 1.5f);
        driftCoeff = dsp::onePoleCoeff (1.5f, sampleRate);

        for (auto& c : channels)
        {
            c.time.set (msToSamples (knob (0), sampleRate));
            c.lowCut.setCutoff (60.0f + 640.0f * band, sampleRate);
            c.antiAlias.setup (juce::jmin (0.45f * knob (4), 9000.0f - 6000.0f * band), 0.7f, sampleRate);
            c.post.setup (juce::jmin (0.45f * knob (4), 9000.0f - 6000.0f * band), 0.7f, sampleRate);
            c.warble.setRate (0.7f, sampleRate);
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];
        c.fbG.set (feedback);
        c.wetG.set (mix);

        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            c.drift += driftCoeff * (c.noise.next() - c.drift);
            const float mod = warbleDepth * (1.0f + c.warble.next()) + driftDepth * 60.0f * std::abs (c.drift);
            const float wet = c.post.process (c.line.read (juce::jmax (2.0f, c.time.next() + mod - 1.0f))).low;

            // Degrade what goes onto the line: band-limit, sample & hold, truncate toward zero.
            const float in = c.antiAlias.process (c.lowCut.process (dry + c.fbG.next() * wet)).low;
            c.holdPhase += holdInc;
            if (c.holdPhase >= 1.0f)
            {
                c.holdPhase -= 1.0f;
                c.held = std::trunc (softLimit (in) / step) * step;
            }
            c.line.push (c.held);

            x[i] = dry + c.wetG.next() * wet;
        }
    }

private:
    struct Channel
    {
        dsp::DelayBuffer line;
        Glide time, fbG, wetG;
        OnePoleHP lowCut;
        dsp::Svf antiAlias, post;
        Lfo warble;
        Noise noise;
        float drift = 0.0f, held = 0.0f, holdPhase = 1.0f;
    };

    std::array<Channel, 2> channels;
    float feedback = 0.0f, mix = 0.0f, step = 0.0f, holdInc = 0.1f;
    float warbleDepth = 0.0f, driftDepth = 0.0f, driftCoeff = 0.0f;
};

//==============================================================================
// SHADOW: ducking delay. Repeats stay tucked under while you play and bloom when you stop.
class DuckingDelay final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.echo.line.setMaxSamples ((int) (sampleRate * 1.6));
            c.time.setTime (0.08f, sampleRate);
            c.fbG.setTime (0.02f, sampleRate);
            c.wetG.setTime (0.02f, sampleRate);
        }
        attack = 1.0f - std::exp (-1.0f / (0.004f * (float) sampleRate));
    }

    void reset() override
    {
        for (auto& c : channels) { c.echo.reset(); c.time.reset(); c.fbG.reset(); c.wetG.reset(); c.env = 0.0f; }
    }

    void update() override
    {
        feedback = knob (1);
        mix = knob (2);
        duckDb = knob (3) * 30.0f;
        release = 1.0f - std::exp (-1.0f / (knob (4) * 0.001f * (float) sampleRate));
        for (auto& c : channels)
        {
            c.time.set (msToSamples (knob (0), sampleRate) - 1.0f);
            c.echo.tone.setCutoff (knob (5), sampleRate);
            c.echo.lowCut.setCutoff (50.0f, sampleRate);
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];
        c.fbG.set (feedback);
        c.wetG.set (mix);
        constexpr float lo = 0.004f, hi = 0.04f;   // about -48 dBFS to -28 dBFS of peak level

        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            const float a = std::abs (dry);
            c.env += (a > c.env ? attack : release) * (a - c.env);
            const float amount = juce::jlimit (0.0f, 1.0f, (c.env - lo) / (hi - lo));
            const float duck = dsp::dbToGain (-duckDb * amount);

            const float wet = c.echo.read (c.time.next());
            c.echo.write (dry + c.fbG.next() * wet);
            x[i] = dry + c.wetG.next() * duck * wet;
        }
    }

private:
    struct Channel { EchoLine echo; Glide time, fbG, wetG; float env = 0.0f; };
    std::array<Channel, 2> channels;
    float feedback = 0.0f, mix = 0.0f, duckDb = 0.0f, attack = 0.1f, release = 0.001f;
};

} // namespace

void registerDelayPedals (Registry& r)
{
    r.add<AnalogDelay> ({ "delay.analog", "ECHO", "analog delay", "Delay",
                          "Warm repeats that get darker as they fade",
                          juce::Colour (0xff27506b), juce::Colour (0xfff1e9d8),
                          { knob ("time", "Time", 40.0f, 1200.0f, 380.0f, "ms", 0.6f),
                            knob ("repeats", "Repeats", 0.0f, 0.92f, 0.35f, "%"),
                            knob ("mix", "Mix", 0.0f, 1.0f, 0.3f, "%") } });

    r.add<DigitalDelay> ({ "delay.digital", "GLASS", "digital delay", "Delay",
                           "Pristine repeats with note divisions of the beat you set",
                           juce::Colour (0xffb9c7cf), juce::Colour (0xff1c2a33),
                           { knob ("time", "Time", 40.0f, 2000.0f, 480.0f, "ms", 0.5f),
                             knob ("repeats", "Repeats", 0.0f, 0.95f, 0.35f, "%"),
                             knob ("mix", "Mix", 0.0f, 1.0f, 0.32f, "%"),
                             choice ("division", "Division", { "1/4", "1/8 dot", "1/8", "1/8 trip" }, 0),
                             knob ("tone", "Tone", 1000.0f, 18000.0f, 9000.0f, "Hz", 0.4f) } });

    r.add<TapeEcho> ({ "delay.tape", "SPOOL", "tape echo", "Delay",
                       "Three playback heads, warbling tape and repeats that saturate",
                       juce::Colour (0xff7a4a2e), juce::Colour (0xfff3e2c4),
                       { knob ("time", "Time", 60.0f, 1000.0f, 360.0f, "ms", 0.6f),
                         knob ("repeats", "Repeats", 0.0f, 1.0f, 0.42f, "%"),
                         knob ("mix", "Mix", 0.0f, 1.0f, 0.35f, "%"),
                         knob ("wow", "Wow", 0.0f, 1.0f, 0.3f, "%"),
                         knob ("drive", "Drive", 0.0f, 1.0f, 0.35f, "%"),
                         knob ("age", "Age", 0.0f, 1.0f, 0.4f, "%"),
                         choice ("heads", "Heads", { "Long", "Mid", "Mid+Long", "Short+Long", "All" }, 0) } });

    r.add<PingPongDelay> ({ "delay.pingpong", "VOLLEY", "ping-pong delay", "Delay",
                            "Repeats that bounce from one side of the room to the other",
                            juce::Colour (0xff4a90c2), juce::Colour (0xff0d1a24),
                            { knob ("time", "Time", 40.0f, 1500.0f, 360.0f, "ms", 0.5f),
                              knob ("repeats", "Repeats", 0.0f, 0.95f, 0.45f, "%"),
                              knob ("mix", "Mix", 0.0f, 1.0f, 0.35f, "%"),
                              knob ("tone", "Tone", 1000.0f, 18000.0f, 7000.0f, "Hz", 0.4f),
                              knob ("width", "Width", 0.0f, 1.0f, 1.0f, "%") } });

    r.add<ReverseDelay> ({ "delay.reverse", "MIRAGE", "reverse delay", "Delay",
                           "Plays what you just played backwards, in smooth swells",
                           juce::Colour (0xff5a3d7a), juce::Colour (0xfff0e2ff),
                           { knob ("time", "Time", 150.0f, 1500.0f, 600.0f, "ms", 0.6f),
                             knob ("repeats", "Repeats", 0.0f, 0.85f, 0.2f, "%"),
                             knob ("mix", "Mix", 0.0f, 1.0f, 0.5f, "%"),
                             knob ("tone", "Tone", 1000.0f, 18000.0f, 8000.0f, "Hz", 0.4f) } });

    r.add<MultiTapDelay> ({ "delay.multitap", "CADENCE", "multi-tap delay", "Delay",
                            "Rhythmic tap patterns spread across the stereo field",
                            juce::Colour (0xff2f6b4f), juce::Colour (0xffe9f2e4),
                            { knob ("time", "Beat", 150.0f, 1000.0f, 420.0f, "ms", 0.7f),
                              choice ("pattern", "Pattern", { "Quarters", "Dotted", "Gallop", "Triplet", "Swing", "Scatter" }, 1),
                              knob ("repeats", "Repeats", 0.0f, 0.85f, 0.25f, "%"),
                              knob ("mix", "Mix", 0.0f, 1.0f, 0.35f, "%"),
                              knob ("tone", "Tone", 1000.0f, 18000.0f, 7000.0f, "Hz", 0.4f),
                              knob ("spread", "Spread", 0.0f, 1.0f, 0.6f, "%") } });

    r.add<Slapback> ({ "delay.slapback", "RICOCHET", "slapback", "Delay",
                       "One quick, gritty bounce for rockabilly and country twang",
                       juce::Colour (0xffb7322c), juce::Colour (0xfffbeee0),
                       { knob ("time", "Time", 40.0f, 220.0f, 110.0f, "ms"),
                         knob ("level", "Level", 0.0f, 1.0f, 0.5f, "%"),
                         knob ("tone", "Tone", 1500.0f, 12000.0f, 4500.0f, "Hz", 0.5f),
                         knob ("grit", "Grit", 0.0f, 1.0f, 0.3f, "%"),
                         knob ("spill", "Spill", 0.0f, 0.4f, 0.05f, "%") } });

    r.add<ModulatedDelay> ({ "delay.modulated", "RIPTIDE", "modulated delay", "Delay",
                             "Repeats that sway in pitch like a slow, seasick chorus",
                             juce::Colour (0xff1f6f78), juce::Colour (0xffdff5f2),
                             { knob ("time", "Time", 40.0f, 1200.0f, 420.0f, "ms", 0.6f),
                               knob ("repeats", "Repeats", 0.0f, 0.92f, 0.4f, "%"),
                               knob ("mix", "Mix", 0.0f, 1.0f, 0.35f, "%"),
                               knob ("rate", "Rate", 0.1f, 5.0f, 0.7f, "Hz", 0.5f),
                               knob ("depth", "Depth", 0.0f, 1.0f, 0.45f, "%"),
                               knob ("tone", "Tone", 1000.0f, 12000.0f, 4500.0f, "Hz", 0.5f) } });

    r.add<LoFiDelay> ({ "delay.lofi", "CORRODE", "lo-fi delay", "Delay",
                        "Crunchy, narrow repeats that wear away a little more each time",
                        juce::Colour (0xffe6a3a3), juce::Colour (0xff3b1d1d),
                        { knob ("time", "Time", 40.0f, 1500.0f, 380.0f, "ms", 0.5f),
                          knob ("repeats", "Repeats", 0.0f, 0.92f, 0.45f, "%"),
                          knob ("mix", "Mix", 0.0f, 1.0f, 0.4f, "%"),
                          knob ("crush", "Crush", 0.0f, 1.0f, 0.45f, "%"),
                          knob ("rate", "Rate", 1500.0f, 24000.0f, 8000.0f, "Hz", 0.5f),
                          knob ("band", "Band", 0.0f, 1.0f, 0.4f, "%"),
                          knob ("warble", "Warble", 0.0f, 1.0f, 0.25f, "%") } });

    r.add<DuckingDelay> ({ "delay.ducking", "SHADOW", "ducking delay", "Delay",
                           "Repeats step aside while you play and bloom when you stop",
                           juce::Colour (0xff2c2f3a), juce::Colour (0xff9fe0c8),
                           { knob ("time", "Time", 40.0f, 1500.0f, 450.0f, "ms", 0.5f),
                             knob ("repeats", "Repeats", 0.0f, 0.92f, 0.45f, "%"),
                             knob ("mix", "Mix", 0.0f, 1.0f, 0.45f, "%"),
                             knob ("duck", "Duck", 0.0f, 1.0f, 0.7f, "%"),
                             knob ("release", "Release", 50.0f, 1500.0f, 300.0f, "ms", 0.5f),
                             knob ("tone", "Tone", 1000.0f, 12000.0f, 6000.0f, "Hz", 0.5f) } });
}

} // namespace woodshed
