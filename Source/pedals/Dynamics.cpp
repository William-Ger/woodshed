#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// HUSH: noise gate with hysteresis and a short hold, so sustained notes don't chatter.
class NoiseGate final : public Pedal
{
public:
    void reset() override                       { for (auto& s : state) s = {}; }

    void prepared() override
    {
        envRelease = std::exp (-1.0f / (0.010f * (float) sampleRate));
        openCoeff = 1.0f - std::exp (-1.0f / (0.0008f * (float) sampleRate));
        holdSamples = (int) (0.015 * sampleRate);
    }

    void update() override
    {
        openThreshold = dsp::dbToGain (knob (0));
        closeThreshold = openThreshold * 0.5f;    // 6 dB of hysteresis
        closeCoeff = 1.0f - std::exp (-1.0f / (knob (1) * 0.001f * (float) sampleRate));
    }

    void processChannel (int ch, float* x, int n) override
    {
        auto& s = state[(size_t) ch];

        for (int i = 0; i < n; ++i)
        {
            s.env = juce::jmax (std::abs (x[i]), s.env * envRelease);

            if (s.env > openThreshold)       { s.open = true; s.hold = holdSamples; }
            else if (s.env < closeThreshold && --s.hold <= 0) s.open = false;

            const float target = s.open ? 1.0f : 0.0f;
            s.gain += (s.open ? openCoeff : closeCoeff) * (target - s.gain);
            x[i] *= s.gain;
        }
    }

private:
    struct State { float env = 0.0f, gain = 0.0f; bool open = false; int hold = 0; };
    std::array<State, 2> state;
    float envRelease = 0.0f, openCoeff = 0.0f, closeCoeff = 0.0f;
    float openThreshold = 0.0f, closeThreshold = 0.0f;
    int holdSamples = 0;
};

} // namespace

void registerDynamicsPedals (Registry& r)
{
    r.add<NoiseGate> ({ "dyn.gate", "HUSH", "noise gate", "Dynamics",
                        "Silences hum and hiss between notes",
                        juce::Colour (0xff3e4c55), juce::Colour (0xffe8eee9),
                        { knob ("threshold", "Threshold", -90.0f, -20.0f, -62.0f, "dB"),
                          knob ("release", "Release", 10.0f, 500.0f, 90.0f, "ms", 0.5f) } });
}

} // namespace woodshed
