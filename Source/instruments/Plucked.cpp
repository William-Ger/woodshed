#include "InstrumentDsp.h"

namespace woodshed
{
namespace
{
using namespace idsp;

inline float msToS (float v)                    { return v * 0.001f; }

/** Per-sample multiplier that decays 60 dB in `seconds`. */
inline float t60Mul (float seconds, double sr)  { return std::exp (-6.9078f / (juce::jmax (0.001f, seconds) * (float) sr)); }

constexpr float lowestNote = 17.0f;             // lowest pitch a string can be tuned to, bend included

//==============================================================================
/**
    One plucked-string waveguide: a delay line closed through a one-pole loop lowpass.
    The loop is tuned with the lowpass's phase delay and its gain set from the wanted
    fundamental decay, so every harmonic decays faster than the one below it.
*/
struct Waveguide
{
    dsp::DelayBuffer line;
    std::vector<float> excitation;
    int excitePos = 0, exciteLen = 0;
    float delay = 100.0f, targetDelay = 100.0f, step = 0.0f, loopGain = 0.99f, loopCoeff = 0.5f, lp = 0.0f;

    void prepare (int maxPeriod)
    {
        line.setMaxSamples (maxPeriod + 8);
        excitation.assign ((size_t) maxPeriod + 8, 0.0f);
    }

    void clear()                                { line.clear(); lp = 0.0f; excitePos = exciteLen = 0; step = 0.0f; }
    bool exciting() const                       { return excitePos < exciteLen; }

    /** hz: pitch; t60: fundamental decay in seconds; cutoffHz: loop lowpass. */
    void tune (float hz, float t60, float cutoffHz, double sr, bool jump)
    {
        const float fs = (float) sr;
        const float f = juce::jlimit (noteToHz (lowestNote), fs * 0.12f, hz);
        // A one-pole loop filter costs the fundamental ~0.5/k^2 per period at cutoff k*f, so keep
        // the cutoff high enough for the wanted decay; below that it only shapes the overtones.
        const float minCutoff = 1.3f * f * std::sqrt (juce::jmax (1.0f, t60 * f / 13.8f));
        loopCoeff = onePole (juce::jmax (minCutoff, cutoffHz), sr);
        const float w = twoPi * f / fs;
        const float b = 1.0f - loopCoeff;
        const float re = 1.0f - b * std::cos (w), im = b * std::sin (w);
        const float phaseDelay = std::atan2 (im, re) / w;
        const float mag = loopCoeff / std::sqrt (re * re + im * im);
        const float period = fs / f;
        targetDelay = juce::jlimit (2.0f, (float) excitation.size() - 8.0f, period - 1.0f - phaseDelay);
        const float perPeriod = std::pow (10.0f, -3.0f * period / (juce::jmax (0.02f, t60) * fs));
        loopGain = juce::jmin (0.99995f, perPeriod / mag);
        if (jump) { delay = targetDelay; step = 0.0f; }
    }

    void rampOver (int n)                       { step = (targetDelay - delay) / (float) juce::jmax (1, n); }

    /** Fills the excitation: the plucked string's velocity shape (from a triangular pluck) mixed with noise notched at the
        pluck position, plus an optional pick click, all lowpassed by how hard it was plucked. */
    void pluck (Noise& noise, int length, float cutHz, float noiseAmount, float click, float position, float amp, int predelay, double sr)
    {
        const int len = juce::jlimit (8, (int) excitation.size(), length);
        float s = 0.0f;
        for (int k = 0; k < len; ++k)
        {
            s += 0.5f * (noise.next() - s);
            excitation[(size_t) k] = s;
        }
        normalise (len);

        const int m = juce::jlimit (1, len - 1, juce::roundToInt (position * (float) len));
        for (int k = len - 1; k >= m; --k)
            excitation[(size_t) k] -= excitation[(size_t) (k - m)];
        normalise (len);

        const float apex = (float) m;
        for (int k = 0; k < len; ++k)
        {
            const float x = (float) k;
            // The string's velocity wave: the derivative of a triangle with its apex at the pluck point.
            const float slope = x < apex ? 1.0f : -apex / juce::jmax (1.0f, (float) len - apex);
            excitation[(size_t) k] = noiseAmount * excitation[(size_t) k] + (1.0f - noiseAmount) * slope;
        }

        for (int k = 0; k < juce::jmin (3, len); ++k)
            excitation[(size_t) k] += click * (k == 1 ? 1.5f : 0.75f);

        const float c = onePole (cutHz, sr);
        s = 0.0f;
        for (int k = 0; k < len; ++k)
        {
            s += c * (excitation[(size_t) k] - s);
            excitation[(size_t) k] = s;
        }
        normalise (len);
        for (int k = 0; k < len; ++k) excitation[(size_t) k] *= amp;
        exciteLen = len;
        excitePos = -juce::jmax (0, predelay);
    }

    float tick()
    {
        delay += step;
        const float y = line.read (delay);
        lp += loopCoeff * (y - lp);
        float v = lp * loopGain;
        if (excitePos < exciteLen)
        {
            if (excitePos >= 0) v += excitation[(size_t) excitePos];
            ++excitePos;
        }
        v = juce::jlimit (-4.0f, 4.0f, v);
        line.push (v);
        return v;
    }

    /** The string `d` samples ago (d >= 2). */
    float past (float d) const                  { return line.read (d); }

private:
    /** Removes the mean and scales the first `len` samples to a peak of 1. */
    void normalise (int len)
    {
        float mean = 0.0f, peak = 1.0e-6f;
        for (int k = 0; k < len; ++k) mean += excitation[(size_t) k];
        mean /= (float) len;
        for (int k = 0; k < len; ++k) { excitation[(size_t) k] -= mean; peak = juce::jmax (peak, std::abs (excitation[(size_t) k])); }
        for (int k = 0; k < len; ++k) excitation[(size_t) k] /= peak;
    }
};

//==============================================================================
/** What a string voice reads from its owner, derived once per block. */
struct StringParams
{
    float exciteCut = 3000.0f, noise = 0.3f, click = 0.0f, position = 0.15f, harmonics = 12.0f, loopMaxHz = 8000.0f;
    float t60 = 4.0f, t60Slope = 0.5f, releaseT60 = 0.15f, pitchDrop = 0.0f;
    float pickup = 0.0f, pickupPos = 0.2f, sub = 0.0f, snap = 0.0f;
    float strum = 0.005f, velSens = 0.6f, gain = 0.15f, glide = 0.0f, vibrato = 0.0f, widthAmt = 0.5f;
    int doubling = 0;                           // 0 off, 1 unison, 2 octave
    float doubleMix = 0.6f;
    bool mono = false;
};

/** A plucked string (optionally doubled as a course), with pickup comb, sub and slap snap. */
class StringVoice
{
public:
    void prepare (double sr)
    {
        sampleRate = sr;
        starting.prepare (sr);
        const int maxPeriod = (int) std::ceil (sr / noteToHz (lowestNote)) + 4;
        strings[0].prepare (maxPeriod);
        strings[1].prepare (maxPeriod);
        noise.setSeed ((uint32_t) (reinterpret_cast<uintptr_t> (this) >> 4));
    }

    void start (int newNote, float velocity, bool stolen)  { starting.request (newNote, velocity, stolen); releasePending = false; }
    void release()                                       { if (starting.pending) releasePending = true; released = true; }
    void kill()                                          { starting.clear(); active = false; released = releasePending = false; }
    bool isActive() const                                { return starting.pending || active; }

    template <typename Owner>
    void render (float* left, float* right, int n, const Owner& owner)
    {
        const StringParams& p = owner.params;
        const float sr = (float) sampleRate;

        // Pitch at the end of this block; the delay lines ramp to it.
        if (p.glide > 0.001f)
            currentNote = targetNote + (currentNote - targetNote) * std::pow (1.0f - 1.0f / (p.glide * 0.35f * sr), (float) n);
        else
            currentNote = targetNote;
        drop *= std::pow (dropMul, (float) n);
        lfoPhase = wrap01 (lfoPhase + 5.2f * (float) n / sr);
        const float vib = (p.vibrato + owner.mod() * 0.35f) * sinTurns (lfoPhase);
        bend = owner.bend();
        if (active) retune (p, false);
        for (auto& s : strings) s.rampOver (n);

        const float subInc = noteToHz (currentNote + bend + vib) / sr;
        const float subMul = t60Mul (released ? juce::jmin (p.releaseT60, subT60) : subT60, sampleRate);
        const float pickupDelay = juce::jmax (2.0f, p.pickupPos * strings[0].targetDelay);
        float blockPeak = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            if (starting.pending)
            {
                const bool live = active && starting.fading;
                if (live && starting.note == note)                     repluck (p);
                else if (live && p.mono && p.glide > 0.001f)            slide (p);
                else if (starting.tick())                               begin (p);
            }
            if (! active && ! starting.pending) break;
            if (! active) continue;

            float y = strings[0].tick();
            if (p.doubling > 0) y += p.doubleMix * strings[1].tick();
            if (p.pickup > 0.0f) y -= p.pickup * strings[0].past (pickupDelay);

            if (p.sub > 0.0f)
            {
                subLevel += 0.004f * (1.0f - subLevel);
                subEnv *= subMul;
                subPhase = wrap01 (subPhase + subInc);
                y += p.sub * subEnv * subLevel * sinTurns (subPhase);
            }

            if (snapEnv > 1.0e-4f)
            {
                y += snapEnv * snapFilter.process (noise.next(), Svf::gFor (3200.0f, sampleRate), 2.0f).band;
                snapEnv *= snapMul;
            }

            float out = dc.process (y) * gain;
            if (starting.fading) out *= juce::jmax (0.0f, starting.fade);
            left[i] += out * panL;
            right[i] += out * panR;
            blockPeak = juce::jmax (blockPeak, std::abs (out));
        }

        if (active && ! starting.pending && ! strings[0].exciting() && blockPeak < 2.0e-6f)
            active = false;
    }

private:
    float noteT60 (const StringParams& p) const
    {
        return p.t60 * std::exp2 (-(currentNote - 48.0f) / 12.0f * p.t60Slope);
    }

    void retune (const StringParams& p, bool jump)
    {
        const float pitch = currentNote + bend + drop;
        const float nat = noteT60 (p);
        const float t60 = released ? juce::jmin (nat, p.releaseT60) : nat;
        const float dark = released ? 0.6f : 1.0f;
        const float hz = noteToHz (pitch);
        strings[0].tune (hz, t60, juce::jmin (p.loopMaxHz, juce::jmax (1200.0f, hz * p.harmonics)) * dark, sampleRate, jump);

        if (p.doubling > 0)
        {
            const float hz2 = noteToHz (pitch + (p.doubling == 2 ? 12.0f : 0.09f));
            strings[1].tune (hz2, t60 * 0.8f, juce::jmin (p.loopMaxHz, juce::jmax (1200.0f, hz2 * p.harmonics)) * dark, sampleRate, jump);
        }
        subT60 = nat;
    }

    /** spread: random onset up to that many seconds, so simultaneous notes don't hit as one spike. */
    void excite (const StringParams& p, float vel, float amp, float spread)
    {
        const float cut = p.exciteCut * (0.2f + 1.1f * vel * vel);
        amp *= noise.next() < 0.0f ? -1.0f : 1.0f;
        const int predelay = (int) (noise.uniform() * spread * (float) sampleRate);
        strings[0].pluck (noise, juce::roundToInt (strings[0].targetDelay), cut, p.noise, p.click * vel, p.position, amp, predelay, sampleRate);
        if (p.doubling > 0)
            strings[1].pluck (noise, juce::roundToInt (strings[1].targetDelay), cut * 1.2f, p.noise, p.click * vel, p.position * 1.3f, amp, predelay + 40, sampleRate);
        snapEnv = p.snap * vel * vel * amp;
        snapMul = t60Mul (0.03f, sampleRate);
    }

    void begin (const StringParams& p)
    {
        const bool glideFrom = active && p.mono && p.glide > 0.001f;
        note = starting.note;
        targetNote = (float) note;
        if (! glideFrom) currentNote = targetNote;
        const float vel = starting.velocity;
        velGain = velocityGain (vel, p.velSens);
        gain = p.gain * velGain;
        drop = p.pitchDrop * (0.3f + 0.7f * vel);
        dropMul = t60Mul (0.25f, sampleRate);

        for (auto& s : strings) s.clear();
        dc.reset();
        snapFilter.reset();
        released = releasePending;
        releasePending = false;
        retune (p, true);
        excite (p, vel, 1.0f, p.strum);
        subEnv = 1.0f;
        subLevel = 0.0f;
        subPhase = 0.0f;

        std::tie (panL, panR) = panGains (juce::jlimit (-1.0f, 1.0f, (targetNote - 60.0f) / 30.0f) * 0.6f * p.widthAmt);
        active = true;
        starting.clear();
    }

    /** The same key again: pluck the ringing string, keeping its vibration. */
    void repluck (const StringParams& p)
    {
        const float vel = starting.velocity;
        released = releasePending;
        releasePending = false;
        retune (p, false);
        excite (p, vel, velocityGain (vel, p.velSens) / juce::jmax (0.05f, velGain), 0.0f);
        subEnv = juce::jmax (subEnv, 1.0f);
        starting.clear();
    }

    /** Mono legato with glide: slide to the new note with a light re-pluck. */
    void slide (const StringParams& p)
    {
        note = starting.note;
        targetNote = (float) note;
        released = releasePending;
        releasePending = false;
        excite (p, starting.velocity * 0.6f, 0.25f, 0.0f);
        subEnv = 1.0f;
        starting.clear();
    }

    double sampleRate = 48000.0;
    PendingStart starting;
    std::array<Waveguide, 2> strings;
    Noise noise;
    DcBlock dc;
    Svf snapFilter;
    int note = -1;
    float currentNote = 60.0f, targetNote = 60.0f, bend = 0.0f, drop = 0.0f, dropMul = 1.0f, lfoPhase = 0.0f;
    float gain = 0.0f, velGain = 1.0f, panL = 1.0f, panR = 1.0f;
    float subEnv = 0.0f, subLevel = 0.0f, subPhase = 0.0f, subT60 = 1.0f, snapEnv = 0.0f, snapMul = 0.0f;
    bool active = false, released = false, releasePending = false;
};

//==============================================================================
/** A small fixed body: three resonances added to the dry signal. */
struct Body
{
    std::array<Svf, 6> bands;
    std::array<float, 3> freqs { 100.0f, 200.0f, 400.0f };

    void reset()                                { for (auto& b : bands) b.reset(); }

    void process (float* left, float* right, int n, float amount, double sr)
    {
        if (amount <= 0.001f) return;
        float g[3];
        for (size_t k = 0; k < 3; ++k) g[k] = Svf::gFor (freqs[k], sr);
        const float gains[] { 1.0f, 0.7f, 0.45f };
        for (int i = 0; i < n; ++i)
        {
            float bl = 0.0f, br = 0.0f;
            for (size_t k = 0; k < 3; ++k)
            {
                bl += gains[k] * bands[k].process (left[i], g[k], 0.18f).band;
                br += gains[k] * bands[k + 3].process (right[i], g[k], 0.18f).band;
            }
            left[i] += amount * bl;
            right[i] += amount * br;
        }
    }
};

//==============================================================================
// Plucked Strings: waveguide nylon, steel, harp and koto strings through a small body.
class PluckedStrings final : public PolyInstrument<PluckedStrings, StringVoice, 16>
{
public:
    enum { string, bright, position, decay, release, mute, body, doubling, vel, vibrato, reverb, width, level };

    StringParams params;

    void update() override
    {
        struct Material { float cut, noise, click, harmonics, loopMax, t60, slope, drop, trimDb; std::array<float, 3> body; };
        static const Material materials[] {
            { 2400.0f, 0.25f, 0.10f, 10.0f,  6000.0f, 4.5f, 0.45f, 0.03f, 1.0f, { 98.0f, 204.0f, 390.0f } },     // nylon
            { 5000.0f, 0.40f, 0.35f, 28.0f, 12000.0f, 6.0f, 0.40f, 0.08f, 3.5f, { 110.0f, 225.0f, 460.0f } },    // steel
            { 1500.0f, 0.15f, 0.00f, 14.0f,  7000.0f, 6.5f, 0.50f, 0.00f, -6.0f, { 150.0f, 320.0f, 600.0f } },    // harp
            { 6000.0f, 0.45f, 0.60f, 40.0f, 14000.0f, 3.5f, 0.50f, 0.35f, 6.0f, { 140.0f, 300.0f, 650.0f } } };  // koto
        const auto& m = materials[juce::jlimit (0, 3, juce::roundToInt (knob (string)))];

        auto& p = params;
        const float b = knob (bright) - 0.5f, mt = knob (mute);
        p.exciteCut = m.cut * std::exp2 (b * 3.0f) * (1.0f - 0.6f * mt);
        p.noise = m.noise;
        p.click = m.click * (1.0f - mt);
        p.harmonics = m.harmonics * std::exp2 (b * 2.0f) * (1.0f - 0.75f * mt);
        p.loopMaxHz = m.loopMax * (1.0f - 0.6f * mt);
        p.position = knob (position);
        p.t60 = m.t60 * knob (decay) * (1.0f - 0.93f * mt);
        p.t60Slope = m.slope;
        p.releaseT60 = msToS (knob (release));
        p.pitchDrop = m.drop;
        p.doubling = juce::roundToInt (knob (doubling));
        p.doubleMix = 0.6f;
        p.velSens = knob (vel);
        p.vibrato = knob (vibrato) * 0.4f;
        p.widthAmt = knob (width);
        p.strum = 0.012f;
        p.gain = 0.17f * dbToGain (knob (level) + m.trimDb + 2.0f * mt) * (p.doubling > 0 ? 0.9f : 1.0f);
        bodyFx.freqs = m.body;
        bodyAmount = knob (body) * 0.8f;
        reverbAmount = knob (reverb) * 0.5f;
    }

protected:
    void prepareEffects() override                      { room.prepare (sampleRate); room.setDecay (1.4f, 5000.0f); }
    void resetEffects() override                        { room.reset(); bodyFx.reset(); }
    void processEffects (float* l, float* r, int n) override
    {
        bodyFx.process (l, r, n, bodyAmount, sampleRate);
        room.process (l, r, n, reverbAmount);
        safetyClip (l, r, n);
    }

private:
    Body bodyFx;
    Room room;
    float bodyAmount = 0.0f, reverbAmount = 0.0f;
};

//==============================================================================
// Electric Bass: waveguide string read through a pickup comb, then tone and drive.
class ElectricBass final : public PolyInstrument<ElectricBass, StringVoice, 8>
{
public:
    enum { style, tone, pickup, decay, release, sub, drive, mono, glide, vel, level };

    StringParams params;

    bool isMono() const override                        { return knob (mono) >= 0.5f; }

    void update() override
    {
        struct Style { float cut, noise, click, harmonics, loopMax, t60, drop, snap, trimDb; };
        static const Style styles[] {
            { 1400.0f, 0.15f, 0.05f, 14.0f, 4000.0f, 5.0f, 0.02f, 0.0f, 0.0f },     // finger
            { 4000.0f, 0.35f, 0.60f, 20.0f, 7000.0f, 4.0f, 0.05f, 0.0f, 3.0f },     // pick
            { 7000.0f, 0.50f, 1.00f, 30.0f, 9000.0f, 3.0f, 0.20f, 0.8f, 5.0f },     // slap
            {  700.0f, 0.20f, 0.10f,  4.0f, 1500.0f, 0.35f, 0.0f, 0.0f, 5.0f } };   // muted
        const auto& s = styles[juce::jlimit (0, 3, juce::roundToInt (knob (style)))];

        auto& p = params;
        p.exciteCut = s.cut;
        p.noise = s.noise;
        p.click = s.click;
        p.harmonics = s.harmonics;
        p.loopMaxHz = s.loopMax;
        p.position = 0.12f;
        p.t60 = s.t60 * knob (decay);
        p.t60Slope = 0.4f;
        p.releaseT60 = msToS (knob (release));
        p.pitchDrop = s.drop;
        p.snap = s.snap;
        p.pickup = 0.85f;
        p.pickupPos = knob (pickup);
        p.sub = knob (sub) * 0.8f;
        p.mono = isMono();
        p.glide = p.mono ? msToS (knob (glide)) : 0.0f;
        p.velSens = knob (vel);
        p.widthAmt = 0.0f;
        p.doubling = 0;
        driveAmount = knob (drive);
        p.strum = 0.006f;
        p.gain = 0.2f * dbToGain (knob (level) + s.trimDb) / (1.0f + 0.3f * driveAmount);
        toneSmooth.set (knob (tone));
    }

protected:
    void prepareEffects() override                      { toneSmooth.prepare (sampleRate, 0.03f); }
    void resetEffects() override                        { toneSmooth.reset(); for (auto& f : toneFilters) f.reset(); }
    void processEffects (float* l, float* r, int n) override
    {
        const float pre = 1.0f + 5.0f * driveAmount;
        for (int i = 0; i < n; ++i)
        {
            const float g = Svf::gFor (toneSmooth.next(), sampleRate);
            float x = toneFilters[0].process (l[i], g, 0.9f).low;
            float y = toneFilters[1].process (r[i], g, 0.9f).low;
            if (driveAmount > 0.001f) { x = saturate (x * pre) / pre * (1.0f + driveAmount); y = saturate (y * pre) / pre * (1.0f + driveAmount); }
            l[i] = x;
            r[i] = y;
        }
        safetyClip (l, r, n);
    }

private:
    std::array<Svf, 2> toneFilters;
    Smooth toneSmooth;
    float driveAmount = 0.0f;
};

//==============================================================================
/** What a mallet voice reads from its owner. */
struct MalletParams
{
    int model = 0;
    float hardness = 0.5f, decay = 1.0f, damperT60 = 1.0f, click = 0.3f, resonator = 0.5f;
    float velSens = 0.7f, widthAmt = 0.5f, transpose = 0.0f, gain = 0.1f;
};

/** Mode tables: frequency ratio, amplitude, relative decay; plus the fundamental's decay at C4. */
struct MalletModel
{
    int count;
    float ratio[8], amp[8], decay[8];
    float t60C4, slope, resonance;
};

const MalletModel malletModels[] {
    // Marimba: rosewood bar tuned 1 : 3.93 : 9.24, upper modes die fast; tube resonator.
    { 4, { 1.0f, 3.93f, 9.24f, 13.6f }, { 1.0f, 0.35f, 0.12f, 0.05f }, { 1.0f, 0.3f, 0.12f, 0.06f }, 1.3f, 0.6f, 1.0f },
    // Vibraphone: aluminium bar tuned 1 : 4 : 10, long ring.
    { 4, { 1.0f, 4.0f, 10.0f, 15.1f }, { 1.0f, 0.3f, 0.09f, 0.03f }, { 1.0f, 0.45f, 0.22f, 0.1f }, 5.5f, 0.45f, 0.7f },
    // Glockenspiel: free steel bar 1 : 2.756 : 5.404 : 8.933.
    { 4, { 1.0f, 2.756f, 5.404f, 8.933f }, { 1.0f, 0.5f, 0.28f, 0.14f }, { 1.0f, 0.6f, 0.4f, 0.25f }, 4.0f, 0.35f, 0.0f },
    // Celesta: struck plates over wooden resonators; nearly harmonic, a fast inharmonic tine.
    { 5, { 1.0f, 2.0f, 3.0f, 5.83f, 8.1f }, { 1.0f, 0.22f, 0.07f, 0.25f, 0.08f }, { 1.0f, 0.5f, 0.35f, 0.04f, 0.03f }, 2.2f, 0.5f, 0.5f },
    // Tubular bells: free-free tube modes; 2 : 3 : 4.17 give the strike note at the played pitch.
    { 8, { 0.617f, 1.21f, 2.0f, 2.99f, 4.17f, 5.56f, 7.14f, 8.9f }, { 0.2f, 0.4f, 1.0f, 0.8f, 0.6f, 0.4f, 0.25f, 0.12f },
         { 0.7f, 0.85f, 1.0f, 0.9f, 0.75f, 0.6f, 0.45f, 0.35f }, 7.0f, 0.3f, 0.0f },
    // Kalimba: steel tine, strong fundamental with a quick inharmonic overtone; box bloom.
    { 3, { 1.0f, 5.95f, 2.01f }, { 1.0f, 0.3f, 0.05f }, { 1.0f, 0.12f, 0.4f }, 1.8f, 0.5f, 0.6f } };

/** Modal bar/tube voice: a damped mode bank struck by a mallet whose hardness sets the brightness. */
class MalletVoice
{
public:
    static constexpr int maxModes = 12;

    void prepare (double sr)
    {
        sampleRate = sr;
        starting.prepare (sr);
        noise.setSeed ((uint32_t) (reinterpret_cast<uintptr_t> (this) >> 4));
    }

    void start (int newNote, float velocity, bool stolen)  { starting.request (newNote, velocity, stolen); releasePending = false; }
    void release()
    {
        if (starting.pending) { releasePending = true; return; }
        if (active && ! released) damp();
    }
    void kill()                                          { starting.clear(); active = false; bank.clear(); clickEnv = 0.0f; }
    bool isActive() const                                { return starting.pending || active; }

    template <typename Owner>
    void render (float* left, float* right, int n, const Owner& owner)
    {
        const MalletParams& p = owner.params;
        damperT60 = p.damperT60;
        if (active && std::abs (owner.bend() - bend) > 0.005f)
        {
            bend = owner.bend();
            setModes (false);
        }

        float blockPeak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            if (starting.pending)
            {
                if (active && starting.fading && starting.note == note) restrike (p);
                else if (starting.tick()) begin (p, owner.bend());
            }
            if (! active && ! starting.pending) break;
            if (! active) continue;

            float y = bank.next() * gain;
            if (clickEnv > 1.0e-5f)
            {
                y += clickEnv * clickFilter.process (noise.next(), clickG, 1.2f).band;
                clickEnv *= clickMul;
            }
            if (starting.fading) y *= juce::jmax (0.0f, starting.fade);
            left[i] += y * panL;
            right[i] += y * panR;
            blockPeak = juce::jmax (blockPeak, std::abs (y));
        }

        if (active && ! starting.pending && clickEnv < 1.0e-5f && blockPeak < 2.0e-6f && bank.level() < 1.0e-4f)
            active = false;
    }

private:
    void setModes (bool fresh)
    {
        const auto& m = malletModels[model];
        const float f0 = noteToHz ((float) note + transpose + bend);
        const float base = m.t60C4 * decayScale * std::exp2 (-((float) note + transpose - 60.0f) / 12.0f * m.slope);
        for (int k = 0; k < m.count; ++k)
        {
            const auto i = (size_t) k;
            freqs[i] = f0 * m.ratio[k];
            if (fresh) t60s[i] = juce::jmin (12.0f, base * m.decay[k]);
            bank.setMode (k, freqs[i], t60s[i], sampleRate);
        }
        const auto r = (size_t) m.count;
        freqs[r] = f0 * 1.0015f;                                    // resonator tube / box bloom
        if (fresh) t60s[r] = juce::jmin (12.0f, base * 1.6f);
        bank.setMode (m.count, freqs[r], t60s[r], sampleRate);
    }

    void begin (const MalletParams& p, float newBend)
    {
        note = starting.note;
        model = (size_t) juce::jlimit (0, (int) std::size (malletModels) - 1, p.model);
        transpose = p.transpose;
        decayScale = p.decay;
        bend = newBend;
        bank.clear();
        bank.setCount (malletModels[model].count + 1);
        polarity = noise.next() < 0.0f ? -1.0f : 1.0f;     // simultaneous strikes don't all add up in phase
        setModes (true);
        strike (p, starting.velocity, 1.0f);
        gain = p.gain;
        std::tie (panL, panR) = panGains (juce::jlimit (-1.0f, 1.0f, ((float) note - 66.0f) / 30.0f) * 0.7f * p.widthAmt);
        released = false;
        active = true;
        starting.clear();
        if (releasePending) { releasePending = false; damp(); }
    }

    void restrike (const MalletParams& p)
    {
        // Re-strike the ringing bar: restore its natural decay and add a new hit.
        setModes (true);
        released = false;
        strike (p, starting.velocity, 1.0f);
        starting.clear();
        if (releasePending) { releasePending = false; damp(); }
    }

    void strike (const MalletParams& p, float vel, float amp)
    {
        const auto& m = malletModels[model];
        const float vg = velocityGain (vel, p.velSens) * amp * polarity;
        // Mallet hardness: harder mallets and harder hits reach higher modes.
        const float fc = 350.0f * std::exp2 (p.hardness * 4.5f + vel * 1.8f);
        for (int k = 0; k < m.count; ++k)
        {
            const float x = freqs[(size_t) k] / fc;
            bank.strike (k, m.amp[k] * vg / (1.0f + x * x));
        }
        bank.strike (m.count, m.resonance * p.resonator * 0.6f * vg);

        clickEnv = p.click * std::abs (vg) * (0.3f + 0.7f * p.hardness) * 0.5f;
        clickMul = t60Mul (0.004f + 0.012f * (1.0f - p.hardness), sampleRate);
        clickG = Svf::gFor (juce::jmin (12000.0f, 1500.0f + 7000.0f * p.hardness + 2.0f * freqs[0]), sampleRate);
    }

    void damp()
    {
        released = true;
        const int count = malletModels[model].count + 1;
        for (int k = 0; k < count; ++k)
        {
            t60s[(size_t) k] = juce::jmin (t60s[(size_t) k], damperT60);
            bank.setDecay (k, t60s[(size_t) k], sampleRate);
        }
    }

    double sampleRate = 48000.0;
    PendingStart starting;
    ModalBank<maxModes> bank;
    std::array<float, maxModes> freqs {}, t60s {};
    Noise noise;
    Svf clickFilter;
    size_t model = 0;
    int note = -1;
    float transpose = 0.0f, decayScale = 1.0f, bend = 0.0f, damperT60 = 1.0f, gain = 0.1f, panL = 1.0f, panR = 1.0f;
    float clickEnv = 0.0f, clickMul = 0.0f, clickG = 0.5f, polarity = 1.0f;
    bool active = false, released = false, releasePending = false;
};

// Mallets: modal marimba, vibraphone, glockenspiel, celesta, tubular bells and kalimba.
class Mallets final : public PolyInstrument<Mallets, MalletVoice, 16>
{
public:
    enum { instrument, hardness, decay, damper, click, resonator, tremolo, rate, octave, vel, reverb, width, level };

    MalletParams params;

    void update() override
    {
        auto& p = params;
        p.model = juce::roundToInt (knob (instrument));
        p.hardness = knob (hardness);
        p.decay = knob (decay);
        p.damperT60 = msToS (knob (damper));
        p.click = knob (click);
        p.resonator = knob (resonator);
        p.transpose = 12.0f * (float) (juce::roundToInt (knob (octave)) - 2);
        p.velSens = knob (vel);
        p.widthAmt = knob (width);
        static const float modelGain[] { 0.13f, 0.11f, 0.09f, 0.12f, 0.065f, 0.12f };
        p.gain = modelGain[juce::jlimit (0, 5, p.model)] * dbToGain (knob (level));
        tremDepth.set (knob (tremolo));
        tremRate = knob (rate);
        reverbAmount = knob (reverb) * 0.5f;
    }

protected:
    void prepareEffects() override                      { room.prepare (sampleRate); room.setDecay (1.8f, 6000.0f); tremDepth.prepare (sampleRate, 0.05f); }
    void resetEffects() override                        { room.reset(); tremDepth.reset(); tremPhase = 0.0f; }
    void processEffects (float* l, float* r, int n) override
    {
        // Vibraphone motor: rotating discs over the tubes, slightly out of phase left/right.
        const float inc = tremRate / (float) sampleRate;
        for (int i = 0; i < n; ++i)
        {
            const float d = tremDepth.next();
            tremPhase = wrap01 (tremPhase + inc);
            if (d > 0.001f)
            {
                l[i] *= 1.0f - d * (0.5f + 0.5f * sinTurns (tremPhase));
                r[i] *= 1.0f - d * (0.5f + 0.5f * sinTurns (tremPhase + 0.15f));
            }
        }
        room.process (l, r, n, reverbAmount);
        safetyClip (l, r, n);
    }

private:
    Room room;
    Smooth tremDepth;
    float tremRate = 5.0f, tremPhase = 0.0f, reverbAmount = 0.0f;
};

const juce::StringArray octaves { "-2", "-1", "0", "+1", "+2" };

} // namespace

/** Plucked strings, electric bass and mallets; registered from Orchestral.cpp so they sit in library order. */
void registerPluckedInstruments (InstrumentRegistry&);
void registerPluckedInstruments (InstrumentRegistry& r)
{
    using P = InstrumentInfo::Patch;

    r.add<PluckedStrings> ({ "pluck.guitar", "Plucked Strings", "Mallets & Plucked",
                             "Physically modelled plucked strings: nylon and steel guitar, harp and koto",
                             juce::Colour (0xffb5793f),
                             { choice ("string", "String", { "Nylon", "Steel", "Harp", "Koto" }, 0),
                               knob ("bright", "Brightness", 0.0f, 1.0f, 0.5f, "%"),
                               knob ("position", "Pluck Pos", 0.04f, 0.5f, 0.15f, "%"),
                               knob ("decay", "Decay", 0.2f, 3.0f, 1.0f, "x", 0.6f),
                               knob ("release", "Release", 20.0f, 3000.0f, 180.0f, "ms", 0.4f),
                               knob ("mute", "Palm Mute", 0.0f, 1.0f, 0.0f, "%"),
                               knob ("body", "Body", 0.0f, 1.0f, 0.5f, "%"),
                               choice ("double", "Course", { "Single", "Unison", "Octave" }, 0),
                               knob ("vel", "Velocity", 0.0f, 1.0f, 0.6f, "%"),
                               knob ("vibrato", "Vibrato", 0.0f, 1.0f, 0.0f, "%"),
                               knob ("reverb", "Room", 0.0f, 1.0f, 0.25f, "%"),
                               knob ("width", "Width", 0.0f, 1.0f, 0.5f, "%"),
                               knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") },
                             { P { "Nylon Guitar", "Guitars", {} },
                               P { "Steel Guitar", "Guitars", { { "string", 1 }, { "position", 0.12f }, { "body", 0.4f } } },
                               P { "12-String", "Guitars", { { "string", 1 }, { "double", 2 }, { "bright", 0.6f }, { "position", 0.1f },
                                                              { "reverb", 0.3f }, { "width", 0.8f } } },
                               P { "Harp", "Harps", { { "string", 2 }, { "position", 0.3f }, { "release", 1500.0f }, { "body", 0.35f },
                                                       { "reverb", 0.45f }, { "width", 0.8f } } },
                               P { "Koto", "World", { { "string", 3 }, { "position", 0.08f }, { "bright", 0.55f }, { "body", 0.4f },
                                                       { "vibrato", 0.15f }, { "reverb", 0.35f } } },
                               P { "Muted Pluck", "Plucks", { { "string", 1 }, { "mute", 0.7f }, { "position", 0.1f }, { "release", 60.0f },
                                                               { "body", 0.3f }, { "reverb", 0.15f } } },
                               P { "Bright Nylon", "Guitars", { { "bright", 0.75f }, { "position", 0.09f }, { "decay", 1.3f }, { "level", 2.5f } } },
                               P { "Dark Ballad Harp", "Harps", { { "string", 2 }, { "bright", 0.3f }, { "position", 0.45f }, { "decay", 1.4f },
                                                                   { "release", 2500.0f }, { "reverb", 0.6f }, { "width", 0.9f }, { "level", -5.0f } } } } });

    r.add<ElectricBass> ({ "bass.electric", "Electric Bass", "Bass",
                           "Modelled electric bass: finger, pick, slap and muted styles through a pickup and tone control",
                           juce::Colour (0xff4b6a8f),
                           { choice ("style", "Style", { "Finger", "Pick", "Slap", "Muted" }, 0),
                             knob ("tone", "Tone", 200.0f, 10000.0f, 3000.0f, "Hz", 0.4f),
                             knob ("pickup", "Pickup Pos", 0.05f, 0.5f, 0.2f, "%"),
                             knob ("decay", "Decay", 0.2f, 3.0f, 1.0f, "x", 0.6f),
                             knob ("release", "Release", 20.0f, 2000.0f, 90.0f, "ms", 0.4f),
                             knob ("sub", "Sub", 0.0f, 1.0f, 0.0f, "%"),
                             knob ("drive", "Drive", 0.0f, 1.0f, 0.0f, "%"),
                             choice ("mono", "Voices", { "Poly", "Mono" }, 0),
                             knob ("glide", "Glide", 0.0f, 500.0f, 0.0f, "ms", 0.5f),
                             knob ("vel", "Velocity", 0.0f, 1.0f, 0.5f, "%"),
                             knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") },
                           { P { "Finger Bass", "Basses", {} },
                             P { "Pick Bass", "Basses", { { "style", 1 }, { "tone", 5000.0f }, { "pickup", 0.15f } } },
                             P { "Slap Bass", "Basses", { { "style", 2 }, { "tone", 8000.0f }, { "pickup", 0.12f }, { "vel", 0.7f } } },
                             P { "Muted Bass", "Basses", { { "style", 3 }, { "tone", 2000.0f }, { "release", 50.0f } } },
                             P { "Fretless", "Basses", { { "tone", 2200.0f }, { "pickup", 0.3f }, { "decay", 1.4f },
                                                          { "release", 200.0f } } },
                             P { "Deep Sub", "Basses", { { "tone", 500.0f }, { "sub", 0.7f }, { "pickup", 0.35f }, { "decay", 1.5f }, { "level", -3.0f } } },
                             P { "Driven Pick", "Basses", { { "style", 1 }, { "tone", 3500.0f }, { "drive", 0.6f }, { "pickup", 0.1f } } } } });

    r.add<Mallets> ({ "mallet.keys", "Mallets", "Mallets & Plucked",
                      "Modal marimba, vibraphone, glockenspiel, celesta, tubular bells and kalimba",
                      juce::Colour (0xff9c6bb3),
                      { choice ("instrument", "Instrument", { "Marimba", "Vibraphone", "Glockenspiel", "Celesta", "Tubular Bells", "Kalimba" }, 0),
                        knob ("hardness", "Mallet", 0.0f, 1.0f, 0.5f, "%"),
                        knob ("decay", "Decay", 0.25f, 3.0f, 1.0f, "x", 0.6f),
                        knob ("damper", "Damper", 30.0f, 4000.0f, 2000.0f, "ms", 0.4f),
                        knob ("click", "Attack", 0.0f, 1.0f, 0.3f, "%"),
                        knob ("resonator", "Resonator", 0.0f, 1.0f, 0.6f, "%"),
                        knob ("tremolo", "Tremolo", 0.0f, 1.0f, 0.0f, "%"),
                        knob ("rate", "Trem Rate", 1.0f, 10.0f, 5.0f, "Hz"),
                        choice ("octave", "Octave", octaves, 2),
                        knob ("vel", "Velocity", 0.0f, 1.0f, 0.7f, "%"),
                        knob ("reverb", "Room", 0.0f, 1.0f, 0.3f, "%"),
                        knob ("width", "Width", 0.0f, 1.0f, 0.6f, "%"),
                        knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") },
                      { P { "Marimba", "Mallets", {} },
                        P { "Vibraphone", "Mallets", { { "instrument", 1 }, { "hardness", 0.4f }, { "damper", 180.0f }, { "tremolo", 0.45f },
                                                        { "rate", 5.5f }, { "click", 0.15f }, { "reverb", 0.4f } } },
                        P { "Glockenspiel", "Mallets", { { "instrument", 2 }, { "hardness", 0.75f }, { "octave", 3 }, { "damper", 2500.0f },
                                                          { "click", 0.4f }, { "reverb", 0.4f } } },
                        P { "Celesta", "Keys", { { "instrument", 3 }, { "hardness", 0.45f }, { "octave", 3 }, { "damper", 250.0f },
                                                  { "click", 0.1f }, { "reverb", 0.35f } } },
                        P { "Tubular Bells", "Bells", { { "instrument", 4 }, { "hardness", 0.7f }, { "damper", 2500.0f }, { "click", 0.35f },
                                                         { "reverb", 0.5f }, { "width", 0.8f } } },
                        P { "Soft Mallets", "Mallets", { { "hardness", 0.1f }, { "click", 0.05f }, { "resonator", 0.9f }, { "reverb", 0.4f } } },
                        P { "Kalimba", "World", { { "instrument", 5 }, { "hardness", 0.6f }, { "damper", 1500.0f }, { "click", 0.25f },
                                                   { "reverb", 0.25f } } },
                        P { "Motor Vibes", "Mallets", { { "instrument", 1 }, { "hardness", 0.25f }, { "damper", 1200.0f }, { "tremolo", 0.8f },
                                                         { "rate", 7.5f }, { "click", 0.05f }, { "reverb", 0.45f }, { "width", 0.9f } } } } });
}

} // namespace woodshed
