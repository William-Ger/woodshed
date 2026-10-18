#include "Loudness.h"

namespace woodshed::mastering
{

//==============================================================================
void KWeighting::prepare (double sampleRate)
{
    // Stage 1: the head's high shelf.
    {
        const double f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
        const double vh = std::pow (10.0, gainDb / 20.0);
        const double vb = std::pow (vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf.b0 = (vh + vb * k / q + k * k) / a0;
        shelf.b1 = 2.0 * (k * k - vh) / a0;
        shelf.b2 = (vh - vb * k / q + k * k) / a0;
        shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf.a2 = (1.0 - k / q + k * k) / a0;
    }

    // Stage 2: the revised low-frequency B-curve high-pass.
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
        const double a0 = 1.0 + k / q + k * k;
        highPass.b0 = 1.0;
        highPass.b1 = -2.0;
        highPass.b2 = 1.0;
        highPass.a1 = 2.0 * (k * k - 1.0) / a0;
        highPass.a2 = (1.0 - k / q + k * k) / a0;
    }

    reset();
}

//==============================================================================
void LoudnessMeter::prepare (double sampleRate)
{
    rate = sampleRate;
    subLength = juce::jmax (1, juce::roundToInt (sampleRate * 0.1));
    for (auto& w : weighting)
        w.prepare (sampleRate);
    reset();
}

void LoudnessMeter::reset()
{
    for (auto& w : weighting) w.reset();
    for (auto& p : peaks) p.reset();
    inSub = 0;
    subSum = 0.0;
    subs.fill (0.0);
    numSubs = totalSamples = 0;
    gatingBlocks.clear();
    shortBlocks.clear();
    maxMomentaryEnergy = maxShortEnergy = 0.0;
    truePeak = samplePeak = 0.0f;
}

double LoudnessMeter::windowEnergy (int count) const
{
    double sum = 0.0;
    for (int i = 0; i < count; ++i)
        sum += subs[(size_t) ((numSubs - 1 - i) % (juce::int64) subs.size())];
    return sum / count;
}

void LoudnessMeter::process (const float* left, const float* right, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = weighting[0].process ((double) left[i]);
        const double r = weighting[1].process ((double) right[i]);
        subSum += l * l + r * r;

        samplePeak = juce::jmax (samplePeak, std::abs (left[i]), std::abs (right[i]));
        if (measureTruePeak)
            truePeak = juce::jmax (truePeak, peaks[0].process (left[i]), peaks[1].process (right[i]));

        if (++inSub == subLength)
        {
            subs[(size_t) (numSubs % (juce::int64) subs.size())] = subSum / subLength;
            ++numSubs;
            inSub = 0;
            subSum = 0.0;

            if (numSubs >= 4)
            {
                const auto e = windowEnergy (4);
                gatingBlocks.push_back (e);
                maxMomentaryEnergy = juce::jmax (maxMomentaryEnergy, e);
            }
            if (numSubs >= 30)
            {
                const auto e = windowEnergy (30);
                shortBlocks.push_back (e);
                maxShortEnergy = juce::jmax (maxShortEnergy, e);
            }
        }
    }

    totalSamples += numSamples;
    if (! measureTruePeak)
        truePeak = juce::jmax (truePeak, samplePeak);
}

float LoudnessMeter::momentary() const
{
    return numSubs >= 4 ? toLufs (windowEnergy (4)) : silentLufs;
}

float LoudnessMeter::shortTerm() const
{
    return numSubs >= 30 ? toLufs (windowEnergy (30)) : silentLufs;
}

float LoudnessMeter::integrated() const
{
    const double absolute = toEnergy (-70.0f);
    double sum = 0.0;
    int n = 0;
    for (auto e : gatingBlocks)
        if (e > absolute) { sum += e; ++n; }
    if (n == 0)
        return silentLufs;

    const double relative = juce::jmax (absolute, sum / n * std::pow (10.0, -1.0));    // -10 LU
    sum = 0.0;
    n = 0;
    for (auto e : gatingBlocks)
        if (e > relative) { sum += e; ++n; }
    return n > 0 ? toLufs (sum / n) : silentLufs;
}

float LoudnessMeter::loudnessRange() const
{
    const double absolute = toEnergy (-70.0f);
    double sum = 0.0;
    int n = 0;
    for (auto e : shortBlocks)
        if (e > absolute) { sum += e; ++n; }
    if (n < 2)
        return 0.0f;

    const double relative = juce::jmax (absolute, sum / n * std::pow (10.0, -2.0));    // -20 LU
    std::vector<float> values;
    values.reserve ((size_t) n);
    for (auto e : shortBlocks)
        if (e > relative)
            values.push_back (toLufs (e));
    if (values.size() < 2)
        return 0.0f;

    std::sort (values.begin(), values.end());
    auto at = [&values] (double fraction)
    {
        return values[(size_t) juce::roundToInt (fraction * (double) (values.size() - 1))];
    };
    return at (0.95) - at (0.10);
}

//==============================================================================
SpectrumAnalyser::SpectrumAnalyser (bool longTermAverage) : average (longTermAverage)
{
    window.resize ((size_t) size);
    juce::dsp::WindowingFunction<float>::fillWindowingTables (window.data(), (size_t) size,
                                                              juce::dsp::WindowingFunction<float>::hann, false);
    fifo.assign ((size_t) size, 0.0f);
    work.assign ((size_t) size * 2, 0.0f);
    power.assign ((size_t) size / 2 + 1, 0.0);
}

void SpectrumAnalyser::prepare (double sampleRate)
{
    rate = sampleRate;
    reset();
}

void SpectrumAnalyser::reset()
{
    std::fill (fifo.begin(), fifo.end(), 0.0f);
    std::fill (power.begin(), power.end(), 0.0);
    filled = 0;
    frames = 0;
}

void SpectrumAnalyser::process (const float* left, const float* right, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        fifo[(size_t) filled++] = 0.5f * (left[i] + right[i]);
        if (filled == size)
        {
            analyseFrame();
            std::copy (fifo.begin() + size / 2, fifo.end(), fifo.begin());     // hop by half a frame
            filled = size / 2;
        }
    }
}

void SpectrumAnalyser::analyseFrame()
{
    double windowEnergy = 0.0;
    for (int i = 0; i < size; ++i)
    {
        work[(size_t) i] = fifo[(size_t) i] * window[(size_t) i];
        windowEnergy += (double) window[(size_t) i] * window[(size_t) i];
    }
    std::fill (work.begin() + size, work.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (work.data(), true);

    // One-sided power, scaled so a sine's bins add up to its amplitude squared.
    const double scale = 4.0 / ((double) size * windowEnergy);
    const double decay = std::exp (-(double) (size / 2) / (0.3 * rate));
    for (size_t k = 0; k < power.size(); ++k)
    {
        const double p = (double) work[k] * work[k] * scale;
        if (average)
            power[k] += p;
        else
            power[k] = frames == 0 ? p : power[k] * decay + p * (1.0 - decay);
    }
    ++frames;
}

float SpectrumAnalyser::bandLevel (float fromHz, float toHz) const
{
    if (frames == 0)
        return -200.0f;

    const double binHz = rate / size;
    const double from = juce::jmax (0.0, (double) fromHz / binHz), to = juce::jmin ((double) power.size() - 1.0, (double) toHz / binHz);
    double sum = 0.0;
    for (int k = juce::jmax (0, (int) std::floor (from)); k <= (int) std::ceil (to); ++k)
    {
        // How much of the bin [k - 0.5, k + 0.5) is inside the band.
        const double overlap = juce::jmin (to, k + 0.5) - juce::jmax (from, k - 0.5);
        if (overlap > 0.0 && k < (int) power.size())
            sum += power[(size_t) k] * juce::jmin (1.0, overlap);
    }
    if (average)
        sum /= (double) frames;
    return (float) (10.0 * std::log10 (juce::jmax (sum, 1.0e-20)));
}

float SpectrumAnalyser::levelAt (float hz) const
{
    const float sixth = std::pow (2.0f, 1.0f / 12.0f);
    return bandLevel (hz / sixth, hz * sixth);
}

} // namespace woodshed::mastering
