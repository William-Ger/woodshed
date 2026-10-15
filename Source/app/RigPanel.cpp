#include "RigPanel.h"
#include "../PluginEditor.h"

namespace woodshed::app
{

namespace
{
constexpr int holdOffAfterDialogMs = 400;

bool rigIsOn (const juce::ValueTree& track)
{
    return (bool) track.getProperty (daw::ids::rigOn, true);
}

void makeQuiet (juce::Component& c)
{
    // Space must keep starting playback after a click in the header, not press the control again.
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}
}

RigPanel::RigPanel (Studio& s) : studio (s)
{
    powerButton.setTooltip ("Rig on or off (off: the track plays and records dry)");
    powerButton.onClick = [this] { setRigOn (! powerButton.lit); };
    addChildComponent (powerButton);

    tunerButton.setTooltip ("Chromatic tuner (T)");
    tunerButton.onClick = [this]
    {
        if (auto* w = rigEditor())
        {
            w->toggleTuner();
            tunerButton.setToggleState (w->isTunerShowing(), juce::dontSendNotification);
        }
        else
        {
            showTuner();
        }
    };
    addChildComponent (tunerButton);

    inputChoice.setTooltip ("The interface input this track records and monitors");
    inputChoice.setWantsKeyboardFocus (false);
    inputChoice.onChange = [this]
    {
        auto track = studio.getSelectedTrack();
        const int input = inputChoice.getSelectedId() - 1;
        if (track.isValid() && input >= 0 && (int) track.getProperty (daw::ids::input, 0) != input)
        {
            auto& p = studio.project();
            p.undo.beginNewTransaction ("Track Input");
            track.setProperty (daw::ids::input, input, &p.undo);
        }
    };
    addChildComponent (inputChoice);

    turnOnButton.getProperties().set ("primary", true);
    turnOnButton.onClick = [this] { setRigOn (true); };
    addChildComponent (turnOnButton);

    for (juce::Component* c : { (juce::Component*) &powerButton, (juce::Component*) &tunerButton,
                                (juce::Component*) &turnOnButton })
        makeQuiet (*c);

    studio.addChangeListener (this);
    studio.engine().addChangeListener (this);
    studio.commands.addListener (this);
    studio.addProjectSwapListener (this);
    sync();
    startTimerHz (10);
}

RigPanel::~RigPanel()
{
    stopTimer();
    dropEditor();
    studio.commands.removeListener (this);
    studio.removeProjectSwapListener (this);
    studio.engine().removeChangeListener (this);
    studio.removeChangeListener (this);
    watchedState.removeListener (this);
}

//==============================================================================
void RigPanel::sync()
{
    // A replaced project: listen to the new one.
    if (watchedState != studio.project().state)
    {
        dropEditor();
        watchedState.removeListener (this);
        watchedState = studio.project().state;
        watchedState.addListener (this);
        projectSwapPending = false;
    }

    auto track = studio.getSelectedTrack();
    const int trackId = track.isValid() ? (int) track[daw::ids::id] : -1;
    const bool on = track.isValid() && rigIsOn (track);
    auto* rig = on && ! waitingForProjectSwap() ? studio.engine().getRig (trackId) : nullptr;

    const bool changed = trackId != shownTrackId || rig != shownRig || on != powerButton.lit
                      || track.isValid() != powerButton.isVisible();
    if (! changed)
        return;

    dropEditor();
    shownTrack = track;
    shownTrackId = trackId;

    if (rig != nullptr)
    {
        if (auto* ed = rig->createEditorAndMakeActive())
        {
            editor.reset (ed);
            shownRig = rig;

            if (auto* w = dynamic_cast<WoodshedEditor*> (ed))
            {
                w->setEmbedded (true);
                w->setTrackName (track[daw::ids::name].toString());
            }

            addAndMakeVisible (*editor);
            adoptEditorControls();
            layOutEditor();
        }
    }

    powerButton.setVisible (track.isValid());
    powerButton.lit = on;
    powerButton.repaint();
    tunerButton.setVisible (editor != nullptr);
    tunerButton.setToggleState (false, juce::dontSendNotification);
    refreshInputChoice();
    turnOnButton.setVisible (track.isValid() && ! on);
    resized();
    repaint();
}

WoodshedEditor* RigPanel::rigEditor() const
{
    return dynamic_cast<WoodshedEditor*> (editor.get());
}

void RigPanel::adoptEditorControls()
{
    // The editor's bar controls move into this header, outside the editor's scaling, and pick up
    // the studio's look from here. They still belong to the editor and leave with it.
    auto* w = rigEditor();
    if (w == nullptr)
        return;

    auto c = w->getHostControls();
    for (juce::Component* control : { (juce::Component*) &c.inputGain, (juce::Component*) &c.inputMeter,
                                      (juce::Component*) &c.previousPreset, (juce::Component*) &c.presets,
                                      (juce::Component*) &c.nextPreset, (juce::Component*) &c.savePreset,
                                      (juce::Component*) &c.addPedal })
    {
        addAndMakeVisible (control);
        makeQuiet (*control);
    }
}

void RigPanel::dropEditor()
{
    if (editor == nullptr)
        return;

    const bool hadFocus = editor->hasKeyboardFocus (true);

    // The processor must forget its editor before the editor goes (JUCE's host contract).
    if (shownRig != nullptr)
        shownRig->editorBeingDeleted (editor.get());

    editor.reset();
    shownRig = nullptr;

    if (hadFocus)
        if (auto* parent = getParentComponent())
            parent->grabKeyboardFocus();
}

bool RigPanel::waitingForProjectSwap()
{
    if (! projectSwapPending)
        return false;

    // The open dialog is modal while it's up; give its result a moment to arrive once it closes.
    const auto now = juce::Time::getMillisecondCounter();
    if (juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0)
        lastModalTime = now;

    if (now - lastModalTime < (juce::uint32) holdOffAfterDialogMs)
        return true;

    projectSwapPending = false;
    return false;
}

void RigPanel::setRigOn (bool on)
{
    auto track = studio.getSelectedTrack();
    if (! track.isValid() || rigIsOn (track) == on)
        return;

    auto& p = studio.project();
    p.undo.beginNewTransaction (on ? "Rig On" : "Rig Off");
    track.setProperty (daw::ids::rigOn, on, &p.undo);
}

void RigPanel::showTuner()
{
    sync();

    if (auto* w = dynamic_cast<WoodshedEditor*> (editor.get()))
    {
        if (! w->isTunerShowing())
            w->toggleTuner();
        return;
    }

    auto track = studio.getSelectedTrack();
    studio.flash (! track.isValid() ? "Select a track to tune"
                                    : rigIsOn (track) ? "The rig is still starting" : "Turn the rig on to use the tuner");
}

//==============================================================================
void RigPanel::IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool on = getToggleState();

    if (on)
    {
        g.setColour (theme::accent.withAlpha (down ? 0.3f : highlighted ? 0.24f : 0.18f));
        g.fillRoundedRectangle (r, theme::radiusControl);
    }
    else if ((highlighted || down) && isEnabled())
    {
        g.setColour (juce::Colours::white.withAlpha (down ? 0.12f : 0.07f));
        g.fillRoundedRectangle (r, theme::radiusControl);
    }

    auto colour = on || lit ? theme::accent : highlighted ? theme::text : theme::dimText;
    if (! isEnabled())
        colour = theme::faintText.withAlpha (0.6f);

    icons::draw (g, icon, r.withSizeKeepingCentre (16.0f, 16.0f), colour, 1.8f);
}

//==============================================================================
void RigPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    // The pane header: one bar in the studio's chrome.
    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (theme::bar);
    g.fillRect (header);
    g.setColour (theme::highlight);
    g.fillRect (header.withHeight (1));
    g.setColour (theme::separator);
    g.fillRect (header.removeFromBottom (1));

    if (shownTrack.isValid())
    {
        auto name = nameArea;
        const auto colour = juce::Colour ((juce::uint32) (int) shownTrack.getProperty (daw::ids::colour, (int) 0xff8d857a));
        g.setColour (colour);
        g.fillEllipse (name.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat());
        name.removeFromLeft (8);

        g.setColour (rigIsOn (shownTrack) ? theme::text : theme::dimText);
        g.setFont (theme::font (13.0f, theme::Weight::semibold));
        g.drawFittedText (shownTrack[daw::ids::name].toString(), name, juce::Justification::centredLeft, 1, 0.85f);

        if (! gainLabelArea.isEmpty())
        {
            g.setColour (theme::faintText);
            g.setFont (theme::font (11.0f, theme::Weight::medium));
            g.drawText ("Gain", gainLabelArea, juce::Justification::centredRight);
        }
    }
    else
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (13.0f, theme::Weight::semibold));
        g.drawText ("Rig", header.reduced (16, 0), juce::Justification::centredLeft);
    }

    if (editor != nullptr)
        return;

    // Nothing to show: say why.
    auto body = getLocalBounds().withTrimmedTop (headerHeight);
    const bool rigOff = shownTrack.isValid() && ! rigIsOn (shownTrack);
    const auto title = ! shownTrack.isValid() ? juce::String ("No track selected")
                     : rigOff ? juce::String ("Rig is off for this track") : juce::String ("Starting the rig...");
    const auto detail = ! shownTrack.isValid() ? juce::String ("Click a track to see its pedalboard, amp and cab.")
                      : rigOff ? juce::String ("It plays and records dry, straight from the input.") : juce::String();

    auto text = body.withSizeKeepingCentre (juce::jmin (body.getWidth() - 40, 520), 52)
                    .translated (0, rigOff ? -24 : 0);
    g.setColour (theme::text);
    g.setFont (theme::font (17.0f, true));
    g.drawText (title, text.removeFromTop (26), juce::Justification::centred);
    g.setColour (theme::dimText);
    g.setFont (theme::font (13.0f));
    g.drawText (detail, text, juce::Justification::centred);
}

void RigPanel::resized()
{
    auto header = getLocalBounds().removeFromTop (headerHeight).withTrimmedBottom (1).reduced (10, 0);
    constexpr int h = 26;
    const auto fit = [&] (juce::Rectangle<int> r) { return r.withSizeKeepingCentre (r.getWidth(), h); };

    // Left: power, colour dot and name, the track's input, the rig's input gain and level.
    powerButton.setBounds (fit (header.removeFromLeft (h)));
    header.removeFromLeft (6);

    const int nameWidth = shownTrack.isValid()
        ? 18 + juce::GlyphArrangement::getStringWidthInt (theme::font (13.0f, theme::Weight::semibold), shownTrack[daw::ids::name].toString())
        : 0;

    // Right: Add Pedal (primary), tuner, Save, then the preset browser takes what's left.
    auto* w = rigEditor();
    juce::Rectangle<int> right;
    if (w != nullptr)
    {
        auto c = w->getHostControls();
        c.addPedal.setBounds (fit (header.removeFromRight (100)));
        header.removeFromRight (8);
        tunerButton.setBounds (fit (header.removeFromRight (h)));
        header.removeFromRight (12);
        c.savePreset.setBounds (fit (header.removeFromRight (52)));
        header.removeFromRight (8);
        c.nextPreset.setBounds (fit (header.removeFromRight (h)));
        header.removeFromRight (4);
        right = header;      // the preset box sizes itself once the left side is known
    }
    else
    {
        tunerButton.setBounds (fit (header.removeFromRight (h)));
        header.removeFromRight (12);
    }

    const int leftFixed = 16 + 96 + 16 + (w != nullptr ? 30 + 4 + h + 8 + 60 + 16 : 0);
    const int presetMin = w != nullptr ? 140 + 4 + h : 0;
    nameArea = header.removeFromLeft (juce::jlimit (60, 220, juce::jmin (nameWidth, header.getWidth() - leftFixed - presetMin)));
    header.removeFromLeft (16);
    inputChoice.setBounds (fit (header.removeFromLeft (96)));
    header.removeFromLeft (16);

    gainLabelArea = {};
    if (w != nullptr)
    {
        auto c = w->getHostControls();
        gainLabelArea = header.removeFromLeft (30);
        header.removeFromLeft (4);
        c.inputGain.setBounds (fit (header.removeFromLeft (h)));
        header.removeFromLeft (8);
        c.inputMeter.setBounds (header.removeFromLeft (60).withSizeKeepingCentre (60, 8));
        header.removeFromLeft (16);

        const int presetWidth = juce::jlimit (140, 260, header.getWidth() - h - 4);
        auto presets = right.removeFromRight (presetWidth);
        c.presets.setBounds (fit (presets));
        right.removeFromRight (4);
        c.previousPreset.setBounds (fit (right.removeFromRight (h)));
    }

    const auto body = getLocalBounds().withTrimmedTop (headerHeight);
    turnOnButton.setBounds (body.getCentreX() - 80, body.getCentreY() + 20, 160, 28);

    layOutEditor();
}

void RigPanel::layOutEditor()
{
    if (editor == nullptr)
        return;

    const auto body = getLocalBounds().withTrimmedTop (headerHeight);
    if (body.isEmpty())
        return;

    // Below the rig's comfortable size, shrink the whole rig rather than crowd or clip it.
    const float scale = juce::jmin (1.0f,
                                    (float) body.getWidth() / (float) WoodshedEditor::embeddedMinWidth,
                                    (float) body.getHeight() / (float) WoodshedEditor::embeddedMinHeight);

    editor->setTransform (juce::AffineTransform::scale (scale).translated ((float) body.getX(), (float) body.getY()));
    editor->setBounds (0, 0, juce::roundToInt ((float) body.getWidth() / scale), juce::roundToInt ((float) body.getHeight() / scale));
}

//==============================================================================
void RigPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    sync();
}

void RigPanel::timerCallback()
{
    // Catches what no message announces: a new track's rig appearing, a dialog closing.
    sync();

    // The tuner also opens and closes from the keyboard and its own Done button.
    const bool tunerOpen = rigEditor() != nullptr && rigEditor()->isTunerShowing();
    if (tunerButton.getToggleState() != tunerOpen)
        tunerButton.setToggleState (tunerOpen, juce::dontSendNotification);
}

void RigPanel::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! tree.hasType (daw::ids::TRACK) || (int) tree[daw::ids::id] != shownTrackId)
        return;

    if (property == daw::ids::rigOn)
    {
        sync();
    }
    else if (property == daw::ids::input)
    {
        refreshInputChoice();
    }
    else if (property == daw::ids::name || property == daw::ids::colour)
    {
        if (auto* w = dynamic_cast<WoodshedEditor*> (editor.get()))
            w->setTrackName (tree[daw::ids::name].toString());
        resized();
        repaint (getLocalBounds().removeFromTop (headerHeight));
    }
}

void RigPanel::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (child.hasType (daw::ids::TRACK))
        sync();
}

void RigPanel::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    // The engine frees a removed track's rig soon after this; the editor must go first.
    if (child.hasType (daw::ids::TRACK) && (int) child[daw::ids::id] == shownTrackId)
    {
        dropEditor();
        sync();
    }
}

void RigPanel::applicationCommandInvoked (const juce::ApplicationCommandTarget::InvocationInfo& info)
{
    // Starting or opening a project frees every rig before anyone hears about it, so let go of
    // the editor now, while its rig is certainly still alive.
    if (info.commandID == cmd::newProject || info.commandID == cmd::openProject)
    {
        dropEditor();
        projectSwapPending = true;
        lastModalTime = juce::Time::getMillisecondCounter();
        repaint();
    }
}

void RigPanel::projectAboutToBeReplaced()
{
    // Every rig belongs to the project being replaced; the editor must go while they're alive.
    dropEditor();
}

void RigPanel::refreshInputChoice()
{
    auto track = studio.getSelectedTrack();
    inputChoice.setVisible (track.isValid());
    if (! track.isValid())
        return;

    int numInputs = 2;
    if (auto* device = studio.deviceManager.getCurrentAudioDevice())
        numInputs = juce::jmax (numInputs, device->getActiveInputChannels().countNumberOfSetBits());

    if (inputChoice.getNumItems() != numInputs)
    {
        inputChoice.clear (juce::dontSendNotification);
        for (int i = 0; i < numInputs; ++i)
            inputChoice.addItem ("Input " + juce::String (i + 1), i + 1);
    }

    inputChoice.setSelectedId ((int) track.getProperty (daw::ids::input, 0) + 1, juce::dontSendNotification);
}

} // namespace woodshed::app
