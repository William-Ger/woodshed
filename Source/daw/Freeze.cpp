// Track freeze: a track's instrument, rig and inserts rendered offline, pre-fader, to a file the
// engine then plays instead (Engine's freeze members; the render is the bounce's).

#include "Engine.h"

namespace woodshed::daw
{

namespace
{
constexpr double freezeTailSeconds = 4.0;      // reverbs and delays ring out past the last region

BounceOptions freezeOptions()
{
    BounceOptions o;
    o.format = "wav";
    o.bits = 32;                               // float: the render plays back exactly as it was
    return o;
}
}

juce::File Engine::freezeFileFor (const juce::ValueTree& track) const
{
    if (project == nullptr)
        return {};
    return project->folder.getChildFile ("Freeze")
                          .getChildFile (juce::File::createLegalFileName (track[ids::name].toString()) + " "
                                         + track[ids::id].toString() + ".wav");
}

std::unique_ptr<Snapshot> Engine::makeFreezeSnapshot (int trackId)
{
    // The bounce's private devices, with only this track playing, as it sounds before its fader:
    // unity gain, centred, no volume or pan automation, no sends, straight to the output.
    auto snap = makeBounceSnapshot (trackId, false);
    for (auto& t : snap->tracks)
    {
        if (t.runtime->id != trackId)
        {
            t.audible = false;
            continue;
        }
        t.gain = 1.0f;
        t.pan = 0.0f;
        t.automation.erase (std::remove_if (t.automation.begin(), t.automation.end(),
                                            [] (const TrackPlayback::AutoLine& l)
                                            { return l.target == TrackPlayback::AutoLine::volume || l.target == TrackPlayback::AutoLine::pan; }),
                            t.automation.end());
        t.scalers.clear();
        t.sends.clear();
        t.toMaster = true;
        t.audible = true;
        t.frozen = nullptr;
        t.runtime->gainLeft.setCurrentAndTargetValue (1.0f);
        t.runtime->gainRight.setCurrentAndTargetValue (1.0f);
    }
    snap->masterGain = 1.0f;
    snap->alignLatencies();
    return snap;
}

bool Engine::renderFreeze (const juce::ValueTree& track, const juce::File& file)
{
    if (project == nullptr || ! track.isValid() || Project::isAuxTrack (track))
        return false;

    syncRigStates();
    file.getParentDirectory().createDirectory();
    auto snap = makeFreezeSnapshot ((int) track[ids::id]);
    const auto end = project->contentEnd() + (juce::int64) (sampleRate * freezeTailSeconds);
    const bool ok = renderToFile (*snap, file, 0, end, freezeOptions(), sampleRate);
    pool.forget (file);
    return ok;
}

void Engine::freezeTrack (juce::ValueTree track, std::function<void (bool)> done)
{
    if (project == nullptr || ! track.isValid() || Project::isAuxTrack (track) || (bool) track[ids::frozen] || bounceThread != nullptr)
    {
        if (done) done (false);
        return;
    }

    syncRigStates();
    const auto file = freezeFileFor (track);
    file.getParentDirectory().createDirectory();
    pool.forget (file);

    const auto end = project->contentEnd();
    auto* forProject = project;
    startRender (makeFreezeSnapshot ((int) track[ids::id]), file, 0, end, freezeOptions(),
                 [this, track, file, forProject, done] (bool ok) mutable
                 {
                     ok = ok && project == forProject && track.getParent().isValid();
                     if (ok)
                     {
                         pool.forget (file);
                         project->undo.beginNewTransaction ("Freeze Track");
                         track.setProperty (ids::freezeFile, file.getRelativePathFrom (project->folder).replaceCharacter ('\\', '/'),
                                            &project->undo);
                         track.setProperty (ids::frozen, true, &project->undo);
                     }
                     if (done) done (ok);
                 });
}

void Engine::unfreezeTrack (juce::ValueTree track)
{
    if (project == nullptr || ! (bool) track[ids::frozen])
        return;
    project->undo.beginNewTransaction ("Unfreeze Track");
    track.setProperty (ids::frozen, false, &project->undo);
}

} // namespace woodshed::daw
