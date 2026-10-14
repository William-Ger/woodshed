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

    void lowShelf (double sr, double hz, double q, double db)
    {
        const double A = std::pow (10.0, db / 40.0);
        const double w = 2.0 * pi * juce::jlimit (10.0, sr * 0.45, hz) / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        const double sa = 2.0 * std::sqrt (A) * al;
        set (A * ((A + 1.0) - (A - 1.0) * c + sa), 2.0 * A * ((A - 1.0) - (A + 1.0) * c), A * ((A + 1.0) - (A - 1.0) * c - sa),
             (A + 1.0) + (A - 1.0) * c + sa, -2.0 * ((A - 1.0) + (A + 1.0) * c), (A + 1.0) + (A - 1.0) * c - sa);
    }

    void peaking (double sr, double hz, double q, double db)
    {
        const double A = std::pow (10.0, db / 40.0);
        const double w = 2.0 * pi * juce::jlimit (10.0, sr * 0.45, hz) / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        set (1.0 + al * A, -2.0 * c, 1.0 - al * A, 1.0 + al / A, -2.0 * c, 1.0 - al / A);
    }

    void highPass (double sr, double hz, double q)
    {
        const double w = 2.0 * pi * juce::jlimit (10.0, sr * 0.45, hz) / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        set ((1.0 + c) * 0.5, -(1.0 + c), (1.0 + c) * 0.5, 1.0 + al, -2.0 * c, 1.0 - al);
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

//==============================================================================
//==============================================================================
// Model amps.
//
// Each of these is its own instrument rather than a voicing table: its own gain
// structure, clipping, tone network, power stage and output network. They share
// a small chassis: base-rate input network -> oversampled nonlinear core ->
// base-rate output network, with a short duck whenever a channel or voicing
// switch changes the topology.
//==============================================================================
//==============================================================================

inline double audioTaper (double k, double taper = 3.0)    { return (std::exp (taper * k) - 1.0) / (std::exp (taper) - 1.0); }

/** Algebraic sigmoid x / sqrt(1 + x^2) with first-order ADAA. Its antiderivative is sqrt(1 + x^2), and the
    ADAA difference quotient simplifies exactly to (x + p) / (F(x) + F(p)): no logs, no ill-conditioned branch. */
struct AntiAliasedAlgebraic
{
    float prevX = 0.0f, prevF = 1.0f;

    void reset()                    { prevX = 0.0f; prevF = 1.0f; }

    static float f (float x)        { return x / std::sqrt (1.0f + x * x); }

    float process (float x)
    {
        const float F = std::sqrt (1.0f + x * x);
        const float y = (x + prevX) / (F + prevF);
        prevX = x;
        prevF = F;
        return y;
    }
};

/** A gain stage with the algebraic curve: a gentler knee that never quite flattens, cheap enough to cascade. */
struct HotStage
{
    OnePole miller, coupling;
    AntiAliasedAlgebraic clip;
    float gain = 1.0f, bias = 0.0f, biasOffset = 0.0f;

    void setup (float g, float b, float millerHz, float couplingHz, double rate)
    {
        gain = g;
        bias = b;
        biasOffset = AntiAliasedAlgebraic::f (b);
        miller.setup (millerHz, rate);
        coupling.setup (couplingHz, rate);
    }

    void reset()                    { miller.reset(); coupling.reset(); clip.reset(); }

    float process (float x)
    {
        const float y = clip.process (miller.lp (x * gain) + bias) - biasOffset;
        return -coupling.hp (y);
    }
};

/** One triode-style gain stage: Miller low-pass, biased soft clip (ADAA), coupling high-pass; inverting. */
struct TriodeStage
{
    OnePole miller, coupling;
    AntiAliasedClip clip;
    float gain = 1.0f, bias = 0.0f, biasOffset = 0.0f;

    void setup (float g, float b, float millerHz, float couplingHz, double rate)
    {
        gain = g;
        bias = b;
        biasOffset = softClip (b);
        miller.setup (millerHz, rate);
        coupling.setup (couplingHz, rate);
    }

    void reset()                    { miller.reset(); coupling.reset(); clip.reset(); }

    float process (float x)
    {
        const float y = clip.process (miller.lp (x * gain) + bias) - biasOffset;
        return -coupling.hp (y);
    }

    /** With a signal-dependent bias shift (grid conduction, cathode drift); the shift's own DC is removed per sample. */
    float process (float x, float shift)
    {
        const float b = bias + shift;
        const float y = clip.process (miller.lp (x * gain) + b) - softClip (b);
        return -coupling.hp (y);
    }
};

/** Power stage: biased soft clip whose rail droops with recent output (sag). */
struct PowerStage
{
    AntiAliasedClip clip;
    float env = 0.0f;

    void reset()                    { clip.reset(); env = 0.0f; }

    /** biasOffset must be softClip (bias); callers with a fixed bias precompute it. */
    float process (float x, float drive, float sag, float bias, float biasOffset, float attack, float release)
    {
        const float rail = 1.0f - sag * juce::jmin (1.0f, env);
        const float y = rail * (clip.process (x * drive / rail + bias) - biasOffset);
        const float a = std::abs (y);
        env += (a > env ? attack : release) * (a - env);
        return y;
    }
};

/** A passive three-knob stack whose pots glide toward their targets, redesigning only while they move. */
struct SmoothedStack
{
    ToneStack filter;
    std::array<double, 3> pots { 0.5, 0.5, 0.5 };   // treble, mid, bass
    bool dirty = true;

    void update (const ToneStackParts& parts, double t, double m, double l, bool snap, double rate)
    {
        const std::array<double, 3> targets { juce::jlimit (0.005, 0.995, t), juce::jlimit (0.005, 0.995, m), juce::jlimit (0.005, 0.995, l) };
        bool moved = dirty || snap;

        for (size_t k = 0; k < 3; ++k)
        {
            const double d = targets[k] - pots[k];
            if (snap || std::abs (d) <= 1.0e-5) pots[k] = targets[k];
            else                                { pots[k] += 0.3 * d; moved = true; }
        }

        if (moved)
        {
            filter.design (parts, pots[0], pots[1], pots[2], rate);
            dirty = false;
        }
    }
};

inline void glide (BlockRamp& r, float target, bool snap)   { if (snap) r.snap (target); else r.advance (target, 0.25f); }
inline void glide (float& v, float target, bool snap)       { v = snap ? target : v + 0.3f * (target - v); }

class ModelAmp : public Pedal
{
public:
    explicit ModelAmp (int oversamplingLog2) : osLog2 (oversamplingLog2)
    {
        for (auto& o : oversamplers)
            o = std::make_unique<juce::dsp::Oversampling<float>> (1, (size_t) osLog2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    }

    void prepared() override
    {
        osRate = sampleRate * (double) (1 << osLog2);
        for (auto& o : oversamplers)
            o->initProcessing ((size_t) maxBlock);

        mode = 0;
        modeChanged();
        firstBlock = true;
    }

    void reset() override
    {
        for (auto& o : oversamplers)
            o->reset();
        resetState();
        firstBlock = true;
    }

    int getLatencySamples() const override      { return (int) std::ceil (oversamplers[0]->getLatencyInSamples()); }

    void update() override
    {
        const int wanted = requestedMode();

        if (firstBlock)
        {
            if (wanted != mode)
            {
                mode = wanted;
                modeChanged();
            }
            duck.snap (1.0f);
        }
        else
        {
            // Topology changes duck the output for a few ms, swap while silent, then fade back in.
            float duckTarget = 1.0f;
            if (wanted != mode)
            {
                duckTarget = 0.0f;
                if (duck.end < 0.001f)
                {
                    mode = wanted;
                    modeChanged();
                    resetState();
                    duckTarget = 1.0f;
                }
            }

            duck.advance (duckTarget, 0.5f);
            if (duckTarget > 0.5f && duck.end > 0.995f) duck.end = 1.0f;
            if (duckTarget < 0.5f && duck.end < 0.001f) duck.end = 0.0f;
        }

        updateParameters (firstBlock);
        firstBlock = false;
    }

    void processChannel (int ch, float* data, int n) override
    {
        preProcess (ch, data, n);

        float* chans[] { data };
        juce::dsp::AudioBlock<float> block (chans, 1, (size_t) n);
        auto& os = *oversamplers[(size_t) ch];
        auto up = os.processSamplesUp (block);
        processOversampled (ch, up.getChannelPointer (0), (int) up.getNumSamples());
        os.processSamplesDown (block);

        postProcess (ch, data, n);

        for (int i = 0; i < n; ++i)
            data[i] *= duck.at (i, n);
    }

protected:
    /** The current topology, from the pedal's switches. */
    virtual int requestedMode() const                           { return 0; }
    /** Set up filters for `mode`. Called at prepare time and while the output is ducked. */
    virtual void modeChanged() = 0;
    virtual void resetState() = 0;
    /** Once per block, after the mode is settled. `snap` jumps straight to the knob targets. */
    virtual void updateParameters (bool snap) = 0;
    virtual void preProcess (int ch, float* data, int n) = 0;
    virtual void processOversampled (int ch, float* x, int n) = 0;
    virtual void postProcess (int ch, float* data, int n) = 0;

    int choiceIndex (int k, int count) const                    { return juce::jlimit (0, count - 1, juce::roundToInt (knob (k))); }

    /** One-pole coefficient for a time constant in ms, at the oversampled rate. */
    float osTimeCoeff (float ms) const                          { return dsp::onePoleCoeff (1000.0f / (juce::MathConstants<float>::twoPi * ms), osRate); }

    const int osLog2;
    double osRate = 192000.0;
    int mode = 0;

private:
    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 2> oversamplers;
    BlockRamp duck;
    bool firstBlock = true;
};

//==============================================================================
// ROADHOUSE: small 1950s-style 1x12 combo.
//
// Two triode stages with one volume and one tone control between them, a
// single-ended power stage (lopsided, even-order heavy) on a soft, saggy
// supply, and no negative feedback. The second stage's bias is dragged down by
// grid conduction on hard hits, which is where the hairy, splatty breakup
// comes from. Bright channel: a bright cap across the volume pot that fades as
// the volume comes up. Normal channel: darker, fuller first stage.
//==============================================================================

class RoadhouseAmp final : public ModelAmp
{
public:
    RoadhouseAmp() : ModelAmp (1) {}

protected:
    int requestedMode() const override                          { return choiceIndex (0, 2); }

    void modeChanged() override
    {
        const bool bright = mode == 0;
        for (auto& c : chans)
        {
            c.inputHp.setup (bright ? 45.0f : 32.0f, sampleRate);
            c.v1.setup (3.0f, 0.18f, bright ? 16000.0f : 7500.0f, bright ? 30.0f : 18.0f, osRate);
            c.v2.setup (6.0f, 0.30f, bright ? 9000.0f : 7000.0f, 25.0f, osRate);
            c.otHp.setup (70.0f, sampleRate);
            c.otLp.lowPass (sampleRate, bright ? 6500.0 : 5600.0, 0.85);
            c.speaker.peaking (sampleRate, 115.0, 1.0, 2.5);
            c.dcBlock.setup (8.0f, sampleRate);
        }

        trim = bright ? 0.1357f : 0.1341f;
        gridAttack = osTimeCoeff (1.0f);
        gridRelease = osTimeCoeff (45.0f);
        sagAttack = osTimeCoeff (12.0f);
        sagRelease = osTimeCoeff (180.0f);
    }

    void resetState() override
    {
        for (auto& c : chans)
        {
            c.inputHp.reset(); c.brightCap.reset(); c.v1.reset(); c.v2.reset(); c.toneLp.reset();
            c.power.reset(); c.otHp.reset(); c.otLp.reset(); c.speaker.reset(); c.dcBlock.reset();
            c.grid = 0.0f;
        }
    }

    void updateParameters (bool snap) override
    {
        const float vol = knob (1), tone = knob (2);
        const float volDb = juce::jmap (vol, -26.0f, 18.0f);
        const float defDb = juce::jmap (defaultVolume, -26.0f, 18.0f);

        glide (volume, dsp::dbToGain (volDb), snap);
        glide (volSmooth, vol, snap);

        // Below about 60% the tone pot shunts treble away; above, it starts to lift it.
        const float w = tone < 0.6f ? 0.85f * std::pow (tone / 0.6f, 1.3f) : 0.85f + 0.95f * (tone - 0.6f) / 0.4f;
        glide (toneMix, w, snap);

        // Past its default the volume mostly drives the amp harder; the output rises only gently.
        const float out = trim * dsp::dbToGain (knob (3)) * dsp::dbToGain (-0.3f * juce::jmax (0.0f, volDb - defDb));
        glide (output, out, snap);

        // The bright cap matters most with the volume low; the tone corner rises with the volume.
        const double brightDb = mode == 0 ? 10.0 * std::pow (1.0 - (double) volSmooth.end, 2.0) : 0.0;
        for (auto& c : chans)
        {
            c.brightCap.highShelf (sampleRate, 2600.0, 0.6, brightDb);
            c.toneLp.setup (800.0f + 1400.0f * volSmooth.end, osRate);
        }
    }

    void preProcess (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        for (int i = 0; i < n; ++i)
            data[i] = c.brightCap.process (c.inputHp.hp (data[i]));
    }

    void processOversampled (int ch, float* x, int n) override
    {
        auto& c = chans[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            float y = c.v1.process (x[i]) * volume.at (i, n);

            const float lp = c.toneLp.lp (y);
            y = lp + toneMix.at (i, n) * (y - lp);

            // Grid conduction: positive swings past ~+0.4 charge the coupling cap and drag the bias down.
            const float g = juce::jmax (0.0f, y * c.v2.gain - 0.4f);
            c.grid += (g > c.grid ? gridAttack : gridRelease) * (g - c.grid);
            y = c.v2.process (y, -0.45f * juce::jmin (c.grid, 2.0f));

            // Single-ended power stage: one-sided, on a supply that droops.
            x[i] = c.power.process (y, 1.4f, 0.35f, 0.32f, seOffset, sagAttack, sagRelease);
        }
    }

    void postProcess (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        for (int i = 0; i < n; ++i)
        {
            float y = c.otLp.process (c.otHp.hp (data[i]));
            y = c.speaker.process (y);
            data[i] = c.dcBlock.hp (y) * output.at (i, n);
        }
    }

private:
    static constexpr float defaultVolume = 0.45f;

    struct Channel
    {
        OnePole inputHp, toneLp, otHp, dcBlock;
        Biquad brightCap, otLp, speaker;
        TriodeStage v1, v2;
        PowerStage power;
        float grid = 0.0f;
    };

    std::array<Channel, 2> chans;
    BlockRamp volume, toneMix, output, volSmooth;
    const float seOffset = softClip (0.32f);
    float trim = 1.0f, gridAttack = 0.01f, gridRelease = 0.001f, sagAttack = 0.01f, sagRelease = 0.001f;
};

//==============================================================================
// LIMOUSINE: boutique smooth-overdrive amp.
//
// A clean preamp stage feeds both a clean path and an overdrive path: a vocal
// mid emphasis, low cut, then three cascaded, heavily filtered stages that
// compress into a singing sustain. The OD knob blends the overdrive in over
// the clean. Pre: the tone stack sits before the overdrive (EQ shapes what
// clips). Post: the stack comes after (EQ shapes the finished tone). A stiff,
// feedback-tamed power stage keeps it polished.
//==============================================================================

class LimousineAmp final : public ModelAmp
{
public:
    LimousineAmp() : ModelAmp (1) {}

protected:
    int requestedMode() const override                          { return choiceIndex (0, 2); }

    void modeChanged() override
    {
        const bool pre = mode == 0;
        for (auto& c : chans)
        {
            c.inputHp.setup (45.0f, sampleRate);
            c.v1.setup (2.0f, 0.10f, 14000.0f, 25.0f, osRate);
            c.vocal.peaking (osRate, pre ? 800.0 : 750.0, 0.7, pre ? 4.0 : 6.0);
            c.odHp.setup (pre ? 110.0f : 140.0f, osRate);
            c.s1.setup (1.0f, 0.15f, 9000.0f, 30.0f, osRate);
            c.s2.setup (3.0f, 0.22f, 7000.0f, 45.0f, osRate);
            c.s3.setup (1.6f, 0.30f, 6000.0f, 25.0f, osRate);
            c.piLp.setup (10000.0f, osRate);
            c.presence.highShelf (sampleRate, 3500.0, 0.6, 1.5);
            c.otLp.lowPass (sampleRate, 7500.0, 0.7);
            c.dcBlock.setup (10.0f, sampleRate);
        }

        trim = pre ? 0.239f : 0.253f;
        odMakeup = pre ? 0.309f : 0.282f;      // the overdrive sits ~2 dB over the clean, so the blend is about level
        sagAttack = osTimeCoeff (8.0f);
        sagRelease = osTimeCoeff (90.0f);
        stack.dirty = true;
    }

    void resetState() override
    {
        for (auto& c : chans)
        {
            c.inputHp.reset(); c.v1.reset(); c.vocal.reset(); c.odHp.reset(); c.s1.reset(); c.s2.reset(); c.s3.reset();
            c.stackState.reset(); c.piLp.reset(); c.power.reset(); c.presence.reset(); c.otLp.reset(); c.dcBlock.reset();
        }
    }

    void updateParameters (bool snap) override
    {
        const float volDb = juce::jmap (knob (1), -12.0f, 12.0f);
        const float driveDb = juce::jmap (knob (2), 0.0f, 30.0f);
        glide (volume, dsp::dbToGain (volDb), snap);
        glide (drive, dsp::dbToGain (driveDb), snap);

        const float b = knob (3);
        glide (cleanMix, 2.0f * std::cos (b * juce::MathConstants<float>::halfPi), snap);
        // The overdrive's output rises only a quarter as fast as its drive (it is compressing hard by then).
        const float odLevel = odMakeup * dsp::dbToGain (-0.25f * (driveDb - 15.0f));
        glide (odMix, odLevel * std::sin (b * juce::MathConstants<float>::halfPi), snap);

        stack.update (parts, knob (6), knob (5), audioTaper (knob (4)), snap, osRate);

        const float masterDb = knob (7) < 0.5f ? juce::jmap (knob (7), 0.0f, 0.5f, -30.0f, 0.0f)
                                               : juce::jmap (knob (7), 0.5f, 1.0f, 0.0f, 12.0f);
        glide (master, dsp::dbToGain (masterDb), snap);
        glide (output, trim * dsp::dbToGain (-0.5f * juce::jmax (0.0f, masterDb) - 0.4f * juce::jmax (0.0f, volDb)), snap);
    }

    void preProcess (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        for (int i = 0; i < n; ++i)
            data[i] = c.inputHp.hp (data[i]) * volume.at (i, n);
    }

    void processOversampled (int ch, float* x, int n) override
    {
        auto& c = chans[(size_t) ch];
        const bool pre = mode == 0;

        for (int i = 0; i < n; ++i)
        {
            float y = -c.v1.process (x[i]);
            if (pre) y = c.stackState.process (stack.filter, y) * 3.0f;

            // Overdrive path (three inversions, flipped back so it sums in phase with the clean).
            float od = c.odHp.hp (c.vocal.process (y)) * drive.at (i, n);
            od = -c.s3.process (c.s2.process (c.s1.process (od)));

            y = cleanMix.at (i, n) * y + odMix.at (i, n) * od;
            if (! pre) y = c.stackState.process (stack.filter, y) * 3.0f;

            y = c.piLp.lp (y * master.at (i, n));
            x[i] = c.power.process (y, 0.8f, 0.12f, 0.05f, ppOffset, sagAttack, sagRelease);
        }
    }

    void postProcess (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        for (int i = 0; i < n; ++i)
        {
            const float y = c.otLp.process (c.presence.process (data[i]));
            data[i] = c.dcBlock.hp (y) * output.at (i, n);
        }
    }

private:
    static constexpr ToneStackParts parts { 250e3, 500e3, 25e3, 68e3, 400e-12, 47e-9, 22e-9 };

    struct Channel
    {
        OnePole inputHp, odHp, piLp, dcBlock;
        Biquad vocal, presence, otLp;
        TriodeStage v1, s1, s2, s3;
        ToneStackState stackState;
        PowerStage power;
    };

    std::array<Channel, 2> chans;
    SmoothedStack stack;
    BlockRamp volume, drive, cleanMix, odMix, master, output;
    const float ppOffset = softClip (0.05f);
    float trim = 1.0f, odMakeup = 1.0f, sagAttack = 0.01f, sagRelease = 0.001f;
};

//==============================================================================
// STADIUM: 1960s British-style amp with no master volume.
//
// Two first-stage triodes (bright, with a volume-dependent bright cap; normal,
// darker and fatter) that can be bridged together, one shared volume into a
// second gain stage and a cathode follower, a mid-heavy passive stack, and a
// push-pull power stage driven hard all the time: the louder it is, the more
// it crunches. Sag sets how far the supply collapses (big, slow, loose lows).
//==============================================================================

class StadiumAmp final : public ModelAmp
{
public:
    StadiumAmp() : ModelAmp (1) {}

protected:
    int requestedMode() const override                          { return choiceIndex (0, 3); }

    void modeChanged() override
    {
        for (auto& c : chans)
        {
            c.inputHp.setup (25.0f, sampleRate);
            c.v1Bright.setup (2.2f, 0.12f, 16000.0f, 22.0f, osRate);
            c.v1Normal.setup (2.4f, 0.15f, 3800.0f, 10.0f, osRate);
            c.v2.setup (4.0f, 0.25f, 11000.0f, 30.0f, osRate);
            c.follower.setup (1.0f, 0.55f, 20000.0f, 10.0f, osRate);
            c.piLp.setup (11000.0f, osRate);
            c.lowBump.peaking (sampleRate, 95.0, 0.8, 2.5);
            c.otLp.lowPass (sampleRate, 7500.0, 0.7);
            c.dcBlock.setup (10.0f, sampleRate);
        }

        static constexpr float trims[] { 0.121f, 0.121f, 0.104f };
        static constexpr float pathMix[] { 1.0f, 1.0f, 0.7f };
        trim = trims[mode];
        mix = pathMix[mode];
        sagAttack = osTimeCoeff (15.0f);
        sagRelease = osTimeCoeff (260.0f);
        stack.dirty = true;
    }

    void resetState() override
    {
        for (auto& c : chans)
        {
            c.inputHp.reset(); c.brightCap.reset(); c.v1Bright.reset(); c.v1Normal.reset(); c.v2.reset(); c.follower.reset();
            c.stackState.reset(); c.piLp.reset(); c.power.reset(); c.lowBump.reset(); c.presence.reset(); c.otLp.reset(); c.dcBlock.reset();
        }
    }

    void updateParameters (bool snap) override
    {
        const float vol = knob (1);
        const float volDb = juce::jmap (vol, -24.0f, 20.0f);
        const float defDb = juce::jmap (defaultVolume, -24.0f, 20.0f);
        glide (volume, dsp::dbToGain (volDb), snap);
        glide (volSmooth, vol, snap);

        const float mains = knob (6);
        glide (sag, 0.2f + 0.5f * mains, snap);
        glide (drive, 1.5f * (1.0f + 0.6f * mains), snap);

        stack.update (parts, knob (4), knob (3), audioTaper (knob (2)), snap, osRate);
        glide (presenceDb, juce::jmap (knob (5), -4.0f, 9.0f), snap);

        // Loud is the sound: past its default the volume mostly pushes the power stage, so the output rises slowly.
        const float out = trim * dsp::dbToGain (-0.2f * juce::jmax (0.0f, volDb - defDb)) / (1.0f + 0.35f * mains);
        glide (output, out, snap);

        const double brightDb = 9.0 * std::pow (1.0 - (double) volSmooth.end, 1.5);
        for (auto& c : chans)
        {
            c.brightCap.highShelf (osRate, 3000.0, 0.6, brightDb);
            c.presence.highShelf (sampleRate, 3200.0, 0.6, presenceDb);
        }
    }

    void preProcess (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        for (int i = 0; i < n; ++i)
            data[i] = c.inputHp.hp (data[i]);
    }

    void processOversampled (int ch, float* x, int n) override
    {
        auto& c = chans[(size_t) ch];
        const bool useBright = mode != 1, useNormal = mode != 0;

        for (int i = 0; i < n; ++i)
        {
            float y = 0.0f;
            if (useBright) y += c.v1Bright.process (c.brightCap.process (x[i]));
            if (useNormal) y += c.v1Normal.process (x[i]);
            y *= mix * volume.at (i, n);

            y = c.v2.process (y);
            y = -c.follower.process (y);    // cathode follower: positive grid swings clip first, no inversion

            y = c.stackState.process (stack.filter, y) * 3.0f;
            y = c.piLp.lp (y);

            const float s = sag.at (i, n);
            x[i] = c.power.process (y, drive.at (i, n), s, 0.0f, 0.0f, sagAttack, sagRelease);
        }
    }

    void postProcess (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        for (int i = 0; i < n; ++i)
        {
            float y = c.lowBump.process (data[i]);
            y = c.otLp.process (c.presence.process (y));
            data[i] = c.dcBlock.hp (y) * output.at (i, n);
        }
    }

private:
    static constexpr float defaultVolume = 0.5f;
    static constexpr ToneStackParts parts { 220e3, 1e6, 25e3, 56e3, 500e-12, 22e-9, 22e-9 };

    struct Channel
    {
        OnePole inputHp, piLp, dcBlock;
        Biquad brightCap, lowBump, presence, otLp;
        TriodeStage v1Bright, v1Normal, v2, follower;
        ToneStackState stackState;
        PowerStage power;
    };

    std::array<Channel, 2> chans;
    SmoothedStack stack;
    BlockRamp volume, volSmooth, sag, drive, output;
    float presenceDb = 0.0f, trim = 1.0f, mix = 1.0f, sagAttack = 0.01f, sagRelease = 0.001f;
};

//==============================================================================
// FORTRESS: modern three-channel high-gain amp.
//
// Clean (two stages), Crunch (three) and Lead (four cascaded stages) share a
// tight input filter with a pre-clip mid push, a modern stack with fixed mids,
// two graphic-style mid bands after the preamp, and a stiff, fast power stage.
// Tight cuts lows before the gain and between stages; Loose lets them through
// and adds an output-transformer thump.
//==============================================================================

struct BrickChannel
{
    float gainMinDb, gainMaxDb, midPushDb;
    int numStages;
    std::array<float, 4> stageGain, bias, millerHz, couplingHz;
    float trim;
};

const std::array<BrickChannel, 3> brickChannels {{
    { -10.0f, 20.0f, 0.0f, 2, { 1.0f, 2.2f, 1.0f, 1.0f }, { 0.08f, 0.05f, 0.0f, 0.0f }, { 14000.0f, 10000.0f, 9000.0f, 9000.0f }, { 30.0f, 40.0f, 40.0f, 40.0f }, 0.280f },
    { -6.0f, 30.0f, 3.0f, 3, { 1.0f, 5.0f, 1.5f, 1.0f }, { 0.12f, 0.20f, 0.35f, 0.0f }, { 11000.0f, 7500.0f, 9000.0f, 9000.0f }, { 60.0f, 100.0f, 30.0f, 30.0f }, 0.110f },
    { -10.0f, 34.0f, 6.0f, 4, { 1.0f, 8.0f, 6.0f, 2.0f }, { 0.10f, 0.15f, 0.20f, 0.30f }, { 10000.0f, 6500.0f, 5500.0f, 6000.0f }, { 120.0f, 160.0f, 110.0f, 40.0f }, 0.061f },
}};

class FortressAmp final : public ModelAmp
{
public:
    FortressAmp() : ModelAmp (2) {}

protected:
    int requestedMode() const override                          { return choiceIndex (0, 3) * 2 + choiceIndex (1, 2); }

    void modeChanged() override
    {
        const auto& v = brickChannels[(size_t) (mode / 2)];
        const bool tight = (mode % 2) == 0;
        const float couplingScale = tight ? 1.6f : 0.6f;

        for (auto& c : chans)
        {
            c.inputHp1.setup (tight ? 140.0f : 55.0f, sampleRate);
            c.inputHp2.setup (tight ? 140.0f : 10.0f, sampleRate);
            c.midPush.peaking (sampleRate, 800.0, 0.8, v.midPushDb);
            for (size_t s = 0; s < 4; ++s)
                c.stages[s].setup (v.stageGain[s], v.bias[s], v.millerHz[s], v.couplingHz[s] * (s == 0 ? 1.0f : couplingScale), osRate);
            c.piLp.setup (12000.0f, osRate);
            c.thump.peaking (sampleRate, tight ? 110.0 : 85.0, 0.9, tight ? 0.5 : 3.0);
            c.presence.highShelf (sampleRate, 3500.0, 0.6, 2.0);
            c.otLp.lowPass (sampleRate, 7500.0, 0.7);
            c.dcBlock.setup (10.0f, sampleRate);
        }

        numStages = v.numStages;
        polarity = (numStages % 2) != 0 ? -1.0f : 1.0f;
        trim = v.trim * (tight ? 1.0f : 0.79f);    // Loose is fatter, not louder
        sagAttack = osTimeCoeff (5.0f);
        sagRelease = osTimeCoeff (70.0f);
        stack.dirty = true;
    }

    void resetState() override
    {
        for (auto& c : chans)
        {
            c.inputHp1.reset(); c.inputHp2.reset(); c.midPush.reset();
            for (auto& s : c.stages) s.reset();
            c.stackState.reset(); c.lowMid.reset(); c.highMid.reset(); c.piLp.reset(); c.power.reset(); c.sagEnv = 0.0f;
            c.thump.reset(); c.presence.reset(); c.otLp.reset(); c.dcBlock.reset();
        }
    }

    void updateParameters (bool snap) override
    {
        const auto& v = brickChannels[(size_t) (mode / 2)];
        glide (gain, dsp::dbToGain (juce::jmap (knob (2), v.gainMinDb, v.gainMaxDb)), snap);

        stack.update (parts, knob (6), 0.35, audioTaper (knob (3)), snap, osRate);

        glide (lowMidDb, knob (4), snap);
        glide (highMidDb, knob (5), snap);
        for (auto& c : chans)
        {
            c.lowMid.peaking (osRate, 450.0, 1.0, lowMidDb);
            c.highMid.peaking (osRate, 1600.0, 1.2, highMidDb);
        }

        const float masterDb = knob (7) < 0.5f ? juce::jmap (knob (7), 0.0f, 0.5f, -30.0f, 0.0f)
                                               : juce::jmap (knob (7), 0.5f, 1.0f, 0.0f, 12.0f);
        glide (master, dsp::dbToGain (masterDb), snap);
        glide (output, polarity * trim * dsp::dbToGain (-0.5f * juce::jmax (0.0f, masterDb)), snap);
    }

    void preProcess (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        for (int i = 0; i < n; ++i)
            data[i] = c.midPush.process (c.inputHp2.hp (c.inputHp1.hp (data[i]))) * gain.at (i, n);
    }

    void processOversampled (int ch, float* x, int n) override
    {
        auto& c = chans[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            float y = x[i];
            for (int s = 0; s < numStages; ++s)
                y = c.stages[(size_t) s].process (y);

            y = c.stackState.process (stack.filter, y) * 3.0f;
            y = c.highMid.process (c.lowMid.process (y));
            y = c.piLp.lp (y * master.at (i, n));
            // Stiff, fast power stage.
            const float rail = 1.0f - 0.1f * juce::jmin (1.0f, c.sagEnv);
            y = rail * c.power.process (y / rail);
            const float a = std::abs (y);
            c.sagEnv += (a > c.sagEnv ? sagAttack : sagRelease) * (a - c.sagEnv);
            x[i] = y;
        }
    }

    void postProcess (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        for (int i = 0; i < n; ++i)
        {
            float y = c.thump.process (data[i]);
            y = c.otLp.process (c.presence.process (y));
            data[i] = c.dcBlock.hp (y) * output.at (i, n);
        }
    }

private:
    static constexpr ToneStackParts parts { 250e3, 1e6, 25e3, 39e3, 680e-12, 33e-9, 22e-9 };

    struct Channel
    {
        OnePole inputHp1, inputHp2, piLp, dcBlock;
        Biquad midPush, lowMid, highMid, thump, presence, otLp;
        std::array<HotStage, 4> stages;
        ToneStackState stackState;
        AntiAliasedAlgebraic power;
        float sagEnv = 0.0f;
    };

    std::array<Channel, 2> chans;
    SmoothedStack stack;
    BlockRamp gain, master, output;
    float lowMidDb = 0.0f, highMidDb = 0.0f, trim = 1.0f, polarity = 1.0f, sagAttack = 0.01f, sagRelease = 0.001f;
    int numStages = 4;
};

//==============================================================================
// BANDSTAND: chimey class-A amp.
//
// Normal: one triode stage and a fixed, warm tone network. Brilliant: an extra
// gain stage and cathode follower driving a two-knob treble/bass network. Both
// feed a cathode-biased push-pull power stage with no negative feedback: it
// saturates early, its bias slides as it works (bloom and compression), and
// the speaker's impedance curve shows through (low thump, rising upper mids).
// Cut is a fixed-frequency treble bleed after the phase inverter.
//==============================================================================

class BandstandAmp final : public ModelAmp
{
public:
    BandstandAmp() : ModelAmp (1) {}

protected:
    int requestedMode() const override                          { return choiceIndex (0, 2); }

    void modeChanged() override
    {
        const bool brilliant = mode == 1;
        for (auto& c : chans)
        {
            c.inputHp.setup (50.0f, sampleRate);
            c.v1.setup (2.0f, 0.25f, 15000.0f, 40.0f, osRate);
            c.v2.setup (4.5f, 0.22f, 11000.0f, 60.0f, osRate);
            c.follower.setup (1.0f, 0.5f, 20000.0f, 15.0f, osRate);
            c.normalTone.setup (4500.0f, osRate);
            c.scoop.peaking (osRate, 700.0, 0.7, -2.0);
            c.impedanceLow.peaking (sampleRate, 110.0, 1.2, 3.0);
            c.impedanceHigh.highShelf (sampleRate, 2800.0, 0.6, 3.5);
            c.otLp.lowPass (sampleRate, 9000.0, 0.7);
            c.dcBlock.setup (10.0f, sampleRate);
        }

        polarity = brilliant ? 1.0f : -1.0f;
        trim = brilliant ? 0.159f : 0.215f;
        sagAttack = osTimeCoeff (10.0f);
        sagRelease = osTimeCoeff (150.0f);
        biasAttack = osTimeCoeff (30.0f);
        biasRelease = osTimeCoeff (400.0f);
    }

    void resetState() override
    {
        for (auto& c : chans)
        {
            c.inputHp.reset(); c.v1.reset(); c.v2.reset(); c.follower.reset(); c.normalTone.reset(); c.scoop.reset();
            c.treble.reset(); c.bass.reset(); c.cut.reset(); c.power.reset();
            c.impedanceLow.reset(); c.impedanceHigh.reset(); c.otLp.reset(); c.dcBlock.reset();
            c.cathode = 0.0f;
        }
    }

    void updateParameters (bool snap) override
    {
        const float volDb = juce::jmap (knob (1), -20.0f, 20.0f);
        glide (volume, dsp::dbToGain (volDb), snap);
        glide (trebleDb, juce::jmap (knob (2), -12.0f, 8.0f), snap);
        glide (bassDb, juce::jmap (knob (3), -14.0f, 4.0f), snap);
        glide (cutHz, 20000.0f * std::pow (1500.0f / 20000.0f, knob (4)), snap);

        for (auto& c : chans)
        {
            c.treble.highShelf (osRate, 2000.0, 0.9, trebleDb);
            c.bass.lowShelf (osRate, 150.0, 0.7, bassDb);
            c.cut.setup (cutHz, osRate);
        }

        const float masterDb = knob (5) < 0.5f ? juce::jmap (knob (5), 0.0f, 0.5f, -30.0f, 0.0f)
                                               : juce::jmap (knob (5), 0.5f, 1.0f, 0.0f, 12.0f);
        glide (master, dsp::dbToGain (masterDb), snap);
        glide (output, polarity * trim * dsp::dbToGain (-0.5f * juce::jmax (0.0f, masterDb) - 0.3f * juce::jmax (0.0f, volDb)), snap);
    }

    void preProcess (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        for (int i = 0; i < n; ++i)
            data[i] = c.inputHp.hp (data[i]);
    }

    void processOversampled (int ch, float* x, int n) override
    {
        auto& c = chans[(size_t) ch];
        const bool brilliant = mode == 1;

        for (int i = 0; i < n; ++i)
        {
            float y = c.v1.process (x[i]) * volume.at (i, n);

            if (brilliant)
            {
                y = -c.follower.process (c.v2.process (y));
                y = c.bass.process (c.treble.process (c.scoop.process (y)));
            }
            else
            {
                y = c.normalTone.lp (y) * 1.6f;
            }

            y = c.cut.lp (y * master.at (i, n));

            // Cathode-biased class A: the operating point slides with the average drive.
            const float bias = 0.12f - 0.35f * juce::jmin (c.cathode, 1.5f);
            y = c.power.process (y, 1.6f, 0.3f, bias, softClip (bias), sagAttack, sagRelease);
            const float a = std::abs (y);
            c.cathode += (a > c.cathode ? biasAttack : biasRelease) * (a - c.cathode);
            x[i] = y;
        }
    }

    void postProcess (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        for (int i = 0; i < n; ++i)
        {
            float y = c.impedanceHigh.process (c.impedanceLow.process (data[i]));
            y = c.otLp.process (y);
            data[i] = c.dcBlock.hp (y) * output.at (i, n);
        }
    }

private:
    struct Channel
    {
        OnePole inputHp, normalTone, cut, dcBlock;
        Biquad scoop, treble, bass, impedanceLow, impedanceHigh, otLp;
        TriodeStage v1, v2, follower;
        PowerStage power;
        float cathode = 0.0f;
    };

    std::array<Channel, 2> chans;
    BlockRamp volume, master, output;
    float trebleDb = 0.0f, bassDb = 0.0f, cutHz = 10000.0f;
    float polarity = 1.0f, trim = 1.0f, sagAttack = 0.01f, sagRelease = 0.001f, biasAttack = 0.01f, biasRelease = 0.001f;
};

//==============================================================================
// JALOPY: a junk-shop amp with a loose speaker.
//
// Practice: a tiny transistor amp. A preamp that starves as the battery runs
// down (it gates and sputters as notes decay), a class-B output stage with a
// crossover dead zone that widens with the dying battery, and a 6" speaker.
// Radio: an old valve radio's narrow, honky band, diode grit, a heterodyne
// whistle that follows the playing, and a loose connection that drops out
// (more often as the battery dies). Rattle: something loose in the cabinet
// buzzes when the cone swings hard. Every noise source rides the signal, so
// the amp is silent when you stop.
//==============================================================================

/** Dead-zone (crossover) nonlinearity with first-order ADAA. */
struct AntiAliasedDeadZone
{
    float prevX = 0.0f;

    void reset()                                { prevX = 0.0f; }

    static float f (float x, float d)           { return x > d ? x - d : (x < -d ? x + d : 0.0f); }
    static float F (float x, float d)           { const float e = juce::jmax (0.0f, std::abs (x) - d); return 0.5f * e * e; }

    float process (float x, float d)
    {
        const float dx = x - prevX;
        const float y = std::abs (dx) > 1.0e-4f ? (F (x, d) - F (prevX, d)) / dx : f (0.5f * (x + prevX), d);
        prevX = x;
        return y;
    }
};

class JalopyAmp final : public ModelAmp
{
public:
    JalopyAmp() : ModelAmp (1) {}

protected:
    int requestedMode() const override                          { return choiceIndex (0, 2); }

    void modeChanged() override
    {
        const bool radio = mode == 1;
        for (auto& c : chans)
        {
            c.inHp.highPass (sampleRate, radio ? 320.0 : 90.0, 0.7);
            c.inLp.lowPass (sampleRate, radio ? 3400.0 : 9000.0, 0.8);
            c.spkHp.highPass (sampleRate, radio ? 300.0 : 170.0, radio ? 1.1 : 0.9);
            c.spkPeak.peaking (sampleRate, radio ? 1000.0 : 1300.0, 1.2, radio ? 4.0 : 5.0);
            c.spkLp.lowPass (sampleRate, radio ? 3300.0 : 4800.0, radio ? 1.4 : 1.2);
            c.excursion1.setup (140.0f, sampleRate);
            c.excursion2.setup (140.0f, sampleRate);
            c.rattleBand.setup (3200.0f, 2.5f, sampleRate);
            c.rattleBand2.setup (5600.0f, 3.0f, sampleRate);
            c.dcBlock.setup (12.0f, sampleRate);
        }

        trim = radio ? 0.217f : 0.1065f;
        sagAttack = osTimeCoeff (4.0f);
        sagRelease = osTimeCoeff (300.0f);
        envAttack = dsp::onePoleCoeff (200.0f, sampleRate);
        envRelease = dsp::onePoleCoeff (2.0f, sampleRate);
        gateCoeff = dsp::onePoleCoeff (60.0f, sampleRate);
    }

    void resetState() override
    {
        for (auto& c : chans)
        {
            c.inHp.reset(); c.inLp.reset(); c.preClip.reset(); c.toneLp.reset(); c.dead.reset(); c.power.reset();
            c.spkHp.reset(); c.spkPeak.reset(); c.spkLp.reset(); c.excursion1.reset(); c.excursion2.reset();
            c.rattleBand.reset(); c.rattleBand2.reset(); c.dcBlock.reset();
            c.env = 0.0f; c.gate = 1.0f; c.dropLeft = 0; c.whistlePhase = 0.0f; c.driftPhase = 0.0f;
        }
        chans[0].rng = 0x1234567u;
        chans[1].rng = 0x7654321u;
    }

    void updateParameters (bool snap) override
    {
        const float gainDb = juce::jmap (knob (1), -6.0f, 30.0f);
        glide (gain, dsp::dbToGain (gainDb), snap);
        glide (toneHz, 900.0f * std::pow (10.0f, knob (2)), snap);

        const float dying = 1.0f - knob (3);
        glide (starve, -1.1f * dying * dying, snap);
        glide (deadZone, 0.015f + 0.12f * dying, snap);
        glide (sag, 0.25f + 0.55f * dying, snap);
        dropChance = 0.02f + 0.5f * dying * dying;
        rattle = knob (4);

        glide (output, trim * dsp::dbToGain (knob (5) - 0.3f * juce::jmax (0.0f, gainDb - 12.0f)) * (1.0f + 0.8f * dying), snap);

        for (auto& c : chans)
            c.toneLp.setup (toneHz, osRate);
    }

    void preProcess (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        for (int i = 0; i < n; ++i)
            data[i] = c.inLp.process (c.inHp.process (data[i])) * gain.at (i, n);
    }

    void processOversampled (int ch, float* x, int n) override
    {
        auto& c = chans[(size_t) ch];
        const bool radio = mode == 1;

        for (int i = 0; i < n; ++i)
        {
            float y;

            if (radio)
            {
                // Germanium-ish detector diode: lopsided, gritty.
                y = c.preClip.process (x[i] * 2.0f + 0.25f) - softClip (0.25f);
                y = c.toneLp.lp (y);
                y = c.power.process (y, 1.2f, sag.at (i, n), 0.0f, 0.0f, sagAttack, sagRelease);
            }
            else
            {
                // Starved transistor: the operating point sinks as the battery dies, so only peaks get through.
                const float b = starve.at (i, n);
                y = c.preClip.process (x[i] * 3.0f + b) - softClip (b);
                y = c.toneLp.lp (y);
                y = c.dead.process (y, deadZone.at (i, n)) * 1.3f;
                y = c.power.process (y, 1.0f, sag.at (i, n), 0.0f, 0.0f, sagAttack, sagRelease);
            }

            x[i] = y;
        }
    }

    void postProcess (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        const bool radio = mode == 1;
        const float sr = (float) sampleRate;

        for (int i = 0; i < n; ++i)
        {
            float y = data[i];

            // Cone excursion (the low end the little speaker can't really handle) and the playing envelope.
            const float exc = c.excursion2.lp (c.excursion1.lp (y));
            const float a = std::abs (y);
            c.env += (a > c.env ? envAttack : envRelease) * (a - c.env);

            y = c.spkLp.process (c.spkPeak.process (c.spkHp.process (y)));

            if (rattle > 0.001f)
            {
                const float threshold = 0.03f + 0.12f * (1.0f - rattle);
                const float hit = juce::jmax (0.0f, std::abs (exc) - threshold) * (0.5f + 0.9f * rattle);
                const float buzz = noise (c) * hit;
                y += c.rattleBand.process (buzz).band + 0.5f * c.rattleBand2.process (buzz).band;
            }

            if (radio)
            {
                // Heterodyne whistle, drifting slowly, only as loud as the playing.
                c.driftPhase += 0.13f / sr;
                if (c.driftPhase >= 1.0f) c.driftPhase -= 1.0f;
                const float hz = 2200.0f + 350.0f * std::sin (juce::MathConstants<float>::twoPi * c.driftPhase);
                c.whistlePhase += hz / sr;
                if (c.whistlePhase >= 1.0f) c.whistlePhase -= 1.0f;
                y += 0.12f * c.env * std::sin (juce::MathConstants<float>::twoPi * c.whistlePhase);

                // Loose connection: now and then the signal drops out for a moment, with a crackle on the way back.
                if (c.dropLeft > 0)
                {
                    if (--c.dropLeft == 0)
                        y += 2.0f * c.env * noise (c);
                }
                else if (c.env > 0.01f && noise01 (c) < dropChance * (1.0f / 2400.0f))
                {
                    c.dropLeft = (int) (sr * (0.02f + 0.12f * noise01 (c)));
                }

                c.gate += gateCoeff * ((c.dropLeft > 0 ? 0.04f : 1.0f) - c.gate);
                y *= c.gate;
            }

            data[i] = c.dcBlock.hp (y) * output.at (i, n);
        }
    }

private:
    struct Channel
    {
        Biquad inHp, inLp, spkHp, spkPeak, spkLp;
        AntiAliasedClip preClip;
        AntiAliasedDeadZone dead;
        OnePole toneLp, excursion1, excursion2, dcBlock;
        PowerStage power;
        dsp::Svf rattleBand, rattleBand2;
        float env = 0.0f, gate = 1.0f, whistlePhase = 0.0f, driftPhase = 0.0f;
        int dropLeft = 0;
        uint32_t rng = 1u;
    };

    static float noise (Channel& c)
    {
        c.rng = c.rng * 1664525u + 1013904223u;
        return (float) (int32_t) c.rng * (1.0f / 2147483648.0f);
    }

    static float noise01 (Channel& c)                           { return 0.5f * (noise (c) + 1.0f); }

    std::array<Channel, 2> chans;
    BlockRamp gain, starve, deadZone, sag, output;
    float toneHz = 3000.0f, dropChance = 0.0f, rattle = 0.0f;
    float trim = 1.0f, sagAttack = 0.01f, sagRelease = 0.001f, envAttack = 0.1f, envRelease = 0.001f, gateCoeff = 0.1f;
};

//==============================================================================
//==============================================================================
// Miked cabinets.
//
// Same approach as BOXCAR (analytic magnitude -> minimum-phase FIR -> zero-
// latency partitioned convolution, IRs morphed in the frequency domain), with
// a different set of cabinets and a microphone model on top: each IR is
// cabinet x mic type x mic position. Distance from the grille is a post
// network: proximity bass, high-frequency loss and a floor reflection.
//==============================================================================
//==============================================================================

namespace conv
{
    constexpr int hop = 64;
    constexpr int fftOrder = 7;
    constexpr int fftSize = 1 << fftOrder;
    constexpr int spectrumFloats = (fftSize / 2 + 1) * 2;
    constexpr int irLength = 2048;
    constexpr int numParts = (irLength - hop) / hop;
    constexpr int tailFloats = numParts * spectrumFloats;
    constexpr int irFloats = hop + tailFloats;      // reversed head taps, then the tail partitions' spectra
}

/** Per-channel state of a zero-latency uniformly partitioned convolver (IR layout as `conv::irFloats`). */
struct ConvolverState
{
    std::array<float, 2 * conv::hop> hist {};
    std::array<float, conv::hop> block {}, prevBlock {}, tailOut {};
    std::array<float, 2 * conv::fftSize> fftBuf {}, accum {};
    std::vector<float> fdl;
    int histPos = 0, pos = 0, slot = 0;

    void prepare()                  { fdl.assign ((size_t) conv::tailFloats, 0.0f); }

    void reset()
    {
        std::fill (fdl.begin(), fdl.end(), 0.0f);
        hist.fill (0.0f);
        block.fill (0.0f);
        prevBlock.fill (0.0f);
        tailOut.fill (0.0f);
        histPos = pos = slot = 0;
    }

    float process (float in, const float* ir, juce::dsp::FFT& fft)
    {
        const float* headRev = ir;

        hist[(size_t) histPos] = in;
        hist[(size_t) (histPos + conv::hop)] = in;
        const float* win = hist.data() + histPos + 1;
        float acc[8] {};
        for (int k = 0; k < conv::hop; k += 8)
            for (int j = 0; j < 8; ++j)
                acc[j] += win[k + j] * headRev[k + j];
        histPos = (histPos + 1) % conv::hop;

        float y = ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
        y += tailOut[(size_t) pos];

        block[(size_t) pos] = in;
        if (++pos == conv::hop)
        {
            runTail (ir + conv::hop, fft);
            pos = 0;
        }

        return y;
    }

private:
    void runTail (const float* tail, juce::dsp::FFT& fft)
    {
        std::copy (prevBlock.begin(), prevBlock.end(), fftBuf.begin());
        std::copy (block.begin(), block.end(), fftBuf.begin() + conv::hop);
        std::fill (fftBuf.begin() + conv::fftSize, fftBuf.end(), 0.0f);
        fft.performRealOnlyForwardTransform (fftBuf.data(), true);

        slot = (slot + 1) % conv::numParts;
        std::copy (fftBuf.begin(), fftBuf.begin() + conv::spectrumFloats, fdl.begin() + slot * conv::spectrumFloats);

        std::fill (accum.begin(), accum.end(), 0.0f);
        float* acc = accum.data();

        for (int m = 0; m < conv::numParts; ++m)
        {
            const int s = (slot - m + conv::numParts) % conv::numParts;
            const float* X = fdl.data() + s * conv::spectrumFloats;
            const float* H = tail + m * conv::spectrumFloats;

            for (int k = 0; k < conv::spectrumFloats; k += 2)
            {
                acc[k]     += X[k] * H[k] - X[k + 1] * H[k + 1];
                acc[k + 1] += X[k] * H[k + 1] + X[k + 1] * H[k];
            }
        }

        fft.performRealOnlyInverseTransform (acc);
        std::copy (acc + conv::hop, acc + conv::fftSize, tailOut.begin());
        prevBlock = block;
    }
};

/** A bank of minimum-phase cabinet IRs, designed from analytic magnitudes at prepare time. */
struct IrBank
{
    std::vector<float> data;
    double rate = 0.0;

    const float* get (int index) const          { return data.data() + (size_t) index * conv::irFloats; }

    /** magnitudeOf (index, hz) -> linear magnitude. Each IR is loudness-normalised over the guitar band. */
    template <typename MagnitudeFn>
    void build (int count, double sr, juce::dsp::FFT& smallFft, MagnitudeFn&& magnitudeOf)
    {
        data.assign ((size_t) count * conv::irFloats, 0.0f);

        constexpr int designOrder = 14;
        constexpr int N = 1 << designOrder;
        juce::dsp::FFT big (designOrder);
        std::vector<std::complex<float>> bufA ((size_t) N), bufB ((size_t) N);
        std::vector<float> ir ((size_t) conv::irLength);
        std::array<float, 2 * conv::fftSize> part {};

        for (int index = 0; index < count; ++index)
        {
            double weighted = 0.0, weights = 0.0;
            for (int k = 0; k <= N / 2; ++k)
            {
                const double hz = (double) k * sr / (double) N;
                const double mag = magnitudeOf (index, hz);
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

            // Real cepstrum, folded to make it causal: minimum phase.
            big.perform (bufA.data(), bufB.data(), true);
            bufA[0] = { bufB[0].real(), 0.0f };
            for (int k = 1; k < N / 2; ++k) bufA[(size_t) k] = { 2.0f * bufB[(size_t) k].real(), 0.0f };
            bufA[(size_t) (N / 2)] = { bufB[(size_t) (N / 2)].real(), 0.0f };
            for (int k = N / 2 + 1; k < N; ++k) bufA[(size_t) k] = {};

            big.perform (bufA.data(), bufB.data(), false);
            for (auto& z : bufB) z = std::exp (z);
            big.perform (bufB.data(), bufA.data(), true);

            const int fade = conv::irLength / 4;
            for (int i = 0; i < conv::irLength; ++i)
            {
                float w = 1.0f;
                if (i >= conv::irLength - fade)
                    w = 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * (float) (i - (conv::irLength - fade)) / (float) fade));
                ir[(size_t) i] = bufA[(size_t) i].real() * w;
            }

            float* dest = data.data() + (size_t) index * conv::irFloats;
            for (int k = 0; k < conv::hop; ++k)
                dest[k] = ir[(size_t) (conv::hop - 1 - k)];

            for (int m = 0; m < conv::numParts; ++m)
            {
                part.fill (0.0f);
                std::copy (ir.begin() + conv::hop + m * conv::hop, ir.begin() + conv::hop + (m + 1) * conv::hop, part.begin());
                smallFft.performRealOnlyForwardTransform (part.data(), true);
                std::copy (part.begin(), part.begin() + conv::spectrumFloats, dest + conv::hop + m * conv::spectrumFloats);
            }
        }

        rate = sr;
    }
};

/** The IR a convolver is actually running, gliding toward a target (a blend of two bank entries). */
struct MorphingIr
{
    std::vector<float> current, target;
    int morphBlocks = 0;

    void prepare()                  { current.assign ((size_t) conv::irFloats, 0.0f); target.assign ((size_t) conv::irFloats, 0.0f); }

    void setTarget (const float* a, const float* b, float w, int blocks)
    {
        for (int k = 0; k < conv::irFloats; ++k)
            target[(size_t) k] = a[k] + w * (b[k] - a[k]);
        morphBlocks = blocks;
    }

    void step (bool snap)
    {
        if (snap)
        {
            std::copy (target.begin(), target.end(), current.begin());
            morphBlocks = 0;
        }
        else if (morphBlocks > 0)
        {
            if (--morphBlocks == 0)
                std::copy (target.begin(), target.end(), current.begin());
            else
                for (size_t k = 0; k < current.size(); ++k)
                    current[k] += 0.15f * (target[k] - current[k]);
        }
    }
};

struct MicCabinet
{
    float lowHz, lowQ;              // cone/box resonance
    float openBack;                 // 0 closed .. 1 fully open back (rear-wave cancellation)
    float rollOffHz;
    float presenceHz, presenceDb, presenceQ;
    std::array<std::array<float, 3>, 4> bumps;   // { Hz, dB, Q }
};

const std::array<MicCabinet, 7> micCabinets {{
    // 0. 1x15 vintage bass: deep, round, dark, a little honk.
    { 55.0f, 1.2f, 0.0f, 3000.0f, 1600.0f, 2.0f, 1.0f, {{ { 120.0f, 2.0f, 1.0f }, { 450.0f, -2.0f, 1.0f }, { 900.0f, 1.5f, 1.5f }, { 2200.0f, -3.0f, 3.0f } }} },
    // 1. 2x12 open back: scooped, glassy, airy, lows that spill out of the back.
    { 90.0f, 0.8f, 1.0f, 5500.0f, 3000.0f, 5.0f, 1.3f, {{ { 700.0f, -2.0f, 1.0f }, { 1300.0f, 1.0f, 1.5f }, { 2000.0f, -3.0f, 3.0f }, { 4200.0f, 2.0f, 3.0f } }} },
    // 2. Warm 4x12: big thump, rounded top, warm low mids.
    { 80.0f, 1.6f, 0.0f, 4200.0f, 2200.0f, 3.5f, 1.1f, {{ { 120.0f, 1.5f, 1.0f }, { 700.0f, 1.5f, 1.0f }, { 1800.0f, -2.0f, 2.0f }, { 3200.0f, 1.5f, 3.0f } }} },
    // 3. 1x12 closed: punchy, focused, compact low end.
    { 105.0f, 1.4f, 0.0f, 5000.0f, 2600.0f, 4.5f, 1.2f, {{ { 250.0f, 1.0f, 1.0f }, { 900.0f, 1.0f, 1.2f }, { 1600.0f, -3.0f, 3.0f }, { 3800.0f, 2.0f, 3.0f } }} },
    // 4. 8x10 bass: huge, even, punchy low mids.
    { 50.0f, 1.0f, 0.0f, 4000.0f, 2500.0f, 2.0f, 1.0f, {{ { 80.0f, 2.0f, 0.8f }, { 250.0f, 1.5f, 1.0f }, { 600.0f, -2.5f, 1.0f }, { 1500.0f, -1.5f, 2.0f } }} },
    // 5. Tight 4x12: modern, scooped, aggressive upper presence.
    { 95.0f, 1.9f, 0.0f, 5200.0f, 2600.0f, 6.0f, 1.1f, {{ { 160.0f, 1.0f, 1.2f }, { 450.0f, -2.5f, 1.0f }, { 1200.0f, -2.0f, 2.0f }, { 3400.0f, 3.0f, 2.5f } }} },
    // 6. 1x10 small open back: boxy, nasal, cheeky.
    { 140.0f, 1.0f, 0.7f, 4600.0f, 2400.0f, 4.0f, 1.2f, {{ { 350.0f, 1.0f, 1.0f }, { 600.0f, 2.5f, 1.2f }, { 1500.0f, -2.5f, 3.0f }, { 3200.0f, 1.5f, 3.0f } }} },
}};

constexpr int numMicTypes = 3;      // dynamic, ribbon, condenser

inline double analogMag (std::complex<double> s, double b0, double b1, double b2, double a0, double a1, double a2)
{
    return std::abs ((b2 * s * s + b1 * s + b0) / (a2 * s * s + a1 * s + a0));
}

/** Analogue-prototype magnitude of a cabinet heard through a mic of `micType`, `edge` from cone centre (0) to edge (1). */
double micCabMagnitude (const MicCabinet& d, int micType, double edge, double hz)
{
    if (hz < 1.0)
        return 0.0;

    auto at = [hz] (double f0) { return std::complex<double> (0.0, hz / f0); };
    auto peak = [&] (double f0, double db, double q)
    {
        const double A = std::pow (10.0, db / 40.0);
        return analogMag (at (f0), 1.0, A / q, 1.0, 1.0, 1.0 / (A * q), 1.0);
    };

    double mag = analogMag (at (d.lowHz), 0.0, 0.0, 1.0, 1.0, 1.0 / (d.lowQ * (1.0 - 0.3 * d.openBack)), 1.0);
    mag *= analogMag (at (d.lowHz * 0.5), 0.0, 1.0, 0.0, 1.0, 1.0, 0.0);

    // Open back: the rear wave cancels some of the front just above the resonance.
    if (d.openBack > 0.0f)
        mag *= peak (d.lowHz * 2.2, -3.5 * d.openBack, 1.1);

    for (auto& b : d.bumps)
        mag *= peak (b[0], b[1], b[2]);

    const double presenceDb = d.presenceDb * (1.0 - 0.55 * edge) - 1.5 * edge;
    mag *= peak (d.presenceHz * (1.0 - 0.12 * edge), presenceDb, d.presenceQ);
    mag *= peak (300.0, 1.5 * edge, 0.8);
    mag *= peak (5200.0, 3.0 * (1.0 - edge) - 2.0, 2.5);

    const double roll = d.rollOffHz * (1.2 - 0.45 * edge);
    mag *= analogMag (at (roll), 1.0, 0.0, 0.0, 1.0, 1.0 / 0.9, 1.0);
    mag *= analogMag (at (roll * 1.15), 1.0, 0.0, 0.0, 1.0, 1.0 / 0.6, 1.0);
    mag *= analogMag (at (roll * 1.4), 1.0, 0.0, 0.0, 1.0, 1.0 / 0.55, 1.0);
    mag *= peak (roll * 1.35, 4.0, 4.0) * peak (roll * 1.7, -5.0, 5.0) * peak (roll * 2.1, 3.0, 4.0);

    switch (micType)
    {
        case 0:     // Dynamic: lean lows, a forward presence lift, a softened top octave.
            mag *= analogMag (at (65.0), 0.0, 0.0, 1.0, 1.0, 1.0 / 0.7, 1.0);
            mag *= peak (4500.0, 4.0, 1.4) * peak (9500.0, -3.0, 1.0) * peak (900.0, 1.0, 0.8);
            break;

        case 1:     // Ribbon: warm, smooth upper mids, a gentle, early top roll-off.
            mag *= peak (140.0, 1.5, 0.7) * peak (2800.0, -1.5, 0.9);
            mag *= analogMag (at (7500.0), 1.0, 0.0, 0.0, 1.0, 1.0 / 0.6, 1.0);
            break;

        default:    // Condenser: extended lows, detailed upper mids, air.
            mag *= peak (4200.0, 3.0, 0.6) * peak (9000.0, 6.0, 0.7) * peak (500.0, -1.0, 0.7);
            break;
    }

    return mag;
}

/** Distance from the grille: proximity bass, top loss and a floor reflection. One per channel. */
struct MicDistance
{
    Biquad proximity, air;
    dsp::DelayBuffer reflection;
    float reflectionLp = 0.0f;

    void prepare (double sr)        { reflection.setMaxSamples ((int) (sr * 0.006)); }
    void reset()                    { proximity.reset(); air.reset(); reflection.clear(); reflectionLp = 0.0f; }

    float process (float y, float delaySamples, float reflectionGain, float lpCoeff)
    {
        reflection.push (y);
        reflectionLp += lpCoeff * (reflection.read (delaySamples) - reflectionLp);
        y += reflectionGain * reflectionLp;
        return air.process (proximity.process (y));
    }
};

/** How much a mic type's proximity effect lifts the lows right on the grille (dB). */
constexpr float proximityDb[numMicTypes] { 5.0f, 8.0f, 3.0f };

//==============================================================================
// HANGAR: miked cabinet with mic type, position and distance.
//==============================================================================

class HangarCab final : public Pedal
{
public:
    static constexpr std::array<int, 5> cabinetIndex { 0, 1, 2, 3, 4 };
    static constexpr int numPositions = 3;

    void prepared() override
    {
        if (fft == nullptr)
            fft = std::make_unique<juce::dsp::FFT> (conv::fftOrder);

        if (std::abs (bank.rate - sampleRate) > 0.5)
        {
            bank.build ((int) cabinetIndex.size() * numMicTypes * numPositions, sampleRate, *fft, [] (int index, double hz)
            {
                const int pos = index % numPositions, mic = (index / numPositions) % numMicTypes, cab = index / (numPositions * numMicTypes);
                return micCabMagnitude (micCabinets[(size_t) cabinetIndex[(size_t) cab]], mic, (double) pos / (numPositions - 1), hz);
            });
        }

        ir.prepare();
        for (auto& c : chans)
        {
            c.conv.prepare();
            c.micDistance.prepare (sampleRate);
        }

        reflectionLpCoeff = dsp::onePoleCoeff (5000.0f, sampleRate);
        snapNext = true;
    }

    void reset() override
    {
        for (auto& c : chans)
        {
            c.conv.reset();
            c.micDistance.reset();
            c.lowCut.reset();
            c.highCut1.reset();
            c.highCut2.reset();
        }
        snapNext = true;
    }

    void update() override
    {
        const int cab = juce::jlimit (0, (int) cabinetIndex.size() - 1, juce::roundToInt (knob (0)));
        const int mic = juce::jlimit (0, numMicTypes - 1, juce::roundToInt (knob (1)));
        const float position = juce::jlimit (0.0f, 1.0f, knob (2));

        if (snapNext || cab != targetCab || mic != targetMic || std::abs (position - targetPosition) > 1.0e-4f)
        {
            const int blocks = (cab != targetCab || mic != targetMic) ? 40 : 12;
            targetCab = cab;
            targetMic = mic;
            targetPosition = position;

            const float p = position * (float) (numPositions - 1);
            const int i0 = juce::jmin ((int) p, numPositions - 2);
            const int base = (cab * numMicTypes + mic) * numPositions + i0;
            ir.setTarget (bank.get (base), bank.get (base + 1), p - (float) i0, blocks);
        }

        ir.step (snapNext);

        // Distance: close in, the mic's proximity effect lifts the lows; further out the top fades and the floor joins in.
        const float dist = juce::jlimit (0.0f, 1.0f, knob (3));
        glide (distance, dist, snapNext);
        glide (proximity, proximityDb[mic], snapNext);
        const float d = distance;
        const float proxDb = proximity * (std::pow (1.0f - d, 2.5f) - 0.35f);
        const float airDb = -7.0f * d;
        const float reflGain = 0.1f + 0.4f * d;
        glide (reflectionDelay, (0.3f + 2.6f * d) * 0.001f * (float) sampleRate, snapNext);
        glide (reflection, reflGain, snapNext);

        const float levelTarget = dsp::dbToGain (knob (6) - 0.3f * proxDb + 2.5f * d) / (1.0f + 0.45f * reflGain);
        glide (lowCutHz, knob (4), snapNext);
        glide (highCutHz, knob (5), snapNext);
        glide (level, levelTarget, snapNext);

        for (auto& c : chans)
        {
            c.micDistance.proximity.lowShelf (sampleRate, 180.0, 0.7, proxDb);
            c.micDistance.air.highShelf (sampleRate, 5000.0, 0.7, airDb);
            c.lowCut.setup (lowCutHz, 0.707f, sampleRate);
            c.highCut1.setup (highCutHz, 0.54f, sampleRate);
            c.highCut2.setup (highCutHz, 1.31f, sampleRate);
        }

        snapNext = false;
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        const float* h = ir.current.data();

        for (int i = 0; i < n; ++i)
        {
            float y = c.conv.process (data[i], h, *fft);
            y = c.micDistance.process (y, juce::jmax (2.0f, reflectionDelay.at (i, n)), reflection.at (i, n), reflectionLpCoeff);
            y = c.lowCut.process (y).high;
            y = c.highCut2.process (c.highCut1.process (y).low).low;
            data[i] = y * level.at (i, n);
        }
    }

private:
    struct Channel
    {
        ConvolverState conv;
        MicDistance micDistance;
        dsp::Svf lowCut, highCut1, highCut2;
    };

    std::array<Channel, 2> chans;
    std::unique_ptr<juce::dsp::FFT> fft;
    IrBank bank;
    MorphingIr ir;

    int targetCab = -1, targetMic = -1;
    float targetPosition = -1.0f;
    float distance = 0.0f, proximity = 0.0f, lowCutHz = 70.0f, highCutHz = 10000.0f;
    BlockRamp reflectionDelay, reflection, level;
    float reflectionLpCoeff = 0.5f;
    bool snapNext = true;
};

//==============================================================================
// SIDECAR: two mics on one cabinet, blended.
//
// Mic A sits close, near the cone centre; mic B further out towards the edge
// and a little further back (Offset: 0 - 1.5 ms of travel time), so blending
// them gives the familiar dual-mic comb and fattening. Two convolvers.
//==============================================================================

class SidecarCab final : public Pedal
{
public:
    static constexpr std::array<int, 5> cabinetIndex { 1, 2, 3, 5, 6 };
    static constexpr int numPositions = 2;              // A: near the centre, B: towards the edge

    void prepared() override
    {
        if (fft == nullptr)
            fft = std::make_unique<juce::dsp::FFT> (conv::fftOrder);

        if (std::abs (bank.rate - sampleRate) > 0.5)
        {
            bank.build ((int) cabinetIndex.size() * numMicTypes * numPositions, sampleRate, *fft, [] (int index, double hz)
            {
                const int pos = index % numPositions, mic = (index / numPositions) % numMicTypes, cab = index / (numPositions * numMicTypes);
                return micCabMagnitude (micCabinets[(size_t) cabinetIndex[(size_t) cab]], mic, pos == 0 ? 0.15 : 0.7, hz);
            });
        }

        irA.prepare();
        irB.prepare();
        for (auto& c : chans)
        {
            c.convA.prepare();
            c.convB.prepare();
            c.delay.assign ((size_t) delaySize, 0.0f);
        }

        snapNext = true;
    }

    void reset() override
    {
        for (auto& c : chans)
        {
            c.convA.reset();
            c.convB.reset();
            std::fill (c.delay.begin(), c.delay.end(), 0.0f);
            c.writePos = 0;
            c.lowCut.reset();
            c.highCut1.reset();
            c.highCut2.reset();
        }
        snapNext = true;
    }

    void update() override
    {
        const int cab = juce::jlimit (0, (int) cabinetIndex.size() - 1, juce::roundToInt (knob (0)));
        const int micA = juce::jlimit (0, numMicTypes - 1, juce::roundToInt (knob (1)));
        const int micB = juce::jlimit (0, numMicTypes - 1, juce::roundToInt (knob (2)));

        if (snapNext || cab != targetCab || micA != targetMicA || micB != targetMicB)
        {
            const int blocks = snapNext ? 0 : 40;
            const float* a = bank.get ((cab * numMicTypes + micA) * numPositions);
            const float* b = bank.get ((cab * numMicTypes + micB) * numPositions + 1);
            irA.setTarget (a, a, 0.0f, blocks);
            irB.setTarget (b, b, 0.0f, blocks);
            targetCab = cab;
            targetMicA = micA;
            targetMicB = micB;
        }

        irA.step (snapNext);
        irB.step (snapNext);

        const float blend = juce::jlimit (0.0f, 1.0f, knob (3));
        // Mostly-equal-power blend: mixing two partly decorrelated mics shouldn't dip in the middle.
        glide (gainA, std::cos (blend * juce::MathConstants<float>::halfPi) * (1.0f - 0.15f * blend), snapNext);
        glide (gainB, std::sin (blend * juce::MathConstants<float>::halfPi) * (1.0f - 0.15f * (1.0f - blend)), snapNext);
        glide (offset, juce::jlimit (0.0f, 1.0f, knob (4)) * 0.0015f * (float) sampleRate, snapNext);   // up to 1.5 ms
        glide (lowCutHz, knob (5), snapNext);
        glide (highCutHz, knob (6), snapNext);
        glide (level, dsp::dbToGain (knob (7) - 1.5f), snapNext);

        for (auto& c : chans)
        {
            c.lowCut.setup (lowCutHz, 0.707f, sampleRate);
            c.highCut1.setup (highCutHz, 0.54f, sampleRate);
            c.highCut2.setup (highCutHz, 1.31f, sampleRate);
        }

        snapNext = false;
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = chans[(size_t) ch];
        const float* hA = irA.current.data();
        const float* hB = irB.current.data();

        for (int i = 0; i < n; ++i)
        {
            const float in = data[i];

            // Mic B hears the cone a little later: a linearly interpolated delay (0 works too).
            c.delay[(size_t) c.writePos] = in;
            const float d = offset.at (i, n);
            const int di = (int) d;
            const float frac = d - (float) di;
            const float s0 = c.delay[(size_t) ((c.writePos - di) & delayMask)];
            const float s1 = c.delay[(size_t) ((c.writePos - di - 1) & delayMask)];
            c.writePos = (c.writePos + 1) & delayMask;
            const float late = s0 + frac * (s1 - s0);

            float y = gainA.at (i, n) * c.convA.process (in, hA, *fft) + gainB.at (i, n) * c.convB.process (late, hB, *fft);
            y = c.lowCut.process (y).high;
            y = c.highCut2.process (c.highCut1.process (y).low).low;
            data[i] = y * level.at (i, n);
        }
    }

private:
    static constexpr int delaySize = 512;               // > 1.5 ms at 192 kHz
    static constexpr int delayMask = delaySize - 1;

    struct Channel
    {
        ConvolverState convA, convB;
        std::vector<float> delay;
        int writePos = 0;
        dsp::Svf lowCut, highCut1, highCut2;
    };

    std::array<Channel, 2> chans;
    std::unique_ptr<juce::dsp::FFT> fft;
    IrBank bank;
    MorphingIr irA, irB;

    int targetCab = -1, targetMicA = -1, targetMicB = -1;
    float lowCutHz = 70.0f, highCutHz = 10000.0f;
    BlockRamp gainA, gainB, offset, level;
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

    r.add<RoadhouseAmp> ({ "amp.roadhouse", "ROADHOUSE", "vintage combo", "Amp",
                           "Small 1950s-style 1x12 combo: one volume, one tone, and early, hairy breakup",
                           juce::Colour (0xffc8a46a), juce::Colour (0xff2b1d12),
                           { choice ("channel", "Channel", { "Bright", "Normal" }, 0),
                             knob ("volume", "Volume", 0.0f, 1.0f, 0.45f, "%"),
                             knob ("tone", "Tone", 0.0f, 1.0f, 0.55f, "%"),
                             knob ("output", "Output", -12.0f, 12.0f, 0.0f, "dB") } });

    r.add<LimousineAmp> ({ "amp.limousine", "LIMOUSINE", "smooth overdrive amp", "Amp",
                           "Boutique overdrive amp: a vocal, compressed lead channel that blends in over a glassy clean",
                           juce::Colour (0xff1b2236), juce::Colour (0xffe6cf9a),
                           { choice ("voicing", "Voicing", { "Pre", "Post" }, 0),
                             knob ("volume", "Volume", 0.0f, 1.0f, 0.5f, "%"),
                             knob ("drive", "Drive", 0.0f, 1.0f, 0.5f, "%"),
                             knob ("od", "OD Level", 0.0f, 1.0f, 0.6f, "%"),
                             knob ("bass", "Bass", 0.0f, 1.0f, 0.5f, "%"),
                             knob ("mid", "Middle", 0.0f, 1.0f, 0.6f, "%"),
                             knob ("treble", "Treble", 0.0f, 1.0f, 0.5f, "%"),
                             knob ("master", "Master", 0.0f, 1.0f, 0.5f, "%") } });

    r.add<StadiumAmp> ({ "amp.stadium", "STADIUM", "non-master amp", "Amp",
                         "1960s British-style amp with no master: loudness is the crunch, with big, saggy lows",
                         juce::Colour (0xffb8952f), juce::Colour (0xff1a1714),
                         { choice ("input", "Input", { "Bright", "Normal", "Bridged" }, 2),
                           knob ("volume", "Volume", 0.0f, 1.0f, 0.5f, "%"),
                           knob ("bass", "Bass", 0.0f, 1.0f, 0.5f, "%"),
                           knob ("mid", "Middle", 0.0f, 1.0f, 0.6f, "%"),
                           knob ("treble", "Treble", 0.0f, 1.0f, 0.6f, "%"),
                           knob ("presence", "Presence", 0.0f, 1.0f, 0.4f, "%"),
                           knob ("sag", "Sag", 0.0f, 1.0f, 0.4f, "%") } });

    r.add<FortressAmp> ({ "amp.fortress", "FORTRESS", "high-gain amp", "Amp",
                          "Modern three-channel high-gain amp: a tight brick wall of distortion with graphic mids",
                          juce::Colour (0xff2c2f33), juce::Colour (0xffe05a4e),
                          { choice ("channel", "Channel", { "Clean", "Crunch", "Lead" }, 2),
                            choice ("tight", "Low End", { "Tight", "Loose" }, 0),
                            knob ("gain", "Gain", 0.0f, 1.0f, 0.5f, "%"),
                            knob ("bass", "Bass", 0.0f, 1.0f, 0.5f, "%"),
                            knob ("lowmid", "Low Mid", -12.0f, 12.0f, 0.0f, "dB"),
                            knob ("highmid", "High Mid", -12.0f, 12.0f, 0.0f, "dB"),
                            knob ("treble", "Treble", 0.0f, 1.0f, 0.55f, "%"),
                            knob ("master", "Master", 0.0f, 1.0f, 0.5f, "%") } });

    r.add<BandstandAmp> ({ "amp.bandstand", "BANDSTAND", "class-A chime amp", "Amp",
                           "Chimey class-A amp with no feedback: jangle, bloom and a Cut knob for the top end",
                           juce::Colour (0xff6b1f24), juce::Colour (0xfff0dfc0),
                           { choice ("channel", "Channel", { "Normal", "Brilliant" }, 1),
                             knob ("volume", "Volume", 0.0f, 1.0f, 0.5f, "%"),
                             knob ("treble", "Treble", 0.0f, 1.0f, 0.6f, "%"),
                             knob ("bass", "Bass", 0.0f, 1.0f, 0.5f, "%"),
                             knob ("cut", "Cut", 0.0f, 1.0f, 0.3f, "%"),
                             knob ("master", "Master", 0.0f, 1.0f, 0.5f, "%") } });

    r.add<JalopyAmp> ({ "amp.jalopy", "JALOPY", "junk-shop amp", "Amp",
                        "Tiny transistor practice amp or busted old radio, with a dying battery and a rattling speaker",
                        juce::Colour (0xff3fa7a0), juce::Colour (0xff15302e),
                        { choice ("voice", "Voice", { "Practice", "Radio" }, 0),
                          knob ("gain", "Gain", 0.0f, 1.0f, 0.5f, "%"),
                          knob ("tone", "Tone", 0.0f, 1.0f, 0.5f, "%"),
                          knob ("battery", "Battery", 0.0f, 1.0f, 0.8f, "%"),
                          knob ("rattle", "Rattle", 0.0f, 1.0f, 0.3f, "%"),
                          knob ("volume", "Volume", -12.0f, 12.0f, 0.0f, "dB") } });

    r.add<HangarCab> ({ "cab.hangar", "HANGAR", "miked cabinet", "Cab",
                        "Miked cabinet: vintage 1x15 to an 8x10 fridge, with dynamic, ribbon or condenser at any distance",
                        juce::Colour (0xff4a4d36), juce::Colour (0xffe3e0c8),
                        { choice ("cabinet", "Cabinet", { "1x15 Vintage", "2x12 Open", "Warm 4x12", "1x12 Closed", "8x10 Bass" }, 2),
                          choice ("mic", "Mic", { "Dynamic", "Ribbon", "Condenser" }, 0),
                          knob ("position", "Position", 0.0f, 1.0f, 0.3f, "%"),
                          knob ("distance", "Distance", 0.0f, 1.0f, 0.2f, "%"),
                          knob ("lowcut", "Low Cut", 20.0f, 300.0f, 50.0f, "Hz", 0.5f),
                          knob ("highcut", "High Cut", 2000.0f, 16000.0f, 12000.0f, "Hz", 0.5f),
                          knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") } });

    r.add<SidecarCab> ({ "cab.sidecar", "SIDECAR", "dual-mic cabinet", "Cab",
                         "One guitar cabinet, two mics: blend a close mic with an edge mic and slide their timing",
                         juce::Colour (0xff8a4b2a), juce::Colour (0xfff3e2cc),
                         { choice ("cabinet", "Cabinet", { "2x12 Open", "Warm 4x12", "1x12 Closed", "Tight 4x12", "1x10 Small" }, 1),
                           choice ("mica", "Mic A", { "Dynamic", "Ribbon", "Condenser" }, 0),
                           choice ("micb", "Mic B", { "Dynamic", "Ribbon", "Condenser" }, 1),
                           knob ("blend", "Blend", 0.0f, 1.0f, 0.4f, "%"),
                           knob ("offset", "Offset", 0.0f, 1.0f, 0.1f, "%"),
                           knob ("lowcut", "Low Cut", 20.0f, 300.0f, 60.0f, "Hz", 0.5f),
                           knob ("highcut", "High Cut", 2000.0f, 16000.0f, 11000.0f, "Hz", 0.5f),
                           knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") } });
}

} // namespace woodshed
