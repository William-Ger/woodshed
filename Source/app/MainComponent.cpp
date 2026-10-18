#include "MainComponent.h"
#include "PluginHost.h"
#include "SpatialPanner.h"

namespace woodshed::app
{

MainComponent::MainComponent (Studio& s)
    : studio (s), controlBar (s), arrange (s), mixer (s), rig (s), instrument (s), pianoRoll (s), scoreView (s), drummer (s), stepSequencer (s), typing (s), takesEditor (s)
{
    addAndMakeVisible (controlBar);
    addAndMakeVisible (arrange);
    addChildComponent (mixer);
    addChildComponent (rig);
    addChildComponent (instrument);
    addChildComponent (pianoRoll);
    addChildComponent (scoreView);
    addChildComponent (drummer);
    addChildComponent (stepSequencer);
    addChildComponent (typing);
    addChildComponent (takesEditor);
    instrument.onMusicalTyping = [this] { typing.isOpen() ? typing.close() : typing.open(); };
    movie = std::make_unique<MovieController> (studio);
    arrange.attachMovie (movie.get());

    // Tracks above, the pane below; the divider between them can be dragged.
    layout.setItemLayout (0, 120, -1.0, -0.36);
    layout.setItemLayout (1, 6, 6, 6);
    layout.setItemLayout (2, 160, -1.0, -0.64);
    divider = std::make_unique<juce::StretchableLayoutResizerBar> (&layout, 1, false);
    addAndMakeVisible (*divider);

    studio.commands.registerAllCommandsForTarget (this);
    studio.commands.setFirstCommandTarget (this);
    addKeyListener (studio.commands.getKeyMappings());
    setWantsKeyboardFocus (true);

    studio.addChangeListener (this);
    changeListenerCallback (&studio);
}

MainComponent::~MainComponent()
{
    studio.removeChangeListener (this);
    arrange.attachMovie (nullptr);
    movie.reset();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
}

juce::Component* MainComponent::paneComponent()
{
    switch (shownPane)
    {
        case Studio::Pane::mixer:  return &mixer;
        case Studio::Pane::editor: return &pianoRoll;
        case Studio::Pane::takes:  return &takesEditor;
        case Studio::Pane::score:  return &scoreView;
        case Studio::Pane::drummer: return &drummer;
        case Studio::Pane::stepSequencer: return &stepSequencer;
        case Studio::Pane::rig:    return shownInstrument ? static_cast<juce::Component*> (&instrument) : &rig;
        case Studio::Pane::none:
        default:                   return nullptr;
    }
}

void MainComponent::showPaneComponents()
{
    auto* shown = paneComponent();
    for (auto* c : std::initializer_list<juce::Component*> { &mixer, &rig, &instrument, &pianoRoll, &takesEditor, &scoreView, &drummer, &stepSequencer })
        c->setVisible (c == shown);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    controlBar.setBounds (r.removeFromTop (theme::toolbarHeight));
    typing.setBounds (juce::Rectangle<int> (MusicalTyping::preferredWidth, MusicalTyping::preferredHeight)
                          .withCentre ({ getWidth() / 2, getHeight() - MusicalTyping::preferredHeight / 2 - 24 }));

    if (shownPane == Studio::Pane::none)
    {
        divider->setVisible (false);
        arrange.setBounds (r);
        return;
    }

    divider->setVisible (true);
    juce::Component* pane = paneComponent();
    juce::Component* items[] { &arrange, divider.get(), pane };
    layout.layOutComponents (items, 3, r.getX(), r.getY(), r.getWidth(), r.getHeight(), true, true);
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto selected = studio.getSelectedTrack();
    const bool instrumentTrack = daw::Project::isInstrumentTrack (selected) || daw::Project::isAuxTrack (selected);
    if (studio.getPane() != shownPane || instrumentTrack != shownInstrument)
    {
        shownPane = studio.getPane();
        shownInstrument = instrumentTrack;
        showPaneComponents();
        resized();
    }

    studio.commands.commandStatusChanged();
}

//==============================================================================
void MainComponent::getAllCommands (juce::Array<juce::CommandID>& c)
{
    c.addArray ({ cmd::newProject, cmd::openProject, cmd::saveProject, cmd::saveProjectAs, cmd::bounce, cmd::audioSettings,
                  cmd::undo, cmd::redo, cmd::deleteSelection, cmd::splitAtPlayhead, cmd::selectAll,
                  cmd::cut, cmd::copy, cmd::paste, cmd::repeatRegions, cmd::nudgeLeft, cmd::nudgeRight,
                  cmd::muteRegions, cmd::muteTrack, cmd::soloTrack,
                  cmd::createMarker, cmd::nextMarker, cmd::previousMarker,
                  cmd::newTrack, cmd::deleteTrack, cmd::duplicateTrack,
                  cmd::playStop, cmd::record, cmd::returnToStart, cmd::toggleLoop, cmd::toggleClick, cmd::toggleCountIn,
                  cmd::loopToSelection, cmd::showMixer, cmd::showRig, cmd::hideBottom, cmd::zoomIn, cmd::zoomOut,
                  cmd::zoomToFit, cmd::toggleFullScreen, cmd::showTuner,
                  cmd::newAudioTrack, cmd::newInstrumentTrack, cmd::newDrumTrack, cmd::showEditor, cmd::musicalTyping, cmd::quantize, cmd::showAutomation, cmd::importMidi, cmd::exportMidi, cmd::pluginManager, cmd::loopRegions, cmd::showScore,
                  cmd::newDrummerTrack, cmd::showDrummer, cmd::showStepSequencer, cmd::visitWebsite });

    // Spatial audio and video.
    c.addArray ({ cmd::binauralMonitoring, cmd::spatialPanner, cmd::openMovie, cmd::removeMovie, cmd::importMovieAudio, cmd::showMovie });
}

void MainComponent::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    using KP = juce::KeyPress;
    const auto cmdKey = juce::ModifierKeys::commandModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    auto& p = studio.project();
    auto& s = p.state;

    switch (id)
    {
        case cmd::newProject:      info.setInfo ("New Project", {}, "File", 0); info.addDefaultKeypress ('n', cmdKey); break;
        case cmd::openProject:     info.setInfo ("Open...", {}, "File", 0); info.addDefaultKeypress ('o', cmdKey); break;
        case cmd::saveProject:     info.setInfo ("Save", {}, "File", 0); info.addDefaultKeypress ('s', cmdKey); break;
        case cmd::saveProjectAs:   info.setInfo ("Save As...", {}, "File", 0); info.addDefaultKeypress ('s', cmdKey | shift); break;
        case cmd::bounce:          info.setInfo ("Bounce...", {}, "File", 0); info.addDefaultKeypress ('b', cmdKey);
                                   info.setActive (! studio.engine().isBouncing()); break;
        case cmd::audioSettings:   info.setInfo ("Settings...", {}, "File", 0); info.addDefaultKeypress (',', cmdKey); break;
        case cmd::visitWebsite:    info.setInfo ("Built by willgerstung.com", {}, "File", 0); break;

        case cmd::undo:            info.setInfo ("Undo " + p.undo.getUndoDescription(), {}, "Edit", 0); info.addDefaultKeypress ('z', cmdKey);
                                   info.setActive (p.undo.canUndo()); break;
        case cmd::redo:            info.setInfo ("Redo " + p.undo.getRedoDescription(), {}, "Edit", 0); info.addDefaultKeypress ('z', cmdKey | shift);
                                   info.setActive (p.undo.canRedo()); break;
        case cmd::deleteSelection: info.setInfo ("Delete", {}, "Edit", 0); info.addDefaultKeypress (KP::backspaceKey, 0);
                                   info.addDefaultKeypress (KP::deleteKey, 0); info.setActive (! studio.getSelectedRegions().isEmpty()); break;
        case cmd::splitAtPlayhead: info.setInfo ("Split at Playhead", {}, "Edit", 0); info.addDefaultKeypress ('t', cmdKey); break;
        case cmd::selectAll:       info.setInfo ("Select All", {}, "Edit", 0); info.addDefaultKeypress ('a', cmdKey); break;
        case cmd::cut:             info.setInfo ("Cut", {}, "Edit", 0); info.addDefaultKeypress ('x', cmdKey);
                                   info.setActive (! studio.getSelectedRegions().isEmpty()); break;
        case cmd::copy:            info.setInfo ("Copy", {}, "Edit", 0); info.addDefaultKeypress ('c', cmdKey);
                                   info.setActive (! studio.getSelectedRegions().isEmpty()); break;
        case cmd::paste:           info.setInfo ("Paste", {}, "Edit", 0); info.addDefaultKeypress ('v', cmdKey);
                                   info.setActive (studio.canPaste()); break;
        case cmd::repeatRegions:   info.setInfo ("Repeat", {}, "Edit", 0); info.addDefaultKeypress ('r', cmdKey);
                                   info.setActive (! studio.getSelectedRegions().isEmpty()); break;
        case cmd::nudgeLeft:       info.setInfo ("Nudge Left", {}, "Edit", 0);
                                   info.addDefaultKeypress (KP::leftKey, juce::ModifierKeys::altModifier);
                                   info.setActive (! studio.getSelectedRegions().isEmpty()); break;
        case cmd::nudgeRight:      info.setInfo ("Nudge Right", {}, "Edit", 0);
                                   info.addDefaultKeypress (KP::rightKey, juce::ModifierKeys::altModifier);
                                   info.setActive (! studio.getSelectedRegions().isEmpty()); break;
        case cmd::muteRegions:     info.setInfo ("Mute Regions", {}, "Edit", 0);
                                   info.addDefaultKeypress ('m', juce::ModifierKeys::ctrlModifier);
                                   info.setActive (! studio.getSelectedRegions().isEmpty()); break;
        case cmd::createMarker:    info.setInfo ("Create Marker", {}, "Transport", 0);
                                   info.addDefaultKeypress ('\'', juce::ModifierKeys::altModifier); break;
        case cmd::nextMarker:      info.setInfo ("Go to Next Marker", {}, "Transport", 0);
                                   info.addDefaultKeypress ('.', juce::ModifierKeys::altModifier); break;
        case cmd::previousMarker:  info.setInfo ("Go to Previous Marker", {}, "Transport", 0);
                                   info.addDefaultKeypress (',', juce::ModifierKeys::altModifier); break;
        case cmd::muteTrack:       info.setInfo ("Mute Track", {}, "Track", 0); info.addDefaultKeypress ('m', 0); break;
        case cmd::soloTrack:       info.setInfo ("Solo Track", {}, "Track", 0); info.addDefaultKeypress ('s', 0); break;

        case cmd::newTrack:        info.setInfo ("New Track", {}, "Track", 0); info.addDefaultKeypress ('n', cmdKey | juce::ModifierKeys::altModifier); break;
        case cmd::deleteTrack:     info.setInfo ("Delete Track", {}, "Track", 0); info.addDefaultKeypress (KP::backspaceKey, cmdKey);
                                   info.setActive (studio.getSelectedTrack().isValid()); break;
        case cmd::duplicateTrack:  info.setInfo ("Duplicate Track", {}, "Track", 0); info.addDefaultKeypress ('d', cmdKey);
                                   info.setActive (studio.getSelectedTrack().isValid()); break;

        case cmd::playStop:        info.setInfo ("Play / Stop", {}, "Transport", 0); info.addDefaultKeypress (KP::spaceKey, 0);
                                   info.setTicked (studio.engine().isPlaying()); break;
        case cmd::record:          info.setInfo ("Record", {}, "Transport", 0); info.addDefaultKeypress ('r', 0);
                                   info.setTicked (studio.engine().isRecording()); break;
        case cmd::returnToStart:   info.setInfo ("Go to Start", {}, "Transport", 0); info.addDefaultKeypress (KP::returnKey, 0);
                                   info.addDefaultKeypress (KP::homeKey, 0); break;
        case cmd::toggleLoop:      info.setInfo ("Loop", {}, "Transport", 0); info.addDefaultKeypress ('c', 0);
                                   info.setTicked ((bool) s[daw::ids::loopOn]); break;
        case cmd::toggleClick:     info.setInfo ("Click", {}, "Transport", 0); info.addDefaultKeypress ('k', 0);
                                   info.setTicked ((bool) s[daw::ids::click]); break;
        case cmd::toggleCountIn:   info.setInfo ("Count-In", {}, "Transport", 0); info.setTicked ((bool) s[daw::ids::countIn]); break;
        case cmd::loopToSelection: info.setInfo ("Loop Selected Regions", {}, "Transport", 0); info.addDefaultKeypress ('u', 0);
                                   info.setActive (! studio.getSelectedRegions().isEmpty()); break;

        case cmd::showMixer:       info.setInfo ("Mixer", {}, "View", 0); info.addDefaultKeypress ('x', 0);
                                   info.setTicked (studio.getPane() == Studio::Pane::mixer); break;
        case cmd::showRig:         info.setInfo ("Rig / Instrument", {}, "View", 0); info.addDefaultKeypress ('b', 0);
                                   info.setTicked (studio.getPane() == Studio::Pane::rig); break;
        case cmd::hideBottom:      info.setInfo ("Tracks Only", {}, "View", 0); info.addDefaultKeypress ('h', 0); break;
        case cmd::zoomIn:          info.setInfo ("Zoom In", {}, "View", 0); info.addDefaultKeypress ('=', cmdKey); break;
        case cmd::zoomOut:         info.setInfo ("Zoom Out", {}, "View", 0); info.addDefaultKeypress ('-', cmdKey); break;
        case cmd::zoomToFit:       info.setInfo ("Zoom to Fit", {}, "View", 0); info.addDefaultKeypress ('z', 0); break;
        case cmd::toggleFullScreen:info.setInfo ("Full Screen", {}, "View", 0); info.addDefaultKeypress ('f', cmdKey | juce::ModifierKeys::ctrlModifier); break;
        case cmd::showTuner:       info.setInfo ("Tuner", {}, "View", 0); break;
        case cmd::newAudioTrack:   info.setInfo ("New Audio Track", {}, "Track", 0); info.addDefaultKeypress ('a', cmdKey | juce::ModifierKeys::altModifier); break;
        case cmd::newInstrumentTrack: info.setInfo ("New Software Instrument Track", {}, "Track", 0);
                                   info.addDefaultKeypress ('s', cmdKey | juce::ModifierKeys::altModifier); break;
        case cmd::newDrumTrack:    info.setInfo ("New Drum Track", {}, "Track", 0); info.addDefaultKeypress ('u', cmdKey | juce::ModifierKeys::altModifier); break;
        case cmd::showEditor:      info.setInfo ("Piano Roll", {}, "View", 0); info.addDefaultKeypress ('e', 0);
                                   info.setTicked (studio.getPane() == Studio::Pane::editor); break;
        case cmd::showScore:       info.setInfo ("Score", {}, "View", 0); info.addDefaultKeypress ('n', 0);
                                   info.setTicked (studio.getPane() == Studio::Pane::score); break;
        case cmd::newDrummerTrack: info.setInfo ("New Drummer Track", {}, "Track", 0); break;
        case cmd::showDrummer:     info.setInfo ("Drummer", {}, "View", 0);
                                   info.setTicked (studio.getPane() == Studio::Pane::drummer); break;
        case cmd::showStepSequencer: info.setInfo ("Step Sequencer", {}, "View", 0);
                                   info.setTicked (studio.getPane() == Studio::Pane::stepSequencer); break;
        case cmd::musicalTyping:   info.setInfo ("Musical Typing", {}, "View", 0); info.addDefaultKeypress ('k', cmdKey);
                                   info.setTicked (typing.isOpen()); break;
        case cmd::quantize:        info.setInfo ("Quantize Notes", {}, "Edit", 0); info.addDefaultKeypress ('q', 0); break;
        case cmd::importMidi:      info.setInfo ("Import MIDI File...", {}, "File", 0); info.addDefaultKeypress ('i', cmdKey); break;
        case cmd::exportMidi:      info.setInfo ("Export MIDI File...", {}, "File", 0); break;
        case cmd::loopRegions:     info.setInfo ("Loop Regions", {}, "Edit", 0); info.addDefaultKeypress ('l', 0);
                                   info.setActive (! studio.getSelectedRegions().isEmpty()); break;
        case cmd::pluginManager:   info.setInfo ("Plug-in Manager...", {}, "View", 0); break;
        case cmd::showAutomation:  info.setInfo ("Show Automation", {}, "View", 0); info.addDefaultKeypress ('a', 0);
                                   info.setTicked (studio.isShowingAutomation()); break;

        // Spatial audio and video ------------------------------------------------
        case cmd::binauralMonitoring: info.setInfo ("Binaural Monitoring (Headphones)", "3D tracks reach the Stereo Out through binaural cues instead of amplitude panning", "Track", 0);
                                   info.setTicked (s[daw::ids::spatialOutput].toString() == "binaural"); break;
        case cmd::spatialPanner:   info.setInfo ("3D Panner...", {}, "Track", 0); info.addDefaultKeypress ('p', cmdKey | juce::ModifierKeys::altModifier);
                                   info.setActive (studio.getSelectedTrack().isValid()); break;
        case cmd::openMovie:       info.setInfo ("Open Movie...", {}, "File", 0); info.addDefaultKeypress ('o', cmdKey | juce::ModifierKeys::altModifier); break;
        case cmd::removeMovie:     info.setInfo ("Remove Movie", {}, "File", 0); info.setActive (movie != nullptr && movie->hasMovie()); break;
        case cmd::importMovieAudio: info.setInfo ("Import Audio from Movie", {}, "File", 0); info.setActive (movie != nullptr && movie->hasMovie()); break;
        case cmd::showMovie:       info.setInfo ("Movie", {}, "View", 0); info.setActive (movie != nullptr && movie->hasMovie());
                                   info.setTicked (movie != nullptr && movie->isWindowShowing()); break;
        default: break;
    }
}

bool MainComponent::perform (const InvocationInfo& info)
{
    auto& e = studio.engine();
    auto& p = studio.project();
    auto& s = p.state;
    auto toggle = [&s] (const juce::Identifier& id) { s.setProperty (id, ! (bool) s[id], nullptr); };

    switch (info.commandID)
    {
        case cmd::newProject:      studio.newProject(); break;
        case cmd::openProject:     studio.openProjectDialog(); break;
        case cmd::saveProject:     studio.flash (studio.saveProject() ? "Saved" : "Couldn't save"); break;
        case cmd::saveProjectAs:   studio.saveProjectAsDialog(); break;
        case cmd::bounce:          studio.bounceDialog(); break;
        case cmd::audioSettings:   studio.showAudioSettings(); break;
        case cmd::visitWebsite:    juce::URL ("https://willgerstung.com").launchInDefaultBrowser(); break;

        case cmd::undo:            p.undo.undo(); break;
        case cmd::redo:            p.undo.redo(); break;
        case cmd::deleteSelection: studio.deleteSelectedRegions(); break;
        case cmd::splitAtPlayhead: studio.splitSelectedAtPlayhead(); break;
        case cmd::cut:             studio.cutSelectedRegions(); break;
        case cmd::copy:            studio.copySelectedRegions(); break;
        case cmd::paste:           studio.paste(); break;
        case cmd::repeatRegions:   studio.repeatSelectedRegions(); break;
        case cmd::nudgeLeft:       studio.nudgeSelectedRegions (-1); break;
        case cmd::nudgeRight:      studio.nudgeSelectedRegions (1); break;
        case cmd::muteRegions:     studio.toggleMuteSelectedRegions(); break;
        case cmd::createMarker:
            p.undo.beginNewTransaction ("Create Marker");
            p.addMarker (e.getPosition());
            break;
        case cmd::nextMarker:
            if (const auto at = p.nextMarkerAfter (e.getPosition()); at >= 0) e.setPosition (at);
            break;
        case cmd::previousMarker:
            if (const auto at = p.previousMarkerBefore (e.getPosition()); at >= 0) e.setPosition (at);
            break;
        case cmd::muteTrack:       studio.toggleSelectedTrack (daw::ids::mute); break;
        case cmd::soloTrack:       studio.toggleSelectedTrack (daw::ids::solo); break;
        case cmd::selectAll:
        {
            juce::Array<int> all;
            for (auto t : s)
                for (auto r : t)
                    if (r.hasType (daw::ids::REGION))
                        all.add ((int) r[daw::ids::id]);
            studio.setSelectedRegions (all);
            break;
        }

        case cmd::newTrack:        studio.showNewTrackMenu(); break;
        case cmd::deleteTrack:     studio.deleteSelectedTrack(); break;
        case cmd::duplicateTrack:  studio.duplicateSelectedTrack(); break;

        case cmd::playStop:        e.togglePlay(); break;
        case cmd::record:          e.toggleRecord(); break;
        case cmd::returnToStart:   e.setPosition (s[daw::ids::loopOn] ? (juce::int64) s[daw::ids::loopStart] : 0); break;
        case cmd::toggleLoop:      toggle (daw::ids::loopOn); break;
        case cmd::toggleClick:     toggle (daw::ids::click); break;
        case cmd::toggleCountIn:   toggle (daw::ids::countIn); break;
        case cmd::loopToSelection:
        {
            juce::int64 from = std::numeric_limits<juce::int64>::max(), to = 0;
            for (int id : studio.getSelectedRegions())
                if (auto r = studio.findRegion (id); r.isValid())
                {
                    from = juce::jmin (from, (juce::int64) r[daw::ids::start]);
                    to = juce::jmax (to, (juce::int64) r[daw::ids::start] + (juce::int64) r[daw::ids::length]);
                }
            if (to > from)
            {
                p.undo.beginNewTransaction ("Set Loop");
                s.setProperty (daw::ids::loopStart, from, &p.undo);
                s.setProperty (daw::ids::loopEnd, to, &p.undo);
                s.setProperty (daw::ids::loopOn, true, &p.undo);
            }
            break;
        }

        case cmd::showMixer:       studio.setPane (studio.getPane() == Studio::Pane::mixer ? Studio::Pane::none : Studio::Pane::mixer); break;
        case cmd::showRig:         studio.setPane (studio.getPane() == Studio::Pane::rig ? Studio::Pane::none : Studio::Pane::rig); break;
        case cmd::hideBottom:      studio.setPane (Studio::Pane::none); break;
        case cmd::zoomIn:          arrange.zoomBy (1.5); break;
        case cmd::zoomOut:         arrange.zoomBy (1.0 / 1.5); break;
        case cmd::zoomToFit:       arrange.zoomToFit(); break;
        case cmd::toggleFullScreen:
            if (auto* w = findParentComponentOfClass<juce::ResizableWindow>())
                w->setFullScreen (! w->isFullScreen());
            break;
        case cmd::showTuner:       studio.setPane (Studio::Pane::rig); rig.showTuner(); break;
        case cmd::newAudioTrack:   studio.addTrack(); break;
        case cmd::newInstrumentTrack: studio.addInstrumentTrack ("synth.analog"); break;
        case cmd::newDrumTrack:    studio.addInstrumentTrack ("drums.kit"); break;
        case cmd::showEditor:      studio.setPane (studio.getPane() == Studio::Pane::editor ? Studio::Pane::none : Studio::Pane::editor); break;
        case cmd::showScore:       studio.setPane (studio.getPane() == Studio::Pane::score ? Studio::Pane::none : Studio::Pane::score); break;
        case cmd::newDrummerTrack: studio.addDrummerTrack(); break;
        case cmd::showDrummer:     studio.setPane (studio.getPane() == Studio::Pane::drummer ? Studio::Pane::none : Studio::Pane::drummer); break;
        case cmd::showStepSequencer:
            studio.setPane (studio.getPane() == Studio::Pane::stepSequencer ? Studio::Pane::none : Studio::Pane::stepSequencer);
            break;
        case cmd::musicalTyping:   typing.isOpen() ? typing.close() : typing.open(); break;
        case cmd::importMidi:      studio.importMidiDialog(); break;
        case cmd::exportMidi:      studio.exportMidiDialog(); break;
        case cmd::pluginManager:   studio.plugins().showManager(); break;
        case cmd::loopRegions:     studio.toggleLoopSelectedRegions(); break;
        case cmd::showAutomation:  studio.setShowingAutomation (! studio.isShowingAutomation()); break;

        // Spatial audio and video ------------------------------------------------
        case cmd::binauralMonitoring:
            p.undo.beginNewTransaction ("Monitoring");
            s.setProperty (daw::ids::spatialOutput, s[daw::ids::spatialOutput].toString() == "binaural" ? "stereo" : "binaural", &p.undo);
            break;
        case cmd::spatialPanner:
            SpatialPanner::show (studio, studio.getSelectedTrack(),
                                 localAreaToGlobal (arrange.getBounds().withSizeKeepingCentre (40, 40)));
            break;
        case cmd::openMovie:       movie->openDialog(); break;
        case cmd::removeMovie:     movie->remove(); break;
        case cmd::importMovieAudio: movie->importAudio(); break;
        case cmd::showMovie:       movie->toggleWindow(); break;
        case cmd::quantize:
            if (studio.getPane() == Studio::Pane::editor && pianoRoll.isShowing())
                pianoRoll.quantizeSelection();
            else
                for (int id : studio.getSelectedRegions())
                    if (auto r = studio.findRegion (id); daw::Project::isMidiRegion (r))
                        studio.quantizeNotes (r, {}, studio.getQuantizeTicks());
            break;
        default:                   return false;
    }

    studio.commands.commandStatusChanged();
    return true;
}

//==============================================================================
juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Track", "Transport", "View" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int index, const juce::String&)
{
    juce::PopupMenu m;
    auto* cm = &studio.commands;

    switch (index)
    {
        case 0:
        {
            m.addCommandItem (cm, cmd::newProject);
            m.addCommandItem (cm, cmd::openProject);

            juce::PopupMenu recent;
            for (auto& path : studio.getRecentProjects())
            {
                const juce::File f (path);
                recent.addItem (f.getFileName(), f.exists(), false, [this, f] { studio.openProject (f); });
            }
            m.addSubMenu ("Open Recent", recent, recent.getNumItems() > 0);
            m.addSeparator();
            m.addCommandItem (cm, cmd::saveProject);
            m.addCommandItem (cm, cmd::saveProjectAs);
            m.addSeparator();
            m.addCommandItem (cm, cmd::importMidi);
            m.addCommandItem (cm, cmd::exportMidi);
            m.addSeparator();
            m.addCommandItem (cm, cmd::openMovie);           // spatial and video
            m.addCommandItem (cm, cmd::importMovieAudio);
            m.addCommandItem (cm, cmd::removeMovie);
            m.addSeparator();
            m.addCommandItem (cm, cmd::bounce);
           #if ! JUCE_MAC
            m.addSeparator();
            m.addCommandItem (cm, cmd::audioSettings);     // on the Mac these are in the app menu
            m.addCommandItem (cm, cmd::visitWebsite);
           #endif
            break;
        }
        case 1:
            m.addCommandItem (cm, cmd::undo);
            m.addCommandItem (cm, cmd::redo);
            m.addSeparator();
            m.addCommandItem (cm, cmd::cut);
            m.addCommandItem (cm, cmd::copy);
            m.addCommandItem (cm, cmd::paste);
            m.addCommandItem (cm, cmd::deleteSelection);
            m.addCommandItem (cm, cmd::selectAll);
            m.addSeparator();
            m.addCommandItem (cm, cmd::splitAtPlayhead);
            m.addCommandItem (cm, cmd::repeatRegions);
            m.addCommandItem (cm, cmd::muteRegions);
            m.addCommandItem (cm, cmd::loopRegions);
            m.addCommandItem (cm, cmd::nudgeLeft);
            m.addCommandItem (cm, cmd::nudgeRight);
            m.addCommandItem (cm, cmd::quantize);
            break;
        case 2:
            m.addCommandItem (cm, cmd::newTrack);
            m.addCommandItem (cm, cmd::newAudioTrack);
            m.addCommandItem (cm, cmd::newInstrumentTrack);
            m.addCommandItem (cm, cmd::newDrumTrack);
            m.addCommandItem (cm, cmd::newDrummerTrack);
            m.addSeparator();
            m.addCommandItem (cm, cmd::duplicateTrack);
            m.addCommandItem (cm, cmd::deleteTrack);
            m.addSeparator();
            m.addCommandItem (cm, cmd::muteTrack);
            m.addCommandItem (cm, cmd::soloTrack);
            m.addSeparator();
            m.addCommandItem (cm, cmd::spatialPanner);       // spatial and video
            m.addCommandItem (cm, cmd::binauralMonitoring);
            break;
        case 3:
            m.addCommandItem (cm, cmd::playStop);
            m.addCommandItem (cm, cmd::record);
            m.addCommandItem (cm, cmd::returnToStart);
            m.addSeparator();
            m.addCommandItem (cm, cmd::toggleLoop);
            m.addCommandItem (cm, cmd::loopToSelection);
            m.addCommandItem (cm, cmd::toggleClick);
            m.addCommandItem (cm, cmd::toggleCountIn);
            m.addSeparator();
            m.addCommandItem (cm, cmd::createMarker);
            m.addCommandItem (cm, cmd::nextMarker);
            m.addCommandItem (cm, cmd::previousMarker);
            break;
        case 4:
            m.addCommandItem (cm, cmd::showRig);
            m.addCommandItem (cm, cmd::showEditor);
            m.addCommandItem (cm, cmd::showScore);
            m.addCommandItem (cm, cmd::showStepSequencer);
            m.addCommandItem (cm, cmd::showDrummer);
            m.addCommandItem (cm, cmd::showMixer);
            m.addCommandItem (cm, cmd::showAutomation);
            m.addCommandItem (cm, cmd::showMovie);           // spatial and video
            m.addCommandItem (cm, cmd::hideBottom);
            m.addSeparator();
            m.addCommandItem (cm, cmd::zoomIn);
            m.addCommandItem (cm, cmd::zoomOut);
            m.addCommandItem (cm, cmd::zoomToFit);
            m.addSeparator();
            m.addCommandItem (cm, cmd::showTuner);
            m.addCommandItem (cm, cmd::musicalTyping);
            m.addCommandItem (cm, cmd::pluginManager);
            m.addCommandItem (cm, cmd::toggleFullScreen);
            break;
        default: break;
    }

    return m;
}

} // namespace woodshed::app
