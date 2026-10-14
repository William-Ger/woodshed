#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// GRIT: tightened low end, asymmetric soft clipping at 4x oversampling, tone, level.
class Overdrive final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.oversampling.initProcessing ((size_t) maxBlock);
            c.preHighPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass (sampleRate, 140.0f);
            c.dcBlock.coefficients     = juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass (sampleRate, 12.0f);
            c.gain.reset (sampleRate, 0.03);
            c.tone.reset (sampleRate, 0.03);
            c.level.reset (sampleRate, 0.03);
        }
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.oversampling.reset();
            c.preHighPass.reset();
            c.dcBlock.reset();
            c.toneState = 0.0f;
        }
    }

    int getLatencySamples() const override      { return (int) std::ceil (channels[0].oversampling.getLatencyInSamples()); }

    void update() override
    {
        for (auto& c : channels)
        {
            c.gain.setTargetValue (dsp::dbToGain (juce::jmap (knob (0), 3.0f, 45.0f)));
            c.tone.setTargetValue (700.0f * std::pow (12.0f, knob (2)));   // ~700 Hz .. 8.4 kHz
            c.level.setTargetValue (dsp::dbToGain (knob (1)));
        }
    }

    void processChannel (int ch, float* data, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
            data[i] = c.preHighPass.processSample (data[i]) * c.gain.getNextValue();

        float* chans[] { data };
        juce::dsp::AudioBlock<float> block (chans, 1, (size_t) n);
        auto up = c.oversampling.processSamplesUp (block);
        auto* x = up.getChannelPointer (0);

        constexpr float bias = 0.18f;
        const float biasOffset = std::tanh (bias);

        for (size_t i = 0; i < up.getNumSamples(); ++i)
            x[i] = std::tanh (x[i] + bias) - biasOffset;

        c.oversampling.processSamplesDown (block);

        for (int i = 0; i < n; ++i)
        {
            const float a = dsp::onePoleCoeff (c.tone.getNextValue(), sampleRate);
            c.toneState += a * (c.dcBlock.processSample (data[i]) - c.toneState);
            data[i] = c.toneState * c.level.getNextValue() * outputTrim;
        }
    }

private:
    struct Channel
    {
        juce::dsp::Oversampling<float> oversampling { 1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
        juce::dsp::IIR::Filter<float> preHighPass, dcBlock;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gain { 1.0f }, tone { 4000.0f };
        juce::SmoothedValue<float> level { 1.0f };
        float toneState = 0.0f;
    };

    std::array<Channel, 2> channels;

    // The clipper saturates near full scale; bring it back to roughly a clean DI's level.
    static constexpr float outputTrim = 0.12f;
};

} // namespace

void registerDrivePedals (Registry& r)
{
    r.add<Overdrive> ({ "drive.overdrive", "GRIT", "overdrive", "Drive",
                        "Warm, mid-forward overdrive that stacks well into an amp",
                        juce::Colour (0xffc4582b), juce::Colour (0xff1d1310),
                        { knob ("drive", "Drive", 0.0f, 1.0f, 0.45f, "%"),
                          knob ("level", "Level", -24.0f, 12.0f, 0.0f, "dB"),
                          knob ("tone", "Tone", 0.0f, 1.0f, 0.55f, "%") } });
}

} // namespace woodshed
