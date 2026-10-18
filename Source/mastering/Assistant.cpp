#include "Assistant.h"

namespace woodshed::mastering
{

namespace
{
constexpr int renderBlock = 512;

/** Runs the processor over `mix` block by block, handing each output block (latency removed)
    to `sink`. Returns false if cancelled. */
bool runProcessor (const juce::AudioBuffer<float>& mix, double sampleRate, const std::array<float, knobs::numKnobs>& knobValues,
                   const Curve& curve, const std::function<void (const float*, const float*, int)>& sink,
                   const std::function<bool()>& keepGoing = {})
{
    MasteringProcessor p;
    for (int k = 0; k < knobs::numKnobs; ++k)
        p.setKnob (k, knobValues[(size_t) k]);
    p.setState (encodeCurve (curve));
    p.prepare (sampleRate, renderBlock);

    const int length = mix.getNumSamples(), latency = p.getLatencySamples();
    const int channels = mix.getNumChannels();
    std::array<float, renderBlock> l {}, r {};
    int skip = latency;

    juce::ScopedNoDenormals noDenormals;
    for (int pos = 0; pos < length + latency; pos += renderBlock)
    {
        const int n = juce::jmin (renderBlock, length + latency - pos);
        for (int i = 0; i < n; ++i)
        {
            const int at = pos + i;
            l[(size_t) i] = at < length ? mix.getSample (0, at) : 0.0f;
            r[(size_t) i] = at < length ? mix.getSample (juce::jmin (1, channels - 1), at) : 0.0f;
        }
        p.update();
        p.processStereo (l.data(), r.data(), n);

        const int drop = juce::jmin (skip, n);
        skip -= drop;
        if (n > drop)
            sink (l.data() + drop, r.data() + drop, n - drop);

        if (keepGoing && (pos / renderBlock) % 256 == 0 && ! keepGoing())
            return false;
    }
    return true;
}

/** Integrated loudness only (no true peak), for the gain search. */
float loudnessOf (const juce::AudioBuffer<float>& mix, double sampleRate, const std::array<float, knobs::numKnobs>& knobValues,
                  const Curve& curve, const std::function<bool()>& keepGoing, bool& cancelled)
{
    LoudnessMeter meter;
    meter.prepare (sampleRate);
    meter.setMeasureTruePeak (false);
    cancelled = ! runProcessor (mix, sampleRate, knobValues, curve,
                                [&meter] (const float* l, const float* r, int n) { meter.process (l, r, n); }, keepGoing);
    return meter.integrated();
}
}

//==============================================================================
const std::vector<Target>& targetPresets()
{
    static const std::vector<Target> list {
        { "Streaming (-14 LUFS)", -14.0f, -1.0f },
        { "Podcast (-16 LUFS)",   -16.0f, -1.0f },
        { "CD (-11 LUFS)",        -11.0f, -0.3f },
        { "Club / Loud (-9 LUFS)", -9.0f, -0.3f },
    };
    return list;
}

const Curve& referenceBalance()
{
    //                          20-90  -200  -450  -1k  -2.2k  -5k   -10k  -20k Hz
    static const Curve balance { 2.0f, 3.0f, 1.5f, 0.0f, -1.5f, -3.5f, -6.5f, -11.0f };
    return balance;
}

const std::vector<float>& spectrumFrequencies()
{
    static const std::vector<float> list = []
    {
        std::vector<float> f;
        for (int k = 0; k < 60; ++k)
            f.push_back (20.0f * std::pow (2.0f, (float) k / 6.0f));
        return f;
    }();
    return list;
}

namespace
{
/** Removes the mean of the middle six bands. */
Curve relative (Curve c)
{
    float mean = 0.0f;
    for (int b = 1; b < numBands - 1; ++b)
        mean += c[(size_t) b];
    mean /= (float) (numBands - 2);
    for (auto& v : c)
        v -= mean;
    return c;
}

Measurement measureWith (LoudnessMeter& meter, SpectrumAnalyser& spectrum)
{
    Measurement m;
    m.integrated = meter.integrated();
    m.range = meter.loudnessRange();
    m.truePeak = meter.truePeakDb();
    m.samplePeak = meter.samplePeakDb();
    m.maxShortTerm = meter.maxShortTerm();
    m.seconds = meter.seconds();

    const auto& edges = bandEdges();
    for (int b = 0; b < numBands; ++b)
        m.bands[(size_t) b] = spectrum.bandLevel (edges[(size_t) b], edges[(size_t) b + 1]);
    m.bands = relative (m.bands);
    for (auto hz : spectrumFrequencies())
        m.spectrum.push_back (spectrum.levelAt (hz));
    return m;
}
}

Measurement measure (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    LoudnessMeter meter;
    meter.prepare (sampleRate);
    SpectrumAnalyser spectrum (true);
    spectrum.prepare (sampleRate);

    const int n = audio.getNumSamples();
    const float* l = audio.getReadPointer (0);
    const float* r = audio.getReadPointer (juce::jmin (1, audio.getNumChannels() - 1));
    for (int pos = 0; pos < n; pos += 4096)
    {
        const int count = juce::jmin (4096, n - pos);
        meter.process (l + pos, r + pos, count);
        spectrum.process (l + pos, r + pos, count);
    }
    return measureWith (meter, spectrum);
}

juce::var toVar (const Measurement& m)
{
    auto* o = new juce::DynamicObject();
    auto finite = [] (float v) { return std::isfinite (v) ? (double) v : -200.0; };
    o->setProperty ("integrated", finite (m.integrated));
    o->setProperty ("range", finite (m.range));
    o->setProperty ("truePeak", finite (m.truePeak));
    o->setProperty ("samplePeak", finite (m.samplePeak));
    o->setProperty ("maxShortTerm", finite (m.maxShortTerm));
    o->setProperty ("seconds", m.seconds);
    juce::Array<juce::var> bands, spectrum;
    for (auto b : m.bands) bands.add (juce::roundToInt (b * 10.0f) / 10.0);
    for (auto v : m.spectrum) spectrum.add (juce::roundToInt (juce::jmax (-200.0f, v) * 10.0f) / 10.0);
    o->setProperty ("bands", bands);
    o->setProperty ("spectrum", spectrum);
    return juce::var (o);
}

Measurement measurementFrom (const juce::var& v)
{
    Measurement m;
    auto get = [&v] (const char* name, float fallback)
    {
        const auto& p = v[name];
        if (p.isVoid()) return fallback;
        const auto f = (float) (double) p;
        return f <= -199.0f ? silentLufs : f;
    };
    m.integrated = get ("integrated", silentLufs);
    m.range = get ("range", 0.0f);
    m.truePeak = get ("truePeak", -200.0f);
    m.samplePeak = get ("samplePeak", -200.0f);
    m.maxShortTerm = get ("maxShortTerm", silentLufs);
    m.seconds = (double) v.getProperty ("seconds", 0.0);
    if (auto* bands = v["bands"].getArray())
        for (int b = 0; b < numBands && b < bands->size(); ++b)
            m.bands[(size_t) b] = (float) (double) (*bands)[b];
    if (auto* spectrum = v["spectrum"].getArray())
        for (auto& x : *spectrum)
            m.spectrum.push_back ((float) (double) x);
    return m;
}

//==============================================================================
Curve matchingCurve (const Measurement& mix, double sampleRate)
{
    const auto& reference = referenceBalance();

    // Half of the difference, with less boost than cut (boosting a band the mix barely has
    // only raises its noise), smoothed across neighbours so the curve stays broad.
    Curve wanted {};
    for (int b = 0; b < numBands; ++b)
    {
        float delta = reference[(size_t) b] - mix.bands[(size_t) b];
        if (mix.bands[(size_t) b] < -30.0f)
            delta = juce::jmin (delta, 0.0f);
        wanted[(size_t) b] = juce::jlimit (-5.0f, 3.5f, 0.5f * delta);
    }
    Curve smooth {};
    for (int b = 0; b < numBands; ++b)
    {
        const float before = wanted[(size_t) juce::jmax (0, b - 1)], after = wanted[(size_t) juce::jmin (numBands - 1, b + 1)];
        smooth[(size_t) b] = 0.25f * before + 0.5f * wanted[(size_t) b] + 0.25f * after;
    }

    // The bands overlap, so solve for gains whose combined response hits the wanted values at
    // the band centres.
    Curve gains = smooth;
    for (int iteration = 0; iteration < 12; ++iteration)
        for (int b = 0; b < numBands; ++b)
        {
            const float got = eqResponseDb (gains, bandCentre (b), sampleRate);
            gains[(size_t) b] = juce::jlimit (-8.0f, 8.0f, gains[(size_t) b] + 0.6f * (smooth[(size_t) b] - got));
        }
    return gains;
}

std::array<float, knobs::numKnobs> knobValuesFor (const Settings& s, float gainDb)
{
    std::array<float, knobs::numKnobs> k {};
    k[knobs::character] = (float) (int) s.character;
    k[knobs::gain] = gainDb;
    k[knobs::eq] = s.eqAmount;
    k[knobs::compress] = s.compress;
    k[knobs::width] = s.width;
    k[knobs::monoBass] = s.monoHz;
    k[knobs::ceiling] = s.target.truePeak;
    return k;
}

juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& mix, double sampleRate,
                                 const std::array<float, knobs::numKnobs>& knobValues, const Curve& curve)
{
    juce::AudioBuffer<float> out (2, mix.getNumSamples());
    int written = 0;
    runProcessor (mix, sampleRate, knobValues, curve, [&] (const float* l, const float* r, int n)
    {
        const int count = juce::jmin (n, out.getNumSamples() - written);
        if (count <= 0)
            return;
        out.copyFrom (0, written, l, count);
        out.copyFrom (1, written, r, count);
        written += count;
    });
    return out;
}

Result master (const juce::AudioBuffer<float>& mix, double sampleRate, const Settings& settings,
               const Measurement* before, std::function<bool (float)> progress)
{
    Result result;
    float done = 0.0f;
    auto report = [&] (float to) { done = to; return ! progress || progress (to); };
    auto keepGoing = [&] { return ! progress || progress (done); };

    result.before = before != nullptr ? *before : measure (mix, sampleRate);
    if (! report (0.15f))
        return result;
    if (! std::isfinite (result.before.integrated) || result.before.integrated < -70.0f)
    {
        result.message = "The mix is silent";
        return result;
    }

    result.curve = matchingCurve (result.before, sampleRate);
    const float target = settings.target.lufs;
    constexpr float minGain = -12.0f, maxGain = 24.0f;

    // Secant search on gain -> integrated loudness (monotonic, slope at most about 1).
    bool cancelled = false;
    auto loudnessAt = [&] (float gain) { return loudnessOf (mix, sampleRate, knobValuesFor (settings, gain), result.curve, keepGoing, cancelled); };
    float g0 = juce::jlimit (minGain, maxGain, target - result.before.integrated);
    float f0 = loudnessAt (g0);
    float bestGain = g0, bestError = std::abs (target - f0);
    float g1 = juce::jlimit (minGain, maxGain, g0 + (target - f0));

    for (int step = 0; step < 6 && bestError > 0.05f && ! cancelled; ++step)
    {
        if (! report (0.15f + 0.12f * (float) (step + 1)))
            return result;
        if (std::abs (g1 - g0) < 0.001f)
            break;
        const float f1 = loudnessAt (g1);
        if (std::abs (target - f1) < bestError)
        {
            bestError = std::abs (target - f1);
            bestGain = g1;
        }
        const float slope = juce::jlimit (0.05f, 1.5f, (f1 - f0) / (g1 - g0));
        g0 = g1;
        f0 = f1;
        g1 = juce::jlimit (minGain, maxGain, g1 + (target - f1) / slope);
    }
    if (cancelled || ! report (0.9f))
        return result;

    result.knobValues = knobValuesFor (settings, bestGain);

    // The result, measured in full.
    {
        LoudnessMeter meter;
        meter.prepare (sampleRate);
        SpectrumAnalyser spectrum (true);
        spectrum.prepare (sampleRate);
        runProcessor (mix, sampleRate, result.knobValues, result.curve, [&] (const float* l, const float* r, int n)
        {
            meter.process (l, r, n);
            spectrum.process (l, r, n);
        });
        result.after = measureWith (meter, spectrum);
    }

    result.ok = true;
    if (std::abs (result.after.integrated - target) > 0.5f)
        result.message = result.after.integrated < target ? "The target is out of reach: the mix needs more gain than the processor gives"
                                                          : "The mix is louder than the target even with the gain all the way down";
    report (1.0f);
    return result;
}

} // namespace woodshed::mastering
