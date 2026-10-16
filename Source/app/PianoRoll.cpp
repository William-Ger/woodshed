#include "PianoRoll.h"
#include "ArrangeHeader.h"

namespace woodshed::app
{

namespace ids = daw::ids;

namespace
{
constexpr int lowestPitch = 0, highestPitch = 127;
const juce::Colour blackKeyRow { 0xff161619 }, whiteKeyRow { 0xff1c1c20 };

bool isBlackKey (int pitch)
{
    const int pc = ((pitch % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

juce::String pitchName (int pitch)
{
    static const char* const names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[((pitch % 12) + 12) % 12]) + juce::String (pitch / 12 - 2);   // Logic: middle C (60) is C3
}

struct GridChoice { const char* label; int ticks; };
const GridChoice gridChoices[] { { "1/4", 960 }, { "1/8", 480 }, { "1/16", 240 }, { "1/32", 120 },
                                 { "1/8 T", 320 }, { "1/16 T", 160 }, { "Bar", 0 } };
}

PianoRoll::PianoRoll (Studio& s) : studio (s)
{
    setWantsKeyboardFocus (true);
    setOpaque (true);

    for (int i = 0; i < (int) std::size (gridChoices); ++i)
        quantizeChoice.addItem (gridChoices[i].label, i + 1);
    quantizeChoice.setTooltip ("Grid: notes snap to it, and Q quantizes to it");
    quantizeChoice.onChange = [this]
    {
        const auto& choice = gridChoices[juce::jlimit (0, (int) std::size (gridChoices) - 1, quantizeChoice.getSelectedId() - 1)];
        const int ticks = choice.ticks > 0 ? choice.ticks : (int) std::llround (studio.project().grid().meterAt (0.0).ticksPerBar());
        studio.setQuantizeTicks (ticks);
        defaultLength = ticks;
    };
    quantizeChoice.setWantsKeyboardFocus (false);
    addAndMakeVisible (quantizeChoice);

    quantizeButton.setTooltip ("Quantize the selected notes (all if none are selected) to the grid (Q)");
    quantizeButton.onClick = [this] { quantizeSelection(); };
    quantizeButton.setWantsKeyboardFocus (false);
    addAndMakeVisible (quantizeButton);

    velocityLabel.setText ("Velocity", juce::dontSendNotification);
    velocityLabel.setFont (theme::font (11.0f, theme::Weight::medium));
    velocityLabel.setColour (juce::Label::textColourId, theme::dimText);
    addAndMakeVisible (velocityLabel);
    velocitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    velocitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 34, 20);
    velocitySlider.setRange (1.0, 127.0, 1.0);
    velocitySlider.setValue (100.0, juce::dontSendNotification);
    velocitySlider.setTooltip ("Velocity of the selected notes");
    velocitySlider.onDragStart = [this] { studio.project().undo.beginNewTransaction ("Note Velocity"); };
    velocitySlider.onValueChange = [this]
    {
        for (auto& n : selection)
        {
            auto note = daw::Project::noteFrom (n);
            note.velocity = (int) velocitySlider.getValue();
            studio.project().setNote (n, note);
        }
    };
    velocitySlider.setWantsKeyboardFocus (false);
    addAndMakeVisible (velocitySlider);

    studio.addChangeListener (this);
    studio.addProjectSwapListener (this);
    sync();
    startTimerHz (30);
}

PianoRoll::~PianoRoll()
{
    watched.removeListener (this);
    studio.removeProjectSwapListener (this);
    studio.removeChangeListener (this);
}

//==============================================================================
void PianoRoll::projectAboutToBeReplaced()
{
    watched.removeListener (this);
    watched = {};
    region = {};
    selection.clear();
}

void PianoRoll::changeListenerCallback (juce::ChangeBroadcaster*)
{
    sync();
}

void PianoRoll::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (tree == region || tree.getParent() == region || tree == region.getParent())
        repaint();
}

void PianoRoll::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent == region)
        repaint();
}

void PianoRoll::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (parent == region)
    {
        selection.removeFirstMatchingValue (child);
        repaint();
    }
    else if (child == region || child == region.getParent())
        sync();
}

void PianoRoll::sync()
{
    if (watched != studio.project().state)
    {
        watched.removeListener (this);
        watched = studio.project().state;
        watched.addListener (this);
    }

    auto next = studio.getEditorRegion();
    if (next != region)
    {
        region = next;
        selection.clear();
        if (region.isValid() && (int) region[ids::id] != lastRegionId)
        {
            lastRegionId = (int) region[ids::id];
            focusOnNotes();
        }
    }

    // The grid menu follows the studio's quantize value.
    const int q = studio.getQuantizeTicks();
    for (int i = 0; i < (int) std::size (gridChoices); ++i)
        if (gridChoices[i].ticks == q && quantizeChoice.getSelectedId() != i + 1)
            quantizeChoice.setSelectedId (i + 1, juce::dontSendNotification);
    if (quantizeChoice.getSelectedId() == 0)
        quantizeChoice.setSelectedId (3, juce::dontSendNotification);

    repaint();
}

void PianoRoll::focusOnNotes()
{
    if (! region.isValid())
        return;

    const auto& grid = studio.project().grid();
    const double originTick = grid.tickAt ((double) daw::Project::contentOrigin (region));
    const double offsetTicks = grid.tickAt ((double) (juce::int64) region[ids::start]) - originTick;
    const double lengthTicks = grid.tickAt ((double) ((juce::int64) region[ids::start] + (juce::int64) region[ids::length])) - originTick - offsetTicks;

    // Horizontally: the whole region with a little room either side.
    const auto area = gridArea();
    if (area.getWidth() > 40)
    {
        pixelsPerTick = juce::jlimit (0.005, 2.0, (double) area.getWidth() / juce::jmax (960.0, lengthTicks * 1.08));
        viewStartTick = offsetTicks - lengthTicks * 0.03;
    }

    // Vertically: centred on the notes, or on middle C.
    int low = 127, high = 0;
    for (auto n : region)
        if (n.hasType (ids::NOTE))
        {
            low = juce::jmin (low, (int) n[ids::pitch]);
            high = juce::jmax (high, (int) n[ids::pitch]);
        }
    const float centre = low <= high ? (float) (low + high) * 0.5f : 60.0f;
    scrollY = (float) (highestPitch - centre) * rowHeight - (float) area.getHeight() * 0.5f;
    scrollY = juce::jlimit (0.0f, juce::jmax (0.0f, 128.0f * rowHeight - (float) area.getHeight()), scrollY);
}

//==============================================================================
juce::Rectangle<int> PianoRoll::rulerArea() const
{
    return { keyboardWidth, headerHeight, getWidth() - keyboardWidth, rulerHeight };
}

juce::Rectangle<int> PianoRoll::gridArea() const
{
    return { keyboardWidth, headerHeight + rulerHeight, getWidth() - keyboardWidth,
             juce::jmax (0, getHeight() - headerHeight - rulerHeight - velocityHeight) };
}

juce::Rectangle<int> PianoRoll::keyboardArea() const
{
    const auto g = gridArea();
    return { 0, g.getY(), keyboardWidth, g.getHeight() };
}

juce::Rectangle<int> PianoRoll::velocityArea() const
{
    return { keyboardWidth, getHeight() - velocityHeight, getWidth() - keyboardWidth, velocityHeight };
}

float PianoRoll::xForTick (double tick) const
{
    return (float) keyboardWidth + (float) ((tick - viewStartTick) * pixelsPerTick);
}

double PianoRoll::tickAtX (float x) const
{
    return viewStartTick + (double) (x - (float) keyboardWidth) / pixelsPerTick;
}

float PianoRoll::yForPitch (int pitch) const
{
    return (float) gridArea().getY() + (float) (highestPitch - pitch) * rowHeight - scrollY;
}

int PianoRoll::pitchAtY (float y) const
{
    return juce::jlimit (lowestPitch, highestPitch, highestPitch - (int) std::floor ((y - (float) gridArea().getY() + scrollY) / rowHeight));
}

int PianoRoll::gridTicks() const
{
    return juce::jmax (1, studio.getQuantizeTicks());
}

double PianoRoll::snapTicks (double tick, bool enabled) const
{
    if (! enabled || ! region.isValid())
        return std::round (tick);

    // The grid belongs to the song, so snap in song time and convert back.
    const auto& grid = studio.project().grid();
    const double origin = grid.tickAt ((double) daw::Project::contentOrigin (region));
    const double step = gridTicks();
    return std::round ((origin + tick) / step) * step - origin;
}

juce::int64 PianoRoll::playheadTick() const
{
    if (! region.isValid())
        return 0;
    const auto& grid = studio.project().grid();
    return (juce::int64) std::llround (grid.tickAt ((double) studio.engine().getPosition()) - grid.tickAt ((double) daw::Project::contentOrigin (region)));
}

juce::Rectangle<float> PianoRoll::noteRect (const daw::Note& n) const
{
    const float x1 = xForTick ((double) n.start), x2 = xForTick ((double) (n.start + n.length));
    return { x1, yForPitch (n.pitch) + 1.0f, juce::jmax (3.0f, x2 - x1), rowHeight - 2.0f };
}

juce::Array<juce::ValueTree> PianoRoll::notes() const
{
    juce::Array<juce::ValueTree> list;
    for (auto n : region)
        if (n.hasType (ids::NOTE))
            list.add (n);
    return list;
}

void PianoRoll::pruneSelection()
{
    for (int i = selection.size(); --i >= 0;)
        if (selection.getReference (i).getParent() != region)
            selection.remove (i);
}

PianoRoll::Hit PianoRoll::noteAt (juce::Point<int> p) const
{
    Hit hit;
    if (! gridArea().contains (p))
        return hit;

    const auto list = notes();
    for (int i = list.size(); --i >= 0;)
    {
        const auto r = noteRect (daw::Project::noteFrom (list[i]));
        if (! r.expanded (0.0f, 0.5f).contains (p.toFloat()))
            continue;
        hit.note = list[i];
        const float edge = juce::jmin (6.0f, r.getWidth() / 3.0f);
        hit.part = (float) p.x > r.getRight() - edge ? Part::rightEdge
                 : (float) p.x < r.getX() + edge ? Part::leftEdge : Part::body;
        return hit;
    }
    return hit;
}

//==============================================================================
void PianoRoll::resized()
{
    auto header = getLocalBounds().removeFromTop (headerHeight).reduced (10, 6);
    header.removeFromLeft (juce::jmin (260, header.getWidth() / 3));      // region name, painted
    quantizeChoice.setBounds (header.removeFromLeft (84));
    header.removeFromLeft (6);
    quantizeButton.setBounds (header.removeFromLeft (30));
    header.removeFromLeft (20);
    velocityLabel.setBounds (header.removeFromLeft (58));
    velocitySlider.setBounds (header.removeFromLeft (juce::jmin (200, header.getWidth())));

    if (region.isValid() && lastRegionId == (int) region[ids::id] && getWidth() > 0)
        focusOnNotes();
}

void PianoRoll::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    // Header.
    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (theme::bar);
    g.fillRect (header);
    g.setColour (theme::highlight);
    g.fillRect (header.withHeight (1));
    g.setColour (theme::separator);
    g.fillRect (header.removeFromBottom (1));

    auto title = header.reduced (12, 0).removeFromLeft (juce::jmin (250, header.getWidth() / 3));
    if (region.isValid())
    {
        const auto colour = arrange::trackColour (region.getParent());
        g.setColour (colour);
        g.fillEllipse (title.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat());
        title.removeFromLeft (8);
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, theme::Weight::semibold));
        g.drawFittedText (region[ids::name].toString() + "  -  Piano Roll", title, juce::Justification::centredLeft, 1, 0.85f);
    }
    else
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (13.0f, theme::Weight::semibold));
        g.drawText ("Piano Roll", title, juce::Justification::centredLeft);

        auto body = getLocalBounds().withTrimmedTop (headerHeight);
        g.setColour (theme::dimText);
        g.setFont (theme::font (13.0f));
        g.drawText ("Select a MIDI region, or double-click an instrument track's lane to make one.", body, juce::Justification::centred);
        return;
    }

    drawGrid (g);
    drawNotes (g);
    drawRuler (g);
    drawKeyboard (g);
    drawVelocities (g);

    // Playhead.
    const float px = xForTick ((double) playheadTick());
    if (px >= (float) keyboardWidth && px < (float) getWidth())
    {
        g.setColour (theme::accent);
        g.fillRect (juce::Rectangle<float> (px - 0.5f, (float) headerHeight, 1.0f, (float) (getHeight() - headerHeight)));
    }

    if (gesture == Gesture::rubber)
    {
        g.setColour (theme::accentDim);
        g.fillRect (rubber);
        g.setColour (theme::accent.withAlpha (0.7f));
        g.drawRect (rubber, 1);
    }
}

void PianoRoll::drawGrid (juce::Graphics& g)
{
    const auto area = gridArea();
    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (area);

    // Rows: black keys darker, a line under every B.
    for (int pitch = lowestPitch; pitch <= highestPitch; ++pitch)
    {
        const float y = yForPitch (pitch);
        if (y > (float) area.getBottom() || y + rowHeight < (float) area.getY())
            continue;
        g.setColour (isBlackKey (pitch) ? blackKeyRow : whiteKeyRow);
        g.fillRect (juce::Rectangle<float> ((float) area.getX(), y, (float) area.getWidth(), rowHeight));
        if (pitch % 12 == 0)
        {
            g.setColour (theme::gridBar.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> ((float) area.getX(), y + rowHeight - 1.0f, (float) area.getWidth(), 1.0f));
        }
    }

    // Columns: bars, beats and the snap grid when it's far enough apart.
    const auto& grid = studio.project().grid();
    const double origin = grid.tickAt ((double) daw::Project::contentOrigin (region));
    const double beat = grid.meterAt (origin + tickAtX ((float) area.getX())).ticksPerBeat();
    const double bar = beat * grid.beatsPerBar;
    const double step = gridTicks() * pixelsPerTick >= 6.0 ? gridTicks() : beat * pixelsPerTick >= 6.0 ? beat : bar;
    const double first = std::floor ((origin + tickAtX ((float) area.getX())) / step) * step - origin;
    for (double t = first; xForTick (t) < (float) area.getRight(); t += step)
    {
        const double song = t + origin;
        const bool isBar = std::abs (std::remainder (song, bar)) < 0.5;
        const bool isBeat = std::abs (std::remainder (song, beat)) < 0.5;
        g.setColour (isBar ? theme::gridBar : isBeat ? theme::gridBar.withAlpha (0.55f) : theme::gridBeat);
        g.fillRect (juce::Rectangle<float> (xForTick (t), (float) area.getY(), 1.0f, (float) area.getHeight()));
    }

    // Outside the region: shaded, since those notes don't play.
    const auto originTick = grid.tickAt ((double) daw::Project::contentOrigin (region));
    const auto startTick = grid.tickAt ((double) (juce::int64) region[ids::start]) - originTick;
    const auto lengthTicks = grid.tickAt ((double) ((juce::int64) region[ids::start] + (juce::int64) region[ids::length])) - originTick - startTick;
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    const float rx1 = xForTick (startTick), rx2 = xForTick (startTick + lengthTicks);
    if (rx1 > (float) area.getX())
        g.fillRect (juce::Rectangle<float> ((float) area.getX(), (float) area.getY(), rx1 - (float) area.getX(), (float) area.getHeight()));
    if (rx2 < (float) area.getRight())
        g.fillRect (juce::Rectangle<float> (rx2, (float) area.getY(), (float) area.getRight() - rx2, (float) area.getHeight()));
}

void PianoRoll::drawNotes (juce::Graphics& g)
{
    const auto area = gridArea();
    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (area);

    const auto colour = arrange::trackColour (region.getParent());
    const auto& grid = studio.project().grid();
    const auto originTick = grid.tickAt ((double) daw::Project::contentOrigin (region));
    const auto startTick = grid.tickAt ((double) (juce::int64) region[ids::start]) - originTick;
    const auto endTick = grid.tickAt ((double) ((juce::int64) region[ids::start] + (juce::int64) region[ids::length])) - originTick;

    for (auto n : notes())
    {
        const auto note = daw::Project::noteFrom (n);
        const auto r = noteRect (note);
        if (r.getRight() < (float) area.getX() || r.getX() > (float) area.getRight()
            || r.getBottom() < (float) area.getY() || r.getY() > (float) area.getBottom())
            continue;

        const bool selected = isSelected (n);
        const bool outside = (double) note.start < startTick - 0.5 || (double) note.start >= endTick;
        // Brighter for louder notes, as in Logic.
        const float v = (float) note.velocity / 127.0f;
        auto fill = colour.withMultipliedBrightness (0.55f + 0.6f * v).withMultipliedSaturation (0.7f + 0.3f * v);
        if (selected)
            fill = fill.interpolatedWith (juce::Colours::white, 0.45f);
        if (outside)
            fill = fill.withAlpha (0.35f);

        g.setColour (fill);
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);

        if (r.getWidth() > 30.0f && rowHeight >= 10.0f)
        {
            g.setColour (fill.getPerceivedBrightness() > 0.6f ? juce::Colour (0xff141416) : juce::Colours::white.withAlpha (0.85f));
            g.setFont (theme::font (juce::jmin (10.0f, rowHeight - 2.0f), theme::Weight::medium));
            g.drawText (pitchName (note.pitch), r.reduced (3.0f, 0.0f), juce::Justification::centredLeft, false);
        }
    }
}

void PianoRoll::drawRuler (juce::Graphics& g)
{
    const auto area = rulerArea();
    g.setColour (theme::panel);
    g.fillRect (area);
    g.setColour (theme::separator);
    g.fillRect (area.withTop (area.getBottom() - 1));

    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (area);
    const auto& grid = studio.project().grid();
    const double origin = grid.tickAt ((double) daw::Project::contentOrigin (region));
    const double bar = grid.meterAt (origin + tickAtX ((float) area.getX())).ticksPerBar();
    const double first = std::floor ((origin + tickAtX ((float) area.getX())) / bar) * bar;
    const int every = juce::jmax (1, (int) std::ceil (40.0 / (bar * pixelsPerTick)));
    g.setFont (theme::readout (10.0f, theme::Weight::medium));
    for (double song = first; xForTick (song - origin) < (float) area.getRight(); song += bar)
    {
        const int barNumber = (int) std::llround (song / bar) + 1;
        const float x = xForTick (song - origin);
        g.setColour (theme::gridBar);
        g.fillRect (juce::Rectangle<float> (x, (float) area.getY() + 10.0f, 1.0f, (float) area.getHeight() - 10.0f));
        if ((barNumber - 1) % every == 0)
        {
            g.setColour (theme::dimText);
            g.drawText (juce::String (barNumber), juce::Rectangle<float> (x + 3.0f, (float) area.getY(), 40.0f, (float) area.getHeight()),
                        juce::Justification::centredLeft, false);
        }
    }
}

void PianoRoll::drawKeyboard (juce::Graphics& g)
{
    const auto area = keyboardArea();
    g.setColour (theme::panel);
    g.fillRect (area.withTop (headerHeight));

    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (area);
    for (int pitch = lowestPitch; pitch <= highestPitch; ++pitch)
    {
        const float y = yForPitch (pitch);
        if (y > (float) area.getBottom() || y + rowHeight < (float) area.getY())
            continue;
        const bool black = isBlackKey (pitch);
        const bool down = pitch == keyboardPitch;
        auto key = juce::Rectangle<float> ((float) area.getX(), y, (float) area.getWidth() - 1.0f, rowHeight);
        if (black)
            key = key.withWidth (key.getWidth() * 0.62f);
        g.setColour (down ? theme::accent : black ? juce::Colour (0xff202024) : juce::Colour (0xffd8d8dc));
        g.fillRect (key.reduced (0.0f, 0.5f));
        if (pitch % 12 == 0)
        {
            g.setColour (down ? juce::Colours::white : juce::Colour (0xff55555c));
            g.setFont (theme::font (juce::jmin (10.0f, rowHeight - 1.0f), theme::Weight::semibold));
            g.drawText (pitchName (pitch), key.withTrimmedRight (4.0f), juce::Justification::centredRight, false);
        }
    }
    g.setColour (theme::separator);
    g.fillRect (area.getRight() - 1, area.getY(), 1, area.getHeight());
}

void PianoRoll::drawVelocities (juce::Graphics& g)
{
    const auto area = velocityArea();
    g.setColour (theme::panel);
    g.fillRect (area);
    g.setColour (theme::separator);
    g.fillRect (area.withHeight (1));
    g.setColour (theme::faintText);
    g.setFont (theme::font (10.0f, theme::Weight::medium));
    g.drawText ("VEL", juce::Rectangle<int> (0, area.getY(), keyboardWidth - 6, area.getHeight()), juce::Justification::centredRight);

    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (area);
    const auto colour = arrange::trackColour (region.getParent());
    const float top = (float) area.getY() + 6.0f, height = (float) area.getHeight() - 10.0f;
    for (auto n : notes())
    {
        const auto note = daw::Project::noteFrom (n);
        const float x = xForTick ((double) note.start);
        if (x < (float) area.getX() - 4.0f || x > (float) area.getRight())
            continue;
        const float h = height * (float) note.velocity / 127.0f;
        g.setColour (isSelected (n) ? juce::Colours::white : colour.withAlpha (0.85f));
        g.fillRect (juce::Rectangle<float> (x, top + height - h, 2.0f, h));
        g.fillEllipse (juce::Rectangle<float> (x - 2.0f, top + height - h - 2.5f, 6.0f, 6.0f));
    }
}

//==============================================================================
void PianoRoll::timerCallback()
{
    if (! region.isValid() || ! isShowing())
        return;
    const float px = xForTick ((double) playheadTick());
    if (std::abs (px - lastPlayheadX) < 0.5f)
        return;

    // Follow the playhead while playing, a page at a time.
    const auto area = gridArea();
    if (studio.engine().isPlaying() && gesture == Gesture::none && (px > (float) area.getRight() - 10.0f || px < (float) area.getX()))
    {
        viewStartTick = (double) playheadTick() - 20.0 / pixelsPerTick;
        repaint();
    }
    else
    {
        const int y = headerHeight;
        repaint (juce::Rectangle<int> ((int) lastPlayheadX - 2, y, 5, getHeight() - y));
        repaint (juce::Rectangle<int> ((int) px - 2, y, 5, getHeight() - y));
    }
    lastPlayheadX = px;
}

//==============================================================================
juce::ValueTree PianoRoll::addNoteAt (juce::Point<int> p)
{
    auto& project = studio.project();
    daw::Note note;
    note.pitch = pitchAtY ((float) p.y);
    // Snap to the cell the click is in (not the nearest line).
    {
        const auto grid = project.grid();
        const double origin = grid.tickAt ((double) daw::Project::contentOrigin (region));
        const double step = gridTicks();
        note.start = juce::jmax ((juce::int64) 0, (juce::int64) std::llround (std::floor ((origin + tickAtX ((float) p.x)) / step) * step - origin));
    }
    note.length = juce::jmax ((juce::int64) 1, defaultLength);
    note.velocity = selection.isEmpty() ? (int) velocitySlider.getValue() : daw::Project::noteFrom (selection.getFirst()).velocity;

    project.undo.beginNewTransaction ("Add Note");
    auto n = project.addNote (region, note);
    selection = { n };
    studio.auditionNote (note.pitch, note.velocity);
    return n;
}

void PianoRoll::deleteSelection()
{
    pruneSelection();
    if (selection.isEmpty())
        return;
    auto& project = studio.project();
    project.undo.beginNewTransaction ("Delete Notes");
    for (auto& n : selection)
        region.removeChild (n, &project.undo);
    selection.clear();
}

void PianoRoll::moveSelection (juce::int64 deltaTicks, int deltaPitch, const juce::String& undoName)
{
    pruneSelection();
    if (selection.isEmpty())
        return;
    auto& project = studio.project();
    project.undo.beginNewTransaction (undoName);
    for (auto& n : selection)
    {
        auto note = daw::Project::noteFrom (n);
        note.start = juce::jmax ((juce::int64) 0, note.start + deltaTicks);
        note.pitch = juce::jlimit (0, 127, note.pitch + deltaPitch);
        project.setNote (n, note);
    }
    if (deltaPitch != 0 && selection.size() == 1)
        studio.auditionNote (daw::Project::noteFrom (selection.getFirst()).pitch);
}

void PianoRoll::quantizeSelection()
{
    pruneSelection();
    if (! region.isValid())
        return;
    studio.quantizeNotes (region, selection, gridTicks());
}

void PianoRoll::setVelocityAt (juce::Point<int> p, bool start)
{
    const auto area = velocityArea();
    const float height = (float) area.getHeight() - 10.0f;
    const int velocity = juce::jlimit (1, 127, juce::roundToInt (127.0f * ((float) area.getBottom() - 4.0f - (float) p.y) / height));

    // The note nearest the mouse in time; if it's selected, every selected note follows.
    if (start)
    {
        grabbed = {};
        float best = 8.0f;
        for (auto n : notes())
        {
            const float d = std::abs (xForTick ((double) daw::Project::noteFrom (n).start) - (float) p.x);
            if (d < best) { best = d; grabbed = n; }
        }
        if (! grabbed.isValid())
            return;
        studio.project().undo.beginNewTransaction ("Note Velocity");
    }
    if (! grabbed.isValid())
        return;

    auto targets = isSelected (grabbed) ? selection : juce::Array<juce::ValueTree> { grabbed };
    for (auto& n : targets)
    {
        auto note = daw::Project::noteFrom (n);
        note.velocity = velocity;
        studio.project().setNote (n, note);
    }
    velocitySlider.setValue (velocity, juce::dontSendNotification);
}

//==============================================================================
void PianoRoll::mouseMove (const juce::MouseEvent& e)
{
    const auto hit = noteAt (e.getPosition());
    setMouseCursor (hit.part == Part::leftEdge || hit.part == Part::rightEdge ? juce::MouseCursor::LeftRightResizeCursor
                    : hit.part == Part::body ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

void PianoRoll::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    gesture = Gesture::none;
    if (! region.isValid())
        return;
    pruneSelection();

    const auto p = e.getPosition();
    downTick = tickAtX ((float) p.x);
    downPitch = pitchAtY ((float) p.y);
    lastAuditionPitch = downPitch;

    if (keyboardArea().contains (p))
    {
        gesture = Gesture::keyboard;
        keyboardPitch = downPitch;
        studio.auditionNote (downPitch, juce::jlimit (1, 127, 40 + (int) (87.0f * (float) p.x / (float) keyboardWidth)));
        repaint (keyboardArea());
        return;
    }

    if (velocityArea().contains (p))
    {
        gesture = Gesture::velocity;
        setVelocityAt (p, true);
        return;
    }

    if (! gridArea().contains (p))
        return;

    auto hit = noteAt (p);

    // Cmd-click on empty grid: the pencil.
    if (! hit.note.isValid() && e.mods.isCommandDown())
    {
        hit.note = addNoteAt (p);
        hit.part = Part::rightEdge;
    }

    if (! hit.note.isValid())
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem ("Add Note Here", [this, p] { addNoteAt (p); });
            m.addItem ("Select All", [this] { selection = notes(); repaint(); });
            m.addItem ("Quantize All", [this] { selection.clear(); quantizeSelection(); });
            m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
            return;
        }
        gesture = Gesture::rubber;
        selectionAtDown = e.mods.isShiftDown() ? selection : juce::Array<juce::ValueTree>();
        if (! e.mods.isShiftDown())
            selection.clear();
        rubber = {};
        repaint();
        return;
    }

    if (e.mods.isShiftDown())
    {
        if (isSelected (hit.note)) selection.removeFirstMatchingValue (hit.note);
        else                       selection.add (hit.note);
        repaint();
        return;
    }
    if (! isSelected (hit.note))
        selection = { hit.note };

    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addItem ("Delete", [this] { deleteSelection(); });
        m.addItem ("Quantize", [this] { quantizeSelection(); });
        m.addItem ("Transpose Up an Octave", [this] { moveSelection (0, 12, "Transpose"); });
        m.addItem ("Transpose Down an Octave", [this] { moveSelection (0, -12, "Transpose"); });
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
        return;
    }

    grabbed = hit.note;
    velocitySlider.setValue (daw::Project::noteFrom (grabbed).velocity, juce::dontSendNotification);
    gesture = hit.part == Part::rightEdge ? Gesture::resizeRight : hit.part == Part::leftEdge ? Gesture::resizeLeft : Gesture::move;

    // Option-drag moves copies, leaving the originals where they were.
    auto& project = studio.project();
    project.undo.beginNewTransaction (gesture == Gesture::move ? (e.mods.isAltDown() ? "Copy Notes" : "Move Notes") : "Note Length");
    if (gesture == Gesture::move && e.mods.isAltDown())
    {
        juce::Array<juce::ValueTree> copies;
        for (auto& n : selection)
        {
            auto c = project.addNote (region, daw::Project::noteFrom (n));
            if (n == grabbed)
                grabbed = c;
            copies.add (c);
        }
        selection = copies;
    }

    originals.clear();
    for (auto& n : selection)
        originals.push_back ({ n, daw::Project::noteFrom (n) });

    if (gesture == Gesture::move)
        studio.auditionNote (daw::Project::noteFrom (grabbed).pitch, daw::Project::noteFrom (grabbed).velocity);
    repaint();
}

void PianoRoll::mouseDrag (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    const bool snapOn = ! e.mods.isCommandDown();
    auto& project = studio.project();

    switch (gesture)
    {
        case Gesture::keyboard:
        {
            const int pitch = pitchAtY ((float) p.y);
            if (pitch != keyboardPitch)
            {
                keyboardPitch = pitch;
                studio.auditionNote (pitch);
                repaint (keyboardArea());
            }
            return;
        }
        case Gesture::velocity:
            setVelocityAt (p, false);
            return;
        case Gesture::rubber:
        {
            rubber = juce::Rectangle<int> (e.getMouseDownPosition(), p).getIntersection (gridArea());
            selection = selectionAtDown;
            for (auto n : notes())
                if (noteRect (daw::Project::noteFrom (n)).intersects (rubber.toFloat()))
                    selection.addIfNotAlreadyThere (n);
            repaint();
            return;
        }
        case Gesture::move:
        {
            if (e.getDistanceFromDragStart() < 3 && ! e.mods.isAltDown())
                return;
            const auto grabbedStart = std::find_if (originals.begin(), originals.end(), [this] (auto& o) { return o.note == grabbed; });
            if (grabbedStart == originals.end())
                return;
            // Snap the grabbed note's new start; everything else moves by the same amount.
            const double wanted = (double) grabbedStart->value.start + (tickAtX ((float) p.x) - downTick);
            auto delta = (juce::int64) std::llround (snapTicks (wanted, snapOn)) - grabbedStart->value.start;
            juce::int64 earliest = std::numeric_limits<juce::int64>::max();
            int lowest = 127, highest = 0;
            for (auto& o : originals)
            {
                earliest = juce::jmin (earliest, o.value.start);
                lowest = juce::jmin (lowest, o.value.pitch);
                highest = juce::jmax (highest, o.value.pitch);
            }
            delta = juce::jmax (delta, -earliest);
            const int pitchDelta = juce::jlimit (-lowest, 127 - highest, pitchAtY ((float) p.y) - downPitch);
            for (auto& o : originals)
            {
                auto note = o.value;
                note.start += delta;
                note.pitch += pitchDelta;
                project.setNote (o.note, note);
            }
            const int nowPitch = grabbedStart->value.pitch + pitchDelta;
            if (nowPitch != lastAuditionPitch)
            {
                lastAuditionPitch = nowPitch;
                studio.auditionNote (nowPitch, grabbedStart->value.velocity);
            }
            return;
        }
        case Gesture::resizeRight:
        case Gesture::resizeLeft:
        {
            const auto grabbedOriginal = std::find_if (originals.begin(), originals.end(), [this] (auto& o) { return o.note == grabbed; });
            if (grabbedOriginal == originals.end())
                return;
            const auto& g0 = grabbedOriginal->value;
            const auto minimum = (juce::int64) juce::jmax (1, snapOn ? gridTicks() / 4 : 1);
            if (gesture == Gesture::resizeRight)
            {
                const auto end = (juce::int64) std::llround (snapTicks ((double) (g0.start + g0.length) + tickAtX ((float) p.x) - downTick, snapOn));
                const auto delta = end - (g0.start + g0.length);
                for (auto& o : originals)
                {
                    auto note = o.value;
                    note.length = juce::jmax (minimum, note.length + delta);
                    project.setNote (o.note, note);
                }
                defaultLength = juce::jmax (minimum, g0.length + delta);
            }
            else
            {
                const auto start = (juce::int64) std::llround (snapTicks ((double) g0.start + tickAtX ((float) p.x) - downTick, snapOn));
                const auto delta = start - g0.start;
                for (auto& o : originals)
                {
                    auto note = o.value;
                    const auto end = note.start + note.length;
                    note.start = juce::jlimit ((juce::int64) 0, end - minimum, note.start + delta);
                    note.length = end - note.start;
                    project.setNote (o.note, note);
                }
            }
            return;
        }
        case Gesture::none:
        default:
            return;
    }
}

void PianoRoll::mouseUp (const juce::MouseEvent&)
{
    if (gesture == Gesture::keyboard)
    {
        keyboardPitch = -1;
        repaint (keyboardArea());
    }
    gesture = Gesture::none;
    originals.clear();
    grabbed = {};
    repaint();
}

void PianoRoll::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! region.isValid() || ! gridArea().contains (e.getPosition()))
        return;
    if (auto hit = noteAt (e.getPosition()); hit.note.isValid())
    {
        // Double-clicking a note deletes it (a quick way to clean up), as many editors do.
        selection = { hit.note };
        deleteSelection();
        return;
    }
    addNoteAt (e.getPosition());
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown())
    {
        mouseMagnify (e, 1.0f + w.deltaY);
        return;
    }
    const auto area = gridArea();
    const float dx = (std::abs (w.deltaX) > 0.0f ? w.deltaX : (e.mods.isShiftDown() ? w.deltaY : 0.0f)) * 400.0f;
    const float dy = e.mods.isShiftDown() ? 0.0f : w.deltaY * 400.0f;
    viewStartTick -= (double) dx / pixelsPerTick;
    scrollY = juce::jlimit (0.0f, juce::jmax (0.0f, 128.0f * rowHeight - (float) area.getHeight()), scrollY - dy);
    repaint();
}

void PianoRoll::mouseMagnify (const juce::MouseEvent& e, float scale)
{
    const double anchor = tickAtX ((float) e.x);
    pixelsPerTick = juce::jlimit (0.002, 2.0, pixelsPerTick * (double) scale);
    viewStartTick = anchor - (double) (e.x - keyboardWidth) / pixelsPerTick;
    repaint();
}

bool PianoRoll::keyPressed (const juce::KeyPress& key)
{
    if (! region.isValid())
        return false;

    const auto mods = key.getModifiers();
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::backspaceKey || code == juce::KeyPress::deleteKey)
    {
        if (selection.isEmpty())
            return false;
        deleteSelection();
        return true;
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        const int step = mods.isShiftDown() ? 12 : 1;
        moveSelection (0, code == juce::KeyPress::upKey ? step : -step, "Transpose");
        return ! selection.isEmpty();
    }
    if ((code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && ! mods.isAltDown() && ! mods.isCommandDown())
    {
        if (selection.isEmpty())
            return false;
        moveSelection (code == juce::KeyPress::leftKey ? -gridTicks() : gridTicks(), 0, "Move Notes");
        return true;
    }
    if (key == juce::KeyPress ('q') || key == juce::KeyPress ('Q'))
    {
        quantizeSelection();
        return true;
    }
    if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0))
    {
        selection = notes();
        repaint();
        return true;
    }
    if (key == juce::KeyPress (juce::KeyPress::escapeKey))
    {
        selection.clear();
        repaint();
        return true;
    }
    return false;
}

} // namespace woodshed::app
