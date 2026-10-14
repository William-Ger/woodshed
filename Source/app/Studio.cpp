#include "Studio.h"
#include "../PluginProcessor.h"

namespace woodshed::app
{

namespace
{
constexpr int preferredBufferSize = 64;
constexpr double preferredSampleRate = 48000.0;
constexpr int autosaveSeconds = 30;

juce::String findInterface (juce::AudioDeviceManager& dm)
{
    for (auto* type : dm.getAvailableDeviceTypes())
    {
       #if JUCE_WINDOWS
        if (type->getTypeName() != "ASIO")
            continue;
       #endif
        type->scanForDevices();
        for (auto& name : type->getDeviceNames (true))
            if (name.containsIgnoreCase ("Scarlett") || name.containsIgnoreCase ("Focusrite"))
                return name;
    }
    return {};
}
}

Studio::Studio (juce::ApplicationProperties& s) : appSettings (s)
{
    engineInstance = std::make_unique<daw::Engine> (deviceManager, [] { return std::make_unique<WoodshedProcessor>(); });
    engineInstance->settleRig = [] (juce::AudioProcessor& rig)
    {
        if (auto* w = dynamic_cast<WoodshedProcessor*> (&rig))
            w->settle();
    };

    initialiseAudio();

    // Reopen the last project (or the one a test launch names); otherwise start a fresh one.
    auto test = juce::SystemStats::getEnvironmentVariable ("WOODSHED_TEST_PROJECT", {});
    const juce::File last (test.isNotEmpty() ? test : appSettings.getUserSettings()->getValue ("lastProject"));
    std::unique_ptr<daw::Project> p;
    if (last.exists())
        p = daw::Project::load (last);
    adopt (p != nullptr ? std::move (p) : daw::Project::createNew (preferredSampleRate));

    startTimerHz (10);
}

Studio::~Studio()
{
    stopTimer();
    saveProject();
    saveAudioSettings();
    engineInstance->setProject (nullptr);
    currentProject->state.removeListener (this);
    engineInstance.reset();
    deviceManager.closeAudioDevice();
}

//==============================================================================
void Studio::initialiseAudio()
{
    auto saved = appSettings.getUserSettings()->getXmlValue ("audioDevice");
    const auto interfaceName = findInterface (deviceManager);

    juce::AudioDeviceManager::AudioDeviceSetup preferred;
    preferred.inputDeviceName  = interfaceName;
    preferred.outputDeviceName = interfaceName;
    preferred.bufferSize       = preferredBufferSize;
    preferred.sampleRate       = preferredSampleRate;

    deviceManager.initialise (2, 2, saved.get(), true, interfaceName, interfaceName.isNotEmpty() ? &preferred : nullptr);
}

void Studio::saveAudioSettings()
{
    if (auto xml = deviceManager.createStateXml())
        appSettings.getUserSettings()->setValue ("audioDevice", xml.get());
    appSettings.saveIfNeeded();
}

void Studio::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (deviceManager, 1, 2, 2, 2, false, false, false, false);
    selector->setSize (520, 420);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio";
    o.dialogBackgroundColour = juce::Colour (0xff1c1a17);
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

//==============================================================================
void Studio::adopt (std::unique_ptr<daw::Project> next)
{
    swapListeners.call ([] (ProjectSwapListener& l) { l.projectAboutToBeReplaced(); });
    engineInstance->setProject (nullptr);
    if (currentProject != nullptr)
        currentProject->state.removeListener (this);

    currentProject = std::move (next);

    // An empty project follows the interface's rate; one with recordings keeps its own.
    const double deviceRate = deviceManager.getCurrentAudioDevice() != nullptr
                                  ? deviceManager.getCurrentAudioDevice()->getCurrentSampleRate() : preferredSampleRate;
    if (currentProject->contentEnd() == 0)
        currentProject->state.setProperty (daw::ids::sampleRate, deviceRate, nullptr);

    currentProject->state.addListener (this);
    engineInstance->setProject (currentProject.get());
    selectedRegions.clear();
    dirty = false;
    rememberProject (currentProject->folder);
    sendChangeMessage();
}

void Studio::rememberProject (const juce::File& folder)
{
    auto* s = appSettings.getUserSettings();
    s->setValue ("lastProject", folder.getFullPathName());

    auto recent = juce::StringArray::fromLines (s->getValue ("recentProjects"));
    recent.removeString (folder.getFullPathName());
    recent.insert (0, folder.getFullPathName());
    recent.removeEmptyStrings();
    while (recent.size() > 10)
        recent.remove (recent.size() - 1);
    s->setValue ("recentProjects", recent.joinIntoString ("\n"));
}

juce::StringArray Studio::getRecentProjects() const
{
    auto recent = juce::StringArray::fromLines (appSettings.getUserSettings()->getValue ("recentProjects"));
    recent.removeEmptyStrings();
    return recent;
}

void Studio::newProject()
{
    saveProject();
    adopt (daw::Project::createNew (preferredSampleRate));
    flash ("New project");
}

bool Studio::openProject (const juce::File& f)
{
    auto p = daw::Project::load (f);
    if (p == nullptr)
    {
        flash ("Couldn't open " + f.getFileName());
        return false;
    }

    saveProject();
    adopt (std::move (p));
    flash ("Opened " + currentProject->folder.getFileName());
    return true;
}

void Studio::openProjectDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Open a project", daw::Project::defaultProjectsFolder(), "*.woodshed");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult() != juce::File())
                                  openProject (fc.getResult());
                          });
}

bool Studio::saveProject()
{
    if (currentProject == nullptr)
        return false;

    engineInstance->syncRigStates();
    const bool ok = currentProject->save();
    if (ok)
        dirty = false;
    return ok;
}

void Studio::saveProjectAsDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Save the project as", daw::Project::defaultProjectsFolder().getChildFile (currentProject->folder.getFileName()));
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto folder = fc.getResult();
                              if (folder == juce::File())
                                  return;

                              engineInstance->syncRigStates();
                              if (currentProject->saveAs (folder.withFileExtension ({})))
                              {
                                  dirty = false;
                                  rememberProject (currentProject->folder);
                                  flash ("Saved as " + currentProject->folder.getFileName());
                                  sendChangeMessage();
                              }
                              else
                              {
                                  flash ("Couldn't save there");
                              }
                          });
}

void Studio::bounceDialog()
{
    const auto end = currentProject->contentEnd();
    if (end <= 0)
    {
        flash ("Nothing to bounce yet: record something first");
        return;
    }

    auto suggested = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                         .getChildFile (currentProject->folder.getFileName() + ".wav");
    chooser = std::make_unique<juce::FileChooser> ("Bounce the song to a WAV file", suggested, "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, end] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();
                              if (file == juce::File())
                                  return;

                              // With the loop on, bounce just the loop; otherwise the whole song.
                              auto& s = currentProject->state;
                              const bool loop = (bool) s[daw::ids::loopOn];
                              const auto from = loop ? (juce::int64) s[daw::ids::loopStart] : (juce::int64) 0;
                              const auto to = loop ? (juce::int64) s[daw::ids::loopEnd] : end;

                              file = file.withFileExtension ("wav");
                              engineInstance->startBounce (file, from, to, [this, file] (bool ok)
                              {
                                  flash (ok ? "Bounced " + file.getFileName() : "Bounce failed");
                                  if (ok)
                                      file.revealToUser();
                              });
                          });
}

//==============================================================================
int Studio::getSelectedTrackId() const
{
    return (int) currentProject->state.getProperty (daw::ids::selectedTrack, -1);
}

juce::ValueTree Studio::getSelectedTrack() const
{
    return currentProject->trackById (getSelectedTrackId());
}

void Studio::selectTrack (int trackId)
{
    if (trackId == getSelectedTrackId())
        return;

    // Record-arm follows the selection when only the previously selected track was armed,
    // so recording track after track never leaves an old track armed by accident.
    if (! engineInstance->isRecording())
    {
        auto previous = getSelectedTrack();
        auto next = currentProject->trackById (trackId);
        int armed = 0;
        for (auto t : currentProject->state)
            armed += t.hasType (daw::ids::TRACK) && (bool) t[daw::ids::arm] ? 1 : 0;

        if (armed == 1 && previous.isValid() && (bool) previous[daw::ids::arm] && next.isValid())
        {
            previous.setProperty (daw::ids::arm, false, nullptr);
            next.setProperty (daw::ids::arm, true, nullptr);
        }
    }

    currentProject->state.setProperty (daw::ids::selectedTrack, trackId, nullptr);
    sendChangeMessage();
}

void Studio::setSelectedRegions (const juce::Array<int>& ids)
{
    if (ids == selectedRegions)
        return;
    selectedRegions = ids;
    sendChangeMessage();
}

juce::ValueTree Studio::findRegion (int regionId) const
{
    for (auto t : currentProject->state)
        for (auto r : t)
            if (r.hasType (daw::ids::REGION) && (int) r[daw::ids::id] == regionId)
                return r;
    return {};
}

juce::ValueTree Studio::addTrack()
{
    auto& p = *currentProject;
    p.undo.beginNewTransaction ("New Track");
    const auto selected = getSelectedTrack();
    const int index = selected.isValid() ? p.state.indexOf (selected) + 1 : -1;
    auto t = p.addTrack ("Track " + juce::String (p.numTracks() + 1), index);
    selectTrack ((int) t[daw::ids::id]);
    return t;
}

void Studio::deleteSelectedTrack()
{
    auto t = getSelectedTrack();
    if (! t.isValid())
        return;

    auto& p = *currentProject;
    const int index = p.state.indexOf (t);
    engineInstance->removeTrack (t);

    auto next = p.track (juce::jmin (index, p.numTracks() - 1));
    selectTrack (next.isValid() ? (int) next[daw::ids::id] : -1);
}

void Studio::duplicateSelectedTrack()
{
    auto t = getSelectedTrack();
    if (! t.isValid())
        return;

    engineInstance->syncRigStates();
    auto& p = *currentProject;
    p.undo.beginNewTransaction ("Duplicate Track");

    auto copy = t.createCopy();
    copy.setProperty (daw::ids::id, p.nextId(), nullptr);
    copy.setProperty (daw::ids::name, t[daw::ids::name].toString() + " copy", nullptr);
    copy.setProperty (daw::ids::arm, false, nullptr);
    copy.setProperty (daw::ids::solo, false, nullptr);

    int nextId = (int) copy[daw::ids::id] + 1;
    for (auto r : copy)
        if (r.hasType (daw::ids::REGION))
            r.setProperty (daw::ids::id, nextId++, nullptr);

    p.state.addChild (copy, p.state.indexOf (t) + 1, &p.undo);
    selectTrack ((int) copy[daw::ids::id]);
}

void Studio::deleteSelectedRegions()
{
    if (selectedRegions.isEmpty())
        return;

    currentProject->undo.beginNewTransaction ("Delete");
    for (int id : selectedRegions)
        if (auto r = findRegion (id); r.isValid())
            r.getParent().removeChild (r, &currentProject->undo);

    setSelectedRegions ({});
}

void Studio::splitSelectedAtPlayhead()
{
    const auto at = engineInstance->getPosition();
    juce::Array<juce::ValueTree> targets;

    for (int id : selectedRegions)
        if (auto r = findRegion (id); r.isValid())
            targets.add (r);

    // Nothing selected: split whatever is under the playhead on the selected track.
    if (targets.isEmpty())
        if (auto t = getSelectedTrack(); t.isValid())
            for (auto r : t)
                if (r.hasType (daw::ids::REGION))
                    targets.add (r);

    currentProject->undo.beginNewTransaction ("Split");
    juce::Array<int> rights;
    for (auto& r : targets)
        if (auto right = currentProject->splitRegion (r, at); right.isValid())
            rights.add ((int) right[daw::ids::id]);

    if (! rights.isEmpty())
        setSelectedRegions (rights);
}

void Studio::setPane (Pane p)
{
    if (pane == p)
        return;
    pane = p;
    sendChangeMessage();
}

void Studio::flash (const juce::String& message)
{
    flashText = message;
    flashTicks = 40;
    sendChangeMessage();
}

void Studio::timerCallback()
{
    if (flashTicks > 0 && --flashTicks == 0)
    {
        flashText = {};
        sendChangeMessage();
    }

    if (engineInstance->isBouncing())
    {
        flashText = "Bouncing " + juce::String (juce::roundToInt (engineInstance->getBounceProgress() * 100.0f)) + "%";
        flashTicks = 5;
        sendChangeMessage();
    }

    // Autosave: there's no "unsaved changes" prompt; the project is always on disk.
    if (++autosaveTicks >= autosaveSeconds * 10)
    {
        autosaveTicks = 0;
        if (! engineInstance->isRecording())
        {
            engineInstance->syncRigStates();
            if (dirty)
                saveProject();
        }
    }
}

} // namespace woodshed::app
