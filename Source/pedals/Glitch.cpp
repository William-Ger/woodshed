#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// Shared building blocks for the glitch family
//==============================================================================

constexpr float pi = juce::MathConstants<float>::pi;
constexpr float twoPi = juce::MathConstants<float>::twoPi;

/** Small xorshift PRNG: realtime-safe and deterministic per pedal. */
struct Rng
{
    uint32_t s = 0x2545f491u;

    void seed (uint32_t v)
    {
        s = v * 0x9e3779b1u + 0x7f4a7c15u;
        if (s == 0) s = 0x2545f491u;
        for (int i = 0; i < 4; ++i) next();
    }

    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }

    float uni()                                 { return (float) (next() >> 8) * (1.0f / 16777216.0f); }
    float bi()                                  { return uni() * 2.0f - 1.0f; }
};

/** Linear below half the ceiling, then a smooth knee up to it. */
inline float softLimit (float x, float ceiling = 1.6f)
{
    const float knee = 0.5f * ceiling;
    const float a = std::abs (x);
    if (a <= knee) return x;
    const float over = (a - knee) / (ceiling - knee);
    return std::copysign (knee + (ceiling - knee) * over / (1.0f + over), x);
}

inline float smoothstep (float x)
{
    x = juce::jlimit (0.0f, 1.0f, x);
    return x * x * (3.0f - 2.0f * x);
}

/** Balance-style pan: centre is unity on both sides, hard pan silences one side. */
inline float balL (float p)                     { return juce::jmin (1.0f, 1.0f - p); }
inline float balR (float p)                     { return juce::jmin (1.0f, 1.0f + p); }

/** Equal-power gains for mixing two uncorrelated signals at a 0..1 mix. */
inline void mixGains (float mix, float& dry, float& wet)
{
    const float m = juce::jlimit (0.0f, 1.0f, mix);
    const float d = 1.0f - m;
    const float c = 1.0f / std::sqrt (d * d + m * m);
    dry = d * c;
    wet = m * c;
}

struct OnePole
{
    float z = 0.0f, a = 1.0f;

    void setHz (float hz, double sr)            { a = dsp::onePoleCoeff (juce::jlimit (5.0f, (float) sr * 0.45f, hz), sr); }
    void reset()                                { z = 0.0f; }
    float lp (float x)                          { z += a * (x - z); return z; }
    float lpWith (float x, float coeff)         { z += coeff * (x - z); return z; }
    float hp (float x)                          { return x - lp (x); }
};

/** Hann window lookup, u in [0, 1]. Built once at static init. */
struct HannTable
{
    static constexpr int size = 2048;
    std::array<float, size + 2> t {};

    HannTable()
    {
        for (int i = 0; i <= size + 1; ++i)
            t[(size_t) i] = 0.5f - 0.5f * std::cos (twoPi * (float) juce::jmin (i, size) / (float) size);
    }

    float operator() (float u) const
    {
        if (u <= 0.0f || u >= 1.0f) return 0.0f;
        const float p = u * (float) size;
        const int i = (int) p;
        return t[(size_t) i] + (p - (float) i) * (t[(size_t) i + 1] - t[(size_t) i]);
    }
};

const HannTable hann;

/** Picks out new notes: a fast envelope jumping well above a slow one. */
struct OnsetDetector
{
    float fast = 0.0f, slow = 0.0f, threshold = 0.01f;
    float fa = 1.0f, fr = 1.0f, sa = 1.0f, sr = 1.0f;
    int hold = 0, holdLen = 3000;

    void prepare (double rate)
    {
        auto c = [rate] (float ms) { return 1.0f - std::exp (-1.0f / (ms * 0.001f * (float) rate)); };
        fa = c (0.3f); fr = c (25.0f); sa = c (30.0f); sr = c (300.0f);
        holdLen = (int) (0.07 * rate);
    }

    void reset()                                { fast = slow = 0.0f; hold = 0; }
    void setThresholdDb (float db)              { threshold = dsp::dbToGain (db); }

    bool process (float x)
    {
        const float a = std::abs (x);
        fast += (a > fast ? fa : fr) * (a - fast);
        slow += (a > slow ? sa : sr) * (a - slow);
        if (fast < 1.0e-12f) fast = 0.0f;
        if (slow < 1.0e-12f) slow = 0.0f;

        if (hold > 0) { --hold; return false; }
        if (fast > threshold && fast > 2.0f * slow) { hold = holdLen; return true; }
        return false;
    }
};

/** Sample at an integer delay (0 = newest). */
inline float tapAt (const dsp::DelayBuffer& b, int delay)
{
    return b.data[(size_t) ((b.writePos - 1 - delay) & b.mask)];
}

/** Read with the delay clamped into the buffer's safe range. */
inline float readSafe (const dsp::DelayBuffer& b, float delay)
{
    return b.read (juce::jlimit (2.0f, (float) (b.mask - 8), delay));
}

/** Sizes per-block control arrays; the chain never sends more than maxBlock, but keep headroom. */
inline int controlCapacity (int maxBlock)       { return juce::jmax (maxBlock, 2048); }

//==============================================================================
/**
    Pitch-shifted voice over a delay line: two crossfading taps whose splice
    points are lined up by a short cross-correlation search. Parks onto the
    undelayed input at exact unison.
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
        glide = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, 0.015f * (float) sr));
        reset();
    }

    int centre() const                          { return minDelay + search + window / 2; }
    int span (int extra = 0) const              { return minDelay + 2 * search + window + segPoints * stride + extra + 16; }

    void reset()
    {
        phase = 0.5f;
        offA = offB = 0.0f;
        st = target;
        ratio = std::exp2 (st / 12.0f);
        park = isUnity (st) ? 1.0f : 0.0f;
    }

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

        float frac = 0.0f;
        if (best > -search && best < search)
        {
            const float sm = score (best - 1), sp = score (best + 1);
            const float denom = sm - 2.0f * bestScore + sp;
            if (denom < -1.0e-12f)
                frac = juce::jlimit (-0.5f, 0.5f, 0.5f * (sm - sp) / denom);
        }

        const float lag = (float) (base + best - ref) + frac;
        return juce::jlimit ((float) -search - 1.0f, (float) search + 1.0f, otherDelay + lag - newBase);
    }

    int window = 1344, search = 256, stride = 3, coarse = 6;
    float lo = 260.0f, invWindow = 1.0f / 1344.0f, parkStep = 0.001f, glide = 0.01f;
    float phase = 0.5f, offA = 0.0f, offB = 0.0f;
    float st = 0.0f, target = 0.0f, ratio = 1.0f, park = 1.0f;
    std::array<float, segPoints> refSeg {};
};

/** Base for pedals that always run both channels together. */
class StereoPedal : public Pedal
{
public:
    bool producesStereo() const override        { return true; }
    void processChannel (int, float*, int) override {}
};

constexpr float shiftWindowMs = 28.0f;
constexpr float shiftSearchMs = 5.333f;

const juce::StringArray noteDivisions { "1/4", "1/8", "1/16", "1/32", "1/64" };
constexpr float noteFractions[] { 1.0f, 0.5f, 0.25f, 0.125f, 0.0625f };

//==============================================================================
// RELAY: buffer-repeat stutter. Grabs the last slice and machine-guns it,
// on a random beat grid, on every pick, or latched forever.
//==============================================================================
class Relay final : public Pedal
{
public:
    void prepared() override
    {
        maxSlice = (int) (sampleRate * 1.05);
        for (auto& c : channels)
        {
            c.ring.setMaxSamples ((int) (sampleRate * 2.2));
            c.slice.assign ((size_t) maxSlice + 8, 0.0f);
        }

        const auto cap = (size_t) controlCapacity (maxBlock);
        ctlPos.assign (cap, -1.0f);
        ctlWet.assign (cap, 0.0f);
        ctlDry.assign (cap, 1.0f);
        onset.prepare (sampleRate);
        onset.setThresholdDb (-42.0f);
        fade = (float) (0.0025 * sampleRate);
        mixCoeff = dsp::onePoleCoeff (30.0f, sampleRate);
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.ring.clear();
            std::fill (c.slice.begin(), c.slice.end(), 0.0f);
        }

        onset.reset();
        rng.seed (1234);
        clock = 0.0f;
        active = playing = infinite = latched = false;
        pending = -1;
        gate = 0.0f;
        mix = knob (6);
        capIndex = -1;
        lastMode = -1;
    }

    void update() override
    {
        const float tempo = juce::jlimit (40.0f, 300.0f, knob (0));
        beatLen = (float) sampleRate * 60.0f / tempo;
        sliceLen = juce::jlimit (64, maxSlice, (int) (beatLen * noteFractions[juce::jlimit (0, 4, (int) knob (1))]));
        mode = juce::jlimit (0, 2, (int) knob (2));
        chance = knob (3);
        decay = 1.0f - 0.45f * knob (4);
        warp = std::exp2 (knob (5) / 12.0f);
        mixTarget = knob (6);

        if (mode != lastMode)
        {
            if (lastMode == 2 && infinite) { infinite = false; remaining = 0.0f; active = false; }
            latched = false;
            pending = -1;
            lastMode = mode;
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        n = juce::jmin (n, (int) ctlPos.size());
        if (ch == 0) control (x, n);
        render (channels[(size_t) (ch & 1)], x, n);
    }

private:
    struct Channel
    {
        dsp::DelayBuffer ring;
        std::vector<float> slice;
    };

    void begin (int i, float duration, bool forever)
    {
        capIndex = i;
        capLen = sliceLen;
        active = playing = true;
        infinite = forever;
        remaining = duration;
        repPhase = 0.0f;
        repRate = 1.0f;
        repGain = 1.0f;
        repDur = (float) capLen;
    }

    void control (const float* x, int n)
    {
        capIndex = -1;

        for (int i = 0; i < n; ++i)
        {
            const bool hit = onset.process (x[i]);

            if (mode == 0)
            {
                clock += 1.0f;
                if (clock >= beatLen)
                {
                    clock -= beatLen;
                    if (! active && rng.uni() < chance)
                    {
                        const float beats = rng.uni() < 0.3f ? 2.0f : 1.0f;
                        begin (i, juce::jmax (beatLen * beats, 2.0f * (float) sliceLen), false);
                    }
                }
            }
            else
            {
                const bool armed = mode == 1 ? ! active : ! latched;
                if (hit && armed && pending < 0)
                    pending = sliceLen;

                if (pending >= 0 && --pending <= 0)
                {
                    pending = -1;
                    if (mode == 2) latched = true;
                    begin (i, juce::jmax (beatLen, 2.0f * (float) sliceLen), mode == 2);
                }
            }

            float pos = -1.0f, wet = 0.0f;

            if (playing)
            {
                if (! infinite) remaining -= 1.0f;
                pos = repPhase * repRate;
                wet = repGain * juce::jmin (1.0f, repPhase / fade, (repDur - repPhase) / fade);
                repPhase += 1.0f;

                if (repPhase >= repDur)
                {
                    repPhase = 0.0f;
                    repGain *= decay;
                    repRate = juce::jlimit (0.25f, 4.0f, repRate * warp);
                    repDur = juce::jmax (fade * 2.0f + 2.0f, (float) capLen / repRate);

                    if ((! infinite && remaining <= 0.0f) || repGain < 1.0e-3f)
                    {
                        // A latched loop that has faded away re-arms on the next pick.
                        playing = active = false;
                        if (infinite) { infinite = false; latched = false; }
                    }
                }
            }

            gate = playing ? juce::jmin (1.0f, gate + 1.0f / fade) : juce::jmax (0.0f, gate - 1.0f / fade);
            mix += mixCoeff * (mixTarget - mix);

            ctlPos[(size_t) i] = pos;
            ctlWet[(size_t) i] = gate * mix * juce::jmax (0.0f, wet);
            // Latched loops sit under your playing; the other modes replace it.
            ctlDry[(size_t) i] = mode == 2 ? 1.0f : 1.0f - gate * mix;
        }
    }

    void render (Channel& c, float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            c.ring.push (x[i]);

            if (i == capIndex)
                for (int j = 0; j < capLen; ++j)
                    c.slice[(size_t) j] = c.ring.data[(size_t) ((c.ring.writePos - capLen + j) & c.ring.mask)];

            float wet = 0.0f;
            const float pos = ctlPos[(size_t) i];
            if (pos >= 0.0f)
            {
                const int idx = juce::jmin ((int) pos, capLen);
                const float f = pos - (float) idx;
                wet = c.slice[(size_t) idx] + f * (c.slice[(size_t) idx + 1] - c.slice[(size_t) idx]);
            }

            x[i] = x[i] * ctlDry[(size_t) i] + wet * ctlWet[(size_t) i];
        }
    }

    std::array<Channel, 2> channels;
    std::vector<float> ctlPos, ctlWet, ctlDry;
    OnsetDetector onset;
    Rng rng;

    int maxSlice = 48000, sliceLen = 6000, capLen = 6000, capIndex = -1, mode = 0, lastMode = -1, pending = -1;
    float beatLen = 24000.0f, chance = 0.35f, decay = 1.0f, warp = 1.0f, mix = 1.0f, mixTarget = 1.0f, mixCoeff = 0.01f;
    float clock = 0.0f, fade = 120.0f, gate = 0.0f;
    float remaining = 0.0f, repPhase = 0.0f, repRate = 1.0f, repGain = 1.0f, repDur = 6000.0f;
    bool active = false, playing = false, infinite = false, latched = false;
};

//==============================================================================
// FLYWHEEL: tape stop and spin-up. The motor brakes your signal down to a
// standstill and winds it back up, on a cycle, on every pick, or by hand.
//==============================================================================
class Flywheel final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
            c.ring.setMaxSamples ((int) (sampleRate * 8.5));

        const auto cap = (size_t) controlCapacity (maxBlock);
        ctlDelay.assign (cap, 2.0f);
        ctlTape.assign (cap, 0.0f);
        ctlLive.assign (cap, 1.0f);
        ctlCoef.assign (cap, 1.0f);
        onset.prepare (sampleRate);
        duck = 1.0f / (float) (0.003 * sampleRate);
        catchStep = 1.0f / (float) (0.03 * sampleRate);
        maxDelay = (float) (sampleRate * 8.0);
    }

    void reset() override
    {
        for (auto& c : channels) { c.ring.clear(); c.lp1.reset(); c.lp2.reset(); }
        onset.reset();
        state = State::run;
        t = 0.0f;
        speed = 1.0f;
        delay = 2.0f;
        live = 1.0f;
        notePeak = 0.0f;
        lastMotor = -1;
    }

    void update() override
    {
        mode = juce::jlimit (0, 2, (int) knob (0));
        const int motor = juce::jlimit (0, 1, (int) knob (1));
        brakeLen = juce::jmax (1.0f, knob (2) * 0.001f * (float) sampleRate);
        startLen = juce::jmax (1.0f, knob (3) * 0.001f * (float) sampleRate);
        period = knob (4) * 0.001f * (float) sampleRate;
        onset.setThresholdDb (knob (5));

        if (mode == 2 && motor != lastMotor)
        {
            if (motor == 1 && (state == State::run || state == State::catchUp || state == State::start)) beginBrake();
            if (motor == 0 && (state == State::brake || state == State::stopped)) beginStart();
        }

        lastMotor = motor;
    }

    void processChannel (int ch, float* x, int n) override
    {
        n = juce::jmin (n, (int) ctlDelay.size());
        if (ch == 0) control (x, n);

        auto& c = channels[(size_t) (ch & 1)];
        for (int i = 0; i < n; ++i)
        {
            c.ring.push (x[i]);
            const float coef = ctlCoef[(size_t) i];
            const float tape = c.lp2.lpWith (c.lp1.lpWith (readSafe (c.ring, ctlDelay[(size_t) i]), coef), coef);
            x[i] = ctlLive[(size_t) i] * x[i] + ctlTape[(size_t) i] * tape;
        }
    }

private:
    enum class State { run, brake, stopped, start, catchUp };

    struct Channel
    {
        dsp::DelayBuffer ring;
        OnePole lp1, lp2;
    };

    void beginBrake()                           { state = State::brake; t = brakeLen * (1.0f - speed); }

    void beginStart()
    {
        if (state == State::brake && speed > 0.3f)
        {
            t = std::asin (juce::jmin (1.0f, speed)) * (2.0f / pi) * startLen;
        }
        else
        {
            t = 0.0f;
            speed = 0.0f;
            delay = 2.0f;
        }

        state = State::start;
    }

    void control (const float* x, int n)
    {
        // Cycle timings: brake and start shrink to fit a short period.
        const float gap = 0.06f * period;
        const float moving = brakeLen + startLen;
        const float squeeze = juce::jmin (1.0f, 0.8f * period / juce::jmax (1.0f, moving));
        const float cycBrake = brakeLen * (mode == 0 ? squeeze : 1.0f);
        const float cycStart = startLen * (mode == 0 ? squeeze : 1.0f);
        const float runLen = juce::jmax (0.0f, period - gap - cycBrake - cycStart);

        for (int i = 0; i < n; ++i)
        {
            const bool hit = onset.process (x[i]);

            if (mode == 1)
            {
                if (hit) { notePeak = 0.0f; beginStart(); }
                notePeak = juce::jmax (notePeak, onset.slow);
                const bool decayed = onset.slow < 0.125f * notePeak || onset.slow < 0.3f * onset.threshold;
                if ((state == State::run || state == State::catchUp) && notePeak > 0.0f && decayed)
                {
                    notePeak = 0.0f;
                    beginBrake();
                }
            }

            t += 1.0f;

            switch (state)
            {
                case State::run:
                    speed = 1.0f;
                    delay = 2.0f;
                    if (mode == 0 && t >= runLen) beginBrake();
                    break;

                case State::brake:
                    speed = 1.0f - t / (mode == 0 ? cycBrake : brakeLen);
                    if (speed <= 0.0f) { speed = 0.0f; state = State::stopped; t = 0.0f; }
                    break;

                case State::stopped:
                    speed = 0.0f;
                    if (mode == 0 && t >= gap) beginStart();
                    break;

                case State::start:
                    speed = std::sin (0.5f * pi * juce::jmin (1.0f, t / (mode == 0 ? cycStart : startLen)));
                    if (t >= (mode == 0 ? cycStart : startLen)) { speed = 1.0f; state = State::catchUp; t = 0.0f; }
                    break;

                case State::catchUp:
                    speed = 1.0f;
                    if (live >= 1.0f) { state = State::run; t = 0.0f; delay = 2.0f; }
                    break;
            }

            delay = juce::jmin (maxDelay, delay + 1.0f - speed);

            if (state == State::run)               live = juce::jmin (1.0f, live + duck);
            else if (state == State::catchUp)      live = juce::jmin (1.0f, live + catchStep);
            else                                   live = juce::jmax (0.0f, live - duck);

            const float sg = smoothstep (speed * 5.0f);
            ctlDelay[(size_t) i] = delay;
            ctlLive[(size_t) i] = live;
            ctlTape[(size_t) i] = (1.0f - live) * sg;
            ctlCoef[(size_t) i] = 1.0f - std::exp (-twoPi * (150.0f + 15000.0f * speed * speed) / (float) sampleRate);
        }
    }

    std::array<Channel, 2> channels;
    std::vector<float> ctlDelay, ctlTape, ctlLive, ctlCoef;
    OnsetDetector onset;
    State state = State::run;

    int mode = 1, lastMotor = -1;
    float brakeLen = 30000.0f, startLen = 12000.0f, period = 100000.0f;
    float t = 0.0f, speed = 1.0f, delay = 2.0f, live = 1.0f, notePeak = 0.0f;
    float duck = 0.007f, catchStep = 0.0007f, maxDelay = 380000.0f;
};

//==============================================================================
// STYLUS: turntable hands. Scratches the last moment you played back and
// forth, or warps it like a drunk record on a bent platter.
//==============================================================================
class Stylus final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
            c.ring.setMaxSamples ((int) (sampleRate * 3.2));   // one slowest hand cycle plus full depth

        const auto cap = (size_t) controlCapacity (maxBlock);
        ctlDelay.assign (cap, 2.0f);
        ctlGain.assign (cap, 0.0f);
        ctlCoef.assign (cap, 1.0f);
        faderCoeff = dsp::onePoleCoeff (300.0f, sampleRate);
        mixCoeff = dsp::onePoleCoeff (30.0f, sampleRate);
    }

    void reset() override
    {
        for (auto& c : channels) { c.ring.clear(); c.lp.reset(); }
        rng.seed (99);
        phase = 0.0f;
        elapsed = 0.0f;
        cycDepth = depth;
        lastHand = 0.0f;
        walk1 = walk2 = walkTarget = 0.0f;
        faderGain = 1.0f;
        mix = knob (4);
    }

    void update() override
    {
        rate = knob (0);
        depth = knob (1) * 0.001f * (float) sampleRate;
        shape = juce::jlimit (0, 3, (int) knob (2));
        fader = knob (3);
        mixTarget = knob (4);
    }

    void processChannel (int ch, float* x, int n) override
    {
        n = juce::jmin (n, (int) ctlDelay.size());
        if (ch == 0) control (n);

        auto& c = channels[(size_t) (ch & 1)];
        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            c.ring.push (dry);
            const float wet = c.lp.lpWith (readSafe (c.ring, ctlDelay[(size_t) i]), ctlCoef[(size_t) i]);
            x[i] = dry * dryGain + wet * ctlGain[(size_t) i];
        }
    }

private:
    struct Channel
    {
        dsp::DelayBuffer ring;
        OnePole lp;
    };

    /** Hand position over one stroke cycle, 0..1 of the depth. */
    float hand (float p) const
    {
        if (shape == 0)                         // baby: push forward, pull back
            return 0.5f - 0.5f * std::cos (twoPi * p);

        // tear: forward in two quick shoves, one long pull back
        if (p < 0.2f)  return 0.5f * smoothstep (p / 0.2f);
        if (p < 0.4f)  return 0.5f + 0.5f * smoothstep ((p - 0.2f) / 0.2f);
        return 1.0f - smoothstep ((p - 0.4f) / 0.6f);
    }

    void control (int n)
    {
        const float inc = rate / (float) sampleRate;
        const float walkCoeff = dsp::onePoleCoeff (rate * 1.5f, sampleRate);
        const float wetComp = shape < 2 ? 1.35f : 1.0f;

        for (int i = 0; i < n; ++i)
        {
            mix += mixCoeff * (mixTarget - mix);
            phase += inc;
            const bool wrapped = phase >= 1.0f;
            if (wrapped)
            {
                phase -= 1.0f;
                elapsed = 0.0f;
                cycDepth = depth;
                walkTarget = rng.uni() * depth;
            }

            float d, g = 1.0f, coef = 1.0f;

            if (shape < 2)
            {
                // Motor off: the record only moves under the hand, so its speed is the hand's speed.
                const float h = cycDepth * hand (phase);
                const float handSpeed = wrapped ? 0.0f : h - lastHand;
                lastHand = h;
                d = elapsed + cycDepth + 2.0f - h;
                elapsed += 1.0f;

                const float s = std::abs (handSpeed);
                g = smoothstep (s / 0.35f);
                const float target = handSpeed < 0.0f ? 1.0f - fader : 1.0f;
                faderGain += faderCoeff * (target - faderGain);
                g *= faderGain;
                coef = 1.0f - std::exp (-twoPi * (250.0f + 11000.0f * juce::jmin (1.0f, s)) / (float) sampleRate);
            }
            else if (shape == 2)
            {
                // Warped platter: the motor runs while the delay sways.
                d = 2.0f + depth * (0.5f - 0.5f * std::cos (twoPi * phase));
            }
            else
            {
                // Drunk: lurches toward a new random spot every cycle.
                walk1 += walkCoeff * (walkTarget - walk1);
                walk2 += walkCoeff * (walk1 - walk2);
                d = 2.0f + walk2;
            }

            const float wetMix = mix * wetComp;
            dryGain = 1.0f - mix;
            ctlDelay[(size_t) i] = d;
            ctlGain[(size_t) i] = g * wetMix;
            ctlCoef[(size_t) i] = coef;
        }
    }

    std::array<Channel, 2> channels;
    std::vector<float> ctlDelay, ctlGain, ctlCoef;
    Rng rng;

    int shape = 0;
    float rate = 2.0f, depth = 8000.0f, fader = 0.0f, mix = 1.0f, mixTarget = 1.0f, dryGain = 0.0f;
    float phase = 0.0f, elapsed = 0.0f, cycDepth = 8000.0f, lastHand = 0.0f;
    float walk1 = 0.0f, walk2 = 0.0f, walkTarget = 0.0f;
    float faderGain = 1.0f, faderCoeff = 0.03f, mixCoeff = 0.004f;
};

//==============================================================================
// REWIND: every note you pick plays backwards, swelling up into its own attack.
//==============================================================================
class Rewind final : public StereoPedal
{
public:
    void prepared() override
    {
        ringL.setMaxSamples ((int) (sampleRate * 6.0));
        ringR.setMaxSamples ((int) (sampleRate * 6.0));
        onset.prepare (sampleRate);
        pre = (int) (0.002 * sampleRate);
        minLen = (int) (0.06 * sampleRate);
        endFade = (float) (0.004 * sampleRate);
        gainCoeff = dsp::onePoleCoeff (30.0f, sampleRate);
    }

    void reset() override
    {
        ringL.clear(); ringR.clear();
        toneL.reset(); toneR.reset();
        onset.reset();
        for (auto& v : voices) v.active = false;
        now = 0;
        pending = -1;
        flip = false;
        dry = knob (6);
        wet = knob (7);
    }

    void update() override
    {
        lenSamples = (int) (knob (0) * 0.001f * (float) sampleRate);
        onset.setThresholdDb (knob (1));
        swellFrac = 0.15f + 0.8f * knob (2);
        static constexpr float rates[] { 0.5f, 1.0f, 2.0f };
        rate = rates[juce::jlimit (0, 2, (int) knob (3))];
        spread = knob (4);
        toneL.setHz (knob (5), sampleRate);
        toneR.a = toneL.a;
        dryTarget = knob (6);
        wetTarget = knob (7);
    }

    void processStereo (float* left, float* right, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float xl = left[i], xr = right[i];
            ringL.push (xl);
            ringR.push (xr);

            if (onset.process (0.5f * (xl + xr)))
            {
                if (pending >= 0) endSegment (now - pre);
                pending = now - pre;
            }

            if (pending >= 0 && now - pending >= lenSamples)
            {
                endSegment (now);

                // A note still ringing keeps getting chopped into back-to-back reversed pieces.
                if (onset.slow > 0.5f * onset.threshold)
                    pending = now;
            }

            float sl = 0.0f, sr = 0.0f;

            for (auto& v : voices)
            {
                if (! v.active) continue;

                const float e = (float) (now - v.end);
                const float u = e * v.rate / v.len;
                if (u >= 1.0f) { v.active = false; continue; }

                const float rise = juce::jmin (1.0f, u / swellFrac);
                const float w = rise * rise * juce::jmin (1.0f, (1.0f - u) * v.len / (v.rate * endFade));
                const float d = v.offset + e * (1.0f + v.rate);
                sl += w * v.gl * readSafe (ringL, d);
                sr += w * v.gr * readSafe (ringR, d);
            }

            dry += gainCoeff * (dryTarget - dry);
            wet += gainCoeff * (wetTarget - wet);
            left[i] = dry * xl + 1.35f * wet * toneL.lp (sl);
            right[i] = dry * xr + 1.35f * wet * toneR.lp (sr);
            ++now;
        }
    }

private:
    struct Voice
    {
        bool active = false;
        int64_t end = 0;
        float len = 1.0f, rate = 1.0f, gl = 1.0f, gr = 1.0f, offset = 0.0f;
    };

    void endSegment (int64_t end)
    {
        const int64_t len = end - pending;
        pending = -1;
        if (len < minLen) return;

        Voice* slot = &voices[0];
        for (auto& v : voices)
        {
            if (! v.active) { slot = &v; break; }
            if (v.end < slot->end) slot = &v;
        }

        flip = ! flip;
        const float p = (flip ? 1.0f : -1.0f) * spread * 0.8f;
        slot->active = true;
        slot->end = now;
        slot->len = (float) len;
        slot->offset = (float) (now - end);
        slot->rate = rate;
        slot->gl = balL (p);
        slot->gr = balR (p);
    }

    dsp::DelayBuffer ringL, ringR;
    OnePole toneL, toneR;
    OnsetDetector onset;
    std::array<Voice, 6> voices;

    int64_t now = 0, pending = -1;
    int pre = 96, minLen = 2880, lenSamples = 24000;
    float swellFrac = 0.5f, rate = 1.0f, spread = 0.5f, endFade = 192.0f;
    float dry = 0.0f, wet = 1.0f, dryTarget = 0.0f, wetTarget = 1.0f, gainCoeff = 0.004f;
    bool flip = false;
};

//==============================================================================
// SCRAPYARD: granular cloud. Sprays splinters of the last couple of seconds
// across the stereo field at scattered pitches, some of them backwards.
//==============================================================================
class Scrapyard final : public StereoPedal
{
public:
    void prepared() override
    {
        ring.setMaxSamples ((int) (sampleRate * 2.6));
        mixCoeff = dsp::onePoleCoeff (30.0f, sampleRate);
    }

    void reset() override
    {
        ring.clear();
        toneL.reset(); toneR.reset();
        for (auto& g : grains) g.active = false;
        rng.seed (777);
        spawn = 0.0f;
        mix = knob (7);
    }

    void update() override
    {
        density = knob (0);
        size = juce::jmax (64.0f, knob (1) * 0.001f * (float) sampleRate);
        scatter = knob (2) * 0.001f * (float) sampleRate;
        pitchMode = juce::jlimit (0, 3, (int) knob (3));
        reverse = knob (4);
        width = knob (5);
        toneL.setHz (knob (6), sampleRate);
        toneR.a = toneL.a;
        mixTarget = knob (7);
        norm = 1.0f / std::sqrt (juce::jmax (1.0f, 0.75f * density * size / (float) sampleRate));
    }

    void processStereo (float* left, float* right, int n) override
    {
        const float spawnInc = density / (float) sampleRate;

        for (int i = 0; i < n; ++i)
        {
            const float xl = left[i], xr = right[i];
            ring.push (0.5f * (xl + xr));

            spawn += spawnInc;
            if (spawn >= 1.0f)
            {
                spawn -= 0.4f + 1.2f * rng.uni();
                spawn = juce::jmin (spawn, 0.9f);
                launch();
            }

            float sl = 0.0f, sr = 0.0f;
            for (auto& g : grains)
            {
                if (! g.active) continue;

                const float u = g.t * g.invLen;
                if (u >= 1.0f) { g.active = false; continue; }

                const float v = hann (u) * readSafe (ring, g.delay);
                sl += g.gl * v;
                sr += g.gr * v;
                g.delay += g.drift;
                g.t += 1.0f;
            }

            mix += mixCoeff * (mixTarget - mix);
            float dg, wg;
            mixGains (mix, dg, wg);
            wg *= norm;
            left[i] = dg * xl + wg * toneL.lp (sl);
            right[i] = dg * xr + wg * toneR.lp (sr);
        }
    }

private:
    struct Grain
    {
        bool active = false;
        float t = 0.0f, invLen = 1.0f, delay = 2.0f, drift = 0.0f, gl = 1.0f, gr = 1.0f;
    };

    float pickRatio()
    {
        const float r = rng.uni();
        switch (pitchMode)
        {
            case 0:  return std::exp2 (rng.bi() * (0.08f / 12.0f));
            case 1:  return r < 0.5f ? 1.0f : (r < 0.8f ? 2.0f : 0.5f);
            case 2:  return r < 0.35f ? 1.0f : (r < 0.6f ? 1.4983f : (r < 0.8f ? 2.0f : 0.6674f));
            default: return std::exp2 (rng.bi());
        }
    }

    void launch()
    {
        Grain* slot = nullptr;
        for (auto& g : grains)
            if (! g.active) { slot = &g; break; }
        if (slot == nullptr) return;

        const float len = size * (0.7f + 0.6f * rng.uni());
        float ratio = pickRatio();
        if (rng.uni() < reverse) ratio = -ratio;

        // delay(t) = start + t * (1 - ratio); keep it inside the buffer for the whole grain.
        const float drift = 1.0f - ratio;
        const float start = 4.0f + juce::jmax (0.0f, -drift * len) + scatter * rng.uni();
        const float limit = (float) ring.mask - 16.0f - juce::jmax (0.0f, drift * len);

        const float p = rng.bi() * width;
        slot->active = true;
        slot->t = 0.0f;
        slot->invLen = 1.0f / len;
        slot->delay = juce::jmin (start, limit);
        slot->drift = drift;
        slot->gl = balL (p);
        slot->gr = balR (p);
    }

    dsp::DelayBuffer ring;
    OnePole toneL, toneR;
    std::array<Grain, 32> grains;
    Rng rng;

    int pitchMode = 1;
    float density = 14.0f, size = 5000.0f, scatter = 20000.0f, reverse = 0.3f, width = 0.8f;
    float spawn = 0.0f, mix = 0.45f, mixTarget = 0.45f, mixCoeff = 0.004f, norm = 1.0f;
};

//==============================================================================
// CRYOTRON: freezes the note you just picked into an endless drone you can
// play over. Each new note thaws the old drone and freezes the new one.
//==============================================================================
class Cryotron final : public StereoPedal
{
public:
    void prepared() override
    {
        ring.setMaxSamples ((int) (sampleRate * 1.0));
        capLen = (int) (0.22 * sampleRate);
        for (auto& s : slots) s.data.assign ((size_t) capLen + 8, 0.0f);
        grainLen = (float) (0.13 * sampleRate);
        octLen = (float) (0.08 * sampleRate);
        captureDelay = (int) (0.04 * sampleRate) + capLen;
        onset.prepare (sampleRate);
        onset.setThresholdDb (-45.0f);
        gainCoeff = dsp::onePoleCoeff (30.0f, sampleRate);
    }

    void reset() override
    {
        ring.clear();
        toneL.reset(); toneR.reset();
        onset.reset();
        rng.seed (4242);
        for (auto& s : slots)
        {
            std::fill (s.data.begin(), s.data.end(), 0.0f);
            s.live = false;
            s.gain = s.target = 0.0f;
            s.level = 0.0f;
        }
        pending = -1;
        droneGain = knob (3);
        dryGain = knob (7);
    }

    void update() override
    {
        hold = (int) knob (0) == 1;
        const float sustain = knob (1) * 0.001f;
        decayMul = (hold || sustain >= 29.5f) ? 1.0f : std::exp (-1.0f / (sustain * (float) sampleRate));
        swellStep = 1.0f / juce::jmax (1.0f, knob (2) * 0.001f * (float) sampleRate);
        droneTarget = knob (3);
        shimmer = knob (4);
        width = knob (5);
        toneL.setHz (knob (6), sampleRate);
        toneR.a = toneL.a;
        dryTarget = knob (7);
    }

    void processStereo (float* left, float* right, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float xl = left[i], xr = right[i];
            ring.push (0.5f * (xl + xr));

            bool anyHeld = false;
            for (auto& s : slots) anyHeld = anyHeld || (s.live && s.target > 0.0f);

            if (onset.process (0.5f * (xl + xr)) && ! (hold && anyHeld))
                pending = captureDelay;

            if (pending > 0) --pending;
            if (pending == 0) tryCapture();

            float dl = 0.0f, dr = 0.0f, ol = 0.0f, orr = 0.0f;

            for (auto& s : slots)
            {
                if (! s.live) continue;

                s.gain = s.target > s.gain ? juce::jmin (s.target, s.gain + swellStep)
                                           : juce::jmax (s.target, s.gain - swellStep);
                s.level *= decayMul;
                const float amp = s.gain * s.level;

                if (amp < 1.0e-5f && s.target <= 0.0f) { s.live = false; continue; }
                if (s.level < 1.0e-5f) { s.live = false; s.target = 0.0f; continue; }

                for (int h = 0; h < 3; ++h)
                {
                    dl += amp * runHead (s, s.heads[(size_t) h], grainLen, 1.0f);
                    dr += amp * runHead (s, s.heads[(size_t) h + 3], grainLen, 1.0f);
                }

                if (shimmer > 0.001f)
                {
                    ol += amp * runHead (s, s.heads[6], octLen, 2.0f);
                    orr += amp * runHead (s, s.heads[7], octLen, 2.0f);
                }
            }

            droneGain += gainCoeff * (droneTarget - droneGain);
            dryGain += gainCoeff * (dryTarget - dryGain);

            const float l = 0.94f * dl + 1.15f * shimmer * ol;
            const float r = 0.94f * dr + 1.15f * shimmer * orr;
            const float a = 0.5f * (1.0f + width), b = 0.5f * (1.0f - width);
            left[i] = dryGain * xl + droneGain * toneL.lp (a * l + b * r);
            right[i] = dryGain * xr + droneGain * toneR.lp (a * r + b * l);
        }
    }

private:
    struct Head
    {
        float pos = 0.0f, inc = 1.0f, t = 0.0f;
    };

    struct Slot
    {
        std::vector<float> data;
        std::array<Head, 8> heads;
        bool live = false;
        float gain = 0.0f, target = 0.0f, level = 0.0f;
    };

    void restartHead (Head& h, float len, float rate)
    {
        h.inc = rate * std::exp2 (rng.bi() * 0.05f / 12.0f);
        const float room = juce::jmax (1.0f, (float) capLen - len * h.inc - 4.0f);
        h.pos = 1.0f + rng.uni() * room;
        h.t = 0.0f;
    }

    float runHead (Slot& s, Head& h, float len, float rate)
    {
        if (h.t >= len) restartHead (h, len, rate);

        const float w = hann (h.t / len);
        const int idx = juce::jlimit (0, capLen, (int) h.pos);
        const float f = h.pos - (float) idx;
        const float v = s.data[(size_t) idx] + f * (s.data[(size_t) idx + 1] - s.data[(size_t) idx]);
        h.pos += h.inc;
        h.t += 1.0f;
        return w * v;
    }

    void tryCapture()
    {
        auto amp = [] (const Slot& s) { return s.live ? s.gain * s.level : 0.0f; };
        const size_t q = amp (slots[1]) < amp (slots[0]) ? 1 : 0;
        Slot* quiet = &slots[q];
        Slot* other = &slots[1 - q];

        if (quiet->live && quiet->gain * quiet->level > 0.02f)
        {
            quiet->target = 0.0f;              // let it finish fading, try again next sample
            return;
        }

        pending = -1;
        for (int j = 0; j < capLen; ++j)
            quiet->data[(size_t) j] = ring.data[(size_t) ((ring.writePos - capLen + j) & ring.mask)];

        quiet->live = true;
        quiet->gain = 0.0f;
        quiet->target = 1.0f;
        quiet->level = 1.0f;

        for (int h = 0; h < 8; ++h)
        {
            auto& head = quiet->heads[(size_t) h];
            const bool oct = h >= 6;
            const float len = oct ? octLen : grainLen;
            restartHead (head, len, oct ? 2.0f : 1.0f);
            head.t = oct ? (float) (h - 6) * 0.5f * len : (float) (h % 3) * len / 3.0f;
        }

        if (other->live) other->target = 0.0f;
    }

    dsp::DelayBuffer ring;
    OnePole toneL, toneR;
    OnsetDetector onset;
    std::array<Slot, 2> slots;
    Rng rng;

    int capLen = 10560, captureDelay = 12480, pending = -1;
    float grainLen = 6240.0f, octLen = 3840.0f;
    float decayMul = 1.0f, swellStep = 0.0001f, shimmer = 0.0f, width = 0.7f;
    float droneGain = 0.5f, droneTarget = 0.5f, dryGain = 1.0f, dryTarget = 1.0f, gainCoeff = 0.004f;
    bool hold = false;
};

//==============================================================================
// GREMLIN: glitch sequencer. Every step a gremlin decides whether to repeat,
// reverse, re-pitch, crush or drop your playing; the pattern loops per seed.
//==============================================================================
class Gremlin final : public StereoPedal
{
public:
    void prepared() override
    {
        ringL.setMaxSamples ((int) (sampleRate * 3.0));
        ringR.setMaxSamples ((int) (sampleRate * 3.0));
        fade = (float) (0.002 * sampleRate);
        panCoeff = dsp::onePoleCoeff (60.0f, sampleRate);
        mixCoeff = dsp::onePoleCoeff (30.0f, sampleRate);
    }

    void reset() override
    {
        ringL.clear(); ringR.clear();
        stepIndex = -1;
        cur = prev = {};
        k = 0.0f;
        crushL = crushR = {};
        pan = 0.0f;
        mix = knob (6);
        free.seed (31337);
        newStep();
    }

    void update() override
    {
        const float tempo = juce::jlimit (40.0f, 300.0f, knob (0));
        static constexpr float steps[] { 0.5f, 0.25f, 0.125f };
        nextStepLen = juce::jmax (256.0f, (float) sampleRate * 60.0f / tempo * steps[juce::jlimit (0, 2, (int) knob (1))]);
        stepsPerBar = (int) knob (1) == 0 ? 8 : ((int) knob (1) == 1 ? 16 : 32);
        chaos = knob (2);
        seed = (uint32_t) juce::roundToInt (knob (3));
        loop = juce::jlimit (0, 2, (int) knob (4));
        spread = knob (5);
        mixTarget = knob (6);
    }

    void processStereo (float* left, float* right, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float xl = left[i], xr = right[i];
            ringL.push (xl);
            ringR.push (xr);

            if (k >= cur.len)
                newStep();

            float ol = eventOut (cur, k, ringL, xl, crushL);
            float orr = eventOut (cur, k, ringR, xr, crushR);

            if (k < fade)
            {
                const float a = k / fade;
                ol = a * ol + (1.0f - a) * eventOut (prev, prev.len + k, ringL, xl, crushL);
                orr = a * orr + (1.0f - a) * eventOut (prev, prev.len + k, ringR, xr, crushR);
            }

            pan += panCoeff * (cur.pan - pan);
            mix += mixCoeff * (mixTarget - mix);
            left[i] = (1.0f - mix) * xl + mix * balL (pan) * ol;
            right[i] = (1.0f - mix) * xr + mix * balR (pan) * orr;
            k += 1.0f;
        }
    }

private:
    enum Kind { pass, repeat2, repeat3, repeat4, reverse, octUp, octDown, drop, crush };

    struct Event
    {
        Kind kind = pass;
        float len = 6000.0f, pan = 0.0f;
    };

    struct Crush
    {
        float held = 0.0f, env = 0.0f;
        int count = 0;
    };

    void newStep()
    {
        prev = cur;
        k = 0.0f;
        ++stepIndex;

        Rng r;
        if (loop == 2)
            r = free;
        else
        {
            const int patternLen = stepsPerBar * (loop == 0 ? 1 : 2);
            r.seed ((seed + 1u) * 0x85ebca77u ^ (uint32_t) (stepIndex % patternLen) * 0xc2b2ae3du);
        }

        Event e;
        e.len = nextStepLen;

        if (r.uni() < chaos)
        {
            const float w = r.uni();
            e.kind = w < 0.20f ? repeat2 : w < 0.32f ? repeat3 : w < 0.46f ? repeat4 : w < 0.64f ? reverse
                   : w < 0.74f ? octUp : w < 0.84f ? octDown : w < 0.91f ? drop : crush;
            e.pan = r.bi() * spread;
        }

        if (loop == 2) free = r;
        cur = e;
    }

    /** Output of an event k samples after its step began (k may run past len during the crossfade). */
    float eventOut (const Event& e, float t, const dsp::DelayBuffer& ring, float x, Crush& c) const
    {
        const float L = e.len;

        switch (e.kind)
        {
            case pass:
                return x;

            case repeat2: case repeat3: case repeat4:
            {
                const float sub = L / (float) (e.kind == repeat2 ? 2 : e.kind == repeat3 ? 3 : 4);
                const float ks = std::fmod (t, sub);
                const float w = juce::jmin (1.0f, ks / fade, (sub - ks) / fade);
                return w * readSafe (ring, t - ks + sub);
            }

            case reverse:
                return readSafe (ring, 2.0f * t + 2.0f);

            case octUp:
            {
                const float ks = std::fmod (2.0f * t, L);
                const float w = juce::jmin (1.0f, ks / (2.0f * fade), (L - ks) / (2.0f * fade));
                return w * readSafe (ring, L + t - ks);
            }

            case octDown:
                return readSafe (ring, L + 0.5f * t);

            case drop:
                return 0.0f;

            case crush:
            {
                // Sample-and-hold plus a coarse quantiser scaled to the playing level.
                c.env += 0.003f * (std::abs (x) - c.env);
                if (--c.count <= 0)
                {
                    const float step = juce::jmax (1.0e-4f, 0.6f * c.env);
                    c.count = 6;
                    c.held = step * std::round (x / step);
                }
                return c.held;
            }
        }

        return x;
    }

    dsp::DelayBuffer ringL, ringR;
    Event cur, prev;
    Crush crushL, crushR;
    Rng free;

    uint32_t seed = 13;
    int stepIndex = -1, stepsPerBar = 16, loop = 0;
    float k = 0.0f, nextStepLen = 6000.0f, chaos = 0.4f, spread = 0.6f;
    float fade = 96.0f, pan = 0.0f, panCoeff = 0.01f, mix = 1.0f, mixTarget = 1.0f, mixCoeff = 0.004f;
};

//==============================================================================
// ESCALATOR: an endlessly climbing (or sinking) shimmer. Three shifted voices
// an octave apart ride up together and fade in at the bottom as they wrap.
//==============================================================================
class Escalator final : public StereoPedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            for (auto& v : c.voices) v.prepare (sampleRate, shiftWindowMs, shiftSearchMs);
            c.ring.setMaxSamples (c.voices[0].span() + 64);
            c.wash.setMaxSamples ((int) (sampleRate * 0.25));
        }

        washTime[0] = (float) (0.143 * sampleRate);
        washTime[1] = (float) (0.197 * sampleRate);
        washLp.setHz (3500.0f, sampleRate);
        washLpR.a = washLp.a;
        mixCoeff = dsp::onePoleCoeff (30.0f, sampleRate);
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.ring.clear();
            c.wash.clear();
            c.tone.reset();
        }

        washLp.reset(); washLpR.reset();
        for (int j = 0; j < 3; ++j) u[(size_t) j] = (float) j / 3.0f;
        setVoices (true);
        mix = knob (2);
    }

    void update() override
    {
        climb = knob (0);
        low = (int) knob (1) == 0 ? -24.0f : -12.0f;
        mixTarget = knob (2);
        washAmt = knob (3);
        for (auto& c : channels) c.tone.setHz (knob (4), sampleRate);
        width = knob (5);
    }

    void processStereo (float* left, float* right, int n) override
    {
        // Advance the Shepard positions once per block; voices glide in between.
        const float du = climb / span * (float) n / (float) sampleRate;
        bool wrapped[3] {};
        for (size_t j = 0; j < 3; ++j)
        {
            u[j] += du;
            if (u[j] >= 1.0f) { u[j] -= 1.0f; wrapped[j] = true; }
            if (u[j] < 0.0f)  { u[j] += 1.0f; wrapped[j] = true; }
        }

        for (size_t j = 0; j < 3; ++j)
        {
            const float semis = low + u[j] * span;
            for (auto& c : channels) c.voices[j].setSemitones (semis, wrapped[j]);

            const float bell = std::sin (pi * u[j]);
            gainNow[j] = bell * bell * (1.0f / std::sqrt (1.125f));
            const float p = (u[j] * 2.0f - 1.0f) * width;
            panL[j] = balL (p);
            panR[j] = balR (p);
        }

        const float fb = 0.72f * washAmt;

        for (int i = 0; i < n; ++i)
        {
            const float xl = left[i], xr = right[i];
            auto& cl = channels[0];
            auto& cr = channels[1];
            cl.ring.push (xl);
            cr.ring.push (xr);

            float vl = 0.0f, vr = 0.0f;
            for (size_t j = 0; j < 3; ++j)
            {
                const float g = gainNow[j];
                vl += g * panL[j] * cl.voices[j].process (cl.ring);
                vr += g * panR[j] * cr.voices[j].process (cr.ring);
            }

            const float el = cl.wash.read (washTime[0]);
            const float er = cr.wash.read (washTime[1]);
            cl.wash.push (softLimit (vl + fb * washLp.lp (er)));
            cr.wash.push (softLimit (vr + fb * washLpR.lp (el)));

            const float wl = cl.tone.lp (vl + washAmt * el);
            const float wr = cr.tone.lp (vr + washAmt * er);

            mix += mixCoeff * (mixTarget - mix);
            float dg, wg;
            mixGains (mix, dg, wg);
            wg *= 1.0f / (1.0f + 0.6f * washAmt);
            left[i] = dg * xl + wg * wl;
            right[i] = dg * xr + wg * wr;
        }
    }

private:
    void setVoices (bool snap)
    {
        for (size_t j = 0; j < 3; ++j)
            for (auto& c : channels) c.voices[j].setSemitones (low + u[j] * span, snap);
    }

    struct Channel
    {
        dsp::DelayBuffer ring, wash;
        std::array<GrainVoice, 3> voices;
        OnePole tone;
    };

    std::array<Channel, 2> channels;
    std::array<float, 3> u {}, gainNow {}, panL {}, panR {};
    std::array<float, 2> washTime {};
    OnePole washLp, washLpR;

    static constexpr float span = 36.0f;
    float climb = 1.5f, low = -12.0f, washAmt = 0.4f, width = 0.7f;
    float mix = 0.45f, mixTarget = 0.45f, mixCoeff = 0.004f;
};

//==============================================================================
// DEGAUSS: ghost echo. Repeats ping-pong between a pitch-shifted head and a
// backwards head that shifts back down, so the echoes keep turning inside out.
//==============================================================================
class Degauss final : public StereoPedal
{
public:
    void prepared() override
    {
        shifter.prepare (sampleRate, shiftWindowMs, shiftSearchMs);
        lineA.setMaxSamples (shifter.span ((int) (sampleRate * 1.05)) + 64);
        lineB.setMaxSamples ((int) (sampleRate * 3.3));
        hpA.setHz (90.0f, sampleRate);
        timeCoeff = dsp::onePoleCoeff (4.0f, sampleRate);
        mixCoeff = dsp::onePoleCoeff (30.0f, sampleRate);
    }

    void reset() override
    {
        lineA.clear(); lineB.clear();
        toneA.reset(); hpA.reset();
        bOut = 0.0f;
        revPhase = 0.0f;
        // Knobs may still read zero on the very first prepare; keep the chunk sane.
        time = juce::jmax (256.0f, knob (0) * 0.001f * (float) sampleRate);
        chunk = time;
        shifter.setSemitones (semis, true);
        mix = knob (5);
    }

    void update() override
    {
        timeTarget = knob (0) * 0.001f * (float) sampleRate;
        feedback = knob (1);
        static constexpr float shifts[] { -12.0f, -5.0f, 0.08f, 7.0f, 12.0f };
        semis = shifts[juce::jlimit (0, 4, (int) knob (2))];
        shifter.setSemitones (semis, false);
        revRate = std::exp2 (-semis / 12.0f);
        toneA.setHz (knob (3), sampleRate);
        width = knob (4);
        mixTarget = knob (5);
    }

    void processStereo (float* left, float* right, int n) override
    {
        const float centre = (float) shifter.centre();

        for (int i = 0; i < n; ++i)
        {
            const float xl = left[i], xr = right[i];
            time += timeCoeff * (timeTarget - time);

            // Head A: pitch-shifted echo of the input plus whatever B sends back.
            lineA.push (0.5f * (xl + xr) + feedback * bOut);
            const float a = softLimit (hpA.hp (toneA.lp (shifter.process (lineA, juce::jmax (0.0f, time - centre)))));

            // Head B: plays A backwards in chunks of one echo time, shifting the pitch back down.
            lineB.push (a);
            revPhase += 1.0f / chunk;
            if (revPhase >= 1.0f)
            {
                revPhase -= 1.0f;
                chunk = juce::jmax (256.0f, time);
            }

            float phaseB = revPhase + 0.5f;
            if (phaseB >= 1.0f) phaseB -= 1.0f;

            const float span = (1.0f + revRate) * chunk;
            const float wA = hann (revPhase);
            const float wB = 1.0f - wA;
            bOut = wA * readSafe (lineB, 2.0f + revPhase * span) + wB * readSafe (lineB, 2.0f + phaseB * span);

            mix += mixCoeff * (mixTarget - mix);
            const float p = 0.5f * (1.0f + width), q = 0.5f * (1.0f - width);
            left[i] = xl + mix * (p * a + q * bOut);
            right[i] = xr + mix * (q * a + p * bOut);
        }
    }

private:
    GrainVoice shifter;
    dsp::DelayBuffer lineA, lineB;
    OnePole toneA, hpA;

    float time = 18000.0f, timeTarget = 18000.0f, timeCoeff = 0.001f, chunk = 18000.0f;
    float feedback = 0.45f, semis = 12.0f, revRate = 0.5f, width = 0.8f;
    float bOut = 0.0f, revPhase = 0.0f, mix = 0.45f, mixTarget = 0.45f, mixCoeff = 0.004f;
};

} // namespace

void registerGlitchPedals (Registry& r)
{
    r.add<Relay> ({ "glitch.relay", "RELAY", "buffer repeat", "Weird",
                    "Grabs the last slice of your playing and machine-guns it in time",
                    juce::Colour (0xff0f4c45), juce::Colour (0xfff2ff5c),
                    { knob ("tempo", "Tempo", 60.0f, 200.0f, 120.0f),
                      choice ("slice", "Slice", noteDivisions, 2),
                      choice ("mode", "Mode", { "Auto", "Picked", "Latch" }, 0),
                      knob ("chance", "Chance", 0.0f, 1.0f, 0.35f, "%"),
                      knob ("fade", "Fade", 0.0f, 1.0f, 0.15f, "%"),
                      knob ("warp", "Warp", -3.0f, 3.0f, 0.0f, "st"),
                      knob ("mix", "Mix", 0.0f, 1.0f, 1.0f, "%") } });

    r.add<Flywheel> ({ "glitch.flywheel", "FLYWHEEL", "tape stop", "Weird",
                       "Brakes your signal to a dead stop and winds it back up to speed",
                       juce::Colour (0xffd96c1e), juce::Colour (0xff1b1410),
                       { choice ("mode", "Mode", { "Cycle", "Picked", "Manual" }, 1),
                         choice ("motor", "Motor", { "Run", "Stop" }, 0),
                         knob ("brake", "Brake", 60.0f, 3000.0f, 600.0f, "ms", 0.5f),
                         knob ("start", "Start", 30.0f, 2000.0f, 180.0f, "ms", 0.5f),
                         knob ("period", "Period", 500.0f, 8000.0f, 2400.0f, "ms", 0.5f),
                         knob ("sense", "Sense", -60.0f, -15.0f, -40.0f, "dB") } });

    r.add<Stylus> ({ "glitch.stylus", "STYLUS", "turntable scratch", "Weird",
                     "Scratches what you just played back and forth, or warps it like a bent record",
                     juce::Colour (0xff141414), juce::Colour (0xfff7c948),
                     { knob ("rate", "Rate", 0.5f, 12.0f, 2.5f, "Hz", 0.5f),
                       knob ("depth", "Depth", 10.0f, 500.0f, 160.0f, "ms", 0.5f),
                       choice ("hand", "Hand", { "Baby", "Tear", "Warp", "Drunk" }, 0),
                       knob ("fader", "Fader", 0.0f, 1.0f, 0.0f, "%"),
                       knob ("mix", "Mix", 0.0f, 1.0f, 0.7f, "%") } });

    r.add<Rewind> ({ "glitch.rewind", "REWIND", "auto reverse", "Weird",
                     "Every note you pick plays backwards, swelling up into its own attack",
                     juce::Colour (0xff7a1f5c), juce::Colour (0xfffde2f3),
                     { knob ("length", "Length", 120.0f, 1500.0f, 550.0f, "ms", 0.6f),
                       knob ("sense", "Sense", -60.0f, -15.0f, -48.0f, "dB"),
                       knob ("swell", "Swell", 0.0f, 1.0f, 0.5f, "%"),
                       choice ("octave", "Octave", { "Down", "Unison", "Up" }, 1),
                       knob ("spread", "Spread", 0.0f, 1.0f, 0.5f, "%"),
                       knob ("tone", "Tone", 800.0f, 16000.0f, 7000.0f, "Hz", 0.4f),
                       knob ("dry", "Dry", 0.0f, 1.0f, 0.0f, "%"),
                       knob ("wet", "Wet", 0.0f, 1.0f, 1.0f, "%") } });

    r.add<Scrapyard> ({ "glitch.scrapyard", "SCRAPYARD", "granular cloud", "Weird",
                       "Sprays splinters of the last two seconds across the stereo field",
                       juce::Colour (0xff2b2d42), juce::Colour (0xffff9f1c),
                       { knob ("density", "Density", 1.0f, 60.0f, 14.0f, "Hz", 0.5f),
                         knob ("size", "Size", 15.0f, 400.0f, 110.0f, "ms", 0.5f),
                         knob ("scatter", "Scatter", 10.0f, 2000.0f, 450.0f, "ms", 0.5f),
                         choice ("pitch", "Pitch", { "Unison", "Octaves", "Fifths", "Chaos" }, 1),
                         knob ("reverse", "Reverse", 0.0f, 1.0f, 0.3f, "%"),
                         knob ("width", "Width", 0.0f, 1.0f, 0.8f, "%"),
                         knob ("tone", "Tone", 800.0f, 16000.0f, 7000.0f, "Hz", 0.4f),
                         knob ("mix", "Mix", 0.0f, 1.0f, 0.45f, "%") } });

    r.add<Cryotron> ({ "glitch.cryotron", "CRYOTRON", "note freezer", "Weird",
                       "Freezes each note you pick into a drone you can play over",
                       juce::Colour (0xffcfeff7), juce::Colour (0xff123a75),
                       { choice ("mode", "Mode", { "Picked", "Hold" }, 0),
                         knob ("sustain", "Sustain", 500.0f, 30000.0f, 6000.0f, "ms", 0.4f),
                         knob ("swell", "Swell", 20.0f, 3000.0f, 400.0f, "ms", 0.5f),
                         knob ("drone", "Drone", 0.0f, 1.0f, 0.45f, "%"),
                         knob ("shimmer", "Shimmer", 0.0f, 1.0f, 0.2f, "%"),
                         knob ("width", "Width", 0.0f, 1.0f, 0.7f, "%"),
                         knob ("tone", "Tone", 500.0f, 12000.0f, 5000.0f, "Hz", 0.4f),
                         knob ("dry", "Dry", 0.0f, 1.0f, 1.0f, "%") } });

    r.add<Gremlin> ({ "glitch.gremlin", "GREMLIN", "glitch sequencer", "Weird",
                      "A gremlin repeats, reverses, re-pitches or drops each step, in a loop you can reseed",
                      juce::Colour (0xff1a3b17), juce::Colour (0xff7dff5a),
                      { knob ("tempo", "Tempo", 60.0f, 200.0f, 120.0f),
                        choice ("step", "Step", { "1/8", "1/16", "1/32" }, 1),
                        knob ("chaos", "Chaos", 0.0f, 1.0f, 0.4f, "%"),
                        knob ("seed", "Seed", 0.0f, 99.0f, 13.0f),
                        choice ("loop", "Loop", { "1 Bar", "2 Bars", "Free" }, 0),
                        knob ("spread", "Spread", 0.0f, 1.0f, 0.6f, "%"),
                        knob ("mix", "Mix", 0.0f, 1.0f, 1.0f, "%") } });

    r.add<Escalator> ({ "glitch.escalator", "ESCALATOR", "endless rise", "Weird",
                        "Shifted voices climb forever, fading in at the bottom as they wrap",
                        juce::Colour (0xfff2f2e9), juce::Colour (0xffe0218a),
                        { knob ("climb", "Climb", -6.0f, 6.0f, 1.5f, "st"),
                          choice ("register", "Register", { "Low", "Mid" }, 1),
                          knob ("mix", "Mix", 0.0f, 1.0f, 0.45f, "%"),
                          knob ("wash", "Wash", 0.0f, 1.0f, 0.4f, "%"),
                          knob ("tone", "Tone", 800.0f, 14000.0f, 6000.0f, "Hz", 0.4f),
                          knob ("width", "Width", 0.0f, 1.0f, 0.7f, "%") } });

    r.add<Degauss> ({ "glitch.degauss", "DEGAUSS", "ghost echo", "Weird",
                      "Echoes alternate between pitch-shifted and backwards as they bounce",
                      juce::Colour (0xff3a0ca3), juce::Colour (0xff4cc9f0),
                      { knob ("time", "Time", 80.0f, 1000.0f, 380.0f, "ms", 0.6f),
                        knob ("feedback", "Feedback", 0.0f, 0.9f, 0.45f, "%"),
                        choice ("shift", "Shift", { "-Oct", "-5th", "Ghost", "+5th", "+Oct" }, 4),
                        knob ("tone", "Tone", 800.0f, 12000.0f, 4500.0f, "Hz", 0.4f),
                        knob ("width", "Width", 0.0f, 1.0f, 0.8f, "%"),
                        knob ("mix", "Mix", 0.0f, 1.0f, 0.45f, "%") } });
}

} // namespace woodshed
