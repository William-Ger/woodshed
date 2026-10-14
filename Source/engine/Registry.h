#pragma once

#include "Pedal.h"

namespace woodshed
{

struct PedalType
{
    PedalInfo info;
    std::function<std::unique_ptr<Pedal>()> create;
};

/** Every pedal type Woodshed knows about, in library order. */
class Registry
{
public:
    static const Registry& get();

    const PedalType* find (const juce::String& type) const;
    const std::vector<PedalType>& all() const                   { return types; }

    /** Library sections, in display order. */
    static const juce::StringArray& categories();

    template <typename PedalClass>
    void add (PedalInfo info)
    {
        jassert (info.knobs.size() <= (size_t) Pedal::maxKnobs);
        jassert (find (info.type) == nullptr);
        types.push_back ({ std::move (info), [] { return std::unique_ptr<Pedal> (new PedalClass()); } });
    }

private:
    Registry();
    std::vector<PedalType> types;
};

// Each pedal family lives in its own file under Source/pedals and registers here.
void registerDynamicsPedals (Registry&);
void registerDrivePedals (Registry&);
void registerFilterPedals (Registry&);
void registerModulationPedals (Registry&);
void registerPitchPedals (Registry&);
void registerDelayPedals (Registry&);
void registerReverbPedals (Registry&);
void registerAmpCabPedals (Registry&);

} // namespace woodshed
