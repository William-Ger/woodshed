#include "InstrumentDsp.h"

namespace woodshed
{
namespace
{
using namespace idsp;

inline float ms (float v)                       { return v * 0.001f; }

/** Deterministic pseudo-random 0..1 from two integers (per-note, per-partial variation). */
inline float hash01 (int a, int b)
{
    uint32_t h = (uint32_t) a * 73856093u ^ (uint32_t) b * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (float) (h & 0xffffu) / 65535.0f;
}

/** Per-sample multiplier that decays 60 dB in `t60` seconds. */
inline float decayCoeff (float t60, double sr)  { return std::exp (-6.9078f / (juce::jmax (0.001f, t60) * (float) sr)); }

//==============================================================================
// Grand Piano: modal synthesis. Every partial is a pair of decaying sines (the two
// polarisations / unison strings): a loud "prompt" mode and a quiet, slower, slightly
// detuned "aftersound" mode, which gives the beating and the two-stage decay. Partials
// are stretched by string stiffness, shaped by hammer hardness (velocity) and the strike
// point, and lose their highs first. Dampers fall on key-up below the top octave and a half.
struct PianoParams
{
    float bright = 0.0f, hammer = 0.5f, decayScale = 1.0f, damper = 0.2f, unison = 0.3f, stretch = 1.0f;
    float velSens = 0.8f, width = 0.6f, level = 1.0f;
};

class PianoVoice
{
public:
    static constexpr int maxPartials = 40;
    static constexpr int pairedPartials = 16;      // only the lower partials ring long enough to need an aftersound mode
    static constexpr int lastDampedNote = 88;

    void prepare (double sr)
    {
        sampleRate = sr;
        starting.prepare (sr);
        noise.setSeed ((uint32_t) (reinterpret_cast<uintptr_t> (this) >> 4));
        knockCoeff = decayCoeff (0.03f, sr);
    }

    void start (int newNote, float velocity, bool stolen)   { starting.request (newNote, velocity, stolen); releasePending = dampPending = false; }
    void release()                                           { if (starting.pending) releasePending = true; else dampPending = true; }
    void kill()                                              { starting.clear(); modes.clear(); active = false; releasePending = dampPending = false; knock = 0.0f; }
    bool isActive() const                                    { return starting.pending || active; }

    template <typename Owner>
    void render (float* left, float* right, int n, const Owner& owner)
    {
        const PianoParams& p = owner.params;
        if (dampPending) { applyDamper (p); dampPending = false; }

        for (int i = 0; i < n; ++i)
        {
            if (starting.pending)
            {
                if (active && starting.note == note) restrike (p);
                else if (starting.tick()) begin (p);
            }
            if (! active) break;

            float x = modes.next();
            if (knock > 1.0e-5f)
            {
                x += knock * knockFilter.process (noise.next(), knockG, 0.9f).band;
                knock *= knockCoeff;
            }

            float out = x * gain;
            if (starting.fading) out *= juce::jmax (0.0f, starting.fade);
            left[i] += out * panL;
            right[i] += out * panR;
        }

        if (active && ! starting.pending && modes.level() < 2.0e-4f && knock < 1.0e-5f)
        {
            active = false;
            modes.clear();
        }
    }

private:
    void begin (const PianoParams& p)
    {
        note = starting.note;
        modes.clear();
        knockFilter.reset();
        setup (p, starting.velocity);
        std::tie (panL, panR) = panGains (juce::jlimit (-1.0f, 1.0f, (float) (note - 64) / 44.0f) * p.width * 0.8f);
        active = true;
        finishStart (p);
    }

    void restrike (const PianoParams& p)
    {
        // Same key again: the hammer hits strings that are still moving. Re-exciting the
        // live modes is click-free and physically right.
        setup (p, starting.velocity);
        finishStart (p);
    }

    void finishStart (const PianoParams& p)
    {
        starting.clear();
        if (releasePending) { applyDamper (p); releasePending = false; }
    }

    /** Sets every mode's frequency and undamped decay, then strikes them for `velocity`. */
    void setup (const PianoParams& p, float velocity)
    {
        const auto sr = (float) sampleRate;
        const float key = (float) note;
        const float rel = (key - 64.0f) / 44.0f;
        const float stretchCents = p.stretch * 22.0f * rel * rel * rel;          // Railsback-ish octave stretch
        const float f0 = noteToHz (key + stretchCents * 0.01f);
        const float B = p.stretch * juce::jmax (1.2e-4f, 3.2e-4f * std::exp2 ((key - 60.0f) / 12.0f));

        const float v = juce::jlimit (0.0f, 1.0f, velocity);
        const float hammerHz = (500.0f + 6500.0f * std::pow (v, 1.6f)) * std::exp2 (1.6f * p.bright);
        const float tilt = 0.85f - 0.25f * v - 0.25f * p.bright;
        const float strike = 1.0f / (7.5f + 1.5f * hash01 (note, 999));
        const float fundT60 = p.decayScale * juce::jlimit (0.8f, 24.0f, 24.0f * std::exp2 (-(key - 33.0f) / 17.0f));
        const float beat = p.unison * (0.6f + 2.4f * p.unison) + (p.unison > 0.7f ? (p.unison - 0.7f) * 40.0f : 0.0f);   // cents

        std::array<float, maxPartials> amps {};
        float energy = 0.0f;
        int count = 0;

        for (int k = 1; k <= maxPartials; ++k)
        {
            const float nk = (float) k;
            const float fk = nk * f0 * std::sqrt (1.0f + B * nk * nk);
            if (fk > juce::jmin (0.42f * sr, 15000.0f)) break;

            const float hammerShape = 1.0f / std::sqrt (1.0f + (fk / hammerHz) * (fk / hammerHz) * (fk / hammerHz) * 0.5f);
            const float strikeComb = 0.12f + 0.88f * std::abs (std::sin (pi * nk * strike));
            const float a = std::pow (nk, -tilt) * hammerShape * strikeComb;
            amps[(size_t) (k - 1)] = a;
            energy += a * a;
            count = k;

            const float lossPerSecond = 6.9078f / fundT60 + 4.0f * (fk / 3000.0f) * (fk / 3000.0f) + 0.45f * fk / 1000.0f;
            const float t60 = 6.9078f / lossPerSecond;
            const float detune = beat * (0.35f + 0.65f * hash01 (note, k)) * (hash01 (k, note) > 0.5f ? 1.0f : -1.0f);

            hz[(size_t) (k - 1)] = fk;
            undamped[(size_t) (k - 1)] = t60 * (k <= pairedPartials ? 0.55f : 0.8f);   // prompt sound
            afterHz[(size_t) (k - 1)] = fk * semitones (detune * 0.01f);
            afterT60[(size_t) (k - 1)] = t60 * 1.9f;                                   // aftersound
        }

        const int paired = juce::jmin (count, (int) pairedPartials);
        partials = count;
        for (int k = 0; k < paired; ++k)
        {
            hz[(size_t) (count + k)] = afterHz[(size_t) k];
            undamped[(size_t) (count + k)] = afterT60[(size_t) k];
        }

        modes.setCount (count + paired);
        const float norm = 1.0f / std::sqrt (juce::jmax (1.0e-6f, energy));
        for (int m = 0; m < count + paired; ++m)
        {
            const int k = m < count ? m : m - count;
            const float share = k >= paired ? 1.0f : (m < count ? 0.74f : 0.26f);
            modes.setMode (m, hz[(size_t) m], undamped[(size_t) m], sampleRate);
            const float sign = hash01 (note * 31 + k, 5) > 0.5f ? 1.0f : -1.0f;      // scattered polarities keep the attack crest down
            modes.strike (m, sign * amps[(size_t) k] * norm * share);
        }

        gain = 0.13f * velocityGain (v, p.velSens) * p.level;
        knock = p.hammer * 0.5f * std::pow (v, 1.5f);
        knockG = Svf::gFor (juce::jlimit (300.0f, 4500.0f, f0 * 3.0f), sampleRate);
    }

    void applyDamper (const PianoParams& p)
    {
        if (note > lastDampedNote) return;
        const float bass = 1.0f + juce::jlimit (0.0f, 1.5f, (float) (60 - note) / 24.0f);
        for (int m = 0; m < modes.count; ++m)
            modes.setDecay (m, juce::jmin (undamped[(size_t) m], p.damper * bass / (1.0f + 0.04f * (float) (m < partials ? m : m - partials))), sampleRate);
    }

    double sampleRate = 48000.0;
    PendingStart starting;
    ModalBank<maxPartials + pairedPartials> modes;
    std::array<float, maxPartials + pairedPartials> hz {}, undamped {};
    std::array<float, maxPartials> afterHz {}, afterT60 {};
    int partials = 0;
    Noise noise;
    Svf knockFilter;
    float knock = 0.0f, knockCoeff = 0.99f, knockG = 0.1f;
    float gain = 0.0f, panL = 1.0f, panR = 1.0f;
    int note = -1;
    bool active = false, releasePending = false, dampPending = false;
};

class GrandPiano final : public PolyInstrument<GrandPiano, PianoVoice, 24>
{
public:
    enum { bright, hammer, decay, damper, strings, stretch, vel, width, room, roomsize, level };

    PianoParams params;

    float bendRange() const override            { return 0.0f; }

    void update() override
    {
        params.bright = knob (bright);
        params.hammer = knob (hammer);
        params.decayScale = knob (decay);
        params.damper = ms (knob (damper));
        params.unison = knob (strings);
        params.stretch = knob (stretch);
        params.velSens = knob (vel);
        params.width = knob (width);
        params.level = dbToGain (knob (level));
        roomAmount = knob (room);
        const float size = knob (roomsize);
        if (std::abs (size - roomSize) > 0.001f) { roomSize = size; reverb.setDecay (size, 5200.0f); }
    }

protected:
    void prepareEffects() override              { reverb.prepare (sampleRate); roomSize = -1.0f; }
    void resetEffects() override                { reverb.reset(); }
    void processEffects (float* l, float* r, int n) override
    {
        reverb.process (l, r, n, roomAmount * 0.3f);
        safetyClip (l, r, n);
    }

private:
    Room reverb;
    float roomAmount = 0.0f, roomSize = -1.0f;
};

//==============================================================================
// Electric Piano: a tine (or reed) as a sine carrier phase-modulated by itself (the
// tone mellows as the index decays) plus a fast-decaying high "bell" modulator, through
// an asymmetric pickup for bark. Tremolo/auto-pan, tone and chorus are shared effects.
struct EPianoParams
{
    int model = 0;
    float bark = 0.5f, bell = 0.5f, decayScale = 1.0f, release = 0.15f, velSens = 0.7f, level = 1.0f;
};

class EPianoVoice
{
public:
    void prepare (double sr)                    { sampleRate = sr; starting.prepare (sr); attackInc = 1.0f / (float) (0.0015 * sr); }
    void start (int newNote, float velocity, bool stolen)   { starting.request (newNote, velocity, stolen); releasePending = releaseNow = false; }
    void release()                              { if (starting.pending) releasePending = true; else releaseNow = true; }
    void kill()                                 { starting.clear(); active = false; amp = 0.0f; releasePending = releaseNow = false; }
    bool isActive() const                       { return starting.pending || active; }

    template <typename Owner>
    void render (float* left, float* right, int n, const Owner& owner)
    {
        const EPianoParams& p = owner.params;
        if (releaseNow) { ampCoeff = decayCoeff (p.release, sampleRate); releaseNow = false; }
        const float bendRatio = semitones (owner.bend());
        const float inc = baseInc * bendRatio;

        for (int i = 0; i < n; ++i)
        {
            if (starting.pending && starting.tick()) begin (p);
            if (! active) break;

            attack = juce::jmin (1.0f, attack + attackInc);
            const float m = index * sinTurns (modPhase) + bellIndex * sinTurns (bellPhase);
            const float x = sinTurns (phase + m);
            float y = x + asym * x * x;
            if (drive > 0.0f) y = saturate (y * (1.0f + drive)) / (1.0f + 0.4f * drive);
            y = dc.process (y);

            float out = y * amp * attack * gain;
            if (starting.fading) out *= juce::jmax (0.0f, starting.fade);
            left[i] += out * panL;
            right[i] += out * panR;

            phase = wrap01 (phase + inc);
            modPhase = wrap01 (modPhase + inc * modRatio);
            bellPhase = wrap01 (bellPhase + inc * bellRatio);
            amp *= ampCoeff;
            index *= indexCoeff;
            bellIndex *= bellCoeff;
        }

        if (active && ! starting.pending && amp < 1.0e-4f) active = false;
    }

private:
    void begin (const EPianoParams& p)
    {
        const int note = starting.note;
        const float key = (float) note;
        const float v = starting.velocity;
        baseInc = noteToHz (key) / (float) sampleRate;
        phase = modPhase = bellPhase = 0.0f;
        attack = 0.0f;
        amp = 1.0f;
        dc.reset();

        float t60 = p.decayScale * juce::jlimit (1.2f, 14.0f, 11.0f * std::exp2 (-(key - 40.0f) / 22.0f));
        const float highCut = juce::jlimit (0.15f, 1.0f, 1.0f - (key - 72.0f) / 48.0f);    // less index up top (no aliasing)

        switch (p.model)
        {
            case 1:     // Reed
                modRatio = 1.0f; bellRatio = 3.0f;
                index = (0.06f + 0.34f * v * v) * (0.5f + p.bark) * highCut;
                indexCoeff = decayCoeff (1.2f, sampleRate);
                bellIndex = p.bell * 0.05f * v;
                bellCoeff = decayCoeff (0.12f, sampleRate);
                asym = 0.25f + 0.35f * p.bark * v;
                drive = 1.2f * p.bark * v;
                t60 *= 0.6f;
                break;
            case 2:     // FM bell
                modRatio = 1.0f; bellRatio = 14.0f;
                index = (0.03f + 0.10f * v) * (0.5f + p.bark) * highCut;
                indexCoeff = decayCoeff (2.0f, sampleRate);
                bellIndex = p.bell * (0.06f + 0.22f * v * v) * highCut;
                bellCoeff = decayCoeff (0.55f, sampleRate);
                asym = 0.0f;
                drive = 0.0f;
                break;
            default:    // Tine
                modRatio = 1.0f; bellRatio = 14.0f;
                index = (0.03f + 0.26f * v * v) * (0.4f + p.bark) * highCut;
                indexCoeff = decayCoeff (0.9f, sampleRate);
                bellIndex = p.bell * 0.12f * std::pow (v, 1.5f) * highCut;
                bellCoeff = decayCoeff (0.22f, sampleRate);
                asym = p.bark * (0.15f + 0.45f * v);
                drive = 0.4f * p.bark * v;
                break;
        }

        ampCoeff = decayCoeff (t60, sampleRate);
        gain = 0.16f * velocityGain (v, p.velSens) * p.level;
        std::tie (panL, panR) = panGains (juce::jlimit (-0.4f, 0.4f, (key - 64.0f) / 80.0f));
        active = true;
        starting.clear();
        if (releasePending) { ampCoeff = decayCoeff (p.release, sampleRate); releasePending = false; }
    }

    double sampleRate = 48000.0;
    PendingStart starting;
    DcBlock dc;
    float phase = 0.0f, modPhase = 0.0f, bellPhase = 0.0f, baseInc = 0.0f, modRatio = 1.0f, bellRatio = 14.0f;
    float amp = 0.0f, ampCoeff = 1.0f, index = 0.0f, indexCoeff = 1.0f, bellIndex = 0.0f, bellCoeff = 1.0f;
    float attack = 0.0f, attackInc = 0.01f, asym = 0.0f, drive = 0.0f, gain = 0.0f, panL = 1.0f, panR = 1.0f;
    bool active = false, releasePending = false, releaseNow = false;
};

class ElectricPiano final : public PolyInstrument<ElectricPiano, EPianoVoice, 16>
{
public:
    enum { model, bark, bell, decay, release, tremolo, tremrate, stereo, tone, chorus, vel, level };

    EPianoParams params;

    void update() override
    {
        params.model = juce::roundToInt (knob (model));
        params.bark = knob (bark);
        params.bell = knob (bell);
        params.decayScale = knob (decay);
        params.release = ms (knob (release));
        params.velSens = knob (vel);
        params.level = dbToGain (knob (level));
        tremDepth.set (knob (tremolo));
        tremInc = knob (tremrate) / (float) sampleRate;
        stereoAmount = knob (stereo);
        toneG = Svf::gFor (knob (tone), sampleRate);
        chorusMix = knob (chorus);
    }

protected:
    void prepareEffects() override              { ensemble.prepare (sampleRate); tremDepth.prepare (sampleRate, 0.05f); }
    void resetEffects() override                { ensemble.reset(); toneL.reset(); toneR.reset(); tremPhase = 0.0f; tremDepth.reset(); }

    void processEffects (float* l, float* r, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            tremPhase += tremInc;
            if (tremPhase >= 1.0f) tremPhase -= 1.0f;
            const float s = sinTurns (tremPhase), d = tremDepth.next();
            const float trem = 1.0f - d * 0.5f * (1.0f + s) * (1.0f - stereoAmount);
            const float pan = 0.6f * d * s * stereoAmount;
            l[i] = toneL.process (l[i], toneG, 0.8f).low * trem * (1.0f - pan);
            r[i] = toneR.process (r[i], toneG, 0.8f).low * trem * (1.0f + pan);
        }
        ensemble.process (l, r, n, chorusMix, 0.7f);
        safetyClip (l, r, n);
    }

private:
    Ensemble ensemble;
    Svf toneL, toneR;
    Smooth tremDepth;
    float tremPhase = 0.0f, tremInc = 0.0f, stereoAmount = 0.0f, toneG = 0.5f, chorusMix = 0.0f;
};

//==============================================================================
// Tonewheel Organ: one shared bank of 92 free-running tonewheels; each key just opens
// nine drawbar taps on it, so any number of keys costs the same and nothing is ever
// stolen. Single-trigger percussion, key click, scanner vibrato/chorus, overdrive and a
// two-rotor rotary speaker (mod wheel switches it fast).
class TonewheelOrgan final : public Instrument
{
public:
    enum { d16, d513, d8, d4, d223, d2, d135, d113, d1, perc, perclevel, click, vibrato, drive, rotary, tone, room, level };

    static constexpr int numWheels = 92;           // MIDI 24 (C1) .. 115
    static constexpr int padded = 92;

    void update() override
    {
        for (size_t t = 0; t < 9; ++t)
        {
            const float step = juce::jlimit (0.0f, 8.0f, knob ((int) t));
            barTarget[t] = step < 1.0f ? step * 0.0891f : std::pow (10.0f, -(8.0f - step) * 0.15f);    // 3 dB per step
        }
        percMode = juce::roundToInt (knob (perc));
        percLevel = knob (perclevel);
        clickLevel = knob (click);
        vibMode = juce::roundToInt (knob (vibrato));
        driveAmount = knob (drive);
        rotaryMode = juce::roundToInt (knob (rotary));
        toneG = Svf::gFor (knob (tone), sampleRate);
        roomAmount = knob (room);
        outGain = 0.032f * dbToGain (knob (level));
    }

    void reset() override
    {
        for (int w = 0; w < padded; ++w)
        {
            const float ph = twoPi * hash01 (w, 7);
            re[(size_t) w] = std::cos (ph);
            im[(size_t) w] = std::sin (ph);
        }
        bus.fill (0.0f);
        for (auto& k : keys) k = {};
        numActive = 0;
        bars = barTarget;
        percEnv = clickEnv = 0.0f;
        sustainDown = false;
        modValue = 0.0f;
        vibLine.clear();
        hornLine.clear(); drumLine.clear();
        crossover.reset(); clickFilter.reset(); toneFilter.reset();
        hornAngle = drumAngle = 0.0f;
        hornSpeed = 0.8f; drumSpeed = 0.67f;
        rotaryMix.reset();
        vibPhase = 0.0f;
        reverb.reset();
    }

    void render (float* left, float* right, int numSamples, const NoteEvent* events, int numEvents) override
    {
        const int chunk = (int) scratchL.size();
        int start = 0, e = 0;
        smoothBars();

        while (chunk > 0 && start < numSamples)
        {
            const int n = juce::jmin (chunk, numSamples - start);
            int done = 0;
            for (; e < numEvents && events[e].offset < start + n; ++e)
            {
                const int until = juce::jlimit (done, n, events[e].offset - start);
                if (until > done) renderMono (scratchL.data() + done, until - done);
                done = until;
                handle (events[e]);
            }
            if (n > done) renderMono (scratchL.data() + done, n - done);

            effects (scratchL.data(), scratchR.data(), n);
            juce::FloatVectorOperations::add (left + start, scratchL.data(), n);
            juce::FloatVectorOperations::add (right + start, scratchR.data(), n);
            start += n;
        }

        for (; e < numEvents; ++e) handle (events[e]);
        renormaliseWheels();
    }

protected:
    void prepared() override
    {
        scratchL.assign ((size_t) juce::jmax (64, maxBlock), 0.0f);
        scratchR.assign ((size_t) juce::jmax (64, maxBlock), 0.0f);

        for (int w = 0; w < padded; ++w)
        {
            // Real wheels come from gear ratios, so they're a hair off equal temperament.
            const float hz = noteToHz (24.0f + (float) w + 0.012f * (hash01 (w, 3) - 0.5f));
            const float a = twoPi * hz / (float) sampleRate;
            cr[(size_t) w] = std::cos (a);
            ci[(size_t) w] = std::sin (a);
        }

        const int offsets[] { -12, 7, 0, 12, 19, 24, 28, 31, 36 };
        for (int note = 0; note < 128; ++note)
            for (size_t t = 0; t < 9; ++t)
            {
                int w = note + offsets[t] - 24;
                while (w < 0) w += 12;
                while (w >= numWheels) w -= 12;
                taps[(size_t) note][t] = w;
            }

        vibLine.setMaxSamples ((int) (sampleRate * 0.004));
        hornLine.setMaxSamples ((int) (sampleRate * 0.006));
        drumLine.setMaxSamples ((int) (sampleRate * 0.006));
        rotaryMix.prepare (sampleRate, 0.05f);
        onRate = 1.0f / (float) (0.0025 * sampleRate);
        offRate = 1.0f / (float) (0.006 * sampleRate);
        clickCoeff = decayCoeff (0.012f, sampleRate);
        clickG = Svf::gFor (2800.0f, sampleRate);
        crossG = Svf::gFor (800.0f, sampleRate);
        reverb.prepare (sampleRate);
        reverb.setDecay (1.3f, 5000.0f);
    }

private:
    struct Key
    {
        float env = 0.0f, target = 0.0f;
        bool held = false, sustained = false, listed = false;
    };

    void handle (const NoteEvent& ev)
    {
        switch (ev.kind)
        {
            case NoteEvent::noteOn:
                if (ev.value <= 0.0f) { keyUp (ev.note); break; }
                keyDown (ev.note);
                break;
            case NoteEvent::noteOff:     keyUp (ev.note); break;
            case NoteEvent::allNotesOff:
                sustainDown = false;
                for (auto& k : keys) { k.held = k.sustained = false; k.target = 0.0f; }
                break;
            case NoteEvent::sustain:
                sustainDown = ev.value >= 0.5f;
                if (! sustainDown)
                    for (auto& k : keys)
                        if (k.sustained) { k.sustained = false; k.target = 0.0f; clickEnv += 0.2f * clickLevel; }
                break;
            case NoteEvent::modWheel:    modValue = ev.value; break;
            case NoteEvent::pitchBend:   break;
        }
    }

    void keyDown (int note)
    {
        if (note < 0 || note > 127) return;
        bool anyHeld = false;
        for (int a = 0; a < numActive; ++a)
            if (keys[(size_t) active[(size_t) a]].held) { anyHeld = true; break; }
        if (! anyHeld && percMode > 0) percEnv = 1.0f;       // single trigger

        auto& k = keys[(size_t) note];
        k.held = true;
        k.sustained = false;
        k.target = 1.0f;
        if (! k.listed) { k.listed = true; active[(size_t) numActive++] = note; }
        clickEnv = juce::jmin (1.5f, clickEnv + 0.7f * clickLevel);
    }

    void keyUp (int note)
    {
        if (note < 0 || note > 127) return;
        auto& k = keys[(size_t) note];
        if (! k.held) return;
        k.held = false;
        if (sustainDown) { k.sustained = true; return; }
        k.target = 0.0f;
        clickEnv = juce::jmin (1.5f, clickEnv + 0.25f * clickLevel);
    }

    void smoothBars()
    {
        bool changed = false;
        for (size_t t = 0; t < 9; ++t)
        {
            const float d = barTarget[t] - bars[t];
            if (std::abs (d) > 1.0e-5f) { bars[t] += std::abs (d) < 1.0e-3f ? d : 0.25f * d; changed = true; }
        }
        if (changed) rebuildBus();
    }

    void rebuildBus()
    {
        bus.fill (0.0f);
        for (int a = 0; a < numActive; ++a)
        {
            const int note = active[(size_t) a];
            const float env = keys[(size_t) note].env;
            for (size_t t = 0; t < 9; ++t)
                bus[(size_t) taps[(size_t) note][t]] += env * bars[t];
        }
    }

    void renderMono (float* out, int n)
    {
        const bool second = percMode == 1 || percMode == 2;
        const float percC = decayCoeff (percMode == 2 || percMode == 4 ? 1.4f : 0.45f, sampleRate);
        const size_t percTap = second ? 3 : 4;                 // 4' or 2 2/3'

        for (int i = 0; i < n; ++i)
        {
            // Key envelopes move the drawbar taps on the shared bus.
            bool anyRamping = false;
            for (int a = 0; a < numActive; ++a)
            {
                const int note = active[(size_t) a];
                auto& k = keys[(size_t) note];
                if (std::abs (k.target - k.env) <= 0.0f) continue;
                anyRamping = true;
                const float next = k.target > k.env ? juce::jmin (k.target, k.env + onRate) : juce::jmax (k.target, k.env - offRate);
                const float delta = next - k.env;
                k.env = next;
                for (size_t t = 0; t < 9; ++t)
                    bus[(size_t) taps[(size_t) note][t]] += delta * bars[t];
            }
            if (anyRamping) pruneKeys();

            float acc = 0.0f;
            for (size_t w = 0; w < (size_t) padded; ++w)
            {
                const float nr = re[w] * cr[w] - im[w] * ci[w];
                const float ni = re[w] * ci[w] + im[w] * cr[w];
                re[w] = nr;
                im[w] = ni;
                acc += bus[w] * ni;
            }

            if (percEnv > 1.0e-5f)
            {
                float p = 0.0f;
                for (int a = 0; a < numActive; ++a)
                {
                    const auto& k = keys[(size_t) active[(size_t) a]];
                    if (k.held || k.sustained) p += k.env * im[(size_t) taps[(size_t) active[(size_t) a]][percTap]];
                }
                acc += p * percEnv * percLevel * 1.2f;
                percEnv *= percC;
            }

            if (clickEnv > 1.0e-5f)
            {
                const float c = clickFilter.process (clickNoise.next(), clickG, 1.2f).band;
                acc += c * clickEnv * 2.5f;
                clickEnv *= clickCoeff;
            }

            out[i] = acc;
        }
    }

    void pruneKeys()
    {
        bool removed = false;
        for (int a = 0; a < numActive;)
        {
            auto& k = keys[(size_t) active[(size_t) a]];
            if (k.env <= 0.0f && k.target <= 0.0f && ! k.held && ! k.sustained)
            {
                k.listed = false;
                active[(size_t) a] = active[(size_t) --numActive];
                removed = true;
            }
            else ++a;
        }
        if (removed && numActive == 0) bus.fill (0.0f);     // no drift left over
    }

    void renormaliseWheels()
    {
        for (size_t w = 0; w < (size_t) padded; ++w)
        {
            const float mag = std::sqrt (re[w] * re[w] + im[w] * im[w]);
            re[w] /= mag;
            im[w] /= mag;
        }
    }

    void effects (float* l, float* r, int n)
    {
        const float sr = (float) sampleRate;
        // Scanner vibrato depths (ms) for V1..V3 / C1..C3.
        const float depths[] { 0.0f, 0.25f, 0.5f, 0.85f, 0.25f, 0.5f, 0.85f };
        const float vibDepth = depths[juce::jlimit (0, 6, vibMode)] * 0.001f * sr;
        const bool chorus = vibMode >= 4;
        const float vibInc = 6.9f / sr;
        const float drv = 1.0f + 6.0f * driveAmount * driveAmount;
        const float driveNorm = 1.0f / (1.0f + 0.9f * driveAmount);

        const bool fast = rotaryMode == 2 || (rotaryMode == 1 && modValue >= 0.5f);
        const float hornTarget = fast ? 6.7f : 0.8f, drumTarget = fast ? 5.8f : 0.67f;
        const float hornAccel = 1.0f / (0.6f * sr), drumAccel = 1.0f / (2.8f * sr);
        rotaryMix.set (rotaryMode > 0 ? 1.0f : 0.0f);
        const float hornMs = 0.001f * sr;

        for (int i = 0; i < n; ++i)
        {
            float x = l[i] * outGain;

            if (driveAmount > 0.001f)
                x = saturate (x * drv) * driveNorm;

            if (vibMode > 0)
            {
                vibLine.push (x);
                vibPhase += vibInc;
                if (vibPhase >= 1.0f) vibPhase -= 1.0f;
                const float wet = vibLine.read (2.0f + vibDepth * (0.5f + 0.5f * sinTurns (vibPhase)));
                x = chorus ? 0.6f * (x + wet) : wet;
            }

            x = toneFilter.process (x, toneG, 0.7f).low;

            const float mix = rotaryMix.next();
            float outL = x, outR = x;
            if (mix > 0.0001f)
            {
                hornSpeed += (hornTarget - hornSpeed) * hornAccel * 4.0f;
                drumSpeed += (drumTarget - drumSpeed) * drumAccel * 4.0f;
                hornAngle = wrap01 (hornAngle + hornSpeed / sr);
                drumAngle = wrap01 (drumAngle + drumSpeed / sr);

                const auto split = crossover.process (x, crossG, 0.707f);
                hornLine.push (split.high + split.band * 0.5f);
                drumLine.push (split.low + split.band * 0.5f);

                const float hs = sinTurns (hornAngle), hc = sinTurns (hornAngle + 0.25f);
                const float hs2 = sinTurns (hornAngle + 0.3f), hc2 = sinTurns (hornAngle + 0.55f);
                const float hornL = hornLine.read (2.0f + hornMs * (1.0f + 0.35f * hs)) * (1.0f + 0.45f * hc);
                const float hornR = hornLine.read (2.0f + hornMs * (1.0f + 0.35f * hs2)) * (1.0f + 0.45f * hc2);
                const float ds = sinTurns (drumAngle), dcos = sinTurns (drumAngle + 0.25f);
                const float drumL = drumLine.read (2.0f + hornMs * (1.0f + 0.12f * ds)) * (1.0f + 0.25f * dcos);
                const float drumR = drumLine.read (2.0f + hornMs * (1.0f - 0.12f * ds)) * (1.0f - 0.25f * dcos);

                outL = (1.0f - mix) * x + mix * 0.8f * (hornL + drumL);
                outR = (1.0f - mix) * x + mix * 0.8f * (hornR + drumR);
            }

            l[i] = outL;
            r[i] = outR;
        }

        reverb.process (l, r, n, roomAmount * 0.3f);
        safetyClip (l, r, n);
    }

    std::vector<float> scratchL, scratchR;
    std::array<float, padded> re {}, im {}, cr {}, ci {}, bus {};
    std::array<std::array<int, 9>, 128> taps {};
    std::array<Key, 128> keys {};
    std::array<int, 128> active {};
    int numActive = 0;
    std::array<float, 9> bars {}, barTarget {};

    int percMode = 0, vibMode = 0, rotaryMode = 0;
    float percLevel = 0.5f, clickLevel = 0.3f, driveAmount = 0.0f, toneG = 0.5f, roomAmount = 0.0f, outGain = 0.03f;
    float percEnv = 0.0f, clickEnv = 0.0f, clickCoeff = 0.99f, clickG = 0.2f, onRate = 0.01f, offRate = 0.01f;
    bool sustainDown = false;
    float modValue = 0.0f;
    Noise clickNoise;
    Svf clickFilter, toneFilter, crossover;
    float crossG = 0.05f;
    dsp::DelayBuffer vibLine, hornLine, drumLine;
    float vibPhase = 0.0f, hornAngle = 0.0f, drumAngle = 0.0f, hornSpeed = 0.8f, drumSpeed = 0.67f;
    Smooth rotaryMix;
    Room reverb;
};

//==============================================================================
// Clavinet: a short, bright struck string read by two pickups near the bridge (their
// positions comb the partials, so the pickup switch really changes the voice), muted by
// yarn the instant the key comes up. Optional per-voice envelope wah for funk.
struct ClavParams
{
    int pickup = 2;
    float mute = 0.0f, decayScale = 1.0f, wah = 0.0f, velSens = 0.7f, level = 1.0f;
};

class ClavVoice
{
public:
    static constexpr int maxPartials = 40;

    void prepare (double sr)                    { sampleRate = sr; starting.prepare (sr); }
    void start (int newNote, float velocity, bool stolen)   { starting.request (newNote, velocity, stolen); releasePending = releaseNow = false; }
    void release()                              { if (starting.pending) releasePending = true; else releaseNow = true; }
    void kill()                                 { starting.clear(); modes.clear(); active = false; releasePending = releaseNow = false; }
    bool isActive() const                       { return starting.pending || active; }

    template <typename Owner>
    void render (float* left, float* right, int n, const Owner& owner)
    {
        const ClavParams& p = owner.params;
        if (releaseNow) { mute(); releaseNow = false; }
        const float base = hzToNote (350.0f);

        for (int i = 0; i < n; ++i)
        {
            if (starting.pending && starting.tick()) begin (p);
            if (! active) break;

            float x = modes.next();
            if (p.wah > 0.001f)
            {
                const float cutoff = juce::jmin (130.0f, base + p.wah * 52.0f * wahEnv + 0.3f * ((float) note - 60.0f));
                x = wahFilter.process (x, Svf::gFor (noteToHz (cutoff), sampleRate), 0.3f).low * (1.0f - 0.3f * p.wah);
                wahEnv *= wahCoeff;
            }

            float out = x * gain;
            if (starting.fading) out *= juce::jmax (0.0f, starting.fade);
            left[i] += out * panL;
            right[i] += out * panR;
        }

        if (active && ! starting.pending && modes.level() < 2.0e-4f) { active = false; modes.clear(); }
    }

private:
    void begin (const ClavParams& p)
    {
        note = starting.note;
        const float key = (float) note;
        const float v = starting.velocity;
        const float f0 = noteToHz (key);
        const float sr = (float) sampleRate;
        const float strikeHz = 1400.0f + 9000.0f * std::pow (v, 1.4f);
        const float fundT60 = p.decayScale * (1.0f - 0.85f * p.mute) * juce::jlimit (0.5f, 7.0f, 6.0f * std::exp2 (-(key - 36.0f) / 22.0f));

        modes.clear();
        std::array<float, maxPartials> amps {};
        float energy = 0.0f;
        int count = 0;
        for (int k = 1; k <= maxPartials; ++k)
        {
            const float nk = (float) k;
            const float fk = nk * f0 * std::sqrt (1.0f + 1.5e-4f * nk * nk);
            if (fk > juce::jmin (0.42f * sr, 15000.0f)) break;

            const float neck = std::sin (pi * nk * 0.16f), bridge = std::sin (pi * nk * 0.055f);
            float pick = 0.0f;
            switch (p.pickup)
            {
                case 0:  pick = neck; break;
                case 1:  pick = bridge * 1.6f; break;
                case 3:  pick = neck - bridge * 1.6f; break;
                default: pick = neck + bridge * 1.6f; break;
            }

            const float a = std::pow (nk, -0.55f) / std::sqrt (1.0f + (fk / strikeHz) * (fk / strikeHz)) * pick;
            amps[(size_t) (k - 1)] = a;
            energy += a * a;
            count = k;

            const float loss = 6.9078f / fundT60 + 3.0f * (fk / 3500.0f) * (fk / 3500.0f);
            modes.setMode (k - 1, fk, 6.9078f / loss, sampleRate);
        }

        modes.setCount (count);
        const float norm = 1.0f / std::sqrt (juce::jmax (1.0e-6f, energy));
        for (int k = 0; k < count; ++k)
            modes.strike (k, (hash01 (note * 17 + k, 9) > 0.5f ? 1.0f : -1.0f) * amps[(size_t) k] * norm);

        wahFilter.reset();
        wahEnv = 0.4f + 0.6f * v;
        wahCoeff = decayCoeff (0.9f, sampleRate);
        gain = 0.14f * velocityGain (v, p.velSens) * p.level;
        std::tie (panL, panR) = panGains (juce::jlimit (-0.3f, 0.3f, (key - 64.0f) / 80.0f));
        active = true;
        starting.clear();
        if (releasePending) { mute(); releasePending = false; }
    }

    void mute()
    {
        for (int m = 0; m < modes.count; ++m)
            modes.setDecay (m, 0.07f / (1.0f + 0.05f * (float) m), sampleRate);
    }

    double sampleRate = 48000.0;
    PendingStart starting;
    ModalBank<maxPartials> modes;
    Svf wahFilter;
    float wahEnv = 0.0f, wahCoeff = 0.99f, gain = 0.0f, panL = 1.0f, panR = 1.0f;
    int note = -1;
    bool active = false, releasePending = false, releaseNow = false;
};

class Clavinet final : public PolyInstrument<Clavinet, ClavVoice, 16>
{
public:
    enum { pickup, tone, mute, decay, wah, drive, vel, level };

    ClavParams params;

    void update() override
    {
        params.pickup = juce::roundToInt (knob (pickup));
        params.mute = knob (mute);
        params.decayScale = knob (decay);
        params.wah = knob (wah);
        params.velSens = knob (vel);
        params.level = dbToGain (knob (level));
        toneG = Svf::gFor (knob (tone), sampleRate);
        driveAmount = knob (drive);
    }

protected:
    void resetEffects() override                { toneL.reset(); toneR.reset(); }
    void processEffects (float* l, float* r, int n) override
    {
        const float drv = 1.0f + 5.0f * driveAmount, norm = 1.0f / (1.0f + 1.2f * driveAmount);
        for (int i = 0; i < n; ++i)
        {
            float a = toneL.process (l[i], toneG, 0.75f).low, b = toneR.process (r[i], toneG, 0.75f).low;
            if (driveAmount > 0.001f) { a = saturate (a * drv) * norm; b = saturate (b * drv) * norm; }
            l[i] = a;
            r[i] = b;
        }
        safetyClip (l, r, n);
    }

private:
    Svf toneL, toneR;
    float toneG = 0.5f, driveAmount = 0.0f;
};

} // namespace

void registerKeyboardInstruments (InstrumentRegistry& r)
{
    using P = InstrumentInfo::Patch;

    r.add<GrandPiano> ({ "keys.piano", "Grand Piano", "Keyboards",
                         "Concert grand modelled string by string: stretched partials, hammer, dampers and a warm room",
                         juce::Colour (0xff3a3f4b),
                         { knob ("bright", "Brightness", -1.0f, 1.0f, 0.0f),
                           knob ("hammer", "Hammer", 0.0f, 1.0f, 0.4f, "%"),
                           knob ("decay", "Sustain", 0.3f, 2.0f, 1.0f, "x"),
                           knob ("damper", "Damper", 40.0f, 1200.0f, 180.0f, "ms", 0.5f),
                           knob ("strings", "Unison", 0.0f, 1.0f, 0.3f, "%"),
                           knob ("stretch", "Stretch", 0.0f, 2.0f, 1.0f, "x"),
                           knob ("vel", "Velocity", 0.0f, 1.0f, 0.8f, "%"),
                           knob ("width", "Width", 0.0f, 1.0f, 0.6f, "%"),
                           knob ("room", "Room", 0.0f, 1.0f, 0.3f, "%"),
                           knob ("roomsize", "Room Size", 0.3f, 2.5f, 1.4f, "x"),
                           knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") },
                         { P { "Concert Grand", "Pianos", {} },
                           P { "Bright Pop", "Pianos", { { "bright", 0.6f }, { "hammer", 0.55f }, { "room", 0.2f }, { "roomsize", 0.9f }, { "level", 0.0f } } },
                           P { "Mellow Ballad", "Pianos", { { "bright", -0.5f }, { "hammer", 0.25f }, { "decay", 1.3f }, { "room", 0.45f }, { "roomsize", 2.0f }, { "level", 0.0f } } },
                           P { "Dry Studio", "Pianos", { { "room", 0.0f }, { "width", 0.8f }, { "bright", 0.15f } } },
                           P { "Upright", "Pianos", { { "bright", -0.1f }, { "decay", 0.65f }, { "strings", 0.55f }, { "stretch", 1.5f }, { "width", 0.3f }, { "room", 0.15f }, { "roomsize", 0.6f } } },
                           P { "Honky Tonk", "Pianos", { { "strings", 0.95f }, { "decay", 0.7f }, { "bright", 0.3f }, { "hammer", 0.6f }, { "room", 0.15f }, { "roomsize", 0.7f }, { "level", 1.0f } } },
                           P { "Felt Piano", "Pianos", { { "bright", -1.0f }, { "hammer", 0.8f }, { "decay", 0.8f }, { "damper", 450.0f }, { "vel", 0.55f }, { "room", 0.5f }, { "roomsize", 1.8f }, { "level", 1.0f } } } } });

    r.add<ElectricPiano> ({ "keys.epiano", "Electric Piano", "Keyboards",
                            "Tine and reed electric pianos: bell-like when soft, barking when hit, with tremolo and chorus",
                            juce::Colour (0xffa8443a),
                            { choice ("model", "Model", { "Tine", "Reed", "FM Bell" }, 0),
                              knob ("bark", "Bark", 0.0f, 1.0f, 0.5f, "%"),
                              knob ("bell", "Bell", 0.0f, 1.0f, 0.5f, "%"),
                              knob ("decay", "Sustain", 0.3f, 2.0f, 1.0f, "x"),
                              knob ("release", "Release", 30.0f, 1500.0f, 140.0f, "ms", 0.5f),
                              knob ("tremolo", "Tremolo", 0.0f, 1.0f, 0.0f, "%"),
                              knob ("tremrate", "Trem Rate", 0.5f, 10.0f, 4.5f, "Hz", 0.6f),
                              knob ("stereo", "Auto-Pan", 0.0f, 1.0f, 1.0f, "%"),
                              knob ("tone", "Tone", 1000.0f, 16000.0f, 9000.0f, "Hz", 0.4f),
                              knob ("chorus", "Chorus", 0.0f, 1.0f, 0.0f, "%"),
                              knob ("vel", "Velocity", 0.0f, 1.0f, 0.7f, "%"),
                              knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") },
                            { P { "Stage Tines", "Electric Pianos", {} },
                              P { "Suitcase", "Electric Pianos", { { "tremolo", 0.55f }, { "tremrate", 4.0f }, { "stereo", 1.0f }, { "bark", 0.35f }, { "tone", 7000.0f } } },
                              P { "Bright Tines", "Electric Pianos", { { "bell", 0.9f }, { "bark", 0.65f }, { "chorus", 0.4f }, { "tone", 14000.0f }, { "level", -1.5f } } },
                              P { "Dark Soul", "Electric Pianos", { { "bell", 0.2f }, { "bark", 0.3f }, { "tone", 3000.0f }, { "tremolo", 0.25f }, { "tremrate", 3.0f }, { "level", 0.0f } } },
                              P { "Reed", "Electric Pianos", { { "model", 1 }, { "bark", 0.5f }, { "bell", 0.3f }, { "tone", 8000.0f } } },
                              P { "Reed Tremolo", "Electric Pianos", { { "model", 1 }, { "bark", 0.6f }, { "tremolo", 0.6f }, { "tremrate", 5.5f }, { "stereo", 0.0f }, { "level", 1.0f } } },
                              P { "FM Ballad", "Electric Pianos", { { "model", 2 }, { "bell", 0.7f }, { "bark", 0.4f }, { "chorus", 0.6f }, { "decay", 1.2f } } },
                              P { "FM Glass", "Electric Pianos", { { "model", 2 }, { "bell", 1.0f }, { "bark", 0.2f }, { "chorus", 0.4f }, { "decay", 1.5f }, { "level", -1.0f } } } } });

    r.add<TonewheelOrgan> ({ "keys.organ", "Tonewheel Organ", "Keyboards",
                             "Drawbar tonewheel organ with percussion, key click, scanner vibrato and a rotary speaker",
                             juce::Colour (0xff7a4b2a),
                             { knob ("d16", "16'", 0.0f, 8.0f, 8.0f),
                               knob ("d513", "5 1/3'", 0.0f, 8.0f, 8.0f),
                               knob ("d8", "8'", 0.0f, 8.0f, 8.0f),
                               knob ("d4", "4'", 0.0f, 8.0f, 0.0f),
                               knob ("d223", "2 2/3'", 0.0f, 8.0f, 0.0f),
                               knob ("d2", "2'", 0.0f, 8.0f, 0.0f),
                               knob ("d135", "1 3/5'", 0.0f, 8.0f, 0.0f),
                               knob ("d113", "1 1/3'", 0.0f, 8.0f, 0.0f),
                               knob ("d1", "1'", 0.0f, 8.0f, 0.0f),
                               choice ("perc", "Percussion", { "Off", "2nd Fast", "2nd Slow", "3rd Fast", "3rd Slow" }, 3),
                               knob ("perclevel", "Perc Level", 0.0f, 1.0f, 0.6f, "%"),
                               knob ("click", "Key Click", 0.0f, 1.0f, 0.35f, "%"),
                               choice ("vibrato", "Vibrato", { "Off", "V1", "V2", "V3", "C1", "C2", "C3" }, 6),
                               knob ("drive", "Drive", 0.0f, 1.0f, 0.15f, "%"),
                               choice ("rotary", "Rotary", { "Off", "Slow", "Fast" }, 1),
                               knob ("tone", "Tone", 1500.0f, 12000.0f, 7000.0f, "Hz", 0.5f),
                               knob ("room", "Room", 0.0f, 1.0f, 0.15f, "%"),
                               knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") },
                             { P { "Jazz Trio", "Organs", {} },
                               P { "Gospel Full", "Organs", { { "d16", 8.0f }, { "d513", 8.0f }, { "d8", 8.0f }, { "d4", 8.0f }, { "d223", 0.0f }, { "d2", 8.0f },
                                                              { "d135", 0.0f }, { "d113", 0.0f }, { "d1", 8.0f }, { "perc", 0 }, { "drive", 0.3f }, { "level", -4.5f } } },
                               P { "Rock Overdrive", "Organs", { { "d4", 6.0f }, { "d223", 4.0f }, { "d2", 6.0f }, { "perc", 0 }, { "drive", 0.75f }, { "rotary", 2 },
                                                                 { "vibrato", 0 }, { "click", 0.5f }, { "level", -6.5f } } },
                               P { "Soul Ballad", "Organs", { { "d16", 6.0f }, { "d513", 8.0f }, { "d8", 8.0f }, { "d4", 6.0f }, { "perc", 0 }, { "vibrato", 5 },
                                                              { "drive", 0.05f }, { "click", 0.15f }, { "level", -1.0f } } },
                               P { "Bright Full", "Organs", { { "d16", 8.0f }, { "d513", 8.0f }, { "d8", 8.0f }, { "d4", 8.0f }, { "d223", 8.0f }, { "d2", 8.0f },
                                                              { "d135", 8.0f }, { "d113", 8.0f }, { "d1", 8.0f }, { "perc", 0 }, { "level", -8.0f } } },
                               P { "Clean Combo", "Organs", { { "d16", 8.0f }, { "d513", 8.0f }, { "d8", 6.0f }, { "perc", 1 }, { "vibrato", 0 }, { "rotary", 0 },
                                                              { "drive", 0.0f }, { "level", 0.0f } } },
                               P { "Mellow Flute", "Organs", { { "d16", 0.0f }, { "d513", 0.0f }, { "d8", 8.0f }, { "d4", 4.0f }, { "perc", 0 }, { "vibrato", 3 },
                                                               { "click", 0.1f }, { "drive", 0.0f }, { "tone", 5000.0f }, { "level", 5.0f } } },
                               P { "Cathedral", "Organs", { { "d16", 8.0f }, { "d513", 4.0f }, { "d8", 8.0f }, { "d4", 7.0f }, { "d223", 3.0f }, { "d2", 6.0f },
                                                            { "d1", 4.0f }, { "perc", 0 }, { "vibrato", 0 }, { "rotary", 0 }, { "click", 0.0f },
                                                            { "drive", 0.0f }, { "room", 0.7f }, { "level", -5.0f } } } } });

    r.add<Clavinet> ({ "keys.clav", "Clavinet", "Keyboards",
                       "Funky struck-string clavinet with pickup selector, mute and envelope wah",
                       juce::Colour (0xffc9a227),
                       { choice ("pickup", "Pickups", { "Neck", "Bridge", "Both", "Both Inverted" }, 2),
                         knob ("tone", "Tone", 800.0f, 16000.0f, 9000.0f, "Hz", 0.4f),
                         knob ("mute", "Mute", 0.0f, 1.0f, 0.0f, "%"),
                         knob ("decay", "Sustain", 0.3f, 2.0f, 1.0f, "x"),
                         knob ("wah", "Wah", 0.0f, 1.0f, 0.0f, "%"),
                         knob ("drive", "Drive", 0.0f, 1.0f, 0.0f, "%"),
                         knob ("vel", "Velocity", 0.0f, 1.0f, 0.7f, "%"),
                         knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") },
                       { P { "Funky Clav", "Clavs", {} },
                         P { "Bridge Bite", "Clavs", { { "pickup", 1 }, { "tone", 12000.0f }, { "drive", 0.25f } } },
                         P { "Muted Clav", "Clavs", { { "mute", 0.75f }, { "level", 1.0f } } },
                         P { "Wah Clav", "Clavs", { { "wah", 0.8f }, { "pickup", 3 }, { "drive", 0.2f }, { "level", 2.0f } } },
                         P { "Mellow Neck", "Clavs", { { "pickup", 0 }, { "tone", 4000.0f }, { "level", 1.0f } } },
                         P { "Phase Clav", "Clavs", { { "pickup", 3 }, { "tone", 11000.0f } } } } });
}

} // namespace woodshed
