#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// Shared building blocks
//==============================================================================

constexpr float twoPi = juce::MathConstants<float>::twoPi;

/**
    Dry/wet gains for a 0..1 mix against a voice currently shifted by `semis`.
    Near unison the wet is the dry (just delayed), so the blend is linear and the
    pedal stays at unity; once shifted the two are unrelated and it moves to
    equal power so the middle of the knob doesn't dip.
*/
inline void mixGains (float mix, float semis, float& dry, float& wet)
{
    const float m = juce::jlimit (0.0f, 1.0f, mix);
    const float d = 1.0f - m;
    const float shifted = juce::jmin (1.0f, std::abs (semis) * 2.0f);
    const float c = 1.0f + shifted * (1.0f / std::sqrt (d * d + m * m) - 1.0f);
    dry = d * c;
    wet = m * c;
}

/** Equal-power pan: -1 (left) .. +1 (right). */
inline float panLeft (float pan)                { return std::cos ((juce::jlimit (-1.0f, 1.0f, pan) + 1.0f) * 0.25f * juce::MathConstants<float>::pi); }
inline float panRight (float pan)               { return std::sin ((juce::jlimit (-1.0f, 1.0f, pan) + 1.0f) * 0.25f * juce::MathConstants<float>::pi); }

/** Sample at an integer delay (0 = newest). */
inline float tapAt (const dsp::DelayBuffer& b, int delay)
{
    return b.data[(size_t) ((b.writePos - 1 - delay) & b.mask)];
}

/** Low-pass whose cutoff glides block by block, so a swept Tone knob never zippers. */
struct ToneFilter
{
    dsp::Svf svf;
    float hz = 5000.0f, target = 5000.0f;

    void reset (double sr)                      { svf.reset(); hz = target; svf.setup (hz, 0.707f, sr); }
    void setTarget (float newHz)                { target = newHz; }

    void step (double sr)
    {
        if (std::abs (target - hz) > 0.5f)
        {
            hz += 0.25f * (target - hz);
            svf.setup (hz, 0.707f, sr);
        }
    }

    float process (float x)                     { return svf.process (x).low; }
};

//==============================================================================
/**
    One pitch-shifted voice reading a shared delay line.

    Two taps sweep a window of W samples in opposite grain phase; the delay of
    each tap moves at (1 - ratio) samples per sample, so it plays back at
    exactly `ratio` times the input speed. Each tap fades in and out with a
    sin^2 window and the two weights always sum to one.

    When a tap wraps (at zero weight) it is re-placed: a short normalised
    cross-correlation search picks the splice offset within +/- search samples
    that best lines its waveform up with the tap currently playing, which takes
    most of the comb/warble out of the crossfades.

    At a ratio of exactly one the voice parks: it crossfades (20 ms) to the
    undelayed input, so a voice at unison is sample-identical to the dry signal
    and a pitch pedal at rest is transparent with no added latency. Leaving
    unison crossfades back into the grains, which restart from the centre delay.

    The shifted output trails the input by roughly centre() samples on average;
    the dry path is never delayed, so pedals using this report zero latency.
*/
class GrainVoice
{
public:
    static constexpr int minDelay = 4;
    static constexpr int segPoints = 96;

    void prepare (double sr, float windowMs, float searchMs)
    {
        const float perMs = (float) sr / 1000.0f;
        window = 2 * juce::jmax (16, juce::roundToInt (windowMs * perMs * 0.5f));
        search = juce::jmax (8, juce::roundToInt (searchMs * perMs));
        lo = (float) (minDelay + search);
        invWindow = 1.0f / (float) window;
        stride = juce::jmax (1, juce::roundToInt (sr / 16000.0));
        coarse = 2 * stride;
        parkStep = 1.0f / (0.02f * (float) sr);
        setGlide (15.0f, sr);
        reset();
    }

    /** Glide time constant for semitone changes. */
    void setGlide (float ms, double sr)
    {
        glide = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, ms * 0.001f * (float) sr));
    }

    /** Average delay of the shifted output, in samples. */
    int centre() const                          { return minDelay + search + window / 2; }

    /** Delay-line length this voice needs, plus any extra delay it will be asked for. */
    int span (int extra = 0) const              { return minDelay + 2 * search + window + segPoints * stride + extra + 16; }

    void reset()
    {
        phase = 0.5f;
        offA = offB = 0.0f;
        st = target;
        ratio = std::exp2 (st / 12.0f);
        park = isUnity (st) ? 1.0f : 0.0f;
    }

    float semitones() const                     { return st; }

    void setSemitones (float semis, bool snap)
    {
        target = semis;
        if (snap) reset();
    }

    float process (const dsp::DelayBuffer& b, float extra = 0.0f)
    {
        if (std::abs (target - st) > 0.0f)
        {
            st += glide * (target - st);
            if (std::abs (target - st) < 1.0e-4f) st = target;
            ratio = std::exp2 (st / 12.0f);
        }

        const bool unity = isUnity (st);
        park = unity ? juce::jmin (1.0f, park + parkStep) : juce::jmax (0.0f, park - parkStep);

        const float input = park > 0.0f ? tapAt (b, 0) : 0.0f;

        if (park >= 1.0f)
        {
            phase = 0.5f;
            offA = offB = 0.0f;
            return input;
        }

        const float prev = phase;
        phase += unity ? 0.0f : (1.0f - ratio) * invWindow;

        bool wrappedA = false;
        if (phase >= 1.0f)      { phase -= 1.0f; wrappedA = true; }
        else if (phase < 0.0f)  { phase += 1.0f; wrappedA = true; }

        float phaseB = phase + 0.5f;
        if (phaseB >= 1.0f) phaseB -= 1.0f;

        const float w = (float) window;

        if (wrappedA)
            offA = splice (b, lo + phase * w + extra, lo + phaseB * w + offB + extra);
        else if ((prev < 0.5f) != (phase < 0.5f))
            offB = splice (b, lo + phaseB * w + extra, lo + phase * w + offA + extra);

        const float dA = juce::jmax (2.0f, lo + phase * w + offA + extra);
        const float dB = juce::jmax (2.0f, lo + phaseB * w + offB + extra);
        const float wA = 0.5f - 0.5f * std::cos (twoPi * phase);
        const float grain = wA * b.read (dA) + (1.0f - wA) * b.read (dB);

        return park > 0.0f ? park * input + (1.0f - park) * grain : grain;
    }

private:
    static bool isUnity (float semis)           { return std::abs (semis) < 1.0e-3f; }

    /** Offset for a tap restarting at newBase so it lines up with the tap at otherDelay. */
    float splice (const dsp::DelayBuffer& b, float newBase, float otherDelay)
    {
        const int ref = juce::roundToInt (otherDelay);
        const int base = juce::roundToInt (newBase);

        float refEnergy = 0.0f;
        for (int k = 0; k < segPoints; ++k)
        {
            const float v = tapAt (b, ref + k * stride);
            refSeg[(size_t) k] = v;
            refEnergy += v * v;
        }

        if (refEnergy < 1.0e-9f)
            return 0.0f;

        auto score = [&] (int off)
        {
            float dot = 0.0f, energy = 1.0e-12f;
            const int start = base + off;
            for (int k = 0; k < segPoints; ++k)
            {
                const float c = tapAt (b, start + k * stride);
                dot += refSeg[(size_t) k] * c;
                energy += c * c;
            }
            return dot / std::sqrt (energy);
        };

        int best = 0;
        float bestScore = -1.0e30f;

        for (int off = -search; off <= search; off += coarse)
        {
            const float s = score (off);
            if (s > bestScore) { bestScore = s; best = off; }
        }

        const int from = juce::jmax (-search, best - coarse + 1);
        const int to = juce::jmin (search, best + coarse - 1);
        const int coarseBest = best;

        for (int off = from; off <= to; ++off)
        {
            if (off == coarseBest) continue;
            const float s = score (off);
            if (s > bestScore) { bestScore = s; best = off; }
        }

        // Sub-sample refinement: fit a parabola through the peak and its neighbours.
        float frac = 0.0f;
        if (best > -search && best < search)
        {
            const float sm = score (best - 1), sp = score (best + 1);
            const float denom = sm - 2.0f * bestScore + sp;
            if (denom < -1.0e-12f)
                frac = juce::jlimit (-0.5f, 0.5f, 0.5f * (sm - sp) / denom);
        }

        // Keep the exact fractional lag between the two taps that the search chose.
        const float lag = (float) (base + best - ref) + frac;
        return juce::jlimit ((float) -search - 1.0f, (float) search + 1.0f, otherDelay + lag - newBase);
    }

    int window = 1344, search = 256, stride = 3, coarse = 6;
    float lo = 260.0f, invWindow = 1.0f / 1344.0f, parkStep = 0.001f, glide = 0.01f;
    float phase = 0.5f, offA = 0.0f, offB = 0.0f;
    float st = 0.0f, target = 0.0f, ratio = 1.0f, park = 1.0f;
    std::array<float, segPoints> refSeg {};
};

// Grain size for the shifter-based pedals: shifted voices trail the dry by ~932 samples (19.4 ms) at 48 kHz.
constexpr float shiftWindowMs = 28.0f;
constexpr float shiftSearchMs = 5.333f;

struct Interval { const char* name; float semis; };

const Interval harmonyIntervals[] {
    { "-Oct", -12.0f }, { "-5th", -7.0f }, { "-4th", -5.0f }, { "-3rd", -4.0f }, { "-m3", -3.0f },
    { "+m3", 3.0f }, { "+3rd", 4.0f }, { "+4th", 5.0f }, { "+5th", 7.0f }, { "+6th", 9.0f },
    { "+m7", 10.0f }, { "+Oct", 12.0f }, { "+Oct 5th", 19.0f }, { "+2 Oct", 24.0f }
};
constexpr int numHarmonyIntervals = (int) std::size (harmonyIntervals);

juce::StringArray harmonyNames (bool withOff)
{
    juce::StringArray a;
    if (withOff) a.add ("Off");
    for (auto& i : harmonyIntervals) a.add (i.name);
    return a;
}

//==============================================================================
// UNDERTOW: polyphonic octaver. Grain-shifted -1 oct, -2 oct and +1 oct voices.
//==============================================================================
class Octaver final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            for (auto& v : c.voices) v.prepare (sampleRate, shiftWindowMs, shiftSearchMs);
            c.line.setMaxSamples (c.voices[0].span());
            for (auto& g : c.gains) g.reset (sampleRate, 0.03);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.line.clear();
            for (size_t i = 0; i < c.voices.size(); ++i)
                c.voices[i].setSemitones (shifts[i], true);
            c.tone.reset (sampleRate);
        }
        fresh = true;
    }

    void update() override
    {
        for (auto& c : channels)
        {
            for (size_t i = 0; i < c.gains.size(); ++i)
            {
                // Sub 2 is quieter by nature of its range; give it a touch more gain.
                const float g = knob ((int) i) * (i == 1 ? 1.2f : 1.0f);
                if (fresh) c.gains[i].setCurrentAndTargetValue (g); else c.gains[i].setTargetValue (g);
            }
            c.tone.setTarget (knob (4));
            if (fresh) c.tone.reset (sampleRate);
            c.tone.step (sampleRate);
        }
        fresh = false;
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];
            c.line.push (in);
            float wet = 0.0f;

            for (size_t v = 0; v < c.voices.size(); ++v)
            {
                const float g = c.gains[v].getNextValue();
                if (g > 1.0e-5f || c.gains[v].isSmoothing())
                    wet += g * c.voices[v].process (c.line);
            }

            const float dry = c.gains[3].getNextValue() * in;
            x[i] = dry + c.tone.process (wet);
        }
    }

private:
    static constexpr std::array<float, 3> shifts { -12.0f, -24.0f, 12.0f };

    struct Channel
    {
        dsp::DelayBuffer line;
        std::array<GrainVoice, 3> voices;
        std::array<juce::SmoothedValue<float>, 4> gains;   // sub1, sub2, up, dry
        ToneFilter tone;
    };

    std::array<Channel, 2> channels;
    bool fresh = true;
};

//==============================================================================
// GRAVEL: analog-style octave. Flip-flop dividers for the subs, a full-wave
// rectifier for the octave up. Monophonic, zero latency.
//==============================================================================
class DividerOctave final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
            for (auto& g : c.gains) g.reset (sampleRate, 0.03);

        hpCoeff = dsp::onePoleCoeff (60.0f, sampleRate);
        dcCoeff = dsp::onePoleCoeff (25.0f, sampleRate);
        attack = dsp::onePoleCoeff (300.0f, sampleRate);
        release = dsp::onePoleCoeff (6.0f, sampleRate);
        edgeSmooth = dsp::onePoleCoeff (900.0f, sampleRate);
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.hpState = c.dcState = c.env = c.trackEnv = 0.0f;
            c.high = c.ff1 = c.ff2 = false;
            c.sq1 = c.sq2 = 0.0f;
            c.track.reset();
            c.tone.reset (sampleRate);
        }
        fresh = true;
    }

    void update() override
    {
        for (auto& c : channels)
        {
            for (size_t i = 0; i < c.gains.size(); ++i)
                if (fresh) c.gains[i].setCurrentAndTargetValue (knob ((int) i)); else c.gains[i].setTargetValue (knob ((int) i));

            c.track.setup (knob (5), 0.6f, sampleRate);
            c.tone.setTarget (knob (4));
            if (fresh) c.tone.reset (sampleRate);
            c.tone.step (sampleRate);
        }
        fresh = false;
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];

            // Strip rumble, then isolate the fundamental for the edge detector.
            c.hpState += hpCoeff * (in - c.hpState);
            const float hp = in - c.hpState;
            const float fundamental = c.track.process (hp).low;

            const float a = std::abs (hp);
            c.env += (a > c.env ? attack : release) * (a - c.env);
            const float fa = std::abs (fundamental);
            c.trackEnv += (fa > c.trackEnv ? attack : release) * (fa - c.trackEnv);

            // Schmitt trigger with hysteresis that scales with the note, so decays keep tracking.
            const float threshold = 0.2f * c.trackEnv;
            if (! c.high && fundamental > threshold && threshold > 0.0f)
            {
                c.high = true;
                c.ff1 = ! c.ff1;
                if (c.ff1) c.ff2 = ! c.ff2;
            }
            else if (c.high && fundamental < -threshold)
            {
                c.high = false;
            }

            // Slightly rounded edges: less fizz than a raw square.
            c.sq1 += edgeSmooth * ((c.ff1 ? 1.0f : -1.0f) - c.sq1);
            c.sq2 += edgeSmooth * ((c.ff2 ? 1.0f : -1.0f) - c.sq2);

            // Full-wave rectifier doubles the frequency; remove the DC it creates.
            const float rect = std::abs (hp);
            c.dcState += dcCoeff * (rect - c.dcState);
            const float up = 1.6f * (rect - c.dcState);

            const float wet = c.env * (0.7f * c.gains[0].getNextValue() * c.sq1 + 0.7f * c.gains[1].getNextValue() * c.sq2)
                              + c.gains[2].getNextValue() * up;

            x[i] = c.gains[3].getNextValue() * in + c.tone.process (wet);
        }
    }

private:
    struct Channel
    {
        dsp::Svf track;
        ToneFilter tone;
        std::array<juce::SmoothedValue<float>, 4> gains;   // sub1, sub2, up, dry
        float hpState = 0.0f, dcState = 0.0f, env = 0.0f, trackEnv = 0.0f, sq1 = 0.0f, sq2 = 0.0f;
        bool high = false, ff1 = false, ff2 = false;
    };

    std::array<Channel, 2> channels;
    float hpCoeff = 0.0f, dcCoeff = 0.0f, attack = 0.0f, release = 0.0f, edgeSmooth = 0.0f;
    bool fresh = true;
};

//==============================================================================
// SIDESTEP: pitch shifter, +/-24 semitones with fine detune.
//==============================================================================
class PitchShifter final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.voice.prepare (sampleRate, shiftWindowMs, shiftSearchMs);
            c.line.setMaxSamples (c.voice.span());
            c.mix.reset (sampleRate, 0.03);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.line.clear();
            c.voice.reset();
            c.tone.reset (sampleRate);
        }
        fresh = true;
    }

    void update() override
    {
        const float semis = (float) juce::roundToInt (knob (0)) + knob (1) * 0.01f;

        for (auto& c : channels)
        {
            c.voice.setSemitones (semis, fresh);
            if (fresh) c.mix.setCurrentAndTargetValue (knob (2)); else c.mix.setTargetValue (knob (2));
            c.tone.setTarget (knob (3));
            if (fresh) c.tone.reset (sampleRate);
            c.tone.step (sampleRate);
        }
        fresh = false;
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];
            c.line.push (in);
            const float wet = c.tone.process (c.voice.process (c.line));
            float dg, wg;
            mixGains (c.mix.getNextValue(), c.voice.semitones(), dg, wg);
            x[i] = dg * in + wg * wet;
        }
    }

private:
    struct Channel
    {
        dsp::DelayBuffer line;
        GrainVoice voice;
        juce::SmoothedValue<float> mix;
        ToneFilter tone;
    };

    std::array<Channel, 2> channels;
    bool fresh = true;
};

//==============================================================================
// CHOIRBOY: dry plus two fixed-interval voices spread across the stereo field.
//==============================================================================
class Harmony final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        for (auto& v : voices) v.prepare (sampleRate, shiftWindowMs, shiftSearchMs);
        mono.setMaxSamples (voices[0].span());

        for (auto* s : { &level1L, &level1R, &level2L, &level2R, &dryGain })
            s->reset (sampleRate, 0.04);
    }

    void reset() override
    {
        mono.clear();
        for (auto& v : voices) v.reset();
        toneL.reset (sampleRate);
        toneR.reset (sampleRate);
        fresh = true;
    }

    void update() override
    {
        const int i1 = juce::jlimit (0, numHarmonyIntervals - 1, juce::roundToInt (knob (0)));
        const int i2 = juce::jlimit (0, numHarmonyIntervals, juce::roundToInt (knob (2)));
        const bool second = i2 > 0;

        voices[0].setSemitones (harmonyIntervals[i1].semis, fresh);
        if (second)
            voices[1].setSemitones (harmonyIntervals[i2 - 1].semis, fresh || ! secondOn);
        secondOn = second;

        // With one voice, it sits in the middle; with two, they fan out.
        const float spread = knob (4);
        const float pan1 = second ? -spread : 0.0f;
        const float pan2 = spread;
        const float l1 = knob (1), l2 = second ? knob (3) : 0.0f;

        auto set = [this] (juce::SmoothedValue<float>& s, float v) { if (fresh) s.setCurrentAndTargetValue (v); else s.setTargetValue (v); };
        set (level1L, l1 * panLeft (pan1));
        set (level1R, l1 * panRight (pan1));
        set (level2L, l2 * panLeft (pan2));
        set (level2R, l2 * panRight (pan2));
        set (dryGain, knob (5));

        for (auto* t : { &toneL, &toneR })
        {
            t->setTarget (knob (6));
            if (fresh) t->reset (sampleRate);
            t->step (sampleRate);
        }
        fresh = false;
    }

    void processChannel (int ch, float* x, int n) override
    {
        // Only reached if the chain ever runs this pedal mono; treat as both sides summed.
        if (ch == 0) processStereo (x, x, n);
    }

    void processStereo (float* left, float* right, int n) override
    {
        const bool aliased = left == right;

        for (int i = 0; i < n; ++i)
        {
            const float inL = left[i], inR = right[i];
            mono.push (0.5f * (inL + inR));

            const float g1L = level1L.getNextValue(), g1R = level1R.getNextValue();
            const float g2L = level2L.getNextValue(), g2R = level2R.getNextValue();
            const float v1 = voices[0].process (mono);
            const float v2 = (g2L + g2R > 1.0e-5f || level2L.isSmoothing()) ? voices[1].process (mono) : 0.0f;
            const float d = dryGain.getNextValue();

            left[i] = d * inL + toneL.process (g1L * v1 + g2L * v2);
            if (! aliased) right[i] = d * inR + toneR.process (g1R * v1 + g2R * v2);
        }
    }

private:
    dsp::DelayBuffer mono;
    std::array<GrainVoice, 2> voices;
    juce::SmoothedValue<float> level1L, level1R, level2L, level2R, dryGain;
    ToneFilter toneL, toneR;
    bool fresh = true, secondOn = false;
};

//==============================================================================
// TWOFOLD: detune doubler. Two copies shifted a few cents either way, slightly
// late and panned wide; the dry path is untouched.
//==============================================================================
class Doubler final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        for (auto& v : voices)
        {
            v.prepare (sampleRate, 12.0f, 2.5f);
            v.setGlide (40.0f, sampleRate);
        }
        maxExtra = (int) std::ceil (0.03 * 1.4 * sampleRate) + 4;
        mono.setMaxSamples (voices[0].span (maxExtra));
        delay.reset (sampleRate, 0.25);
        for (auto* s : { &gAL, &gAR, &gBL, &gBR, &dryGain }) s->reset (sampleRate, 0.04);
    }

    void reset() override
    {
        mono.clear();
        for (auto& v : voices) v.reset();
        fresh = true;
    }

    void update() override
    {
        const float cents = knob (0);
        voices[0].setSemitones (-cents * 0.01f, fresh);
        voices[1].setSemitones (cents * 0.01f, fresh);

        const float d = knob (1) * 0.001f * (float) sampleRate;
        if (fresh) delay.setCurrentAndTargetValue (d); else delay.setTargetValue (d);

        // Wet level plus a gentle trim so the doubled sound stays at the dry's loudness.
        const float mix = knob (3), width = knob (2);
        const float wet = 0.75f * mix;
        const float trim = 1.0f / std::sqrt (1.0f + 1.1f * wet * wet);

        auto set = [this] (juce::SmoothedValue<float>& s, float v) { if (fresh) s.setCurrentAndTargetValue (v); else s.setTargetValue (v); };
        set (gAL, trim * wet * panLeft (-width));
        set (gAR, trim * wet * panRight (-width));
        set (gBL, trim * wet * panLeft (width));
        set (gBR, trim * wet * panRight (width));
        set (dryGain, trim);
        fresh = false;
    }

    void processChannel (int ch, float* x, int n) override
    {
        if (ch == 0) processStereo (x, x, n);
    }

    void processStereo (float* left, float* right, int n) override
    {
        const bool aliased = left == right;
        const float maxD = (float) maxExtra / 1.4f - 4.0f;

        for (int i = 0; i < n; ++i)
        {
            const float inL = left[i], inR = right[i];
            mono.push (0.5f * (inL + inR));

            const float d = juce::jlimit (0.0f, maxD, delay.getNextValue());
            const float a = voices[0].process (mono, d);
            const float b = voices[1].process (mono, d * 1.37f);
            const float g = dryGain.getNextValue();
            const float al = gAL.getNextValue(), ar = gAR.getNextValue(), bl = gBL.getNextValue(), br = gBR.getNextValue();

            left[i] = g * inL + al * a + bl * b;
            if (! aliased) right[i] = g * inR + ar * a + br * b;
        }
    }

private:
    dsp::DelayBuffer mono;
    std::array<GrainVoice, 2> voices;
    juce::SmoothedValue<float> delay, gAL, gAR, gBL, gBR, dryGain;
    int maxExtra = 2048;
    bool fresh = true;
};

//==============================================================================
// SLINGSHOT: expression pitch bend. Position sweeps from unison to the chosen
// interval; at rest the pedal passes the guitar through untouched.
//==============================================================================
const Interval bendRanges[] {
    { "+1 Oct", 12.0f }, { "+2 Oct", 24.0f }, { "-1 Oct", -12.0f }, { "-2 Oct", -24.0f },
    { "+5th", 7.0f }, { "+4th", 5.0f }, { "-2nd", -2.0f }, { "Dive", -36.0f }
};
constexpr int numBendRanges = (int) std::size (bendRanges);

class Bend final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.voice.prepare (sampleRate, shiftWindowMs, shiftSearchMs);
            c.line.setMaxSamples (c.voice.span());
            c.mix.reset (sampleRate, 0.03);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.line.clear();
            c.voice.reset();
        }
        fresh = true;
    }

    void update() override
    {
        const int range = juce::jlimit (0, numBendRanges - 1, juce::roundToInt (knob (1)));
        const float semis = juce::jlimit (0.0f, 1.0f, knob (0)) * bendRanges[range].semis;

        for (auto& c : channels)
        {
            c.voice.setGlide (knob (3), sampleRate);
            c.voice.setSemitones (semis, fresh);
            if (fresh) c.mix.setCurrentAndTargetValue (knob (2)); else c.mix.setTargetValue (knob (2));
        }
        fresh = false;
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];
            c.line.push (in);
            const float wet = c.voice.process (c.line);
            float dg, wg;
            mixGains (c.mix.getNextValue(), c.voice.semitones(), dg, wg);
            x[i] = dg * in + wg * wet;
        }
    }

private:
    struct Channel
    {
        dsp::DelayBuffer line;
        GrainVoice voice;
        juce::SmoothedValue<float> mix;
    };

    std::array<Channel, 2> channels;
    bool fresh = true;
};

//==============================================================================
// STAIRWELL: pitch sequencer. A shifted voice steps through a pattern of
// intervals with optional glide and a per-step gate.
//==============================================================================
struct Pattern { const char* name; int length; std::array<float, 8> steps; };

const Pattern stepPatterns[] {
    { "Octaves", 2, { 0.0f, 12.0f } },
    { "Power",   4, { 0.0f, 7.0f, 12.0f, 7.0f } },
    { "Major",   4, { 0.0f, 4.0f, 7.0f, 12.0f } },
    { "Minor",   4, { 0.0f, 3.0f, 7.0f, 12.0f } },
    { "Climb",   8, { 0.0f, 7.0f, 12.0f, 19.0f, 24.0f, 19.0f, 12.0f, 7.0f } },
    { "Sub",     4, { 0.0f, -12.0f, -5.0f, -12.0f } },
    { "Random",  0, { -12.0f, 0.0f, 5.0f, 7.0f, 12.0f, 19.0f } }
};
constexpr int numStepPatterns = (int) std::size (stepPatterns);
constexpr int randomPoolSize = 6;

class Steps final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        voice.prepare (sampleRate, shiftWindowMs, shiftSearchMs);
        mono.setMaxSamples (voice.span());
        mix.reset (sampleRate, 0.03);
        gateCoeff = dsp::onePoleCoeff (60.0f, sampleRate);
    }

    void reset() override
    {
        mono.clear();
        voice.reset();
        tone.reset (sampleRate);
        clock = 0.0;
        step = 0;
        gateEnv = 1.0f;
        rng.setSeed (0x5eed);
        fresh = true;
    }

    void update() override
    {
        rateHz = knob (0);
        pattern = juce::jlimit (0, numStepPatterns - 1, juce::roundToInt (knob (1)));
        gate = knob (3);

        // Glide is a fraction of the step length.
        voice.setGlide (juce::jmax (1.0f, knob (2) * 1000.0f / juce::jmax (0.1f, rateHz) / 3.0f), sampleRate);

        if (fresh) mix.setCurrentAndTargetValue (knob (4)); else mix.setTargetValue (knob (4));

        tone.setTarget (knob (5));
        if (fresh) { tone.reset (sampleRate); voice.setSemitones (stepSemis(), true); }
        tone.step (sampleRate);
        fresh = false;
    }

    void processChannel (int ch, float* x, int n) override
    {
        if (ch == 0) processStereo (x, x, n);
    }

    void processStereo (float* left, float* right, int n) override
    {
        const bool aliased = left == right;
        const double inc = (double) rateHz / sampleRate;

        for (int i = 0; i < n; ++i)
        {
            clock += inc;
            if (clock >= 1.0)
            {
                clock -= std::floor (clock);
                advance();
                voice.setSemitones (stepSemis(), false);
            }

            gateEnv += gateCoeff * (((float) clock < gate ? 1.0f : 0.0f) - gateEnv);

            const float inL = left[i], inR = right[i];
            mono.push (0.5f * (inL + inR));

            float d, wg;
            mixGains (mix.getNextValue(), voice.semitones(), d, wg);
            const float w = wg * gateEnv * tone.process (voice.process (mono));
            left[i] = d * inL + w;
            if (! aliased) right[i] = d * inR + w;
        }
    }

private:
    void advance()
    {
        const auto& p = stepPatterns[pattern];
        if (p.length == 0)
            randomSemis = p.steps[(size_t) rng.nextInt (randomPoolSize)];
        else
            step = (step + 1) % p.length;
    }

    float stepSemis() const
    {
        const auto& p = stepPatterns[pattern];
        return p.length == 0 ? randomSemis : p.steps[(size_t) (step % p.length)];
    }

    dsp::DelayBuffer mono;
    GrainVoice voice;
    juce::SmoothedValue<float> mix;
    ToneFilter tone;
    juce::Random rng;
    double clock = 0.0;
    float rateHz = 5.0f, gate = 0.8f, gateEnv = 1.0f, gateCoeff = 0.01f, randomSemis = 12.0f;
    int pattern = 1, step = 0;
    bool fresh = true;
};

juce::StringArray patternNames()
{
    juce::StringArray a;
    for (auto& p : stepPatterns) a.add (p.name);
    return a;
}

juce::StringArray bendNames()
{
    juce::StringArray a;
    for (auto& r : bendRanges) a.add (r.name);
    return a;
}

} // namespace

void registerPitchPedals (Registry& r)
{
    r.add<Octaver> ({ "pitch.octave", "UNDERTOW", "polyphonic octaver", "Pitch",
                      "One and two octaves down plus one up; tracks chords cleanly",
                      juce::Colour (0xff1f3a46), juce::Colour (0xff9fe0d6),
                      { knob ("sub1", "Sub 1", 0.0f, 1.0f, 0.6f, "%"),
                        knob ("sub2", "Sub 2", 0.0f, 1.0f, 0.0f, "%"),
                        knob ("up", "Up", 0.0f, 1.0f, 0.0f, "%"),
                        knob ("dry", "Dry", 0.0f, 1.0f, 1.0f, "%"),
                        knob ("tone", "Tone", 300.0f, 9000.0f, 2400.0f, "Hz", 0.4f) } });

    r.add<DividerOctave> ({ "pitch.divider", "GRAVEL", "analog octave", "Pitch",
                            "Flip-flop square subs and a rectified octave up; single notes, zero latency",
                            juce::Colour (0xff3b3b38), juce::Colour (0xfff0b44c),
                            { knob ("sub1", "Sub 1", 0.0f, 1.0f, 0.5f, "%"),
                              knob ("sub2", "Sub 2", 0.0f, 1.0f, 0.0f, "%"),
                              knob ("up", "Up", 0.0f, 1.0f, 0.0f, "%"),
                              knob ("dry", "Dry", 0.0f, 1.0f, 1.0f, "%"),
                              knob ("tone", "Tone", 200.0f, 6000.0f, 900.0f, "Hz", 0.4f),
                              knob ("track", "Track", 120.0f, 800.0f, 320.0f, "Hz", 0.5f) } });

    r.add<PitchShifter> ({ "pitch.shift", "SIDESTEP", "pitch shifter", "Pitch",
                           "Moves your signal up or down two octaves with fine detune",
                           juce::Colour (0xff5b2a4e), juce::Colour (0xfff4d9e8),
                           { knob ("shift", "Shift", -24.0f, 24.0f, 12.0f, "st"),
                             knob ("fine", "Cents", -50.0f, 50.0f, 0.0f),
                             knob ("mix", "Mix", 0.0f, 1.0f, 0.4f, "%"),
                             knob ("tone", "Tone", 500.0f, 12000.0f, 7000.0f, "Hz", 0.4f) } });

    r.add<Harmony> ({ "pitch.harmony", "CHOIRBOY", "interval harmony", "Pitch",
                      "Up to two parallel voices at fixed intervals, spread in stereo",
                      juce::Colour (0xffe9e1cf), juce::Colour (0xff6b2430),
                      { choice ("voice1", "Voice 1", harmonyNames (false), 8),
                        knob ("level1", "Level 1", 0.0f, 1.0f, 0.5f, "%"),
                        choice ("voice2", "Voice 2", harmonyNames (true), 12),
                        knob ("level2", "Level 2", 0.0f, 1.0f, 0.3f, "%"),
                        knob ("spread", "Spread", 0.0f, 1.0f, 0.6f, "%"),
                        knob ("dry", "Dry", 0.0f, 1.0f, 1.0f, "%"),
                        knob ("tone", "Tone", 500.0f, 12000.0f, 6000.0f, "Hz", 0.4f) } });

    r.add<Doubler> ({ "pitch.doubler", "TWOFOLD", "detune doubler", "Pitch",
                      "Two slightly detuned, slightly late copies panned wide",
                      juce::Colour (0xff2e4a2f), juce::Colour (0xffe6efc9),
                      { knob ("detune", "Cents", 2.0f, 30.0f, 9.0f),
                        knob ("delay", "Delay", 0.0f, 30.0f, 11.0f, "ms"),
                        knob ("width", "Width", 0.0f, 1.0f, 1.0f, "%"),
                        knob ("mix", "Mix", 0.0f, 1.0f, 0.6f, "%") } });

    r.add<Bend> ({ "pitch.bend", "SLINGSHOT", "expression pitch bend", "Pitch",
                   "Sweep Position from unison to the chosen interval; built for an expression pedal",
                   juce::Colour (0xffc8452c), juce::Colour (0xfffff1dc),
                   { knob ("position", "Position", 0.0f, 1.0f, 1.0f, "%"),
                     choice ("range", "Range", bendNames(), 0),
                     knob ("mix", "Mix", 0.0f, 1.0f, 0.5f, "%"),
                     knob ("glide", "Glide", 2.0f, 500.0f, 40.0f, "ms", 0.4f) } });

    r.add<Steps> ({ "pitch.steps", "STAIRWELL", "pitch sequencer", "Pitch",
                    "A shifted voice that climbs through interval patterns in time",
                    juce::Colour (0xff23284a), juce::Colour (0xffb9c4ff),
                    { knob ("rate", "Rate", 0.5f, 16.0f, 6.0f, "Hz", 0.5f),
                      choice ("pattern", "Pattern", patternNames(), 1),
                      knob ("glide", "Glide", 0.0f, 1.0f, 0.1f, "%"),
                      knob ("gate", "Gate", 0.1f, 1.0f, 0.85f, "%"),
                      knob ("mix", "Mix", 0.0f, 1.0f, 0.55f, "%"),
                      knob ("tone", "Tone", 500.0f, 12000.0f, 7000.0f, "Hz", 0.4f) } });
}

} // namespace woodshed
