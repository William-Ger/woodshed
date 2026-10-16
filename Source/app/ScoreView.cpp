#include "ScoreView.h"
#include "ArrangeHeader.h"

namespace woodshed::app
{

namespace ids = daw::ids;

namespace
{
const juce::Identifier scoreKey { "scoreKey" }, scoreQuantize { "scoreQuantize" }, scoreStaff { "scoreStaff" },
                       scoreInterpret { "scoreInterpret" };

struct Resolution { const char* label; int ticks; };
const Resolution resolutions[] { { "1/4", 960 }, { "1/8", 480 }, { "1/16", 240 }, { "1/32", 120 }, { "1/8 T", 320 }, { "1/16 T", 160 } };

/** Key menu order: C, the sharp keys, then the flat keys. */
const int keyOrder[] { 0, 1, 2, 3, 4, 5, 6, 7, -1, -2, -3, -4, -5, -6, -7 };

juce::String keyLabel (int fifths)
{
    static const char* const minors[] { "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "D#", "A#" };
    return score::keyName (fifths) + " major / " + minors[fifths + 7] + " minor";
}

constexpr float baseStaffSpace = 9.0f;
}

ScoreView::ScoreView (Studio& s) : studio (s)
{
    setWantsKeyboardFocus (true);
    setOpaque (true);

    for (int i = 0; i < (int) std::size (resolutions); ++i)
        resolutionChoice.addItem (resolutions[i].label, i + 1);
    resolutionChoice.setTooltip ("Display quantize: how finely the score shows rhythm (the notes don't change)");
    resolutionChoice.onChange = [this]
    {
        const int i = juce::jlimit (0, (int) std::size (resolutions) - 1, resolutionChoice.getSelectedId() - 1);
        setRegionProperty (scoreQuantize, resolutions[i].ticks, "Score Display Quantize");
    };

    for (int i = 0; i < (int) std::size (keyOrder); ++i)
        keyChoice.addItem (keyLabel (keyOrder[i]), i + 1);
    keyChoice.setTooltip ("Key signature: sets how accidentals are spelled");
    keyChoice.onChange = [this]
    {
        const int i = juce::jlimit (0, (int) std::size (keyOrder) - 1, keyChoice.getSelectedId() - 1);
        setRegionProperty (scoreKey, keyOrder[i], "Score Key");
    };

    staffChoice.addItemList ({ "Auto Staff", "Treble", "Bass", "Grand Staff" }, 1);
    staffChoice.setTooltip ("Staff: chosen from the notes' range, or fixed");
    staffChoice.onChange = [this] { setRegionProperty (scoreStaff, juce::jmax (0, staffChoice.getSelectedId() - 1), "Score Staff"); };

    interpretButton.setClickingTogglesState (false);
    interpretButton.setTooltip ("Interpretation: show short notes lengthened up to the next note in the beat, instead of short rests");
    interpretButton.onClick = [this]
    {
        if (region.isValid())
            setRegionProperty (scoreInterpret, ! (bool) region.getProperty (scoreInterpret, true), "Score Interpretation");
    };
    interpretButton.setColour (juce::TextButton::buttonOnColourId, theme::accent.withAlpha (0.35f));

    zoomOutButton.setTooltip ("Zoom out (Cmd-scroll or pinch)");
    zoomInButton.setTooltip ("Zoom in (Cmd-scroll or pinch)");
    zoomOutButton.onClick = [this] { setZoom (zoom / 1.25f, (float) scoreArea().getCentreX()); };
    zoomInButton.onClick = [this] { setZoom (zoom * 1.25f, (float) scoreArea().getCentreX()); };

    for (juce::Component* c : std::initializer_list<juce::Component*> { &resolutionChoice, &keyChoice, &staffChoice, &interpretButton, &zoomOutButton, &zoomInButton })
    {
        c->setWantsKeyboardFocus (false);
        addAndMakeVisible (c);
    }

    studio.addChangeListener (this);
    studio.addProjectSwapListener (this);
    sync();
    startTimerHz (30);
}

ScoreView::~ScoreView()
{
    watched.removeListener (this);
    studio.removeProjectSwapListener (this);
    studio.removeChangeListener (this);
}

//==============================================================================
void ScoreView::projectAboutToBeReplaced()
{
    watched.removeListener (this);
    watched = {};
    region = {};
    selection.clear();
    noteTrees.clear();
    renderer.setLayout ({}, baseStaffSpace, 0.1);
}

void ScoreView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    sync();
}

void ScoreView::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (! region.isValid())
        return;
    if (tree == region || tree.getParent() == region || tree == watched || tree.getParent() == region.getParent())
    {
        rebuild();
        syncControls();
        repaint();
    }
}

void ScoreView::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent == region || parent == region.getParent())
    {
        rebuild();
        repaint();
    }
}

void ScoreView::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (parent == region)
    {
        selection.removeFirstMatchingValue (child);
        rebuild();
        repaint();
    }
    else if (child == region || child == region.getParent())
        sync();
}

void ScoreView::sync()
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
            scrollX = 0.0f;
            scrollY = 0.0f;
        }
    }
    rebuild();
    syncControls();
    repaint();
}

void ScoreView::syncControls()
{
    const bool on = region.isValid();
    for (juce::Component* c : std::initializer_list<juce::Component*> { &resolutionChoice, &keyChoice, &staffChoice, &interpretButton })
        c->setEnabled (on);

    const int res = resolution();
    for (int i = 0; i < (int) std::size (resolutions); ++i)
        if (resolutions[i].ticks == res)
            resolutionChoice.setSelectedId (i + 1, juce::dontSendNotification);
    const int key = keyFifths();
    for (int i = 0; i < (int) std::size (keyOrder); ++i)
        if (keyOrder[i] == key)
            keyChoice.setSelectedId (i + 1, juce::dontSendNotification);
    staffChoice.setSelectedId (juce::jlimit (0, 3, on ? (int) region.getProperty (scoreStaff, 0) : 0) + 1, juce::dontSendNotification);
    keyChoice.setEnabled (on && ! isDrumTrack());
    staffChoice.setEnabled (on && ! isDrumTrack());
    interpretButton.setToggleState (on && (bool) region.getProperty (scoreInterpret, true), juce::dontSendNotification);
}

bool ScoreView::isDrumTrack() const
{
    const auto track = region.getParent();
    return track.isValid() && track.getChildWithName (ids::INSTRUMENT)[ids::type].toString().startsWith ("drums");
}

int ScoreView::resolution() const
{
    const int r = region.isValid() ? (int) region.getProperty (scoreQuantize, 240) : 240;
    return r > 0 ? r : 240;
}

int ScoreView::keyFifths() const
{
    return region.isValid() ? juce::jlimit (-7, 7, (int) region.getProperty (scoreKey, 0)) : 0;
}

void ScoreView::setRegionProperty (const juce::Identifier& id, const juce::var& value, const juce::String& undoName)
{
    if (! region.isValid() || region[id] == value)
        return;
    auto& p = studio.project();
    p.undo.beginNewTransaction (undoName);
    region.setProperty (id, value, &p.undo);
}

double ScoreView::originTicks() const
{
    return region.isValid() ? studio.project().grid().tickAt ((double) daw::Project::contentOrigin (region)) : 0.0;
}

double ScoreView::playheadTick() const
{
    return studio.project().grid().tickAt ((double) studio.engine().getPosition());
}

void ScoreView::rebuild()
{
    noteTrees.clear();
    if (! region.isValid())
    {
        renderer.setLayout ({}, baseStaffSpace, 0.1);
        return;
    }

    const auto grid = studio.project().grid();
    const auto origin = (juce::int64) std::llround (originTicks());
    std::vector<score::InputNote> input;
    for (auto n : region)
        if (n.hasType (ids::NOTE))
        {
            const auto note = daw::Project::noteFrom (n);
            input.push_back ({ note.pitch, origin + note.start, note.length, (int) noteTrees.size() });
            noteTrees.push_back (n);
        }

    score::Options o;
    o.beatsPerBar = grid.beatsPerBar;
    o.beatUnit = grid.beatUnit;
    o.resolution = resolution();
    o.keyFifths = keyFifths();
    o.percussion = isDrumTrack();
    o.staff = (score::StaffMode) juce::jlimit (0, 3, (int) region.getProperty (scoreStaff, 0));
    o.interpret = region.getProperty (scoreInterpret, true);
    const auto start = (juce::int64) region[ids::start];
    o.from = (juce::int64) std::llround (grid.tickAt ((double) start));
    o.to = juce::jmax (o.from + 1, (juce::int64) std::llround (grid.tickAt ((double) (start + (juce::int64) region[ids::length]))));

    const float sp = baseStaffSpace * zoom;
    const double ppt = juce::jlimit (0.05, 0.5, 2.9 * (double) sp / (double) juce::jmin (o.resolution, 480));
    renderer.setLayout (score::layout (input, o), sp, ppt);
    clampScroll();
}

//==============================================================================
juce::Rectangle<int> ScoreView::scoreArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight);
}

float ScoreView::offsetY() const
{
    const auto area = scoreArea();
    const float spare = (float) area.getHeight() - renderer.getHeight();
    return (float) area.getY() + (spare > 0.0f ? spare * 0.5f : -scrollY);
}

juce::Point<float> ScoreView::toScore (juce::Point<float> local) const
{
    return { local.x - (float) scoreArea().getX() + scrollX, local.y - offsetY() };
}

float ScoreView::toLocalX (float scoreX) const
{
    return scoreX - scrollX + (float) scoreArea().getX();
}

void ScoreView::clampScroll()
{
    const auto area = scoreArea();
    scrollX = juce::jlimit (0.0f, juce::jmax (0.0f, renderer.getWidth() - (float) area.getWidth()), scrollX);
    scrollY = juce::jlimit (0.0f, juce::jmax (0.0f, renderer.getHeight() - (float) area.getHeight()), scrollY);
}

void ScoreView::setZoom (float newZoom, float anchorX)
{
    newZoom = juce::jlimit (0.6f, 2.6f, newZoom);
    if (juce::approximatelyEqual (newZoom, zoom))
        return;
    const double anchorTick = renderer.tickAtX (toScore ({ anchorX, 0.0f }).x);
    zoom = newZoom;
    rebuild();
    scrollX = renderer.xForTick (anchorTick) - (anchorX - (float) scoreArea().getX());
    clampScroll();
    repaint();
}

//==============================================================================
void ScoreView::resized()
{
    auto header = getLocalBounds().removeFromTop (headerHeight).reduced (10, 6);
    header.removeFromLeft (juce::jmin (230, header.getWidth() / 4));      // region name, painted
    resolutionChoice.setBounds (header.removeFromLeft (78));
    header.removeFromLeft (6);
    keyChoice.setBounds (header.removeFromLeft (176));
    header.removeFromLeft (6);
    staffChoice.setBounds (header.removeFromLeft (110));
    header.removeFromLeft (6);
    interpretButton.setBounds (header.removeFromLeft (104));
    zoomInButton.setBounds (header.removeFromRight (28));
    header.removeFromRight (4);
    zoomOutButton.setBounds (header.removeFromRight (28));
    clampScroll();
}

void ScoreView::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (theme::bar);
    g.fillRect (header);
    g.setColour (theme::highlight);
    g.fillRect (header.withHeight (1));
    g.setColour (theme::separator);
    g.fillRect (header.removeFromBottom (1));

    auto title = header.reduced (12, 0).removeFromLeft (juce::jmin (220, header.getWidth() / 4));
    g.setFont (theme::font (13.0f, theme::Weight::semibold));
    if (! region.isValid())
    {
        g.setColour (theme::dimText);
        g.drawText ("Score", title, juce::Justification::centredLeft);
        g.setFont (theme::font (13.0f));
        g.drawText ("Select a MIDI region, or double-click an instrument track's lane to make one.", scoreArea(), juce::Justification::centred);
        return;
    }
    g.setColour (arrange::trackColour (region.getParent()));
    g.fillEllipse (title.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat());
    title.removeFromLeft (8);
    g.setColour (theme::text);
    g.drawFittedText (region[ids::name].toString() + "  -  Score", title, juce::Justification::centredLeft, 1, 0.85f);

    // The paper.
    const auto area = scoreArea();
    score::Style style;
    style.numberFont = theme::font (10.0f, theme::Weight::medium);
    style.selected = juce::Colour (0xffe0850f);
    g.setColour (style.paper);
    g.fillRect (area);

    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (area);

    // Outside the region (those notes don't play): shaded.
    const auto grid = studio.project().grid();
    const auto start = (juce::int64) region[ids::start];
    const float rx1 = toLocalX (renderer.xForTick (grid.tickAt ((double) start)));
    const float rx2 = toLocalX (renderer.xForTick (grid.tickAt ((double) (start + (juce::int64) region[ids::length]))));
    g.setColour (juce::Colour (0x10000000));
    const auto& bars = renderer.getLayout().bars;
    const float firstX = bars.empty() ? rx1 : toLocalX (renderer.xForTick ((double) bars.front().start));
    if (rx1 > firstX + 1.0f)
        g.fillRect (juce::Rectangle<float> (firstX, (float) area.getY(), rx1 - firstX, (float) area.getHeight()));
    if (rx2 < (float) area.getRight())
        g.fillRect (juce::Rectangle<float> (rx2, (float) area.getY(), (float) area.getRight() - rx2, (float) area.getHeight()));

    {
        juce::Graphics::ScopedSaveState inner (g);
        g.addTransform (juce::AffineTransform::translation ((float) area.getX() - scrollX, offsetY()));
        renderer.paint (g, style, [this] (int id)
                        {
                            return id >= 0 && id < (int) noteTrees.size() && isSelected (noteTrees[(size_t) id]);
                        },
                        { scrollX, scrollX + (float) area.getWidth() });
    }

    // Playhead.
    const float px = toLocalX (renderer.xForTick (playheadTick()));
    if (px >= (float) area.getX() && px < (float) area.getRight())
    {
        g.setColour (theme::accent);
        g.fillRect (juce::Rectangle<float> (px - 0.75f, (float) area.getY(), 1.5f, (float) area.getHeight()));
    }
}

void ScoreView::timerCallback()
{
    if (! region.isValid() || ! isShowing())
        return;
    const float px = toLocalX (renderer.xForTick (playheadTick()));
    if (std::abs (px - lastPlayheadX) < 0.5f)
        return;

    const auto area = scoreArea();
    if (studio.engine().isPlaying() && ! dragging && (px > (float) area.getRight() - 20.0f || px < (float) area.getX()))
    {
        scrollX += px - (float) area.getX() - renderer.getContentLeft() * 0.5f;
        clampScroll();
        repaint();
    }
    else
    {
        repaint (juce::Rectangle<int> ((int) lastPlayheadX - 2, area.getY(), 5, area.getHeight()));
        repaint (juce::Rectangle<int> ((int) px - 2, area.getY(), 5, area.getHeight()));
    }
    lastPlayheadX = toLocalX (renderer.xForTick (playheadTick()));
}

//==============================================================================
void ScoreView::pruneSelection()
{
    for (int i = selection.size(); --i >= 0;)
        if (selection.getReference (i).getParent() != region)
            selection.remove (i);
}

void ScoreView::deleteSelection()
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

int ScoreView::shiftPitch (int pitch, int steps, bool chromatic) const
{
    if (chromatic)
        return juce::jlimit (0, 127, pitch + steps);
    if (isDrumTrack())
        return score::drumPitchForStep (score::drumPosition (pitch).step + steps);
    for (int i = 0; i < std::abs (steps); ++i)
        pitch = score::scaleStep (pitch, keyFifths(), steps > 0 ? 1 : -1);
    return pitch;
}

void ScoreView::transpose (const std::vector<std::pair<juce::ValueTree, int>>& from, int steps, bool chromatic)
{
    auto& project = studio.project();
    for (auto& [tree, pitch] : from)
    {
        if (tree.getParent() != region)
            continue;
        auto note = daw::Project::noteFrom (tree);
        note.pitch = shiftPitch (pitch, steps, chromatic);
        project.setNote (tree, note);
    }
}

void ScoreView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragging = dragChanged = false;
    dragFrom.clear();
    dragSteps = 0;
    if (! region.isValid() || ! scoreArea().contains (e.getPosition()))
        return;
    pruneSelection();

    const int id = renderer.noteAt (toScore (e.position));
    if (id < 0 || id >= (int) noteTrees.size())
    {
        if (! e.mods.isShiftDown())
            selection.clear();
        repaint();
        return;
    }

    const auto tree = noteTrees[(size_t) id];
    if (e.mods.isShiftDown())
    {
        if (isSelected (tree)) selection.removeFirstMatchingValue (tree);
        else                   selection.add (tree);
    }
    else if (! isSelected (tree))
        selection = { tree };

    const auto note = daw::Project::noteFrom (tree);
    studio.auditionNote (note.pitch, note.velocity);

    if (isSelected (tree))
    {
        dragging = true;
        // The grabbed note first: its pitch is the one auditioned while dragging.
        dragFrom.push_back ({ tree, note.pitch });
        for (auto& n : selection)
            if (n != tree)
                dragFrom.push_back ({ n, daw::Project::noteFrom (n).pitch });
    }
    repaint();
}

void ScoreView::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging || dragFrom.empty())
        return;
    const float halfSpace = renderer.getStaffSpace() * 0.5f;
    const int steps = -juce::roundToInt ((float) e.getDistanceFromDragStartY() / halfSpace);
    if (steps == dragSteps)
        return;
    if (! dragChanged)
        studio.project().undo.beginNewTransaction ("Change Pitch");
    dragChanged = true;
    dragSteps = steps;
    transpose (dragFrom, steps, e.mods.isShiftDown());
    studio.auditionNote (daw::Project::noteFrom (dragFrom.front().first).pitch);
}

void ScoreView::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    dragFrom.clear();
    dragSteps = 0;
}

void ScoreView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! region.isValid() || ! scoreArea().contains (e.getPosition()))
        return;
    const auto p = toScore (e.position);
    if (renderer.noteAt (p) >= 0 || p.x < renderer.getContentLeft() || renderer.getLayout().staves.empty())
        return;

    const int staff = renderer.staffAt (p.y);
    const int step = renderer.stepAt (staff, p.y);
    const int pitch = juce::jlimit (0, 127, isDrumTrack() ? score::drumPitchForStep (step) : score::pitchForStep (step, keyFifths()));

    // Snap to the display resolution's cell the click is in.
    const double res = resolution();
    const double songTick = std::floor (renderer.tickAtX (p.x) / res) * res;
    const auto start = (juce::int64) std::llround (songTick - originTicks());
    if (start < 0)
        return;

    daw::Note note;
    note.pitch = pitch;
    note.start = start;
    note.length = (juce::int64) res;
    note.velocity = selection.isEmpty() ? 100 : daw::Project::noteFrom (selection.getFirst()).velocity;

    auto& project = studio.project();
    project.undo.beginNewTransaction ("Add Note");
    selection = { project.addNote (region, note) };
    studio.auditionNote (note.pitch, note.velocity);
}

void ScoreView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown())
    {
        setZoom (zoom * (1.0f + w.deltaY), (float) e.x);
        return;
    }
    const bool fitsVertically = renderer.getHeight() <= (float) scoreArea().getHeight();
    const float dx = std::abs (w.deltaX) > 0.0f ? w.deltaX : (e.mods.isShiftDown() || fitsVertically ? w.deltaY : 0.0f);
    const float dy = e.mods.isShiftDown() || fitsVertically ? 0.0f : w.deltaY;
    scrollX -= dx * 400.0f;
    scrollY -= dy * 400.0f;
    clampScroll();
    repaint();
}

void ScoreView::mouseMagnify (const juce::MouseEvent& e, float scale)
{
    setZoom (zoom * scale, (float) e.x);
}

bool ScoreView::keyPressed (const juce::KeyPress& key)
{
    if (! region.isValid())
        return false;
    pruneSelection();
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::backspaceKey || code == juce::KeyPress::deleteKey)
    {
        if (selection.isEmpty())
            return false;
        deleteSelection();
        return true;
    }
    if ((code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) && ! key.getModifiers().isCommandDown())
    {
        if (selection.isEmpty())
            return false;
        std::vector<std::pair<juce::ValueTree, int>> from;
        for (auto& n : selection)
            from.push_back ({ n, daw::Project::noteFrom (n).pitch });
        studio.project().undo.beginNewTransaction ("Transpose");
        transpose (from, code == juce::KeyPress::upKey ? 1 : -1, key.getModifiers().isShiftDown());
        if (selection.size() == 1)
            studio.auditionNote (daw::Project::noteFrom (selection.getFirst()).pitch);
        return true;
    }
    if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0))
    {
        selection.clear();
        for (auto& n : noteTrees)
            selection.add (n);
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
