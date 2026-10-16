#include "Studio.h"
#include "SettingsWindow.h"
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
    engineInstance = std::make_unique<daw::Engine> (deviceManager,
        [] { return std::make_unique<WoodshedProcessor>(); },
        [] (const juce::String& type) -> std::unique_ptr<Instrument>
        {
            if (auto* t = InstrumentRegistry::get().find (type))
                return t->create();
            return nullptr;
        });
    engineInstance->settleRig = [] (juce::AudioProcessor& rig)
    {
        if (auto* w = dynamic_cast<WoodshedProcessor*> (&rig))
            w->settle();
    };

    initialiseAudio();

    // Preferences from the Settings window.
    auto* prefs = appSettings.getUserSettings();
    if (const juce::File folder (prefs->getValue ("projectsFolder")); folder.isDirectory())
        daw::Project::setDefaultProjectsFolder (folder);
    engineInstance->setClickLevel (juce::Decibels::decibelsToGain ((float) prefs->getDoubleValue ("clickDb", -10.5), -36.0f));
    const bool reopen = prefs->getBoolValue ("reopenLast", true);

    // Reopen the last project (or the one a test launch names); otherwise start a fresh one.
    auto test = juce::SystemStats::getEnvironmentVariable ("WOODSHED_TEST_PROJECT", {});
    const juce::File last (test.isNotEmpty() ? test : reopen ? prefs->getValue ("lastProject") : juce::String());
    std::unique_ptr<daw::Project> p;
    if (last.exists())
        p = daw::Project::load (last);
    adopt (p != nullptr ? std::move (p) : daw::Project::createNew (preferredSampleRate));

    // Test launches can open a pane (rig, mixer, editor) and select a track by number.
    const auto testPane = juce::SystemStats::getEnvironmentVariable ("WOODSHED_TEST_PANE", {});
    if (testPane == "editor") pane = Pane::editor;
    else if (testPane == "mixer") pane = Pane::mixer;
    else if (testPane == "none") pane = Pane::none;
    if (const auto n = juce::SystemStats::getEnvironmentVariable ("WOODSHED_TEST_TRACK", {}).getIntValue(); n > 0)
        if (auto t = currentProject->track (n - 1); t.isValid())
            currentProject->state.setProperty (daw::ids::selectedTrack, t[daw::ids::id], nullptr);

    startTimerHz (10);
}

Studio::~Studio()
{
    stopTimer();
    settingsWindow = nullptr;
    saveProject();
    deviceManager.removeChangeListener (this);
    engineInstance->setProject (nullptr);
    currentProject->state.removeListener (this);
    engineInstance.reset();
    deviceManager.closeAudioDevice();
}

//==============================================================================
namespace
{
bool deviceAvailable (juce::AudioDeviceManager& dm, const juce::String& name, bool input)
{
    if (name.isEmpty())
        return true;
    for (auto* type : dm.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        if (type->getDeviceNames (input).contains (name))
            return true;
    }
    return false;
}
}

juce::AudioDeviceManager::AudioDeviceSetup Studio::wantedSetup (bool& known)
{
    // What the player chose in Settings (saved), else a Scarlett/Focusrite at 64 samples, 48 kHz.
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    known = false;

    if (auto saved = appSettings.getUserSettings()->getXmlValue ("audioDevice"))
    {
        setup.outputDeviceName = saved->getStringAttribute ("audioOutputDeviceName");
        setup.inputDeviceName = saved->getStringAttribute ("audioInputDeviceName");
        setup.sampleRate = saved->getDoubleAttribute ("audioDeviceRate", preferredSampleRate);
        setup.bufferSize = saved->getIntAttribute ("audioDeviceBufferSize", preferredBufferSize);
        known = setup.outputDeviceName.isNotEmpty() || setup.inputDeviceName.isNotEmpty();
    }

    if (! known)
    {
        const auto interfaceName = findInterface (deviceManager);
        setup.inputDeviceName = setup.outputDeviceName = interfaceName;
        setup.sampleRate = preferredSampleRate;
        setup.bufferSize = preferredBufferSize;
        known = interfaceName.isNotEmpty();
    }

    return setup;
}

void Studio::initialiseAudio()
{
    deviceManager.initialise (2, 2, nullptr, false);

    bool known = false;
    const auto wanted = wantedSetup (known);
    if (known && deviceAvailable (deviceManager, wanted.outputDeviceName, false)
              && deviceAvailable (deviceManager, wanted.inputDeviceName, true))
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName = wanted.outputDeviceName;
        setup.inputDeviceName = wanted.inputDeviceName;
        setup.sampleRate = wanted.sampleRate;
        setup.bufferSize = wanted.bufferSize;
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = true;
        deviceManager.setAudioDeviceSetup (setup, true);
    }

    // When the chosen interface is plugged back in, go back to it. (Whatever the Mac falls back to
    // while it's unplugged is never saved as the choice: only the Settings window saves.)
    deviceManager.addChangeListener (this);
}

void Studio::changeListenerCallback (juce::ChangeBroadcaster*)
{
    bool known = false;
    const auto wanted = wantedSetup (known);
    if (! known || switchingDevice)
        return;

    const auto current = deviceManager.getAudioDeviceSetup();
    if (current.outputDeviceName == wanted.outputDeviceName && current.inputDeviceName == wanted.inputDeviceName)
        return;

    if (! deviceAvailable (deviceManager, wanted.outputDeviceName, false) || ! deviceAvailable (deviceManager, wanted.inputDeviceName, true))
        return;

    const juce::ScopedValueSetter<bool> guard (switchingDevice, true);
    auto setup = current;
    setup.outputDeviceName = wanted.outputDeviceName;
    setup.inputDeviceName = wanted.inputDeviceName;
    setup.sampleRate = wanted.sampleRate;
    setup.bufferSize = wanted.bufferSize;
    setup.useDefaultInputChannels = setup.useDefaultOutputChannels = true;
    if (deviceManager.setAudioDeviceSetup (setup, true).isEmpty())
        flash ("Switched to " + wanted.outputDeviceName);
}

void Studio::checkFeedbackRisk()
{
    // The computer's own microphone with its own speakers: whatever the mic hears comes straight
    // back out a few inches away and howls (worse through a guitar amp). Headphones are fine.
    const auto setup = deviceManager.getAudioDeviceSetup();
    auto builtIn = [] (const juce::String& name)
    {
        return name.containsIgnoreCase ("MacBook") || name.containsIgnoreCase ("Built-in") || name.containsIgnoreCase ("iMac")
            || name.containsIgnoreCase ("Mac mini") || name.containsIgnoreCase ("Mac Studio") || name.containsIgnoreCase ("Realtek")
            || name.containsIgnoreCase ("Internal");
    };
    const bool micIn = builtIn (setup.inputDeviceName) || setup.inputDeviceName.containsIgnoreCase ("Microphone Array");
    const bool speakersOut = (builtIn (setup.outputDeviceName) && ! setup.outputDeviceName.containsIgnoreCase ("Headphone"))
                          || setup.outputDeviceName.containsIgnoreCase ("Speakers");
    const bool risk = micIn && speakersOut && deviceManager.getCurrentAudioDevice() != nullptr;

    if (risk == feedbackRisk)
        return;
    feedbackRisk = risk;
    engineInstance->setFeedbackProtection (risk);
    if (risk)
        flash ("Built-in mic and speakers: live input is muted to stop feedback. Use headphones or your interface to hear it.");
}

void Studio::checkAudioAlive()
{
    if (retryTicks > 0)
    {
        --retryTicks;
        return;
    }

    const auto count = engineInstance->getCallbackCount();
    auto* device = deviceManager.getCurrentAudioDevice();
    const bool stalled = device == nullptr || count == lastCallbackCount;
    lastCallbackCount = count;
    silentTicks = stalled && ! switchingDevice ? silentTicks + 1 : 0;
    if (silentTicks < 10)           // a second with no audio callbacks
        return;

    silentTicks = 0;
    retryTicks = 20;                // then give it two seconds before trying again

    // Go back to the chosen interface if it's there, else reopen whatever was open.
    bool known = false;
    auto setup = deviceManager.getAudioDeviceSetup();
    const auto wanted = wantedSetup (known);
    if (known && deviceAvailable (deviceManager, wanted.outputDeviceName, false)
              && deviceAvailable (deviceManager, wanted.inputDeviceName, true))
    {
        setup.outputDeviceName = wanted.outputDeviceName;
        setup.inputDeviceName = wanted.inputDeviceName;
        setup.sampleRate = wanted.sampleRate;
        setup.bufferSize = wanted.bufferSize;
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = true;
    }
    else if (setup.outputDeviceName.isEmpty() || ! deviceAvailable (deviceManager, setup.outputDeviceName, false))
    {
        return;                     // nothing to reopen yet; the device-list listener takes it from here
    }

    const juce::ScopedValueSetter<bool> guard (switchingDevice, true);
    deviceManager.closeAudioDevice();
    const auto error = deviceManager.setAudioDeviceSetup (setup, true);
    flash (error.isEmpty() ? "Reconnected to " + setup.outputDeviceName : "Audio interface: " + error);
}

void Studio::saveAudioSettings()
{
    if (auto xml = deviceManager.createStateXml())
        appSettings.getUserSettings()->setValue ("audioDevice", xml.get());
    appSettings.saveIfNeeded();
}

void Studio::showSettings()
{
    if (settingsWindow == nullptr)
        settingsWindow = std::make_unique<SettingsWindow> (*this);
    settingsWindow->toFront (true);
}

void Studio::closeSettings()
{
    // Called from the window's own close button, so let it finish before it goes.
    juce::MessageManager::callAsync ([this] { settingsWindow = nullptr; });
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

juce::ValueTree Studio::addInstrumentTrack (const juce::String& type, int patchIndex)
{
    auto* it = InstrumentRegistry::get().find (type);
    if (it == nullptr)
        return {};

    auto& p = *currentProject;
    const auto& info = it->info;
    const auto* patch = juce::isPositiveAndBelow (patchIndex, (int) info.patches.size()) ? &info.patches[(size_t) patchIndex] : nullptr;
    const auto selected = getSelectedTrack();
    const int index = selected.isValid() ? p.state.indexOf (selected) + 1 : -1;
    const auto name = patch != nullptr ? patch->name : info.name;

    auto t = p.addInstrumentTrack (name, type, patch != nullptr ? patch->name : juce::String(), patchValues (info, patch), index);
    p.undo.setCurrentTransactionName ("New Instrument Track");
    selectTrack ((int) t[daw::ids::id]);
    return t;
}

void Studio::setInstrument (juce::ValueTree track, const juce::String& type, int patchIndex)
{
    auto* it = InstrumentRegistry::get().find (type);
    if (it == nullptr || ! daw::Project::isInstrumentTrack (track))
        return;

    auto& p = *currentProject;
    const auto& info = it->info;
    const auto* patch = juce::isPositiveAndBelow (patchIndex, (int) info.patches.size()) ? &info.patches[(size_t) patchIndex] : nullptr;
    auto inst = track.getOrCreateChildWithName (daw::ids::INSTRUMENT, &p.undo);

    // A track still named after its old sound takes the new one's name.
    const auto oldPatch = inst[daw::ids::patch].toString();
    const auto* oldType = InstrumentRegistry::get().find (inst[daw::ids::type].toString());
    const auto trackName = track[daw::ids::name].toString();
    const bool autoNamed = trackName == oldPatch || (oldType != nullptr && trackName == oldType->info.name);

    p.undo.beginNewTransaction (patch != nullptr ? "Choose " + patch->name : "Choose " + info.name);
    inst.setProperty (daw::ids::type, type, &p.undo);
    inst.setProperty (daw::ids::patch, patch != nullptr ? patch->name : juce::String(), &p.undo);
    inst.setProperty (daw::ids::knobs, daw::Project::encodeValues (patchValues (info, patch)), &p.undo);
    if (autoNamed)
        track.setProperty (daw::ids::name, patch != nullptr ? patch->name : info.name, &p.undo);
}

void Studio::showNewTrackMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("New Track");
    m.addItem ("Audio  (microphone, guitar or line in)", [this] { addTrack(); });

    // Software instruments, by library section; each instrument's sounds in a submenu.
    juce::PopupMenu instruments;
    for (auto& category : InstrumentRegistry::categories())
    {
        bool header = false;
        for (auto& t : InstrumentRegistry::get().all())
        {
            if (t.info.category != category)
                continue;
            if (! header)
            {
                instruments.addSectionHeader (category);
                header = true;
            }
            juce::PopupMenu sounds;
            const auto type = t.info.type;
            for (int i = 0; i < (int) t.info.patches.size(); ++i)
                sounds.addItem (t.info.patches[(size_t) i].name, [this, type, i] { addInstrumentTrack (type, i); });
            if (sounds.getNumItems() > 0)
                instruments.addSubMenu (t.info.name, sounds);
            else
                instruments.addItem (t.info.name, [this, type] { addInstrumentTrack (type); });
        }
    }
    m.addSubMenu ("Software Instrument", instruments, instruments.getNumItems() > 0);
    m.addItem ("Drums  (a drum kit to program or play)", InstrumentRegistry::get().find ("drums.kit") != nullptr,
               false, [this] { addInstrumentTrack ("drums.kit"); });
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

juce::ValueTree Studio::getEditorRegion() const
{
    if (auto r = findRegion (editorRegionId); r.isValid() && daw::Project::isMidiRegion (r))
        return r;

    for (int id : selectedRegions)
        if (auto r = findRegion (id); r.isValid() && daw::Project::isMidiRegion (r))
            return r;

    auto track = getSelectedTrack();
    if (! daw::Project::isInstrumentTrack (track))
        return {};

    const auto at = engineInstance->getPosition();
    juce::ValueTree best;
    for (auto r : track)
    {
        if (! r.hasType (daw::ids::REGION) || ! daw::Project::isMidiRegion (r))
            continue;
        const auto start = (juce::int64) r[daw::ids::start];
        if (start <= at && at < start + (juce::int64) r[daw::ids::length])
            return r;
        if (start <= at && (! best.isValid() || start > (juce::int64) best[daw::ids::start]))
            best = r;
    }
    if (best.isValid())
        return best;
    for (auto r : track)
        if (r.hasType (daw::ids::REGION) && daw::Project::isMidiRegion (r))
            return r;
    return {};
}

void Studio::openInEditor (int regionId)
{
    editorRegionId = regionId;
    if (auto r = findRegion (regionId); r.isValid())
        selectTrack ((int) r.getParent()[daw::ids::id]);
    pane = Pane::editor;
    sendChangeMessage();
}

juce::ValueTree Studio::createMidiRegion (juce::ValueTree track, juce::int64 at)
{
    if (! daw::Project::isInstrumentTrack (track))
        return {};
    auto& p = *currentProject;
    const auto bar = p.grid().samplesPerBar();
    const auto start = (juce::int64) (std::floor ((double) juce::jmax ((juce::int64) 0, at) / bar) * bar);
    p.undo.beginNewTransaction ("Create MIDI Region");
    auto r = p.addMidiRegion (track, start, (juce::int64) std::llround (bar), track[daw::ids::name].toString());
    setSelectedRegions ({ (int) r[daw::ids::id] });
    return r;
}

void Studio::quantizeNotes (juce::ValueTree region, const juce::Array<juce::ValueTree>& notes, int gridTicks, float strength)
{
    if (! region.isValid() || gridTicks <= 0)
        return;

    juce::Array<juce::ValueTree> targets (notes);
    if (targets.isEmpty())
        for (auto n : region)
            if (n.hasType (daw::ids::NOTE))
                targets.add (n);

    // Snap each note's start relative to the bar grid of the song, not of the region.
    auto& p = *currentProject;
    const auto grid = p.grid();
    const double originTicks = grid.toTicks ((double) daw::Project::contentOrigin (region));
    p.undo.beginNewTransaction ("Quantize");
    for (auto& n : targets)
    {
        auto note = daw::Project::noteFrom (n);
        const double absolute = originTicks + (double) note.start;
        const double snapped = std::round (absolute / gridTicks) * gridTicks;
        note.start = (juce::int64) std::llround ((double) note.start + (snapped - absolute) * strength);
        p.setNote (n, note);
    }
}

void Studio::auditionNote (int pitch, int velocity)
{
    if (! daw::Project::isInstrumentTrack (getSelectedTrack()))
        return;
    engineInstance->setMidiTarget (getSelectedTrackId());
    engineInstance->sendLiveMidi (juce::MidiMessage::noteOn (1, pitch, (juce::uint8) juce::jlimit (1, 127, velocity)));
    auditions.push_back ({ pitch, 3 });
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

void Studio::copySelectedRegions()
{
    if (! selectedRegions.isEmpty())
        clipboard = currentProject->copyRegions (selectedRegions);
}

void Studio::cutSelectedRegions()
{
    if (selectedRegions.isEmpty())
        return;
    copySelectedRegions();
    currentProject->undo.beginNewTransaction ("Cut");
    for (int id : selectedRegions)
        if (auto r = findRegion (id); r.isValid())
            r.getParent().removeChild (r, &currentProject->undo);
    setSelectedRegions ({});
}

void Studio::paste()
{
    auto& p = *currentProject;
    if (! canPaste())
        return;
    if (p.numTracks() == 0)
        addTrack();

    const auto selected = getSelectedTrack();
    const int firstTrack = selected.isValid() ? p.trackList().indexOf (selected) : 0;
    p.undo.beginNewTransaction ("Paste");
    setSelectedRegions (p.pasteRegions (clipboard, engineInstance->getPosition(), juce::jmax (0, firstTrack)));
}

void Studio::repeatSelectedRegions()
{
    if (selectedRegions.isEmpty())
        return;
    currentProject->undo.beginNewTransaction ("Repeat");
    setSelectedRegions (currentProject->repeatRegions (selectedRegions));
}

void Studio::nudgeSelectedRegions (int direction)
{
    if (selectedRegions.isEmpty())
        return;
    const auto grid = currentProject->grid();
    const auto step = (juce::int64) std::llround (grid.samplesPerBeat() * grid.beatUnit / 16.0);   // a 16th note in any meter
    currentProject->undo.beginNewTransaction ("Nudge");
    currentProject->nudgeRegions (selectedRegions, step * direction);
}

void Studio::toggleMuteSelectedRegions()
{
    juce::Array<juce::ValueTree> regions;
    bool allMuted = true;
    for (int id : selectedRegions)
        if (auto r = findRegion (id); r.isValid())
        {
            regions.add (r);
            allMuted = allMuted && (bool) r[daw::ids::mute];
        }
    if (regions.isEmpty())
        return;

    currentProject->undo.beginNewTransaction (allMuted ? "Unmute Regions" : "Mute Regions");
    for (auto& r : regions)
    {
        if (allMuted) r.removeProperty (daw::ids::mute, &currentProject->undo);
        else          r.setProperty (daw::ids::mute, true, &currentProject->undo);
    }
}

void Studio::toggleSelectedTrack (const juce::Identifier& property)
{
    if (auto t = getSelectedTrack(); t.isValid())
        t.setProperty (property, ! (bool) t[property], nullptr);
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
    checkAudioAlive();
    checkFeedbackRisk();

    // Live MIDI plays the selected track when it's an instrument; new keyboards are picked up.
    {
        auto selected = getSelectedTrack();
        engineInstance->setMidiTarget (daw::Project::isInstrumentTrack (selected) ? (int) selected[daw::ids::id] : -1);
        if (--midiScanTicks <= 0)
        {
            midiScanTicks = 20;
            engineInstance->enableMidiInputs();
        }
        for (auto it = auditions.begin(); it != auditions.end();)
        {
            if (--it->ticksLeft <= 0)
            {
                engineInstance->sendLiveMidi (juce::MidiMessage::noteOff (1, it->pitch));
                it = auditions.erase (it);
            }
            else
                ++it;
        }
    }

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
