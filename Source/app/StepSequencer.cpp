#include "StepSequencer.h"
#include "ArrangeHeader.h"

namespace woodshed::app
{

namespace ids = daw::ids;
using daw::StepPattern;

namespace
{
void makeQuiet (juce::Component& c)
{
    // Space must keep starting playback after a click in the pane, not press the control again.
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

struct LengthChoice { const char* label; int ticks; };
const LengthChoice lengths[] { { "1/8", 480 }, { "1/16", 240 }, { "1/32", 120 }, { "1/8 T", 320 }, { "1/16 T", 160 } };
const int counts[] { 4, 6, 8, 12, 16, 24, 32, 48, 64 };
const int rowCounts[] { 24, 15, 15, 11 };     // chromatic, major, minor, pentatonic: two octaves or so

constexpr float chipWidth = 20.0f, chipHeight = 15.0f;

/** The mute and solo chips at the right of a row's header. */
juce::Rectangle<float> chipRect (juce::Rectangle<float> row, int which)
{
    return juce::Rectangle<float> (chipWidth, juce::jmin (chipHeight, row.getHeight() - 3.0f))
               .withCentre ({ row.getRight() - 10.0f - chipWidth * 0.5f - (float) (1 - which) * (chipWidth + 4.0f), row.getCentreY() });
}
}

StepSequencer::StepSequencer (Studio& s) : studio (s)
{
    setOpaque (true);

    for (int i = 0; i < (int) std::size (lengths); ++i)
        stepLength.addItem (juce::String ("Step ") + lengths[i].label, i + 1);
    stepLength.setTooltip ("How long each step is");
    stepLength.onChange = [this]
    {
        const int i = stepLength.getSelectedId() - 1;
        if (! juce::isPositiveAndBelow (i, (int) std::size (lengths)))
            return;
        pattern.stepTicks = lengths[i].ticks;
        edited ("Step Length");
    };

    for (int n : counts)
        stepCount.addItem (juce::String (n) + " steps", n);
    stepCount.setTooltip ("How many steps the pattern has before it repeats");
    stepCount.onChange = [this]
    {
        if (stepCount.getSelectedId() <= 0)
            return;
        pattern.setNumSteps (stepCount.getSelectedId());
        edited ("Pattern Length");
    };

    const auto scales = StepPattern::scaleNames();
    for (int i = 0; i < scales.size(); ++i)
        rowSet.addItem (scales[i], i + 1);
    rowSet.setTextWhenNothingSelected ("Rows");
    rowSet.setTooltip ("The notes the rows play, from the lowest row's octave up");
    rowSet.onChange = [this]
    {
        const int i = rowSet.getSelectedId() - 1;
        if (i < 0 || pattern.rows.empty())
            return;
        const int lowest = pattern.rows.back().pitch - pattern.rows.back().pitch % 12;
        pattern.setRows (StepPattern::scaleRows ((StepPattern::Scale) i, lowest, rowCounts[i]));
        edited ("Step Rows");
    };

    auto shift = [this] (int semitones)
    {
        if (pattern.rows.empty())
            return;
        for (auto& r : pattern.rows)
            if (r.pitch + semitones < 0 || r.pitch + semitones > 127)
                return;
        for (auto& r : pattern.rows)
            r.pitch += semitones;
        edited ("Transpose Rows");
    };
    octaveDown.onClick = [shift] { shift (-12); };
    octaveUp.onClick = [shift] { shift (12); };
    octaveDown.setTooltip ("Rows an octave down");
    octaveUp.setTooltip ("Rows an octave up");

    clearButton.setTooltip ("Turn every step off");
    clearButton.onClick = [this]
    {
        pattern.clear();
        edited ("Clear Pattern");
    };
    randomButton.setTooltip ("A new random pattern: a groove on drums, a phrase in the rows' scale otherwise");
    randomButton.onClick = [this]
    {
        seed = seed * 1664525u + 1013904223u;
        pattern.randomize (seed, drums(), 0.5f);
        edited ("Randomize Pattern");
    };

    for (auto* c : std::initializer_list<juce::Component*> { &stepLength, &stepCount, &rowSet, &octaveDown, &octaveUp, &clearButton, &randomButton })
    {
        makeQuiet (*c);
        addAndMakeVisible (*c);
    }

    seed = (juce::uint32) juce::Random::getSystemRandom().nextInt (1 << 30) + 1u;
    studio.addChangeListener (this);
    studio.addProjectSwapListener (this);
    sync();
    startTimerHz (30);
}

StepSequencer::~StepSequencer()
{
    watched.removeListener (this);
    studio.removeProjectSwapListener (this);
    studio.removeChangeListener (this);
}

//==============================================================================
void StepSequencer::projectAboutToBeReplaced()
{
    watched.removeListener (this);
    watched = {};
    region = {};
    snapshot.clear();
    previewPending = false;
    gesture = Gesture::none;
}

void StepSequencer::changeListenerCallback (juce::ChangeBroadcaster*)
{
    sync();
}

void StepSequencer::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree == region && ! writing && ! snapshot.isActive() && (property == StepPattern::property || property == ids::name))
    {
        loadPattern();
        repaint();
    }
}

void StepSequencer::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    // A region that isn't a pattern yet shows its notes as steps, so follow them.
    if (parent == region && ! writing && ! snapshot.isActive() && ! region.hasProperty (StepPattern::property))
    {
        loadPattern();
        repaint();
    }
}

void StepSequencer::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (child == region || child == region.getParent())
        sync();
    else
        valueTreeChildAdded (parent, child);
}

void StepSequencer::sync()
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
        if (snapshot.isActive())
            commit (gestureName);
        gesture = Gesture::none;
        region = next;
        selectedRow = 0;
        firstRow = 0;
        loadPattern();
    }
    showControls();
    repaint();
}

bool StepSequencer::drums() const
{
    return Studio::isDrumTrack (region.getParent());
}

void StepSequencer::loadPattern()
{
    if (! region.isValid())
    {
        pattern = {};
        return;
    }

    if (! StepPattern::fromJson (region[StepPattern::property].toString(), pattern))
    {
        // Not a pattern yet: read the notes onto a grid of 16 sixteenths.
        const auto notes = daw::regionNotes (region);
        std::vector<int> rows;
        if (drums())
        {
            rows = StepPattern::drumRows();
            for (auto& n : notes)
                if (std::find (rows.begin(), rows.end(), n.pitch) == rows.end())
                    rows.push_back (n.pitch);
        }
        else if (notes.empty())
            rows = StepPattern::scaleRows (StepPattern::Scale::major, 48, rowCounts[1]);
        else
        {
            int low = 127, high = 0;
            for (auto& n : notes)
            {
                low = juce::jmin (low, n.pitch);
                high = juce::jmax (high, n.pitch);
            }
            rows = StepPattern::scaleRows (StepPattern::Scale::chromatic, low, juce::jmax (12, high - low + 1));
        }
        pattern = StepPattern::fromNotes (notes, rows, daw::ticksPerQuarter / 4, 16);
    }

    selectedRow = juce::jlimit (0, juce::jmax (0, (int) pattern.rows.size() - 1), selectedRow);
    firstRow = juce::jlimit (0, juce::jmax (0, (int) pattern.rows.size() - visibleRows()), firstRow);
    showControls();
}

void StepSequencer::showControls()
{
    const bool on = region.isValid();
    for (auto* c : std::initializer_list<juce::Component*> { &stepLength, &stepCount, &clearButton, &randomButton })
        c->setVisible (on);
    for (auto* c : std::initializer_list<juce::Component*> { &rowSet, &octaveDown, &octaveUp })
        c->setVisible (on && ! drums());

    int lengthId = 0;
    for (int i = 0; i < (int) std::size (lengths); ++i)
        if (lengths[i].ticks == pattern.stepTicks)
            lengthId = i + 1;
    stepLength.setSelectedId (lengthId, juce::dontSendNotification);
    if (stepCount.indexOfItemId (pattern.numSteps) < 0)
        stepCount.addItem (juce::String (pattern.numSteps) + " steps", pattern.numSteps);
    stepCount.setSelectedId (pattern.numSteps, juce::dontSendNotification);
    rowSet.setSelectedId (0, juce::dontSendNotification);
}

//==============================================================================
void StepSequencer::edited (const juce::String& undoName)
{
    if (! region.isValid())
        return;
    if (gesture != Gesture::none)
    {
        // During a drag: preview without undo; mouseUp makes it one undo step.
        if (! snapshot.isActive())
            snapshot.take (region);
        gestureName = undoName;
        previewPending = true;
        repaint();
        return;
    }
    commit (undoName);
}

void StepSequencer::commit (const juce::String& undoName)
{
    if (! region.isValid())
        return;
    auto& project = studio.project();
    const juce::ScopedValueSetter<bool> guard (writing, true);
    if (snapshot.isActive())
    {
        snapshot.restore();
        snapshot.clear();
    }
    previewPending = false;
    project.undo.beginNewTransaction (undoName);
    // Written by the step sequencer, it's no longer the Drummer's to rewrite.
    if (region.hasProperty (daw::drummer::property))
        region.removeProperty (daw::drummer::property, &project.undo);
    pattern.applyTo (project, region, &project.undo);
    showControls();
    repaint();
}

void StepSequencer::timerCallback()
{
    if (previewPending && snapshot.isActive() && region.isValid())
    {
        const juce::ScopedValueSetter<bool> guard (writing, true);
        pattern.applyTo (studio.project(), region, nullptr);
        previewPending = false;
    }

    if (isShowing())
    {
        const int step = playingStep();
        if (step != lastPlayingStep)
        {
            lastPlayingStep = step;
            repaint (gridArea().withTop (headerHeight));
        }
    }
}

//==============================================================================
juce::Rectangle<int> StepSequencer::rowHeaderArea() const
{
    return { 0, headerHeight, rowHeaderWidth, juce::jmax (0, getHeight() - headerHeight - velocityHeight) };
}

juce::Rectangle<int> StepSequencer::gridArea() const
{
    return { rowHeaderWidth, headerHeight + 6, juce::jmax (0, getWidth() - rowHeaderWidth - 10),
             juce::jmax (0, getHeight() - headerHeight - velocityHeight - 6) };
}

juce::Rectangle<int> StepSequencer::velocityArea() const
{
    const auto g = gridArea();
    return { g.getX(), getHeight() - velocityHeight + 6, g.getWidth(), velocityHeight - 12 };
}

float StepSequencer::rowHeight() const
{
    const int rows = juce::jmax (1, (int) pattern.rows.size());
    return juce::jlimit (16.0f, 26.0f, (float) gridArea().getHeight() / (float) rows);
}

int StepSequencer::visibleRows() const
{
    return juce::jmax (1, (int) ((float) gridArea().getHeight() / rowHeight()));
}

float StepSequencer::stepWidth() const
{
    return (float) gridArea().getWidth() / (float) juce::jmax (1, pattern.numSteps);
}

int StepSequencer::rowAt (float y) const
{
    const int r = firstRow + (int) std::floor ((y - (float) gridArea().getY()) / rowHeight());
    return juce::isPositiveAndBelow (r, (int) pattern.rows.size()) && y >= (float) gridArea().getY() ? r : -1;
}

int StepSequencer::stepAt (float x) const
{
    const int s = (int) std::floor ((x - (float) gridArea().getX()) / stepWidth());
    return juce::isPositiveAndBelow (s, pattern.numSteps) && x >= (float) gridArea().getX() ? s : -1;
}

juce::Rectangle<float> StepSequencer::cell (int row, int step) const
{
    const auto g = gridArea().toFloat();
    return { g.getX() + (float) step * stepWidth(), g.getY() + (float) (row - firstRow) * rowHeight(), stepWidth(), rowHeight() };
}

int StepSequencer::playingStep() const
{
    auto& engine = studio.engine();
    if (! region.isValid() || ! engine.isPlaying() || pattern.lengthTicks() <= 0)
        return -1;
    const auto pos = engine.getPosition();
    const auto start = (juce::int64) region[ids::start];
    if (pos < start || pos >= start + (juce::int64) region[ids::length])
        return -1;
    const auto tick = (juce::int64) studio.project().grid().toTicks ((double) (pos - daw::Project::contentOrigin (region)));
    return (int) ((tick % pattern.lengthTicks()) / pattern.stepTicks);
}

//==============================================================================
void StepSequencer::resized()
{
    auto header = getLocalBounds().removeFromTop (headerHeight).reduced (12, 7);
    randomButton.setBounds (header.removeFromRight (96));
    header.removeFromRight (6);
    clearButton.setBounds (header.removeFromRight (64));
    header.removeFromRight (14);
    octaveUp.setBounds (header.removeFromRight (26));
    header.removeFromRight (4);
    octaveDown.setBounds (header.removeFromRight (26));
    header.removeFromRight (6);
    rowSet.setBounds (header.removeFromRight (116));
    header.removeFromRight (14);
    stepCount.setBounds (header.removeFromRight (102));
    header.removeFromRight (6);
    stepLength.setBounds (header.removeFromRight (104));

    firstRow = juce::jlimit (0, juce::jmax (0, (int) pattern.rows.size() - visibleRows()), firstRow);
}

void StepSequencer::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (theme::bar);
    g.fillRect (header);
    g.setColour (theme::highlight);
    g.fillRect (header.withHeight (1));
    g.setColour (theme::separator);
    g.fillRect (header.removeFromBottom (1));

    auto title = header.reduced (12, 0).removeFromLeft (juce::jmin (260, header.getWidth() / 4));
    g.setFont (theme::font (13.0f, theme::Weight::semibold));
    if (! region.isValid())
    {
        g.setColour (theme::dimText);
        g.drawText ("Step Sequencer", title, juce::Justification::centredLeft);
        g.setFont (theme::font (13.0f));
        g.drawText ("Select a MIDI region, or double-click an instrument track's lane to make one.",
                    getLocalBounds().withTrimmedTop (headerHeight), juce::Justification::centred);
        return;
    }

    const auto colour = arrange::trackColour (region.getParent());
    g.setColour (colour);
    g.fillEllipse (title.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat());
    title.removeFromLeft (8);
    g.setColour (theme::text);
    g.drawFittedText (region[ids::name].toString() + "  -  Step Sequencer", title, juce::Justification::centredLeft, 1, 0.8f);

    const auto grid = gridArea();
    const auto heads = rowHeaderArea();
    const float rh = rowHeight(), sw = stepWidth();
    const int playing = playingStep();

    // Beats alternate in shade so the steps read in groups.
    const int stepsPerBeat = juce::jmax (1, (int) std::lround ((double) daw::ticksPerQuarter / (double) pattern.stepTicks));
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (grid.withTop (heads.getY()).withBottom (grid.getBottom()));
        for (int s = 0; s < pattern.numSteps; ++s)
        {
            const auto col = cell (firstRow, s).withY ((float) grid.getY()).withHeight ((float) grid.getHeight());
            g.setColour ((s / stepsPerBeat) % 2 == 0 ? theme::lane : theme::laneAlt);
            g.fillRect (col);
            if (s % stepsPerBeat == 0)
            {
                g.setColour ((s / stepsPerBeat) % 4 == 0 ? theme::gridBar : theme::gridBeat);
                g.fillRect (col.withWidth (1.0f));
            }
            if (s == playing)
            {
                g.setColour (theme::accent.withAlpha (0.10f));
                g.fillRect (col);
            }
        }

        const int last = juce::jmin ((int) pattern.rows.size(), firstRow + visibleRows() + 1);
        for (int r = firstRow; r < last; ++r)
        {
            const bool audible = pattern.isAudible (r);
            if (r == selectedRow)
            {
                g.setColour (juce::Colours::white.withAlpha (0.025f));
                g.fillRect (cell (r, 0).withWidth ((float) grid.getWidth()));
            }
            for (int s = 0; s < pattern.numSteps; ++s)
            {
                const auto box = cell (r, s).reduced (juce::jmin (2.0f, sw * 0.12f), 2.0f);
                const int v = pattern.velocityAt (r, s);
                if (v <= 0)
                {
                    g.setColour (theme::control.withAlpha (0.45f));
                    g.fillRoundedRectangle (box, 3.0f);
                    continue;
                }
                const float amount = (float) v / 127.0f;
                auto fillColour = (s == playing ? theme::accent : colour).withMultipliedAlpha (audible ? 1.0f : 0.3f);
                g.setColour (fillColour.withMultipliedAlpha (0.35f + 0.65f * amount));
                g.fillRoundedRectangle (box, 3.0f);
                // The velocity as a bar along the bottom of the step.
                g.setColour (juce::Colours::white.withAlpha (audible ? 0.55f : 0.2f));
                g.fillRect (box.withTrimmedTop (box.getHeight() - 2.0f).withWidth (box.getWidth() * amount).reduced (1.0f, 0.0f));
            }
        }
    }

    // Row headers: name, mute and solo.
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (heads.withTop (grid.getY()));
        g.setColour (theme::panel);
        g.fillRect (heads);
        const bool isDrums = drums();
        const int last = juce::jmin ((int) pattern.rows.size(), firstRow + visibleRows() + 1);
        for (int r = firstRow; r < last; ++r)
        {
            const auto& row = pattern.rows[(size_t) r];
            const auto box = juce::Rectangle<float> (0.0f, cell (r, 0).getY(), (float) rowHeaderWidth, rh);
            if (r == selectedRow)
            {
                g.setColour (theme::panelLight);
                g.fillRect (box);
                g.setColour (theme::accent);
                g.fillRect (box.withWidth (2.0f));
            }
            g.setColour (pattern.isAudible (r) ? theme::text : theme::faintText);
            g.setFont (theme::font (juce::jmin (12.0f, rh * 0.6f), r == selectedRow ? theme::Weight::semibold : theme::Weight::regular));
            g.drawFittedText (StepPattern::rowName (row.pitch, isDrums), box.withTrimmedLeft (12.0f).withTrimmedRight (2 * chipWidth + 20.0f).toNearestInt(),
                              juce::Justification::centredLeft, 1, 0.8f);

            const std::pair<bool, juce::Colour> chips[] { { row.mute, theme::mute }, { row.solo, theme::solo } };
            for (int c = 0; c < 2; ++c)
            {
                const auto chip = chipRect (box, c);
                g.setColour (chips[c].first ? chips[c].second : theme::control);
                g.fillRoundedRectangle (chip, theme::radiusChip);
                g.setColour (chips[c].first ? theme::window : theme::dimText);
                g.setFont (theme::font (10.0f, theme::Weight::bold));
                g.drawText (c == 0 ? "M" : "S", chip, juce::Justification::centred);
            }
        }
        g.setColour (theme::separator);
        g.fillRect (heads.withX (heads.getRight() - 1).withWidth (1));
    }

    // The velocity lane for the selected row.
    const auto lane = velocityArea();
    g.setColour (theme::separator);
    g.fillRect (0, lane.getY() - 6, getWidth(), 1);
    if (juce::isPositiveAndBelow (selectedRow, (int) pattern.rows.size()))
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (11.0f, theme::Weight::semibold));
        g.drawText ("VELOCITY", juce::Rectangle<int> (12, lane.getY(), rowHeaderWidth - 16, 16), juce::Justification::topLeft);
        g.setColour (theme::faintText);
        g.setFont (theme::font (11.0f));
        g.drawText (StepPattern::rowName (pattern.rows[(size_t) selectedRow].pitch, drums()),
                    juce::Rectangle<int> (12, lane.getY() + 16, rowHeaderWidth - 16, 16), juce::Justification::topLeft);

        theme::drawInset (g, lane.toFloat(), theme::radiusChip);
        for (int s = 0; s < pattern.numSteps; ++s)
        {
            const int v = pattern.velocityAt (selectedRow, s);
            if (v <= 0)
                continue;
            const float x = (float) lane.getX() + (float) s * sw;
            const float h = (float) (lane.getHeight() - 4) * (float) v / 127.0f;
            g.setColour (colour.withMultipliedAlpha (0.85f));
            g.fillRoundedRectangle (juce::Rectangle<float> (x + juce::jmin (3.0f, sw * 0.2f), (float) lane.getBottom() - 2.0f - h,
                                                            juce::jmax (2.0f, sw - 2.0f * juce::jmin (3.0f, sw * 0.2f)), h), 2.0f);
        }
    }
}

//==============================================================================
void StepSequencer::mouseDown (const juce::MouseEvent& e)
{
    if (! region.isValid())
        return;
    const auto p = e.position;

    // Row headers: mute, solo, or select (and hear) the row.
    if (rowHeaderArea().withTop (gridArea().getY()).contains (e.getPosition()))
    {
        const int r = rowAt (p.y);
        if (r < 0)
            return;
        const auto box = juce::Rectangle<float> (0.0f, cell (r, 0).getY(), (float) rowHeaderWidth, rowHeight());
        auto& row = pattern.rows[(size_t) r];
        if (chipRect (box, 0).expanded (2.0f).contains (p))
        {
            row.mute = ! row.mute;
            edited (row.mute ? "Mute Row" : "Unmute Row");
        }
        else if (chipRect (box, 1).expanded (2.0f).contains (p))
        {
            row.solo = ! row.solo;
            edited (row.solo ? "Solo Row" : "Unsolo Row");
        }
        else
        {
            selectedRow = r;
            studio.auditionNote (row.pitch, 100);
            repaint();
        }
        return;
    }

    if (velocityArea().expanded (0, 4).contains (e.getPosition()))
    {
        gesture = Gesture::lane;
        mouseDrag (e);
        return;
    }

    const int r = rowAt (p.y), s = stepAt (p.x);
    if (r < 0 || s < 0)
        return;
    selectedRow = r;
    downRow = r;
    downStep = s;
    if (pattern.isOn (r, s))
    {
        gesture = Gesture::maybeOff;
        downVelocity = pattern.velocityAt (r, s);
        repaint();
    }
    else
    {
        gesture = Gesture::paintOn;
        pattern.set (r, s, lastVelocity);
        studio.auditionNote (pattern.rows[(size_t) r].pitch, lastVelocity);
        edited ("Add Steps");
    }
}

void StepSequencer::mouseDrag (const juce::MouseEvent& e)
{
    const auto p = e.position;
    switch (gesture)
    {
        case Gesture::lane:
        {
            const int s = stepAt (p.x);
            if (s >= 0 && pattern.isOn (selectedRow, s))
            {
                const auto lane = velocityArea().toFloat();
                const int v = juce::jlimit (1, 127, juce::roundToInt (127.0f * (lane.getBottom() - p.y) / lane.getHeight()));
                pattern.set (selectedRow, s, v);
                lastVelocity = v;
                edited ("Step Velocity");
            }
            return;
        }
        case Gesture::maybeOff:
        {
            const int dx = std::abs (e.getDistanceFromDragStartX()), dy = std::abs (e.getDistanceFromDragStartY());
            if (dy > 4 && dy > dx)
                gesture = Gesture::velocity;
            else if (rowAt (p.y) != downRow || stepAt (p.x) != downStep)
            {
                gesture = Gesture::paintOff;
                pattern.set (downRow, downStep, 0);
                edited ("Remove Steps");
            }
            else
                return;
            mouseDrag (e);
            return;
        }
        case Gesture::velocity:
        {
            const int v = juce::jlimit (1, 127, downVelocity - juce::roundToInt ((float) e.getDistanceFromDragStartY() * 0.75f));
            pattern.set (downRow, downStep, v);
            lastVelocity = v;
            edited ("Step Velocity");
            return;
        }
        case Gesture::paintOn:
        case Gesture::paintOff:
        {
            const int r = rowAt (p.y), s = stepAt (p.x);
            if (r < 0 || s < 0)
                return;
            const bool on = pattern.isOn (r, s);
            if (gesture == Gesture::paintOn && ! on)
            {
                pattern.set (r, s, lastVelocity);
                edited ("Add Steps");
            }
            else if (gesture == Gesture::paintOff && on)
            {
                pattern.set (r, s, 0);
                edited ("Remove Steps");
            }
            return;
        }
        case Gesture::none:
        default:
            return;
    }
}

void StepSequencer::mouseUp (const juce::MouseEvent&)
{
    const auto was = gesture;
    gesture = Gesture::none;
    if (was == Gesture::maybeOff)
    {
        // A click on an on step turns it off.
        pattern.set (downRow, downStep, 0);
        commit ("Remove Step");
    }
    else if (snapshot.isActive())
        commit (gestureName);
}

void StepSequencer::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int maxFirst = juce::jmax (0, (int) pattern.rows.size() - visibleRows());
    if (maxFirst == 0 || std::abs (wheel.deltaY) < 1.0e-4f)
    {
        Component::mouseWheelMove (e, wheel);
        return;
    }
    firstRow = juce::jlimit (0, maxFirst, firstRow + (wheel.deltaY < 0 ? 1 : -1));
    repaint();
}

} // namespace woodshed::app
