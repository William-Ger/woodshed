#pragma once

#include "Loudness.h"
#include "../engine/Pedal.h"

namespace woodshed::mastering
{

/** The Registry type of the mastering processor (the Stereo Out's last insert). */
inline const juce::String processorType { "dyn.mastering" };

/** Its knobs, in order. */
namespace knobs
{
    enum { character, gain, eq, compress, width, monoBass, ceiling, numKnobs };
}

/** The Character choices. */
enum class Character { clean, valve, punch, transparent };
const juce::StringArray& characterNames();

/** The matching EQ's bands: a low shelf, six bells and a high shelf, one per analysis band. */
constexpr int numBands = 8;
/** Band edges in Hz (numBands + 1 of them); the analysis measures the power between them. */
const std::array<float, numBands + 1>& bandEdges();
/** Each band's centre (the geometric mean of its edges). */
float bandCentre (int band);

using Curve = std::array<float, numBands>;      // dB per band
juce::String encodeCurve (const Curve&);
Curve decodeCurve (const juce::String&);

/** The EQ's response in dB at `hz`, for these band gains (amount already applied). */
float eqResponseDb (const Curve& gains, float hz, double sampleRate);

/**
    The mastering processor: gain, a smooth matching EQ, a bus compressor, the Character's
    colour, stereo width with mono lows, and a true-peak lookahead limiter, in that order.

    Knob values (real units): Character (choice), Gain (dB into the chain), EQ (how much of
    the matching curve, 0..1), Compress (0..1), Width (0..2, 1 = as mixed), Mono Bass (Hz; the
    side signal below it is removed, off at 20), Ceiling (dBTP).

    The matching curve isn't a knob: it's the FX node's `state` (encodeCurve), given through
    setState() on the message thread.

    The compressor's threshold follows the music's running level (3 s), so Compress means
    the same thing at any loudness: how hard the peaks above the song's own level get held.
    The limiter detects 8x oversampled true peaks, holds the deepest gain any of them needs
    across a lookahead window and averages it over that window, so the gain has fully arrived
    at every peak: the output never exceeds the ceiling, between samples too.
*/
class MasteringProcessor final : public Pedal
{
public:
    MasteringProcessor();

    void prepared() override;
    void reset() override;
    void update() override;
    void processChannel (int channel, float* data, int numSamples) override;
    bool producesStereo() const override                { return true; }
    void processStereo (float* left, float* right, int numSamples) override;

    // TODO: if the Pedal base gains a latencySamples() hook for delay compensation, report the
    // lookahead there too.
    int getLatencySamples() const override              { return latency; }

    /** Message thread: the matching curve (encodeCurve). */
    void setState (const juce::String& state) override;

    /** The gain reduction the limiter and compressor applied in the last block, dB (positive). */
    float getLimiterReduction() const                   { return limiterReduction.load (std::memory_order_relaxed); }
    float getCompressorReduction() const                { return compressorReduction.load (std::memory_order_relaxed); }

private:
    void updateEq (bool immediately);

    // Gain, width
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> inputGain { 1.0f };
    juce::SmoothedValue<float> widthAmount { 1.0f };
    float monoHz = 20.0f;
    std::array<Biquad, 2> sideHighPass;     // the side above Mono Bass (4th-order Linkwitz-Riley)

    // EQ
    std::array<std::atomic<float>, numBands> curve;
    Curve appliedGains {};
    std::array<std::array<Biquad, numBands>, 2> eqFilters;
    bool eqFlat = true;

    // Compressor (stereo-linked)
    double compEnv = 0.0, compRef = 0.0, compAttack = 0.0, compRelease = 0.0, compRefCoeff = 0.0;
    float compRatio = 2.0f, compAmount = 0.0f, compGainDb = 0.0f;

    // Character
    Character character = Character::clean;
    std::array<double, 2> dcIn {}, dcOut {};
    double dcCoeff = 0.999;

    // Limiter
    // The limiter looks at 8x oversampled peaks through a long interpolator: finer than a 4x
    // meter, so it doesn't miss high-frequency peaks that fall between a meter's points or
    // that a shorter filter's transition band shaves off.
    using Detector = PeakInterpolator<8, 64>;
    std::array<Detector, 2> detectors;
    std::vector<float> delayL, delayR;      // the signal, waiting for its gain
    int delayMask = 0, writePos = 0;
    std::vector<float> minValues;           // sliding-minimum deque (values and their sample numbers)
    std::vector<juce::int64> minTimes;
    int minHead = 0, minTail = 0, minMask = 0;
    std::vector<double> boxValues;          // the averaging window
    int boxPos = 0;
    double boxSum = 0.0;
    double released = 1.0, releaseCoeff = 0.001;
    juce::int64 sampleCount = 0;
    int window = 64, slack = 3, latency = 78;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> ceilingGain { 0.89f };

    std::atomic<float> limiterReduction { 0.0f }, compressorReduction { 0.0f };
};

} // namespace woodshed::mastering
