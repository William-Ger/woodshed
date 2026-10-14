#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// Shared helpers
//==============================================================================

constexpr float twoPi = juce::MathConstants<float>::twoPi;

/** Transparent below 1.0, then a smooth knee that never exceeds 2.0. */
inline float safety (float x)
{
    const float a = std::abs (x);
    return a <= 1.0f ? x : std::copysign (1.0f + std::tanh (a - 1.0f), x);
}

/** Snaps vanishing values to zero so decaying tails reach true silence. */
inline float flush (float x)                    { return std::abs (x) < 1.0e-12f ? 0.0f : x; }


/** Tiny xorshift PRNG: realtime safe, deterministic. */
struct Rng
{
    uint32_t s = 0x2545f491u;
    void seed (uint32_t v)                      { s = v != 0 ? v : 1u; }
    uint32_t nextInt()                          { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float bipolar()                             { return (float) (int32_t) nextInt() * (1.0f / 2147483648.0f); }
    float unipolar()                            { return (float) (nextInt() >> 8) * (1.0f / 16777216.0f); }
};

/** One-pole parameter smoother. */
struct Smooth
{
    float value = 0.0f, target = 0.0f, coeff = 0.01f;
    void setTime (float ms, double sr)          { coeff = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, ms * 0.001f * (float) sr)); }
    void set (float t, bool snap)               { target = t; if (snap) value = t; }
    float next()                                { value += coeff * (target - value); return value; }
};

/** Peak envelope follower with separate attack and release and a hard floor. */
struct Follower
{
    float env = 0.0f, att = 0.0f, rel = 0.0f;

    void setup (float attackMs, float releaseMs, double sr)
    {
        att = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, attackMs * 0.001f * (float) sr));
        rel = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, releaseMs * 0.001f * (float) sr));
    }

    void reset()                                { env = 0.0f; }

    float process (float x)
    {
        const float a = std::abs (x);
        env += (a > env ? att : rel) * (a - env);
        if (env < 1.0e-7f) env = 0.0f;
        return env;
    }
};

/** Pick detector: a fast envelope jumping well above a slow one, with a refractory period. */
struct OnsetDetector
{
    Follower fast, slow;
    int holdoff = 0, refractory = 2400;
    float ratio = 1.8f, floorLevel = 0.002f;

    void setup (double sr)
    {
        fast.setup (0.5f, 25.0f, sr);
        slow.setup (40.0f, 120.0f, sr);
        refractory = (int) (0.06 * sr);
    }

    void reset()                                { fast.reset(); slow.reset(); holdoff = 0; }

    /** Returns the pick strength (fast envelope) on an onset, otherwise 0. */
    float process (float x)
    {
        const float f = fast.process (x);
        const float s = slow.process (x);
        if (holdoff > 0) { --holdoff; return 0.0f; }
        if (f > floorLevel && f > ratio * s + 0.0005f)
        {
            holdoff = refractory;
            return f;
        }
        return 0.0f;
    }
};

inline float polyBlep (float t, float dt)
{
    if (t < dt)         { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt)  { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

/** Band-limited oscillator (saw, square, narrow pulse). */
struct Osc
{
    float phase = 0.0f;

    float saw (float dt)
    {
        const float v = 2.0f * phase - 1.0f - polyBlep (phase, dt);
        advance (dt);
        return v;
    }

    float pulse (float dt, float width)
    {
        float t2 = phase + 1.0f - width;
        t2 -= std::floor (t2);
        const float v = (phase < width ? 1.0f : -1.0f) + polyBlep (phase, dt) - polyBlep (t2, dt);
        advance (dt);
        return v;
    }

    float sine (float dt)
    {
        const float v = std::sin (twoPi * phase);
        advance (dt);
        return v;
    }

    void advance (float dt)                     { phase += dt; phase -= std::floor (phase); }
};

//==============================================================================
/**
    Monophonic pitch tracker for guitar. The input is band-limited and decimated
    to about 12 kHz, then a YIN difference function (cumulative-mean normalised)
    runs every ~2.7 ms over a 26 ms window. The median of the last three
    estimates rejects the odd octave slip at note changes. Holds the last pitch
    when the signal is quiet or unpitched.
*/
class PitchTracker
{
public:
    void prepare (double sr)
    {
        decim = juce::jmax (1, (int) std::round (sr / 12000.0));
        dsr = (float) (sr / decim);
        window = (int) (0.024f * dsr);
        maxLag = (int) (dsr / 62.0f);
        minLag = juce::jmax (2, (int) (dsr / 1300.0f));
        hop = juce::jmax (8, (int) (0.0027f * dsr));

        int size = 1;
        while (size < window + maxLag + 8) size <<= 1;
        ring.assign ((size_t) size, 0.0f);
        mask = size - 1;
        frame.assign ((size_t) (window + maxLag + 2), 0.0f);
        diff.assign ((size_t) (maxLag + 2), 0.0f);

        aa1.setup (1600.0f, 0.54f, sr);
        aa2.setup (1600.0f, 1.31f, sr);
        hpCoeff = dsp::onePoleCoeff (50.0f, sr);
        reset();
    }

    void reset()
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        aa1.reset(); aa2.reset();
        hpState = 0.0f;
        writePos = decimCount = hopCount = 0;
        hz = 0.0f;
        voiced = false;
        history = { 0.0f, 0.0f, 0.0f };
        histCount = 0;
    }

    void push (float x)
    {
        hpState += hpCoeff * (x - hpState);
        const float y = aa2.process (aa1.process (x - hpState).low).low;

        if (++decimCount < decim) return;
        decimCount = 0;
        ring[(size_t) writePos] = y;
        writePos = (writePos + 1) & mask;

        if (++hopCount >= hop)
        {
            hopCount = 0;
            analyse();
        }
    }

    float hz = 0.0f;        // last confident pitch (0 until the first note)
    bool voiced = false;    // the latest analysis found a clear pitch

private:
    void analyse()
    {
        const int len = window + maxLag;
        for (int i = 0; i < len; ++i)
            frame[(size_t) i] = ring[(size_t) ((writePos - len + i) & mask)];

        float energy = 0.0f;
        for (int j = 0; j < window; ++j) energy += frame[(size_t) j] * frame[(size_t) j];
        if (energy < gateLevel * gateLevel * (float) window)
        {
            voiced = false;
            histCount = 0;
            return;
        }

        const float* f = frame.data();
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            float d = 0.0f;
            for (int j = 0; j < window; ++j)
            {
                const float e = f[j] - f[j + tau];
                d += e * e;
            }
            diff[(size_t) tau] = d;
        }

        // Cumulative mean normalised difference.
        float running = 0.0f;
        diff[0] = 1.0f;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            running += diff[(size_t) tau];
            diff[(size_t) tau] = running > 0.0f ? diff[(size_t) tau] * (float) tau / running : 1.0f;
        }

        int best = -1;
        for (int tau = minLag; tau < maxLag; ++tau)
        {
            if (diff[(size_t) tau] < 0.15f)
            {
                while (tau + 1 < maxLag && diff[(size_t) tau + 1] < diff[(size_t) tau]) ++tau;
                best = tau;
                break;
            }
        }

        if (best < 0)
        {
            // No clear dip under the threshold: accept the global minimum only if it is still decent.
            float lowest = 1.0f;
            for (int tau = minLag; tau < maxLag; ++tau)
                if (diff[(size_t) tau] < lowest) { lowest = diff[(size_t) tau]; best = tau; }
            if (lowest > 0.3f) best = -1;
        }

        if (best < 0)
        {
            voiced = false;
            histCount = 0;
            return;
        }

        // Parabolic refinement.
        float period = (float) best;
        if (best > 1 && best < maxLag)
        {
            const float a = diff[(size_t) best - 1], b = diff[(size_t) best], c = diff[(size_t) best + 1];
            const float den = a - 2.0f * b + c;
            if (den > 1.0e-9f) period += juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / den);
        }

        const float est = dsr / period;
        history[2] = history[1];
        history[1] = history[0];
        history[0] = est;
        histCount = juce::jmin (3, histCount + 1);

        if (histCount < 2)
        {
            voiced = false;     // wait for a second opinion before speaking
            return;
        }

        float m = est;
        if (histCount == 2)
        {
            // Two estimates: only trust them if they agree within a semitone.
            if (std::abs (std::log2 (history[0] / history[1])) > 0.085f) { voiced = false; return; }
            m = std::sqrt (history[0] * history[1]);
        }
        else
        {
            const float a = history[0], b = history[1], c = history[2];
            m = juce::jmax (juce::jmin (a, b), juce::jmin (juce::jmax (a, b), c));
        }

        hz = m;
        voiced = true;
    }

    std::vector<float> ring, frame, diff;
    dsp::Svf aa1, aa2;
    std::array<float, 3> history {};
    float dsr = 12000.0f, hpState = 0.0f, hpCoeff = 0.0f;
    static constexpr float gateLevel = 0.0012f;
    int decim = 4, window = 312, maxLag = 193, minLag = 9, hop = 32;
    int mask = 0, writePos = 0, decimCount = 0, hopCount = 0, histCount = 0;
};

/** Smooths the tracker's on/off decision into a gain so unpitched gaps fade instead of clicking. */
struct VoicingGate
{
    float value = 0.0f, up = 0.0f, down = 0.0f;
    void setup (double sr)                      { up = dsp::onePoleCoeff (60.0f, sr); down = dsp::onePoleCoeff (4.0f, sr); }
    void reset()                                { value = 0.0f; }
    float process (bool on)                     { value += (on ? up : down) * ((on ? 1.0f : 0.0f) - value); if (value < 1.0e-6f) value = 0.0f; return value; }
};

//==============================================================================
/** 90-degree phase-difference network: two allpass chains whose outputs stay in quadrature from ~20 Hz to ~20 kHz. */
struct Hilbert
{
    struct Section
    {
        float c = 0.0f, x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
        float process (float x)
        {
            const float y = c * (x + y2) - x2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            return y;
        }
    };

    std::array<Section, 4> a, b;
    float delayed = 0.0f;

    Hilbert()
    {
        const float ca[] { 0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f };
        const float cb[] { 0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f };
        for (int i = 0; i < 4; ++i) { a[(size_t) i].c = ca[i] * ca[i]; b[(size_t) i].c = cb[i] * cb[i]; }
    }

    void reset()
    {
        for (auto* chain : { &a, &b })
            for (auto& s : *chain) s.x1 = s.x2 = s.y1 = s.y2 = 0.0f;
        delayed = 0.0f;
    }

    /** In-phase (i) and quadrature (q, lagging by 90 degrees). */
    void process (float x, float& i, float& q)
    {
        float ya = x, yb = x;
        for (auto& s : a) ya = s.process (ya);
        for (auto& s : b) yb = s.process (yb);
        q = delayed;
        delayed = ya;
        i = yb;
    }
};

//==============================================================================
// BASILISK: guitar synth. Tracks the note you play and voices it on a pair of
// detuned band-limited oscillators plus a square sub, through a resonant
// low-pass that snaps open on every pick.
//==============================================================================
class GuitarSynth final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        tracker.prepare (sampleRate);
        gate.setup (sampleRate);
        ampEnv.setup (3.0f, 90.0f, sampleRate);
        onset.setup (sampleRate);
        for (auto* s : { &sub, &cutoff, &reso, &envAmt, &mix })
            s->setTime (30.0f, sampleRate);
    }

    void reset() override
    {
        tracker.reset();
        gate.reset();
        ampEnv.reset();
        onset.reset();
        for (auto& f : filters) f.reset();
        for (auto& o : oscs) o.phase = 0.0f;
        subOsc.phase = 0.0f;
        logHz = -1.0f;
        filterEnv = 0.0f;
        fresh = true;
    }

    void update() override
    {
        wave = juce::roundToInt (knob (0));
        octave = (float) (juce::roundToInt (knob (1)) - 2);
        sub.set (knob (2), fresh);
        glideCoeff = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, knob (3) * 0.00025f * (float) sampleRate));
        cutoff.set (std::log2 (knob (4)), fresh);
        reso.set (knob (5), fresh);
        envAmt.set (knob (6), fresh);
        mix.set (knob (7), fresh);
        fresh = false;
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        const float envDecay = std::exp (-1.0f / (0.28f * (float) sampleRate));

        for (int i = 0; i < n; ++i)
        {
            const float in = 0.5f * (left[i] + right[i]);
            tracker.push (in);
            const float amp = ampEnv.process (in) * gate.process (tracker.voiced);

            if (const float pick = onset.process (in); pick > 0.0f)
                filterEnv = juce::jmin (1.0f, 0.4f + pick * 25.0f);
            else
                filterEnv *= envDecay;

            if (tracker.hz > 0.0f)
            {
                const float target = std::log2 (tracker.hz) + octave;
                if (logHz < 0.0f) logHz = target;
                logHz += glideCoeff * (target - logHz);
            }

            float wetL = 0.0f, wetR = 0.0f;
            const float subLevel = sub.next(), res = reso.next(), env = envAmt.next(), m = mix.next(), cut = cutoff.next();

            if (logHz > 0.0f)
            {
                const float hz = std::exp2 (logHz);
                const float inv = 1.0f / (float) sampleRate;
                const float dtA = hz * 1.0035f * inv, dtB = hz * 0.9965f * inv;
                float a, b;
                if (wave == 0)      { a = oscs[0].saw (dtA);           b = oscs[1].saw (dtB); }
                else if (wave == 1) { a = oscs[0].pulse (dtA, 0.5f);   b = oscs[1].pulse (dtB, 0.5f); }
                else                { a = oscs[0].pulse (dtA, 0.22f);  b = oscs[1].pulse (dtB, 0.22f); }
                const float s = subOsc.pulse (hz * 0.5f * inv, 0.5f) * subLevel;

                if ((counter++ & 3) == 0)
                {
                    // Cutoff tracks the note a little and opens with the pick envelope.
                    const float c = std::exp2 (cut + 0.35f * (logHz - 8.0f) + env * 4.5f * filterEnv);
                    for (auto& f : filters) f.setup (juce::jmin (c, 16000.0f), res, sampleRate);
                }

                const float drive = amp * synthGain;
                wetL = filters[0].process ((a + s) * drive).low;
                wetR = filters[1].process ((b + s) * drive).low;
                const float trim = 1.0f / std::sqrt (juce::jmax (1.0f, res));
                wetL *= trim;
                wetR *= trim;
            }

            wetL = flush (wetL);
            wetR = flush (wetR);
            left[i]  = safety ((1.0f - m) * left[i]  + m * wetL);
            right[i] = safety ((1.0f - m) * right[i] + m * wetR);
        }
    }

private:
    static constexpr float synthGain = 1.8f;

    PitchTracker tracker;
    VoicingGate gate;
    Follower ampEnv;
    OnsetDetector onset;
    std::array<dsp::Svf, 2> filters;
    std::array<Osc, 2> oscs;
    Osc subOsc;
    Smooth sub, cutoff, reso, envAmt, mix;
    float logHz = -1.0f, glideCoeff = 0.1f, filterEnv = 0.0f, octave = 0.0f;
    int wave = 0;
    uint32_t counter = 0;
    bool fresh = true;
};

//==============================================================================
// ECLIPSE: single-sideband frequency shifter. Every partial moves by the same
// number of Hz, so harmony turns metallic; tiny shifts swirl like a phaser
// that never turns around. Feedback through a short delay makes it spiral.
//==============================================================================
class FrequencyShifter final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        for (auto& c : chans) c.line.setMaxSamples ((int) (0.45 * sampleRate) + 8);
        for (auto* s : { &shift, &feedback, &delay, &mix, &drift })
            s->setTime (40.0f, sampleRate);
    }

    void reset() override
    {
        for (auto& c : chans)
        {
            c.line.clear();
            c.hilbert.reset();
            c.fb = 0.0f;
            c.dc = 0.0f;
        }
        phase = 0.0;
        lfoPhase = 0.0f;
        fresh = true;
    }

    void update() override
    {
        shift.set (knob (0), fresh);
        mode = juce::roundToInt (knob (1));
        feedback.set (knob (2), fresh);
        delay.set (knob (3) * 0.001f * (float) sampleRate, fresh);
        drift.set (knob (4), fresh);
        mix.set (knob (5), fresh);
        fresh = false;
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        const float lfoInc = 0.13f / (float) sampleRate;
        const double inv = 1.0 / sampleRate;

        for (int i = 0; i < n; ++i)
        {
            const float hz = shift.next(), fb = feedback.next(), d = juce::jmax (2.0f, delay.next()), m = mix.next(), dr = drift.next();

            lfoPhase += lfoInc;
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
            const float wobble = 1.0f + dr * 0.6f * std::sin (twoPi * lfoPhase);

            phase += (double) (hz * wobble) * inv;
            phase -= std::floor (phase);
            const float c = (float) std::cos (juce::MathConstants<double>::twoPi * phase);
            const float s = (float) std::sin (juce::MathConstants<double>::twoPi * phase);

            float* io[2] { left, right };
            for (int ch = 0; ch < 2; ++ch)
            {
                auto& st = chans[(size_t) ch];
                const float dry = io[ch][i];
                const float x = dry + st.fb;
                float re, im;
                st.hilbert.process (x, re, im);

                // Up: I cos - Q sin. Down: I cos + Q sin. Split: left up, right down.
                const bool down = mode == 1 || (mode == 2 && ch == 1);
                float y = down ? re * c + im * s : re * c - im * s;
                st.dc += 0.0005f * (y - st.dc);
                y -= st.dc;

                st.line.push (y);
                st.fb = flush (fb * std::tanh (st.line.read (d)));
                const float wet = flush (y);
                io[ch][i] = safety (std::sqrt (1.0f - m) * dry + std::sqrt (m) * wet);
            }
        }
    }

private:
    struct Chan
    {
        Hilbert hilbert;
        dsp::DelayBuffer line;
        float fb = 0.0f, dc = 0.0f;
    };

    std::array<Chan, 2> chans;
    Smooth shift, feedback, delay, mix, drift;
    double phase = 0.0;
    float lfoPhase = 0.0f;
    int mode = 2;
    bool fresh = true;
};

//==============================================================================
// AURORA: sympathetic strings. Eight tuned resonators set to a chord in a key
// ring along whenever you play a note that shares their harmonics, like a sitar's
// drone strings or a piano with the sustain pedal down.
//==============================================================================
struct ChordShape { const char* name; std::array<int, 4> steps; int count; };

constexpr std::array<ChordShape, 7> chordShapes {{
    { "Major",  { 0, 4, 7, 0 },   3 },
    { "Minor",  { 0, 3, 7, 0 },   3 },
    { "Fifths", { 0, 7, 0, 0 },   2 },
    { "Sus",    { 0, 5, 7, 0 },   3 },
    { "Maj7",   { 0, 4, 7, 11 },  4 },
    { "Min7",   { 0, 3, 7, 10 },  4 },
    { "Drone",  { 0, 0, 0, 0 },   1 },
}};

juce::StringArray chordNames()
{
    juce::StringArray a;
    for (auto& c : chordShapes) a.add (c.name);
    return a;
}

juce::StringArray keyNames()                    { return { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }; }

class SympatheticStrings final : public Pedal
{
public:
    static constexpr int numStrings = 8;

    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        for (auto& s : strings)
        {
            s.line.setMaxSamples ((int) (sampleRate / 30.0) + 8);
            s.period.setTime (25.0f, sampleRate);
        }
        for (auto* s : { &mix, &width, &excite })
            s->setTime (30.0f, sampleRate);
        hpCoeff = dsp::onePoleCoeff (90.0f, sampleRate);
        lpCoeff = dsp::onePoleCoeff (3500.0f, sampleRate);
    }

    void reset() override
    {
        for (auto& s : strings)
        {
            s.line.clear();
            s.damp = 0.0f;
        }
        hp = lp = 0.0f;
        fresh = true;
    }

    void update() override
    {
        const int key = juce::roundToInt (knob (0));
        const auto& shape = chordShapes[(size_t) juce::jlimit (0, (int) chordShapes.size() - 1, juce::roundToInt (knob (1)))];
        const float decaySec = knob (2) * 0.001f;
        const float bright = knob (3);
        const int range = juce::roundToInt (knob (6));

        // Root in octave 2 (E2 = 82 Hz) for Low, an octave higher for Mid, two for High.
        const float rootMidi = 36.0f + (float) key + 12.0f * (float) range;
        dampCoeff = 0.08f + 0.9f * bright * bright;

        float totalLoss = 0.0f;
        for (int i = 0; i < numStrings; ++i)
        {
            auto& s = strings[(size_t) i];
            const int octaveUp = i / shape.count;
            float midi = rootMidi + (float) shape.steps[(size_t) (i % shape.count)] + 12.0f * (float) octaveUp;
            if (shape.count == 1) midi = rootMidi + (float) (i % 2 == 0 ? 0 : 7) + 12.0f * (float) (i / 2);
            while (midi > 88.0f) midi -= 12.0f;
            const float hz = 440.0f * std::exp2 ((midi - 69.0f) / 12.0f);
            // One-pole damping delays the loop by (1 - c) / c samples at low frequencies.
            const float p = (float) sampleRate / hz - (1.0f - dampCoeff) / dampCoeff;
            s.period.set (juce::jmax (4.0f, p), fresh);
            s.gain = std::pow (10.0f, -3.0f * (float) sampleRate / hz / (decaySec * (float) sampleRate));
            s.gain = juce::jmin (s.gain, 0.9995f);
            totalLoss += 1.0f - s.gain * s.gain;
        }

        // Feed the strings less as they ring longer so the bank's loudness stays put.
        excite.set (std::sqrt (totalLoss / (float) numStrings) * 1.6f, fresh);
        width.set (knob (5), fresh);
        mix.set (knob (4), fresh);
        fresh = false;
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        juce::ScopedNoDenormals noDenormals;

        for (int i = 0; i < n; ++i)
        {
            const float in = 0.5f * (left[i] + right[i]);
            hp += hpCoeff * (in - hp);
            lp += lpCoeff * ((in - hp) - lp);
            const float x = lp * excite.next();
            const float w = width.next(), m = mix.next();

            float sumL = 0.0f, sumR = 0.0f;
            for (int k = 0; k < numStrings; ++k)
            {
                auto& s = strings[(size_t) k];
                const float back = s.line.read (s.period.next());
                s.damp = flush (s.damp + dampCoeff * (back - s.damp));
                const float y = x + s.gain * s.damp;
                s.line.push (y);
                const float pan = (k & 1) ? 0.5f + 0.5f * w : 0.5f - 0.5f * w;
                sumL += y * (1.0f - pan);
                sumR += y * pan;
            }

            left[i]  = safety (left[i]  + m * stringGain * sumL);
            right[i] = safety (right[i] + m * stringGain * sumR);
        }
    }

private:
    static constexpr float stringGain = 0.36f;

    struct String
    {
        dsp::DelayBuffer line;
        Smooth period;
        float gain = 0.9f, damp = 0.0f;
    };

    std::array<String, numStrings> strings;
    Smooth mix, width, excite;
    float hp = 0.0f, lp = 0.0f, hpCoeff = 0.0f, lpCoeff = 0.0f, dampCoeff = 0.5f;
    bool fresh = true;
};

//==============================================================================
// LOON: amp feedback on demand. Hold a note and a howl blooms in on top of it,
// locked to the note or one of its harmonics, like standing in front of a
// cranked stack. A new pick chokes it; letting the note die kills it.
//==============================================================================
class FeedbackMachine final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        tracker.prepare (sampleRate);
        env.setup (2.0f, 70.0f, sampleRate);
        peakHold.setup (1.0f, 1800.0f, sampleRate);
        onset.setup (sampleRate);
        release = dsp::onePoleCoeff (7.0f, sampleRate);
        glide = dsp::onePoleCoeff (9.0f, sampleRate);
        for (auto* s : { &level, &wobble, &drive, &width })
            s->setTime (40.0f, sampleRate);
    }

    void reset() override
    {
        tracker.reset();
        env.reset();
        peakHold.reset();
        onset.reset();
        bloom = 0.0f;
        logHz = -1.0f;
        phases = { 0.0f, 0.0f };
        vibPhase = 0.0f;
        rng.seed (0x51ed27u);
        drift = driftTarget = 0.0f;
        fresh = true;
    }

    void update() override
    {
        constexpr float ratios[] { 1.0f, 2.0f, 3.0f, 4.0f };
        ratio = ratios[juce::jlimit (0, 3, juce::roundToInt (knob (0)))];
        bloomCoeff = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, knob (1) * 0.001f * 0.35f * (float) sampleRate));
        level.set (knob (2), fresh);
        threshold = dsp::dbToGain (knob (3));
        wobble.set (knob (4), fresh);
        drive.set (knob (5), fresh);
        width.set (knob (6), fresh);
        fresh = false;
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        const float inv = 1.0f / (float) sampleRate;

        for (int i = 0; i < n; ++i)
        {
            const float dryL = left[i], dryR = right[i];
            const float in = 0.5f * (dryL + dryR);
            tracker.push (in);
            const float e = env.process (in);
            const float held = peakHold.process (in);

            if (onset.process (in) > 0.0f)
                bloom *= 0.3f;

            const bool open = e > threshold && tracker.hz > 0.0f;
            bloom += open ? bloomCoeff * (1.0f - bloom) : -release * bloom;
            if (bloom < 1.0e-6f) bloom = 0.0f;

            if (tracker.hz > 0.0f)
            {
                float target = std::log2 (tracker.hz * ratio);
                while (target > 11.6f) target -= 1.0f;     // keep the howl under ~3 kHz
                if (logHz < 0.0f) logHz = target;
                logHz += glide * (target - logHz);
            }

            // Slow wandering plus a little throat vibrato, like a string swaying in the air.
            if ((counter++ & 255) == 0) driftTarget = rng.bipolar();
            drift += 0.0004f * (driftTarget - drift);
            vibPhase += 5.3f * inv;
            if (vibPhase >= 1.0f) vibPhase -= 1.0f;

            const float w = wobble.next(), dr = drive.next(), lv = level.next(), wd = width.next();
            const float bend = w * (0.18f * std::sin (twoPi * vibPhase) + 0.25f * drift) / 12.0f;

            // Cranked-amp dirt on the dry note.
            const float g = 1.0f + dr * 24.0f;
            const float makeup = (1.0f + dr * 0.8f) / g;
            float outL = std::tanh (dryL * g) * makeup;
            float outR = std::tanh (dryR * g) * makeup;

            if (bloom > 0.0f && logHz > 0.0f)
            {
                const float amp = bloom * lv * std::sqrt (held * e) * howlGain;
                const float hz = std::exp2 (logHz + bend);
                const float spread = wd * 0.0025f;
                float* ph[2] { &phases[0], &phases[1] };
                float howl[2];
                for (int c = 0; c < 2; ++c)
                {
                    auto& p = *ph[c];
                    p += hz * (c == 0 ? 1.0f + spread : 1.0f - spread) * inv;
                    p -= std::floor (p);
                    const float tone = std::sin (twoPi * p) + 0.3f * std::sin (2.0f * twoPi * p);
                    howl[c] = std::tanh (1.6f * tone) * amp;
                }
                outL += howl[0];
                outR += howl[1];
            }

            left[i] = safety (outL);
            right[i] = safety (outR);
        }
    }

private:
    static constexpr float howlGain = 1.4f;

    PitchTracker tracker;
    Follower env, peakHold;
    OnsetDetector onset;
    Rng rng;
    Smooth level, wobble, drive, width;
    std::array<float, 2> phases {};
    float bloom = 0.0f, bloomCoeff = 0.001f, release = 0.001f, glide = 0.01f, logHz = -1.0f;
    float ratio = 2.0f, threshold = 0.002f, vibPhase = 0.0f, drift = 0.0f, driftTarget = 0.0f;
    uint32_t counter = 0;
    bool fresh = true;
};

//==============================================================================
// SATURN: bell maker. Your signal is ring-modulated by partials at inharmonic
// ratios of the note you're playing, so every pick strikes a bell, gong or
// glass tone that rings out and settles back into your note.
//==============================================================================
struct MetalSet { const char* name; std::array<float, 3> ratio, weight; };

constexpr std::array<MetalSet, 5> metals {{
    { "Bell",  { 2.76f, 5.40f, 1.19f }, { 1.0f, 0.55f, 0.45f } },
    { "Gong",  { 1.41f, 2.29f, 3.17f }, { 1.0f, 0.75f, 0.55f } },
    { "Glass", { 3.00f, 4.18f, 6.93f }, { 1.0f, 0.70f, 0.40f } },
    { "Chime", { 2.00f, 3.00f, 5.04f }, { 1.0f, 0.50f, 0.35f } },
    { "Clank", { 1.73f, 2.65f, 4.46f }, { 1.0f, 0.80f, 0.60f } },
}};

juce::StringArray metalNames()
{
    juce::StringArray a;
    for (auto& m : metals) a.add (m.name);
    return a;
}

class BellMaker final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        tracker.prepare (sampleRate);
        onset.setup (sampleRate);
        glide = dsp::onePoleCoeff (25.0f, sampleRate);
        for (auto* s : { &tune, &sustain, &shimmer, &mix, &tone })
            s->setTime (40.0f, sampleRate);
    }

    void reset() override
    {
        tracker.reset();
        onset.reset();
        for (auto& c : chans) { c.phases = { 0.0f, 0.0f, 0.0f }; c.lp.reset(); }
        strike = 0.0f;
        logHz = std::log2 (220.0f);
        fresh = true;
    }

    void update() override
    {
        metal = juce::jlimit (0, (int) metals.size() - 1, juce::roundToInt (knob (0)));
        tune.set (knob (1), fresh);
        strikeDecay = std::exp (-6.9f / juce::jmax (1.0f, knob (2) * 0.001f * (float) sampleRate));
        sustain.set (knob (3), fresh);
        shimmer.set (knob (4), fresh);
        tone.set (knob (5), fresh);
        mix.set (knob (6), fresh);
        for (auto& c : chans) c.lp.setup (tone.value, 0.6f, sampleRate);
        fresh = false;
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        const auto& set = metals[(size_t) metal];
        const float wsum = set.weight[0] + set.weight[1] + set.weight[2];
        const float inv = 1.0f / (float) sampleRate;

        for (int i = 0; i < n; ++i)
        {
            const float in = 0.5f * (left[i] + right[i]);
            tracker.push (in);

            if (onset.process (in) > 0.0f) strike = 1.0f;
            else                           strike *= strikeDecay;
            if (strike < 1.0e-6f) strike = 0.0f;

            if (tracker.hz > 0.0f)
                logHz += glide * (std::log2 (tracker.hz) - logHz);

            const float tn = tune.next(), sus = sustain.next(), sh = shimmer.next(), m = mix.next();
            tone.next();
            const float ring = sus + (1.0f - sus) * strike;
            const float base = std::exp2 (logHz) * tn * inv;

            float* io[2] { left, right };
            for (int ch = 0; ch < 2; ++ch)
            {
                auto& c = chans[(size_t) ch];
                const float detune = 1.0f + (ch == 0 ? 0.006f : -0.006f) * sh;
                float mod = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    auto& p = c.phases[(size_t) k];
                    p += juce::jmin (0.45f, base * set.ratio[(size_t) k] * detune);
                    p -= std::floor (p);
                    mod += set.weight[(size_t) k] * std::sin (twoPi * p);
                }
                mod *= 1.5f / wsum;

                const float dry = io[ch][i];
                const float wet = c.lp.process (dry * mod * ring).low;
                io[ch][i] = safety (dry * (1.0f - 0.55f * m * ring) + m * bellGain * wet);
            }
        }
    }

private:
    static constexpr float bellGain = 2.2f;

    struct Chan
    {
        std::array<float, 3> phases {};
        dsp::Svf lp;
    };

    PitchTracker tracker;
    OnsetDetector onset;
    std::array<Chan, 2> chans;
    Smooth tune, sustain, shimmer, mix, tone;
    float strike = 0.0f, strikeDecay = 0.999f, glide = 0.01f, logHz = 8.0f;
    int metal = 0;
    bool fresh = true;
};

//==============================================================================
// FIREFLY: every pick fires a laser zap: an oscillator that dives (or climbs)
// between two pitches, in bursts that bounce across the stereo field.
//==============================================================================
class LaserZap final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        onset.setup (sampleRate);
        for (auto* s : { &level, &dry })
            s->setTime (30.0f, sampleRate);
    }

    void reset() override
    {
        onset.reset();
        for (auto& v : voices) v = {};
        nextPan = 0;
        fresh = true;
    }

    void update() override
    {
        startLog = std::log2 (knob (0));
        endLog = std::log2 (knob (1));
        timeSamples = juce::jmax (8.0f, knob (2) * 0.001f * (float) sampleRate);
        wave = juce::roundToInt (knob (3));
        constexpr int bursts[] { 1, 2, 3, 4, 6 };
        burst = bursts[juce::jlimit (0, 4, juce::roundToInt (knob (4)))];
        onset.floorLevel = dsp::dbToGain (-30.0f - 30.0f * knob (5));
        onset.ratio = 2.6f - 1.1f * knob (5);
        level.set (knob (6), fresh);
        dry.set (knob (7), fresh);
        fresh = false;
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        const float inv = 1.0f / (float) sampleRate;

        for (int i = 0; i < n; ++i)
        {
            const float in = 0.5f * (left[i] + right[i]);

            if (const float pick = onset.process (in); pick > 0.0f)
            {
                const float strength = juce::jlimit (0.25f, 1.0f, pick * 10.0f);
                for (int b = 0; b < burst; ++b)
                    fire (strength * (b == 0 ? 1.0f : 0.8f), b * (int) (timeSamples * 0.55f));
            }

            const float lv = level.next() * zapGain, d = dry.next();
            float sumL = 0.0f, sumR = 0.0f;

            for (auto& v : voices)
            {
                if (! v.active) continue;
                if (v.wait > 0) { --v.wait; continue; }

                const float progress = juce::jmin (1.0f, (float) v.age / timeSamples);
                const float hz = std::exp2 (startLog + (endLog - startLog) * (1.0f - (1.0f - progress) * (1.0f - progress)));
                const float dt = juce::jmin (0.45f, hz * inv);
                float s;
                if (wave == 0)      s = v.osc.sine (dt);
                else if (wave == 1) s = 0.6f * v.osc.pulse (dt, 0.5f);
                else
                {
                    v.mod.advance (dt * 2.01f);
                    v.osc.advance (dt);
                    s = std::sin (twoPi * v.osc.phase + 2.2f * std::sin (twoPi * v.mod.phase));
                }

                // Quick attack, exponential fade across the sweep.
                const float att = juce::jmin (1.0f, (float) v.age * 0.01f);
                const float y = s * v.amp * att;
                v.amp *= v.decay;
                ++v.age;
                if (v.amp < 1.0e-5f) v.active = false;

                sumL += y * (1.0f - v.pan);
                sumR += y * v.pan;
            }

            left[i]  = safety (d * left[i]  + lv * sumL);
            right[i] = safety (d * right[i] + lv * sumR);
        }
    }

private:
    static constexpr float zapGain = 0.6f;

    struct Voice
    {
        Osc osc, mod;
        float amp = 0.0f, decay = 0.999f, pan = 0.5f;
        int wait = 0, age = 0;
        bool active = false;
    };

    void fire (float strength, int wait)
    {
        Voice* slot = &voices[0];
        for (auto& v : voices)
        {
            if (! v.active) { slot = &v; break; }
            if (v.amp < slot->amp) slot = &v;
        }

        *slot = {};
        slot->active = true;
        slot->amp = strength;
        slot->wait = wait;
        slot->decay = std::exp (-5.0f / (timeSamples * 1.2f));
        constexpr float pans[] { 0.2f, 0.8f, 0.35f, 0.65f };
        slot->pan = pans[(nextPan++) & 3];
    }

    OnsetDetector onset;
    std::array<Voice, 8> voices;
    Smooth level, dry;
    float startLog = 11.0f, endLog = 7.0f, timeSamples = 4800.0f;
    int wave = 0, burst = 1;
    uint32_t nextPan = 0;
    bool fresh = true;
};

//==============================================================================
// STARLING: a resonant filter flown by a Lorenz attractor. The cutoff never
// repeats: it loops, swerves and dives like a flock turning, with the left and
// right filters steered by different axes of the same chaotic flight.
//==============================================================================
class ChaosFilter final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        for (auto* s : { &depth, &center, &reso, &drive, &mix, &spread })
            s->setTime (40.0f, sampleRate);
    }

    void reset() override
    {
        lx = 1.0f; ly = 1.0f; lz = 20.0f;
        for (auto& f : filters) f.reset();
        fresh = true;
    }

    void update() override
    {
        // The attractor makes roughly one lap per 0.7 time units.
        step = knob (0) * 0.7f * (float) chunk / (float) sampleRate;
        depth.set (knob (1), fresh);
        center.set (std::log2 (knob (2)), fresh);
        reso.set (knob (3), fresh);
        mode = juce::roundToInt (knob (4));
        drive.set (knob (5), fresh);
        spread.set (knob (6), fresh);
        mix.set (knob (7), fresh);
        fresh = false;
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        juce::ScopedNoDenormals noDenormals;

        for (int i = 0; i < n; ++i)
        {
            const float dp = depth.next(), c = center.next(), q = reso.next(), dr = drive.next(), m = mix.next(), sp = spread.next();

            if ((counter++ % chunk) == 0)
            {
                // Two half steps keep the integration stable at the fastest rate.
                for (int k = 0; k < 2; ++k)
                {
                    const float h = 0.5f * step;
                    const float dx = 10.0f * (ly - lx);
                    const float dy = lx * (28.0f - lz) - ly;
                    const float dz = lx * ly - (8.0f / 3.0f) * lz;
                    lx = juce::jlimit (-30.0f, 30.0f, lx + h * dx);
                    ly = juce::jlimit (-40.0f, 40.0f, ly + h * dy);
                    lz = juce::jlimit (0.0f, 60.0f, lz + h * dz);
                }

                const float a = lx / 18.0f;
                const float b = (1.0f - sp) * a + sp * (ly / 24.0f);
                filters[0].setup (std::exp2 (c + dp * a), q, sampleRate);
                filters[1].setup (std::exp2 (c + dp * b), q, sampleRate);
                trim = 1.0f / std::pow (juce::jmax (1.0f, q), mode == 0 ? 0.3f : 0.5f);
            }

            const float g = 1.0f + dr * 12.0f;
            const float makeup = 0.62f / (1.0f + dr * 4.0f);
            float* io[2] { left, right };
            for (int ch = 0; ch < 2; ++ch)
            {
                const float dry = io[ch][i];
                const float x = dr > 0.001f ? std::tanh (dry * g) * makeup : dry;
                const auto o = filters[(size_t) ch].process (x);
                float wet = mode == 0 ? o.low : (mode == 1 ? o.band * 1.6f : o.high);
                wet = flush (wet * trim);
                io[ch][i] = safety ((1.0f - m) * dry + m * wet);
            }
        }
    }

private:
    static constexpr int chunk = 8;

    std::array<dsp::Svf, 2> filters;
    Smooth depth, center, reso, drive, mix, spread;
    float lx = 1.0f, ly = 1.0f, lz = 20.0f, step = 0.0001f, trim = 1.0f;
    int mode = 0;
    uint32_t counter = 0;
    bool fresh = true;
};

//==============================================================================
// NAUTILUS: your guitar, ten fathoms down. Muffled, swaying in the current,
// with bubbles that rise out of every note you play.
//==============================================================================
class Underwater final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        for (auto& c : chans) c.line.setMaxSamples ((int) (0.05 * sampleRate) + 8);
        env.setup (3.0f, 150.0f, sampleRate);
        for (auto* s : { &depth, &wobble, &bubbles, &mix })
            s->setTime (40.0f, sampleRate);
    }

    void reset() override
    {
        for (auto& c : chans)
        {
            c.line.clear();
            c.lp.reset();
            c.lfo = c.lfoTarget = c.sway = c.swayTarget = 0.0f;
        }
        for (auto& b : bubbleVoices) b.active = false;
        env.reset();
        rng.seed (0x7f4a7c15u);
        fresh = true;
    }

    void update() override
    {
        depth.set (knob (0), fresh);
        wobble.set (knob (1), fresh);
        currentHz = knob (2);
        bubbles.set (knob (3), fresh);
        mix.set (knob (4), fresh);
        fresh = false;
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        const float inv = 1.0f / (float) sampleRate;
        const float lfoCoeff = 1.0f - std::exp (-twoPi * currentHz * 0.7f * inv);

        for (int i = 0; i < n; ++i)
        {
            const float in = 0.5f * (left[i] + right[i]);
            const float e = env.process (in);
            const float dp = depth.next(), wb = wobble.next(), bb = bubbles.next(), m = mix.next();

            // Bubbles rise out of whatever you play: louder playing, more bubbles.
            const float rate = bb * 45.0f * juce::jmin (1.0f, e * 40.0f);
            if (rate > 0.0f && rng.unipolar() < rate * inv)
                spawnBubble (e);

            float bubL = 0.0f, bubR = 0.0f;
            for (auto& b : bubbleVoices)
            {
                if (! b.active) continue;
                const float t = (float) b.age * inv;
                const float hz = b.f0 * (1.0f + b.rise * t);
                b.phase += juce::jmin (0.45f, hz * inv);
                b.phase -= std::floor (b.phase);
                const float y = std::sin (twoPi * b.phase) * b.amp;
                b.amp *= b.decay;
                ++b.age;
                if (b.amp < 1.0e-6f) b.active = false;
                bubL += y * (1.0f - b.pan);
                bubR += y * b.pan;
            }

            if ((counter++ & 15) == 0)
            {
                for (auto& c : chans)
                {
                    if (rng.unipolar() < currentHz * 16.0f * inv) c.lfoTarget = rng.bipolar();
                    if (rng.unipolar() < currentHz * 4.0f * inv)  c.swayTarget = rng.bipolar();
                    const float cutoff = 2800.0f * std::exp2 (-3.4f * dp + 0.6f * c.sway);
                    c.lp.setup (cutoff, 1.4f, sampleRate);
                }
            }

            float* io[2] { left, right };
            const float bub[2] { bubL, bubR };
            for (int ch = 0; ch < 2; ++ch)
            {
                auto& c = chans[(size_t) ch];
                c.lfo += lfoCoeff * (c.lfoTarget - c.lfo);
                c.sway += lfoCoeff * 0.25f * (c.swayTarget - c.sway);
                const float dry = io[ch][i];
                c.line.push (dry);
                const float d = (0.012f + wb * 0.009f * c.lfo) * (float) sampleRate;
                const float wet = flush (c.lp.process (c.line.read (juce::jmax (2.0f, d))).low) * 0.95f + bub[ch];
                io[ch][i] = safety ((1.0f - m) * dry + m * wet);
            }
        }
    }

private:
    struct Bubble
    {
        float f0 = 800.0f, rise = 2.0f, amp = 0.0f, decay = 0.99f, phase = 0.0f, pan = 0.5f;
        int age = 0;
        bool active = false;
    };

    void spawnBubble (float level)
    {
        Bubble* slot = &bubbleVoices[0];
        for (auto& b : bubbleVoices)
        {
            if (! b.active) { slot = &b; break; }
            if (b.amp < slot->amp) slot = &b;
        }

        // Small bubbles ring higher, shorter and rise faster in pitch.
        const float f0 = 350.0f * std::exp2 (rng.unipolar() * 2.6f);
        const float damping = 0.043f * f0 + 0.0014f * std::pow (f0, 1.5f);
        *slot = {};
        slot->active = true;
        slot->f0 = f0;
        slot->rise = 0.1f * damping;
        slot->decay = std::exp (-damping / (float) sampleRate);
        slot->amp = level * (0.6f + 0.8f * rng.unipolar()) * 1.4f;
        slot->pan = 0.5f + 0.45f * rng.bipolar();
    }

    struct Chan
    {
        dsp::DelayBuffer line;
        dsp::Svf lp;
        float lfo = 0.0f, lfoTarget = 0.0f, sway = 0.0f, swayTarget = 0.0f;
    };

    std::array<Chan, 2> chans;
    std::array<Bubble, 10> bubbleVoices;
    Follower env;
    Rng rng;
    Smooth depth, wobble, bubbles, mix;
    float currentHz = 0.5f;
    uint32_t counter = 0;
    bool fresh = true;
};

//==============================================================================
// CRICKET: a dial-up handshake that plays along. Two warbling FSK tones hop
// and chirp in time with your dynamics, picks spit bursts of line hiss, and
// your guitar gets ring-modulated by the carrier.
//==============================================================================
class DialUp final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }

    void prepared() override
    {
        env.setup (2.0f, 60.0f, sampleRate);
        onset.setup (sampleRate);
        for (auto& c : chans) c.logHz.setTime (1.5f, sampleRate);
        hissDecay = std::exp (-1.0f / (0.07f * (float) sampleRate));
        for (auto* s : { &chirp, &hiss, &ring, &mix })
            s->setTime (30.0f, sampleRate);
    }

    void reset() override
    {
        env.reset();
        onset.reset();
        uint32_t seed = 0x1234567u;
        for (auto& c : chans)
        {
            c.carrier = c.mod = 0.0f;
            c.countdown = 0;
            c.bp.reset();
            c.rng.seed (seed);
            c.logHz.set (10.0f, true);
            seed = seed * 2654435761u + 1u;
        }
        hissEnv = 0.0f;
        fresh = true;
    }

    void update() override
    {
        baud = knob (0);
        toneLog = std::log2 (knob (1));
        chirp.set (knob (2), fresh);
        hiss.set (knob (3), fresh);
        ring.set (knob (4), fresh);
        mix.set (knob (5), fresh);
        for (auto& c : chans) c.bp.setup (2100.0f, 1.2f, sampleRate);
        fresh = false;
    }

    void processChannel (int, float*, int) override {}

    void processStereo (float* left, float* right, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        constexpr float steps[] { 0.0f, 0.585f, 0.874f, 1.137f, 1.459f, -0.415f };
        const float inv = 1.0f / (float) sampleRate;

        for (int i = 0; i < n; ++i)
        {
            const float in = 0.5f * (left[i] + right[i]);
            const float e = env.process (in);
            const float drive = juce::jmin (1.0f, e * 25.0f);   // 0..1 dynamics

            if (onset.process (in) > 0.0f) hissEnv = 1.0f;
            else                           hissEnv *= hissDecay;
            if (hissEnv < 1.0e-6f) hissEnv = 0.0f;

            const float ch = chirp.next(), hs = hiss.next(), rg = ring.next(), m = mix.next();

            float* io[2] { left, right };
            for (int c = 0; c < 2; ++c)
            {
                auto& st = chans[(size_t) c];

                // Hop to a new tone; harder playing talks faster.
                if (--st.countdown <= 0)
                {
                    const int pick = (int) (st.rng.nextInt() % 6u);
                    st.logHz.set (toneLog + steps[pick] + (c == 1 ? 0.25f : 0.0f), false);
                    st.countdown = (int) ((float) sampleRate / (baud * (0.5f + 1.5f * drive)));
                }

                const float hz = std::exp2 (st.logHz.next());
                st.mod += juce::jmin (0.45f, hz * 1.414f * inv);
                st.mod -= std::floor (st.mod);
                st.carrier += juce::jmin (0.45f, hz * inv);
                st.carrier -= std::floor (st.carrier);
                const float tone = std::sin (twoPi * st.carrier + ch * 4.0f * drive * std::sin (twoPi * st.mod));

                const float noise = st.bp.process (st.rng.bipolar()).band;
                const float dry = io[c][i];
                const float wet = e * toneGain * tone
                                + rg * 2.2f * dry * tone
                                + hs * hissEnv * e * 6.0f * noise;
                io[c][i] = safety (std::sqrt (1.0f - m) * dry + std::sqrt (m) * flush (wet));
            }
        }
    }

private:
    static constexpr float toneGain = 0.5f;

    struct Chan
    {
        Rng rng;
        Smooth logHz;
        dsp::Svf bp;
        float carrier = 0.0f, mod = 0.0f;
        int countdown = 0;
    };

    std::array<Chan, 2> chans;
    Follower env;
    OnsetDetector onset;
    Smooth chirp, hiss, ring, mix;
    float baud = 12.0f, toneLog = 10.0f, hissEnv = 0.0f, hissDecay = 0.999f;
    bool fresh = true;
};

} // namespace

void registerWeirdPedals (Registry& r)
{
    r.add<GuitarSynth> ({ "weird.synth", "BASILISK", "guitar synth", "Weird",
                          "Tracks your notes and plays them on detuned oscillators through a snapping resonant filter",
                          juce::Colour (0xff1d5c3a), juce::Colour (0xffd8ff6a),
                          { choice ("wave", "Wave", { "Saw", "Square", "Reed" }, 0),
                            choice ("octave", "Octave", { "-2", "-1", "0", "+1" }, 2),
                            knob ("sub", "Sub", 0.0f, 1.0f, 0.5f, "%"),
                            knob ("glide", "Glide", 1.0f, 400.0f, 25.0f, "ms", 0.4f),
                            knob ("cutoff", "Cutoff", 80.0f, 6000.0f, 500.0f, "Hz", 0.35f),
                            knob ("reso", "Reso", 0.5f, 14.0f, 4.0f, {}, 0.5f),
                            knob ("env", "Env", 0.0f, 1.0f, 0.6f, "%"),
                            knob ("mix", "Mix", 0.0f, 1.0f, 0.8f, "%") } });

    r.add<FrequencyShifter> ({ "weird.shift", "ECLIPSE", "frequency shifter", "Weird",
                               "Moves every partial by the same Hz: barber-pole swirl, then clang and metal",
                               juce::Colour (0xff141018), juce::Colour (0xffffb347),
                               { knob ("shift", "Shift", 0.0f, 2000.0f, 6.0f, "Hz", 0.25f),
                                 choice ("mode", "Mode", { "Up", "Down", "Split" }, 2),
                                 knob ("feedback", "Feedback", 0.0f, 0.92f, 0.45f, "%"),
                                 knob ("delay", "Delay", 2.0f, 400.0f, 9.0f, "ms", 0.35f),
                                 knob ("drift", "Drift", 0.0f, 1.0f, 0.3f, "%"),
                                 knob ("mix", "Mix", 0.0f, 1.0f, 0.5f, "%") } });

    r.add<SympatheticStrings> ({ "weird.strings", "AURORA", "sympathetic strings", "Weird",
                                 "Eight drone strings tuned to a chord that ring along with whatever you play",
                                 juce::Colour (0xff23104a), juce::Colour (0xff7dffcf),
                                 { choice ("key", "Key", keyNames(), 4),
                                   choice ("chord", "Chord", chordNames(), 0),
                                   knob ("decay", "Decay", 400.0f, 12000.0f, 3500.0f, "ms", 0.5f),
                                   knob ("bright", "Bright", 0.0f, 1.0f, 0.55f, "%"),
                                   knob ("mix", "Mix", 0.0f, 1.0f, 0.5f, "%"),
                                   knob ("width", "Width", 0.0f, 1.0f, 0.7f, "%"),
                                   choice ("range", "Range", { "Low", "Mid", "High" }, 1) } });

    r.add<FeedbackMachine> ({ "weird.feedback", "LOON", "feedback machine", "Weird",
                              "Hold a note and amp feedback blooms in, locked to the note or a harmonic above it",
                              juce::Colour (0xffb8321f), juce::Colour (0xfffff0c2),
                              { choice ("harmonic", "Harmonic", { "Unison", "Octave", "Fifth", "2 Oct" }, 1),
                                knob ("bloom", "Bloom", 80.0f, 4000.0f, 700.0f, "ms", 0.4f),
                                knob ("level", "Howl", 0.0f, 1.0f, 0.6f, "%"),
                                knob ("sustain", "Sustain", -66.0f, -24.0f, -50.0f, "dB"),
                                knob ("wobble", "Wobble", 0.0f, 1.0f, 0.35f, "%"),
                                knob ("drive", "Drive", 0.0f, 1.0f, 0.3f, "%"),
                                knob ("width", "Width", 0.0f, 1.0f, 0.5f, "%") } });

    r.add<BellMaker> ({ "weird.bell", "SATURN", "bell ring modulator", "Weird",
                        "Ring-mods each note against inharmonic partials of itself: picks strike bells, gongs and glass",
                        juce::Colour (0xffe8d9a8), juce::Colour (0xff3b1f5c),
                        { choice ("metal", "Metal", metalNames(), 0),
                          knob ("tune", "Tune", 0.5f, 2.0f, 1.0f, "x", 0.63f),
                          knob ("strike", "Strike", 60.0f, 5000.0f, 900.0f, "ms", 0.4f),
                          knob ("sustain", "Sustain", 0.0f, 1.0f, 0.1f, "%"),
                          knob ("shimmer", "Shimmer", 0.0f, 1.0f, 0.4f, "%"),
                          knob ("tone", "Tone", 500.0f, 12000.0f, 5000.0f, "Hz", 0.4f),
                          knob ("mix", "Mix", 0.0f, 1.0f, 0.7f, "%") } });

    r.add<LaserZap> ({ "weird.zap", "FIREFLY", "laser zapper", "Weird",
                       "Every pick fires pitch-diving laser zaps in bursts that bounce left and right",
                       juce::Colour (0xff0b2b1e), juce::Colour (0xffeaff3d),
                       { knob ("start", "Start", 100.0f, 8000.0f, 3200.0f, "Hz", 0.3f),
                         knob ("end", "End", 40.0f, 8000.0f, 160.0f, "Hz", 0.3f),
                         knob ("time", "Time", 20.0f, 800.0f, 150.0f, "ms", 0.5f),
                         choice ("wave", "Wave", { "Sine", "Square", "Ray" }, 2),
                         choice ("burst", "Burst", { "1", "2", "3", "4", "6" }, 1),
                         knob ("sense", "Sense", 0.0f, 1.0f, 0.6f, "%"),
                         knob ("level", "Zap", 0.0f, 1.0f, 0.5f, "%"),
                         knob ("dry", "Dry", 0.0f, 1.0f, 1.0f, "%") } });

    r.add<ChaosFilter> ({ "weird.chaos", "STARLING", "chaotic filter", "Weird",
                          "A resonant filter steered by a strange attractor: sweeps that loop and swerve but never repeat",
                          juce::Colour (0xff2b2f6e), juce::Colour (0xffff8fd0),
                          { knob ("rate", "Rate", 0.05f, 12.0f, 0.8f, "Hz", 0.4f),
                            knob ("depth", "Depth", 0.0f, 4.0f, 2.2f),
                            knob ("center", "Center", 150.0f, 5000.0f, 650.0f, "Hz", 0.4f),
                            knob ("reso", "Reso", 0.5f, 15.0f, 4.0f, {}, 0.5f),
                            choice ("mode", "Mode", { "Low", "Band", "High" }, 1),
                            knob ("drive", "Drive", 0.0f, 1.0f, 0.2f, "%"),
                            knob ("spread", "Spread", 0.0f, 1.0f, 0.7f, "%"),
                            knob ("mix", "Mix", 0.0f, 1.0f, 1.0f, "%") } });

    r.add<Underwater> ({ "weird.underwater", "NAUTILUS", "underwater", "Weird",
                         "Muffled, swaying and seasick, with bubbles rising out of every note",
                         juce::Colour (0xff0e4f5c), juce::Colour (0xffbff8ff),
                         { knob ("depth", "Depth", 0.0f, 1.0f, 0.55f, "%"),
                           knob ("wobble", "Wobble", 0.0f, 1.0f, 0.5f, "%"),
                           knob ("current", "Current", 0.1f, 6.0f, 0.7f, "Hz", 0.5f),
                           knob ("bubbles", "Bubbles", 0.0f, 1.0f, 0.5f, "%"),
                           knob ("mix", "Mix", 0.0f, 1.0f, 0.9f, "%") } });

    r.add<DialUp> ({ "weird.dialup", "CRICKET", "dial-up modem", "Weird",
                     "Two chirping modem tones that hop and warble with your dynamics, plus line hiss on every pick",
                     juce::Colour (0xffc9c4b0), juce::Colour (0xff1c3d2a),
                     { knob ("baud", "Baud", 2.0f, 60.0f, 11.0f, "Hz", 0.5f),
                       knob ("tone", "Tone", 200.0f, 3000.0f, 1100.0f, "Hz", 0.5f),
                       knob ("chirp", "Chirp", 0.0f, 1.0f, 0.4f, "%"),
                       knob ("hiss", "Hiss", 0.0f, 1.0f, 0.3f, "%"),
                       knob ("ring", "Ring", 0.0f, 1.0f, 0.5f, "%"),
                       knob ("mix", "Mix", 0.0f, 1.0f, 0.5f, "%") } });
}

} // namespace woodshed
