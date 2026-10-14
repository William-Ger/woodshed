#include "ArrangeHeader.h"
#include "ArrangeView.h"

namespace woodshed::app::arrange
{

namespace ids = daw::ids;

const juce::Array<juce::Colour>& trackSwatches()
{
    static const juce::Array<juce::Colour> swatches {
        juce::Colour (0xffd9893b), juce::Colour (0xff4fa3c7), juce::Colour (0xff8bbf5a), juce::Colour (0xffc75b7a),
        juce::Colour (0xff9a7bd6), juce::Colour (0xffd6c04f), juce::Colour (0xff4fc7a6), juce::Colour (0xffd66b4f),
        juce::Colour (0xff8f8a80), juce::Colour (0xff6f86d6) };
    return swatches;
}

juce::Colour trackColour (const juce::ValueTree& track)
{
    // Stored as an int64 ARGB: going through int would wrap the alpha.
    return juce::Colour ((juce::uint32) (juce::int64) track.getProperty (ids::colour, (juce::int64) 0xff8f8a80));
}

//==============================================================================
TrackHeader::Toggle::Toggle (TrackHeader& o, const juce::Identifier& p, juce::String l, Glyph gl, juce::Colour on, juce::String u)
    : owner (o), property (p), label (std::move (l)), undoName (std::move (u)), glyph (gl), onColour (on)
{
    setRepaintsOnMouseActivity (true);
}

void TrackHeader::Toggle::paint (juce::Graphics& g)
{
    const bool on = (bool) owner.track[property];
    const auto r = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (on ? onColour : (isMouseOver() ? theme::panelLight.brighter (0.08f) : theme::panelLight));
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (on ? onColour.darker (0.4f) : theme::outline);
    g.drawRoundedRectangle (r, 3.0f, 1.0f);

    if (glyph == Glyph::dot)
    {
        const auto d = r.withSizeKeepingCentre (8.0f, 8.0f);
        g.setColour (on ? juce::Colours::white : theme::record.withAlpha (0.75f));
        g.fillEllipse (d);
        return;
    }

    g.setColour (on ? juce::Colour (0xff15130f) : theme::dimText);
    g.setFont (theme::font (11.0f, true));
    g.drawText (label, getLocalBounds(), juce::Justification::centred);
}

void TrackHeader::Toggle::mouseDown (const juce::MouseEvent&)
{
    auto& p = owner.view.project();
    p.undo.beginNewTransaction (undoName);
    owner.track.setProperty (property, ! (bool) owner.track[property], &p.undo);
    owner.view.studio.selectTrack ((int) owner.track[ids::id]);
}

//==============================================================================
TrackHeader::TrackHeader (ArrangeView& v, juce::ValueTree t)
    : view (v), track (std::move (t)),
      mute (*this, ids::mute, "M", Toggle::Glyph::text, theme::mute, "Mute Track"),
      solo (*this, ids::solo, "S", Toggle::Glyph::text, theme::solo, "Solo Track"),
      arm (*this, ids::arm, "R", Toggle::Glyph::dot, theme::record, "Arm Track"),
      monitor (*this, ids::monitor, "IN", Toggle::Glyph::text, theme::accent, "Monitor Input")
{
    mute.setTooltip ("Mute");
    solo.setTooltip ("Solo");
    arm.setTooltip ("Arm for recording");
    monitor.setTooltip ("Hear the input through the rig");
    for (auto* c : { &mute, &solo, &arm, &monitor })
        addAndMakeVisible (*c);

    name.setText (track[ids::name].toString(), juce::dontSendNotification);
    name.setFont (theme::font (14.0f, true));
    name.setColour (juce::Label::textColourId, theme::text);
    name.setColour (juce::Label::textWhenEditingColourId, theme::text);
    name.setColour (juce::Label::backgroundWhenEditingColourId, theme::window);
    name.setColour (juce::Label::outlineWhenEditingColourId, theme::accent);
    name.setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.35f));
    name.setColour (juce::CaretComponent::caretColourId, theme::accent);
    name.setMinimumHorizontalScale (0.8f);
    name.setBorderSize ({ 1, 3, 1, 3 });
    // Clicks on the name select and drag the track; only the editor (a child) takes the mouse.
    name.setInterceptsMouseClicks (false, true);
    name.onTextChange = [this]
    {
        const auto newName = name.getText().trim();
        if (newName.isEmpty() || newName == track[ids::name].toString())
        {
            name.setText (track[ids::name].toString(), juce::dontSendNotification);
            return;
        }
        auto& p = view.project();
        p.undo.beginNewTransaction ("Rename Track");
        track.setProperty (ids::name, newName, &p.undo);
    };
    addAndMakeVisible (name);

    track.addListener (this);
}

TrackHeader::~TrackHeader()
{
    track.removeListener (this);
}

void TrackHeader::startRename()
{
    name.showEditor();
}

void TrackHeader::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree != track)
        return;
    if (property == ids::name)
        name.setText (track[ids::name].toString(), juce::dontSendNotification);
    repaint();
}

void TrackHeader::paint (juce::Graphics& g)
{
    const bool selected = view.studio.getSelectedTrackId() == (int) track[ids::id];
    const auto colour = trackColour (track);
    auto r = getLocalBounds();

    g.setColour (selected ? theme::panelLight : theme::panel);
    g.fillRect (r);
    if (selected)
    {
        g.setColour (colour.withAlpha (0.07f));
        g.fillRect (r);
    }

    // Colour strip: wider and brighter on the selected track.
    g.setColour (selected ? colour : colour.withMultipliedSaturation (0.8f).withMultipliedBrightness (0.8f));
    g.fillRect (r.removeFromLeft (selected ? 6 : 4));

    // Track number, top right.
    const int index = view.project().state.indexOf (track);
    g.setColour (theme::dimText.withAlpha (0.7f));
    g.setFont (theme::font (11.0f));
    g.drawText (juce::String (index + 1), getLocalBounds().removeFromTop (28).removeFromRight (30).withTrimmedRight (10),
                juce::Justification::centredRight);

    g.setColour (theme::outline);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());

    if ((bool) track[ids::mute])
    {
        g.setColour (theme::window.withAlpha (0.25f));
        g.fillRect (getLocalBounds().withTrimmedLeft (6));
    }
}

void TrackHeader::resized()
{
    name.setBounds (12, 9, getWidth() - 12 - 38, 22);

    auto row = juce::Rectangle<int> (15, 40, getWidth() - 30, 20);
    mute.setBounds (row.removeFromLeft (26));
    row.removeFromLeft (4);
    solo.setBounds (row.removeFromLeft (26));
    row.removeFromLeft (4);
    arm.setBounds (row.removeFromLeft (26));
    row.removeFromLeft (4);
    monitor.setBounds (row.removeFromLeft (30));
}

void TrackHeader::mouseDown (const juce::MouseEvent& e)
{
    dragging = false;
    view.studio.selectTrack ((int) track[ids::id]);

    if (e.mods.isPopupMenu())
        view.showTrackMenu (track);
}

void TrackHeader::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || name.isBeingEdited())
        return;
    if (! dragging && e.getDistanceFromDragStartY() != 0 && std::abs (e.getDistanceFromDragStartY()) < 4)
        return;

    if (! dragging)
    {
        dragging = true;
        view.setUserGesture (true);
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }
    view.dragTrackTo (track, e.getEventRelativeTo (&view).y);
}

void TrackHeader::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
    {
        view.endTrackDrag();
        view.setUserGesture (false);
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }
    dragging = false;
}

void TrackHeader::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (name.getBounds().expanded (0, 4).contains (e.getPosition()))
        startRename();
}

} // namespace woodshed::app::arrange
