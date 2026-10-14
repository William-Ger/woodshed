#pragma once

#include <juce_dsp/juce_dsp.h>

namespace woodshed
{

/**
    Monophonic pitch detection with the McLeod Pitch Method: a normalised
    square-difference function computed from an FFT autocorrelation, then the
    first "key maximum" above a fraction of the highest one, refined with a
    parabola. Runs on the message thread (it allocates in the constructor only).
*/
class PitchDetector
{
public:
    static constexpr int windowSize = 4096;

    struct Result
    {
        float frequency = 0.0f;   // Hz, 0 when nothing confident was found
        float clarity = 0.0f;     // 0..1, how periodic the window is
        float level = 0.0f;       // RMS of the window
    };

    PitchDetector() : fft (fftOrder), work ((size_t) fftSize * 2), nsdf ((size_t) windowSize) {}

    /** `samples` must hold windowSize floats, oldest first. */
    Result analyse (const float* samples, double sampleRate, float minHz = 27.0f, float maxHz = 1400.0f)
    {
        Result r;

        double mean = 0.0;
        for (int i = 0; i < windowSize; ++i) mean += samples[i];
        mean /= windowSize;

        double energy = 0.0;
        for (int i = 0; i < windowSize; ++i)
        {
            const float v = samples[i] - (float) mean;
            work[(size_t) i] = v;
            energy += (double) v * v;
        }

        r.level = (float) std::sqrt (energy / windowSize);
        if (r.level < 1.0e-4f)
            return r;

        // Autocorrelation via the power spectrum of the zero-padded window.
        std::fill (work.begin() + windowSize, work.end(), 0.0f);
        fft.performRealOnlyForwardTransform (work.data());
        for (int k = 0; k <= fftSize / 2; ++k)
        {
            const float re = work[(size_t) (2 * k)], im = work[(size_t) (2 * k + 1)];
            work[(size_t) (2 * k)] = re * re + im * im;
            work[(size_t) (2 * k + 1)] = 0.0f;
        }
        fft.performRealOnlyInverseTransform (work.data());

        const int maxLag = juce::jmin (windowSize - 2, (int) (sampleRate / minHz));
        const int minLag = juce::jmax (2, (int) (sampleRate / maxHz));

        // m(tau) = sum of x[j]^2 + x[j+tau]^2 over the overlap, updated incrementally.
        double m = 2.0 * energy;
        for (int tau = 0; tau < maxLag; ++tau)
        {
            if (tau > 0)
            {
                const double a = samples[tau - 1] - mean, b = samples[windowSize - tau] - mean;
                m -= a * a + b * b;
            }

            nsdf[(size_t) tau] = m > 1.0e-12 ? (float) (2.0 * work[(size_t) tau] / m) : 0.0f;
        }

        // Key maxima: the highest point between each upward and downward zero crossing.
        std::array<int, 64> peaks {};
        int numPeaks = 0, tau = 1;
        while (tau < maxLag && nsdf[(size_t) tau] > 0.0f) ++tau;   // skip the lobe around zero lag

        while (tau < maxLag && numPeaks < (int) peaks.size())
        {
            while (tau < maxLag && nsdf[(size_t) tau] <= 0.0f) ++tau;
            int best = -1;
            while (tau < maxLag && nsdf[(size_t) tau] > 0.0f)
            {
                if (tau >= minLag && (best < 0 || nsdf[(size_t) tau] > nsdf[(size_t) best]))
                    best = tau;
                ++tau;
            }
            if (best > 0) peaks[(size_t) numPeaks++] = best;
        }

        if (numPeaks == 0)
            return r;

        float highest = 0.0f;
        for (int i = 0; i < numPeaks; ++i)
            highest = juce::jmax (highest, nsdf[(size_t) peaks[(size_t) i]]);

        const float threshold = 0.9f * highest;
        int chosen = peaks[0];
        for (int i = 0; i < numPeaks; ++i)
        {
            if (nsdf[(size_t) peaks[(size_t) i]] >= threshold)
            {
                chosen = peaks[(size_t) i];
                break;
            }
        }

        // Parabolic interpolation around the chosen peak.
        const float y0 = nsdf[(size_t) chosen - 1], y1 = nsdf[(size_t) chosen], y2 = nsdf[(size_t) chosen + 1];
        const float denom = y0 - 2.0f * y1 + y2;
        const float shift = std::abs (denom) > 1.0e-9f ? 0.5f * (y0 - y2) / denom : 0.0f;
        const float period = (float) chosen + juce::jlimit (-0.5f, 0.5f, shift);

        r.clarity = y1 - 0.25f * (y0 - y2) * shift;
        r.frequency = (float) sampleRate / period;
        return r;
    }

private:
    static constexpr int fftOrder = 13, fftSize = 1 << fftOrder;   // 8192 >= 2 * windowSize
    juce::dsp::FFT fft;
    std::vector<float> work, nsdf;
};

} // namespace woodshed
