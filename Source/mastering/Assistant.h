#pragma once

#include "MasteringProcessor.h"

namespace woodshed::mastering
{

/** A loudness target: integrated LUFS and the true-peak ceiling. */
struct Target
{
    juce::String name;
    float lufs = -14.0f, truePeak = -1.0f;
};

/** Streaming (-14 LUFS, -1 dBTP), Podcast (-16), CD (-11), Club / Loud (-9). The panel adds Custom. */
const std::vector<Target>& targetPresets();

/** The spectral balance the matching EQ steers toward: each band's power in dB, relative to the
    mean of the middle six. A smooth, modern master: a little weight in the lows, a gentle fall
    above the presence region. */
const Curve& referenceBalance();

/** The 1/6-octave frequencies (20 Hz .. 20 kHz) a Measurement's spectrum is given at. */
const std::vector<float>& spectrumFrequencies();

/** What a mix measures. */
struct Measurement
{
    float integrated = silentLufs, range = 0.0f, truePeak = -200.0f, samplePeak = -200.0f, maxShortTerm = silentLufs;
    Curve bands {};                     // each band's power, dB relative to the middle six's mean
    std::vector<float> spectrum;        // dB at spectrumFrequencies()
    double seconds = 0.0;
};

Measurement measure (const juce::AudioBuffer<float>& audio, double sampleRate);

/** A measurement as JSON-friendly data (kept on the processor's FX node so the panel can show
    before and after), and back. */
juce::var toVar (const Measurement&);
Measurement measurementFrom (const juce::var&);

/** How the assistant should master. */
struct Settings
{
    Target target;
    Character character = Character::clean;
    float eqAmount = 1.0f, compress = 0.3f, width = 1.0f, monoHz = 100.0f;
};

struct Result
{
    bool ok = false;
    juce::String message;               // why it failed, or a note (a target out of reach)
    std::array<float, knobs::numKnobs> knobValues {};
    Curve curve {};
    Measurement before, after;
};

/** The matching EQ curve (the processor's band gains) that moves `mix` toward referenceBalance(). */
Curve matchingCurve (const Measurement& mix, double sampleRate);

/** Knob values for these settings at `gainDb`. */
std::array<float, knobs::numKnobs> knobValuesFor (const Settings&, float gainDb);

/** The processor over a whole buffer, offline, with these knobs and curve (latency removed). */
juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& mix, double sampleRate,
                                 const std::array<float, knobs::numKnobs>& knobValues, const Curve&);

/**
    The mastering assistant. Measures the mix (unless `before` is given), builds the matching
    curve, then finds the gain that puts the processor's output on the target's integrated
    loudness by running the processor over the mix and measuring it (a secant search, as the
    compressor and limiter make loudness bend), and measures the result.

    Any thread. `progress` (0..1) returns false to cancel.
*/
Result master (const juce::AudioBuffer<float>& mix, double sampleRate, const Settings&,
               const Measurement* before = nullptr, std::function<bool (float)> progress = {});

} // namespace woodshed::mastering
