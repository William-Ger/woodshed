#pragma once

#include "../engine/Pedal.h"

namespace woodshed
{

//==============================================================================
/** A timed MIDI-style event for an instrument, `offset` samples into the block. */
struct NoteEvent
{
    enum Kind : std::uint8_t { noteOn, noteOff, allNotesOff, sustain, pitchBend, modWheel };

    int offset = 0;
    Kind kind = noteOn;
    int note = 60;              // noteOn / noteOff
    float value = 0.0f;         // noteOn: velocity 0..1; sustain: 0 or 1; pitchBend: -1..1; modWheel: 0..1
};

/** Everything the UI and the library browser need to know about an instrument type. */
struct InstrumentInfo
{
    juce::String type;              // stable id stored in projects, e.g. "synth.analog"; never rename
    juce::String name;              // shown in the instrument header, e.g. "Analog Synth"
    juce::String category;          // one of InstrumentRegistry::categories()
    juce::String description;       // one line for the library browser
    juce::Colour colour;            // accent for the panel and the track icon
    std::vector<KnobSpec> knobs;    // at most Instrument::maxKnobs; never reorder or re-range once shipped

    /** Named sounds: knob id -> real value. Knobs a patch doesn't mention take their defaults. */
    struct Patch
    {
        juce::String name, category;
        std::vector<std::pair<juce::String, float>> values;
    };
    std::vector<Patch> patches;     // the first is the instrument's default sound
};

//==============================================================================
/**
    Base class for every software instrument.

    render() adds (never replaces) `numSamples` of stereo output to left/right, applying the
    events at their offsets. Knob values arrive in real units via knob(i) once per block,
    followed by update(). Never allocate, lock or log in update() / render().
*/
class Instrument
{
public:
    static constexpr int maxKnobs = 24;

    virtual ~Instrument() = default;

    void prepare (double newSampleRate, int newMaxBlock)
    {
        sampleRate = newSampleRate;
        maxBlock = newMaxBlock;
        prepared();
        reset();
    }

    /** Silences every voice and clears any state (filters, delay lines). */
    virtual void reset() = 0;
    virtual void update() {}
    virtual void render (float* left, float* right, int numSamples, const NoteEvent* events, int numEvents) = 0;

    /** How many samples late the output comes (lookahead, oversampling); the mixer delays the
        other tracks to match. */
    virtual int getLatencySamples() const                       { return 0; }

    float knob (int index) const                                { return knobValues[(size_t) index]; }
    void setKnob (int index, float realValue)                   { knobValues[(size_t) index] = realValue; }

protected:
    /** Allocate buffers here. sampleRate and maxBlock are valid. */
    virtual void prepared() {}

    double sampleRate = 48000.0;
    int maxBlock = 512;

private:
    std::array<float, maxKnobs> knobValues {};
};

//==============================================================================
/**
    A polyphonic instrument made of identical voices: voice allocation, stealing, the sustain
    pedal, pitch bend and the mod wheel are handled here, so an instrument only writes a Voice.

    Voice must provide:
        void prepare (double sampleRate);
        void start (int note, float velocity, bool stolen);   // stolen: it was still sounding
        void release();                                      // key up (after the sustain pedal)
        void kill();                                         // silence now
        bool isActive() const;
        void render (float* left, float* right, int numSamples, const Owner&);   // adds
    and may read the owner's knobs, bend() (in semitones) and mod() while rendering.

    Voices and effects render into private scratch that is then added to the output, so
    processEffects() only sees this instrument. Allocate effect buffers in prepareEffects()
    (prepared() is used here).
*/
template <typename Owner, typename Voice, int numVoices = 16>
class PolyInstrument : public Instrument
{
public:
    void reset() override
    {
        for (auto& v : voices)
        {
            v.voice.kill();
            v.note = -1;
            v.held = v.sustained = false;
            v.age = 0;
        }
        sustainDown = false;
        bendTarget = bendNow = 0.0f;
        modValue = 0.0f;
        resetEffects();
    }

    void render (float* left, float* right, int numSamples, const NoteEvent* events, int numEvents) override
    {
        // Voices and effects run on private scratch, then get added to the caller's buffers,
        // so shared effects only ever see this instrument's own signal.
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
                if (until > done)
                    renderVoices (scratchL.data() + done, scratchR.data() + done, until - done);
                done = until;
                handle (events[e]);
            }

            if (n > done)
                renderVoices (scratchL.data() + done, scratchR.data() + done, n - done);

            processEffects (scratchL.data(), scratchR.data(), n);
            juce::FloatVectorOperations::add (left + start, scratchL.data(), n);
            juce::FloatVectorOperations::add (right + start, scratchR.data(), n);
            start += n;
        }

        for (; e < numEvents; ++e)
            handle (events[e]);
    }

    /** Pitch bend in semitones (smoothed), for voices to read. */
    float bend() const                                          { return bendNow; }
    float mod() const                                           { return modValue; }
    double getSampleRate() const                                { return sampleRate; }

    /** Semitones of bend at full deflection; override per instrument. */
    virtual float bendRange() const                             { return 2.0f; }
    /** Mono instruments (bass, leads) override this to retrigger/glide one voice. */
    virtual bool isMono() const                                 { return false; }

protected:
    void prepared() override
    {
        scratchL.assign ((size_t) juce::jmax (64, maxBlock), 0.0f);
        scratchR.assign ((size_t) juce::jmax (64, maxBlock), 0.0f);
        for (auto& v : voices)
            v.voice.prepare (sampleRate);
        prepareEffects();
    }

    /** Optional shared post-processing (chorus, cabinet, rotary...) on the summed voices. */
    virtual void prepareEffects() {}
    virtual void resetEffects() {}
    virtual void processEffects (float*, float*, int) {}

    struct Slot
    {
        Voice voice;
        int note = -1;
        bool held = false;           // key down
        bool sustained = false;      // key up, held by the pedal
        juce::uint64 age = 0;
    };

    std::array<Slot, (size_t) numVoices> voices;

private:
    void renderVoices (float* l, float* r, int n)
    {
        // Glide the bend once per sub-block; plenty smooth at audio block sizes.
        bendNow += (bendTarget - bendNow) * juce::jmin (1.0f, (float) n / (float) (sampleRate * 0.01));
        for (auto& v : voices)
            if (v.voice.isActive())
                v.voice.render (l, r, n, static_cast<const Owner&> (*this));
            else
                v.note = -1;
    }

    void handle (const NoteEvent& ev)
    {
        switch (ev.kind)
        {
            case NoteEvent::noteOn:
            {
                if (ev.value <= 0.0f) { noteOff (ev.note); break; }
                auto& slot = pickVoice (ev.note);
                const bool stolen = slot.voice.isActive();
                slot.note = ev.note;
                slot.held = true;
                slot.sustained = false;
                slot.age = ++clock;
                slot.voice.start (ev.note, juce::jlimit (0.0f, 1.0f, ev.value), stolen);
                break;
            }
            case NoteEvent::noteOff:      noteOff (ev.note); break;
            case NoteEvent::allNotesOff:
                for (auto& v : voices) { v.voice.release(); v.held = v.sustained = false; }
                sustainDown = false;
                break;
            case NoteEvent::sustain:
                sustainDown = ev.value >= 0.5f;
                if (! sustainDown)
                    for (auto& v : voices)
                        if (v.sustained) { v.sustained = false; v.voice.release(); }
                break;
            case NoteEvent::pitchBend:    bendTarget = juce::jlimit (-1.0f, 1.0f, ev.value) * bendRange(); break;
            case NoteEvent::modWheel:     modValue = juce::jlimit (0.0f, 1.0f, ev.value); break;
        }
    }

    void noteOff (int note)
    {
        for (auto& v : voices)
            if (v.note == note && v.held)
            {
                v.held = false;
                if (sustainDown) v.sustained = true;
                else             v.voice.release();
            }
    }

    Slot& pickVoice (int note)
    {
        if (isMono())
            return voices[0];

        // The same note again reuses its voice (a repeated piano key), else a free voice,
        // else the oldest released one, else the oldest held one.
        for (auto& v : voices) if (v.note == note && v.voice.isActive()) return v;
        for (auto& v : voices) if (! v.voice.isActive()) return v;

        Slot* oldest = nullptr;
        for (auto& v : voices)
            if (! v.held && (oldest == nullptr || v.age < oldest->age)) oldest = &v;
        if (oldest != nullptr) return *oldest;

        oldest = &voices[0];
        for (auto& v : voices)
            if (v.age < oldest->age) oldest = &v;
        return *oldest;
    }

    std::vector<float> scratchL, scratchR;
    bool sustainDown = false;
    float bendTarget = 0.0f, bendNow = 0.0f, modValue = 0.0f;
    juce::uint64 clock = 0;
};

//==============================================================================
struct InstrumentType
{
    InstrumentInfo info;
    std::function<std::unique_ptr<Instrument>()> create;
};

/** Every software instrument Woodshed knows about, in library order. */
class InstrumentRegistry
{
public:
    static const InstrumentRegistry& get();

    const InstrumentType* find (const juce::String& type) const;
    const std::vector<InstrumentType>& all() const              { return types; }

    /** Library sections, in display order. */
    static const juce::StringArray& categories();

    template <typename InstrumentClass>
    void add (InstrumentInfo info)
    {
        jassert (info.knobs.size() <= (size_t) Instrument::maxKnobs);
        jassert (find (info.type) == nullptr);
        types.push_back ({ std::move (info), [] { return std::unique_ptr<Instrument> (new InstrumentClass()); } });
    }

private:
    InstrumentRegistry();
    std::vector<InstrumentType> types;
};

/** Knob values (real units, in knob order) for a patch of `info`: defaults overridden by the patch. */
std::vector<float> patchValues (const InstrumentInfo& info, const InstrumentInfo::Patch* patch);

// Each family lives in its own file under Source/instruments and registers here.
void registerSynthInstruments (InstrumentRegistry&);
void registerKeyboardInstruments (InstrumentRegistry&);
void registerDrumInstruments (InstrumentRegistry&);
void registerOrchestralInstruments (InstrumentRegistry&);

} // namespace woodshed
