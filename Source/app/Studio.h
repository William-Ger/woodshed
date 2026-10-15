#pragma once

#include "../daw/Engine.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace woodshed::app
{

class SettingsWindow;

/** Every menu and keyboard command in the studio. MainComponent routes them. */
namespace cmd
{
    enum : juce::CommandID
    {
        newProject = 0x2000, openProject, saveProject, saveProjectAs, bounce, audioSettings,
        undo, redo, deleteSelection, splitAtPlayhead, selectAll,
        cut, copy, paste, repeatRegions, nudgeLeft, nudgeRight, muteRegions, muteTrack, soloTrack,
        createMarker, nextMarker, previousMarker,
        newTrack, deleteTrack, duplicateTrack,
        playStop, record, returnToStart, toggleLoop, toggleClick, toggleCountIn, loopToSelection,
        showMixer, showRig, hideBottom, zoomIn, zoomOut, zoomToFit, toggleFullScreen, showTuner
    };
}

/**
    The open studio: the audio interface, the engine, the current project and what's selected.
    Views hold a Studio& and listen to it; it broadcasts a change whenever the project is replaced
    or the selection or visible panes change. Project content changes come from the ValueTree.
*/
class Studio final : public juce::ChangeBroadcaster,
                     private juce::ChangeListener,
                     private juce::ValueTree::Listener,
                     private juce::Timer
{
public:
    explicit Studio (juce::ApplicationProperties&);
    ~Studio() override;

    /** Told just before the project (and with it every track's rig) is replaced, so views can
        let go of anything that belongs to the old one. The change message comes after. */
    struct ProjectSwapListener
    {
        virtual ~ProjectSwapListener() = default;
        virtual void projectAboutToBeReplaced() = 0;
    };
    void addProjectSwapListener (ProjectSwapListener* l)    { swapListeners.add (l); }
    void removeProjectSwapListener (ProjectSwapListener* l) { swapListeners.remove (l); }

    juce::AudioDeviceManager deviceManager;
    juce::ApplicationCommandManager commands;

    daw::Engine& engine()                                   { return *engineInstance; }
    daw::Project& project()                                 { return *currentProject; }
    juce::ApplicationProperties& settings()                 { return appSettings; }

    /** Opens the audio interface: the saved setup, else a Scarlett/Focusrite at 64 samples, 48 kHz. */
    void initialiseAudio();
    void saveAudioSettings();
    void showAudioSettings()                                { showSettings(); }
    /** The Settings window (app menu > Settings..., Cmd+,). */
    void showSettings();
    void closeSettings();

    // Project lifecycle (message thread). Async dialogs call back into these.
    void newProject();
    bool openProject (const juce::File& projectFileOrFolder);
    void openProjectDialog();
    bool saveProject();
    void saveProjectAsDialog();
    void bounceDialog();
    juce::StringArray getRecentProjects() const;
    bool isDirty() const                                    { return dirty; }

    // Selection. The selected track is stored in the project (selectedTrack), regions here.
    int getSelectedTrackId() const;
    juce::ValueTree getSelectedTrack() const;
    void selectTrack (int trackId);
    const juce::Array<int>& getSelectedRegions() const      { return selectedRegions; }
    void setSelectedRegions (const juce::Array<int>& regionIds);
    juce::ValueTree findRegion (int regionId) const;

    // Edits that several views trigger.
    juce::ValueTree addTrack();
    void deleteSelectedTrack();
    void duplicateSelectedTrack();
    void deleteSelectedRegions();
    void splitSelectedAtPlayhead();

    // Clipboard and Logic's region commands. Paste goes to the playhead on the selected track.
    void copySelectedRegions();
    void cutSelectedRegions();
    void paste();
    bool canPaste() const                                   { return clipboard.getNumChildren() > 0; }
    void repeatSelectedRegions();
    /** Moves the selected regions by a 1/16 note (the nudge value), -1 earlier or +1 later. */
    void nudgeSelectedRegions (int direction);
    /** Mutes the selected regions, or unmutes them if they're all muted. */
    void toggleMuteSelectedRegions();
    void toggleSelectedTrack (const juce::Identifier& property);

    // Panes below the tracks.
    enum class Pane { none, rig, mixer };
    Pane getPane() const                                    { return pane; }
    void setPane (Pane);

    /** A short message for the status line (saved, bounced, errors). */
    void flash (const juce::String& message);
    juce::String getFlash() const                           { return flashText; }

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { dirty = true; }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override             { dirty = true; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override      { dirty = true; }
    void timerCallback() override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    juce::AudioDeviceManager::AudioDeviceSetup wantedSetup (bool& known);
    bool switchingDevice = false;

    void adopt (std::unique_ptr<daw::Project>);
    void rememberProject (const juce::File&);

    juce::ApplicationProperties& appSettings;
    std::unique_ptr<daw::Engine> engineInstance;
    std::unique_ptr<daw::Project> currentProject;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<SettingsWindow> settingsWindow;

    juce::ListenerList<ProjectSwapListener> swapListeners;
    juce::Array<int> selectedRegions;
    juce::ValueTree clipboard;
    Pane pane = Pane::rig;
    bool dirty = false;
    juce::String flashText;
    int flashTicks = 0, autosaveTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Studio)
};

} // namespace woodshed::app
