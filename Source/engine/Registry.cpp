#include "Registry.h"

namespace woodshed
{

Registry::Registry()
{
    registerDynamicsPedals (*this);
    registerDrivePedals (*this);
    registerAmpCabPedals (*this);
    registerFilterPedals (*this);
    registerModulationPedals (*this);
    registerPitchPedals (*this);
    registerDelayPedals (*this);
    registerReverbPedals (*this);
}

const Registry& Registry::get()
{
    static const Registry instance;
    return instance;
}

const PedalType* Registry::find (const juce::String& type) const
{
    for (auto& t : types)
        if (t.info.type == type)
            return &t;

    return nullptr;
}

const juce::StringArray& Registry::categories()
{
    static const juce::StringArray list { "Dynamics", "Drive", "Amp", "Cab", "Filter", "Modulation", "Pitch", "Delay", "Reverb" };
    return list;
}

} // namespace woodshed
