#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_graphics/juce_graphics.h>

namespace woodshed
{

//==============================================================================
/** One knob (or stepped selector) on a pedal, in real units. */
struct KnobSpec
{
    juce::String id, label;
    float min = 0.0f, max = 1.0f, def = 0.5f;
    float skew = 1.0f;              // NormalisableRange skew; < 1 gives more travel to low values
    juce::String unit;              // "dB", "ms", "Hz", "%", "st", "x" or empty
    juce::StringArray choices;      // non-empty: a stepped selector, value is the choice index

    bool isChoice() const                       { return ! choices.isEmpty(); }

    juce::NormalisableRange<float> range() const
    {
        if (isChoice())
            return { 0.0f, (float) (choices.size() - 1), 1.0f };

        juce::NormalisableRange<float> r (min, max);
        r.skew = skew;
        return r;
    }

    float toReal (float norm) const             { return range().convertFrom0to1 (juce::jlimit (0.0f, 1.0f, norm)); }
    float toNorm (float real) const             { return range().convertTo0to1 (juce::jlimit (range().start, range().end, real)); }
    float defaultNorm() const                   { return toNorm (def); }

    juce::String format (float real) const
    {
        if (isChoice())
            return choices[juce::jlimit (0, choices.size() - 1, juce::roundToInt (real))];

        if (unit == "%")  return juce::String (juce::roundToInt (real * 100.0f)) + "%";
        if (unit == "ms") return real >= 1000.0f ? juce::String (real / 1000.0f, 2) + " s" : juce::String (juce::roundToInt (real)) + " ms";
        if (unit == "Hz") return real >= 1000.0f ? juce::String (real / 1000.0f, 1) + " kHz" : juce::String (real, real < 10.0f ? 2 : 0) + " Hz";
        if (unit == "dB") return (real > 0.0f ? "+" : "") + juce::String (real, 1) + " dB";
        if (unit == "st") return (real > 0.0f ? "+" : "") + juce::String (juce::roundToInt (real)) + " st";
        if (unit == "x")  return juce::String (real, 2) + "x";
        return juce::String (real, 2);
    }
};

/** A continuous knob: knob ("time", "Time", 40, 1200, 380, "ms", 0.6f). */
inline KnobSpec knob (juce::String id, juce::String label, float min, float max, float def,
                      juce::String unit = {}, float skew = 1.0f)
{
    KnobSpec k;
    k.id = std::move (id); k.label = std::move (label);
    k.min = min; k.max = max; k.def = def; k.skew = skew; k.unit = std::move (unit);
    return k;
}

/** A stepped selector: choice ("voice", "Voice", { "Clean", "Crunch", "Lead" }, 1). */
inline KnobSpec choice (juce::String id, juce::String label, juce::StringArray choices, int defaultIndex = 0)
{
    KnobSpec k;
    k.id = std::move (id); k.label = std::move (label);
    k.choices = std::move (choices);
    k.min = 0.0f; k.max = (float) (k.choices.size() - 1); k.def = (float) defaultIndex;
    return k;
}

/** Everything the UI and the library browser need to know about a pedal type. */
struct PedalInfo
{
    juce::String type;              // stable id stored in presets, e.g. "drive.overdrive"; never rename
    juce::String name;              // printed on the enclosure, e.g. "GRIT"
    juce::String subtitle;          // e.g. "overdrive"
    juce::String category;          // one of Registry::categories()
    juce::String description;       // one line for the library browser
    juce::Colour body, ink;
    std::vector<KnobSpec> knobs;    // at most Pedal::maxKnobs
};

//==============================================================================
/**
    Base class for every effect on the board.

    Mono-style pedals implement processChannel() and keep independent state for
    channel 0 and channel 1 (the chain only runs channel 1 once the signal has
    gone stereo). Pedals that create stereo (chorus, ping-pong, reverbs...)
    return true from producesStereo() and implement processStereo().

    Knob values arrive in real units once per block via knob(i), followed by a
    call to update() on the audio thread. Smooth anything that would zipper.
    Never allocate, lock or log in update() / process*().
*/
class Pedal
{
public:
    static constexpr int maxKnobs = 8;

    virtual ~Pedal() = default;

    void prepare (double newSampleRate, int newMaxBlock)
    {
        sampleRate = newSampleRate;
        maxBlock = newMaxBlock;
        prepared();
        reset();
    }

    virtual void reset() = 0;
    virtual void update() {}
    virtual void processChannel (int channel, float* data, int numSamples) = 0;

    virtual bool producesStereo() const                         { return false; }
    virtual void processStereo (float* left, float* right, int numSamples)
    {
        processChannel (0, left, numSamples);
        processChannel (1, right, numSamples);
    }

    virtual int getLatencySamples() const                       { return 0; }

    float knob (int index) const                                { return knobValues[(size_t) index]; }
    void setKnob (int index, float realValue)                   { knobValues[(size_t) index] = realValue; }

    /** Bypass crossfade, owned by the chain. */
    juce::SmoothedValue<float> engaged;

    /** Set while the board is being rearranged: the chain stops feeding knob values
        so the pedal can't briefly read another slot's knobs. */
    std::atomic<bool> knobsHeld { false };

protected:
    /** Allocate buffers and set up filters here. sampleRate and maxBlock are valid. */
    virtual void prepared() {}

    double sampleRate = 48000.0;
    int maxBlock = 512;

private:
    std::array<float, maxKnobs> knobValues {};
};

//==============================================================================
// Small helpers shared by pedal implementations.
namespace dsp
{
    inline float dbToGain (float db)                            { return juce::Decibels::decibelsToGain (db); }

    /** One-pole smoothing coefficient for a cutoff in Hz. */
    inline float onePoleCoeff (float hz, double sr)
    {
        return 1.0f - std::exp (-juce::MathConstants<float>::twoPi * hz / (float) sr);
    }

    /** Topology-preserving state-variable filter (Simper/Zavalishin), cheap to modulate per sample. */
    struct Svf
    {
        float ic1 = 0.0f, ic2 = 0.0f;
        float g = 0.0f, k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;

        void setup (float cutoffHz, float q, double sr)
        {
            g = std::tan (juce::MathConstants<float>::pi * juce::jlimit (10.0f, (float) sr * 0.49f, cutoffHz) / (float) sr);
            k = 1.0f / juce::jmax (0.05f, q);
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }

        void reset()                                            { ic1 = ic2 = 0.0f; }

        struct Out { float low, band, high; };

        Out process (float v0)
        {
            const float v3 = v0 - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            return { v2, v1, v0 - k * v1 - v2 };
        }
    };

    /** Fractional delay line with cubic (Hermite) interpolation. Size it in prepared(). */
    struct DelayBuffer
    {
        std::vector<float> data;
        int writePos = 0, mask = 0;

        void setMaxSamples (int maxSamples)
        {
            int size = 1;
            while (size < maxSamples + 4) size <<= 1;
            data.assign ((size_t) size, 0.0f);
            mask = size - 1;
            writePos = 0;
        }

        void clear()                                            { std::fill (data.begin(), data.end(), 0.0f); }
        void push (float x)                                     { data[(size_t) writePos] = x; writePos = (writePos + 1) & mask; }

        /** Read `delay` samples behind the most recently pushed sample. Keep delay >= 2 (cubic needs a sample ahead). */
        float read (float delay) const
        {
            const float pos = (float) writePos - 1.0f - delay;
            const int i = (int) std::floor (pos);
            const float f = pos - (float) i;
            auto at = [this] (int idx) { return data[(size_t) (idx & mask)]; };
            const float xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return ((c3 * f + c2) * f + c1) * f + x0;
        }
    };
}

} // namespace woodshed
