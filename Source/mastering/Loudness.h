#pragma once

#include <juce_dsp/juce_dsp.h>

namespace woodshed::mastering
{

/** Loudness in LUFS (or LU) that means "nothing measured yet". */
constexpr float silentLufs = -std::numeric_limits<float>::infinity();

/** One biquad (transposed direct form II), in double precision so very low cutoffs stay exact. */
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept                           { z1 = z2 = 0.0; }
};

/** ITU-R BS.1770 K-weighting: the head's high shelf (+4 dB above about 1.7 kHz), then the
    RLB high-pass (38 Hz). Derived from the filters' analog parameters, so it holds at any
    sample rate; at 48 kHz it gives the standard's coefficients. */
struct KWeighting
{
    Biquad shelf, highPass;

    void prepare (double sampleRate);
    void reset() noexcept                           { shelf.reset(); highPass.reset(); }
    double process (double x) noexcept              { return highPass.process (shelf.process (x)); }
};

/**
    Peaks between samples: the signal oversampled through a Kaiser-windowed-sinc interpolator,
    and the largest of the `factor` points around each input sample. Catches the peaks a DAC
    (or a lossy encoder) will produce.

    The interpolator delays by `latency` samples: the points returned for input n lie at
    n - latency, n - latency + 1 / factor, ... n - latency + (factor - 1) / factor.
*/
template <int factor, int tapsPerPhase = 32>
class PeakInterpolator
{
public:
    static constexpr int latency = tapsPerPhase / 2;

    PeakInterpolator()                              { reset(); }
    void reset() noexcept                           { history.fill (0.0f); pos = 0; }

    /** The largest absolute value among the interpolated points of this sample. */
    float process (float x) noexcept
    {
        history[(size_t) pos] = x;
        history[(size_t) (pos + tapsPerPhase)] = x;
        pos = (pos + 1) % tapsPerPhase;

        // history[pos .. pos + taps) is oldest .. newest.
        const float* h = history.data() + pos;
        const auto& c = coefficients();
        float most = 0.0f;
        for (int p = 0; p < factor; ++p)
        {
            const float* k = c[(size_t) p].data();
            float sum = 0.0f;
            for (int j = 0; j < tapsPerPhase; ++j)
                sum += k[j] * h[j];
            most = juce::jmax (most, std::abs (sum));
        }
        return most;
    }

    /** Phase p's taps, oldest sample first. Cut off at the original Nyquist and centred on a
        tap, so phase 0 is the input sample itself; each phase has unity gain at DC. Longer
        filters have a narrower transition, so they keep more of the top octave's peaks. */
    using Phases = std::array<std::array<float, tapsPerPhase>, factor>;
    static const Phases& coefficients()
    {
        static const Phases phases = []
        {
            constexpr int length = factor * tapsPerPhase;
            constexpr double centre = (double) (tapsPerPhase / 2 * factor), beta = tapsPerPhase >= 64 ? 8.0 : 7.0;
            auto bessel0 = [] (double x)
            {
                double sum = 1.0, term = 1.0;
                for (int k = 1; k < 40; ++k)
                {
                    term *= (x / (2.0 * k)) * (x / (2.0 * k));
                    sum += term;
                }
                return sum;
            };

            // h[i] multiplies the input sample i / factor before the output point.
            std::array<double, length> h {};
            for (int i = 0; i < length; ++i)
            {
                const double t = ((double) i - centre) / (double) factor;
                const double sinc = std::abs (t) < 1.0e-12 ? 1.0 : std::sin (juce::MathConstants<double>::pi * t) / (juce::MathConstants<double>::pi * t);
                const double r = ((double) i - centre) / centre;
                const double w = std::abs (r) <= 1.0 ? bessel0 (beta * std::sqrt (1.0 - r * r)) / bessel0 (beta) : 0.0;
                h[(size_t) i] = sinc * w;
            }

            // Output point at n - latency + p / factor; tap j holds input n - (taps - 1) + j.
            Phases out {};
            for (int p = 0; p < factor; ++p)
            {
                std::array<double, tapsPerPhase> taps {};
                double sum = 0.0;
                for (int j = 0; j < tapsPerPhase; ++j)
                {
                    const int age = tapsPerPhase - 1 - j;                       // samples before n
                    const int i = (age - latency) * factor + p + (int) centre;  // distance in 1/factor steps
                    taps[(size_t) j] = juce::isPositiveAndBelow (i, length) ? h[(size_t) i] : 0.0;
                    sum += taps[(size_t) j];
                }
                for (int j = 0; j < tapsPerPhase; ++j)
                    out[(size_t) p][(size_t) j] = (float) (taps[(size_t) j] / sum);
            }
            return out;
        }();
        return phases;
    }

private:
    std::array<float, tapsPerPhase * 2> history {};
    int pos = 0;
};

/** True peak as BS.1770-4 Annex 2 measures it: 4x oversampling (with a longer interpolator
    than the standard's example, so content near Nyquist reads neither high nor low). */
using TruePeak = PeakInterpolator<4, 64>;

/**
    An EBU R128 loudness meter for a stereo signal: momentary (400 ms), short-term (3 s) and
    integrated (gated: absolute at -70 LUFS, relative at -10 LU) loudness, loudness range
    (EBU Tech 3342: short-term values gated at -70 LUFS and -20 LU, 10th to 95th percentile)
    and true and sample peak. Feed it any block sizes; read it whenever. Not real-time safe
    (the gating history grows), so it runs on the message thread or offline.
*/
class LoudnessMeter
{
public:
    void prepare (double sampleRate);
    void reset();

    /** `measureTruePeak` false skips the oversampling (a quicker integrated-only pass). */
    void process (const float* left, const float* right, int numSamples);
    void setMeasureTruePeak (bool on)               { measureTruePeak = on; }

    float momentary() const;                        // LUFS
    float shortTerm() const;                        // LUFS
    float integrated() const;                       // LUFS
    float loudnessRange() const;                    // LU
    float maxMomentary() const                      { return toLufs (maxMomentaryEnergy); }
    float maxShortTerm() const                      { return toLufs (maxShortEnergy); }
    float truePeakDb() const                        { return juce::Decibels::gainToDecibels (truePeak, -200.0f); }
    float samplePeakDb() const                      { return juce::Decibels::gainToDecibels (samplePeak, -200.0f); }
    /** Seconds measured. */
    double seconds() const                          { return (double) totalSamples / rate; }

    static float toLufs (double energy)
    {
        return energy > 0.0 ? (float) (-0.691 + 10.0 * std::log10 (energy)) : silentLufs;
    }
    static double toEnergy (float lufs)             { return std::pow (10.0, ((double) lufs + 0.691) / 10.0); }

private:
    double windowEnergy (int subBlocks) const;

    double rate = 48000.0;
    std::array<KWeighting, 2> weighting;
    std::array<TruePeak, 2> peaks;
    bool measureTruePeak = true;

    // 100 ms sub-blocks: their summed mean squares, the last 30 of them in a ring.
    int subLength = 4800, inSub = 0;
    double subSum = 0.0;
    std::array<double, 30> subs {};
    juce::int64 numSubs = 0, totalSamples = 0;

    std::vector<double> gatingBlocks;               // 400 ms, every 100 ms
    std::vector<double> shortBlocks;                // 3 s, every 100 ms
    double maxMomentaryEnergy = 0.0, maxShortEnergy = 0.0;
    float truePeak = 0.0f, samplePeak = 0.0f;
};

/** Stereo correlation, -1 (out of phase) .. +1 (mono), over about 300 ms. */
class CorrelationMeter
{
public:
    void prepare (double sampleRate)                { coeff = 1.0 - std::exp (-1.0 / (0.3 * sampleRate)); reset(); }
    void reset()                                    { lr = ll = rr = 0.0; }
    void process (const float* left, const float* right, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            lr += coeff * ((double) left[i] * right[i] - lr);
            ll += coeff * ((double) left[i] * left[i] - ll);
            rr += coeff * ((double) right[i] * right[i] - rr);
        }
    }
    float value() const                             { return ll * rr > 1.0e-14 ? (float) (lr / std::sqrt (ll * rr)) : 1.0f; }

private:
    double coeff = 0.001, lr = 0.0, ll = 0.0, rr = 0.0;
};

/**
    A spectrum: 4096-point Hann-windowed FFTs every 2048 samples of the mono sum, their power
    either averaged over everything (the long-term spectrum of a song) or followed with a
    ~300 ms decay (a live analyser).
*/
class SpectrumAnalyser
{
public:
    static constexpr int order = 12, size = 1 << order;

    explicit SpectrumAnalyser (bool longTermAverage = true);
    void prepare (double sampleRate);
    void reset();
    void process (const float* left, const float* right, int numSamples);

    /** Power in dB (a full-scale sine reads about 0) at a frequency, smoothed over 1/6 octave. */
    float levelAt (float hz) const;
    /** Total power in dB between two frequencies. */
    float bandLevel (float fromHz, float toHz) const;
    bool hasData() const                            { return frames > 0; }

private:
    void analyseFrame();

    juce::dsp::FFT fft { order };
    std::vector<float> window, fifo, work;
    std::vector<double> power;                      // size / 2 + 1 bins
    int filled = 0;
    juce::int64 frames = 0;
    double rate = 48000.0;
    bool average = true;
};

} // namespace woodshed::mastering
