#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// Shared building blocks
//==============================================================================

constexpr double pi = juce::MathConstants<double>::pi;

/** Rational tanh approximation: unity slope at 0, exactly +-1 (with zero slope) beyond +-3. */
inline float softClip (float x)
{
    x = juce::jlimit (-3.0f, 3.0f, x);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/** Antiderivative of softClip(), continuous at +-3. */
inline float softClipIntegral (float x)
{
    const float ax = std::abs (x);
    if (ax >= 3.0f)
        return ax + 0.81320887f;    // F(3) - 3 = 0.5 + (4/3) ln 12 - 3
    return x * x * (1.0f / 18.0f) + (4.0f / 3.0f) * std::log (3.0f + x * x);
}

/** softClip() with first-order antiderivative anti-aliasing; keeps the previous input and its integral. */
struct AntiAliasedClip
{
    float prevX = 0.0f, prevF = softClipIntegral (0.0f);

    void reset()                    { prevX = 0.0f; prevF = softClipIntegral (0.0f); }

    float process (float x)
    {
        const float f = softClipIntegral (x);
        const float dx = x - prevX;
        const float y = std::abs (dx) > 1.0e-3f ? (f - prevF) / dx : softClip (0.5f * (x + prevX));
        prevX = x;
        prevF = f;
        return y;
    }
};

/** Topology-preserving one-pole; lp() and hp() share the same state, use one or the other. */
struct OnePole
{
    float g = 0.0f, s = 0.0f;

    void setup (float hz, double sr)
    {
        const double w = std::tan (pi * juce::jlimit (1.0, sr * 0.45, (double) hz) / sr);
        g = (float) (w / (1.0 + w));
    }

    void reset()                    { s = 0.0f; }

    float lp (float x)
    {
        const float v = g * (x - s);
        const float y = v + s;
        s = y + v;
        return y;
    }

    float hp (float x)              { return x - lp (x); }
};

/** Transposed direct-form II biquad with RBJ cookbook designs (no allocation, safe to redesign per block). */
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset()                    { z1 = z2 = 0.0f; }

    float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2)
    {
        b0 = (float) (nb0 / na0); b1 = (float) (nb1 / na0); b2 = (float) (nb2 / na0);
        a1 = (float) (na1 / na0); a2 = (float) (na2 / na0);
    }

    void lowPass (double sr, double hz, double q)
    {
        const double w = 2.0 * pi * juce::jlimit (10.0, sr * 0.45, hz) / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        set ((1.0 - c) * 0.5, 1.0 - c, (1.0 - c) * 0.5, 1.0 + al, -2.0 * c, 1.0 - al);
    }

    void highShelf (double sr, double hz, double q, double db)
    {
        const double A = std::pow (10.0, db / 40.0);
        const double w = 2.0 * pi * juce::jlimit (10.0, sr * 0.45, hz) / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        const double sa = 2.0 * std::sqrt (A) * al;
        set (A * ((A + 1.0) + (A - 1.0) * c + sa), -2.0 * A * ((A - 1.0) + (A + 1.0) * c), A * ((A + 1.0) + (A - 1.0) * c - sa),
             (A + 1.0) - (A - 1.0) * c + sa, 2.0 * ((A - 1.0) - (A + 1.0) * c), (A + 1.0) - (A - 1.0) * c - sa);
    }
};

/** A per-block parameter ramp, identical for both channels: linear from start to end across each block. */
struct BlockRamp
{
    float start = 0.0f, end = 0.0f;

    void snap (float v)                         { start = end = v; }
    void advance (float target, float coeff)    { start = end; end += coeff * (target - end); }
    float at (int i, int n) const               { return start + (end - start) * ((float) (i + 1) / (float) n); }
};

//==============================================================================
// Passive three-knob (treble / bass / mid) tone stack.
//
// Classic topology: input -> C1 -> treble pot (wiper = output) -> bass pot ->
// top of mid pot; input -> slope resistor R4 -> C2 to the treble/bass junction
// and C3 to the mid pot wiper. The analogue transfer function is third order,
// with coefficients polynomial in the pot positions t, m, l; it is discretised
// with the bilinear transform and run in double precision.
//==============================================================================

struct ToneStackParts
{
    double R1, R2, R3, R4;          // treble pot, bass pot, mid pot, slope resistor (ohms)
    double C1, C2, C3;              // treble, bass, mid caps (farads)
};

struct ToneStack
{
    double b[4] { 1.0, 0.0, 0.0, 0.0 }, a[4] { 1.0, 0.0, 0.0, 0.0 };

    void design (const ToneStackParts& p, double t, double m, double l, double fs)
    {
        const double R1 = p.R1, R2 = p.R2, R3 = p.R3, R4 = p.R4, C1 = p.C1, C2 = p.C2, C3 = p.C3;

        const double sb1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);

        const double sb2 = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4)
                         - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                         + m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                         + l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4)
                         + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3)
                         + (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);

        const double c123 = C1 * C2 * C3;
        const double sb3 = l * m * c123 * (R1 * R2 * R3 + R2 * R3 * R4)
                         - m * m * c123 * (R1 * R3 * R3 + R3 * R3 * R4)
                         + m * c123 * (R1 * R3 * R3 + R3 * R3 * R4)
                         + t * c123 * R1 * R3 * R4 - t * m * c123 * R1 * R3 * R4
                         + t * l * c123 * R1 * R2 * R4;

        const double sa0 = 1.0;
        const double sa1 = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);

        const double sa2 = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                         + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3)
                         - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                         + l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4)
                         + (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);

        const double sa3 = l * m * c123 * (R1 * R2 * R3 + R2 * R3 * R4)
                         - m * m * c123 * (R1 * R3 * R3 + R3 * R3 * R4)
                         + m * c123 * (R3 * R3 * R4 + R1 * R3 * R3 - R1 * R3 * R4)
                         + l * c123 * R1 * R2 * R4 + c123 * R1 * R3 * R4;

        // Bilinear transform: s = c (1 - z^-1) / (1 + z^-1). s^k maps to c^k (1 - z^-1)^k (1 + z^-1)^(3 - k).
        static constexpr double basis[4][4] { { 1, 3, 3, 1 }, { 1, 1, -1, -1 }, { 1, -1, -1, 1 }, { 1, -3, 3, -1 } };
        const double c = 2.0 * fs;
        const double bs[4] { 0.0, sb1 * c, sb2 * c * c, sb3 * c * c * c };
        const double as[4] { sa0, sa1 * c, sa2 * c * c, sa3 * c * c * c };

        double nb[4] {}, na[4] {};
        for (int k = 0; k < 4; ++k)
            for (int n = 0; n < 4; ++n)
            {
                nb[n] += bs[k] * basis[k][n];
                na[n] += as[k] * basis[k][n];
            }

        for (int n = 0; n < 4; ++n)
        {
            b[n] = nb[n] / na[0];
            a[n] = na[n] / na[0];
        }
    }
};

/** Per-channel state for a ToneStack (TDF-II, double precision). */
struct ToneStackState
{
    double z[3] {};

    void reset()                    { z[0] = z[1] = z[2] = 0.0; }

    float process (const ToneStack& f, float xf)
    {
        const double x = xf;
        const double y = f.b[0] * x + z[0];
        z[0] = f.b[1] * x - f.a[1] * y + z[1];
        z[1] = f.b[2] * x - f.a[2] * y + z[2];
        z[2] = f.b[3] * x - f.a[3] * y;
        return (float) y;
    }
};

//==============================================================================
// FOUNDRY: tube amp head with five voicings.
//
// input high-pass -> Gain -> [4x oversampled: 2-3 triode-style stages with
// biased (asymmetric) soft clipping, Miller low-pass and coupling high-pass,
// phase inverting stage to stage -> passive tone stack -> Master -> push-pull
// power stage with symmetric saturation and supply sag] -> presence shelf ->
// output transformer low-pass -> DC block -> trim.
//==============================================================================

struct Voicing
{
    float inputHpHz;
    float gainMinDb, gainMaxDb;
    int numStages;
    std::array<float, 3> stageGain, bias, millerHz, couplingHz;
    ToneStackParts stack;
    float stackMakeup;              // recovers the stack's insertion loss
    float powerDrive, sag;
    float outputLowPassHz, presenceHz;
    float trim;                     // calibrated so a DI at default knobs leaves at unity loudness
    float lowBlend;                 // clean low end mixed back in after the amp (bass voicings)
};

const std::array<Voicing, 5> guitarVoicings {{
    // American Clean: lots of headroom, two stages, scooped wide-band stack. Breaks up when pushed.
    { 60.0f, -6.0f, 24.0f, 2,
      { 1.0f, 1.8f, 1.0f }, { 0.12f, 0.08f, 0.0f }, { 14000.0f, 9000.0f, 9000.0f }, { 25.0f, 35.0f, 35.0f },
      { 250e3, 1e6, 10e3, 100e3, 250e-12, 100e-9, 47e-9 }, 4.0f,
      0.7f, 0.25f, 9000.0f, 3200.0f, 0.575f, 0.0f },

    // Chime: strongly biased stages (rich even harmonics), bright stack, a power stage that saturates early and sags.
    { 80.0f, -3.0f, 30.0f, 2,
      { 1.0f, 3.0f, 1.0f }, { 0.30f, 0.20f, 0.0f }, { 12000.0f, 8000.0f, 8000.0f }, { 40.0f, 60.0f, 60.0f },
      { 1e6, 1e6, 10e3, 100e3, 56e-12, 22e-9, 22e-9 }, 3.0f,
      1.5f, 0.45f, 8000.0f, 3600.0f, 0.129f, 0.0f },

    // British Crunch: two gain stages into a soft follower stage, mid-forward stack with a small slope resistor.
    { 90.0f, 0.0f, 36.0f, 3,
      { 1.0f, 6.0f, 1.4f }, { 0.15f, 0.25f, 0.40f }, { 11000.0f, 7000.0f, 10000.0f }, { 60.0f, 110.0f, 25.0f },
      { 220e3, 1e6, 22e3, 33e3, 470e-12, 22e-9, 22e-9 }, 3.0f,
      1.1f, 0.30f, 7500.0f, 3000.0f, 0.065f, 0.0f },

    // Hot Lead: three cascaded high-gain stages, darker Miller poles, thick mids.
    { 110.0f, -12.0f, 36.0f, 3,
      { 1.0f, 9.0f, 7.0f }, { 0.15f, 0.20f, 0.30f }, { 10000.0f, 6000.0f, 5000.0f }, { 90.0f, 130.0f, 80.0f },
      { 250e3, 1e6, 25e3, 47e3, 470e-12, 22e-9, 47e-9 }, 3.0f,
      1.0f, 0.20f, 7000.0f, 2800.0f, 0.059f, 0.0f },

    // Modern High Gain: very tight input, three hot stages, scooped stack, stiff supply.
    { 150.0f, -8.0f, 40.0f, 3,
      { 1.0f, 12.0f, 9.0f }, { 0.10f, 0.15f, 0.20f }, { 9000.0f, 7000.0f, 5500.0f }, { 160.0f, 200.0f, 120.0f },
      { 250e3, 1e6, 25e3, 47e3, 500e-12, 47e-9, 22e-9 }, 3.0f,
      0.9f, 0.10f, 7000.0f, 3200.0f, 0.048f, 0.0f },
}};

// Bass voicings: low coupling corners, deeper stack caps, a clean low-end blend on the dirty voice.
const std::array<Voicing, 3> bassVoicings {{
    // Studio Clean: flat, deep and stiff.
    { 25.0f, -6.0f, 24.0f, 2,
      { 1.0f, 1.5f, 1.0f }, { 0.10f, 0.05f, 0.0f }, { 12000.0f, 9000.0f, 9000.0f }, { 12.0f, 15.0f, 15.0f },
      { 250e3, 1e6, 25e3, 100e3, 470e-12, 100e-9, 47e-9 }, 3.0f,
      0.6f, 0.15f, 7000.0f, 1800.0f, 0.767f, 0.0f },

    // Tube Warm: biased stages and a softer, saggy power stage for round, growly notes.
    { 30.0f, -3.0f, 30.0f, 2,
      { 1.0f, 3.0f, 1.0f }, { 0.25f, 0.15f, 0.0f }, { 10000.0f, 6000.0f, 6000.0f }, { 15.0f, 20.0f, 20.0f },
      { 250e3, 1e6, 25e3, 47e3, 250e-12, 100e-9, 47e-9 }, 3.0f,
      1.2f, 0.35f, 6000.0f, 2000.0f, 0.111f, 0.0f },

    // Grind: three stages of snarl on top, the clean lows kept underneath.
    { 35.0f, 0.0f, 38.0f, 3,
      { 1.0f, 6.0f, 3.0f }, { 0.15f, 0.25f, 0.30f }, { 9000.0f, 6000.0f, 5000.0f }, { 20.0f, 40.0f, 25.0f },
      { 250e3, 1e6, 25e3, 33e3, 470e-12, 47e-9, 22e-9 }, 3.0f,
      1.0f, 0.20f, 6000.0f, 2200.0f, 0.045f, 0.6f },
}};

class AmpHead : public Pedal
{
public:
    AmpHead() : AmpHead (guitarVoicings.data(), (int) guitarVoicings.size()) {}

    static constexpr int osFactorLog2 = 2;   // 4x

    void prepared() override
    {
        osRate = sampleRate * (double) (1 << osFactorLog2);

        for (auto& c : channels)
        {
            c.oversampling.initProcessing ((size_t) maxBlock);
            c.dry.assign ((size_t) maxBlock, 0.0f);
        }

        attackCoeff  = dsp::onePoleCoeff (1.0f / (2.0f * juce::MathConstants<float>::pi * 0.008f), osRate);
        releaseCoeff = dsp::onePoleCoeff (1.0f / (2.0f * juce::MathConstants<float>::pi * 0.120f), osRate);

        activeVoice = requestedVoice = juce::jlimit (0, numVoicings - 1, juce::roundToInt (knob (0)));
        applyVoice();
        firstBlock = true;
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.oversampling.reset();
            c.inputHp.reset();
            for (auto& s : c.miller)   s.reset();
            for (auto& s : c.coupling) s.reset();
            c.stack.reset();
            c.presence.reset();
            c.outputLp.reset();
            c.dcBlock.reset();
            c.env = 0.0f;
            for (auto& s : c.clip) s.reset();
            c.powerClip.reset();
            c.phaseInverterLp.reset();
            c.blendLp1.reset();
            c.blendLp2.reset();
        }

        firstBlock = true;
    }

    int getLatencySamples() const override      { return (int) std::ceil (channels[0].oversampling.getLatencyInSamples()); }

    void update() override
    {
        const int wanted = juce::jlimit (0, numVoicings - 1, juce::roundToInt (knob (0)));

        if (firstBlock && wanted != activeVoice)
        {
            activeVoice = requestedVoice = wanted;
            applyVoice();
        }

        // Voice changes duck the output for a few ms, swap, then fade back in.
        requestedVoice = wanted;
        float duckTarget = 1.0f;

        if (requestedVoice != activeVoice)
        {
            duckTarget = 0.0f;

            if (duck.end < 0.001f)
            {
                activeVoice = requestedVoice;
                applyVoice();
                for (auto& c : channels) c.env = 0.0f;
                duckTarget = 1.0f;
            }
        }

        const auto& v = table[activeVoice];
        const float gainTarget = dsp::dbToGain (juce::jmap (knob (1), v.gainMinDb, v.gainMaxDb));

        // Pots: bass and mid follow an audio taper, treble is linear.
        const double taper = 3.0;
        auto audioTaper = [taper] (double k) { return (std::exp (taper * k) - 1.0) / (std::exp (taper) - 1.0); };
        const double tTarget = juce::jlimit (0.005, 0.995, (double) knob (4));
        const double lTarget = juce::jlimit (0.005, 0.995, audioTaper (knob (2)));
        const double mTarget = juce::jlimit (0.005, 0.995, (double) knob (3));

        const float masterDb = knob (6) < 0.5f ? juce::jmap (knob (6), 0.0f, 0.5f, -30.0f, 0.0f)
                                               : juce::jmap (knob (6), 0.5f, 1.0f, 0.0f, 12.0f);
        const float presenceTarget = juce::jmap (knob (5), -4.0f, 10.0f);

        // Above noon, Master mostly pushes the power stage; the output rises only half as fast.
        const float outputTarget = v.trim * dsp::dbToGain (-0.5f * juce::jmax (0.0f, masterDb));

        if (firstBlock)
        {
            outputGainRamp.snap (outputTarget);
            gain.snap (gainTarget);
            master.snap (dsp::dbToGain (masterDb));
            duck.snap (1.0f);
            pots = { tTarget, mTarget, lTarget };
            presenceDb = presenceTarget;
            redesignStack();
            redesignPresence();
            firstBlock = false;
        }
        else
        {
            outputGainRamp.advance (outputTarget, 0.25f);
            gain.advance (gainTarget, 0.25f);
            master.advance (dsp::dbToGain (masterDb), 0.25f);
            duck.advance (duckTarget, 0.5f);

            if (duckTarget > 0.5f && duck.end > 0.995f) duck.end = 1.0f;
            if (duckTarget < 0.5f && duck.end < 0.001f) duck.end = 0.0f;

            const std::array<double, 3> targets { tTarget, mTarget, lTarget };
            bool moved = stackDirty;
            for (size_t k = 0; k < 3; ++k)
            {
                const double d = targets[k] - pots[k];
                if (std::abs (d) > 1.0e-5) { pots[k] += 0.3 * d; moved = true; }
                else                         pots[k] = targets[k];
            }
            if (moved) redesignStack();

            if (std::abs (presenceTarget - presenceDb) > 0.01f || presenceDirty)
            {
                presenceDb += 0.3f * (presenceTarget - presenceDb);
                redesignPresence();
            }
        }


    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];
        const auto& v = table[activeVoice];

        const bool blend = v.lowBlend > 0.0f;
        for (int i = 0; i < n; ++i)
        {
            if (blend) c.dry[(size_t) i] = c.blendLp2.lp (c.blendLp1.lp (data[i]));
            data[i] = c.inputHp.hp (data[i]) * gain.at (i, n);
        }

        float* chans[] { data };
        juce::dsp::AudioBlock<float> block (chans, 1, (size_t) n);
        auto up = c.oversampling.processSamplesUp (block);
        auto* x = up.getChannelPointer (0);
        const int nUp = (int) up.getNumSamples();

        std::array<float, 3> biasOffset {};
        for (size_t s = 0; s < 3; ++s) biasOffset[s] = softClip (v.bias[s]);

        const float polarity = (v.numStages % 2) != 0 ? -1.0f : 1.0f;   // keep the head non-inverting overall
        const float makeup = v.stackMakeup;
        const float drive = v.powerDrive;
        const float sag = v.sag;

        for (int i = 0; i < nUp; ++i)
        {
            float y = x[i];

            for (int s = 0; s < v.numStages; ++s)
            {
                const auto si = (size_t) s;
                y = c.miller[si].lp (y * v.stageGain[si]);
                y = c.clip[si].process (y + v.bias[si]) - biasOffset[si];
                y = -c.coupling[si].hp (y);         // each triode stage inverts
            }

            y = c.stack.process (stack, y) * makeup * master.at (i, nUp);
            y = c.phaseInverterLp.lp (y);          // phase inverter / driver bandwidth

            // Push-pull power stage: symmetric, and the rail droops with recent output (sag).
            const float rail = 1.0f - sag * juce::jmin (1.0f, c.env);
            y = rail * c.powerClip.process (y * drive / rail);

            const float a = std::abs (y);
            c.env += (a > c.env ? attackCoeff : releaseCoeff) * (a - c.env);
            x[i] = y;
        }

        c.oversampling.processSamplesDown (block);

        for (int i = 0; i < n; ++i)
        {
            float y = c.presence.process (data[i]);
            y = c.outputLp.process (y);
            y = c.dcBlock.hp (y) * polarity * outputGainRamp.at (i, n);
            if (blend) y += v.lowBlend * c.dry[(size_t) i];
            data[i] = y * duck.at (i, n);
        }
    }

protected:
    AmpHead (const Voicing* voicingTable, int count) : table (voicingTable), numVoicings (count) {}

private:
    const Voicing* table;
    int numVoicings;

    void applyVoice()
    {
        const auto& v = table[activeVoice];

        for (auto& c : channels)
        {
            c.inputHp.setup (v.inputHpHz, sampleRate);
            for (size_t s = 0; s < 3; ++s)
            {
                c.miller[s].setup (v.millerHz[s], osRate);
                c.coupling[s].setup (v.couplingHz[s], osRate);
            }
            c.outputLp.lowPass (sampleRate, v.outputLowPassHz, 0.6);
            c.dcBlock.setup (10.0f, sampleRate);
            c.phaseInverterLp.setup (10000.0f, osRate);
            c.blendLp1.setup (220.0f, sampleRate);
            c.blendLp2.setup (220.0f, sampleRate);
        }

        stackDirty = presenceDirty = true;
    }

    void redesignStack()
    {
        stack.design (table[activeVoice].stack, pots[0], pots[1], pots[2], osRate);
        stackDirty = false;
    }

    void redesignPresence()
    {
        for (auto& c : channels)
            c.presence.highShelf (sampleRate, table[activeVoice].presenceHz, 0.6, presenceDb);
        presenceDirty = false;
    }

    struct Channel
    {
        juce::dsp::Oversampling<float> oversampling { 1, (size_t) osFactorLog2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
        OnePole inputHp, dcBlock;
        std::array<OnePole, 3> miller, coupling;
        ToneStackState stack;
        Biquad presence, outputLp;
        std::array<AntiAliasedClip, 3> clip;
        AntiAliasedClip powerClip;
        OnePole phaseInverterLp, blendLp1, blendLp2;
        std::vector<float> dry;
        float env = 0.0f;
    };

    std::array<Channel, 2> channels;
    ToneStack stack;
    std::array<double, 3> pots { 0.5, 0.5, 0.5 };    // treble, mid, bass positions
    float presenceDb = 0.0f;
    BlockRamp gain, master, duck, outputGainRamp;
    double osRate = 192000.0;
    float attackCoeff = 0.01f, releaseCoeff = 0.001f;
    int activeVoice = 0, requestedVoice = 0;
    bool firstBlock = true, stackDirty = true, presenceDirty = true;
};

/** BASEMENT: the same amp engine with bass voicings. */
class BassAmp final : public AmpHead
{
public:
    BassAmp() : AmpHead (bassVoicings.data(), (int) bassVoicings.size()) {}
};

//==============================================================================
// BOXCAR: speaker cabinet simulator.
//
// Each cabinet's magnitude response is designed analytically (resonance bump,
// mid bumps and notches, presence peak, steep cone roll-off with break-up
// ripple) at three mic positions, turned into a minimum-phase 2048-tap FIR via
// the real cepstrum at prepare time, and run through a zero-latency convolver:
// the first 64 taps direct-form, the rest uniformly partitioned (FFT 128).
// Mic Position blends the designs; cabinet changes morph between IRs.
//==============================================================================

struct CabDesign
{
    float lowHz, lowQ;              // cone/box resonance (2nd-order high-pass with a bump)
    float rollOffHz;                // start of the steep top-end roll-off
    float presenceHz, presenceDb, presenceQ;
    std::array<std::array<float, 3>, 4> bumps;   // { Hz, dB, Q }
};

const std::array<CabDesign, 7> cabinets {{
    // 1x8 Tiny: boxy, thin low end, honky upper mids.
    { 150.0f, 1.1f, 4200.0f, 2200.0f, 5.0f, 1.2f, {{ { 700.0f, 2.5f, 1.4f }, { 1400.0f, -3.0f, 3.0f }, { 380.0f, 1.0f, 1.0f }, { 3300.0f, -2.0f, 3.0f } }} },
    // 1x10 Open: back-wave dip in the low mids, airy.
    { 110.0f, 0.9f, 4800.0f, 2600.0f, 4.0f, 1.3f, {{ { 220.0f, -3.0f, 1.5f }, { 900.0f, 1.5f, 1.2f }, { 1700.0f, -2.5f, 3.0f }, { 3900.0f, 1.5f, 3.0f } }} },
    // 1x12 Open: rounder, sparkly top.
    { 95.0f, 1.0f, 5200.0f, 2800.0f, 4.5f, 1.3f, {{ { 200.0f, -2.5f, 1.4f }, { 1200.0f, 1.5f, 1.5f }, { 1900.0f, -3.0f, 3.0f }, { 3900.0f, 2.0f, 3.0f } }} },
    // 2x12 Closed: tighter thump, focused presence.
    { 95.0f, 1.4f, 5000.0f, 2500.0f, 5.0f, 1.2f, {{ { 400.0f, -1.5f, 1.2f }, { 1100.0f, 1.0f, 1.5f }, { 1800.0f, -3.5f, 3.0f }, { 3600.0f, 2.5f, 3.0f } }} },
    // 4x12 Closed: big low resonance, mid scoop, aggressive upper presence.
    { 85.0f, 1.8f, 4800.0f, 2300.0f, 5.5f, 1.1f, {{ { 500.0f, -2.0f, 1.0f }, { 1500.0f, -3.0f, 2.5f }, { 3300.0f, 3.0f, 2.5f }, { 160.0f, 1.0f, 1.2f } }} },
    // 4x10 Vintage: open, lively lower mids, extended top.
    { 100.0f, 1.2f, 5500.0f, 3000.0f, 4.0f, 1.2f, {{ { 250.0f, -1.0f, 1.0f }, { 800.0f, 2.0f, 1.2f }, { 2000.0f, -2.0f, 3.0f }, { 4200.0f, 2.0f, 3.0f } }} },
    // 2x10 Bass: deep, extended lows and a softer top.
    { 58.0f, 1.3f, 4500.0f, 2000.0f, 3.0f, 1.0f, {{ { 150.0f, 2.0f, 1.0f }, { 600.0f, -2.0f, 1.2f }, { 1500.0f, -2.0f, 2.0f }, { 3200.0f, 1.0f, 3.0f } }} },
}};

constexpr int numMicPositions = 3;

class SpeakerCab final : public Pedal
{
public:
    static constexpr int hop = 64;
    static constexpr int fftOrder = 7;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int numBins = fftSize / 2 + 1;
    static constexpr int spectrumFloats = numBins * 2;
    static constexpr int irLength = 2048;
    static constexpr int numParts = (irLength - hop) / hop;
    static constexpr int tailFloats = numParts * spectrumFloats;

    void prepared() override
    {
        if (std::abs (designedRate - sampleRate) > 0.5)
            buildImpulseResponses();

        current.resize (irFloats);
        target.resize (irFloats);

        for (auto& c : channels)
        {
            c.fdl.assign ((size_t) tailFloats, 0.0f);
            c.room.setMaxSamples ((int) (sampleRate * 0.03));
        }

        roomLpCoeff = dsp::onePoleCoeff (4500.0f, sampleRate);
        snapNext = true;
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            std::fill (c.fdl.begin(), c.fdl.end(), 0.0f);
            c.hist.fill (0.0f);
            c.block.fill (0.0f);
            c.prevBlock.fill (0.0f);
            c.tailOut.fill (0.0f);
            c.histPos = c.pos = c.slot = 0;
            c.room.clear();
            c.roomLp = 0.0f;
            c.lowCut.reset();
            c.highCut1.reset();
            c.highCut2.reset();
        }

        snapNext = true;
    }

    void update() override
    {
        const int cab = juce::jlimit (0, (int) cabinets.size() - 1, juce::roundToInt (knob (0)));
        const float mic = juce::jlimit (0.0f, 1.0f, knob (1));

        if (snapNext || cab != targetCab || std::abs (mic - targetMic) > 1.0e-4f)
        {
            morphBlocks = (cab != targetCab) ? 40 : 12;
            targetCab = cab;
            targetMic = mic;

            const float pos = mic * (float) (numMicPositions - 1);
            const int i0 = juce::jmin ((int) pos, numMicPositions - 2);
            const float w = pos - (float) i0;
            const float* A = designs.data() + (size_t) ((cab * numMicPositions + i0) * irFloats);
            const float* B = A + irFloats;
            for (int k = 0; k < irFloats; ++k)
                target[(size_t) k] = A[k] + w * (B[k] - A[k]);
        }

        if (snapNext)
        {
            current = target;
            morphBlocks = 0;
        }
        else if (morphBlocks > 0)
        {
            if (--morphBlocks == 0)
                current = target;
            else
                for (int k = 0; k < irFloats; ++k)
                    current[(size_t) k] += 0.15f * (target[(size_t) k] - current[(size_t) k]);
        }

        const float lowTarget = knob (2), highTarget = knob (3);
        const float roomTarget = knob (4);
        const float levelTarget = dsp::dbToGain (knob (5));   // IRs are already loudness-normalised

        if (snapNext)
        {
            lowCutHz = lowTarget;
            highCutHz = highTarget;
            room.snap (roomTarget);
            level.snap (levelTarget);
        }
        else
        {
            lowCutHz *= std::pow (lowTarget / lowCutHz, 0.3f);
            highCutHz *= std::pow (highTarget / highCutHz, 0.3f);
            room.advance (roomTarget, 0.25f);
            level.advance (levelTarget, 0.25f);
        }

        for (auto& c : channels)
        {
            c.lowCut.setup (lowCutHz, 0.707f, sampleRate);
            c.highCut1.setup (highCutHz, 0.54f, sampleRate);
            c.highCut2.setup (highCutHz, 1.31f, sampleRate);
        }

        snapNext = false;
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];
        const float* headRev = current.data();
        const float* tail = current.data() + hop;

        static constexpr int roomTaps = 6;
        static constexpr float roomMs[roomTaps]   { 3.1f, 4.7f, 7.3f, 9.9f, 13.2f, 17.8f };
        static constexpr float roomGain[roomTaps] { 0.55f, -0.45f, 0.38f, -0.30f, 0.24f, -0.18f };
        std::array<int, roomTaps> tapDelay {};
        for (int t = 0; t < roomTaps; ++t)
            tapDelay[(size_t) t] = juce::roundToInt (roomMs[t] * 0.001f * (float) sampleRate);

        for (int i = 0; i < n; ++i)
        {
            const float in = data[i];

            // Direct-form head: the first `hop` taps, against the last `hop` inputs.
            c.hist[(size_t) c.histPos] = in;
            c.hist[(size_t) (c.histPos + hop)] = in;
            const float* win = c.hist.data() + c.histPos + 1;
            float acc[8] {};
            for (int k = 0; k < hop; k += 8)
                for (int j = 0; j < 8; ++j)
                    acc[j] += win[k + j] * headRev[k + j];
            c.histPos = (c.histPos + 1) % hop;

            float y = ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
            y += c.tailOut[(size_t) c.pos];

            c.block[(size_t) c.pos] = in;
            if (++c.pos == hop)
            {
                runTail (c, tail);
                c.pos = 0;
            }

            // Short early reflections from the room around the cab.
            c.room.push (y);
            float wet = 0.0f;
            for (int t = 0; t < roomTaps; ++t)
                wet += roomGain[t] * c.room.data[(size_t) ((c.room.writePos - 1 - tapDelay[(size_t) t]) & c.room.mask)];
            c.roomLp += roomLpCoeff * (wet - c.roomLp);
            y += room.at (i, n) * 0.6f * c.roomLp;

            y = c.lowCut.process (y).high;
            y = c.highCut2.process (c.highCut1.process (y).low).low;
            data[i] = y * level.at (i, n);
        }
    }

private:
    static constexpr int irFloats = hop + tailFloats;

    struct Channel
    {
        std::array<float, 2 * hop> hist {};
        std::array<float, hop> block {}, prevBlock {}, tailOut {};
        std::array<float, 2 * fftSize> fftBuf {}, accum {};
        std::vector<float> fdl;                 // frequency-domain delay line, numParts spectra
        int histPos = 0, pos = 0, slot = 0;
        dsp::DelayBuffer room;
        float roomLp = 0.0f;
        dsp::Svf lowCut, highCut1, highCut2;
    };

    /** Called once per completed input block: produces the tail's output for the next block (overlap-save). */
    void runTail (Channel& c, const float* tail)
    {
        std::copy (c.prevBlock.begin(), c.prevBlock.end(), c.fftBuf.begin());
        std::copy (c.block.begin(), c.block.end(), c.fftBuf.begin() + hop);
        std::fill (c.fftBuf.begin() + fftSize, c.fftBuf.end(), 0.0f);
        fft->performRealOnlyForwardTransform (c.fftBuf.data(), true);

        c.slot = (c.slot + 1) % numParts;
        std::copy (c.fftBuf.begin(), c.fftBuf.begin() + spectrumFloats, c.fdl.begin() + c.slot * spectrumFloats);

        std::fill (c.accum.begin(), c.accum.end(), 0.0f);
        float* acc = c.accum.data();

        for (int m = 0; m < numParts; ++m)
        {
            const int s = (c.slot - m + numParts) % numParts;
            const float* X = c.fdl.data() + s * spectrumFloats;
            const float* H = tail + m * spectrumFloats;

            for (int k = 0; k < spectrumFloats; k += 2)
            {
                acc[k]     += X[k] * H[k] - X[k + 1] * H[k + 1];
                acc[k + 1] += X[k] * H[k + 1] + X[k + 1] * H[k];
            }
        }

        fft->performRealOnlyInverseTransform (acc);
        std::copy (acc + hop, acc + fftSize, c.tailOut.begin());
        c.prevBlock = c.block;
    }

    //==========================================================================
    void buildImpulseResponses()
    {
        fft = std::make_unique<juce::dsp::FFT> (fftOrder);
        designs.assign ((size_t) (cabinets.size() * numMicPositions * irFloats), 0.0f);

        constexpr int designOrder = 14;
        constexpr int N = 1 << designOrder;
        juce::dsp::FFT big (designOrder);
        std::vector<std::complex<float>> bufA ((size_t) N), bufB ((size_t) N);
        std::vector<float> ir ((size_t) irLength);
        std::array<float, 2 * fftSize> part {};

        for (size_t cab = 0; cab < cabinets.size(); ++cab)
        {
            for (int mic = 0; mic < numMicPositions; ++mic)
            {
                const float edge = (float) mic / (float) (numMicPositions - 1);   // 0 = cone centre, 1 = edge

                // 1. Target log-magnitude, normalised for unity loudness over the guitar band (pink weighting).
                double weighted = 0.0, weights = 0.0;
                for (int k = 0; k <= N / 2; ++k)
                {
                    const double hz = (double) k * sampleRate / (double) N;
                    const double mag = targetMagnitude (cabinets[cab], edge, hz);
                    bufA[(size_t) k] = (float) std::log (juce::jmax (1.0e-4, mag));

                    if (hz >= 80.0 && hz <= 6000.0)
                    {
                        weighted += mag * mag / hz;
                        weights += 1.0 / hz;
                    }
                }

                const float logNorm = (float) std::log (std::sqrt (weighted / weights));
                for (int k = 0; k <= N / 2; ++k)
                    bufA[(size_t) k] -= logNorm;
                for (int k = N / 2 + 1; k < N; ++k)
                    bufA[(size_t) k] = bufA[(size_t) (N - k)];

                // 2. Real cepstrum, folded to make it causal: minimum phase.
                big.perform (bufA.data(), bufB.data(), true);
                bufA[0] = { bufB[0].real(), 0.0f };
                for (int k = 1; k < N / 2; ++k) bufA[(size_t) k] = { 2.0f * bufB[(size_t) k].real(), 0.0f };
                bufA[(size_t) (N / 2)] = { bufB[(size_t) (N / 2)].real(), 0.0f };
                for (int k = N / 2 + 1; k < N; ++k) bufA[(size_t) k] = {};

                big.perform (bufA.data(), bufB.data(), false);
                for (auto& z : bufB) z = std::exp (z);
                big.perform (bufB.data(), bufA.data(), true);

                // 3. Truncate with a half-Hann fade over the last quarter.
                const int fade = irLength / 4;
                for (int i = 0; i < irLength; ++i)
                {
                    float w = 1.0f;
                    if (i >= irLength - fade)
                        w = 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * (float) (i - (irLength - fade)) / (float) fade));
                    ir[(size_t) i] = bufA[(size_t) i].real() * w;
                }

                // 4. Split: reversed head taps, then the tail partitions' spectra.
                float* dest = designs.data() + (size_t) (((int) cab * numMicPositions + mic) * irFloats);
                for (int k = 0; k < hop; ++k)
                    dest[k] = ir[(size_t) (hop - 1 - k)];

                for (int m = 0; m < numParts; ++m)
                {
                    part.fill (0.0f);
                    std::copy (ir.begin() + hop + m * hop, ir.begin() + hop + (m + 1) * hop, part.begin());
                    fft->performRealOnlyForwardTransform (part.data(), true);
                    std::copy (part.begin(), part.begin() + spectrumFloats, dest + hop + m * spectrumFloats);
                }
            }
        }

        designedRate = sampleRate;
    }

    static double biquadMag (std::complex<double> s, double b0, double b1, double b2, double a0, double a1, double a2)
    {
        return std::abs ((b2 * s * s + b1 * s + b0) / (a2 * s * s + a1 * s + a0));
    }

    /** Analogue-prototype magnitude of one cabinet at `hz`; `edge` moves the mic from cone centre (0) to edge (1). */
    static double targetMagnitude (const CabDesign& d, float edge, double hz)
    {
        if (hz < 1.0)
            return 0.0;

        auto at = [hz] (double f0) { return std::complex<double> (0.0, hz / f0); };
        auto peak = [&] (double f0, double db, double q)
        {
            const double A = std::pow (10.0, db / 40.0);
            return biquadMag (at (f0), 1.0, A / q, 1.0, 1.0, 1.0 / (A * q), 1.0);
        };

        double mag = biquadMag (at (d.lowHz), 0.0, 0.0, 1.0, 1.0, 1.0 / d.lowQ, 1.0);   // resonant high-pass
        mag *= biquadMag (at (d.lowHz * 0.5), 0.0, 1.0, 0.0, 1.0, 1.0, 0.0);            // extra first-order low roll-off

        for (auto& b : d.bumps)
            mag *= peak (b[0], b[1], b[2]);

        // The cone centre is brighter; towards the edge, the presence peak softens and the top rolls off earlier.
        const double presenceDb = d.presenceDb * (1.0 - 0.55 * edge) - 1.5 * edge;
        mag *= peak (d.presenceHz * (1.0 - 0.12 * edge), presenceDb, d.presenceQ);
        mag *= peak (300.0, 1.5 * edge, 0.8);
        mag *= peak (5200.0, 3.0 * (1.0 - edge) - 2.0, 2.5);

        const double roll = d.rollOffHz * (1.2 - 0.45 * edge);
        mag *= biquadMag (at (roll), 1.0, 0.0, 0.0, 1.0, 1.0 / 0.9, 1.0);
        mag *= biquadMag (at (roll * 1.15), 1.0, 0.0, 0.0, 1.0, 1.0 / 0.6, 1.0);
        mag *= biquadMag (at (roll * 1.4), 1.0, 0.0, 0.0, 1.0, 1.0 / 0.55, 1.0);

        // Cone break-up: a few narrow ripples above the roll-off.
        mag *= peak (roll * 1.35, 4.0, 4.0) * peak (roll * 1.7, -5.0, 5.0) * peak (roll * 2.1, 3.0, 4.0);

        return mag;
    }

    //==========================================================================
    std::array<Channel, 2> channels;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> designs, current, target;
    double designedRate = 0.0;

    int targetCab = -1, morphBlocks = 0;
    float targetMic = -1.0f;
    float lowCutHz = 70.0f, highCutHz = 10000.0f;
    BlockRamp room, level;
    float roomLpCoeff = 0.5f;
    bool snapNext = true;
};

} // namespace

void registerAmpCabPedals (Registry& r)
{
    r.add<AmpHead> ({ "amp.head", "FOUNDRY", "tube amp", "Amp",
                      "Five-voice tube amp: sparkling clean to modern high gain, with a real passive tone stack",
                      juce::Colour (0xff23201d), juce::Colour (0xffe6d3a3),
                      { choice ("voice", "Voice", { "American Clean", "Chime", "British Crunch", "Hot Lead", "Modern High Gain" }, 0),
                        knob ("gain", "Gain", 0.0f, 1.0f, 0.5f, "%"),
                        knob ("bass", "Bass", 0.0f, 1.0f, 0.5f, "%"),
                        knob ("mid", "Mid", 0.0f, 1.0f, 0.5f, "%"),
                        knob ("treble", "Treble", 0.0f, 1.0f, 0.6f, "%"),
                        knob ("presence", "Presence", 0.0f, 1.0f, 0.4f, "%"),
                        knob ("master", "Master", 0.0f, 1.0f, 0.5f, "%") } });

    r.add<BassAmp> ({ "amp.bass", "BASEMENT", "bass amp", "Amp",
                      "Bass and baritone amp: deep clean, warm tube growl, or grind over a solid low end",
                      juce::Colour (0xff1f2a33), juce::Colour (0xffd7e0e6),
                      { choice ("voice", "Voice", { "Studio Clean", "Tube Warm", "Grind" }, 0),
                        knob ("gain", "Gain", 0.0f, 1.0f, 0.5f, "%"),
                        knob ("bass", "Bass", 0.0f, 1.0f, 0.55f, "%"),
                        knob ("mid", "Mid", 0.0f, 1.0f, 0.5f, "%"),
                        knob ("treble", "Treble", 0.0f, 1.0f, 0.5f, "%"),
                        knob ("presence", "Presence", 0.0f, 1.0f, 0.4f, "%"),
                        knob ("master", "Master", 0.0f, 1.0f, 0.5f, "%") } });

    r.add<SpeakerCab> ({ "cab.speaker", "BOXCAR", "speaker cabinet", "Cab",
                         "Speaker cabinet and mic: tames the fizz and gives an amp its body",
                         juce::Colour (0xff3b3029), juce::Colour (0xffd9c6a0),
                         { choice ("cabinet", "Cabinet", { "1x8 Tiny", "1x10 Open", "1x12 Open", "2x12 Closed", "4x12 Closed", "4x10 Vintage", "2x10 Bass" }, 3),
                           knob ("mic", "Mic Position", 0.0f, 1.0f, 0.3f, "%"),
                           knob ("lowcut", "Low Cut", 20.0f, 300.0f, 60.0f, "Hz", 0.5f),
                           knob ("highcut", "High Cut", 2000.0f, 16000.0f, 11000.0f, "Hz", 0.5f),
                           knob ("room", "Room", 0.0f, 1.0f, 0.15f, "%"),
                           knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") } });
}

} // namespace woodshed
