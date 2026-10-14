#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// ROOM: stereo room reverb (takes mono in, gives stereo out).
class RoomReverb final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }
    void prepared() override                    { reverb.setSampleRate (sampleRate); }
    void reset() override                       { reverb.reset(); }

    void update() override
    {
        juce::Reverb::Parameters p;
        p.roomSize = knob (0);
        p.damping  = knob (1);
        p.wetLevel = knob (2) * 0.3f;
        p.dryLevel = 0.5f * (1.0f - 0.3f * knob (2));   // juce::Reverb doubles the dry signal internally
        p.width    = 1.0f;
        reverb.setParameters (p);
    }

    void processChannel (int, float*, int) override {}
    void processStereo (float* left, float* right, int n) override   { reverb.processStereo (left, right, n); }

private:
    juce::Reverb reverb;
};

//==============================================================================
// Building blocks for the reverb family.

constexpr float twoPi = juce::MathConstants<float>::twoPi;

/** Linear below half the ceiling, then a smooth rational knee up to the ceiling. */
inline float softLimit (float x, float ceiling = 1.6f)
{
    const float knee = 0.5f * ceiling;
    const float a = std::abs (x);
    if (a <= knee) return x;
    const float over = (a - knee) / (ceiling - knee);
    return std::copysign (knee + (ceiling - knee) * over / (1.0f + over), x);
}

inline float msToSamples (float ms, double sr)            { return ms * 0.001f * (float) sr; }
inline int scaled (int samples48k, double sr)             { return juce::jmax (1, (int) std::lround (samples48k * sr / 48000.0)); }

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

/** Sine oscillator as a rotating phasor. */
struct Lfo
{
    float c = 1.0f, s = 0.0f, rc = 1.0f, rs = 0.0f;

    void setPhase (float radians)                           { c = std::cos (radians); s = std::sin (radians); }
    void setRate (float hz, double sr)
    {
        const float w = twoPi * hz / (float) sr;
        rc = std::cos (w); rs = std::sin (w);
        const float mag = 1.0f / std::sqrt (c * c + s * s);
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

/** Integer delay line: tap (d) is the sample pushed d samples ago (d >= 1). */
struct Line
{
    std::vector<float> buf;
    int idx = 0, size = 1;

    void setup (int maxDelay)                               { size = juce::jmax (2, maxDelay + 1); buf.assign ((size_t) size, 0.0f); idx = 0; }
    void clear()                                            { std::fill (buf.begin(), buf.end(), 0.0f); }
    float tap (int d) const                                 { int j = idx - d; if (j < 0) j += size; return buf[(size_t) j]; }
    void push (float x)                                     { buf[(size_t) idx] = x; if (++idx == size) idx = 0; }
};

/** Schroeder allpass, H = (g + z^-L) / (1 + g z^-L). tap() reads its internal line. */
struct Allpass
{
    Line line;
    int len = 1;
    float g = 0.5f;

    void setup (int length, float gain)                     { len = juce::jmax (1, length); line.setup (len); g = gain; }
    void clear()                                            { line.clear(); }
    float tap (int d) const                                 { return line.tap (d); }

    float process (float x)
    {
        const float d = line.tap (len);
        const float w = x - g * d;
        line.push (w);
        return d + g * w;
    }
};

/** Allpass with a modulated (fractional) length. */
struct ModAllpass
{
    dsp::DelayBuffer line;
    float g = 0.5f;

    void setup (int maxLength, float gain)                  { line.setMaxSamples (maxLength + 4); g = gain; }
    void clear()                                            { line.clear(); }

    float process (float x, float length)
    {
        const float d = line.read (juce::jmax (2.0f, length - 1.0f));
        const float w = x - g * d;
        line.push (w);
        return d + g * w;
    }
};

/** Octave/interval shifter: two Hann-windowed taps sweeping through a short delay line. */
struct GrainShifter
{
    dsp::DelayBuffer line;
    dsp::Svf highPass, lowPass;
    float window = 2048.0f, phase = 0.0f, ratio = 2.0f;

    void setup (int windowSamples)                          { window = (float) windowSamples; line.setMaxSamples (windowSamples + 8); }
    void clear()                                            { line.clear(); highPass.reset(); lowPass.reset(); }

    float process (float x)
    {
        line.push (x);
        phase += std::abs (ratio - 1.0f) / window;
        if (phase >= 1.0f) phase -= 1.0f;
        const float pb = phase < 0.5f ? phase + 0.5f : phase - 0.5f;
        const bool up = ratio > 1.0f;
        const float da = 2.0f + window * (up ? 1.0f - phase : phase);
        const float db = 2.0f + window * (up ? 1.0f - pb : pb);
        const float s = 4.0f * phase * (1.0f - phase);          // ~sin(pi p); windows still sum to exactly 1
        const float wa = s * s;
        return wa * line.read (da) + (1.0f - wa) * line.read (db);
    }
};

//==============================================================================
/**
    Eight-line feedback delay network: modulated, mutually prime lines, a
    Hadamard mixing matrix, per-line absorption (decay set as RT60 at low and
    high frequencies) and a soft limiter on every write. A hook lets a pedal
    alter the line signals inside the loop (diffusers, pitch shifters...).
*/
struct Fdn8
{
    static constexpr int N = 8;

    std::array<dsp::DelayBuffer, N> lines;
    std::array<float, N> base {}, gain {}, pole {}, lp {};
    std::array<Lfo, N> lfo;
    Glide scale, depth;
    double sr = 48000.0;

    static constexpr float inSign[N]  { 1, -1,  1,  1, -1, -1,  1, -1 };
    static constexpr float outL[N]    { 1,  1, -1,  1, -1,  1,  1, -1 };
    static constexpr float outR[N]    { 1, -1,  1,  1,  1, -1,  1, -1 };

    void prepare (const std::array<int, N>& lengths48k, double sampleRate, float maxScale, float maxModSamples48k,
                  float lfoHz, float scaleGlideSeconds = 0.4f)
    {
        sr = sampleRate;
        const float srScale = (float) (sr / 48000.0);
        for (int i = 0; i < N; ++i)
        {
            base[(size_t) i] = (float) lengths48k[(size_t) i] * srScale;
            lines[(size_t) i].setMaxSamples ((int) (base[(size_t) i] * maxScale + 2.0f * maxModSamples48k * srScale) + 8);
            lfo[(size_t) i].setRate (lfoHz * (0.62f + 0.23f * (float) i), sr);
        }
        scale.setTime (scaleGlideSeconds, sr);
        depth.setTime (0.1f, sr);
    }

    void reset()
    {
        for (int i = 0; i < N; ++i)
        {
            lines[(size_t) i].clear();
            lp[(size_t) i] = 0.0f;
            lfo[(size_t) i].setPhase (0.9f * (float) i);
        }
        scale.reset(); depth.reset();
    }

    /** decayRate = 1 / RT60 in 1/s (0 = infinite). hfRate likewise for the top of the band. */
    void setDecay (float decayRate, float hfRate)
    {
        for (int i = 0; i < N; ++i)
        {
            const float lenSeconds = base[(size_t) i] * scale.value / (float) sr;
            gain[(size_t) i] = std::pow (10.0f, -3.0f * lenSeconds * decayRate);
            const float r = std::pow (10.0f, -3.0f * lenSeconds * juce::jmax (0.0f, hfRate - decayRate));
            pole[(size_t) i] = (1.0f - r) / (1.0f + r);
        }
    }

    void setRates (float lfoHz)
    {
        for (int i = 0; i < N; ++i) lfo[(size_t) i].setRate (lfoHz * (0.62f + 0.23f * (float) i), sr);
    }

    template <typename Hook>
    void process (float in, float& l, float& r, Hook&& hook)
    {
        const float sc = scale.next();
        const float dep = depth.next();
        std::array<float, N> z;
        float sumL = 0.0f, sumR = 0.0f;

        for (int i = 0; i < N; ++i)
        {
            const auto k = (size_t) i;
            const float y = lines[k].read (juce::jmax (2.0f, base[k] * sc + dep * (1.0f + lfo[k].next()) - 1.0f));
            sumL += outL[i] * y;
            sumR += outR[i] * y;
            lp[k] = y + pole[k] * (lp[k] - y);
            z[k] = gain[k] * lp[k];
        }

        hook (z);

        // Fast Walsh-Hadamard transform, orthonormal.
        for (int h = 1; h < N; h <<= 1)
            for (int i = 0; i < N; i += h << 1)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = z[(size_t) j], b = z[(size_t) (j + h)];
                    z[(size_t) j] = a + b;
                    z[(size_t) (j + h)] = a - b;
                }

        constexpr float norm = 0.35355339f;
        for (int i = 0; i < N; ++i)
            lines[(size_t) i].push (softLimit (z[(size_t) i] * norm + inSign[i] * in));

        l = sumL * norm;
        r = sumR * norm;
    }
};

/** Pre-delay, low cut and a short chain of input diffusers, mono in. */
struct ReverbInput
{
    dsp::DelayBuffer predelay;
    Glide delay;
    dsp::Svf lowCut;
    std::array<Allpass, 4> diffusers;
    int numDiffusers = 4;

    void prepare (double sr, float maxPredelayMs, const std::array<int, 4>& lengths48k, float g1, float g2, int count = 4)
    {
        predelay.setMaxSamples ((int) msToSamples (maxPredelayMs + 5.0f, sr));
        delay.setTime (0.15f, sr);
        numDiffusers = count;
        for (size_t i = 0; i < 4; ++i)
            diffusers[i].setup (scaled (lengths48k[i], sr), i < 2 ? g1 : g2);
    }

    void reset()
    {
        predelay.clear(); delay.reset(); lowCut.reset();
        for (auto& d : diffusers) d.clear();
    }

    void set (float predelayMs, float lowCutHz, double sr)
    {
        delay.set (juce::jmax (2.0f, msToSamples (predelayMs, sr)));
        lowCut.setup (lowCutHz, 0.6f, sr);
    }

    float process (float x)
    {
        predelay.push (x);
        float v = lowCut.process (predelay.read (delay.next())).high;
        for (int i = 0; i < numDiffusers; ++i) v = diffusers[(size_t) i].process (v);
        return v;
    }
};

/** Dry stays at unity until Mix passes halfway, then fades; wet reaches full level at halfway. */
struct WetDry
{
    Glide dry, wet;
    OnePoleHP dcL, dcR;

    void prepare (double sr)                                { dry.setTime (0.02f, sr); wet.setTime (0.02f, sr); dcL.setCutoff (12.0f, sr); dcR.setCutoff (12.0f, sr); }
    void reset()                                            { dry.reset(); wet.reset(); dcL.reset(); dcR.reset(); }
    void set (float mix, float wetCalibration)
    {
        dry.set (juce::jmin (1.0f, 2.0f * (1.0f - mix)));
        wet.set (juce::jmin (1.0f, 2.0f * mix) * wetCalibration);
    }
    void mix (float& left, float& right, float wl, float wr)
    {
        const float d = dry.next(), w = wet.next();
        left  = d * left  + w * dcL.process (wl);
        right = d * right + w * dcR.process (wr);
    }
};

//==============================================================================
// VAULT: hall. Diffused input into a modulated 8-line FDN; long, smooth and wide.
class HallReverb final : public Pedal
{
public:
    bool producesStereo() const override                    { return true; }
    void processChannel (int, float*, int) override {}

    void prepared() override
    {
        input.prepare (sampleRate, 200.0f, { 229, 173, 611, 447 }, 0.72f, 0.62f);
        fdn.prepare ({ 1427, 1777, 2087, 2477, 2797, 3089, 3361, 3797 }, sampleRate, 1.5f, 14.0f, 0.5f);
        out.prepare (sampleRate);
    }

    void reset() override                                   { input.reset(); fdn.reset(); out.reset(); }

    void update() override
    {
        const float rt = knob (0) * 0.001f;
        fdn.scale.set (0.5f + knob (1));
        input.set (knob (2), knob (5), sampleRate);
        fdn.setDecay (1.0f / rt, 1.0f / (rt * (1.0f - 0.9f * knob (3))));
        fdn.depth.set (knob (4) * 14.0f * (float) (sampleRate / 48000.0));
        out.set (knob (6), 0.55f);
    }

    void processStereo (float* left, float* right, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            float wl, wr;
            fdn.process (input.process (0.5f * (left[i] + right[i])), wl, wr, [] (auto&) {});
            out.mix (left[i], right[i], wl, wr);
        }
    }

private:
    ReverbInput input;
    Fdn8 fdn;
    WetDry out;
};

//==============================================================================
// GILT: plate. A figure-of-eight tank of allpasses and delays (Dattorro topology), dense and bright.
class PlateReverb final : public Pedal
{
public:
    bool producesStereo() const override                    { return true; }
    void processChannel (int, float*, int) override {}

    void prepared() override
    {
        const double s = sampleRate / 29761.0;
        auto at = [s] (int samples) { return juce::jmax (1, (int) std::lround (samples * s)); };

        input.prepare (sampleRate, 150.0f, { 229, 173, 611, 447 }, 0.75f, 0.625f);

        excursion = 8.0f * (float) s;
        modL.setup (at (672) + (int) excursion + 4, -0.7f);
        modR.setup (at (908) + (int) excursion + 4, -0.7f);
        lenModL = (float) at (672); lenModR = (float) at (908);
        apL.setup (at (1800), 0.5f);
        apR.setup (at (2656), 0.5f);
        d1L = at (4453); d2L = at (3720); d1R = at (4217); d2R = at (3163);
        delL1.setup (d1L); delL2.setup (d2L); delR1.setup (d1R); delR2.setup (d2R);

        const int tapsL[] { 266, 2974, 1913, 1996, 1990, 187, 1066 };
        const int tapsR[] { 353, 3627, 1228, 2673, 2111, 335, 121 };
        for (size_t i = 0; i < 7; ++i) { tapL[i] = at (tapsL[i]); tapR[i] = at (tapsR[i]); }

        lfoL.setRate (0.77f, sampleRate);
        lfoR.setRate (0.93f, sampleRate);
        decay.setTime (0.05f, sampleRate);
        depth.setTime (0.1f, sampleRate);
        out.prepare (sampleRate);
    }

    void reset() override
    {
        input.reset(); modL.clear(); modR.clear(); apL.clear(); apR.clear();
        delL1.clear(); delL2.clear(); delR1.clear(); delR2.clear();
        dampL.reset(); dampR.reset(); decay.reset(); depth.reset(); out.reset();
        lfoL.setPhase (0.0f); lfoR.setPhase (1.6f);
    }

    void update() override
    {
        // Each full trip round the tank is ~0.725 s and passes four decay gains.
        const float rt = knob (0) * 0.001f;
        decay.set (juce::jmin (0.95f, std::pow (10.0f, -3.0f * 0.7254f / (4.0f * rt))));
        input.set (knob (1), knob (4), sampleRate);
        dampL.setCutoff (knob (2), sampleRate);
        dampR.setCutoff (knob (2), sampleRate);
        depth.set (knob (3) * excursion);
        out.set (knob (5), 0.6f);
    }

    void processStereo (float* left, float* right, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float x = input.process (0.5f * (left[i] + right[i]));
            const float g = decay.next();
            const float dep = depth.next();
            const float endL = delL2.tap (d2L), endR = delR2.tap (d2R);

            const float a = modL.process (x + g * endR, lenModL + dep * lfoL.next());
            const float l1 = delL1.tap (d1L);
            delL1.push (softLimit (a));
            delL2.push (apL.process (g * dampL.process (l1)));

            const float b = modR.process (x + g * endL, lenModR + dep * lfoR.next());
            const float r1 = delR1.tap (d1R);
            delR1.push (softLimit (b));
            delR2.push (apR.process (g * dampR.process (r1)));

            const float wl = delR1.tap (tapL[0]) + delR1.tap (tapL[1]) - apR.tap (tapL[2]) + delR2.tap (tapL[3])
                           - delL1.tap (tapL[4]) - apL.tap (tapL[5]) - delL2.tap (tapL[6]);
            const float wr = delL1.tap (tapR[0]) + delL1.tap (tapR[1]) - apL.tap (tapR[2]) + delL2.tap (tapR[3])
                           - delR1.tap (tapR[4]) - apR.tap (tapR[5]) - delR2.tap (tapR[6]);

            out.mix (left[i], right[i], 0.6f * wl, 0.6f * wr);
        }
    }

private:
    ReverbInput input;
    ModAllpass modL, modR;
    Allpass apL, apR;
    Line delL1, delL2, delR1, delR2;
    int d1L = 1, d2L = 1, d1R = 1, d2R = 1;
    std::array<int, 7> tapL {}, tapR {};
    OnePoleLP dampL, dampR;
    Lfo lfoL, lfoR;
    Glide decay, depth;
    float excursion = 8.0f, lenModL = 1000.0f, lenModR = 1000.0f;
    WetDry out;
};

//==============================================================================
// COIL: spring. Two springs, each a loop of stretched allpasses (dispersion = the chirpy drip),
// band-limited like a real tank, with a sway that wobbles the springs when you dig in.
class SpringReverb final : public Pedal
{
public:
    static constexpr int preStages = 30, loopStages = 40;

    bool producesStereo() const override                    { return true; }
    void processChannel (int, float*, int) override {}

    void prepared() override
    {
        stretch = juce::jmax (1, (int) std::lround (sampleRate / (2.0 * 4200.0)));
        const float loopsMs[] { 31.3f, 38.9f };
        for (size_t s = 0; s < 2; ++s)
        {
            auto& sp = springs[s];
            sp.state.assign ((size_t) ((preStages + loopStages) * stretch), 0.0f);
            sp.loopDelay = msToSamples (loopsMs[s], sampleRate);
            sp.loop.setMaxSamples ((int) (sp.loopDelay + msToSamples (3.0f, sampleRate)) + 8);
            sp.sway.setRate (s == 0 ? 4.3f : 3.7f, sampleRate);
            sp.dc.setCutoff (70.0f, sampleRate);
        }
        inHp.setup (130.0f, 0.7f, sampleRate);
        inLp.setup (4800.0f, 0.7f, sampleRate);
        swayDecay = std::exp (-1.0f / (0.45f * (float) sampleRate));
        swaySmooth = dsp::onePoleCoeff (30.0f, sampleRate);
        fastAtk = 1.0f - std::exp (-1.0f / (0.0005f * (float) sampleRate));
        fastRel = 1.0f - std::exp (-1.0f / (0.02f * (float) sampleRate));
        slowCoef = 1.0f - std::exp (-1.0f / (0.25f * (float) sampleRate));
        cooldownSamples = (int) (0.12 * sampleRate);
        for (auto& sp : springs) sp.feedback.setTime (0.05f, sampleRate);
        coef.setTime (0.05f, sampleRate);
        out.prepare (sampleRate);
    }

    void reset() override
    {
        for (size_t s = 0; s < 2; ++s)
        {
            auto& sp = springs[s];
            std::fill (sp.state.begin(), sp.state.end(), 0.0f);
            sp.loop.clear(); sp.dc.reset(); sp.tone.reset(); sp.image.reset();
            sp.sway.setPhase (s == 0 ? 0.0f : 2.0f);
        }
        idx = 0; inHp.reset(); inLp.reset();
        fastEnv = slowEnv = swayAmp = swayTarget = 0.0f; cooldown = 0;
        for (auto& sp : springs) sp.feedback.reset();
        coef.reset(); out.reset();
    }

    void update() override
    {
        const float rt = knob (0) * 0.001f;
        for (auto& sp : springs)
            sp.feedback.set (juce::jmin (0.96f, std::pow (10.0f, -3.0f * sp.loopDelay / (float) sampleRate / rt)));
        for (auto& sp : springs)
        {
            sp.tone.setCutoff (knob (1), sampleRate);
            sp.image.setup (4300.0f, 0.6f, sampleRate);
        }
        coef.set (0.35f + 0.4f * knob (2));
        boing = knob (3);
        out.set (knob (4), 0.9f);
    }

    void processStereo (float* left, float* right, int n) override
    {
        const float maxSway = msToSamples (1.3f, sampleRate);

        for (int i = 0; i < n; ++i)
        {
            const float in = inLp.process (inHp.process (0.5f * (left[i] + right[i])).high).low;
            const float a = coef.next();

            // Transient detector: a hard pick sets the springs swaying.
            const float mag = std::abs (in);
            fastEnv += (mag > fastEnv ? fastAtk : fastRel) * (mag - fastEnv);
            slowEnv += slowCoef * (mag - slowEnv);
            if (cooldown > 0) --cooldown;
            if (cooldown == 0 && fastEnv > 2.5f * slowEnv + 0.004f)
            {
                swayTarget = juce::jmax (swayTarget, boing * maxSway * juce::jmin (1.0f, fastEnv * 6.0f));
                cooldown = cooldownSamples;
            }
            swayTarget *= swayDecay;
            swayAmp += swaySmooth * (swayTarget - swayAmp);

            float o[2];
            for (size_t s = 0; s < 2; ++s)
            {
                auto& sp = springs[s];
                const float fb = sp.feedback.next();
                const float ret = sp.loop.read (juce::jmax (2.0f, sp.loopDelay + swayAmp * (1.0f + sp.sway.next())));
                o[s] = ret;

                float v = sp.dc.process (sp.tone.process (sp.image.process (ret).low));
                float x = in;
                float* st = sp.state.data();
                const int k = stretch;

                for (int stg = 0; stg < preStages + loopStages; ++stg)
                {
                    if (stg == preStages) x = x + fb * v;     // close the loop after the input dispersion
                    float& cell = st[stg * k + idx];
                    const float wd = cell;
                    const float wn = x - a * wd;
                    cell = wn;
                    x = a * wn + wd;
                }

                sp.loop.push (softLimit (x));
            }

            if (++idx >= stretch) idx = 0;
            out.mix (left[i], right[i], 0.9f * o[0] + 0.1f * o[1], 0.9f * o[1] + 0.1f * o[0]);
        }
    }

private:
    struct Spring
    {
        std::vector<float> state;
        dsp::DelayBuffer loop;
        float loopDelay = 1000.0f;
        OnePoleLP tone;
        OnePoleHP dc;
        dsp::Svf image;
        Lfo sway;
        Glide feedback;
    };

    std::array<Spring, 2> springs;
    dsp::Svf inHp, inLp;
    int stretch = 6, idx = 0, cooldown = 0, cooldownSamples = 1000;
    float boing = 0.0f, fastEnv = 0.0f, slowEnv = 0.0f, swayAmp = 0.0f, swayTarget = 0.0f;
    float swayDecay = 0.999f, swaySmooth = 0.01f, fastAtk = 0.1f, fastRel = 0.01f, slowCoef = 0.001f;
    Glide coef;
    WetDry out;
};

//==============================================================================
// HALO: shimmer. Half the FDN lines run through interval shifters inside the loop,
// so every pass climbs another step and the tail blooms upward.
class ShimmerReverb final : public Pedal
{
public:
    bool producesStereo() const override                    { return true; }
    void processChannel (int, float*, int) override {}

    void prepared() override
    {
        input.prepare (sampleRate, 200.0f, { 229, 173, 611, 447 }, 0.72f, 0.62f);
        fdn.prepare ({ 1553, 1871, 2161, 2539, 2887, 3203, 3541, 3923 }, sampleRate, 1.0f, 12.0f, 0.45f);
        const int windows[] { 2311, 2053, 2477, 1999 };
        for (size_t i = 0; i < shifters.size(); ++i)
        {
            shifters[i].setup (scaled (windows[i], sampleRate));
            shifters[i].highPass.setup (220.0f, 0.6f, sampleRate);
            shifters[i].lowPass.setup (9000.0f, 0.6f, sampleRate);
        }
        amount.setTime (0.05f, sampleRate);
        out.prepare (sampleRate);
    }

    void reset() override
    {
        input.reset(); fdn.reset(); out.reset(); amount.reset();
        for (size_t i = 0; i < shifters.size(); ++i) { shifters[i].clear(); shifters[i].phase = 0.25f * (float) i; }
    }

    void update() override
    {
        static constexpr float ratios[] { 2.0f, 1.5f, 4.0f, 0.5f };
        const float ratio = ratios[juce::jlimit (0, 3, (int) knob (2))];
        for (auto& s : shifters) s.ratio = ratio;

        const float rt = knob (0) * 0.001f;
        fdn.scale.set (1.0f);
        fdn.setDecay (1.0f / rt, 1.0f / (rt * (1.0f - 0.9f * knob (3))));
        amount.set (knob (1));
        fdn.depth.set (knob (4) * 12.0f * (float) (sampleRate / 48000.0));
        input.set (knob (5), 150.0f, sampleRate);
        out.set (knob (6), 0.5f);
    }

    void processStereo (float* left, float* right, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float s = amount.next();
            float wl, wr;
            fdn.process (input.process (0.5f * (left[i] + right[i])), wl, wr, [this, s] (auto& z)
            {
                for (size_t k = 0; k < shifters.size(); ++k)
                {
                    float& v = z[2 * k];
                    // Equal-power blend. The shifted path is band-limited so nothing near DC (where a
                    // shifted copy still correlates with the original) can build up round the loop.
                    auto& sh = shifters[k];
                    const float shifted = sh.lowPass.process (sh.highPass.process (sh.process (v)).high).low;
                    v = std::sqrt (1.0f - s) * v + 1.1f * std::sqrt (s) * shifted;
                }
            });
            out.mix (left[i], right[i], wl, wr);
        }
    }

private:
    ReverbInput input;
    Fdn8 fdn;
    std::array<GrainShifter, 4> shifters;
    Glide amount;
    WetDry out;
};

//==============================================================================
// NIMBUS: ambient. Huge, slowly drifting FDN with allpass diffusers inside every line;
// Freeze holds the cloud indefinitely (lossless loop, input closed).
class AmbientReverb final : public Pedal
{
public:
    bool producesStereo() const override                    { return true; }
    void processChannel (int, float*, int) override {}

    void prepared() override
    {
        input.prepare (sampleRate, 50.0f, { 331, 251, 797, 617 }, 0.75f, 0.68f);
        fdn.prepare ({ 2341, 2917, 3449, 3989, 4561, 5113, 5669, 6311 }, sampleRate, 1.4f, 40.0f, 0.18f, 1.2f);
        const int apLengths[] { 241, 317, 389, 457, 523, 601, 673, 743 };
        for (size_t i = 0; i < inner.size(); ++i) inner[i].setup (scaled (apLengths[i], sampleRate), 0.55f);
        freeze.setTime (0.12f, sampleRate);
        out.prepare (sampleRate);
    }

    void reset() override
    {
        input.reset(); fdn.reset(); out.reset(); freeze.reset();
        for (auto& a : inner) a.clear();
    }

    void update() override
    {
        freeze.set (knob (4) > 0.5f ? 1.0f : 0.0f);
        const float f = freeze.value;      // block rate is plenty for the loop gain
        const float rate = 1.0f / (knob (0) * 0.001f);
        fdn.scale.set (0.6f + 0.8f * knob (1));
        fdn.setDecay (rate * (1.0f - f), rate / (1.0f - 0.92f * knob (2)) * (1.0f - f));
        fdn.depth.set (knob (3) * 40.0f * (float) (sampleRate / 48000.0));
        input.set (12.0f, 90.0f, sampleRate);     // low cut keeps sub-bass and DC out of the endless loop
        out.set (knob (5), 0.3f);
    }

    void processStereo (float* left, float* right, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float open = 1.0f - freeze.next();
            float wl, wr;
            fdn.process (open * input.process (0.5f * (left[i] + right[i])), wl, wr, [this] (auto& z)
            {
                for (size_t k = 0; k < inner.size(); ++k) z[k] = inner[k].process (z[k]);
            });
            out.mix (left[i], right[i], wl, wr);
        }
    }

private:
    ReverbInput input;
    Fdn8 fdn;
    std::array<Allpass, 8> inner;
    Glide freeze;
    WetDry out;
};

//==============================================================================
// NEON: gated reverb. A dense, bright burst that slams shut when the hold time runs out.
class GatedReverb final : public Pedal
{
public:
    bool producesStereo() const override                    { return true; }
    void processChannel (int, float*, int) override {}

    void prepared() override
    {
        input.prepare (sampleRate, 20.0f, { 142, 107, 379, 277 }, 0.75f, 0.65f);
        fdn.prepare ({ 701, 853, 1009, 1163, 1301, 1447, 1597, 1741 }, sampleRate, 1.0f, 3.0f, 0.7f);
        attack = 1.0f - std::exp (-1.0f / (0.001f * (float) sampleRate));
        envRelease = 1.0f - std::exp (-1.0f / (0.02f * (float) sampleRate));
        gateOpen = 1.0f - std::exp (-1.0f / (0.0015f * (float) sampleRate));
        gateClose = 1.0f - std::exp (-1.0f / (0.012f * (float) sampleRate));
        out.prepare (sampleRate);
    }

    void reset() override
    {
        input.reset(); fdn.reset(); out.reset();
        env = gate = 0.0f; holdLeft = 0; sinceOpen = 0;
    }

    void update() override
    {
        holdSamples = (int) msToSamples (knob (0), sampleRate);
        const float rt = knob (1) * 0.001f;
        fdn.scale.set (1.0f);
        fdn.setDecay (1.0f / rt, 1.0f / (rt * (1.0f - 0.85f * knob (4))));
        fdn.depth.set (2.0f);
        threshold = dsp::dbToGain (knob (2));
        swell = knob (3) > 0.5f;
        input.set (5.0f, 120.0f, sampleRate);
        out.set (knob (5), 0.32f);
    }

    void processStereo (float* left, float* right, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float in = 0.5f * (left[i] + right[i]);
            const float mag = std::abs (in);
            env += (mag > env ? attack : envRelease) * (mag - env);

            if (env > threshold)
            {
                if (holdLeft == 0) sinceOpen = 0;
                holdLeft = holdSamples;
            }

            float target = 0.0f;
            if (holdLeft > 0)
            {
                --holdLeft;
                ++sinceOpen;
                target = swell ? 0.12f + 0.88f * juce::jmin (1.0f, (float) sinceOpen / (float) juce::jmax (1, holdSamples))
                               : 1.0f;
            }
            gate += (target > gate ? gateOpen : gateClose) * (target - gate);

            float wl, wr;
            fdn.process (input.process (in), wl, wr, [] (auto&) {});
            out.mix (left[i], right[i], gate * wl, gate * wr);
        }
    }

private:
    ReverbInput input;
    Fdn8 fdn;
    WetDry out;
    float env = 0.0f, gate = 0.0f, threshold = 0.01f, attack = 0.1f, envRelease = 0.001f, gateOpen = 0.1f, gateClose = 0.01f;
    int holdSamples = 1000, holdLeft = 0, sinceOpen = 0;
    bool swell = false;
};

} // namespace

void registerReverbPedals (Registry& r)
{
    r.add<RoomReverb> ({ "reverb.room", "ROOM", "reverb", "Reverb",
                         "A natural-sounding room around your guitar",
                         juce::Colour (0xffd8cdb4), juce::Colour (0xff2a2520),
                         { knob ("size", "Size", 0.0f, 1.0f, 0.55f, "%"),
                           knob ("damp", "Damp", 0.0f, 1.0f, 0.45f, "%"),
                           knob ("mix", "Mix", 0.0f, 1.0f, 0.22f, "%") } });

    r.add<HallReverb> ({ "reverb.hall", "VAULT", "hall reverb", "Reverb",
                         "A big, lush concert hall with a smooth, slow-moving tail",
                         juce::Colour (0xff3b3f6b), juce::Colour (0xfff0ead6),
                         { knob ("decay", "Decay", 600.0f, 12000.0f, 2600.0f, "ms", 0.45f),
                           knob ("size", "Size", 0.0f, 1.0f, 0.6f, "%"),
                           knob ("predelay", "Pre-delay", 0.0f, 200.0f, 20.0f, "ms", 0.6f),
                           knob ("damp", "Damp", 0.0f, 1.0f, 0.45f, "%"),
                           knob ("mod", "Mod", 0.0f, 1.0f, 0.35f, "%"),
                           knob ("lowcut", "Low Cut", 20.0f, 500.0f, 100.0f, "Hz", 0.5f),
                           knob ("mix", "Mix", 0.0f, 1.0f, 0.28f, "%") } });

    r.add<PlateReverb> ({ "reverb.plate", "GILT", "plate reverb", "Reverb",
                          "Dense, bright and shimmering, like a big steel sheet",
                          juce::Colour (0xffd9b44a), juce::Colour (0xff2a210c),
                          { knob ("decay", "Decay", 400.0f, 10000.0f, 2400.0f, "ms", 0.45f),
                            knob ("predelay", "Pre-delay", 0.0f, 150.0f, 12.0f, "ms", 0.6f),
                            knob ("damp", "Damp", 1500.0f, 16000.0f, 8500.0f, "Hz", 0.5f),
                            knob ("mod", "Mod", 0.0f, 1.0f, 0.3f, "%"),
                            knob ("lowcut", "Low Cut", 20.0f, 600.0f, 140.0f, "Hz", 0.5f),
                            knob ("mix", "Mix", 0.0f, 1.0f, 0.25f, "%") } });

    r.add<SpringReverb> ({ "reverb.spring", "COIL", "spring reverb", "Reverb",
                           "Drippy, twangy amp-style springs that boing when you dig in",
                           juce::Colour (0xff202020), juce::Colour (0xfff2c14e),
                           { knob ("decay", "Decay", 400.0f, 5000.0f, 1800.0f, "ms", 0.6f),
                             knob ("tone", "Tone", 1500.0f, 6000.0f, 3800.0f, "Hz", 0.6f),
                             knob ("drip", "Drip", 0.0f, 1.0f, 0.55f, "%"),
                             knob ("boing", "Boing", 0.0f, 1.0f, 0.35f, "%"),
                             knob ("mix", "Mix", 0.0f, 1.0f, 0.3f, "%") } });

    r.add<ShimmerReverb> ({ "reverb.shimmer", "HALO", "shimmer reverb", "Reverb",
                            "A long reverb whose tail rises in octaves like a choir",
                            juce::Colour (0xfff2eef7), juce::Colour (0xff5b4a8a),
                            { knob ("decay", "Decay", 1000.0f, 20000.0f, 8000.0f, "ms", 0.45f),
                              knob ("shimmer", "Shimmer", 0.0f, 1.0f, 0.45f, "%"),
                              choice ("interval", "Interval", { "Octave", "Fifth", "Two oct", "Oct down" }, 0),
                              knob ("tone", "Damp", 0.0f, 1.0f, 0.35f, "%"),
                              knob ("mod", "Mod", 0.0f, 1.0f, 0.4f, "%"),
                              knob ("predelay", "Pre-delay", 0.0f, 200.0f, 40.0f, "ms", 0.6f),
                              knob ("mix", "Mix", 0.0f, 1.0f, 0.35f, "%") } });

    r.add<AmbientReverb> ({ "reverb.ambient", "NIMBUS", "ambient reverb", "Reverb",
                            "An endless, drifting cloud you can freeze and play over",
                            juce::Colour (0xff9db6a4), juce::Colour (0xff1f2e25),
                            { knob ("decay", "Decay", 2000.0f, 60000.0f, 12000.0f, "ms", 0.4f),
                              knob ("size", "Size", 0.0f, 1.0f, 0.8f, "%"),
                              knob ("tone", "Damp", 0.0f, 1.0f, 0.55f, "%"),
                              knob ("mod", "Mod", 0.0f, 1.0f, 0.5f, "%"),
                              choice ("freeze", "Freeze", { "Off", "On" }, 0),
                              knob ("mix", "Mix", 0.0f, 1.0f, 0.35f, "%") } });

    r.add<GatedReverb> ({ "reverb.gated", "NEON", "gated reverb", "Reverb",
                          "A huge burst of room that slams shut, straight out of the eighties",
                          juce::Colour (0xffe0338a), juce::Colour (0xff1a0612),
                          { knob ("hold", "Hold", 50.0f, 800.0f, 300.0f, "ms", 0.6f),
                            knob ("decay", "Decay", 500.0f, 4000.0f, 2200.0f, "ms", 0.6f),
                            knob ("threshold", "Threshold", -60.0f, -15.0f, -40.0f, "dB"),
                            choice ("shape", "Shape", { "Gate", "Swell" }, 0),
                            knob ("tone", "Damp", 0.0f, 1.0f, 0.3f, "%"),
                            knob ("mix", "Mix", 0.0f, 1.0f, 0.4f, "%") } });
}

} // namespace woodshed
