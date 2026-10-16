#pragma once

#include "Instrument.h"

/** DSP building blocks shared by the software instruments. Everything here is realtime-safe
    once prepared: no allocation outside prepare()/setMaxSamples(). */
namespace woodshed::idsp
{

constexpr float pi    = juce::MathConstants<float>::pi;
constexpr float twoPi = juce::MathConstants<float>::twoPi;

inline float noteToHz (float note)              { return 440.0f * std::exp2 ((note - 69.0f) * (1.0f / 12.0f)); }
inline float hzToNote (float hz)                { return 69.0f + 12.0f * std::log2 (hz / 440.0f); }
inline float semitones (float st)               { return std::exp2 (st * (1.0f / 12.0f)); }
inline float wrap01 (float p)                   { return p - std::floor (p); }
inline float dbToGain (float db)                { return std::pow (10.0f, db * 0.05f); }

/** sin (2 pi p) for any p in turns; polynomial, error about 4e-6, much cheaper than std::sin. */
inline float sinTurns (float p)
{
    float x = p - std::floor (p + 0.5f);        // [-0.5, 0.5)
    if (x > 0.25f)       x = 0.5f - x;
    else if (x < -0.25f) x = -0.5f - x;
    const float t = twoPi * x, t2 = t * t;
    return t * (1.0f + t2 * (-1.0f / 6.0f + t2 * (1.0f / 120.0f + t2 * (-1.0f / 5040.0f + t2 * (1.0f / 362880.0f)))));
}

/** Cheap tanh-like saturation, unity slope at 0, reaches +-1 at +-3. */
inline float saturate (float x)
{
    const float c = juce::jlimit (-3.0f, 3.0f, x);
    return c * (27.0f + c * c) / (27.0f + 9.0f * c * c);
}

/** Output safety: transparent below 0.7, then a soft knee that never quite reaches 1. */
inline float safetyClip (float x)
{
    const float a = std::abs (x);
    if (a <= 0.7f) return x;
    const float y = 0.7f + 0.28f * saturate ((a - 0.7f) * (1.0f / 0.28f));
    return x < 0.0f ? -y : y;
}

inline void safetyClip (float* l, float* r, int n)
{
    for (int i = 0; i < n; ++i) { l[i] = safetyClip (l[i]); r[i] = safetyClip (r[i]); }
}

/** Velocity (0..1) to gain: sens 0 ignores velocity, 1 is a full ~40 dB range. */
inline float velocityGain (float velocity, float sens)
{
    const float v = juce::jlimit (0.0f, 1.0f, velocity);
    return (1.0f - sens) + sens * v * v;
}

/** Equal-power pan, p in -1..1. */
inline std::pair<float, float> panGains (float p)
{
    const float a = (juce::jlimit (-1.0f, 1.0f, p) + 1.0f) * pi * 0.25f;
    return { std::cos (a) * juce::MathConstants<float>::sqrt2, std::sin (a) * juce::MathConstants<float>::sqrt2 };
}

//==============================================================================
/** White noise in [-1, 1] from a tiny LCG. */
struct Noise
{
    uint32_t seed = 22222;
    void setSeed (uint32_t s)                   { seed = s * 2654435761u + 1u; }
    float next()                                { seed = seed * 1664525u + 1013904223u; return (float) (seed >> 8) * (2.0f / 16777216.0f) - 1.0f; }
    float uniform()                             { return 0.5f * (next() + 1.0f); }
};

/** One-pole parameter smoother; jumps to the first value after reset(). */
struct Smooth
{
    float value = 0.0f, target = 0.0f, coeff = 1.0f;
    bool primed = false;

    void prepare (double sr, float seconds)     { coeff = 1.0f - std::exp (-1.0f / (seconds * (float) sr)); primed = false; }
    void reset()                                { primed = false; }
    void set (float t)                          { target = t; if (! primed) { value = t; primed = true; } }
    float next()                                { value += coeff * (target - value); return value; }
};

struct OnePoleLp
{
    float s = 0.0f;
    void reset()                                { s = 0.0f; }
    float process (float x, float c)            { s += c * (x - s); return s; }
};

struct OnePoleHp
{
    float s = 0.0f;
    void reset()                                { s = 0.0f; }
    float process (float x, float c)            { s += c * (x - s); return x - s; }
};

/** Coefficient for OnePoleLp/Hp at `hz`. */
inline float onePole (float hz, double sr)      { return 1.0f - std::exp (-twoPi * juce::jmin (hz, (float) sr * 0.45f) / (float) sr); }

//==============================================================================
/** State-variable filter (TPT) taking g = tan (pi fc / sr) and k = 1/Q per sample. */
struct Svf
{
    float ic1 = 0.0f, ic2 = 0.0f;
    struct Out { float low, band, high; };

    void reset()                                { ic1 = ic2 = 0.0f; }

    Out process (float v0, float g, float k)
    {
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float v1 = a1 * (ic1 + g * (v0 - ic2));
        const float v2 = ic2 + g * v1;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return { v2, v1, v0 - k * v1 - v2 };
    }

    static float gFor (float hz, double sr)     { return std::tan (pi * juce::jlimit (16.0f, (float) sr * 0.45f, hz) / (float) sr); }
};

/** Resonant 24 dB/oct lowpass from two SVF stages; resonance 0..1 (self-oscillates near 1). */
struct Lowpass24
{
    Svf a, b;
    void reset()                                { a.reset(); b.reset(); }

    float process (float x, float g, float resonance)
    {
        const float k1 = 1.85f - 1.8f * resonance;            // Q 0.54 .. 20
        const float y = a.process (saturate (x), g, k1).low;
        return b.process (y, g, 0.765f).low;                  // Butterworth-ish second stage
    }
};

/** One-pole DC blocker. */
struct DcBlock
{
    float x1 = 0.0f, y1 = 0.0f;
    void reset()                                { x1 = y1 = 0.0f; }
    float process (float x)                     { const float y = x - x1 + 0.9975f * y1; x1 = x; y1 = y; return y; }
};

//==============================================================================
inline float polyBlep (float t, float dt)
{
    if (t < dt)        { t /= dt;               return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt;   return t * t + t + t + 1.0f; }
    return 0.0f;
}

/** Band-limited (PolyBLEP) oscillator. Phase in turns. */
struct Osc
{
    enum Shape { saw, square, pulse, triangle, sine };
    float phase = 0.0f;

    float next (float inc, int shape, float pulseWidth = 0.25f)
    {
        const float t = phase;
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
        const float dt = juce::jlimit (1.0e-6f, 0.45f, inc);

        switch (shape)
        {
            case saw:       return 2.0f * t - 1.0f - polyBlep (t, dt);
            case square:
            case pulse:
            {
                const float pw = shape == square ? 0.5f : pulseWidth;
                float t2 = t - pw;
                if (t2 < 0.0f) t2 += 1.0f;
                const float v = (t < pw ? 1.0f : -1.0f) + polyBlep (t, dt) - polyBlep (t2, dt);
                return v - (2.0f * pw - 1.0f);
            }
            case triangle:  return 1.0f - 4.0f * std::abs (t - 0.5f);
            default:        return sinTurns (t);
        }
    }
};

//==============================================================================
/** ADSR with a curved attack and exponential decay/release. Times in seconds. */
struct Adsr
{
    enum class Stage { idle, attack, decay, sustain, release };
    Stage stage = Stage::idle;
    float value = 0.0f, attackCoeff = 1.0f, decayCoeff = 0.0f, sustainLevel = 1.0f, releaseCoeff = 0.0f;

    void set (double sr, float attackS, float decayS, float sustain, float releaseS)
    {
        const auto fs = (float) sr;
        attackCoeff  = 1.0f - std::exp (-1.466f / juce::jmax (1.0f, attackS * fs));     // reaches 1 (aiming at 1.3) in attackS
        decayCoeff   = std::exp (-6.9f / juce::jmax (1.0f, decayS * fs));               // -60 dB of the distance
        releaseCoeff = std::exp (-6.9f / juce::jmax (1.0f, releaseS * fs));
        sustainLevel = juce::jlimit (0.0f, 1.0f, sustain);
    }

    void trigger()                              { stage = Stage::attack; }
    void noteOff()                              { if (stage != Stage::idle) stage = Stage::release; }
    void kill()                                 { stage = Stage::idle; value = 0.0f; }
    bool isActive() const                       { return stage != Stage::idle; }

    float next()
    {
        switch (stage)
        {
            case Stage::attack:
                value += attackCoeff * (1.3f - value);
                if (value >= 1.0f) { value = 1.0f; stage = Stage::decay; }
                break;
            case Stage::decay:
                value = sustainLevel + (value - sustainLevel) * decayCoeff;
                if (value - sustainLevel < 1.0e-4f) stage = Stage::sustain;
                break;
            case Stage::sustain:
                value += 0.002f * (sustainLevel - value);
                if (value < 1.0e-4f) { value = 0.0f; stage = Stage::idle; }
                break;
            case Stage::release:
                value *= releaseCoeff;
                if (value < 1.0e-4f) { value = 0.0f; stage = Stage::idle; }
                break;
            case Stage::idle:
                break;
        }
        return value;
    }
};

//==============================================================================
/**
    The start protocol shared by voices: start() only records the note, and the voice
    begins on its next render (where it can read its owner). A stolen voice first fades
    its old sound out over ~3 ms so nothing clicks.
*/
struct PendingStart
{
    int note = -1;
    float velocity = 0.0f, fade = 1.0f, fadeStep = 0.01f;
    bool pending = false, fading = false;

    void prepare (double sr)                    { fadeStep = 1.0f / (float) (0.003 * sr); }
    void request (int n, float v, bool stolen)  { note = n; velocity = v; pending = true; fading = stolen; }
    void clear()                                { pending = fading = false; fade = 1.0f; }

    /** Call once per sample while pending; true when the voice should begin now. */
    bool tick()
    {
        if (! fading) return true;
        fade -= fadeStep;
        return fade <= 0.0f;
    }
};

//==============================================================================
/**
    A bank of exponentially decaying sine modes (rotating phasors), the core of every
    struck or plucked sound: per-mode frequency and decay for four multiplies a sample.
*/
template <int maxModes>
struct ModalBank
{
    static_assert (maxModes % 4 == 0, "modes are processed four at a time");

    std::array<float, (size_t) maxModes> re {}, im {}, cr {}, ci {};
    int count = 0;

    void clear()                                { re.fill (0.0f); im.fill (0.0f); }

    /** Sets how many modes run; the tail up to a multiple of four is silenced. */
    void setCount (int n)
    {
        count = juce::jlimit (0, maxModes, (n + 3) & ~3);
        for (int i = n; i < count; ++i) { cr[(size_t) i] = ci[(size_t) i] = 0.0f; re[(size_t) i] = im[(size_t) i] = 0.0f; }
    }

    /** t60: seconds to decay 60 dB. Modes above 0.45 sr are muted. */
    void setMode (int i, float hz, float t60, double sr)
    {
        const auto idx = (size_t) i;
        if (hz >= (float) sr * 0.45f || hz <= 0.0f) { cr[idx] = ci[idx] = 0.0f; re[idx] = im[idx] = 0.0f; return; }
        const float w = twoPi * hz / (float) sr;
        const float r = std::exp (-6.9078f / (juce::jmax (0.001f, t60) * (float) sr));
        cr[idx] = r * std::cos (w);
        ci[idx] = r * std::sin (w);
    }

    /** Changes only the decay of mode i, keeping its frequency. */
    void setDecay (int i, float t60, double sr)
    {
        const auto idx = (size_t) i;
        const float mag = std::sqrt (cr[idx] * cr[idx] + ci[idx] * ci[idx]);
        if (mag <= 0.0f) return;
        const float r = std::exp (-6.9078f / (juce::jmax (0.001f, t60) * (float) sr));
        cr[idx] *= r / mag;
        ci[idx] *= r / mag;
    }

    /** Adds an impulse of amplitude `amp` (the mode starts at sine phase, so no click). */
    void strike (int i, float amp)              { re[(size_t) i] += amp; }

    float next()
    {
        float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
        for (int i = 0; i < count; i += 4)
        {
            float out[4];
            for (int k = 0; k < 4; ++k)
            {
                const auto j = (size_t) (i + k);
                const float nr = re[j] * cr[j] - im[j] * ci[j];
                const float ni = re[j] * ci[j] + im[j] * cr[j];
                re[j] = nr;
                im[j] = ni;
                out[k] = ni;
            }
            acc0 += out[0]; acc1 += out[1]; acc2 += out[2]; acc3 += out[3];
        }
        return (acc0 + acc1) + (acc2 + acc3);
    }

    /** Rough amplitude bound, for deciding when the voice has died away. */
    float level() const
    {
        float s = 0.0f;
        for (int i = 0; i < count; ++i) s += std::abs (re[(size_t) i]) + std::abs (im[(size_t) i]);
        return s;
    }
};

//==============================================================================
/** Stereo ensemble chorus: three modulated taps per side, for pads, strings and synths. */
struct Ensemble
{
    std::array<dsp::DelayBuffer, 2> lines;
    float phase = 0.0f, shimmer = 0.0f, rateInc = 0.0f, shimmerInc = 0.0f, msToSamples = 48.0f;

    void prepare (double sr)
    {
        for (auto& l : lines) l.setMaxSamples ((int) (sr * 0.04));
        msToSamples = (float) sr * 0.001f;
        rateInc = 0.55f / (float) sr;
        shimmerInc = 5.3f / (float) sr;
    }

    void reset()                                { for (auto& l : lines) l.clear(); phase = shimmer = 0.0f; }

    /** mix 0..1 blends in the chorus (mix 1 is fully wet); depth scales the sweep. */
    void process (float* left, float* right, int n, float mix, float depth = 1.0f)
    {
        if (mix <= 0.0001f) return;
        for (int i = 0; i < n; ++i)
        {
            lines[0].push (left[i]);
            lines[1].push (right[i]);
            phase += rateInc;   if (phase >= 1.0f) phase -= 1.0f;
            shimmer += shimmerInc; if (shimmer >= 1.0f) shimmer -= 1.0f;

            float wl = 0.0f, wr = 0.0f;
            for (int v = 0; v < 3; ++v)
            {
                const float off = (float) v / 3.0f;
                const float sl = sinTurns (phase + off) * 2.2f + sinTurns (shimmer + off) * 0.25f;
                const float sr = sinTurns (phase + off + 0.17f) * 2.2f + sinTurns (shimmer + off + 0.5f) * 0.25f;
                wl += lines[0].read ((9.0f + 2.5f * (float) v + depth * sl) * msToSamples);
                wr += lines[1].read ((9.0f + 2.5f * (float) v + depth * sr) * msToSamples);
            }

            const float dry = 1.0f - 0.5f * mix;
            left[i]  = dry * left[i]  + mix * 0.45f * wl;
            right[i] = dry * right[i] + mix * 0.45f * wr;
        }
    }
};

//==============================================================================
/** Small stereo room: two diffusers into a four-line feedback delay network with damping. */
struct Room
{
    struct Line
    {
        std::vector<float> buf;
        int pos = 0, length = 1;
        float lp = 0.0f;
        float read() const                      { return buf[(size_t) pos]; }
        void write (float x)                    { buf[(size_t) pos] = x; if (++pos >= length) pos = 0; }
    };

    std::array<Line, 4> lines;
    std::array<Line, 2> diffusers;
    std::array<float, 4> gains {};
    float damp = 0.3f;
    double sr = 48000.0;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        const float ms[] { 31.7f, 37.9f, 43.3f, 49.1f };
        for (size_t i = 0; i < 4; ++i)
        {
            lines[i].length = (int) (ms[i] * 0.001f * (float) sr);
            lines[i].buf.assign ((size_t) lines[i].length, 0.0f);
        }
        const float dms[] { 4.7f, 7.3f };
        for (size_t i = 0; i < 2; ++i)
        {
            diffusers[i].length = (int) (dms[i] * 0.001f * (float) sr);
            diffusers[i].buf.assign ((size_t) diffusers[i].length, 0.0f);
        }
        setDecay (1.2f, 6000.0f);
    }

    void reset()
    {
        for (auto& l : lines)     { std::fill (l.buf.begin(), l.buf.end(), 0.0f); l.pos = 0; l.lp = 0.0f; }
        for (auto& d : diffusers) { std::fill (d.buf.begin(), d.buf.end(), 0.0f); d.pos = 0; }
    }

    /** rt60 in seconds (keep under ~3 s), damping cutoff in Hz. */
    void setDecay (float rt60, float dampHz)
    {
        for (size_t i = 0; i < 4; ++i)
            gains[i] = std::pow (10.0f, -3.0f * (float) lines[i].length / (juce::jmax (0.05f, rt60) * (float) sr));
        damp = onePole (dampHz, sr);
    }

    /** Adds `amount` of reverb to the stereo signal in place. */
    void process (float* left, float* right, int n, float amount)
    {
        if (amount <= 0.0001f) return;
        for (int i = 0; i < n; ++i)
        {
            float x = 0.5f * (left[i] + right[i]);
            for (auto& d : diffusers)
            {
                const float delayed = d.read();
                const float v = x + 0.6f * delayed;
                d.write (v);
                x = delayed - 0.6f * v;
            }

            const float a = lines[0].read(), b = lines[1].read(), c = lines[2].read(), d = lines[3].read();
            // Hadamard mix (energy preserving with the 0.5).
            const float m0 = 0.5f * (a + b + c + d), m1 = 0.5f * (a - b + c - d);
            const float m2 = 0.5f * (a + b - c - d), m3 = 0.5f * (a - b - c + d);
            const float mixed[] { m0, m1, m2, m3 };
            for (size_t k = 0; k < 4; ++k)
            {
                auto& l = lines[k];
                l.lp += damp * (mixed[k] * gains[k] - l.lp);
                l.write (l.lp + x * (k % 2 == 0 ? 0.5f : -0.5f));
            }

            left[i]  += amount * (a + 0.6f * c);
            right[i] += amount * (b + 0.6f * d);
        }
    }
};

} // namespace woodshed::idsp
