#include "InstrumentDsp.h"

namespace woodshed
{
namespace
{
using namespace idsp;

//==============================================================================
/** How one drum hit is synthesised: a pitched tone with a pitch drop, filtered noise
    (optionally in clap bursts), a six-oscillator metallic cluster and a beater click. */
struct Recipe
{
    bool valid = false;
    int section = 0;                            // kick, snare, hats, toms, cymbals, perc
    float pan = 0.0f, gain = 1.0f, attackMs = 0.3f;

    float toneLevel = 0.0f, toneHz = 100.0f, tone2Ratio = 1.5f, tone2Gain = 0.0f, toneDecay = 0.2f;
    float pitchAmount = 0.0f, pitchTime = 0.03f, toneDrive = 0.0f, toneBpHz = 0.0f;
    bool squareTone = false;

    float noiseLevel = 0.0f, noiseDecay = 0.1f, noiseHz = 5000.0f, noiseQ = 0.7f;
    int noiseMode = 1;                          // 0 low, 1 band, 2 high
    int bursts = 0;
    float burstGapMs = 9.0f;

    float metalLevel = 0.0f, metalDecay = 0.1f, metalScale = 1.0f, metalBpHz = 8000.0f, metalHpHz = 6000.0f;
    float clickLevel = 0.0f, clickMs = 3.0f;
};

enum Section { kickSection, snareSection, hatSection, tomSection, cymbalSection, percSection, numSections };
enum Kit { studio, tr808, tr909, electronic, lofi };

/** Values the kit reads at each hit, derived once per block. */
struct KitParams
{
    int kit = studio;
    float kickTune = 1.0f, kickDecay = 1.0f, snareTune = 1.0f, snappy = 0.5f, hatDecay = 1.0f, tomTune = 1.0f;
    float width = 0.7f, velSens = 0.8f;
    std::array<float, numSections> levels {};
};

//==============================================================================
Recipe kick (float hz, float decay)
{
    Recipe r;
    r.section = kickSection; r.gain = 1.0f;
    r.toneLevel = 1.0f; r.toneHz = hz; r.tone2Ratio = 1.62f; r.tone2Gain = 0.2f; r.toneDecay = decay;
    r.pitchAmount = 1.2f; r.pitchTime = 0.018f;
    r.noiseLevel = 0.25f; r.noiseMode = 0; r.noiseHz = 1100.0f; r.noiseDecay = 0.05f;
    r.clickLevel = 0.45f; r.clickMs = 4.0f;
    return r;
}

Recipe snare (float hz, float noiseHz)
{
    Recipe r;
    r.section = snareSection; r.pan = -0.05f; r.gain = 0.8f;
    r.toneLevel = 0.7f; r.toneHz = hz; r.tone2Ratio = 1.72f; r.tone2Gain = 0.6f; r.toneDecay = 0.16f;
    r.pitchAmount = 0.25f; r.pitchTime = 0.01f;
    r.noiseLevel = 0.9f; r.noiseMode = 1; r.noiseHz = noiseHz; r.noiseQ = 0.8f; r.noiseDecay = 0.2f;
    r.clickLevel = 0.35f; r.clickMs = 2.0f;
    return r;
}

Recipe tom (float hz, float pan)
{
    Recipe r;
    r.section = tomSection; r.pan = pan; r.gain = 0.8f;
    r.toneLevel = 1.0f; r.toneHz = hz; r.tone2Ratio = 1.5f; r.tone2Gain = 0.3f; r.toneDecay = 0.4f + 22.0f / hz;
    r.pitchAmount = 0.35f; r.pitchTime = 0.06f;
    r.noiseLevel = 0.2f; r.noiseMode = 0; r.noiseHz = 1800.0f; r.noiseDecay = 0.05f;
    r.clickLevel = 0.25f; r.clickMs = 3.0f;
    return r;
}

Recipe hat (float decay, float level)
{
    Recipe r;
    r.section = hatSection; r.pan = -0.4f; r.gain = level;
    r.metalLevel = 0.5f; r.metalDecay = decay; r.metalScale = 1.45f; r.metalBpHz = 7500.0f; r.metalHpHz = 6000.0f;
    r.noiseLevel = 0.6f; r.noiseMode = 1; r.noiseHz = 10000.0f; r.noiseQ = 0.6f; r.noiseDecay = decay * 0.85f;
    return r;
}

Recipe cymbal (float decay, float scale, float pan)
{
    Recipe r;
    r.section = cymbalSection; r.pan = pan; r.gain = 0.7f; r.attackMs = 1.0f;
    r.metalLevel = 0.45f; r.metalDecay = decay; r.metalScale = scale; r.metalBpHz = 6000.0f; r.metalHpHz = 3500.0f;
    r.noiseLevel = 0.5f; r.noiseMode = 2; r.noiseHz = 5000.0f; r.noiseDecay = decay * 0.8f;
    return r;
}

Recipe pitched (float hz, float ratio, float g2, float decay, float level)
{
    Recipe r;
    r.section = percSection; r.gain = level;
    r.toneLevel = 1.0f; r.toneHz = hz; r.tone2Ratio = ratio; r.tone2Gain = g2; r.toneDecay = decay;
    return r;
}

Recipe noisy (float hpHz, float decay, float attackMs, float level)
{
    Recipe r;
    r.section = percSection; r.gain = level; r.attackMs = attackMs;
    r.noiseLevel = 1.0f; r.noiseMode = 2; r.noiseHz = hpHz; r.noiseDecay = decay;
    return r;
}

/** The acoustic ("Studio") kit: the base every other kit modifies. */
Recipe studioRecipe (int note)
{
    switch (note)
    {
        case 35: { auto r = kick (50.0f, 0.5f); r.clickLevel = 0.3f; return r; }
        case 36: return kick (58.0f, 0.42f);
        case 37: { auto r = pitched (520.0f, 1.9f, 0.5f, 0.05f, 0.75f); r.section = snareSection; r.toneLevel = 0.6f;
                   r.noiseLevel = 0.5f; r.noiseHz = 2200.0f; r.noiseQ = 2.0f; r.noiseMode = 1; r.noiseDecay = 0.03f; r.clickLevel = 0.6f; return r; }
        case 38: return snare (190.0f, 2800.0f);
        case 40: { auto r = snare (215.0f, 3500.0f); r.noiseDecay = 0.22f; r.gain = 0.85f; return r; }
        case 39: { Recipe r; r.section = snareSection; r.pan = 0.1f; r.gain = 0.9f; r.noiseLevel = 1.0f; r.noiseMode = 1;
                   r.noiseHz = 1300.0f; r.noiseQ = 1.2f; r.noiseDecay = 0.18f; r.bursts = 3; r.burstGapMs = 9.0f; return r; }
        case 42: return hat (0.045f, 0.75f);
        case 44: { auto r = hat (0.06f, 0.55f); r.clickLevel = 0.15f; r.clickMs = 2.0f; return r; }
        case 46: return hat (0.55f, 0.7f);
        case 41: return tom (82.0f, 0.4f);
        case 43: return tom (98.0f, 0.3f);
        case 45: return tom (112.0f, 0.15f);
        case 47: return tom (131.0f, 0.0f);
        case 48: return tom (150.0f, -0.15f);
        case 50: return tom (176.0f, -0.3f);
        case 49: return cymbal (1.6f, 1.2f, -0.45f);
        case 57: return cymbal (1.4f, 1.3f, 0.45f);
        case 55: { auto r = cymbal (0.7f, 1.6f, -0.2f); r.metalHpHz = 5000.0f; r.gain = 0.6f; return r; }
        case 52: { auto r = cymbal (1.1f, 0.8f, 0.55f); r.metalBpHz = 3500.0f; r.metalHpHz = 2000.0f;
                   r.noiseMode = 1; r.noiseHz = 3500.0f; return r; }
        case 51:
        case 59:
        {
            auto r = cymbal (note == 51 ? 1.8f : 1.6f, note == 51 ? 1.1f : 1.0f, 0.45f);
            r.metalLevel = 0.3f; r.metalBpHz = 7000.0f; r.metalHpHz = 4000.0f;
            r.noiseLevel = 0.12f; r.noiseHz = 6000.0f; r.noiseDecay = 1.2f;
            r.toneLevel = 0.12f; r.toneHz = 3100.0f; r.tone2Ratio = 1.43f; r.tone2Gain = 0.5f; r.toneDecay = 0.4f;
            return r;
        }
        case 53: { auto r = cymbal (1.1f, 1.1f, 0.45f); r.metalLevel = 0.2f; r.noiseLevel = 0.05f;
                   r.toneLevel = 0.45f; r.toneHz = 760.0f; r.tone2Ratio = 2.26f; r.tone2Gain = 0.7f; r.toneDecay = 1.0f; return r; }
        case 54: { auto r = noisy (7000.0f, 0.18f, 4.0f, 0.6f); r.noiseLevel = 0.3f; r.pan = 0.3f;
                   r.metalLevel = 0.45f; r.metalScale = 2.2f; r.metalBpHz = 9000.0f; r.metalHpHz = 6000.0f; r.metalDecay = 0.22f; return r; }
        case 56: { auto r = pitched (540.0f, 800.0f / 540.0f, 1.0f, 0.25f, 0.6f); r.squareTone = true; r.toneBpHz = 900.0f;
                   r.pan = 0.25f; return r; }
        case 60: { auto r = pitched (420.0f, 1.6f, 0.25f, 0.12f, 0.6f); r.pitchAmount = 0.3f; r.pitchTime = 0.02f; r.clickLevel = 0.3f; r.pan = 0.3f; return r; }
        case 61: { auto r = pitched (320.0f, 1.6f, 0.25f, 0.16f, 0.6f); r.pitchAmount = 0.3f; r.pitchTime = 0.02f; r.clickLevel = 0.3f; r.pan = 0.2f; return r; }
        case 62: { auto r = pitched (330.0f, 1.5f, 0.3f, 0.06f, 0.6f); r.clickLevel = 0.4f; r.pan = -0.25f; return r; }
        case 63: { auto r = pitched (340.0f, 1.5f, 0.2f, 0.25f, 0.6f); r.pitchAmount = 0.15f; r.clickLevel = 0.25f; r.pan = -0.25f; return r; }
        case 64: { auto r = pitched (250.0f, 1.5f, 0.2f, 0.3f, 0.6f); r.pitchAmount = 0.15f; r.clickLevel = 0.25f; r.pan = -0.35f; return r; }
        case 69: { auto r = noisy (5000.0f, 0.08f, 3.0f, 0.4f); r.pan = 0.35f; return r; }
        case 70: { auto r = noisy (6000.0f, 0.05f, 8.0f, 0.45f); r.pan = -0.35f; return r; }
        case 75: { auto r = pitched (2500.0f, 1.0f, 0.0f, 0.07f, 0.5f); r.clickLevel = 0.2f; r.pan = 0.3f; return r; }
        case 76: { auto r = pitched (1050.0f, 2.4f, 0.4f, 0.07f, 0.6f); r.clickLevel = 0.2f; r.pan = -0.3f; return r; }
        case 77: { auto r = pitched (800.0f, 2.4f, 0.4f, 0.08f, 0.6f); r.clickLevel = 0.2f; r.pan = -0.3f; return r; }
        case 80: { auto r = pitched (3200.0f, 1.84f, 0.6f, 0.12f, 0.3f); r.pan = 0.4f; return r; }
        case 81: { auto r = pitched (3200.0f, 1.84f, 0.6f, 1.5f, 0.3f); r.pan = 0.4f; return r; }
        default: return {};
    }
}

/** The 808: pure sine kick and toms, metallic six-square hats and cymbals. */
void apply808 (Recipe& r, int note)
{
    switch (note)
    {
        case 35: case 36:
            r.toneHz = note == 35 ? 44.0f : 49.0f; r.tone2Gain = 0.0f; r.toneDecay = 0.9f; r.pitchAmount = 0.5f; r.pitchTime = 0.008f;
            r.noiseLevel = 0.0f; r.clickLevel = 0.2f; r.clickMs = 1.5f; r.toneDrive = 0.3f; break;
        case 38: case 40:
            r.toneHz = note == 38 ? 180.0f : 200.0f; r.tone2Ratio = 1.83f; r.tone2Gain = 0.7f; r.toneDecay = 0.1f; r.pitchAmount = 0.1f;
            r.noiseMode = 2; r.noiseHz = 1800.0f; r.noiseLevel = 0.65f; r.noiseDecay = 0.14f; r.clickLevel = 0.0f; break;
        case 37:
            r.toneHz = 455.0f; r.tone2Ratio = 3.66f; r.tone2Gain = 0.8f; r.toneDecay = 0.025f; r.toneLevel = 0.9f;
            r.noiseLevel = 0.0f; r.clickLevel = 0.1f; break;
        case 39: r.noiseHz = 1000.0f; r.noiseQ = 1.5f; r.noiseDecay = 0.25f; r.burstGapMs = 10.0f; break;
        case 42: case 44: case 46:
            r.metalScale = 1.0f; r.metalBpHz = 10000.0f; r.metalHpHz = 7500.0f; r.metalLevel = 0.6f; r.noiseLevel = 0.0f;
            r.metalDecay = note == 42 ? 0.04f : note == 44 ? 0.05f : 0.4f; r.clickLevel = 0.0f; break;
        case 41: case 43: case 45: case 47: case 48: case 50:
            r.toneHz *= 0.9f; r.tone2Gain = 0.0f; r.pitchAmount = 0.15f; r.pitchTime = 0.08f; r.toneDecay = 0.45f;
            r.noiseLevel = 0.0f; r.clickLevel = 0.05f; break;
        case 49: case 57: case 55: case 52:
            r.metalScale = 1.0f; r.metalLevel = 0.5f; r.metalBpHz = 7000.0f; r.metalHpHz = 4500.0f; r.noiseLevel = 0.15f; break;
        case 51: case 59:
            r.metalScale = 1.25f; r.metalBpHz = 8000.0f; r.metalLevel = 0.35f; r.toneLevel = 0.0f; r.metalDecay = 1.2f; break;
        case 70: r.noiseHz = 7000.0f; r.noiseDecay = 0.03f; break;
        case 75: r.toneDecay = 0.04f; r.clickLevel = 0.0f; break;
        default: break;
    }
}

/** The 909: snappy pitch-swept kick, noisy snare and hats. */
void apply909 (Recipe& r, int note)
{
    switch (note)
    {
        case 35: case 36:
            r.toneHz = note == 35 ? 47.0f : 52.0f; r.tone2Gain = 0.0f; r.toneDecay = 0.5f; r.pitchAmount = 2.5f; r.pitchTime = 0.012f;
            r.clickLevel = 0.7f; r.clickMs = 2.0f; r.toneDrive = 0.5f; r.noiseLevel = 0.1f; r.noiseHz = 1500.0f; r.noiseDecay = 0.02f; break;
        case 38: case 40:
            r.toneHz = note == 38 ? 200.0f : 225.0f; r.tone2Ratio = 1.75f; r.tone2Gain = 0.5f; r.toneDecay = 0.12f; r.toneLevel = 0.55f;
            r.noiseHz = 6000.0f; r.noiseQ = 0.4f; r.noiseLevel = 1.0f; r.noiseDecay = 0.2f; r.clickLevel = 0.3f; break;
        case 39: r.bursts = 4; r.burstGapMs = 8.0f; r.noiseHz = 1200.0f; r.noiseQ = 1.0f; r.noiseDecay = 0.2f; break;
        case 37: r.toneHz = 500.0f; r.tone2Ratio = 2.0f; r.toneDecay = 0.03f; r.clickLevel = 0.5f; r.noiseHz = 3000.0f; r.noiseLevel = 0.3f; break;
        case 42: case 44: case 46:
            r.metalLevel = 0.3f; r.metalScale = 1.6f; r.metalBpHz = 10000.0f; r.metalHpHz = 8000.0f;
            r.noiseHz = 9000.0f; r.noiseLevel = 0.55f; r.metalDecay = note == 46 ? 0.45f : 0.05f; r.noiseDecay = r.metalDecay; break;
        case 41: case 43: case 45: case 47: case 48: case 50:
            r.pitchAmount = 0.5f; r.pitchTime = 0.05f; r.toneDecay = 0.4f; r.tone2Gain = 0.1f;
            r.noiseLevel = 0.3f; r.noiseHz = 1500.0f; r.noiseDecay = 0.08f; break;
        case 49: case 57: case 55:
            r.noiseHz = 6000.0f; r.noiseLevel = 0.55f; r.metalLevel = 0.35f; r.metalScale = 1.4f; break;
        case 51: case 59:
            r.metalLevel = 0.35f; r.metalScale = 1.3f; r.metalBpHz = 9000.0f; r.noiseLevel = 0.1f; r.toneLevel = 0.05f; break;
        default: break;
    }
}

/** Electronic: saturated, zappy, tight. */
void applyElectronic (Recipe& r, int note)
{
    apply808 (r, note);
    switch (note)
    {
        case 35: case 36:
            r.toneHz = note == 35 ? 42.0f : 46.0f; r.toneDecay = 0.35f; r.pitchAmount = 3.0f; r.pitchTime = 0.02f;
            r.toneDrive = 0.9f; r.clickLevel = 0.4f; r.clickMs = 1.0f; break;
        case 38: case 40:
            r.toneHz = note == 38 ? 220.0f : 240.0f; r.toneDecay = 0.08f; r.toneDrive = 0.3f;
            r.noiseMode = 1; r.noiseHz = 3500.0f; r.noiseQ = 0.7f; r.noiseLevel = 1.0f; r.noiseDecay = 0.16f; break;
        case 39: r.noiseHz = 1500.0f; break;
        case 42: case 44: case 46:
            r.noiseMode = 2; r.noiseHz = 10000.0f; r.noiseLevel = 0.7f; r.metalLevel = 0.15f;
            r.metalDecay = note == 46 ? 0.3f : 0.03f; r.noiseDecay = r.metalDecay; break;
        case 41: case 43: case 45: case 47: case 48: case 50:
            r.pitchAmount = 1.0f; r.pitchTime = 0.04f; r.toneDecay = 0.3f; r.toneDrive = 0.4f; break;
        case 49: case 57: case 55: case 52:
            r.noiseHz = 7000.0f; r.noiseLevel = 0.6f; r.noiseDecay = 1.0f; r.metalLevel = 0.25f; r.metalDecay = 1.0f; break;
        default: break;
    }
}

/** Lo-Fi: a dusty acoustic kit with a boomy, clickless kick (the crunch is on the bus). */
void applyLofi (Recipe& r, int note)
{
    switch (note)
    {
        case 35: case 36: r.toneDecay = 0.55f; r.pitchAmount = 0.8f; r.clickLevel = 0.15f; r.toneDrive = 0.4f; break;
        case 38: case 40: r.noiseHz = 3500.0f; r.noiseDecay = 0.15f; r.toneLevel = 0.85f; break;
        case 42: case 44: case 46: r.metalBpHz = 7000.0f; r.metalHpHz = 5000.0f; r.metalDecay *= 0.8f; r.noiseDecay *= 0.8f; break;
        default: break;
    }
}

Recipe recipeFor (int note, int kit)
{
    auto r = studioRecipe (note);
    r.valid = r.toneLevel > 0.0f || r.noiseLevel > 0.0f || r.metalLevel > 0.0f;
    if (! r.valid) return r;

    switch (kit)
    {
        case tr808:      apply808 (r, note); break;
        case tr909:      apply909 (r, note); break;
        case electronic: applyElectronic (r, note); break;
        case lofi:       applyLofi (r, note); break;
        default:         break;
    }
    return r;
}

//==============================================================================
/** One drum piece. A retrigger fades the old hit out over ~2 ms first. */
class DrumVoice
{
public:
    void prepare (double sr)
    {
        sampleRate = sr;
        invSr = 1.0f / (float) sr;
        fadeStep = 1.0f / (float) (0.002 * sr);
    }

    void setSeed (uint32_t s)                   { noise.setSeed (s); }
    bool isActive() const                       { return running || pending; }
    void kill()                                 { running = pending = false; }

    void trigger (const Recipe& r, float newGain, float newPanL, float newPanR, float velocity)
    {
        next = r; nextGain = newGain; nextPanL = newPanL; nextPanR = newPanR; nextVelocity = velocity;
        if (running) { pending = true; fade = 1.0f; }
        else         begin();
    }

    /** Damps the hit away over `seconds` (hi-hat choke, all notes off). */
    void choke (float seconds)
    {
        pending = false;
        if (running)
            chokeCoeff = juce::jmin (chokeCoeff, std::exp (-6.9f / (seconds * (float) sampleRate)));
    }

    void render (float* left, float* right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if (pending)
            {
                fade -= fadeStep;
                if (fade <= 0.0f || ! running) begin();
            }
            if (! running) return;

            const float w = noise.next();
            float s = 0.0f;

            if (rc.toneLevel > 0.0f)
            {
                const float inc = toneHz * (1.0f + rc.pitchAmount * pitchEnv) * invSr;
                pitchEnv *= pitchCoeff;
                float t;
                if (rc.squareTone)
                    t = 0.5f * (sq1.next (inc, Osc::square) + rc.tone2Gain * sq2.next (inc * rc.tone2Ratio, Osc::square));
                else
                {
                    ph1 += inc;               ph1 -= std::floor (ph1);
                    ph2 += inc * rc.tone2Ratio; ph2 -= std::floor (ph2);
                    t = sinTurns (ph1) + rc.tone2Gain * tone2Env * sinTurns (ph2);
                    tone2Env *= tone2Coeff;
                }
                if (rc.toneDrive > 0.0f) t = saturate (t * (1.0f + 4.0f * rc.toneDrive)) * (1.0f - 0.3f * rc.toneDrive);
                if (rc.toneBpHz > 0.0f)  t = 0.8f * toneFilter.process (t, toneG, 0.7f).band;
                s += t * toneEnv * rc.toneLevel;
                toneEnv *= toneCoeff;
            }

            if (rc.noiseLevel > 0.0f)
            {
                if (burstsLeft > 0 && --burstTimer <= 0) { noiseEnv = 1.0f; --burstsLeft; burstTimer = burstGap; }
                const auto o = noiseFilter.process (w, noiseG, noiseK);
                const float nf = rc.noiseMode == 0 ? o.low : rc.noiseMode == 1 ? o.band * 1.6f : o.high;
                s += nf * noiseEnv * rc.noiseLevel;
                noiseEnv *= burstsLeft > 0 ? burstCoeff : noiseCoeff;
            }

            if (rc.metalLevel > 0.0f)
            {
                float m = 0.0f;
                for (size_t j = 0; j < metal.size(); ++j)
                    m += metal[j].next (metalInc[j], Osc::square);
                m = metalBp.process (m, metalBpG, 0.9f).band;
                m = metalHp.process (m, metalHpG, 1.2f).high;
                s += m * metalEnv * rc.metalLevel * 0.35f;
                metalEnv *= metalCoeff;
            }

            if (clickEnv > 1.0e-4f)
            {
                s += clickLp.process (w, 0.45f) * clickEnv * clickLevel;
                clickEnv *= clickCoeff;
            }

            if (attack < 1.0f) attack = juce::jmin (1.0f, attack + attackStep);
            s *= gain * attack * chokeGain * (pending ? fade : 1.0f);
            chokeGain *= chokeCoeff;

            left[i] += s * panL;
            right[i] += s * panR;

            if (chokeGain < 1.0e-4f
                || (toneEnv < 1.0e-4f && noiseEnv < 1.0e-4f && burstsLeft == 0 && metalEnv < 1.0e-4f && clickEnv < 1.0e-4f))
            {
                running = false;
                if (pending) begin();
            }
        }
    }

private:
    float coeff (float seconds) const           { return std::exp (-6.9f / (juce::jmax (0.001f, seconds) * (float) sampleRate)); }

    void begin()
    {
        rc = next;
        pending = false;
        running = true;
        gain = nextGain; panL = nextPanL; panR = nextPanR;
        const float v = nextVelocity;

        toneHz = rc.toneHz;
        toneEnv = rc.toneLevel > 0.0f ? 1.0f : 0.0f;
        tone2Env = 1.0f;
        toneCoeff = coeff (rc.toneDecay);
        tone2Coeff = coeff (rc.toneDecay * 0.6f);
        pitchEnv = 1.0f;
        pitchCoeff = std::exp (-1.0f / (juce::jmax (0.001f, rc.pitchTime) * (float) sampleRate));
        ph1 = ph2 = 0.0f;
        sq1.phase = sq2.phase = 0.0f;
        toneG = Svf::gFor (juce::jmax (20.0f, rc.toneBpHz), sampleRate);
        toneFilter.reset();

        noiseEnv = rc.noiseLevel > 0.0f ? 1.0f : 0.0f;
        noiseCoeff = coeff (rc.noiseDecay);
        burstCoeff = coeff (0.006f);
        burstsLeft = juce::jmax (0, rc.bursts - 1);
        burstGap = juce::jmax (1, (int) (rc.burstGapMs * 0.001f * (float) sampleRate));
        burstTimer = burstGap;
        noiseG = Svf::gFor (rc.noiseHz * (0.7f + 0.3f * v), sampleRate);
        noiseK = 1.0f / rc.noiseQ;
        noiseFilter.reset();

        metalEnv = rc.metalLevel > 0.0f ? 1.0f : 0.0f;
        metalCoeff = coeff (rc.metalDecay);
        static constexpr float metalHz[] { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };
        for (size_t j = 0; j < metal.size(); ++j)
        {
            metalInc[j] = metalHz[j] * rc.metalScale * invSr;
            metal[j].phase = noise.uniform();
        }
        metalBpG = Svf::gFor (rc.metalBpHz * (0.8f + 0.2f * v), sampleRate);
        metalHpG = Svf::gFor (rc.metalHpHz, sampleRate);
        metalBp.reset(); metalHp.reset();

        clickEnv = rc.clickLevel > 0.0f ? 1.0f : 0.0f;
        clickLevel = rc.clickLevel * (0.3f + 0.7f * v);
        clickCoeff = coeff (rc.clickMs * 0.001f);
        clickLp.reset();

        attack = 0.0f;
        attackStep = 1.0f / juce::jmax (1.0f, rc.attackMs * 0.001f * (float) sampleRate);
        chokeGain = 1.0f;
        chokeCoeff = 1.0f;
        fade = 1.0f;
    }

    double sampleRate = 48000.0;
    float invSr = 1.0f / 48000.0f, fadeStep = 0.01f, fade = 1.0f;
    bool running = false, pending = false;
    Recipe rc, next;
    float gain = 0.0f, panL = 1.0f, panR = 1.0f, nextGain = 0.0f, nextPanL = 1.0f, nextPanR = 1.0f, nextVelocity = 1.0f;
    Noise noise;

    float toneHz = 100.0f, toneEnv = 0.0f, toneCoeff = 0.0f, tone2Env = 0.0f, tone2Coeff = 0.0f, pitchEnv = 0.0f, pitchCoeff = 0.0f;
    float ph1 = 0.0f, ph2 = 0.0f, toneG = 0.1f;
    Osc sq1, sq2;
    Svf toneFilter;

    float noiseEnv = 0.0f, noiseCoeff = 0.0f, burstCoeff = 0.0f, noiseG = 0.1f, noiseK = 1.0f;
    int burstsLeft = 0, burstGap = 1, burstTimer = 1;
    Svf noiseFilter;

    std::array<Osc, 6> metal;
    std::array<float, 6> metalInc {};
    float metalEnv = 0.0f, metalCoeff = 0.0f, metalBpG = 0.1f, metalHpG = 0.1f;
    Svf metalBp, metalHp;

    float clickEnv = 0.0f, clickLevel = 0.0f, clickCoeff = 0.0f;
    OnePoleLp clickLp;

    float attack = 1.0f, attackStep = 1.0f, chokeGain = 1.0f, chokeCoeff = 1.0f;
};

//==============================================================================
// Drum Kit: one synthesised voice per General MIDI drum note, five kits, a shared room.
class DrumKit final : public Instrument
{
public:
    static constexpr int firstNote = 27, numNotes = 61;

    enum { kitKnob, kickTune, kickDecay, snareTune, snappy, hatDecay, tomTune, tone, room,
           kickLevel, snareLevel, hatLevel, tomLevel, cymbalLevel, percLevel, width, velocity, drive, level };

    void reset() override
    {
        for (auto& v : voices) v.kill();
        roomFx.reset();
        for (auto& t : tilt) t.reset();
        holdCount = 0;
        held = { 0.0f, 0.0f };
        for (auto& c : crushLp) c.reset();
    }

    void update() override
    {
        params.kit = juce::jlimit (0, 4, juce::roundToInt (knob (kitKnob)));
        params.kickTune = semitones (knob (kickTune));
        params.kickDecay = std::pow (2.5f, (knob (kickDecay) - 0.5f) * 2.0f);
        params.snareTune = semitones (knob (snareTune));
        params.snappy = knob (snappy);
        params.hatDecay = std::pow (2.5f, (knob (hatDecay) - 0.5f) * 2.0f);
        params.tomTune = semitones (knob (tomTune));
        params.width = knob (width);
        params.velSens = knob (velocity);
        for (int s = 0; s < numSections; ++s)
            params.levels[(size_t) s] = dbToGain (knob (kickLevel + s));

        tiltAmount = knob (tone);
        roomAmount = knob (room);
        driveAmount = knob (drive);
        outGain = dbToGain (knob (level));
        roomFx.setDecay (params.kit == tr808 || params.kit == electronic ? 0.9f : 1.1f, params.kit == lofi ? 3500.0f : 6000.0f);
    }

    void render (float* left, float* right, int numSamples, const NoteEvent* events, int numEvents) override
    {
        const int chunk = (int) scratchL.size();
        int start = 0, e = 0;

        while (chunk > 0 && start < numSamples)
        {
            const int n = juce::jmin (chunk, numSamples - start);
            std::fill (scratchL.begin(), scratchL.begin() + n, 0.0f);
            std::fill (scratchR.begin(), scratchR.begin() + n, 0.0f);

            int done = 0;
            for (; e < numEvents && events[e].offset < start + n; ++e)
            {
                const int until = juce::jlimit (done, n, events[e].offset - start);
                renderVoices (done, until);
                done = until;
                handle (events[e]);
            }
            renderVoices (done, n);

            master (scratchL.data(), scratchR.data(), n);
            juce::FloatVectorOperations::add (left + start, scratchL.data(), n);
            juce::FloatVectorOperations::add (right + start, scratchR.data(), n);
            start += n;
        }

        for (; e < numEvents; ++e)
            handle (events[e]);
    }

protected:
    void prepared() override
    {
        scratchL.assign ((size_t) juce::jmax (64, maxBlock), 0.0f);
        scratchR.assign ((size_t) juce::jmax (64, maxBlock), 0.0f);
        for (size_t i = 0; i < voices.size(); ++i)
        {
            voices[i].prepare (sampleRate);
            voices[i].setSeed ((uint32_t) i + 101u);
        }
        roomFx.prepare (sampleRate);
        tiltCoeff = onePole (1200.0f, sampleRate);
        crushCoeff = onePole (6000.0f, sampleRate);
    }

private:
    void renderVoices (int from, int to)
    {
        if (to <= from) return;
        for (auto& v : voices)
            if (v.isActive())
                v.render (scratchL.data() + from, scratchR.data() + from, to - from);
    }

    DrumVoice* voiceFor (int note)
    {
        const int i = note - firstNote;
        return i >= 0 && i < numNotes ? &voices[(size_t) i] : nullptr;
    }

    void handle (const NoteEvent& ev)
    {
        if (ev.kind == NoteEvent::allNotesOff)
        {
            for (auto& v : voices) v.choke (0.08f);
            return;
        }
        if (ev.kind != NoteEvent::noteOn || ev.value <= 0.0f)
            return;

        auto* voice = voiceFor (ev.note);
        if (voice == nullptr) return;

        auto r = recipeFor (ev.note, params.kit);
        if (! r.valid) return;

        // Hi-hat choke group; the mute triangle chokes the open one.
        if (ev.note == 42 || ev.note == 44) if (auto* open = voiceFor (46)) open->choke (0.03f);
        if (ev.note == 80)                   if (auto* open = voiceFor (81)) open->choke (0.03f);

        const float vel = juce::jlimit (0.0f, 1.0f, ev.value);
        switch (r.section)
        {
            case kickSection:   r.toneHz *= params.kickTune; r.toneDecay *= params.kickDecay; break;
            case snareSection:
                if (ev.note != 39) r.toneHz *= params.snareTune;
                r.noiseLevel *= 0.3f + 1.4f * params.snappy;
                r.noiseDecay *= 0.6f + 0.8f * params.snappy;
                break;
            case hatSection:    r.metalDecay *= params.hatDecay; r.noiseDecay *= params.hatDecay; break;
            case tomSection:    r.toneHz *= params.tomTune; break;
            default: break;
        }

        const float g = 0.6f * r.gain * params.levels[(size_t) r.section]
                      * ((1.0f - params.velSens) + params.velSens * std::pow (vel, 1.4f));
        const auto [pl, pr] = panGains (r.pan * params.width);
        voice->trigger (r, g, pl, pr, vel);
    }

    void master (float* l, float* r, int n)
    {
        roomFx.process (l, r, n, roomAmount * 0.35f);
        const float driveGain = 1.0f + 3.0f * driveAmount;
        const float driveNorm = 1.0f / (1.0f + driveAmount);
        const bool crush = params.kit == lofi;

        for (int i = 0; i < n; ++i)
        {
            float x[2] { l[i], r[i] };
            for (size_t c = 0; c < 2; ++c)
            {
                const float lp = tilt[c].process (x[c], tiltCoeff);
                x[c] = lp + (1.0f + tiltAmount) * (x[c] - lp);
                if (driveAmount > 0.0f) x[c] = saturate (x[c] * driveGain) * driveNorm;
            }

            if (crush)
            {
                // ~12 kHz sample-and-hold, 10-bit steps, then a dull, saturated top.
                if (holdCount-- <= 0) { holdCount = 3; held = { std::round (x[0] * 512.0f) / 512.0f, std::round (x[1] * 512.0f) / 512.0f }; }
                for (size_t c = 0; c < 2; ++c)
                    x[c] = saturate (1.3f * crushLp[c].process (held[c], crushCoeff)) * 0.85f;
            }

            l[i] = safetyClip (x[0] * outGain);
            r[i] = safetyClip (x[1] * outGain);
        }
    }

    std::array<DrumVoice, (size_t) numNotes> voices;
    std::vector<float> scratchL, scratchR;
    KitParams params;
    Room roomFx;
    std::array<OnePoleLp, 2> tilt, crushLp;
    std::array<float, 2> held {};
    int holdCount = 0;
    float tiltCoeff = 0.1f, crushCoeff = 0.5f, tiltAmount = 0.0f, roomAmount = 0.0f, driveAmount = 0.0f, outGain = 1.0f;
};

} // namespace

void registerDrumInstruments (InstrumentRegistry& r)
{
    using P = InstrumentInfo::Patch;

    r.add<DrumKit> ({ "drums.kit", "Drum Kit", "Drums",
                      "General MIDI drum kit, synthesised: acoustic studio kit, 808, 909, electronic and lo-fi",
                      juce::Colour (0xffc0463a),
                      { choice ("kit", "Kit", { "Studio", "808", "909", "Electronic", "Lo-Fi" }, 0),
                        knob ("kicktune", "Kick Tune", -12.0f, 12.0f, 0.0f, "st"),
                        knob ("kickdecay", "Kick Decay", 0.0f, 1.0f, 0.5f, "%"),
                        knob ("snaretune", "Snare Tune", -12.0f, 12.0f, 0.0f, "st"),
                        knob ("snappy", "Snappy", 0.0f, 1.0f, 0.5f, "%"),
                        knob ("hatdecay", "Hat Decay", 0.0f, 1.0f, 0.5f, "%"),
                        knob ("tomtune", "Tom Tune", -12.0f, 12.0f, 0.0f, "st"),
                        knob ("tone", "Tone", -1.0f, 1.0f, 0.0f, "%"),
                        knob ("room", "Room", 0.0f, 1.0f, 0.2f, "%"),
                        knob ("kick", "Kick", -24.0f, 6.0f, 0.0f, "dB"),
                        knob ("snare", "Snare", -24.0f, 6.0f, 0.0f, "dB"),
                        knob ("hats", "Hats", -24.0f, 6.0f, 0.0f, "dB"),
                        knob ("toms", "Toms", -24.0f, 6.0f, 0.0f, "dB"),
                        knob ("cymbals", "Cymbals", -24.0f, 6.0f, 0.0f, "dB"),
                        knob ("perc", "Percussion", -24.0f, 6.0f, 0.0f, "dB"),
                        knob ("width", "Width", 0.0f, 1.0f, 0.7f, "%"),
                        knob ("velocity", "Velocity", 0.0f, 1.0f, 0.8f, "%"),
                        knob ("drive", "Drive", 0.0f, 1.0f, 0.0f, "%"),
                        knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") },
                      { P { "Studio Kit", "Acoustic", {} },
                        P { "Rock Room", "Acoustic", { { "kickdecay", 0.6f }, { "snaretune", -2.0f }, { "snappy", 0.65f }, { "tomtune", -2.0f },
                                                       { "room", 0.55f }, { "tone", 0.15f }, { "drive", 0.25f } } },
                        P { "Tight Funk", "Acoustic", { { "kickdecay", 0.25f }, { "snaretune", 4.0f }, { "snappy", 0.7f }, { "hatdecay", 0.3f },
                                                        { "room", 0.08f }, { "tone", 0.2f } } },
                        P { "808", "Electronic", { { "kit", 1.0f }, { "room", 0.1f } } },
                        P { "808 Boom", "Electronic", { { "kit", 1.0f }, { "kickdecay", 0.8f }, { "kicktune", -2.0f }, { "kick", 0.0f },
                                                        { "drive", 0.2f }, { "room", 0.05f } } },
                        P { "909", "Electronic", { { "kit", 2.0f }, { "room", 0.12f }, { "snappy", 0.6f } } },
                        P { "Electro", "Electronic", { { "kit", 3.0f }, { "room", 0.15f }, { "drive", 0.3f } } },
                        P { "Lo-Fi Dusty", "Lo-Fi", { { "kit", 4.0f }, { "room", 0.25f }, { "tone", -0.3f }, { "hatdecay", 0.35f }, { "drive", 0.3f } } } } });
}

} // namespace woodshed
