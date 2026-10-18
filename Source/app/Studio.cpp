#include "Studio.h"
#include "Theme.h"
#include "SettingsWindow.h"
#include "PluginHost.h"
#include "../PluginProcessor.h"
#include "../daw/MidiFiles.h"
#include "../daw/Drummer.h"
#include "../daw/StepPattern.h"

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
    pluginHost = std::make_unique<PluginHost> (*this);
    engineInstance->setPluginFactory ([this] (const juce::String& xml, double rate, int block, juce::String& error)
                                      { return pluginHost->create (xml, rate, block, error); });
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

    // Test launches can open a pane (rig, mixer, editor, score) and select a track by number.
    const auto testPane = juce::SystemStats::getEnvironmentVariable ("WOODSHED_TEST_PANE", {});
    if (testPane == "editor") pane = Pane::editor;
    else if (testPane == "mixer") pane = Pane::mixer;
    else if (testPane == "score") pane = Pane::score;
    else if (testPane == "none") pane = Pane::none;
    else if (testPane == "takes") pane = Pane::takes;
    else if (testPane == "pitchEdit") pane = Pane::pitchEdit;
    if (const auto r = juce::SystemStats::getEnvironmentVariable ("WOODSHED_TEST_REGION", {}).getIntValue(); r > 0)
        selectedRegions = { r };
    else if (testPane == "drummer") pane = Pane::drummer;
    else if (testPane == "steps") pane = Pane::stepSequencer;
    automationShown = juce::SystemStats::getEnvironmentVariable ("WOODSHED_TEST_AUTOMATION", {}) == "1";
    if (juce::SystemStats::getEnvironmentVariable ("WOODSHED_TEST_PLAY", {}) == "1")
        juce::Timer::callAfterDelay (1500, [this] { engineInstance->play(); });
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
    pluginHost->closeEditors();
    engineInstance->setProject (nullptr);
    currentProject->state.removeListener (this);
    engineInstance.reset();
    pluginHost.reset();
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

namespace
{
/** Logic's Bounce window, cut down: what to bounce and how. */
class BounceSettings final : public juce::Component
{
public:
    BounceSettings (juce::PropertiesFile& p, bool cycleOn, std::function<void (daw::BounceOptions, bool cycle)> go)
        : props (p), onBounce (std::move (go))
    {
        format.addItemList ({ "WAV", "AIFF", "FLAC" }, 1);
        bits.addItemList ({ "16-bit", "24-bit", "32-bit float" }, 1);
        range.addItemList ({ "Whole song", "Cycle (loop) range" }, 1);
        const auto f = props.getValue ("bounceFormat", "wav");
        format.setSelectedId (f == "aiff" ? 2 : f == "flac" ? 3 : 1, juce::dontSendNotification);
        const int b = props.getIntValue ("bounceBits", 24);
        bits.setSelectedId (b == 16 ? 1 : b == 32 ? 3 : 2, juce::dontSendNotification);
        range.setSelectedId (cycleOn ? 2 : 1, juce::dontSendNotification);
        normalize.setToggleState (props.getBoolValue ("bounceNormalize", false), juce::dontSendNotification);
        stems.setToggleState (false, juce::dontSendNotification);
        format.onChange = [this] { if (format.getSelectedId() == 3 && bits.getSelectedId() == 3) bits.setSelectedId (2); };

        for (auto* c : std::initializer_list<juce::Component*> { &format, &bits, &range, &normalize, &stems, &bounce, &cancel })
            addAndMakeVisible (c);
        for (auto* l : { &formatLabel, &bitsLabel, &rangeLabel })
        {
            l->setFont (theme::font (12.0f, theme::Weight::medium));
            l->setColour (juce::Label::textColourId, theme::dimText);
            l->setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (l);
        }
        bounce.getProperties().set ("primary", true);
        bounce.onClick = [this]
        {
            daw::BounceOptions o;
            o.format = format.getSelectedId() == 2 ? "aiff" : format.getSelectedId() == 3 ? "flac" : "wav";
            o.bits = bits.getSelectedId() == 1 ? 16 : bits.getSelectedId() == 3 ? 32 : 24;
            o.normalize = normalize.getToggleState();
            o.stems = stems.getToggleState();
            props.setValue ("bounceFormat", o.format);
            props.setValue ("bounceBits", o.bits);
            props.setValue ("bounceNormalize", o.normalize);
            const bool cycle = range.getSelectedId() == 2;
            auto callback = onBounce;
            if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
                w->exitModalState (0);
            callback (o, cycle);
        };
        cancel.onClick = [this] { if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0); };
        setSize (380, 250);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 18);
        auto row = [&r] { auto x = r.removeFromTop (28); r.removeFromTop (8); return x; };
        auto line = row(); formatLabel.setBounds (line.removeFromLeft (90)); line.removeFromLeft (10); format.setBounds (line);
        line = row(); bitsLabel.setBounds (line.removeFromLeft (90)); line.removeFromLeft (10); bits.setBounds (line);
        line = row(); rangeLabel.setBounds (line.removeFromLeft (90)); line.removeFromLeft (10); range.setBounds (line);
        line = r.removeFromTop (24); line.removeFromLeft (100); normalize.setBounds (line);
        line = r.removeFromTop (24); line.removeFromLeft (100); stems.setBounds (line);
        auto buttons = getLocalBounds().reduced (20, 16).removeFromBottom (30);
        bounce.setBounds (buttons.removeFromRight (100));
        buttons.removeFromRight (10);
        cancel.setBounds (buttons.removeFromRight (90));
    }

private:
    juce::PropertiesFile& props;
    std::function<void (daw::BounceOptions, bool)> onBounce;
    juce::ComboBox format, bits, range;
    juce::Label formatLabel { {}, "Format" }, bitsLabel { {}, "Resolution" }, rangeLabel { {}, "Range" };
    juce::ToggleButton normalize { "Normalize (peak at -0.1 dB)" }, stems { "All tracks as separate files (stems)" };
    juce::TextButton bounce { "Bounce" }, cancel { "Cancel" };
};
}

void Studio::bounceDialog()
{
    const auto end = currentProject->contentEnd();
    if (end <= 0)
    {
        flash ("Nothing to bounce yet: record something first");
        return;
    }

    auto& s = currentProject->state;
    auto settings = std::make_unique<BounceSettings> (*appSettings.getUserSettings(), (bool) s[daw::ids::loopOn],
        [this, end] (daw::BounceOptions options, bool cycle)
        {
            auto& st = currentProject->state;
            const auto from = cycle ? (juce::int64) st[daw::ids::loopStart] : (juce::int64) 0;
            const auto to = cycle ? (juce::int64) st[daw::ids::loopEnd] : end;
            const auto extension = options.format == "aiff" ? ".aif" : options.format == "flac" ? ".flac" : ".wav";
            const auto name = currentProject->folder.getFileName();
            auto suggested = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                 .getChildFile (options.stems ? name + " stems" : name + extension);
            chooser = std::make_unique<juce::FileChooser> (options.stems ? "Choose a folder for the stems" : "Bounce the song to a file", suggested,
                                                           options.stems ? juce::String() : "*" + juce::String (extension));
            const auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting
                             | (options.stems ? juce::FileBrowserComponent::canSelectDirectories : juce::FileBrowserComponent::canSelectFiles);
            chooser->launchAsync (flags, [this, options, from, to, extension] (const juce::FileChooser& fc)
            {
                auto target = fc.getResult();
                if (target == juce::File())
                    return;
                if (! options.stems)
                    target = target.withFileExtension (extension);
                engineInstance->startBounce (target, from, to, options, [this, target, options] (bool ok)
                {
                    flash (ok ? (options.stems ? "Bounced stems to " : "Bounced ") + target.getFileName() : juce::String ("Bounce failed"));
                    if (ok)
                        target.revealToUser();
                });
            });
        });

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (settings.release());
    o.dialogTitle = "Bounce";
    o.dialogBackgroundColour = theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
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

juce::ValueTree Studio::addTrack (bool throughRig, const juce::String& name)
{
    auto& p = *currentProject;
    p.undo.beginNewTransaction ("New Track");
    const auto selected = getSelectedTrack();
    const int index = selected.isValid() ? p.state.indexOf (selected) + 1 : -1;
    auto t = p.addTrack (name.isNotEmpty() ? name : "Track " + juce::String (p.numTracks() + 1), index);
    if (! throughRig)
        t.setProperty (daw::ids::rigOn, false, &p.undo);
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

    if (! importing)
        p.undo.beginNewTransaction ("New Instrument Track");
    auto t = p.addInstrumentTrack (name, type, patch != nullptr ? patch->name : juce::String(), patchValues (info, patch), index);
    selectTrack ((int) t[daw::ids::id]);
    return t;
}

juce::ValueTree Studio::addAuxTrack (const juce::String& name, const juce::String& withEffect)
{
    auto& p = *currentProject;
    const auto selected = getSelectedTrack();
    const int index = selected.isValid() ? p.state.indexOf (selected) + 1 : -1;
    p.undo.beginNewTransaction ("New Aux");
    auto t = p.addAuxTrack (name, index);
    if (auto* type = Registry::get().find (withEffect))
    {
        std::vector<float> values;
        for (auto& k : type->info.knobs)
            values.push_back (k.id == "mix" ? k.max : k.def);      // a bus effect runs fully wet
        p.addInsert (t, withEffect, values);
    }
    selectTrack ((int) t[daw::ids::id]);
    return t;
}

juce::ValueTree Studio::addPluginInstrumentTrack (const juce::PluginDescription& d)
{
    auto& p = *currentProject;
    const auto selected = getSelectedTrack();
    const int index = selected.isValid() ? p.state.indexOf (selected) + 1 : -1;
    p.undo.beginNewTransaction ("New Instrument Track");
    auto t = p.addInstrumentTrack (d.name, "plugin", {}, {}, index);
    auto inst = t.getChildWithName (daw::ids::INSTRUMENT);
    inst.setProperty (daw::ids::plugin, PluginHost::toXml (d), &p.undo);
    inst.setProperty (daw::ids::patch, d.name, &p.undo);
    selectTrack ((int) t[daw::ids::id]);
    setPane (Pane::rig);
    return t;
}

void Studio::setPluginInstrument (juce::ValueTree track, const juce::PluginDescription& d)
{
    if (! daw::Project::isInstrumentTrack (track))
        return;
    auto& p = *currentProject;
    p.undo.beginNewTransaction ("Choose " + d.name);
    auto inst = track.getOrCreateChildWithName (daw::ids::INSTRUMENT, &p.undo);
    inst.setProperty (daw::ids::type, "plugin", &p.undo);
    inst.setProperty (daw::ids::plugin, PluginHost::toXml (d), &p.undo);
    inst.setProperty (daw::ids::patch, d.name, &p.undo);
    inst.removeProperty (daw::ids::state, &p.undo);
    inst.removeProperty (daw::ids::knobs, &p.undo);
}

juce::ValueTree Studio::addPluginInsert (juce::ValueTree owner, const juce::PluginDescription& d)
{
    auto& p = *currentProject;
    p.undo.beginNewTransaction ("Add " + d.name);
    auto fx = p.addInsert (owner, "plugin", {});
    fx.setProperty (daw::ids::plugin, PluginHost::toXml (d), &p.undo);
    fx.setProperty (daw::ids::name, d.name, &p.undo);
    return fx;
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
    inst.removeProperty (daw::ids::plugin, &p.undo);
    inst.removeProperty (daw::ids::state, &p.undo);
    inst.setProperty (daw::ids::type, type, &p.undo);
    inst.setProperty (daw::ids::patch, patch != nullptr ? patch->name : juce::String(), &p.undo);
    inst.setProperty (daw::ids::knobs, daw::Project::encodeValues (patchValues (info, patch)), &p.undo);
    if (autoNamed)
        track.setProperty (daw::ids::name, patch != nullptr ? patch->name : info.name, &p.undo);
}

namespace
{
/** The built-in instrument (and sound) closest to a General MIDI program, or the drum kit. */
std::pair<juce::String, juce::String> instrumentForProgram (int program, bool drums)
{
    if (drums)                  return { "drums.kit", "Studio Kit" };
    if (program < 4)            return { "keys.piano", program == 3 ? "Honky Tonk" : "Concert Grand" };
    if (program < 6)            return { "keys.epiano", program == 4 ? "Stage Tines" : "FM Ballad" };
    if (program == 6)           return { "keys.clav", "Funky Clav" };
    if (program == 7)           return { "keys.clav", "Mellow Neck" };
    if (program < 16)           return { "mallet.keys", program == 11 ? "Vibraphone" : program == 9 ? "Glockenspiel" : program == 14 ? "Tubular Bells" : program == 8 ? "Celesta" : "Marimba" };
    if (program < 24)           return { "keys.organ", program == 19 ? "Cathedral" : program == 17 ? "Rock Overdrive" : "Jazz Trio" };
    if (program < 32)           return { "pluck.guitar", program == 24 ? "Nylon Guitar" : "Steel Guitar" };
    if (program < 38)           return { "bass.electric", program == 35 ? "Fretless" : program == 36 || program == 37 ? "Slap Bass" : program == 34 ? "Pick Bass" : "Finger Bass" };
    if (program < 40)           return { "synth.mono", "Fat Bass" };
    if (program == 45)          return { "orch.strings", "Pizzicato" };
    if (program == 46)          return { "pluck.guitar", "Harp" };
    if (program < 52)           return { "orch.strings", program == 44 ? "Tremolo" : program >= 48 ? "Full Section" : "Chamber" };
    if (program < 56)           return { "orch.choir", program == 53 ? "Oohs" : "Aahs" };
    if (program < 64)           return { "orch.brass", program == 56 ? "Trumpets" : program == 57 ? "Trombones" : program == 59 ? "Muted Trumpet" : program == 60 ? "Horns" : "Brass Section" };
    if (program < 80)           return { "synth.fm", "Glass Mallet" };
    if (program < 88)           return { "synth.mono", "Saw Lead" };
    if (program < 104)          return { "synth.pad", "Warm Analog Pad" };
    if (program < 112)          return { "pluck.guitar", program == 107 ? "Koto" : "Steel Guitar" };
    if (program < 120)          return { "mallet.keys", "Kalimba" };
    return { "synth.pad", "Dark Drone" };
}
}

int Studio::importMidiFile (const juce::File& file, juce::int64 at)
{
    daw::midifile::Contents contents;
    if (! daw::midifile::read (file, contents) || contents.parts.empty())
    {
        flash ("No notes in " + file.getFileName());
        return 0;
    }

    auto& p = *currentProject;
    p.undo.beginNewTransaction ("Import MIDI File");

    // An empty song takes the file's tempo and meter.
    if (p.contentEnd() == 0)
    {
        if (contents.tempo > 0.0)
            p.state.setProperty (daw::ids::tempo, std::round (contents.tempo * 100.0) / 100.0, &p.undo);
        for (auto& c : contents.tempoChanges)
            p.setTempoChange (c.tick, std::round (c.bpm * 100.0) / 100.0);
        if (contents.beatsPerBar > 0 && contents.beatUnit > 0)
        {
            p.state.setProperty (daw::ids::beatsPerBar, contents.beatsPerBar, &p.undo);
            p.state.setProperty (daw::ids::beatUnit, contents.beatUnit, &p.undo);
        }
    }

    const juce::ScopedValueSetter<bool> quiet (importing, true);
    juce::Array<int> added;
    for (auto& part : contents.parts)
    {
        const auto [type, patchName] = instrumentForProgram (part.program, part.channel == 10);
        int patchIndex = 0;
        if (auto* it = InstrumentRegistry::get().find (type))
            for (size_t i = 0; i < it->info.patches.size(); ++i)
                if (it->info.patches[i].name == patchName)
                    patchIndex = (int) i;
        auto track = addInstrumentTrack (type, patchIndex);
        if (! track.isValid())
            continue;
        track.setProperty (daw::ids::name, part.name, &p.undo);
        added.add ((int) daw::midifile::addPart (p, track, part, at)[daw::ids::id]);
    }
    setSelectedRegions (added);
    flash ("Imported " + juce::String (added.size()) + (added.size() == 1 ? " part" : " parts") + " from " + file.getFileName());
    return added.size();
}

void Studio::importMidiDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Import a MIDI file", juce::File::getSpecialLocation (juce::File::userHomeDirectory), "*.mid;*.midi");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        if (fc.getResult().existsAsFile())
            importMidiFile (fc.getResult(), engineInstance->getPosition());
    });
}

void Studio::exportMidiDialog()
{
    juce::Array<juce::ValueTree> regions;
    for (int id : selectedRegions)
        if (auto r = findRegion (id); daw::Project::isMidiRegion (r))
            regions.add (r);
    if (regions.isEmpty())
        for (auto t : currentProject->state)
            for (auto r : t)
                if (r.hasType (daw::ids::REGION) && daw::Project::isMidiRegion (r))
                    regions.add (r);
    if (regions.isEmpty())
    {
        flash ("No MIDI regions to export");
        return;
    }

    const auto name = currentProject->state[daw::ids::name].toString();
    chooser = std::make_unique<juce::FileChooser> ("Export MIDI", currentProject->folder.getChildFile (name + ".mid"), "*.mid");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, regions] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult().withFileExtension ("mid");
        if (fc.getResult() != juce::File())
            flash (daw::midifile::write (*currentProject, regions, file) ? "Exported " + file.getFileName() : juce::String ("Couldn't export the MIDI file"));
    });
}

void Studio::showNewTrackMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("New Track");
    m.addItem ("Guitar or Bass  (audio through the rig)", [this] { addTrack (true); });
    m.addItem ("Voice or Microphone  (audio, no rig)", [this] { addTrack (false, "Vocal"); });

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
    // Audio Units and VST3s, by maker.
    {
        const auto found = pluginHost->instruments();
        if (! found.isEmpty())
        {
            instruments.addSeparator();
            instruments.addSectionHeader ("Audio Units & VST3");
            juce::String maker;
            juce::PopupMenu byMaker;
            auto flush = [&]
            {
                if (byMaker.getNumItems() > 0)
                    instruments.addSubMenu (maker.isNotEmpty() ? maker : juce::String ("Other"), byMaker);
                byMaker = {};
            };
            for (auto& d : found)
            {
                if (d.manufacturerName != maker)
                {
                    flush();
                    maker = d.manufacturerName;
                }
                byMaker.addItem (d.name + (d.pluginFormatName == "VST3" ? "  (VST3)" : ""), [this, d] { addPluginInstrumentTrack (d); });
            }
            flush();
        }
        instruments.addItem ("Plug-in Manager...", [this] { pluginHost->showManager(); });
    }
    m.addSubMenu ("Software Instrument", instruments, instruments.getNumItems() > 0);
    juce::PopupMenu aux;
    aux.addItem ("Reverb", [this] { addAuxTrack ("Reverb", "reverb.hall"); });
    aux.addItem ("Delay", [this] { addAuxTrack ("Delay", "delay.digital"); });
    aux.addItem ("Plate", [this] { addAuxTrack ("Plate", "reverb.plate"); });
    aux.addItem ("Empty Aux", [this] { addAuxTrack ("Aux"); });
    m.addSubMenu ("Aux  (a bus that tracks send to)", aux);
    m.addItem ("Drums  (a drum kit to program or play)", InstrumentRegistry::get().find ("drums.kit") != nullptr,
               false, [this] { addInstrumentTrack ("drums.kit"); });
    m.addItem ("Drummer  (a drummer that plays along)", InstrumentRegistry::get().find ("drums.kit") != nullptr,
               false, [this] { addDrummerTrack(); });
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
    if (pane != Pane::score)     // the score stays open on the region; otherwise the piano roll
        pane = Pane::editor;
    sendChangeMessage();
}

juce::ValueTree Studio::getTakeFolder() const
{
    if (auto r = findRegion (takeFolderId); daw::Project::isTakeFolder (r))
        return r;
    for (int id : selectedRegions)
        if (auto r = findRegion (id); daw::Project::isTakeFolder (r))
            return r;
    return {};
}

void Studio::openTakes (int regionId)
{
    takeFolderId = regionId;
    if (auto r = findRegion (regionId); r.isValid())
    {
        selectTrack ((int) r.getParent()[daw::ids::id]);
        setSelectedRegions ({ regionId });
    }
    pane = Pane::takes;
    sendChangeMessage();
}

void Studio::openInDrummer (int regionId)
{
    editorRegionId = regionId;
    if (auto r = findRegion (regionId); r.isValid())
        selectTrack ((int) r.getParent()[daw::ids::id]);
    pane = Pane::drummer;
    sendChangeMessage();
}

void Studio::openInStepSequencer (int regionId)
{
    editorRegionId = regionId;
    if (auto r = findRegion (regionId); r.isValid())
        selectTrack ((int) r.getParent()[daw::ids::id]);
    pane = Pane::stepSequencer;
    sendChangeMessage();
}

juce::ValueTree Studio::getPitchEditRegion() const
{
    auto isAudio = [] (const juce::ValueTree& r)
    {
        return r.isValid() && r.hasProperty (daw::ids::file) && ! daw::Project::isMidiRegion (r) && ! daw::Project::isTakeFolder (r);
    };
    if (auto r = findRegion (pitchEditRegionId); isAudio (r))
        return r;
    for (int id : selectedRegions)
        if (auto r = findRegion (id); isAudio (r))
            return r;
    return {};
}

void Studio::openInPitchEdit (int regionId)
{
    pitchEditRegionId = regionId;
    if (auto r = findRegion (regionId); r.isValid())
        selectTrack ((int) r.getParent()[daw::ids::id]);
    pane = Pane::pitchEdit;
    sendChangeMessage();
}

void Studio::openRegion (int regionId)
{
    const auto r = findRegion (regionId);
    if (r.hasProperty (daw::ids::file) && ! daw::Project::isMidiRegion (r) && ! daw::Project::isTakeFolder (r))
        openInPitchEdit (regionId);     // an audio region: its notes in Pitch Edit
    else if (daw::drummer::isDrummerRegion (r))
        openInDrummer (regionId);
    else if (r.hasProperty (daw::StepPattern::property))
        openInStepSequencer (regionId);
    else
        openInEditor (regionId);
}

bool Studio::isDrumTrack (const juce::ValueTree& track)
{
    return daw::Project::isInstrumentTrack (track)
        && track.getChildWithName (daw::ids::INSTRUMENT)[daw::ids::type].toString() == "drums.kit";
}

juce::ValueTree Studio::createDrummerRegion (juce::ValueTree track, juce::int64 at, int bars)
{
    if (! daw::Project::isInstrumentTrack (track))
        return {};
    auto& p = *currentProject;
    const auto& grid = p.grid();
    const int firstBar = (int) std::floor (grid.barAt (grid.tickAt ((double) juce::jmax ((juce::int64) 0, at))) + 1.0e-9);
    const auto start = grid.barStartSample (firstBar);
    auto r = p.addMidiRegion (track, start, grid.barStartSample (firstBar + bars) - start, "Drummer");
    daw::drummer::Settings settings;
    settings.bars = bars;
    settings.seed = (juce::uint32) juce::Random::getSystemRandom().nextInt (1 << 30) + 1u;
    daw::drummer::apply (p, r, settings, &p.undo);
    setSelectedRegions ({ (int) r[daw::ids::id] });
    return r;
}

juce::ValueTree Studio::addDrummerTrack()
{
    auto t = addInstrumentTrack ("drums.kit");
    if (! t.isValid())
        return {};
    auto& p = *currentProject;
    p.undo.setCurrentTransactionName ("New Drummer Track");
    t.setProperty (daw::ids::name, "Drummer", &p.undo);
    auto r = createDrummerRegion (t, engineInstance->getPosition());
    openInDrummer ((int) r[daw::ids::id]);
    return t;
}

juce::ValueTree Studio::createMidiRegion (juce::ValueTree track, juce::int64 at)
{
    if (! daw::Project::isInstrumentTrack (track))
        return {};
    auto& p = *currentProject;
    const auto& grid = p.grid();
    const int firstBar = (int) std::floor (grid.barAt (grid.tickAt ((double) juce::jmax ((juce::int64) 0, at))) + 1.0e-9);
    const auto start = grid.barStartSample (firstBar);
    p.undo.beginNewTransaction ("Create MIDI Region");
    auto r = p.addMidiRegion (track, start, grid.barStartSample (firstBar + 1) - start, track[daw::ids::name].toString());
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
    const double originTicks = grid.tickAt ((double) daw::Project::contentOrigin (region));
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

void Studio::toggleLoopSelectedRegions()
{
    juce::Array<juce::ValueTree> regions;
    bool allLooped = true;
    for (int id : selectedRegions)
        if (auto r = findRegion (id); r.isValid())
        {
            regions.add (r);
            allLooped = allLooped && (bool) r[daw::ids::loop];
        }
    if (regions.isEmpty())
        return;
    currentProject->undo.beginNewTransaction (allLooped ? "Unloop Regions" : "Loop Regions");
    for (auto& r : regions)
    {
        if (allLooped) r.removeProperty (daw::ids::loop, &currentProject->undo);
        else           r.setProperty (daw::ids::loop, true, &currentProject->undo);
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
