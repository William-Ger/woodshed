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

    void start (int note, float velocity, bool stolen)  { starting.request (note, velocity, stolen); releasePending = false; }
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
}

} // namespace woodshed
