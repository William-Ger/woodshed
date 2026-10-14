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
}

RigPanel::RigPanel (Studio& s) : studio (s)
{
    rigSwitch.setColour (juce::ToggleButton::textColourId, theme::text);
    rigSwitch.setColour (juce::ToggleButton::tickColourId, theme::accent);
    rigSwitch.setColour (juce::ToggleButton::tickDisabledColourId, theme::dimText);
    rigSwitch.setTooltip ("Run this track through its rig (off: the track plays dry)");
    rigSwitch.onClick = [this] { setRigOn (rigSwitch.getToggleState()); };
    addChildComponent (rigSwitch);

    turnOnButton.setColour (juce::TextButton::buttonColourId, theme::accent);
    turnOnButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff1a1410));
    turnOnButton.onClick = [this] { setRigOn (true); };
    addChildComponent (turnOnButton);

    // Space must keep starting playback after a click here, not press the button again.
    for (juce::Component* c : { (juce::Component*) &rigSwitch, (juce::Component*) &turnOnButton })
    {
        c->setWantsKeyboardFocus (false);
        c->setMouseClickGrabsKeyboardFocus (false);
    }

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

    const bool changed = trackId != shownTrackId || rig != shownRig || on != rigSwitch.getToggleState()
                      || track.isValid() != rigSwitch.isVisible();
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
            layOutEditor();
        }
    }

    rigSwitch.setVisible (track.isValid());
    rigSwitch.setToggleState (on, juce::dontSendNotification);
    turnOnButton.setVisible (track.isValid() && ! on);
    resized();
    repaint();
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
void RigPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    // Header strip: the track's colour, its name and the rig switch.
    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (theme::bar);
    g.fillRect (header);
    g.setColour (theme::outline);
    g.fillRect (header.removeFromBottom (1));
    header.reduce (14, 0);

    if (shownTrack.isValid())
    {
        const auto colour = juce::Colour ((juce::uint32) (int) shownTrack.getProperty (daw::ids::colour, (int) 0xff8d857a));
        g.setColour (colour);
        g.fillRoundedRectangle (header.removeFromLeft (12).withSizeKeepingCentre (12, 12).toFloat(), 3.0f);
        header.removeFromLeft (10);

        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        g.drawFittedText (shownTrack[daw::ids::name].toString(), header.withTrimmedRight (rigSwitch.getWidth() + 20),
                          juce::Justification::centredLeft, 1, 0.8f);
    }
    else
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (13.0f, true));
        g.drawText ("RIG", header, juce::Justification::centredLeft);
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
    auto header = getLocalBounds().removeFromTop (headerHeight).reduced (14, 0);
    rigSwitch.setBounds (header.removeFromRight (84).withSizeKeepingCentre (84, 22));

    const auto body = getLocalBounds().withTrimmedTop (headerHeight);
    turnOnButton.setBounds (body.getCentreX() - 80, body.getCentreY() + 20, 160, 30);

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
}

void RigPanel::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! tree.hasType (daw::ids::TRACK) || (int) tree[daw::ids::id] != shownTrackId)
        return;

    if (property == daw::ids::rigOn)
    {
        sync();
    }
    else if (property == daw::ids::name || property == daw::ids::colour)
    {
        if (auto* w = dynamic_cast<WoodshedEditor*> (editor.get()))
            w->setTrackName (tree[daw::ids::name].toString());
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

} // namespace woodshed::app
