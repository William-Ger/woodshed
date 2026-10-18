#include "SpatialPanner.h"

namespace woodshed::app
{

namespace ids = daw::ids;

namespace
{
const juce::String degree (juce::CharPointer_UTF8 ("\xc2\xb0"));
constexpr float maxRadiusMetres = spatial::maxDistance;
}

float SpatialPanner::radiusFor (float metres)
{
    const float m = juce::jlimit (spatial::minDistance, maxRadiusMetres, metres);
    return std::log (m / spatial::minDistance) / std::log (maxRadiusMetres / spatial::minDistance);
}

float SpatialPanner::metresAt (float radius)
{
    return spatial::minDistance * std::pow (maxRadiusMetres / spatial::minDistance, juce::jlimit (0.0f, 1.0f, radius));
}

SpatialPanner::SpatialPanner (Studio& s, juce::ValueTree t)
    : studio (s), track (std::move (t)), projectState (studio.project().state)
{
    auto& undo = studio.project().undo;
    enabled.onClick = [this, &undo]
    {
        undo.beginNewTransaction (enabled.getToggleState() ? "3D Position" : "Stereo Pan");
        track.setProperty (ids::spatial, enabled.getToggleState(), &undo);
    };
    binaural.setTooltip ("How 3D tracks reach the Stereo Out: binaural cues for headphones, or amplitude panning for speakers. "
                         "For the whole project.");
    binaural.onClick = [this, &undo]
    {
        undo.beginNewTransaction ("Monitoring");
        projectState.setProperty (ids::spatialOutput, binaural.getToggleState() ? "binaural" : "stereo", &undo);
    };

    auto setup = [this, &undo] (juce::Slider& slider, juce::Label& label, double lo, double hi, double def,
                                const juce::Identifier& prop, const juce::String& suffix, bool vertical)
    {
        slider.setSliderStyle (vertical ? juce::Slider::LinearVertical : juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (vertical ? juce::Slider::TextBoxBelow : juce::Slider::TextBoxRight, false, 48, 18);
        slider.setRange (lo, hi, vertical ? 1.0 : 0.01);
        slider.setTextValueSuffix (suffix);
        slider.setDoubleClickReturnValue (true, def);
        slider.onDragStart = [&undo, &label] { undo.beginNewTransaction ("3D " + label.getText()); };
        slider.onValueChange = [this, &slider, prop] { set (prop, slider.getValue()); };
        label.setFont (theme::font (11.0f, theme::Weight::medium));
        label.setColour (juce::Label::textColourId, theme::dimText);
        label.setJustificationType (vertical ? juce::Justification::centred : juce::Justification::centredLeft);
        addAndMakeVisible (slider);
        addAndMakeVisible (label);
    };
    setup (elevation, elevationLabel, -90.0, 90.0, 0.0, ids::elevation, degree, true);
    setup (spread, spreadLabel, 0.0, 1.0, 0.0, ids::spread, {}, false);
    setup (room, roomLabel, 0.0, 1.0, 0.0, ids::room, {}, false);
    spread.setTooltip ("How far apart the track's left and right channels sit (0: a point).");
    room.setTooltip ("Early reflections from a small room.");

    addAndMakeVisible (enabled);
    addAndMakeVisible (binaural);
    track.addListener (this);
    projectState.addListener (this);
    syncControls();
    setSize (330, 420);
    startTimerHz (30);
}

SpatialPanner::~SpatialPanner()
{
    track.removeListener (this);
    projectState.removeListener (this);
}

void SpatialPanner::show (Studio& s, juce::ValueTree t, juce::Rectangle<int> target)
{
    if (t.isValid())
        juce::CallOutBox::launchAsynchronously (std::make_unique<SpatialPanner> (s, t), target, nullptr);
}

void SpatialPanner::syncControls()
{
    const bool on = (bool) track[ids::spatial];
    enabled.setToggleState (on, juce::dontSendNotification);
    binaural.setToggleState (projectState[ids::spatialOutput].toString() == "binaural", juce::dontSendNotification);
    elevation.setValue ((double) track.getProperty (ids::elevation, 0.0), juce::dontSendNotification);
    spread.setValue ((double) track.getProperty (ids::spread, 0.0), juce::dontSendNotification);
    room.setValue ((double) track.getProperty (ids::room, 0.0), juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &elevation, &spread, &room })
        c->setEnabled (on);
    repaint();
}

void SpatialPanner::valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&)
{
    syncControls();
}

void SpatialPanner::set (const juce::Identifier& prop, double value)
{
    if (! juce::approximatelyEqual ((double) track.getProperty (prop, prop == ids::distance ? 1.0 : 0.0), value))
        track.setProperty (prop, value, &studio.project().undo);
}

juce::Rectangle<float> SpatialPanner::circleArea() const
{
    return getLocalBounds().toFloat().reduced (14.0f).withTrimmedTop (56.0f).withTrimmedRight (56.0f)
                           .withHeight (240.0f).withSizeKeepingCentre (232.0f, 232.0f);
}

juce::Point<float> SpatialPanner::pointFor (float azimuth, float metres) const
{
    const auto c = circleArea();
    const float r = radiusFor (metres) * c.getWidth() * 0.5f;
    const float a = azimuth * juce::MathConstants<float>::pi / 180.0f;
    return { c.getCentreX() + r * std::sin (a), c.getCentreY() - r * std::cos (a) };
}

void SpatialPanner::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    const auto c = circleArea();
    const auto centre = c.getCentre();
    const bool on = (bool) track[ids::spatial];

    // The floor: rings at 1, 2, 5, 10 and 20 m, the front marked at the top.
    theme::drawInset (g, c, c.getWidth() * 0.5f);
    g.setFont (theme::readout (9.0f, theme::Weight::medium));
    for (float m : { 1.0f, 2.0f, 5.0f, 10.0f, 20.0f })
    {
        const float r = radiusFor (m) * c.getWidth() * 0.5f;
        g.setColour (juce::approximatelyEqual (m, 1.0f) ? theme::gridBar.brighter (0.25f) : theme::gridBar);
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre), 1.0f);
        g.setColour (theme::faintText);
        g.drawText (juce::String ((int) m) + " m", juce::Rectangle<float> (40.0f, 12.0f).withCentre ({ centre.x + r * 0.707f + 12.0f, centre.y + r * 0.707f }),
                    juce::Justification::centredLeft, false);
    }
    g.setColour (theme::gridBeat.brighter (0.2f));
    g.drawLine (c.getX(), centre.y, c.getRight(), centre.y, 1.0f);
    g.drawLine (centre.x, c.getY(), centre.x, c.getBottom(), 1.0f);
    g.setColour (theme::dimText);
    g.setFont (theme::font (10.0f, theme::Weight::medium));
    g.drawText ("Front", juce::Rectangle<float> (60.0f, 12.0f).withCentre ({ centre.x, c.getY() - 8.0f }), juce::Justification::centred, false);
    g.drawText ("Back", juce::Rectangle<float> (60.0f, 12.0f).withCentre ({ centre.x, c.getBottom() + 8.0f }), juce::Justification::centred, false);

    // The listener: a head with its nose to the front.
    const float head = 9.0f;
    g.setColour (theme::control.brighter (0.2f));
    g.fillEllipse (juce::Rectangle<float> (head * 2.0f, head * 2.0f).withCentre (centre));
    juce::Path nose;
    nose.addTriangle (centre.x - 4.0f, centre.y - head + 1.0f, centre.x + 4.0f, centre.y - head + 1.0f, centre.x, centre.y - head - 6.0f);
    g.fillPath (nose);

    const float az = (float) (double) track.getProperty (ids::azimuth, 0.0);
    const float dist = (float) (double) track.getProperty (ids::distance, 1.0);
    const float sp = (float) (double) track.getProperty (ids::spread, 0.0);
    const auto colour = juce::Colour ((juce::uint32) (int) track.getProperty (ids::colour, (int) theme::accent.getARGB()));
    const auto tint = on ? colour : theme::faintText;

    // The spread: the two channels either side of the puck.
    if (sp > 0.001f)
    {
        g.setColour (tint.withAlpha (0.6f));
        for (float side : { -1.0f, 1.0f })
        {
            const auto p = pointFor (az + side * sp * spatial::spreadDegrees, dist);
            g.drawLine ({ pointFor (az, dist), p }, 1.0f);
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (p));
        }
    }

    const auto puck = pointFor (az, dist);
    g.setColour (tint.withAlpha (0.25f));
    g.drawLine ({ centre, puck }, 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillEllipse (juce::Rectangle<float> (18.0f, 18.0f).withCentre (puck.translated (0.0f, 1.0f)));
    g.setColour (tint);
    g.fillEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (puck));
    g.setColour (theme::text.withAlpha (0.8f));
    g.drawEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (puck), 1.0f);

    if (live.x >= 0.0f)
    {
        g.setColour (theme::accent);
        g.drawEllipse (juce::Rectangle<float> (20.0f, 20.0f).withCentre (live), 1.5f);
    }

    // Readout.
    const float el = (float) (double) track.getProperty (ids::elevation, 0.0);
    g.setColour (on ? theme::text : theme::faintText);
    g.setFont (theme::readout (11.0f, theme::Weight::medium));
    const auto readout = "Az " + juce::String (juce::roundToInt (az)) + degree + "   El " + juce::String (juce::roundToInt (el)) + degree
                       + "   " + juce::String (dist, dist < 10.0f ? 1 : 0) + " m";
    g.drawText (readout, getLocalBounds().reduced (14, 0).withTop ((int) c.getBottom() + 18).withHeight (16), juce::Justification::centredLeft, false);
}

void SpatialPanner::resized()
{
    auto r = getLocalBounds().reduced (14);
    enabled.setBounds (r.removeFromTop (22));
    r.removeFromTop (4);
    binaural.setBounds (r.removeFromTop (22));

    const auto c = circleArea();
    elevationLabel.setBounds ((int) c.getRight() + 14, (int) c.getY() - 6, 48, 14);
    elevation.setBounds ((int) c.getRight() + 14, (int) c.getY() + 10, 48, (int) c.getHeight() - 10);

    auto bottom = getLocalBounds().reduced (14).removeFromBottom (54);
    auto row = bottom.removeFromTop (24);
    spreadLabel.setBounds (row.removeFromLeft (52));
    spread.setBounds (row);
    bottom.removeFromTop (6);
    row = bottom.removeFromTop (24);
    roomLabel.setBounds (row.removeFromLeft (52));
    room.setBounds (row);
}

void SpatialPanner::setFromPoint (juce::Point<float> p, bool keepDistance)
{
    const auto c = circleArea();
    const auto d = p - c.getCentre();
    const float az = std::atan2 (d.x, -d.y) * 180.0f / juce::MathConstants<float>::pi;
    set (ids::azimuth, (double) juce::roundToInt (az));
    if (! keepDistance)
    {
        const float metres = metresAt (d.getDistanceFromOrigin() / (c.getWidth() * 0.5f));
        set (ids::distance, std::round ((double) metres * 100.0) / 100.0);
    }
}

void SpatialPanner::mouseDown (const juce::MouseEvent& e)
{
    if (! circleArea().contains (e.position))
        return;
    studio.project().undo.beginNewTransaction ("3D Position");
    if (! (bool) track[ids::spatial])
        track.setProperty (ids::spatial, true, &studio.project().undo);
    setFromPoint (e.position, e.mods.isShiftDown());
}

void SpatialPanner::mouseDrag (const juce::MouseEvent& e)
{
    if (circleArea().contains (e.mouseDownPosition))
        setFromPoint (e.position, e.mods.isShiftDown());
}

void SpatialPanner::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! circleArea().contains (e.position))
        return;
    studio.project().undo.beginNewTransaction ("3D Position");
    set (ids::azimuth, 0.0);
    set (ids::distance, 1.0);
}

void SpatialPanner::timerCallback()
{
    // Automated: show where the line has the track at the playhead.
    auto previous = live;
    live = { -1.0f, -1.0f };
    const auto when = studio.engine().getPosition();
    const auto az = daw::Project::getAutomation (track, "spatial:azimuth");
    const auto dist = daw::Project::getAutomation (track, "spatial:distance");
    if ((bool) track[ids::spatial] && (! az.empty() || ! dist.empty()))
        live = pointFor (az.empty() ? (float) (double) track.getProperty (ids::azimuth, 0.0) : daw::Project::evaluate (az, when),
                         dist.empty() ? (float) (double) track.getProperty (ids::distance, 1.0) : daw::Project::evaluate (dist, when));
    if (live != previous)
        repaint();
}

} // namespace woodshed::app
