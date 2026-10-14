#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// ECHO: analog-flavoured delay; repeats darken as they feed back.
class AnalogDelay final : public Pedal
{
public:
    void prepared() override
    {
        for (auto& c : channels)
        {
            c.line.setMaxSamples ((int) (sampleRate * 2.1));
            c.time.reset (sampleRate, 0.12);
            c.feedback.reset (sampleRate, 0.03);
            c.mix.reset (sampleRate, 0.03);
        }

        damping = dsp::onePoleCoeff (3800.0f, sampleRate);
    }

    void reset() override
    {
        for (auto& c : channels)
        {
            c.line.clear();
            c.lowPass = 0.0f;
        }
    }

    void update() override
    {
        for (auto& c : channels)
        {
            c.time.setTargetValue (juce::jmax (2.0f, knob (0) * 0.001f * (float) sampleRate));
            c.feedback.setTargetValue (knob (1));
            c.mix.setTargetValue (knob (2));
        }
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& c = channels[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            const float wet = c.line.read (c.time.getNextValue());
            c.lowPass += damping * (wet - c.lowPass);
            c.line.push (dry + c.feedback.getNextValue() * c.lowPass);
            x[i] = dry + c.mix.getNextValue() * wet;
        }
    }

private:
    struct Channel
    {
        dsp::DelayBuffer line;
        juce::SmoothedValue<float> time, feedback, mix;
        float lowPass = 0.0f;
    };

    std::array<Channel, 2> channels;
    float damping = 0.3f;
};

} // namespace

void registerDelayPedals (Registry& r)
{
    r.add<AnalogDelay> ({ "delay.analog", "ECHO", "analog delay", "Delay",
                          "Warm repeats that get darker as they fade",
                          juce::Colour (0xff27506b), juce::Colour (0xfff1e9d8),
                          { knob ("time", "Time", 40.0f, 1200.0f, 380.0f, "ms", 0.6f),
                            knob ("repeats", "Repeats", 0.0f, 0.92f, 0.35f, "%"),
                            knob ("mix", "Mix", 0.0f, 1.0f, 0.3f, "%") } });
}

} // namespace woodshed
