#include "Instrument.h"

namespace woodshed
{

InstrumentRegistry::InstrumentRegistry()
{
    registerKeyboardInstruments (*this);
    registerSynthInstruments (*this);
    registerOrchestralInstruments (*this);
    registerDrumInstruments (*this);
    registerSamplerInstruments (*this);
}

const InstrumentRegistry& InstrumentRegistry::get()
{
    static const InstrumentRegistry instance;
    return instance;
}

const InstrumentType* InstrumentRegistry::find (const juce::String& type) const
{
    for (auto& t : types)
        if (t.info.type == type)
            return &t;

    return nullptr;
}

const juce::StringArray& InstrumentRegistry::categories()
{
    static const juce::StringArray list { "Keyboards", "Synthesizers", "Bass", "Drums", "Orchestral", "Mallets & Plucked", "Sampler" };
    return list;
}

std::vector<float> patchValues (const InstrumentInfo& info, const InstrumentInfo::Patch* patch)
{
    std::vector<float> values;
    values.reserve (info.knobs.size());
    for (auto& k : info.knobs)
        values.push_back (k.def);

    if (patch != nullptr)
        for (auto& [id, value] : patch->values)
            for (size_t i = 0; i < info.knobs.size(); ++i)
                if (info.knobs[i].id == id)
                {
                    const auto r = info.knobs[i].range();
                    values[i] = juce::jlimit (r.start, r.end, value);
                    break;
                }

    return values;
}

} // namespace woodshed
