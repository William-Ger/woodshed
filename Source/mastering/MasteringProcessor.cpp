#include "MasteringProcessor.h"
#include "../engine/Registry.h"

namespace woodshed::mastering
{

namespace
{
inline double msCoeff (double ms, double sr)        { return 1.0 - std::exp (-1.0 / (ms * 0.001 * sr)); }

/** What each Character does to the compressor and the limiter. */
struct Voicing { float ratio, attackMs, releaseMs, limiterReleaseMs; };
Voicing voicing (Character c)
{
    switch (c)
    {
        case Character::valve:       return { 2.5f, 20.0f, 250.0f, 120.0f };
        case Character::punch:       return { 3.5f, 30.0f, 90.0f, 50.0f };
        case Character::transparent: return { 1.6f, 5.0f, 300.0f, 200.0f };
        case Character::clean:
        default:                     return { 2.0f, 10.0f, 150.0f, 80.0f };
    }
}

/** RBJ cookbook biquads for the matching EQ: band 0 a low shelf, the last a high shelf, the rest bells. */
Biquad eqBand (int band, float gainDb, double sr)
{
    Biquad f;
    const double a = std::pow (10.0, gainDb / 40.0);
    const auto& edges = bandEdges();
    const bool lowShelf = band == 0, highShelf = band == numBands - 1;
    const double hz = lowShelf ? edges[1] : highShelf ? edges[(size_t) numBands - 1] : bandCentre (band);
    const double w0 = juce::MathConstants<double>::twoPi * juce::jmin (hz, sr * 0.45) / sr;
    const double cw = std::cos (w0), sw = std::sin (w0);
    double b0, b1, b2, a0, a1, a2;

    if (lowShelf || highShelf)
    {
        const double alpha = sw / 2.0 * std::sqrt (2.0);      // shelf slope 1
        const double s = 2.0 * std::sqrt (a) * alpha;
        if (lowShelf)
        {
            b0 = a * ((a + 1) - (a - 1) * cw + s);
            b1 = 2 * a * ((a - 1) - (a + 1) * cw);
            b2 = a * ((a + 1) - (a - 1) * cw - s);
            a0 = (a + 1) + (a - 1) * cw + s;
            a1 = -2 * ((a - 1) + (a + 1) * cw);
            a2 = (a + 1) + (a - 1) * cw - s;
        }
        else
        {
            b0 = a * ((a + 1) + (a - 1) * cw + s);
            b1 = -2 * a * ((a - 1) + (a + 1) * cw);
            b2 = a * ((a + 1) + (a - 1) * cw - s);
            a0 = (a + 1) - (a - 1) * cw + s;
            a1 = 2 * ((a - 1) - (a + 1) * cw);
            a2 = (a + 1) - (a - 1) * cw - s;
        }
    }
    else
    {
        const double q = 0.9;           // about 1.5 octaves: broad, overlapping bells
        const double alpha = sw / (2.0 * q);
        b0 = 1 + alpha * a;
        b1 = -2 * cw;
        b2 = 1 - alpha * a;
        a0 = 1 + alpha / a;
        a1 = -2 * cw;
        a2 = 1 - alpha / a;
    }

    f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0;
    f.a1 = a1 / a0; f.a2 = a2 / a0;
    return f;
}

double magnitude (const Biquad& f, double w)
{
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
    return std::abs ((f.b0 + f.b1 * z1 + f.b2 * z2) / (1.0 + f.a1 * z1 + f.a2 * z2));
}

/** Second-order Butterworth high-pass (two in a row make Mono Bass's Linkwitz-Riley). */
Biquad highPass (double hz, double sr)
{
    Biquad f;
    const double w0 = juce::MathConstants<double>::twoPi * hz / sr;
    const double alpha = std::sin (w0) / juce::MathConstants<double>::sqrt2;     // Q = 1/sqrt(2)
    const double cw = std::cos (w0), a0 = 1.0 + alpha;
    f.b0 = (1.0 + cw) / 2.0 / a0; f.b1 = -(1.0 + cw) / a0; f.b2 = f.b0;
    f.a1 = -2.0 * cw / a0; f.a2 = (1.0 - alpha) / a0;
    return f;
}

int nextPowerOfTwo (int n)
{
    int size = 1;
    while (size < n) size <<= 1;
    return size;
}
}

//==============================================================================
const juce::StringArray& characterNames()
{
    static const juce::StringArray names { "Clean", "Valve", "Punch", "Transparent" };
    return names;
}

const std::array<float, numBands + 1>& bandEdges()
{
    static const std::array<float, numBands + 1> edges { 20.0f, 90.0f, 200.0f, 450.0f, 1000.0f, 2200.0f, 5000.0f, 10000.0f, 20000.0f };
    return edges;
}

float bandCentre (int band)
{
    const auto& e = bandEdges();
    return std::sqrt (e[(size_t) band] * e[(size_t) band + 1]);
}

juce::String encodeCurve (const Curve& c)
{
    juce::StringArray parts;
    for (auto g : c)
        parts.add (juce::String (g, 2));
    return parts.joinIntoString (",");
}

Curve decodeCurve (const juce::String& s)
{
    Curve c {};
    auto parts = juce::StringArray::fromTokens (s, ",", {});
    for (int i = 0; i < numBands && i < parts.size(); ++i)
        c[(size_t) i] = juce::jlimit (-12.0f, 12.0f, parts[i].getFloatValue());
    return c;
}

float eqResponseDb (const Curve& gains, float hz, double sampleRate)
{
    const double w = juce::MathConstants<double>::twoPi * juce::jmin ((double) hz, sampleRate * 0.499) / sampleRate;
    double mag = 1.0;
    for (int b = 0; b < numBands; ++b)
        if (std::abs (gains[(size_t) b]) > 0.001f)
            mag *= magnitude (eqBand (b, gains[(size_t) b], sampleRate), w);
    return (float) (20.0 * std::log10 (mag));
}

//==============================================================================
MasteringProcessor::MasteringProcessor()
{
    for (auto& c : curve)
        c.store (0.0f);
}

void MasteringProcessor::setState (const juce::String& state)
{
    const auto c = decodeCurve (state);
    for (int b = 0; b < numBands; ++b)
        curve[(size_t) b].store (c[(size_t) b], std::memory_order_relaxed);
}

void MasteringProcessor::prepared()
{
    // About 1.3 ms of lookahead: long enough for a click-free gain curve, short enough to stay
    // out of the way. The signal waits for the window, the slack either side of a peak and the
    // detector's own delay.
    window = juce::jmax (16, juce::roundToInt (0.0013 * sampleRate));
    slack = 8;
    latency = window + slack + Detector::latency - 1;

    const int delaySize = nextPowerOfTwo (latency + 2);
    delayL.assign ((size_t) delaySize, 0.0f);
    delayR.assign ((size_t) delaySize, 0.0f);
    delayMask = delaySize - 1;

    const int dequeSize = nextPowerOfTwo (window + 2 * slack + 2);
    minValues.assign ((size_t) dequeSize, 1.0f);
    minTimes.assign ((size_t) dequeSize, 0);
    minMask = dequeSize - 1;
    boxValues.assign ((size_t) window, 1.0);

    monoHz = 0.0f;          // recompute the Mono Bass filter for this rate
    compRefCoeff = msCoeff (3000.0, sampleRate);
    dcCoeff = 1.0 - juce::MathConstants<double>::twoPi * 8.0 / sampleRate;

    inputGain.reset (sampleRate, 0.05);
    widthAmount.reset (sampleRate, 0.05);
    ceilingGain.reset (sampleRate, 0.05);
}

void MasteringProcessor::reset()
{
    std::fill (delayL.begin(), delayL.end(), 0.0f);
    std::fill (delayR.begin(), delayR.end(), 0.0f);
    writePos = 0;
    minHead = minTail = 0;
    std::fill (boxValues.begin(), boxValues.end(), 1.0);
    boxPos = 0;
    boxSum = (double) window;
    released = 1.0;
    sampleCount = 0;
    for (auto& d : detectors) d.reset();

    for (auto& ch : eqFilters)
        for (auto& f : ch)
            f.reset();
    for (auto& f : sideHighPass) f.reset();
    compEnv = compRef = 0.0;
    compGainDb = 0.0f;
    dcIn.fill (0.0);
    dcOut.fill (0.0);

    // Jump straight to the knobs' settings, without a fade from defaults.
    update();
    inputGain.setCurrentAndTargetValue (inputGain.getTargetValue());
    widthAmount.setCurrentAndTargetValue (widthAmount.getTargetValue());
    ceilingGain.setCurrentAndTargetValue (ceilingGain.getTargetValue());
    updateEq (true);
}

void MasteringProcessor::update()
{
    character = (Character) juce::jlimit (0, 3, juce::roundToInt (knob (knobs::character)));
    const auto v = voicing (character);
    compRatio = v.ratio;
    compAttack = msCoeff (v.attackMs, sampleRate);
    compRelease = msCoeff (v.releaseMs, sampleRate);
    releaseCoeff = msCoeff (v.limiterReleaseMs, sampleRate);
    compAmount = juce::jlimit (0.0f, 1.0f, knob (knobs::compress));

    inputGain.setTargetValue (dsp::dbToGain (juce::jlimit (-24.0f, 30.0f, knob (knobs::gain))));
    widthAmount.setTargetValue (juce::jlimit (0.0f, 2.0f, knob (knobs::width)));
    // A little under the ceiling: the detector can still miss a peak between its points by up
    // to about 0.15 dB near Nyquist, and the gain curve moves a little between them.
    ceilingGain.setTargetValue (dsp::dbToGain (juce::jlimit (-12.0f, 0.0f, knob (knobs::ceiling)) - 0.12f));

    const float mono = juce::jlimit (20.0f, 500.0f, knob (knobs::monoBass));
    if (std::abs (mono - monoHz) > 0.5f)
    {
        monoHz = mono;
        const auto f = highPass (monoHz, sampleRate);
        for (auto& s : sideHighPass)
        {
            s.b0 = f.b0; s.b1 = f.b1; s.b2 = f.b2; s.a1 = f.a1; s.a2 = f.a2;
        }
    }

    updateEq (false);
}

void MasteringProcessor::updateEq (bool immediately)
{
    const float amount = juce::jlimit (0.0f, 1.0f, knob (knobs::eq));
    eqFlat = true;
    for (int b = 0; b < numBands; ++b)
    {
        const float target = curve[(size_t) b].load (std::memory_order_relaxed) * amount;
        float& g = appliedGains[(size_t) b];
        const float before = g;
        g = immediately || std::abs (target - g) < 0.01f ? target : g + 0.2f * (target - g);
        if (immediately || std::abs (g - before) > 0.001f)
        {
            const auto f = eqBand (b, g, sampleRate);
            for (auto& ch : eqFilters)
            {
                auto& e = ch[(size_t) b];
                e.b0 = f.b0; e.b1 = f.b1; e.b2 = f.b2; e.a1 = f.a1; e.a2 = f.a2;
            }
        }
        eqFlat = eqFlat && std::abs (g) < 0.005f;
    }
}

void MasteringProcessor::processChannel (int, float*, int)
{
    // Stereo only (producesStereo): everything is linked across the two channels.
}

void MasteringProcessor::processStereo (float* left, float* right, int n)
{
    const double att = compAttack, rel = compRelease;
    const float slope = 1.0f - 1.0f / compRatio;
    const float offset = 14.0f - 10.0f * compAmount;       // threshold above the running level
    const float maxReduction = 2.0f + 10.0f * compAmount;
    const bool compressing = compAmount > 0.001f;
    const int span = window + 2 * slack;
    float deepestLimit = 1.0f, deepestComp = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        // 1. Gain and EQ.
        const float g = inputGain.getNextValue();
        double l = (double) (left[i] * g), r = (double) (right[i] * g);
        if (! eqFlat)
            for (int b = 0; b < numBands; ++b)
            {
                l = eqFilters[0][(size_t) b].process (l);
                r = eqFilters[1][(size_t) b].process (r);
            }

        // 2. Bus compressor, linked, its threshold riding the running level.
        if (compressing)
        {
            const double power = juce::jmax (l * l, r * r);
            compEnv += (power > compEnv ? att : rel) * (power - compEnv);
            compRef += compRefCoeff * (0.5 * (l * l + r * r) - compRef);
            const float levelDb = (float) (10.0 * std::log10 (compEnv + 1.0e-12));
            const float threshold = juce::jmax (-60.0f, (float) (10.0 * std::log10 (compRef + 1.0e-12))) + offset;
            const float over = levelDb - threshold;
            float reduction = over <= -3.0f ? 0.0f
                            : over < 3.0f   ? slope * (over + 3.0f) * (over + 3.0f) / 12.0f
                                            : slope * over;
            reduction = juce::jmin (reduction, maxReduction);
            compGainDb = reduction;
            const double cg = std::exp (-(double) reduction * 0.11512925464970229);
            l *= cg;
            r *= cg;
            deepestComp = juce::jmax (deepestComp, reduction);
        }

        // 3. Character: Valve warms with gentle, slightly asymmetric saturation.
        if (character == Character::valve)
        {
            constexpr double k = 0.6, bias = 0.25;
            const double t0 = std::tanh (k * bias), norm = 1.0 / (k * (1.0 - t0 * t0));
            auto sat = [&] (double x) { return (std::tanh (k * (x + bias)) - t0) * norm; };
            const double sl = sat (l), sr = sat (r);
            // DC blocker: the asymmetry adds a little offset.
            const double yl = sl - dcIn[0] + dcCoeff * dcOut[0];
            const double yr = sr - dcIn[1] + dcCoeff * dcOut[1];
            dcIn[0] = sl; dcIn[1] = sr; dcOut[0] = yl; dcOut[1] = yr;
            l = yl;
            r = yr;
        }

        // 4. Width, with the side's lows removed below Mono Bass.
        {
            const double mid = 0.5 * (l + r);
            double side = 0.5 * (l - r);
            if (monoHz > 20.5f)
                side = sideHighPass[1].process (sideHighPass[0].process (side));
            side *= (double) widthAmount.getNextValue();
            l = mid + side;
            r = mid - side;
        }

        // 5. True-peak limiter. The gain each detected point needs, held as a sliding minimum
        // over the lookahead, released, then averaged over the window: by the time a peak
        // comes out of the delay the gain has reached (at least) what it needs.
        const float fl = (float) l, fr = (float) r;
        const float peakNow = juce::jmax (detectors[0].process (fl), detectors[1].process (fr));
        const float ceil = ceilingGain.getNextValue();
        const float need = peakNow > ceil ? ceil / peakNow : 1.0f;

        while (minTail != minHead && minValues[(size_t) ((minTail - 1) & minMask)] >= need)
            --minTail;
        minValues[(size_t) (minTail & minMask)] = need;
        minTimes[(size_t) (minTail & minMask)] = sampleCount;
        ++minTail;
        while (minTimes[(size_t) (minHead & minMask)] <= sampleCount - span)
            ++minHead;
        if (minHead > (1 << 28))
        {
            minHead -= (1 << 28);
            minTail -= (1 << 28);
        }
        const double held = (double) minValues[(size_t) (minHead & minMask)];

        released = held < released ? held : released + releaseCoeff * (held - released);

        boxSum += released - boxValues[(size_t) boxPos];
        boxValues[(size_t) boxPos] = released;
        if (++boxPos == window)
        {
            boxPos = 0;
            boxSum = 0.0;           // re-add exactly now and then, so rounding never drifts
            for (auto v : boxValues)
                boxSum += v;
        }
        const float gain = (float) juce::jmin (1.0, boxSum / window);

        delayL[(size_t) writePos] = fl;
        delayR[(size_t) writePos] = fr;
        const int readPos = (writePos - latency) & delayMask;
        left[i] = delayL[(size_t) readPos] * gain;
        right[i] = delayR[(size_t) readPos] * gain;
        writePos = (writePos + 1) & delayMask;
        ++sampleCount;
        deepestLimit = juce::jmin (deepestLimit, gain);
    }

    limiterReduction.store (-juce::Decibels::gainToDecibels (deepestLimit, -60.0f), std::memory_order_relaxed);
    compressorReduction.store (deepestComp, std::memory_order_relaxed);
}

} // namespace woodshed::mastering

namespace woodshed
{
void registerMasteringPedals (Registry& r)
{
    using namespace mastering;
    r.add<MasteringProcessor> ({ processorType, "SUMMIT", "mastering", "Dynamics",
                                 "Finishes a mix: matching EQ, bus compression, width and a true-peak limiter",
                                 juce::Colour (0xff23262b), juce::Colour (0xffe9c46a),
                                 { choice ("character", "Character", characterNames(), 0),
                                   knob ("gain", "Gain", -12.0f, 24.0f, 0.0f, "dB"),
                                   knob ("eq", "EQ", 0.0f, 1.0f, 1.0f, "%"),
                                   knob ("compress", "Compress", 0.0f, 1.0f, 0.3f, "%"),
                                   knob ("width", "Width", 0.0f, 2.0f, 1.0f, "%"),
                                   knob ("mono", "Mono Bass", 20.0f, 300.0f, 100.0f, "Hz", 0.5f),
                                   knob ("ceiling", "Ceiling", -6.0f, 0.0f, -1.0f, "dB") } });
}
} // namespace woodshed
