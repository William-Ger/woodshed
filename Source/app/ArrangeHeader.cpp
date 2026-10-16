#include "ArrangeHeader.h"
#include "ArrangeView.h"
#include "Icons.h"

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
    const bool over = isMouseOver(), down = isMouseButtonDown();
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    constexpr float radius = 5.0f;

    if (on)
    {
        g.setColour (down ? onColour.darker (0.2f) : (over ? onColour.brighter (0.08f) : onColour));
        g.fillRoundedRectangle (r, radius);
    }
    else
    {
        g.setColour (down ? theme::controlDown : (over ? theme::controlHover : theme::control.withAlpha (0.7f)));
        g.fillRoundedRectangle (r, radius);
        // Top light, as on the other raised controls.
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (r.withHeight (radius).toNearestInt());
        g.setColour (theme::highlight);
        g.drawRoundedRectangle (r.reduced (0.5f), radius - 0.5f, 1.0f);
    }

    // Ink on the lit fill: dark on the light colours, white on record red.
    const auto ink = on ? (onColour.getPerceivedBrightness() > 0.6f ? juce::Colour (0xff141416) : juce::Colours::white)
                        : (over ? theme::text : theme::dimText);

    switch (glyph)
    {
        case Glyph::dot:
            g.setColour (on ? juce::Colours::white : theme::record.withAlpha (over ? 0.95f : 0.75f));
            g.fillEllipse (r.withSizeKeepingCentre (8.0f, 8.0f));
            break;

        case Glyph::headphones:
            icons::draw (g, icons::headphones(), r.withSizeKeepingCentre (13.0f, 13.0f), ink);
            break;

        case Glyph::text:
        default:
            g.setColour (ink);
            g.setFont (theme::font (11.0f, theme::Weight::semibold));
            g.drawText (label, getLocalBounds().translated (0, 0), juce::Justification::centred);
            break;
    }
}

void TrackHeader::Toggle::mouseDown (const juce::MouseEvent&)
{
    repaint();
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
      monitor (*this, ids::monitor, "IN", Toggle::Glyph::headphones, theme::monitor, "Monitor Input")
{
    mute.setTooltip ("Mute");
    solo.setTooltip ("Solo");
    arm.setTooltip ("Arm for recording");
    monitor.setTooltip ("Hear the input through the rig");
    for (auto* c : { &mute, &solo, &arm, &monitor })
        addAndMakeVisible (*c);
    // An instrument has no input to monitor: what you play always sounds.
    monitor.setVisible (! daw::Project::isInstrumentTrack (track));

    name.setText (track[ids::name].toString(), juce::dontSendNotification);
    name.setFont (theme::font (13.0f, theme::Weight::medium));
    name.setColour (juce::Label::textColourId, theme::text);
    name.setColour (juce::Label::textWhenEditingColourId, theme::text);
    name.setColour (juce::Label::backgroundWhenEditingColourId, theme::window);
    name.setColour (juce::Label::outlineWhenEditingColourId, theme::accent);
    name.setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.35f));
    name.setColour (juce::CaretComponent::caretColourId, theme::accent);
    name.setMinimumHorizontalScale (0.85f);
    name.setBorderSize ({ 0, 2, 0, 2 });
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

    setRepaintsOnMouseActivity (true);
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

void TrackHeader::updateNameStyle (bool selected)
{
    const bool muted = (bool) track[ids::mute];
    if (styled && selected == styledSelected && muted == styledMuted)
        return;

    styled = true;
    styledSelected = selected;
    styledMuted = muted;
    name.setFont (theme::font (13.0f, selected ? theme::Weight::semibold : theme::Weight::medium));
    name.setColour (juce::Label::textColourId, muted ? theme::dimText : theme::text);
}

void TrackHeader::paint (juce::Graphics& g)
{
    const bool selected = view.studio.getSelectedTrackId() == (int) track[ids::id];
    const auto colour = trackColour (track);
    const auto r = getLocalBounds();
    updateNameStyle (selected);

    // Selected: a raised fill with a line of light along the top. Hover: a whisper of it.
    if (selected)
    {
        g.setColour (theme::panelLight);
        g.fillRect (r);
        g.setColour (theme::highlight);
        g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
    }
    else if (isMouseOver (true))
    {
        g.setColour (juce::Colours::white.withAlpha (0.02f));
        g.fillRect (r);
    }

    // Slim colour bar on the left edge.
    const auto muted = (bool) track[ids::mute];
    g.setColour (muted ? colour.withSaturation (0.15f).withMultipliedBrightness (0.7f) : colour);
    g.fillRect (juce::Rectangle<int> (0, 0, 3, getHeight() - 1));

    // Track number, faint, beside the name.
    const int index = view.project().state.indexOf (track);
    g.setColour (selected ? theme::dimText : theme::faintText);
    g.setFont (theme::font (11.0f, theme::Weight::medium));
    g.drawText (juce::String (index + 1), juce::Rectangle<int> (8, name.getY(), 18, name.getHeight()),
                juce::Justification::centred);

    // What kind of track: piano keys for an instrument, a waveform for audio.
    {
        const bool instrument = daw::Project::isInstrumentTrack (track);
        const auto box = juce::Rectangle<float> ((float) getWidth() - 32.0f, (float) mute.getY(), 20.0f, 20.0f);
        icons::draw (g, instrument ? icons::keyboard() : icons::waveform(), box.reduced (1.0f),
                     selected ? theme::dimText : theme::faintText, 1.4f);
    }

    g.setColour (theme::separator);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
}

void TrackHeader::resized()
{
    constexpr int left = 28;
    name.setBounds (left - 2, 11, getWidth() - left - 10, 20);

    auto row = juce::Rectangle<int> (left, getHeight() - 9 - 20 - 1, getWidth() - left - 10, 20);
    for (auto* t : { &mute, &solo, &arm, &monitor })
    {
        t->setBounds (row.removeFromLeft (20));
        row.removeFromLeft (4);
    }
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
