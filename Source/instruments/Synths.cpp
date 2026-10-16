#include "InstrumentDsp.h"

namespace woodshed
{
namespace
{
using namespace idsp;

//==============================================================================
/** What an analog-style voice reads from its owner, derived once per block. */
struct AnalogParams
{
    int wave1 = Osc::saw, wave2 = Osc::saw;
    float ratio1 = 1.0f, ratio2 = 1.0f, mix = 0.5f, sub = 0.0f, noise = 0.0f;
    float cutoffNote = 100.0f, reso = 0.0f, keyTrack = 0.5f, envAmount = 0.0f;
    float fAttack = 0.01f, fDecay = 0.3f, fSustain = 0.5f;
    float attack = 0.01f, decay = 0.3f, sustain = 0.7f, release = 0.3f;
    float lfoRate = 5.0f, vibrato = 0.0f, lfoFilter = 0.0f, glide = 0.0f;
    float drive = 1.0f, level = 1.0f, velSens = 0.5f;
    bool legato = false;
};

/** Two PolyBLEP oscillators, sub, noise, a resonant 24 dB lowpass with its own envelope. */
class AnalogVoice
{
public:
    void prepare (double sr)
    {
        sampleRate = sr;
        starting.prepare (sr);
        noiseGen.setSeed ((uint32_t) (reinterpret_cast<uintptr_t> (this) >> 4));
    }

    void start (int newNote, float velocity, bool stolen) { starting.request (newNote, velocity, stolen); releasePending = false; }
    void release()                                       { if (starting.pending) releasePending = true; amp.noteOff(); filterEnv.noteOff(); }
    void kill()                                          { starting.clear(); amp.kill(); filterEnv.kill(); filter.reset(); releasePending = false; }
    bool isActive() const                                { return starting.pending || amp.isActive(); }

    template <typename Owner>
    void render (float* left, float* right, int n, const Owner& owner)
    {
        const AnalogParams& p = owner.params;
        const float sr = (float) sampleRate;
        amp.set (sampleRate, p.attack, p.decay, p.sustain, p.release);
        filterEnv.set (sampleRate, p.fAttack, p.fDecay, p.fSustain, p.release);
        const float glideCoeff = p.glide > 0.001f ? 1.0f - std::exp (-1.0f / (p.glide * 0.35f * sr)) : 1.0f;
        const float bend = owner.bend();
        const float vib = p.vibrato + owner.mod() * 0.5f;
        const float lfoInc = p.lfoRate / sr;

        for (int i = 0; i < n; ++i)
        {
            if (starting.pending)
            {
                if (starting.fading && p.legato && amp.isActive()) beginLegato();
                else if (starting.tick()) begin (p);
            }
            if (! amp.isActive() && ! starting.pending) break;

            currentNote += glideCoeff * (targetNote - currentNote);
            lfoPhase += lfoInc;
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
            const float lfo = sinTurns (lfoPhase);

            const float pitch = currentNote + bend + lfo * vib;
            const float inc = noteToHz (pitch) / sr;
            const float pw = 0.3f + 0.12f * lfo;
            float x = (1.0f - p.mix) * osc1.next (inc * p.ratio1, p.wave1, pw)
                    + p.mix * osc2.next (inc * p.ratio2, p.wave2, pw);
            if (p.sub > 0.0f)   x += p.sub * subOsc.next (inc * 0.5f, Osc::square);
            if (p.noise > 0.0f) x += p.noise * noiseGen.next();

            const float fe = filterEnv.next();
            const float cutoff = p.cutoffNote + p.keyTrack * (pitch - 60.0f) + p.envAmount * fe * velFilter + p.lfoFilter * lfo;
            const float y = filter.process (x * p.drive, Svf::gFor (noteToHz (juce::jmin (cutoff, 135.0f)), sampleRate), p.reso);

            float out = y * amp.next() * gain * p.level;
            if (starting.fading) out *= juce::jmax (0.0f, starting.fade);
            left[i] += out * panL;
            right[i] += out * panR;
        }
    }

private:
    void begin (const AnalogParams& p)
    {
        const bool wasActive = amp.isActive();
        targetNote = (float) starting.note;
        if (! wasActive || p.glide <= 0.001f) currentNote = targetNote;

        const float v = starting.velocity;
        gain = 0.16f * velocityGain (v, p.velSens);
        velFilter = 0.45f + 0.55f * v;
        const float pan = juce::jlimit (-0.35f, 0.35f, (targetNote - 60.0f) / 60.0f);
        std::tie (panL, panR) = panGains (pan);

        amp.kill(); filterEnv.kill();
        amp.trigger(); filterEnv.trigger();
        filter.reset();
        osc1.phase = noiseGen.uniform();
        osc2.phase = noiseGen.uniform();
        subOsc.phase = osc1.phase * 0.5f;
        lfoPhase = 0.0f;
        starting.clear();
        if (releasePending) { amp.noteOff(); filterEnv.noteOff(); releasePending = false; }
    }

    void beginLegato()
    {
        targetNote = (float) starting.note;
        starting.clear();
        if (releasePending) { amp.noteOff(); filterEnv.noteOff(); releasePending = false; }
    }

    double sampleRate = 48000.0;
    PendingStart starting;
    Osc osc1, osc2, subOsc;
    Noise noiseGen;
    Lowpass24 filter;
    Adsr amp, filterEnv;
    float currentNote = 60.0f, targetNote = 60.0f, lfoPhase = 0.0f;
    float gain = 0.0f, velFilter = 1.0f, panL = 1.0f, panR = 1.0f;
    bool releasePending = false;
};

inline float ms (float v)                       { return v * 0.001f; }

//==============================================================================
// Analog Synth: classic polyphonic subtractive synth with a stereo ensemble.
class AnalogSynth final : public PolyInstrument<AnalogSynth, AnalogVoice, 16>
{
public:
    enum { wave1, wave2, tune2, detune, mix, sub, noise, cutoff, reso, keytrack, fenv, fattack, fdecay, fsustain,
           attack, decay, sustain, release, lforate, vibrato, lfofilter, glide, chorus, level };

    AnalogParams params;

    void update() override
    {
        auto& p = params;
        p.wave1 = juce::roundToInt (knob (wave1));
        p.wave2 = juce::roundToInt (knob (wave2));
        const float cents = knob (detune) * 0.4f;
        p.ratio1 = semitones (-0.5f * cents);
        p.ratio2 = semitones (knob (tune2) + 0.5f * cents);
        p.mix = knob (mix);
        p.sub = knob (sub) * 0.8f;
        p.noise = knob (noise) * 0.6f;
        p.cutoffNote = hzToNote (knob (cutoff));
        p.reso = knob (reso) * 0.97f;
        p.keyTrack = knob (keytrack);
        p.envAmount = knob (fenv) * 60.0f;
        p.fAttack = ms (knob (fattack));
        p.fDecay = ms (knob (fdecay));
        p.fSustain = knob (fsustain);
        p.attack = ms (knob (attack));
        p.decay = ms (knob (decay));
        p.sustain = knob (sustain);
        p.release = ms (knob (release));
        p.lfoRate = knob (lforate);
        p.vibrato = knob (vibrato) * knob (vibrato);
        p.lfoFilter = knob (lfofilter) * 24.0f;
        p.glide = ms (knob (glide));
        p.level = dbToGain (knob (level));
        p.drive = 1.0f + 0.6f * knob (reso);
        chorusMix = knob (chorus);
    }

protected:
    void prepareEffects() override                      { ensemble.prepare (sampleRate); }
    void resetEffects() override                        { ensemble.reset(); }
    void processEffects (float* l, float* r, int n) override
    {
        ensemble.process (l, r, n, chorusMix);
        safetyClip (l, r, n);
    }

private:
    Ensemble ensemble;
    float chorusMix = 0.0f;
};

const juce::StringArray waves { "Saw", "Square", "Pulse", "Triangle", "Sine" };

std::vector<KnobSpec> analogKnobs()
{
    return { choice ("wave1", "Osc 1", waves, 0),
             choice ("wave2", "Osc 2", waves, 0),
             knob ("tune2", "Osc 2 Tune", -24.0f, 24.0f, 0.0f, "st"),
             knob ("detune", "Detune", 0.0f, 1.0f, 0.2f, "%"),
             knob ("mix", "Osc Mix", 0.0f, 1.0f, 0.5f, "%"),
             knob ("sub", "Sub", 0.0f, 1.0f, 0.0f, "%"),
             knob ("noise", "Noise", 0.0f, 1.0f, 0.0f, "%"),
             knob ("cutoff", "Cutoff", 30.0f, 18000.0f, 2400.0f, "Hz", 0.3f),
             knob ("reso", "Resonance", 0.0f, 1.0f, 0.2f, "%"),
             knob ("keytrack", "Key Track", 0.0f, 1.0f, 0.5f, "%"),
             knob ("fenv", "Filter Env", -1.0f, 1.0f, 0.3f, "%"),
             knob ("fattack", "F Attack", 1.0f, 5000.0f, 5.0f, "ms", 0.35f),
             knob ("fdecay", "F Decay", 5.0f, 8000.0f, 600.0f, "ms", 0.35f),
             knob ("fsustain", "F Sustain", 0.0f, 1.0f, 0.3f, "%"),
             knob ("attack", "Attack", 1.0f, 8000.0f, 5.0f, "ms", 0.35f),
             knob ("decay", "Decay", 5.0f, 8000.0f, 800.0f, "ms", 0.35f),
             knob ("sustain", "Sustain", 0.0f, 1.0f, 0.75f, "%"),
             knob ("release", "Release", 5.0f, 6000.0f, 350.0f, "ms", 0.35f),
             knob ("lforate", "LFO Rate", 0.05f, 20.0f, 5.0f, "Hz", 0.4f),
             knob ("vibrato", "Vibrato", 0.0f, 1.0f, 0.0f, "%"),
             knob ("lfofilter", "LFO > Filter", 0.0f, 1.0f, 0.0f, "%"),
             knob ("glide", "Glide", 0.0f, 2000.0f, 0.0f, "ms", 0.4f),
             knob ("chorus", "Chorus", 0.0f, 1.0f, 0.35f, "%"),
             knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") };
}

//==============================================================================
// Mono Lead/Bass: one analog voice with last-note priority, legato glide and drive.
class MonoSynth final : public PolyInstrument<MonoSynth, AnalogVoice, 1>
{
public:
    enum { wave1, wave2, tune2, detune, mix, sub, noise, cutoff, reso, keytrack, fenv, fattack, fdecay, fsustain,
           attack, decay, sustain, release, glide, legato, drive, lforate, vibrato, level };

    AnalogParams params;

    bool isMono() const override                        { return true; }

    void update() override
    {
        auto& p = params;
        p.wave1 = juce::roundToInt (knob (wave1));
        p.wave2 = juce::roundToInt (knob (wave2));
        const float cents = knob (detune) * 0.4f;
        p.ratio1 = semitones (-0.5f * cents);
        p.ratio2 = semitones (knob (tune2) + 0.5f * cents);
        p.mix = knob (mix);
        p.sub = knob (sub) * 0.8f;
        p.noise = knob (noise) * 0.6f;
        p.cutoffNote = hzToNote (knob (cutoff));
        p.reso = knob (reso) * 0.97f;
        p.keyTrack = knob (keytrack);
        p.envAmount = knob (fenv) * 60.0f;
        p.fAttack = ms (knob (fattack));
        p.fDecay = ms (knob (fdecay));
        p.fSustain = knob (fsustain);
        p.attack = ms (knob (attack));
        p.decay = ms (knob (decay));
        p.sustain = knob (sustain);
        p.release = ms (knob (release));
        p.glide = ms (knob (glide));
        p.legato = knob (legato) > 0.5f;
        p.lfoRate = knob (lforate);
        p.vibrato = knob (vibrato) * knob (vibrato);
        p.drive = 1.0f + 5.0f * knob (drive) + 0.6f * knob (reso);
        // A single voice: louder than one voice of a poly patch, less as the drive compresses.
        p.level = dbToGain (knob (level)) * 2.2f / (1.0f + 1.2f * knob (drive));
    }

    void reset() override
    {
        held = 0;
        PolyInstrument::reset();
    }

    /** Last-note priority: releasing the newest key returns to the one still held. */
    void render (float* left, float* right, int numSamples, const NoteEvent* events, int numEvents) override
    {
        int count = 0;
        for (int e = 0; e < numEvents && count < (int) rewritten.size(); ++e)
        {
            const auto& ev = events[e];
            const bool isOn = ev.kind == NoteEvent::noteOn && ev.value > 0.0f;
            const bool isOff = ev.kind == NoteEvent::noteOff || (ev.kind == NoteEvent::noteOn && ev.value <= 0.0f);

            if (isOn)
            {
                forget (ev.note);
                if (held < (int) stack.size()) stack[(size_t) held++] = ev.note;
                lastVelocity = ev.value;
                rewritten[(size_t) count++] = ev;
            }
            else if (isOff)
            {
                const bool wasTop = held > 0 && stack[(size_t) (held - 1)] == ev.note;
                forget (ev.note);
                if (wasTop && held > 0)
                {
                    auto back = ev;
                    back.kind = NoteEvent::noteOn;
                    back.note = stack[(size_t) (held - 1)];
                    back.value = lastVelocity;
                    rewritten[(size_t) count++] = back;
                }
                else if (wasTop)
                {
                    auto offEv = ev;
                    offEv.kind = NoteEvent::noteOff;
                    rewritten[(size_t) count++] = offEv;
                }
            }
            else
            {
                if (ev.kind == NoteEvent::allNotesOff) held = 0;
                rewritten[(size_t) count++] = ev;
            }
        }

        PolyInstrument::render (left, right, numSamples, rewritten.data(), count);
    }

protected:
    void processEffects (float* l, float* r, int n) override  { safetyClip (l, r, n); }

private:
    void forget (int note)
    {
        int w = 0;
        for (int i = 0; i < held; ++i)
            if (stack[(size_t) i] != note) stack[(size_t) w++] = stack[(size_t) i];
        held = w;
    }

    std::array<int, 16> stack {};
    std::array<NoteEvent, 256> rewritten {};
    int held = 0;
    float lastVelocity = 0.8f;
};

std::vector<KnobSpec> monoKnobs()
{
    return { choice ("wave1", "Osc 1", waves, 0),
             choice ("wave2", "Osc 2", waves, 1),
             knob ("tune2", "Osc 2 Tune", -24.0f, 24.0f, -12.0f, "st"),
             knob ("detune", "Detune", 0.0f, 1.0f, 0.15f, "%"),
             knob ("mix", "Osc Mix", 0.0f, 1.0f, 0.4f, "%"),
             knob ("sub", "Sub", 0.0f, 1.0f, 0.3f, "%"),
             knob ("noise", "Noise", 0.0f, 1.0f, 0.0f, "%"),
             knob ("cutoff", "Cutoff", 30.0f, 18000.0f, 700.0f, "Hz", 0.3f),
             knob ("reso", "Resonance", 0.0f, 1.0f, 0.3f, "%"),
             knob ("keytrack", "Key Track", 0.0f, 1.0f, 0.4f, "%"),
             knob ("fenv", "Filter Env", -1.0f, 1.0f, 0.45f, "%"),
             knob ("fattack", "F Attack", 1.0f, 5000.0f, 2.0f, "ms", 0.35f),
             knob ("fdecay", "F Decay", 5.0f, 8000.0f, 350.0f, "ms", 0.35f),
             knob ("fsustain", "F Sustain", 0.0f, 1.0f, 0.2f, "%"),
             knob ("attack", "Attack", 1.0f, 8000.0f, 2.0f, "ms", 0.35f),
             knob ("decay", "Decay", 5.0f, 8000.0f, 600.0f, "ms", 0.35f),
             knob ("sustain", "Sustain", 0.0f, 1.0f, 0.85f, "%"),
             knob ("release", "Release", 5.0f, 6000.0f, 120.0f, "ms", 0.35f),
             knob ("glide", "Glide", 0.0f, 2000.0f, 40.0f, "ms", 0.4f),
             choice ("legato", "Mode", { "Retrigger", "Legato" }, 1),
             knob ("drive", "Drive", 0.0f, 1.0f, 0.15f, "%"),
             knob ("lforate", "LFO Rate", 0.05f, 20.0f, 5.5f, "Hz", 0.4f),
             knob ("vibrato", "Vibrato", 0.0f, 1.0f, 0.0f, "%"),
             knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") };
}

//==============================================================================
/** Four-operator phase-modulation voice. Operators are numbered 0..3 (op 1..4 on the panel). */
struct FmAlgorithm
{
    std::array<uint8_t, 4> modulators;     // bitmask of the operators modulating each operator
    std::array<bool, 4> carrier;
};

// Modulators always have a higher index than what they modulate, so ops run 3, 2, 1, 0.
const FmAlgorithm fmAlgorithms[] {
    { { 0b0010, 0b0100, 0b1000, 0 }, { true,  false, false, false } },   // Stack: 4 > 3 > 2 > 1
    { { 0b0010, 0,      0b1000, 0 }, { true,  false, true,  false } },   // Two pairs: 2 > 1, 4 > 3
    { { 0b1110, 0,      0,      0 }, { true,  false, false, false } },   // Three to one
    { { 0b0010, 0b1100, 0,      0 }, { true,  false, false, false } },   // Branch: 3 + 4 > 2 > 1
    { { 0b1000, 0b1000, 0b1000, 0 }, { true,  true,  true,  false } },   // One to three
    { { 0b0010, 0b0100, 0,      0 }, { true,  false, false, true  } },   // Stack of three + carrier
    { { 0,      0,      0,      0 }, { true,  true,  true,  true  } },   // Additive
};

struct FmParams
{
    int algorithm = 0;
    std::array<float, 4> ratio { 1.0f, 1.0f, 1.0f, 1.0f }, level { 1.0f, 0.5f, 0.3f, 0.2f }, detune {};
    float feedback = 0.0f, velIndex = 0.5f, velAmp = 0.6f, keyScale = 0.3f, vibrato = 0.0f;
    float mAttack = 0.001f, mDecay = 0.5f, mSustain = 0.3f;
    float attack = 0.001f, decay = 1.0f, sustain = 0.6f, release = 0.3f;
};

class FmVoice
{
public:
    void prepare (double sr)                             { sampleRate = sr; starting.prepare (sr); }
    void start (int newNote, float velocity, bool stolen) { starting.request (newNote, velocity, stolen); releasePending = false; }
    void release()                                       { if (starting.pending) releasePending = true; for (auto& e : env) e.noteOff(); }
    void kill()                                          { starting.clear(); for (auto& e : env) e.kill(); releasePending = false; }
    bool isActive() const                                { return starting.pending || carriersActive(); }

    template <typename Owner>
    void render (float* left, float* right, int n, const Owner& owner)
    {
        const FmParams& p = owner.params;
        const auto& algo = fmAlgorithms[(size_t) juce::jlimit (0, 6, p.algorithm)];
        const float sr = (float) sampleRate;
        isCarrier = algo.carrier;

        // Key scaling: higher notes get less modulation and shorter envelopes.
        const float above = juce::jmax (0.0f, (float) note - 48.0f) / 48.0f;
        const float timeScale = 1.0f / (1.0f + 1.5f * p.keyScale * above);
        const float indexScale = juce::jlimit (0.15f, 1.0f, 1.0f - 0.7f * p.keyScale * above);

        std::array<float, 4> amount {};
        for (size_t op = 0; op < 4; ++op)
        {
            if (isCarrier[op])
            {
                env[op].set (sampleRate, p.attack, p.decay * timeScale, p.sustain, p.release);
                amount[op] = op == 0 ? 1.0f : p.level[op];
            }
            else
            {
                env[op].set (sampleRate, p.mAttack, p.mDecay * timeScale, p.mSustain, p.release);
                amount[op] = p.level[op] * p.level[op] * 2.2f * indexScale * velIndexScale;   // turns of phase deviation
            }
        }

        int carriers = 0;
        for (auto c : isCarrier) carriers += c ? 1 : 0;
        const float carrierNorm = 1.0f / std::sqrt ((float) juce::jmax (1, carriers));
        const float vib = p.vibrato + owner.mod() * 0.4f;
        const float fb = p.feedback * p.feedback * 0.25f;

        for (int i = 0; i < n; ++i)
        {
            if (starting.pending && starting.tick()) begin (p);
            if (! carriersActive() && ! starting.pending) break;

            lfoPhase += 5.2f / sr;
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
            if (--ctrlCount <= 0)
            {
                ctrlCount = 8;
                baseInc = noteToHz ((float) note + owner.bend() + vib * sinTurns (lfoPhase)) / sr;
            }

            std::array<float, 4> out {};
            float sum = 0.0f;
            for (int op = 3; op >= 0; --op)
            {
                const auto o = (size_t) op;
                float m = 0.0f;
                for (size_t src = 0; src < 4; ++src)
                    if ((algo.modulators[o] >> src) & 1u) m += out[src];
                if (op == 3) m += fb * (fb1 + fb2);

                phase[o] += baseInc * p.ratio[o] * p.detune[o];
                if (phase[o] >= 1.0f) phase[o] -= std::floor (phase[o]);
                const float s = sinTurns (phase[o] + m);
                const float e = env[o].next();
                out[o] = s * e * amount[o];
                if (op == 3) { fb2 = fb1; fb1 = s * e; }
                if (isCarrier[o]) sum += out[o];
            }

            float y = sum * carrierNorm * gain;
            if (starting.fading) y *= juce::jmax (0.0f, starting.fade);
            left[i] += y * panL;
            right[i] += y * panR;
        }
    }

private:
    bool carriersActive() const
    {
        for (size_t op = 0; op < 4; ++op)
            if (isCarrier[op] && env[op].isActive()) return true;
        return false;
    }

    void begin (const FmParams& p)
    {
        note = starting.note;
        const float v = starting.velocity;
        gain = 0.17f * velocityGain (v, p.velAmp);
        velIndexScale = (1.0f - p.velIndex) + p.velIndex * (0.25f + 0.75f * v);
        std::tie (panL, panR) = panGains (juce::jlimit (-0.3f, 0.3f, ((float) note - 60.0f) / 60.0f));
        for (auto& e : env) { e.kill(); e.trigger(); }
        phase.fill (0.0f);
        fb1 = fb2 = 0.0f;
        ctrlCount = 0;
        starting.clear();
        if (releasePending) { for (auto& e : env) e.noteOff(); releasePending = false; }
    }

    double sampleRate = 48000.0;
    PendingStart starting;
    std::array<Adsr, 4> env;
    std::array<float, 4> phase {};
    std::array<bool, 4> isCarrier { true, false, false, false };
    int note = 60;
    float gain = 0.0f, velIndexScale = 1.0f, panL = 1.0f, panR = 1.0f, fb1 = 0.0f, fb2 = 0.0f, lfoPhase = 0.0f, baseInc = 0.0f;
    int ctrlCount = 0;
    bool releasePending = false;
};

// FM Synth: four operators, seven algorithms; bells, electric pianos, basses, brass.
class FmSynth final : public PolyInstrument<FmSynth, FmVoice, 16>
{
public:
    enum { algorithm, ratio1, ratio2, ratio3, ratio4, level2, level3, level4, feedback, detune,
           mattack, mdecay, msustain, attack, decay, sustain, release, velindex, velamp, keyscale, vibrato, chorus, level };

    FmParams params;

    void update() override
    {
        auto& p = params;
        p.algorithm = juce::roundToInt (knob (algorithm));
        p.ratio = { knob (ratio1), knob (ratio2), knob (ratio3), knob (ratio4) };
        p.level = { 1.0f, knob (level2), knob (level3), knob (level4) };
        const float cents = knob (detune) * 12.0f;
        p.detune = { 1.0f, semitones (cents * 0.01f), semitones (-cents * 0.01f), semitones (cents * 0.007f) };
        p.feedback = knob (feedback);
        p.mAttack = ms (knob (mattack));
        p.mDecay = ms (knob (mdecay));
        p.mSustain = knob (msustain);
        p.attack = ms (knob (attack));
        p.decay = ms (knob (decay));
        p.sustain = knob (sustain);
        p.release = ms (knob (release));
        p.velIndex = knob (velindex);
        p.velAmp = knob (velamp);
        p.keyScale = knob (keyscale);
        p.vibrato = knob (vibrato) * knob (vibrato) * 0.5f;
        chorusMix = knob (chorus);
        outLevel = dbToGain (knob (level));
    }

protected:
    void prepareEffects() override                      { ensemble.prepare (sampleRate); }
    void resetEffects() override                        { ensemble.reset(); }
    void processEffects (float* l, float* r, int n) override
    {
        juce::FloatVectorOperations::multiply (l, outLevel, n);
        juce::FloatVectorOperations::multiply (r, outLevel, n);
        ensemble.process (l, r, n, chorusMix, 0.6f);
        safetyClip (l, r, n);
    }

private:
    Ensemble ensemble;
    float chorusMix = 0.0f, outLevel = 1.0f;
};

std::vector<KnobSpec> fmKnobs()
{
    return { choice ("algorithm", "Algorithm", { "Stack", "Two Pairs", "Three to One", "Branch", "One to Three", "Stack + Carrier", "Additive" }, 1),
             knob ("ratio1", "Op 1 Ratio", 0.5f, 16.0f, 1.0f, "x", 0.5f),
             knob ("ratio2", "Op 2 Ratio", 0.5f, 16.0f, 1.0f, "x", 0.5f),
             knob ("ratio3", "Op 3 Ratio", 0.5f, 16.0f, 1.0f, "x", 0.5f),
             knob ("ratio4", "Op 4 Ratio", 0.5f, 16.0f, 14.0f, "x", 0.5f),
             knob ("level2", "Op 2 Level", 0.0f, 1.0f, 0.55f, "%"),
             knob ("level3", "Op 3 Level", 0.0f, 1.0f, 0.5f, "%"),
             knob ("level4", "Op 4 Level", 0.0f, 1.0f, 0.35f, "%"),
             knob ("feedback", "Feedback", 0.0f, 1.0f, 0.0f, "%"),
             knob ("detune", "Detune", 0.0f, 1.0f, 0.25f, "%"),
             knob ("mattack", "Mod Attack", 0.5f, 3000.0f, 1.0f, "ms", 0.35f),
             knob ("mdecay", "Mod Decay", 5.0f, 8000.0f, 900.0f, "ms", 0.35f),
             knob ("msustain", "Mod Sustain", 0.0f, 1.0f, 0.15f, "%"),
             knob ("attack", "Attack", 0.5f, 5000.0f, 1.0f, "ms", 0.35f),
             knob ("decay", "Decay", 5.0f, 10000.0f, 2500.0f, "ms", 0.35f),
             knob ("sustain", "Sustain", 0.0f, 1.0f, 0.2f, "%"),
             knob ("release", "Release", 5.0f, 6000.0f, 400.0f, "ms", 0.35f),
             knob ("velindex", "Vel > Bright", 0.0f, 1.0f, 0.7f, "%"),
             knob ("velamp", "Vel > Level", 0.0f, 1.0f, 0.6f, "%"),
             knob ("keyscale", "Key Scaling", 0.0f, 1.0f, 0.4f, "%"),
             knob ("vibrato", "Vibrato", 0.0f, 1.0f, 0.0f, "%"),
             knob ("chorus", "Chorus", 0.0f, 1.0f, 0.3f, "%"),
             knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") };
}

//==============================================================================
struct PadParams
{
    int shape = 0, saws = 7;
    float detune = 0.3f, spread = 0.7f, octave = 0.0f, sub = 0.0f, air = 0.0f;
    float cutoffNote = 100.0f, reso = 0.0f, envAmount = 0.0f, fAttack = 0.5f, fDecay = 2.0f;
    float attack = 0.5f, decay = 2.0f, sustain = 0.8f, release = 1.5f;
    float motion = 0.2f, motionRate = 0.2f, velSens = 0.4f, vibrato = 0.0f;
};

/** Up to seven detuned oscillators spread across the stereo field, through a stereo filter. */
class PadVoice
{
public:
    void prepare (double sr)
    {
        sampleRate = sr;
        starting.prepare (sr);
        rng.setSeed ((uint32_t) (reinterpret_cast<uintptr_t> (this) >> 4));
    }

    void start (int newNote, float velocity, bool stolen) { starting.request (newNote, velocity, stolen); releasePending = false; }
    void release()                                       { if (starting.pending) releasePending = true; amp.noteOff(); filterEnv.noteOff(); }
    void kill()                                          { starting.clear(); amp.kill(); filterEnv.kill(); releasePending = false; }
    bool isActive() const                                { return starting.pending || amp.isActive(); }

    template <typename Owner>
    void render (float* left, float* right, int n, const Owner& owner)
    {
        const PadParams& p = owner.params;
        const float sr = (float) sampleRate;
        amp.set (sampleRate, p.attack, p.decay, p.sustain, p.release);
        filterEnv.set (sampleRate, p.fAttack, p.fDecay, 0.0f, p.release);

        // Per-block oscillator layout: symmetric detune, alternating pan.
        const int count = juce::jlimit (1, 7, p.saws);
        std::array<float, 7> ratio {}, gl {}, gr {};
        for (int k = 0; k < count; ++k)
        {
            const float pos = count == 1 ? 0.0f : (float) k / (float) (count - 1) * 2.0f - 1.0f;   // -1..1
            ratio[(size_t) k] = semitones (pos * p.detune * 0.6f + (k == count / 2 ? 0.0f : 0.013f * (float) k));
            std::tie (gl[(size_t) k], gr[(size_t) k]) = panGains ((k % 2 == 0 ? pos : -pos) * p.spread);
        }
        const float norm = 1.0f / std::sqrt ((float) count);
        const float k1 = 2.0f - 1.9f * p.reso;
        const float vib = p.vibrato + owner.mod() * 0.3f;
        const float shape2 = p.shape == 2 ? 1.0f : 0.0f;

        for (int i = 0; i < n; ++i)
        {
            if (starting.pending && starting.tick()) begin (p);
            if (! amp.isActive() && ! starting.pending) break;

            lfoPhase += p.motionRate / sr;
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
            vibPhase += 4.8f / sr;
            if (vibPhase >= 1.0f) vibPhase -= 1.0f;

            // Pitch and cutoff move slowly here, so they update every 8 samples.
            const float fe = filterEnv.next();
            if (--ctrlCount <= 0)
            {
                ctrlCount = 8;
                const float pitch = (float) note + owner.bend() + vib * sinTurns (vibPhase);
                ctrlInc = noteToHz (pitch) / sr;
                const float cut = p.cutoffNote + 0.4f * (pitch - 60.0f) + p.envAmount * fe * velFilter
                                  + p.motion * 18.0f * sinTurns (lfoPhase + lfoOffset);
                ctrlG = Svf::gFor (noteToHz (juce::jmin (cut, 135.0f)), sampleRate);
            }
            const float inc = ctrlInc, g = ctrlG;
            float sl = 0.0f, sr2 = 0.0f;
            for (size_t k = 0; k < (size_t) count; ++k)
            {
                const float v = waveFor (oscs[k], inc * ratio[k], p.shape);
                sl += v * gl[k];
                sr2 += v * gr[k];
            }
            sl *= norm;
            sr2 *= norm;

            float mono = 0.0f;
            if (p.octave > 0.0f) mono += p.octave * 0.5f * octOsc.next (inc * 2.0f, shape2 > 0.5f ? Osc::sine : Osc::saw);
            if (p.sub > 0.0f)    mono += p.sub * 0.7f * subOsc.next (inc * 0.5f, Osc::sine);
            if (p.air > 0.0f)    mono += p.air * 0.25f * rng.next();
            sl += mono;
            sr2 += mono;

            const float yl = filterL.process (sl, g, k1).low;
            const float yr = filterR.process (sr2, g, k1).low;

            float a = amp.next() * gain;
            if (starting.fading) a *= juce::jmax (0.0f, starting.fade);
            left[i] += yl * a;
            right[i] += yr * a;
        }
    }

private:
    static float waveFor (Osc& o, float inc, int shape)
    {
        switch (shape)
        {
            case 1:  return 0.75f * o.next (inc, Osc::saw) + 0.25f * idsp::sinTurns (o.phase * 2.0f);      // Strings: saw with a bit of octave sine
            case 2:  return o.next (inc, Osc::saw);                                                         // Glass: saw + sine octave (layer)
            case 3:  return 1.3f * o.next (inc, Osc::triangle);                                             // Warm
            case 4:  return 0.8f * o.next (inc, Osc::square);                                               // Hollow
            default: return o.next (inc, Osc::saw);                                                         // Supersaw
        }
    }

    void begin (const PadParams& p)
    {
        note = starting.note;
        const float v = starting.velocity;
        gain = 0.13f * velocityGain (v, p.velSens);
        velFilter = 0.5f + 0.5f * v;
        if (! amp.isActive())
        {
            filterL.reset(); filterR.reset();
            for (auto& o : oscs) o.phase = rng.uniform();
            lfoOffset = rng.uniform();
        }
        amp.kill(); filterEnv.kill();
        amp.trigger(); filterEnv.trigger();
        ctrlCount = 0;
        starting.clear();
        if (releasePending) { amp.noteOff(); filterEnv.noteOff(); releasePending = false; }
    }

    double sampleRate = 48000.0;
    PendingStart starting;
    std::array<Osc, 7> oscs;
    Osc octOsc, subOsc;
    Svf filterL, filterR;
    Adsr amp, filterEnv;
    Noise rng;
    int note = 60;
    float gain = 0.0f, velFilter = 1.0f, lfoPhase = 0.0f, lfoOffset = 0.0f, vibPhase = 0.0f, ctrlInc = 0.0f, ctrlG = 0.1f;
    int ctrlCount = 0;
    bool releasePending = false;
};

// Pad / Ensemble: supersaw and ensemble pads with slow envelopes, chorus and room.
class PadSynth final : public PolyInstrument<PadSynth, PadVoice, 16>
{
public:
    enum { shape, saws, detune, spread, octave, sub, air, cutoff, reso, fenv, fattack, fdecay,
           attack, decay, sustain, release, motion, motionrate, vel, vibrato, ensemble, reverb, level };

    PadParams params;

    void update() override
    {
        auto& p = params;
        p.shape = juce::roundToInt (knob (shape));
        p.saws = 3 + 2 * juce::roundToInt (knob (saws));
        p.detune = knob (detune);
        p.spread = knob (spread);
        p.octave = knob (octave);
        p.sub = knob (sub);
        p.air = knob (air);
        p.cutoffNote = hzToNote (knob (cutoff));
        p.reso = knob (reso);
        p.envAmount = knob (fenv) * 48.0f;
        p.fAttack = ms (knob (fattack));
        p.fDecay = ms (knob (fdecay));
        p.attack = ms (knob (attack));
        p.decay = ms (knob (decay));
        p.sustain = knob (sustain);
        p.release = ms (knob (release));
        p.motion = knob (motion);
        p.motionRate = knob (motionrate);
        p.velSens = knob (vel);
        p.vibrato = knob (vibrato) * knob (vibrato) * 0.4f;
        ensembleMix = knob (ensemble);
        reverbMix = knob (reverb);
        outLevel = dbToGain (knob (level));
    }

protected:
    void prepareEffects() override                      { chorus.prepare (sampleRate); room.prepare (sampleRate); room.setDecay (2.2f, 5000.0f); }
    void resetEffects() override                        { chorus.reset(); room.reset(); }
    void processEffects (float* l, float* r, int n) override
    {
        juce::FloatVectorOperations::multiply (l, outLevel, n);
        juce::FloatVectorOperations::multiply (r, outLevel, n);
        chorus.process (l, r, n, ensembleMix);
        room.process (l, r, n, reverbMix * 0.5f);
        safetyClip (l, r, n);
    }

private:
    Ensemble chorus;
    Room room;
    float ensembleMix = 0.0f, reverbMix = 0.0f, outLevel = 1.0f;
};

std::vector<KnobSpec> padKnobs()
{
    return { choice ("shape", "Shape", { "Supersaw", "Strings", "Glass", "Warm", "Hollow" }, 0),
             choice ("saws", "Voices", { "3", "5", "7" }, 2),
             knob ("detune", "Detune", 0.0f, 1.0f, 0.35f, "%"),
             knob ("spread", "Spread", 0.0f, 1.0f, 0.8f, "%"),
             knob ("octave", "Octave Up", 0.0f, 1.0f, 0.0f, "%"),
             knob ("sub", "Sub", 0.0f, 1.0f, 0.0f, "%"),
             knob ("air", "Air", 0.0f, 1.0f, 0.0f, "%"),
             knob ("cutoff", "Cutoff", 60.0f, 18000.0f, 2800.0f, "Hz", 0.3f),
             knob ("reso", "Resonance", 0.0f, 1.0f, 0.1f, "%"),
             knob ("fenv", "Filter Env", -1.0f, 1.0f, 0.1f, "%"),
             knob ("fattack", "F Attack", 1.0f, 8000.0f, 800.0f, "ms", 0.35f),
             knob ("fdecay", "F Decay", 10.0f, 10000.0f, 3000.0f, "ms", 0.35f),
             knob ("attack", "Attack", 1.0f, 8000.0f, 600.0f, "ms", 0.35f),
             knob ("decay", "Decay", 10.0f, 10000.0f, 2000.0f, "ms", 0.35f),
             knob ("sustain", "Sustain", 0.0f, 1.0f, 0.9f, "%"),
             knob ("release", "Release", 10.0f, 6000.0f, 1600.0f, "ms", 0.35f),
             knob ("motion", "Motion", 0.0f, 1.0f, 0.2f, "%"),
             knob ("motionrate", "Motion Rate", 0.02f, 5.0f, 0.15f, "Hz", 0.4f),
             knob ("vel", "Velocity", 0.0f, 1.0f, 0.4f, "%"),
             knob ("vibrato", "Vibrato", 0.0f, 1.0f, 0.0f, "%"),
             knob ("ensemble", "Ensemble", 0.0f, 1.0f, 0.5f, "%"),
             knob ("reverb", "Reverb", 0.0f, 1.0f, 0.35f, "%"),
             knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB") };
}

} // namespace

void registerSynthInstruments (InstrumentRegistry& r)
{
    using P = InstrumentInfo::Patch;

    r.add<AnalogSynth> ({ "synth.analog", "Analog Synth", "Synthesizers",
                          "Warm polyphonic analog-style synth: two oscillators, sub, a resonant ladder-ish filter and ensemble",
                          juce::Colour (0xffd9822b), analogKnobs(),
                          { P { "Classic Poly", "Keys", {} },
                            P { "Warm Pad", "Pads", { { "wave2", 0 }, { "detune", 0.45f }, { "cutoff", 1200.0f }, { "reso", 0.1f }, { "fenv", 0.15f },
                                                       { "fattack", 900.0f }, { "fdecay", 3000.0f }, { "fsustain", 0.6f }, { "attack", 700.0f },
                                                       { "sustain", 1.0f }, { "release", 1800.0f }, { "chorus", 0.7f }, { "lforate", 0.3f },
                                                       { "lfofilter", 0.15f } } },
                            P { "Saw Lead", "Leads", { { "detune", 0.35f }, { "tune2", 12.0f }, { "mix", 0.4f }, { "cutoff", 3500.0f }, { "reso", 0.25f },
                                                        { "fenv", 0.25f }, { "fdecay", 400.0f }, { "sustain", 1.0f }, { "release", 200.0f },
                                                        { "vibrato", 0.35f }, { "lforate", 5.5f }, { "glide", 60.0f }, { "chorus", 0.2f } } },
                            P { "Pluck", "Plucks", { { "wave2", 1 }, { "cutoff", 300.0f }, { "reso", 0.35f }, { "fenv", 0.55f }, { "fdecay", 260.0f },
                                                      { "fsustain", 0.0f }, { "decay", 700.0f }, { "sustain", 0.0f }, { "release", 300.0f }, { "chorus", 0.4f } } },
                            P { "Brass Stab", "Brass", { { "detune", 0.3f }, { "cutoff", 500.0f }, { "reso", 0.1f }, { "fenv", 0.45f }, { "fattack", 60.0f },
                                                          { "fdecay", 500.0f }, { "fsustain", 0.4f }, { "attack", 25.0f }, { "sustain", 0.85f },
                                                          { "release", 180.0f }, { "chorus", 0.25f } } },
                            P { "Sub Bass", "Basses", { { "wave1", 1 }, { "wave2", 4 }, { "tune2", -12.0f }, { "mix", 0.6f }, { "sub", 0.6f },
                                                         { "cutoff", 400.0f }, { "reso", 0.05f }, { "keytrack", 0.3f }, { "fenv", 0.15f },
                                                         { "fdecay", 300.0f }, { "sustain", 1.0f }, { "release", 120.0f }, { "chorus", 0.0f } } },
                            P { "Sweep", "Pads", { { "detune", 0.5f }, { "noise", 0.1f }, { "cutoff", 250.0f }, { "reso", 0.6f }, { "fenv", 0.5f },
                                                    { "fattack", 4000.0f }, { "fdecay", 5000.0f }, { "fsustain", 0.5f }, { "attack", 400.0f },
                                                    { "sustain", 1.0f }, { "release", 2500.0f }, { "lforate", 0.15f }, { "lfofilter", 0.3f },
                                                    { "chorus", 0.6f } } },
                            P { "Square Keys", "Keys", { { "wave1", 1 }, { "wave2", 2 }, { "tune2", 12.0f }, { "mix", 0.35f }, { "cutoff", 1800.0f },
                                                          { "fenv", 0.2f }, { "fdecay", 900.0f }, { "decay", 1500.0f }, { "sustain", 0.5f },
                                                          { "chorus", 0.5f } } } } });

    r.add<MonoSynth> ({ "synth.mono", "Mono Lead/Bass", "Synthesizers",
                        "Monophonic analog-style synth with legato glide and drive, for basses and leads",
                        juce::Colour (0xffc9483c), monoKnobs(),
                        { P { "Fat Bass", "Basses", {} },
                          P { "Acid Bass", "Basses", { { "wave2", 0 }, { "mix", 0.0f }, { "sub", 0.0f }, { "cutoff", 220.0f }, { "reso", 0.82f },
                                                        { "fenv", 0.6f }, { "fdecay", 220.0f }, { "fsustain", 0.0f }, { "keytrack", 0.5f },
                                                        { "glide", 80.0f }, { "drive", 0.45f } } },
                          P { "Reese Bass", "Basses", { { "wave2", 0 }, { "tune2", 0.0f }, { "detune", 1.0f }, { "mix", 0.5f }, { "sub", 0.4f },
                                                         { "cutoff", 500.0f }, { "reso", 0.15f }, { "fenv", 0.1f }, { "drive", 0.35f } } },
                          P { "Sub Drop", "Basses", { { "wave1", 4 }, { "wave2", 3 }, { "mix", 0.3f }, { "sub", 0.5f }, { "cutoff", 300.0f },
                                                       { "reso", 0.0f }, { "fenv", 0.05f }, { "drive", 0.3f }, { "glide", 120.0f } } },
                          P { "Saw Lead", "Leads", { { "wave2", 0 }, { "tune2", 0.0f }, { "detune", 0.45f }, { "sub", 0.0f }, { "cutoff", 2800.0f },
                                                      { "reso", 0.2f }, { "fenv", 0.25f }, { "fsustain", 0.4f }, { "vibrato", 0.3f }, { "glide", 70.0f },
                                                      { "drive", 0.2f }, { "release", 200.0f } } },
                          P { "Square Lead", "Leads", { { "wave1", 1 }, { "wave2", 2 }, { "tune2", 12.0f }, { "mix", 0.3f }, { "sub", 0.0f },
                                                         { "cutoff", 2400.0f }, { "reso", 0.1f }, { "fenv", 0.2f }, { "vibrato", 0.3f },
                                                         { "glide", 50.0f }, { "release", 220.0f } } },
                          P { "Portamento Lead", "Leads", { { "wave1", 3 }, { "wave2", 1 }, { "tune2", 0.0f }, { "mix", 0.35f }, { "sub", 0.0f },
                                                             { "cutoff", 3200.0f }, { "fenv", 0.1f }, { "glide", 260.0f }, { "vibrato", 0.4f },
                                                             { "lforate", 5.0f }, { "release", 300.0f } } } } });

    r.add<FmSynth> ({ "synth.fm", "FM Synth", "Synthesizers",
                      "Four-operator FM: glassy electric pianos, bells, punchy basses and brass",
                      juce::Colour (0xff3f8fc4), fmKnobs(),
                      { P { "DX Keys", "Keys", {} },
                        P { "Bell", "Bells", { { "ratio2", 3.5f }, { "level2", 0.6f }, { "ratio4", 7.0f }, { "level4", 0.3f }, { "mdecay", 4000.0f },
                                                { "msustain", 0.1f }, { "decay", 6000.0f }, { "sustain", 0.0f }, { "release", 1500.0f },
                                                { "keyscale", 0.5f }, { "chorus", 0.4f } } },
                        P { "FM Bass", "Basses", { { "algorithm", 0 }, { "level2", 0.7f }, { "level3", 0.0f }, { "level4", 0.0f },
                                                    { "feedback", 0.35f }, { "mdecay", 300.0f }, { "msustain", 0.2f }, { "decay", 1500.0f },
                                                    { "sustain", 0.5f }, { "release", 120.0f }, { "keyscale", 0.0f }, { "chorus", 0.0f } } },
                        P { "FM Brass", "Brass", { { "algorithm", 2 }, { "ratio2", 1.0f }, { "ratio3", 1.0f }, { "ratio4", 1.0f }, { "level2", 0.55f },
                                                    { "level3", 0.3f }, { "level4", 0.25f }, { "feedback", 0.45f }, { "mattack", 90.0f },
                                                    { "mdecay", 600.0f }, { "msustain", 0.7f }, { "attack", 40.0f }, { "sustain", 0.85f },
                                                    { "release", 180.0f }, { "vibrato", 0.25f }, { "chorus", 0.35f }, { "level", -3.0f } } },
                        P { "Glass Mallet", "Mallets", { { "ratio2", 3.0f }, { "level2", 0.45f }, { "ratio4", 9.0f }, { "level4", 0.25f },
                                                          { "mdecay", 250.0f }, { "msustain", 0.0f }, { "decay", 1600.0f }, { "sustain", 0.0f },
                                                          { "release", 600.0f } } },
                        P { "Drawbar FM Organ", "Organs", { { "algorithm", 6 }, { "ratio2", 2.0f }, { "ratio3", 3.0f }, { "ratio4", 4.0f },
                                                             { "level2", 0.6f }, { "level3", 0.35f }, { "level4", 0.25f }, { "attack", 4.0f },
                                                             { "sustain", 1.0f }, { "release", 70.0f }, { "velamp", 0.0f }, { "chorus", 0.6f },
                                                             { "level", -2.0f } } },
                        P { "Digital Pad", "Pads", { { "algorithm", 3 }, { "ratio2", 2.0f }, { "ratio3", 3.0f }, { "ratio4", 0.5f }, { "level2", 0.4f },
                                                      { "level3", 0.25f }, { "level4", 0.3f }, { "mattack", 1200.0f }, { "mdecay", 3000.0f },
                                                      { "msustain", 0.5f }, { "attack", 700.0f }, { "sustain", 0.9f }, { "release", 1500.0f },
                                                      { "vibrato", 0.2f }, { "chorus", 0.7f } } } } });

    r.add<PadSynth> ({ "synth.pad", "Pad/Ensemble", "Synthesizers",
                       "Supersaw and ensemble pads: wide, slow and shimmering, with chorus and room",
                       juce::Colour (0xff7d5bb5), padKnobs(),
                       { P { "Supersaw Pad", "Pads", {} },
                         P { "Trance Stack", "Leads", { { "detune", 0.55f }, { "octave", 0.35f }, { "cutoff", 7000.0f }, { "fenv", 0.0f },
                                                         { "attack", 5.0f }, { "sustain", 1.0f }, { "release", 400.0f }, { "motion", 0.0f },
                                                         { "vel", 0.3f }, { "reverb", 0.3f }, { "level", -2.0f } } },
                         P { "Glass Pad", "Pads", { { "shape", 2 }, { "octave", 0.6f }, { "detune", 0.25f }, { "cutoff", 4000.0f },
                                                     { "attack", 900.0f }, { "reverb", 0.5f } } },
                         P { "Warm Analog Pad", "Pads", { { "shape", 3 }, { "sub", 0.3f }, { "detune", 0.25f }, { "cutoff", 1500.0f },
                                                           { "attack", 500.0f }, { "ensemble", 0.7f }, { "level", 1.0f } } },
                         P { "String Machine", "Pads", { { "shape", 1 }, { "saws", 1 }, { "detune", 0.2f }, { "cutoff", 3500.0f },
                                                          { "attack", 350.0f }, { "release", 900.0f }, { "ensemble", 0.95f }, { "vibrato", 0.2f } } },
                         P { "Dark Drone", "Pads", { { "shape", 4 }, { "cutoff", 450.0f }, { "reso", 0.4f }, { "motion", 0.6f },
                                                      { "motionrate", 0.07f }, { "attack", 2000.0f }, { "release", 3000.0f }, { "level", 3.0f } } },
                         P { "Airy Pad", "Pads", { { "shape", 3 }, { "air", 0.4f }, { "octave", 0.25f }, { "cutoff", 2600.0f },
                                                    { "ensemble", 0.8f }, { "reverb", 0.6f }, { "attack", 1200.0f } } } } });
}

} // namespace woodshed
