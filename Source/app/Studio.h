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
        showMixer, showRig, hideBottom, zoomIn, zoomOut, zoomToFit, toggleFullScreen, showTuner,
        newAudioTrack, newInstrumentTrack, newDrumTrack, showEditor, musicalTyping, quantize, showAutomation,
        importMidi, exportMidi,
        newDrummerTrack, showDrummer, showStepSequencer
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
    /** A software instrument track after the selected one, playing `type` with its patch `patchIndex`. */
    juce::ValueTree addInstrumentTrack (const juce::String& type, int patchIndex = 0);
    /** An aux (bus) after the selected track; with `withEffect`, that pedal type on it (a shared reverb). */
    juce::ValueTree addAuxTrack (const juce::String& name, const juce::String& withEffect = {});
    /** Each part of a .mid file on a new instrument track (chosen by its General MIDI program),
        starting at `at`. An empty song takes the file's tempo. Returns how many parts. */
    int importMidiFile (const juce::File&, juce::int64 at);
    void importMidiDialog();
    /** The selected MIDI regions (or all of them) as a .mid file. */
    void exportMidiDialog();
    /** Logic's New Track choice: Audio, Software Instrument (by category) or Drums, at the mouse. */
    void showNewTrackMenu();
    /** Switches an instrument track to another instrument or patch (one undo step). */
    void setInstrument (juce::ValueTree track, const juce::String& type, int patchIndex);

    // The MIDI region the piano roll edits: the one opened explicitly, else the first selected
    // MIDI region, else the selected track's region under (or nearest before) the playhead.
    juce::ValueTree getEditorRegion() const;
    void openInEditor (int regionId);
    /** The same region in the Drummer or Step Sequencer pane. */
    void openInDrummer (int regionId);
    void openInStepSequencer (int regionId);
    /** Opens a region where it was made: a Drummer region in the Drummer, a step pattern in the
        Step Sequencer, anything else in the piano roll (double-click). */
    void openRegion (int regionId);
    /** Logic's Drummer track: a drum kit with an 8-bar Drummer region at the playhead's bar,
        opened in the Drummer (one undo step). */
    juce::ValueTree addDrummerTrack();
    /** A Drummer region of `bars` bars on an instrument track at `at` (snapped to the bar). */
    juce::ValueTree createDrummerRegion (juce::ValueTree track, juce::int64 at, int bars = 8);
    static bool isDrumTrack (const juce::ValueTree& track);
    /** A new empty MIDI region of one bar on an instrument track at `at` (snapped to the bar). */
    juce::ValueTree createMidiRegion (juce::ValueTree track, juce::int64 at);
    /** Snaps the notes (the selected ones, or all) of the editor's region to `gridTicks`. */
    void quantizeNotes (juce::ValueTree region, const juce::Array<juce::ValueTree>& notes, int gridTicks, float strength = 1.0f);
    int getQuantizeTicks() const                            { return quantizeTicks; }
    void setQuantizeTicks (int t)                           { quantizeTicks = t; sendChangeMessage(); }

    /** Plays a short note on the MIDI target (auditioning from the piano roll). */
    void auditionNote (int pitch, int velocity = 100);
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

    /** Logic's automation view (A): every track shows the line of its chosen parameter. */
    bool isShowingAutomation() const                        { return automationShown; }
    void setShowingAutomation (bool on)                     { automationShown = on; sendChangeMessage(); }
    /** The parameter a track shows in automation view (volume unless chosen otherwise). */
    static juce::String shownAutomationParam (const juce::ValueTree& track)
    {
        return track.getProperty (daw::ids::autoParam, "volume").toString();
    }

    // Panes below the tracks.
    enum class Pane { none, rig, mixer, editor, drummer, stepSequencer };
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

    /** Reopens the interface when it stops calling back (a USB blip leaves a dead device behind
        under the same name, so the device list alone never shows the change). */
    void checkAudioAlive();
    /** Turns feedback protection on while the computer's own mic and speakers are in use. */
    void checkFeedbackRisk();
    bool feedbackRisk = false;
    juce::uint64 lastCallbackCount = 0;
    int silentTicks = 0, retryTicks = 0;

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
    int editorRegionId = -1;
    bool automationShown = false, importing = false;
    int quantizeTicks = daw::ticksPerQuarter / 4;
    int midiScanTicks = 0;
    struct Audition { int pitch; int ticksLeft; };
    std::vector<Audition> auditions;
    bool dirty = false;
    juce::String flashText;
    int flashTicks = 0, autosaveTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Studio)
};

} // namespace woodshed::app
