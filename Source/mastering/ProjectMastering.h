#pragma once

#include "Assistant.h"
#include "../daw/Project.h"

/*
    The mastering processor's place in a project: the last insert on the Stereo Out (the
    MASTER node). Header-only, as it needs the song model and the mastering DSP needs to stay
    free of it (the rig plug-in builds the DSP too).

    The FX node keeps, besides its knobs and the matching curve (`state`):
      report  JSON: { target, targetLufs, targetPeak, before: <Measurement>, after: <Measurement> }
*/
namespace woodshed::mastering
{

inline const juce::Identifier reportId { "report" };

/** The project's mastering processor (an FX node on MASTER), or invalid. Doesn't create MASTER. */
inline juce::ValueTree findProcessor (const daw::Project& project)
{
    const auto master = project.state.getChildWithName (daw::ids::MASTER);
    for (auto fx : daw::Project::inserts (master))
        if (fx[daw::ids::type].toString() == processorType)
            return fx;
    return {};
}

/** Puts a mastering result on the project: the processor as the Stereo Out's last insert (added
    or moved there), switched on with the result's knobs, curve and report, and the master fader
    at 0 dB (the processor's ceiling is the output's only while the fader adds nothing after it).
    Through the project's undo manager: begin a transaction first. Returns the FX node. */
inline juce::ValueTree applyResult (daw::Project& project, const Result& result, const Target& target)
{
    auto* um = &project.undo;
    auto master = project.master();
    auto fx = findProcessor (project);
    std::vector<float> values (result.knobValues.begin(), result.knobValues.end());
    if (! fx.isValid())
        fx = project.addInsert (master, processorType, values);
    else if (master.indexOf (fx) != master.getNumChildren() - 1)
        master.moveChild (master.indexOf (fx), master.getNumChildren() - 1, um);

    fx.setProperty (daw::ids::knobs, daw::Project::encodeValues (values), um);
    fx.setProperty (daw::ids::state, encodeCurve (result.curve), um);
    fx.setProperty (daw::ids::on, true, um);

    auto* report = new juce::DynamicObject();
    report->setProperty ("target", target.name);
    report->setProperty ("targetLufs", target.lufs);
    report->setProperty ("targetPeak", target.truePeak);
    report->setProperty ("before", toVar (result.before));
    report->setProperty ("after", toVar (result.after));
    fx.setProperty (reportId, juce::JSON::toString (juce::var (report), true), um);

    project.state.setProperty (daw::ids::masterVolume, 0.0, um);
    return fx;
}

} // namespace woodshed::mastering
