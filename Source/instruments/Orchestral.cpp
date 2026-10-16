#include "InstrumentDsp.h"

namespace woodshed
{
void registerPluckedInstruments (InstrumentRegistry&);     // Plucked.cpp

namespace
{
using namespace idsp;

inline float ms (float v)                       { return v * 0.001f; }

/** Unity-peak bandpass from an SVF: band output scaled by k. */
inline float bandpass (Svf& f, float x, float g, float k)  { return f.process (x, g, k).band * k; }

/** Precomputed SVF coefficients, for filters whose cutoff only changes at control rate. */
struct SvfCoeffs
{
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f;
    void set (float g, float newK)              { k = newK; a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2; }
};

struct SvfState
{
    float ic1 = 0.0f, ic2 = 0.0f;
    void reset()                                { ic1 = ic2 = 0.0f; }

    /** Returns the lowpass; `band` receives the bandpass. */
    float process (float v0, const SvfCoeffs& c, float& band)
    {
        const float v3 = v0 - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;
        const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        band = v1;
        return v2;
    }

    float low (float x, const SvfCoeffs& c)     { float b; return process (x, c, b); }
    float bandUnity (float x, const SvfCoeffs& c) { float b; process (x, c, b); return b * c.k; }
};

constexpr int controlRate = 8;      // samples between filter cutoff updates

/** Orchestral seating: high voices a little left, low voices a little right. */
inline std::pair<float, float> seatPan (int note, float width)
{
    return panGains (juce::jlimit (-1.0f, 1.0f, -(float) (note - 60) / 36.0f) * width);
}

/** The effects every section shares: ensemble, a hall-ish room and the output safety clip. */
struct SectionFx
{
    Ensemble ensemble;
    Room room;
    float ensembleMix = 0.0f, roomMix = 0.0f;

    void prepare (double sr, float rt60, float dampHz)  { ensemble.prepare (sr); room.prepare (sr); room.setDecay (rt60, dampHz); }
    void reset()                                        { ensemble.reset(); room.reset(); }

    void process (float* l, float* r, int n)
    {
        ensemble.process (l, r, n, ensembleMix);
        room.process (l, r, n, roomMix);
        safetyClip (l, r, n);
    }
};

//==============================================================================
// String Ensemble: a section of detuned bowed saws per note, split across the stereo
// field, each player with its own vibrato; the tone darkens as the bow eases off, and a
// body resonance bank gives it wood.
struct StringParams
{
    int players = 4;
    float detuneCents = 8.0f, brightNote = 100.0f, velBright = 0.5f;
    float attack = 0.2f, decay = 1.0f, sustain = 1.0f, release = 0.5f;
    float vibDepth = 0.15f, vibRate = 5.5f, vibDelay = 0.3f, trem = 0.0f, tremRate = 10.0f;
    float bowNoise = 0.1f, body = 0.5f, level = 1.0f, velSens = 0.5f;
    std::array<SvfCoeffs, 3> bodyC {};
    std::array<float, 3> bodyGain {};
};

class StringVoice
{
public:
    static constexpr int maxPlayers = 6;

    void prepare (double sr)
    {
        sampleRate = sr;
        starting.prepare (sr);
        rng.setSeed ((uint32_t) (reinterpret_cast<uintptr_t> (this) >> 4));
    }

    void start (int newNote, float velocity, bool stolen)  { starting.request (newNote, velocity, stolen); releasePending = false; }
    void release()                                       { if (starting.pending) releasePending = true; amp.noteOff(); }
    void kill()                                          { starting.clear(); amp.kill(); releasePending = false; }
    bool isActive() const                                { return starting.pending || amp.isActive(); }

    template <typename Owner>
    void render (float* left, float* right, int n, const Owner& owner)
    {
        const StringParams& p = owner.params;
        const float sr = (float) sampleRate;
        amp.set (sampleRate, p.attack, p.decay, p.sustain, p.release);
        const float bend = owner.bend();
        float baseInc = noteToHz ((float) note + bend) / sr;
        const float vibMax = p.vibDepth + owner.mod() * 0.35f;
        const float vibRamp = 1.0f / (juce::jmax (0.01f, p.vibDelay) * sr);
        const float tremInc = p.tremRate / sr;
        const float noiseDecay = std::exp (-1.0f / (0.06f * sr));
        SvfCoeffs lowCut;
        lowCut.set (Svf::gFor (noteToHz ((float) note) * 1.2f, sampleRate), 1.0f);

        for (int i = 0; i < n; ++i)
        {
            if (starting.pending && starting.tick())
            {
                begin (p);
                baseInc = noteToHz ((float) note + bend) / sr;
                lowCut.set (Svf::gFor (noteToHz ((float) note) * 1.2f, sampleRate), 1.0f);
            }
            if (! amp.isActive() && ! starting.pending) break;

            const float env = amp.next();
            vibAmount = juce::jmin (1.0f, vibAmount + vibRamp);
            const bool vibUpdate = vibTick-- <= 0;
            if (vibUpdate) vibTick = controlRate - 1;
            tremPhase += tremInc;
            if (tremPhase >= 1.0f) tremPhase -= 1.0f;

            float sumL = 0.0f, sumR = 0.0f;
            for (int k = 0; k < players; ++k)
            {
                const auto j = (size_t) k;
                if (vibUpdate)
                {
                    vibPhase[j] += vibInc[j] * (float) controlRate;
                    if (vibPhase[j] >= 1.0f) vibPhase[j] -= 1.0f;
                    vibMul[j] = 1.0f + 0.0578f * vibAmount * vibMax * sinTurns (vibPhase[j]);
                }
                float s = osc[j].next (baseInc * ratio[j] * vibMul[j], Osc::saw);
                if (p.trem > 0.0f)
                    s *= 1.0f - p.trem * (0.5f + 0.5f * sinTurns (tremPhase + 0.29f * (float) k));
                (k % 2 == 0 ? sumL : sumR) += s;
            }

            noiseEnv *= noiseDecay;
            const float bow = p.bowNoise * (0.25f + 2.0f * noiseEnv) * rng.next();
            sumL += bow;
            sumR += bow;

            const float cutNote = p.brightNote + 0.5f * ((float) note - 60.0f) + velCut + 14.0f * (env - 1.0f);
            if (controlCount-- <= 0)
            {
                controlCount = controlRate - 1;
                cut.set (Svf::gFor (noteToHz (juce::jmin (cutNote, 135.0f)), sampleRate), 1.0f / 1.1f);
            }
            const float yL = chain (0, sumL, lowCut, p);
            const float yR = players > 1 ? chain (1, sumR, lowCut, p) : yL;

            const float out = env * gain * p.level * (starting.fading ? juce::jmax (0.0f, starting.fade) : 1.0f);
            const float l = (yL + 0.35f * yR) * out, r = (yR + 0.35f * yL) * out;
            left[i] += l * panL;
            right[i] += r * panR;
        }
    }

private:
    float chain (int side, float x, const SvfCoeffs& lowCut, const StringParams& p)
    {
        auto& c = sides[(size_t) side];
        // Bowed strings have little true fundamental energy below the body; a gentle high-pass shapes that.
        const float lp = c.lp.low (x, cut);
        const float y = lp - 0.4f * c.hp.low (lp, lowCut);
        float body = 0.0f;
        for (size_t b = 0; b < 3; ++b)
            body += p.bodyGain[b] * c.body[b].bandUnity (y, p.bodyC[b]);
        return y + p.body * body;
    }

    void begin (const StringParams& p)
    {
        note = starting.note;
        const float v = starting.velocity;
        players = juce::jlimit (1, maxPlayers, p.players);
        gain = 0.12f * velocityGain (v, p.velSens) / std::sqrt ((float) players);
        velCut = p.velBright * (v - 0.7f) * 24.0f;
        std::tie (panL, panR) = seatPan (note, 0.5f);

        for (int k = 0; k < players; ++k)
        {
            const auto j = (size_t) k;
            const float spread = players == 1 ? 0.0f : 2.0f * (float) k / (float) (players - 1) - 1.0f;
            const float cents = p.detuneCents * (spread + 0.3f * rng.next());
            ratio[j] = semitones (cents * 0.01f);
            vibInc[j] = p.vibRate * (0.88f + 0.24f * rng.uniform()) / (float) sampleRate;
            vibPhase[j] = rng.uniform();
            osc[j].phase = rng.uniform();
        }

        for (auto& c : sides) { c.lp.reset(); c.hp.reset(); for (auto& b : c.body) b.reset(); }
        controlCount = 0;
        vibAmount = 0.0f;
        noiseEnv = 1.0f;
        vibTick = 0;
        vibMul.fill (1.0f);
        tremPhase = rng.uniform();
        amp.kill();
        amp.trigger();
        starting.clear();
        if (releasePending) { amp.noteOff(); releasePending = false; }
    }

    struct Side { SvfState lp, hp; std::array<SvfState, 3> body; };

    double sampleRate = 48000.0;
    PendingStart starting;
    Noise rng;
    Adsr amp;
    std::array<Osc, maxPlayers> osc;
    std::array<float, maxPlayers> ratio {}, vibInc {}, vibPhase {}, vibMul {};
    std::array<Side, 2> sides;
    SvfCoeffs cut;
    int controlCount = 0;
    int note = 60, players = 4;
    int vibTick = 0;
    float gain = 0.0f, velCut = 0.0f, panL = 1.0f, panR = 1.0f, vibAmount = 0.0f, noiseEnv = 0.0f, tremPhase = 0.0f;
    bool releasePending = false;
};

class StringEnsemble final : public PolyInstrument<StringEnsemble, StringVoice, 16>
{
public:
    enum { section, timbre, detune, bright, velbright, attack, decay, sustain, release, vibrato, vibrate,
           vibdelay, tremolo, tremrate, bow, body, ensemble, room, velocity, level };

    StringParams params;

    void update() override
    {
        auto& p = params;
        const int sec = juce::roundToInt (knob (section));
        p.players = sec == 0 ? 1 : (sec == 1 ? 3 : 6);
        p.detuneCents = knob (detune) * 22.0f;
        p.brightNote = hzToNote (knob (bright));
        p.velBright = knob (velbright);
        p.attack = ms (knob (attack));
        p.decay = ms (knob (decay));
        p.sustain = knob (sustain);
        p.release = ms (knob (release));
        p.vibDepth = knob (vibrato) * 0.45f;
        p.vibRate = knob (vibrate);
        p.vibDelay = ms (knob (vibdelay));
        p.trem = knob (tremolo);
        p.tremRate = knob (tremrate);
        p.bowNoise = knob (bow) * 0.25f;
        p.body = knob (body) * 1.5f;
        p.velSens = knob (velocity);
        p.level = dbToGain (knob (level));

        static constexpr float freqs[4][3] { { 290.0f, 540.0f, 2600.0f }, { 230.0f, 440.0f, 2100.0f },
                                             { 125.0f, 290.0f, 1600.0f }, { 70.0f, 170.0f, 1000.0f } };
        static constexpr float qs[3] { 3.0f, 2.5f, 1.6f };
        static constexpr float gains[3] { 0.6f, 0.8f, 0.9f };
        const auto t = (size_t) juce::jlimit (0, 3, juce::roundToInt (knob (timbre)));
        for (size_t b = 0; b < 3; ++b)
        {
            p.bodyC[b].set (Svf::gFor (freqs[t][b], sampleRate), 1.0f / qs[b]);
            p.bodyGain[b] = gains[b];
        }

        fx.ensembleMix = knob (ensemble);
        fx.roomMix = knob (room) * 0.6f;
    }

protected:
    void prepareEffects() override                      { fx.prepare (sampleRate, 1.9f, 5500.0f); }
    void resetEffects() override                        { fx.reset(); }
    void processEffects (float* l, float* r, int n) override  { fx.process (l, r, n); }

private:
    SectionFx fx;
};

//==============================================================================
// Brass Section: saw players behind a lowpass that opens as the note swells and with how
// hard it's played, a lip scoop into pitch, growl, and optional mellow/muted bells.
struct BrassParams
{
    int players = 3, mute = 0;
    float detuneCents = 6.0f, brightNote = 80.0f, swell = 30.0f, velBright = 0.6f;
    float attack = 0.05f, decay = 0.6f, sustain = 0.8f, release = 0.25f;
    float scoop = 1.0f, growl = 0.2f, vibDepth = 0.1f, vibRate = 5.0f, breath = 0.1f, level = 1.0f, velSens = 0.5f;
};

class BrassVoice
{
public:
    static constexpr int maxPlayers = 3;

    void prepare (double sr)
    {
        sampleRate = sr;
        starting.prepare (sr);
        rng.setSeed ((uint32_t) (reinterpret_cast<uintptr_t> (this) >> 4) + 77u);
    }

    void start (int newNote, float velocity, bool stolen)  { starting.request (newNote, velocity, stolen); releasePending = false; }
    void release()                                       { if (starting.pending) releasePending = true; amp.noteOff(); swellEnv.noteOff(); }
    void kill()                                          { starting.clear(); amp.kill(); swellEnv.kill(); releasePending = false; }
    bool isActive() const                                { return starting.pending || amp.isActive(); }

    template <typename Owner>
    void render (float* left, float* right, int n, const Owner& owner)
    {
        const BrassParams& p = owner.params;
        const float sr = (float) sampleRate;
        amp.set (sampleRate, p.attack, p.decay, p.sustain, p.release);
        swellEnv.set (sampleRate, p.attack * 1.6f + 0.03f, p.decay * 1.5f, 0.6f, p.release * 0.8f);
        const float bend = owner.bend();
        const float vibMax = p.vibDepth + owner.mod() * 0.35f;
        const float vibInc = p.vibRate / sr, vibRamp = 1.0f / (0.35f * sr);
        const float scoopDecay = std::exp (-1.0f / (0.045f * sr));
        const float flutterC = onePole (40.0f, sampleRate);
        const float gMellow = onePole (1400.0f, sampleRate);
        const float gMute = Svf::gFor (1700.0f, sampleRate);
        const float gHp = Svf::gFor (700.0f, sampleRate);

        for (int i = 0; i < n; ++i)
        {
            if (starting.pending && starting.tick()) { begin (p); controlCount = 0; }
            if (! amp.isActive() && ! starting.pending) break;

            const float env = amp.next(), sw = swellEnv.next();
            scoopEnv *= scoopDecay;
            vibAmount = juce::jmin (1.0f, vibAmount + vibRamp);
            vibPhase += vibInc;
            if (vibPhase >= 1.0f) vibPhase -= 1.0f;

            const float inc = noteToHz ((float) note + bend - scoopAmount * scoopEnv + vibAmount * vibMax * sinTurns (vibPhase)) / sr;
            float x = 0.0f;
            for (int k = 0; k < players; ++k)
                x += osc[(size_t) k].next (inc * ratio[(size_t) k], Osc::saw);
            x *= playerNorm;

            const float flutter = flutterLp.process (rng.next(), flutterC);
            x *= 1.0f + p.growl * 3.0f * flutter;
            x = saturate (x * (1.0f + 2.5f * p.growl * sw)) + p.breath * 0.25f * rng.next() * sw;

            const float cutNote = p.brightNote + 0.6f * ((float) note - 60.0f) + p.swell * sw * velFactor;
            if (controlCount-- <= 0)
            {
                controlCount = controlRate - 1;
                cutG = Svf::gFor (noteToHz (juce::jmin (cutNote, 135.0f)), sampleRate);
            }
            float y = filter.process (x, cutG, 0.15f);

            if (p.mute == 1)      y = mellow.process (y, gMellow) * 1.3f;
            else if (p.mute == 2) y = 2.0f * bandpass (muteBp, y - hp.process (y, gHp, 1.0f).low, gMute, 0.45f);

            const float out = y * env * gain * p.level * (starting.fading ? juce::jmax (0.0f, starting.fade) : 1.0f);
            left[i] += out * panL;
            right[i] += out * panR;
        }
    }

private:
    void begin (const BrassParams& p)
    {
        note = starting.note;
        const float v = starting.velocity;
        players = juce::jlimit (1, maxPlayers, p.players);
        playerNorm = 1.0f / std::sqrt ((float) players);
        gain = 0.15f * velocityGain (v, p.velSens);
        velFactor = (1.0f - p.velBright) + p.velBright * v;
        scoopAmount = p.scoop * (1.2f - 0.5f * v);
        std::tie (panL, panR) = seatPan (note, 0.35f);

        for (int k = 0; k < players; ++k)
        {
            const auto j = (size_t) k;
            const float spread = players == 1 ? 0.0f : 2.0f * (float) k / (float) (players - 1) - 1.0f;
            ratio[j] = semitones (p.detuneCents * 0.01f * (spread + 0.25f * rng.next()));
            osc[j].phase = rng.uniform();
        }

        filter.reset(); mellow.reset(); muteBp.reset(); hp.reset(); flutterLp.reset();
        scoopEnv = 1.0f;
        vibAmount = 0.0f;
        vibPhase = rng.uniform();
        amp.kill(); swellEnv.kill();
        amp.trigger(); swellEnv.trigger();
        starting.clear();
        if (releasePending) { amp.noteOff(); swellEnv.noteOff(); releasePending = false; }
    }

    double sampleRate = 48000.0;
    PendingStart starting;
    Noise rng;
    Adsr amp, swellEnv;
    std::array<Osc, maxPlayers> osc;
    std::array<float, maxPlayers> ratio {};
    Lowpass24 filter;
    float cutG = 0.1f;
    int controlCount = 0;
    OnePoleLp mellow, flutterLp;
    Svf muteBp, hp;
    int note = 60, players = 3;
    float gain = 0.0f, playerNorm = 1.0f, velFactor = 1.0f, scoopAmount = 0.0f, scoopEnv = 0.0f;
    float vibAmount = 0.0f, vibPhase = 0.0f, panL = 1.0f, panR = 1.0f;
    bool releasePending = false;
};

class BrassSection final : public PolyInstrument<BrassSection, BrassVoice, 16>
{
public:
    enum { section, bell, detune, bright, swell, velbright, attack, decay, sustain, release,
           scoop, growl, breath, vibrato, vibrate, ensemble, room, velocity, level };

    BrassParams params;

    void update() override
    {
        auto& p = params;
        p.players = 1 + juce::roundToInt (knob (section));
        p.mute = juce::roundToInt (knob (bell));
        p.detuneCents = knob (detune) * 20.0f;
        p.brightNote = hzToNote (knob (bright));
        p.swell = knob (swell) * 48.0f;
        p.velBright = knob (velbright);
        p.attack = ms (knob (attack));
        p.decay = ms (knob (decay));
        p.sustain = knob (sustain);
        p.release = ms (knob (release));
        p.scoop = knob (scoop);
        p.growl = knob (growl);
        p.breath = knob (breath);
        p.vibDepth = knob (vibrato) * 0.4f;
        p.vibRate = knob (vibrate);
        p.velSens = knob (velocity);
        p.level = dbToGain (knob (level));
        fx.ensembleMix = knob (ensemble);
        fx.roomMix = knob (room) * 0.6f;
    }

protected:
    void prepareEffects() override                      { fx.prepare (sampleRate, 1.6f, 6000.0f); }
    void resetEffects() override                        { fx.reset(); }
    void processEffects (float* l, float* r, int n) override  { fx.process (l, r, n); }

private:
    SectionFx fx;
};

//==============================================================================
// Choir: glottal-ish saw singers through five vowel formants that can morph and drift,
// with breath, a section's worth of detune and vibrato, and an optional octave-up layer.
struct Vowel { float f[5], db[5], bw[5]; };

constexpr Vowel vowels[4] {
    { { 300.0f, 870.0f, 2240.0f, 3000.0f, 3600.0f },  { 0.0f, -8.0f, -26.0f, -30.0f, -38.0f },  { 55.0f, 80.0f, 110.0f, 130.0f, 140.0f } },   // Ooh
    { { 730.0f, 1090.0f, 2440.0f, 3400.0f, 4000.0f }, { 0.0f, -4.0f, -16.0f, -22.0f, -32.0f },  { 80.0f, 90.0f, 120.0f, 140.0f, 160.0f } },   // Aah
    { { 400.0f, 2000.0f, 2650.0f, 3400.0f, 4100.0f }, { 0.0f, -10.0f, -13.0f, -20.0f, -30.0f }, { 60.0f, 100.0f, 120.0f, 140.0f, 160.0f } },  // Eeh
    { { 250.0f, 1100.0f, 2300.0f, 3200.0f, 4000.0f }, { 0.0f, -24.0f, -34.0f, -40.0f, -46.0f }, { 60.0f, 100.0f, 130.0f, 150.0f, 160.0f } },  // Mmm
};

struct ChoirParams
{
    int singers = 3, vowelA = 1, vowelB = 0;
    float morph = 0.0f, motion = 0.0f, formantShift = 1.0f, detuneCents = 10.0f;
    float vibDepth = 0.15f, vibRate = 5.0f, breath = 0.1f, brightHz = 3000.0f, octaveUp = 0.0f;
    float attack = 0.3f, decay = 1.0f, sustain = 1.0f, release = 0.8f, level = 1.0f, velSens = 0.4f;
};

class ChoirVoice
{
public:
    static constexpr int maxSingers = 7;     // five plus two octave-up

    void prepare (double sr)
    {
        sampleRate = sr;
        starting.prepare (sr);
        rng.setSeed ((uint32_t) (reinterpret_cast<uintptr_t> (this) >> 4) + 991u);
    }

    void start (int newNote, float velocity, bool stolen)  { starting.request (newNote, velocity, stolen); releasePending = false; }
    void release()                                       { if (starting.pending) releasePending = true; amp.noteOff(); }
    void kill()                                          { starting.clear(); amp.kill(); releasePending = false; }
    bool isActive() const                                { return starting.pending || amp.isActive(); }

    template <typename Owner>
    void render (float* left, float* right, int n, const Owner& owner)
    {
        const ChoirParams& p = owner.params;
        const float sr = (float) sampleRate;
        amp.set (sampleRate, p.attack, p.decay, p.sustain, p.release);
        setFormants (p, n);

        const float bend = owner.bend();
        float baseInc = noteToHz ((float) note + bend) / sr;
        const float vibMax = p.vibDepth + owner.mod() * 0.3f;
        const float vibRamp = 1.0f / (0.4f * sr);
        float tilt = onePole (p.brightHz * brightScale, sampleRate);
        const float breathC = onePole (2500.0f, sampleRate);

        for (int i = 0; i < n; ++i)
        {
            if (starting.pending && starting.tick())
            {
                begin (p);
                baseInc = noteToHz ((float) note + bend) / sr;
                tilt = onePole (p.brightHz * brightScale, sampleRate);
            }
            if (! amp.isActive() && ! starting.pending) break;

            const float env = amp.next();
            vibAmount = juce::jmin (1.0f, vibAmount + vibRamp);
            const bool vibUpdate = vibTick-- <= 0;
            if (vibUpdate) vibTick = controlRate - 1;

            float src[2] = { 0.0f, 0.0f };
            for (int k = 0; k < singers; ++k)
            {
                const auto j = (size_t) k;
                if (vibUpdate)
                {
                    vibPhase[j] += vibInc[j] * (float) controlRate;
                    if (vibPhase[j] >= 1.0f) vibPhase[j] -= 1.0f;
                    vibMul[j] = 1.0f + 0.0578f * vibAmount * vibMax * sinTurns (vibPhase[j]);
                }
                src[k % 2] += singerGain[j] * osc[j].next (baseInc * ratio[j] * vibMul[j], Osc::saw);
            }

            const float air = p.breath * (rng.next() - breathLp.process (rng.next(), breathC) * 0.5f);
            float out[2];
            for (size_t s = 0; s < 2; ++s)
            {
                const float x = tiltLp[s].process (src[s], tilt) + air;
                float y = 0.0f;
                for (size_t f = 0; f < 5; ++f)
                    y += fGain[f] * formant[s][f].bandUnity (x, fC[f]);
                out[s] = y;
            }

            const float g = env * gain * p.level * (starting.fading ? juce::jmax (0.0f, starting.fade) : 1.0f);
            left[i] += (out[0] + 0.3f * out[1]) * g * panL;
            right[i] += (out[1] + 0.3f * out[0]) * g * panR;
        }
    }

private:
    /** Interpolated formants for this block, with each voice's own slow vowel drift. */
    void setFormants (const ChoirParams& p, int n)
    {
        motionPhase += (float) n * motionRate / (float) sampleRate;
        if (motionPhase >= 1.0f) motionPhase -= 1.0f;
        const float m = juce::jlimit (0.0f, 1.0f, p.morph + p.motion * 0.5f * (1.0f + sinTurns (motionPhase)));
        const auto& a = vowels[(size_t) p.vowelA];
        const auto& b = vowels[(size_t) p.vowelB];
        for (size_t f = 0; f < 5; ++f)
        {
            const float hz = a.f[f] * std::pow (b.f[f] / a.f[f], m) * p.formantShift;
            const float bw = a.bw[f] + m * (b.bw[f] - a.bw[f]);
            fC[f].set (Svf::gFor (hz, sampleRate), juce::jlimit (0.02f, 1.5f, bw / hz));
            fGain[f] = dbToGain (a.db[f] + m * (b.db[f] - a.db[f]));
        }
    }

    void begin (const ChoirParams& p)
    {
        note = starting.note;
        const float v = starting.velocity;
        const int base = juce::jlimit (1, 5, p.singers);
        const int extra = p.octaveUp > 0.01f ? 2 : 0;
        singers = base + extra;
        float total = 0.0f;

        for (int k = 0; k < singers; ++k)
        {
            const auto j = (size_t) k;
            const bool up = k >= base;
            const float spread = base == 1 ? 0.0f : 2.0f * (float) (k % base) / (float) (base - 1) - 1.0f;
            ratio[j] = (up ? 2.0f : 1.0f) * semitones (p.detuneCents * 0.01f * (spread + 0.3f * rng.next()));
            vibInc[j] = p.vibRate * (0.85f + 0.3f * rng.uniform()) / (float) sampleRate;
            vibPhase[j] = rng.uniform();
            osc[j].phase = rng.uniform();
            singerGain[j] = up ? p.octaveUp * 0.8f : 1.0f;
            total += singerGain[j] * singerGain[j];
        }

        gain = 0.4f * velocityGain (v, p.velSens) / std::sqrt (juce::jmax (1.0f, total));
        brightScale = 0.6f + 0.4f * v;
        std::tie (panL, panR) = seatPan (note, 0.4f);
        for (auto& side : formant) for (auto& f : side) f.reset();
        for (auto& t : tiltLp) t.reset();
        breathLp.reset();
        vibTick = 0;
        vibMul.fill (1.0f);
        vibAmount = 0.0f;
        motionRate = 0.13f + 0.12f * rng.uniform();
        motionPhase = rng.uniform();
        amp.kill();
        amp.trigger();
        starting.clear();
        if (releasePending) { amp.noteOff(); releasePending = false; }
    }

    double sampleRate = 48000.0;
    PendingStart starting;
    Noise rng;
    Adsr amp;
    std::array<Osc, maxSingers> osc;
    std::array<float, maxSingers> ratio {}, vibInc {}, vibPhase {}, vibMul {}, singerGain {};
    std::array<std::array<SvfState, 5>, 2> formant;
    std::array<OnePoleLp, 2> tiltLp;
    OnePoleLp breathLp;
    std::array<SvfCoeffs, 5> fC {};
    std::array<float, 5> fGain {};
    int note = 60, singers = 3;
    int vibTick = 0;
    float gain = 0.0f, brightScale = 1.0f, panL = 1.0f, panR = 1.0f, vibAmount = 0.0f;
    float motionPhase = 0.0f, motionRate = 0.2f;
    bool releasePending = false;
};

class Choir final : public PolyInstrument<Choir, ChoirVoice, 16>
{
public:
    enum { vowel, morph, motion, formant, section, detune, vibrato, vibrate, breath, bright,
           attack, decay, sustain, release, octave, ensemble, room, velocity, level };

    ChoirParams params;

    void update() override
    {
        auto& p = params;
        p.vowelA = juce::jlimit (0, 3, juce::roundToInt (knob (vowel)));
        p.vowelB = (p.vowelA + 1) % 4;
        p.morph = knob (morph);
        p.motion = knob (motion);
        p.formantShift = std::exp2 (knob (formant) * 0.3f);
        p.singers = 1 + 2 * juce::roundToInt (knob (section));
        p.detuneCents = knob (detune) * 25.0f;
        p.vibDepth = knob (vibrato) * 0.4f;
        p.vibRate = knob (vibrate);
        p.breath = knob (breath) * 0.12f;
        p.brightHz = knob (bright);
        p.attack = ms (knob (attack));
        p.decay = ms (knob (decay));
        p.sustain = knob (sustain);
        p.release = ms (knob (release));
        p.octaveUp = knob (octave);
        p.velSens = knob (velocity);
        p.level = dbToGain (knob (level));
        fx.ensembleMix = knob (ensemble);
        fx.roomMix = knob (room) * 0.6f;
    }

protected:
    void prepareEffects() override                      { fx.prepare (sampleRate, 2.2f, 5000.0f); }
    void resetEffects() override                        { fx.reset(); }
    void processEffects (float* l, float* r, int n) override  { fx.process (l, r, n); }

private:
    SectionFx fx;
};

} // namespace

void registerOrchestralInstruments (InstrumentRegistry& r)
{
    using P = InstrumentInfo::Patch;

    r.add<StringEnsemble> ({ "orch.strings", "String Ensemble", "Orchestral",
                             "Bowed string section: detuned players across the stage, delayed vibrato, rosin and a wooden body",
                             juce::Colour (0xff9c5a2e),
                             { choice ("section", "Section", { "Solo", "Chamber", "Full" }, 2),
                               choice ("timbre", "Body", { "Violins", "Violas", "Cellos", "Basses" }, 0),
                               knob ("detune", "Detune", 0.0f, 1.0f, 0.4f, "%"),
                               knob ("bright", "Brightness", 300.0f, 12000.0f, 3200.0f, "Hz", 0.35f),
                               knob ("velbright", "Bow Pressure", 0.0f, 1.0f, 0.5f, "%"),
                               knob ("attack", "Attack", 2.0f, 4000.0f, 220.0f, "ms", 0.35f),
                               knob ("decay", "Decay", 20.0f, 6000.0f, 1500.0f, "ms", 0.35f),
                               knob ("sustain", "Sustain", 0.0f, 1.0f, 0.9f, "%"),
                               knob ("release", "Release", 20.0f, 4000.0f, 600.0f, "ms", 0.35f),
                               knob ("vibrato", "Vibrato", 0.0f, 1.0f, 0.4f, "%"),
                               knob ("vibrate", "Vib Rate", 2.0f, 9.0f, 5.4f, "Hz"),
                               knob ("vibdelay", "Vib Delay", 10.0f, 2000.0f, 350.0f, "ms", 0.4f),
                               knob ("tremolo", "Tremolo", 0.0f, 1.0f, 0.0f, "%"),
                               knob ("tremrate", "Trem Rate", 4.0f, 16.0f, 11.0f, "Hz"),
                               knob ("bow", "Bow Noise", 0.0f, 1.0f, 0.25f, "%"),
                               knob ("body", "Body", 0.0f, 1.0f, 0.5f, "%"),
                               knob ("ensemble", "Ensemble", 0.0f, 1.0f, 0.4f, "%"),
                               knob ("room", "Hall", 0.0f, 1.0f, 0.3f, "%"),
                               knob ("velocity", "Velocity", 0.0f, 1.0f, 0.5f, "%"),
                               knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") },
                             { P { "Full Section", "Strings", {} },
                               P { "Slow Strings", "Strings", { { "attack", 900.0f }, { "release", 1500.0f }, { "bright", 2400.0f }, { "vibdelay", 700.0f },
                                                               { "ensemble", 0.55f }, { "room", 0.4f } } },
                               P { "Chamber", "Strings", { { "section", 1 }, { "detune", 0.3f }, { "attack", 120.0f }, { "bright", 4000.0f },
                                                           { "vibrato", 0.5f }, { "bow", 0.35f }, { "ensemble", 0.2f }, { "room", 0.25f } } },
                               P { "Solo Violin", "Strings", { { "section", 0 }, { "attack", 90.0f }, { "bright", 4500.0f }, { "vibrato", 0.6f },
                                                               { "vibdelay", 250.0f }, { "bow", 0.4f }, { "body", 0.7f }, { "ensemble", 0.0f },
                                                               { "room", 0.3f }, { "level", -6.0f } } },
                               P { "Pizzicato", "Strings", { { "attack", 3.0f }, { "decay", 450.0f }, { "sustain", 0.0f }, { "release", 300.0f },
                                                             { "bright", 2200.0f }, { "velbright", 0.8f }, { "vibrato", 0.0f }, { "bow", 0.0f },
                                                             { "body", 0.9f }, { "ensemble", 0.2f }, { "room", 0.35f }, { "level", 2.0f } } },
                               P { "Tremolo", "Strings", { { "attack", 60.0f }, { "tremolo", 0.75f }, { "tremrate", 12.0f }, { "vibrato", 0.15f },
                                                           { "bow", 0.4f }, { "bright", 3800.0f } } },
                               P { "High Strings", "Strings", { { "bright", 5500.0f }, { "detune", 0.5f }, { "attack", 350.0f }, { "vibrato", 0.5f },
                                                                { "ensemble", 0.6f }, { "room", 0.45f }, { "level", -2.0f } } },
                               P { "Cellos", "Strings", { { "timbre", 2 }, { "section", 1 }, { "bright", 1800.0f }, { "attack", 180.0f },
                                                          { "vibrato", 0.45f }, { "vibrate", 5.0f }, { "body", 0.8f }, { "bow", 0.3f }, { "level", -2.0f } } } } });

    r.add<BrassSection> ({ "orch.brass", "Brass Section", "Orchestral",
                           "Swelling brass that brightens the harder you play, with lip scoops, growl and mutes",
                           juce::Colour (0xffc9a227),
                           { choice ("section", "Section", { "Solo", "Duo", "Section" }, 2),
                             choice ("bell", "Bell", { "Open", "Mellow", "Muted" }, 0),
                             knob ("detune", "Detune", 0.0f, 1.0f, 0.35f, "%"),
                             knob ("bright", "Brightness", 100.0f, 6000.0f, 500.0f, "Hz", 0.35f),
                             knob ("swell", "Swell", 0.0f, 1.0f, 0.6f, "%"),
                             knob ("velbright", "Vel > Bright", 0.0f, 1.0f, 0.7f, "%"),
                             knob ("attack", "Attack", 2.0f, 2000.0f, 40.0f, "ms", 0.35f),
                             knob ("decay", "Decay", 20.0f, 4000.0f, 700.0f, "ms", 0.35f),
                             knob ("sustain", "Sustain", 0.0f, 1.0f, 0.8f, "%"),
                             knob ("release", "Release", 10.0f, 3000.0f, 220.0f, "ms", 0.35f),
                             knob ("scoop", "Scoop", 0.0f, 3.0f, 0.8f, "st"),
                             knob ("growl", "Growl", 0.0f, 1.0f, 0.2f, "%"),
                             knob ("breath", "Breath", 0.0f, 1.0f, 0.15f, "%"),
                             knob ("vibrato", "Vibrato", 0.0f, 1.0f, 0.15f, "%"),
                             knob ("vibrate", "Vib Rate", 2.0f, 8.0f, 5.2f, "Hz"),
                             knob ("ensemble", "Ensemble", 0.0f, 1.0f, 0.3f, "%"),
                             knob ("room", "Hall", 0.0f, 1.0f, 0.25f, "%"),
                             knob ("velocity", "Velocity", 0.0f, 1.0f, 0.5f, "%"),
                             knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") },
                           { P { "Brass Section", "Brass", {} },
                             P { "Horns", "Brass", { { "bell", 1 }, { "bright", 350.0f }, { "swell", 0.45f }, { "attack", 90.0f }, { "scoop", 0.4f },
                                                     { "growl", 0.05f }, { "vibrato", 0.1f }, { "ensemble", 0.45f }, { "room", 0.4f }, { "level", 0.0f } } },
                             P { "Trumpets", "Brass", { { "bright", 900.0f }, { "swell", 0.7f }, { "attack", 25.0f }, { "scoop", 1.0f },
                                                        { "growl", 0.25f }, { "vibrato", 0.2f }, { "level", -2.0f } } },
                             P { "Trombones", "Brass", { { "bright", 300.0f }, { "swell", 0.65f }, { "attack", 60.0f }, { "scoop", 1.4f },
                                                         { "growl", 0.35f }, { "vibrato", 0.05f }, { "level", 1.0f } } },
                             P { "Stabs", "Brass", { { "attack", 8.0f }, { "decay", 350.0f }, { "sustain", 0.3f }, { "release", 150.0f },
                                                     { "bright", 700.0f }, { "swell", 0.8f }, { "scoop", 0.3f }, { "growl", 0.3f },
                                                     { "vibrato", 0.0f }, { "room", 0.2f }, { "level", 1.0f } } },
                             P { "Soft Horns", "Brass", { { "bell", 1 }, { "bright", 250.0f }, { "swell", 0.35f }, { "attack", 300.0f },
                                                          { "release", 600.0f }, { "scoop", 0.0f }, { "growl", 0.0f }, { "breath", 0.25f },
                                                          { "ensemble", 0.6f }, { "room", 0.45f }, { "velocity", 0.3f }, { "level", 1.0f } } },
                             P { "Muted Trumpet", "Brass", { { "section", 0 }, { "bell", 2 }, { "bright", 1500.0f }, { "swell", 0.5f },
                                                             { "attack", 30.0f }, { "scoop", 0.6f }, { "vibrato", 0.3f }, { "ensemble", 0.0f },
                                                             { "room", 0.3f }, { "level", 2.0f } } } } });

    r.add<Choir> ({ "orch.choir", "Choir", "Orchestral",
                    "A singing section with morphing vowels, breath and an optional octave-up layer",
                    juce::Colour (0xff7d6fb0),
                    { choice ("vowel", "Vowel", { "Ooh", "Aah", "Eeh", "Mmm" }, 1),
                      knob ("morph", "Morph", 0.0f, 1.0f, 0.0f, "%"),
                      knob ("motion", "Motion", 0.0f, 1.0f, 0.2f, "%"),
                      knob ("formant", "Formant", -1.0f, 1.0f, 0.0f),
                      choice ("section", "Section", { "Solo", "Small", "Large" }, 2),
                      knob ("detune", "Detune", 0.0f, 1.0f, 0.4f, "%"),
                      knob ("vibrato", "Vibrato", 0.0f, 1.0f, 0.35f, "%"),
                      knob ("vibrate", "Vib Rate", 2.0f, 8.0f, 5.0f, "Hz"),
                      knob ("breath", "Breath", 0.0f, 1.0f, 0.3f, "%"),
                      knob ("bright", "Brightness", 400.0f, 8000.0f, 2000.0f, "Hz", 0.4f),
                      knob ("attack", "Attack", 5.0f, 4000.0f, 250.0f, "ms", 0.35f),
                      knob ("decay", "Decay", 20.0f, 6000.0f, 1500.0f, "ms", 0.35f),
                      knob ("sustain", "Sustain", 0.0f, 1.0f, 0.9f, "%"),
                      knob ("release", "Release", 20.0f, 4000.0f, 800.0f, "ms", 0.35f),
                      knob ("octave", "Octave Up", 0.0f, 1.0f, 0.0f, "%"),
                      knob ("ensemble", "Ensemble", 0.0f, 1.0f, 0.4f, "%"),
                      knob ("room", "Hall", 0.0f, 1.0f, 0.35f, "%"),
                      knob ("velocity", "Velocity", 0.0f, 1.0f, 0.4f, "%"),
                      knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") },
                    { P { "Aahs", "Choir", {} },
                      P { "Oohs", "Choir", { { "vowel", 0 }, { "morph", 0.15f }, { "bright", 1500.0f }, { "attack", 350.0f }, { "level", 0.0f } } },
                      P { "Mixed Choir", "Choir", { { "vowel", 0 }, { "morph", 0.6f }, { "motion", 0.4f }, { "octave", 0.6f }, { "detune", 0.5f },
                                                    { "ensemble", 0.5f }, { "room", 0.45f } } },
                      P { "Soft Hum", "Choir", { { "vowel", 3 }, { "morph", 0.1f }, { "motion", 0.1f }, { "bright", 900.0f }, { "breath", 0.15f },
                                                 { "attack", 500.0f }, { "vibrato", 0.2f }, { "velocity", 0.2f }, { "level", 2.0f } } },
                      P { "Angel Pad", "Choir", { { "vowel", 0 }, { "morph", 0.5f }, { "motion", 0.7f }, { "formant", 0.4f }, { "octave", 0.8f },
                                                  { "detune", 0.7f }, { "attack", 1200.0f }, { "release", 2500.0f }, { "bright", 3000.0f },
                                                  { "ensemble", 0.8f }, { "room", 0.6f }, { "velocity", 0.1f } } },
                      P { "Solo Voice", "Choir", { { "section", 0 }, { "vibrato", 0.5f }, { "vibrate", 5.5f }, { "breath", 0.4f },
                                                   { "ensemble", 0.0f }, { "room", 0.3f }, { "level", -1.0f } } },
                      P { "Eeh Chant", "Choir", { { "vowel", 2 }, { "motion", 0.1f }, { "formant", -0.4f }, { "bright", 2500.0f },
                                                  { "vibrato", 0.15f }, { "room", 0.5f } } } } });

    registerPluckedInstruments (r);
}

} // namespace woodshed
