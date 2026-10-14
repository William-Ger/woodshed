#include "Chain.h"

namespace woodshed
{

Chain::Entry Chain::makeEntry (std::shared_ptr<Pedal> pedal, const PedalInfo& info, const std::atomic<float>* on,
                               const std::atomic<float>* const* knobSources)
{
    Entry e;
    e.pedal = std::move (pedal);
    e.info = &info;
    e.on = on;
    e.numKnobs = (int) info.knobs.size();

    for (int k = 0; k < e.numKnobs; ++k)
    {
        e.knobs[(size_t) k] = knobSources[k];
        e.ranges[(size_t) k] = info.knobs[(size_t) k].range();
    }

    return e;
}

void Chain::process (float* left, float* right, float* scratchLeft, float* scratchRight, int n) const
{
    using FVO = juce::FloatVectorOperations;
    bool mono = true;

    for (auto& e : entries)
    {
        auto& p = *e.pedal;
        p.engaged.setTargetValue (e.on->load (std::memory_order_relaxed) > 0.5f ? 1.0f : 0.0f);

        const bool fading = p.engaged.isSmoothing();
        if (! fading && p.engaged.getCurrentValue() < 0.5f)
            continue;

        for (int k = 0; k < e.numKnobs; ++k)
        {
            const auto& r = e.ranges[(size_t) k];
            p.setKnob (k, r.snapToLegalValue (r.convertFrom0to1 (e.knobs[(size_t) k]->load (std::memory_order_relaxed))));
        }

        p.update();

        const bool stereoPedal = p.producesStereo();
        if (mono && stereoPedal)
        {
            FVO::copy (right, left, n);
            mono = false;
        }

        float* outL = left;
        float* outR = right;

        if (fading)
        {
            FVO::copy (scratchLeft, left, n);
            if (! mono) FVO::copy (scratchRight, right, n);
            outL = scratchLeft;
            outR = scratchRight;
        }

        if (stereoPedal)
        {
            p.processStereo (outL, outR, n);
        }
        else
        {
            p.processChannel (0, outL, n);
            if (! mono) p.processChannel (1, outR, n);
        }

        if (fading)
        {
            for (int i = 0; i < n; ++i)
            {
                const float a = p.engaged.getNextValue();
                left[i] += a * (scratchLeft[i] - left[i]);
                if (! mono) right[i] += a * (scratchRight[i] - right[i]);
            }
        }
    }

    if (mono)
        FVO::copy (right, left, n);
}

} // namespace woodshed
