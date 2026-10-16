#include "InstrumentDsp.h"

namespace woodshed
{

void registerPluckedInstruments (InstrumentRegistry&);     // Plucked.cpp

void registerOrchestralInstruments (InstrumentRegistry& r)
{
    registerPluckedInstruments (r);
}

} // namespace woodshed
