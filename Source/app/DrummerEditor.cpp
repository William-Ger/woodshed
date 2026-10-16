#include "DrummerEditor.h"
#include "ArrangeHeader.h"

namespace woodshed::app
{

namespace ids = daw::ids;
namespace dr = daw::drummer;

namespace
{
void makeQuiet (juce::Component& c)
{
    // Space must keep starting playback after a click in the pane, not press the control again.
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

juce::String percent (double v)         { return juce::String (juce::roundToInt (v * 100.0)) + "%"; }

/** Where everything goes below the header. */
struct Layout
{
    juce::Rectangle<int> styleLabel, styles, padLabel, pad, kitLabel, kit, knobs;
};

Layout layoutFor (juce::Rectangle<int> body)
{
    Layout l;
    auto left = body.removeFromLeft (juce::jlimit (220, 380, (int) ((float) body.getWidth() * 0.34f)));
    l.styleLabel = left.removeFromTop (20);
    l.styles = left.removeFromTop (juce::jmin (left.getHeight(), 4 * 30 + 3 * 6));

    body.removeFromLeft (24);
    auto middle = body.removeFromLeft (juce::jmin (juce::jmax (120, body.getHeight() - 20), juce::jmin (300, body.getWidth() / 2)));
    l.padLabel = middle.removeFromTop (20);
    l.pad = middle.withHeight (juce::jmin (middle.getHeight(), middle.getWidth()));

    body.removeFromLeft (24);
    auto right = body.withWidth (juce::jmin (body.getWidth(), 300));
    l.kitLabel = right.removeFromTop (20);
    l.kit = right.removeFromTop (3 * 26 + 2 * 6);
    right.removeFromTop (14);
    l.knobs = right.removeFromTop (juce::jmin (right.getHeight(), 86));
    return l;
}
}

//==============================================================================
class DrummerEditor::StyleGrid final : public juce::Component
{
public:
    std::function<void (int)> onPick;
    int selected = 0;

    void paint (juce::Graphics& g) override
    {
        for (int i = 0; i < dr::numStyles; ++i)
        {
            const auto r = chip (i).toFloat();
            const bool on = i == selected, over = i == hovered;
            g.setColour (on ? theme::accentDim : over ? theme::controlHover : theme::control);
            g.fillRoundedRectangle (r, theme::radiusControl);
            g.setColour (on ? theme::accent : theme::outline);
            g.drawRoundedRectangle (r.reduced (0.5f), theme::radiusControl, 1.0f);
            g.setColour (on ? theme::accent : theme::text);
            g.setFont (theme::font (12.0f, on ? theme::Weight::semibold : theme::Weight::medium));
            g.drawFittedText (dr::styleName ((dr::Style) i), chip (i).reduced (6, 0), juce::Justification::centred, 1, 0.8f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override     { setHovered (indexAt (e.getPosition())); }
    void mouseExit (const juce::MouseEvent&) override       { setHovered (-1); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = indexAt (e.getPosition());
        if (i >= 0 && onPick != nullptr)
            onPick (i);
    }

private:
    static constexpr int columns = 4, rows = 4, gap = 6;
    int hovered = -1;

    juce::Rectangle<int> chip (int i) const
    {
        const int w = (getWidth() - gap * (columns - 1)) / columns;
        const int h = juce::jmin (30, (getHeight() - gap * (rows - 1)) / rows);
        return { (i % columns) * (w + gap), (i / columns) * (h + gap), w, h };
    }

    int indexAt (juce::Point<int> p) const
    {
        for (int i = 0; i < dr::numStyles; ++i)
            if (chip (i).contains (p))
                return i;
        return -1;
    }

    void setHovered (int i)
    {
        if (i != hovered)
        {
            hovered = i;
            repaint();
        }
    }
};

//==============================================================================
class DrummerEditor::XYPad final : public juce::Component
{
public:
    std::function<void()> onStart, onEnd;
    std::function<void (float, float)> onMove;
    float x = 0.4f, y = 0.6f;       // complexity, loudness

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        theme::drawInset (g, r, theme::radiusPanel);

        // A faint grid and the four corners' names.
        g.setColour (theme::gridBeat);
        for (int i = 1; i < 4; ++i)
        {
            const float fx = r.getX() + r.getWidth() * (float) i / 4.0f, fy = r.getY() + r.getHeight() * (float) i / 4.0f;
            g.fillRect (juce::Rectangle<float> (fx, r.getY() + 6.0f, 1.0f, r.getHeight() - 12.0f));
            g.fillRect (juce::Rectangle<float> (r.getX() + 6.0f, fy, r.getWidth() - 12.0f, 1.0f));
        }
        g.setColour (theme::faintText);
        g.setFont (theme::font (10.5f, theme::Weight::medium));
        const auto inner = getLocalBounds().reduced (10, 8);
        g.drawText ("Loud", inner, juce::Justification::topLeft);
        g.drawText ("Soft", inner, juce::Justification::bottomLeft);
        g.drawText ("Complex", inner, juce::Justification::bottomRight);
        g.drawText ("Simple", inner.withTrimmedLeft (34), juce::Justification::bottomLeft);

        // The puck, with crosshairs to the edges.
        const auto c = puck();
        g.setColour (theme::accent.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float> (c.x - 0.5f, r.getY() + 2.0f, 1.0f, r.getHeight() - 4.0f));
        g.fillRect (juce::Rectangle<float> (r.getX() + 2.0f, c.y - 0.5f, r.getWidth() - 4.0f, 1.0f));
        g.setColour (theme::accentDim);
        g.fillEllipse (juce::Rectangle<float> (30.0f, 30.0f).withCentre (c));
        g.setColour (theme::accent);
        g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (c));
        g.setColour (theme::window);
        g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (c));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (onStart != nullptr) onStart();
        moveTo (e.position);
    }
    void mouseDrag (const juce::MouseEvent& e) override     { moveTo (e.position); }
    void mouseUp (const juce::MouseEvent&) override          { if (onEnd != nullptr) onEnd(); }

private:
    static constexpr float margin = 12.0f;

    juce::Point<float> puck() const
    {
        const auto r = getLocalBounds().toFloat().reduced (margin);
        return { r.getX() + x * r.getWidth(), r.getBottom() - y * r.getHeight() };
    }

    void moveTo (juce::Point<float> p)
    {
        const auto r = getLocalBounds().toFloat().reduced (margin);
        x = juce::jlimit (0.0f, 1.0f, (p.x - r.getX()) / juce::jmax (1.0f, r.getWidth()));
        y = juce::jlimit (0.0f, 1.0f, (r.getBottom() - p.y) / juce::jmax (1.0f, r.getHeight()));
        repaint();
        if (onMove != nullptr) onMove (x, y);
    }
};

//==============================================================================
DrummerEditor::DrummerEditor (Studio& s) : studio (s)
{
    setOpaque (true);

    styleGrid = std::make_unique<StyleGrid>();
    styleGrid->onPick = [this] (int i)
    {
        change (dr::withStyle (settings, (dr::Style) i), "Drummer Style", false);
        styleGrid->selected = i;
        styleGrid->repaint();
    };
    addAndMakeVisible (*styleGrid);

    pad = std::make_unique<XYPad>();
    pad->onStart = [this] { gestureName = "Drummer Complexity and Loudness"; };
    pad->onMove = [this] (float x, float y)
    {
        auto next = settings;
        next.complexity = x;
        next.loudness = y;
        change (next, gestureName, true);
    };
    pad->onEnd = [this] { endGesture(); };
    addAndMakeVisible (*pad);

    auto setUpKnob = [this] (juce::Slider& k, const juce::String& tip, const juce::String& undoName, std::function<void (dr::Settings&, float)> set)
    {
        k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        k.setRange (0.0, 1.0, 0.01);
        k.setTooltip (tip);
        k.setColour (juce::Slider::rotarySliderFillColourId, theme::accent);
        k.onDragStart = [this, undoName] { gestureName = undoName; };
        k.onValueChange = [this, &k, set, undoName]
        {
            auto next = settings;
            set (next, (float) k.getValue());
            change (next, undoName, k.isMouseButtonDown());
            repaint();
        };
        k.onDragEnd = [this] { endGesture(); };
        makeQuiet (k);
        addAndMakeVisible (k);
    };
    setUpKnob (swing, "Swing: pushes the off-beats late, up to a full triplet", "Drummer Swing",
               [] (dr::Settings& st, float v) { st.swing = v; });
    setUpKnob (fill, "Fills: none, then fills every 8 bars, then longer ones every 4", "Drummer Fills",
               [] (dr::Settings& st, float v) { st.fill = v; });
    setUpKnob (feel, "Feel: how loose the timing and dynamics are", "Drummer Feel",
               [] (dr::Settings& st, float v) { st.humanize = v; });
    swing.setDoubleClickReturnValue (true, 0.0);
    fill.setDoubleClickReturnValue (true, 0.5);
    feel.setDoubleClickReturnValue (true, 0.5);

    auto setUpPiece = [this] (juce::TextButton& b, const juce::String& tip, bool dr::Settings::* member)
    {
        b.setClickingTogglesState (true);
        b.setTooltip (tip);
        b.onClick = [this, &b, member]
        {
            auto next = settings;
            next.*member = b.getToggleState();
            change (next, "Drummer Kit Pieces", false);
        };
        makeQuiet (b);
        addAndMakeVisible (b);
    };
    setUpPiece (kickSnare, "Kick and snare", &dr::Settings::kickSnare);
    setUpPiece (hiHat, "Hi-hat (and its pedal)", &dr::Settings::hiHat);
    setUpPiece (cymbals, "Ride and crashes", &dr::Settings::cymbals);
    setUpPiece (toms, "Toms in the fills", &dr::Settings::toms);
    setUpPiece (percussion, "Tambourine, shaker, cowbell or clap, by style", &dr::Settings::percussion);

    timekeeper.addItem ("Timekeeper: Auto", 1);
    timekeeper.addItem ("Timekeeper: Hi-Hat", 2);
    timekeeper.addItem ("Timekeeper: Ride", 3);
    timekeeper.setTooltip ("What keeps time: the style's choice, the hi-hat or the ride");
    timekeeper.onChange = [this]
    {
        auto next = settings;
        next.timekeeper = (dr::Timekeeper) juce::jlimit (0, 2, timekeeper.getSelectedId() - 1);
        change (next, "Drummer Timekeeper", false);
    };
    makeQuiet (timekeeper);
    addAndMakeVisible (timekeeper);

    for (int b = 1; b <= 16; ++b)
        bars.addItem (juce::String (b) + (b == 1 ? " bar" : " bars"), b);
    bars.setTooltip ("How long the region is");
    bars.onChange = [this]
    {
        auto next = settings;
        next.bars = juce::jmax (1, bars.getSelectedId());
        change (next, "Drummer Bars", false);
    };
    makeQuiet (bars);
    addAndMakeVisible (bars);

    regenerate.setTooltip ("Play the same settings another way (a new performance)");
    regenerate.onClick = [this]
    {
        auto next = settings;
        next.seed = next.seed * 1664525u + 1013904223u;
        change (next, "Regenerate Drums", false);
    };
    makeQuiet (regenerate);
    addAndMakeVisible (regenerate);

    make.onClick = [this] { makeDrummerRegion(); };
    makeQuiet (make);
    addChildComponent (make);

    studio.addChangeListener (this);
    studio.addProjectSwapListener (this);
    sync();
    startTimerHz (20);
}

DrummerEditor::~DrummerEditor()
{
    watched.removeListener (this);
    studio.removeProjectSwapListener (this);
    studio.removeChangeListener (this);
}

//==============================================================================
void DrummerEditor::projectAboutToBeReplaced()
{
    watched.removeListener (this);
    watched = {};
    region = {};
    snapshot.clear();
    previewPending = false;
}

void DrummerEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    sync();
}

void DrummerEditor::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // Undo and redo bring other settings back; show them (unless this pane is the one writing).
    if (tree == region && ! writing && ! snapshot.isActive() && (property == dr::property || property == ids::name))
    {
        showSettings();
        repaint();
    }
}

void DrummerEditor::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child == region || child == region.getParent())
        sync();
}

void DrummerEditor::sync()
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
            endGesture();
        region = next;
    }
    if (! snapshot.isActive())
        showSettings();
    resized();
    repaint();
}

void DrummerEditor::showSettings()
{
    const bool editing = isEditing();
    for (auto* c : std::initializer_list<juce::Component*> { styleGrid.get(), pad.get(), &swing, &fill, &feel, &kickSnare, &hiHat,
                                                              &cymbals, &toms, &percussion, &timekeeper, &bars, &regenerate })
        c->setVisible (editing);

    // Nothing to edit: offer to make a Drummer region from what's there.
    const auto track = region.isValid() ? region.getParent() : studio.getSelectedTrack();
    make.setButtonText (region.isValid() ? "Make This a Drummer Region" : "New Drummer Region");
    make.setVisible (! editing && daw::Project::isInstrumentTrack (track));

    if (! editing)
        return;

    settings = dr::settingsOf (region);
    styleGrid->selected = (int) settings.style;
    styleGrid->repaint();
    pad->x = settings.complexity;
    pad->y = settings.loudness;
    pad->repaint();
    swing.setValue (settings.swing, juce::dontSendNotification);
    fill.setValue (settings.fill, juce::dontSendNotification);
    feel.setValue (settings.humanize, juce::dontSendNotification);
    kickSnare.setToggleState (settings.kickSnare, juce::dontSendNotification);
    hiHat.setToggleState (settings.hiHat, juce::dontSendNotification);
    cymbals.setToggleState (settings.cymbals, juce::dontSendNotification);
    toms.setToggleState (settings.toms, juce::dontSendNotification);
    percussion.setToggleState (settings.percussion, juce::dontSendNotification);
    timekeeper.setSelectedId ((int) settings.timekeeper + 1, juce::dontSendNotification);
    bars.setSelectedId (juce::jlimit (1, 16, settings.bars), juce::dontSendNotification);
}

//==============================================================================
void DrummerEditor::change (const dr::Settings& next, const juce::String& undoName, bool dragging)
{
    if (! isEditing())
        return;
    settings = next;
    if (dragging)
    {
        // Preview without undo (the timer writes at most 20 times a second); one step at the end.
        if (! snapshot.isActive())
            snapshot.take (region);
        gestureName = undoName;
        previewPending = true;
        return;
    }
    commit (undoName);
}

void DrummerEditor::endGesture()
{
    if (snapshot.isActive())
        commit (gestureName.isNotEmpty() ? gestureName : juce::String ("Drummer"));
}

void DrummerEditor::commit (const juce::String& undoName)
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
    dr::apply (project, region, settings, &project.undo);
    repaint();
}

void DrummerEditor::timerCallback()
{
    if (previewPending && snapshot.isActive() && region.isValid())
    {
        const juce::ScopedValueSetter<bool> guard (writing, true);
        dr::apply (studio.project(), region, settings, nullptr);
        previewPending = false;
    }
}

void DrummerEditor::makeDrummerRegion()
{
    auto& project = studio.project();
    if (region.isValid())
    {
        // Keep the region where it is; its length rounds to whole bars.
        dr::Settings fresh;
        fresh.bars = juce::jlimit (1, 16, juce::roundToInt ((double) (juce::int64) region[ids::length] / project.grid().samplesPerBar()));
        fresh.seed = (juce::uint32) juce::Random::getSystemRandom().nextInt (1 << 30) + 1u;
        if (Studio::isDrumTrack (region.getParent()))
            fresh.percussion = false;
        project.undo.beginNewTransaction ("Make Drummer Region");
        dr::apply (project, region, fresh, &project.undo);
    }
    else if (auto track = studio.getSelectedTrack(); daw::Project::isInstrumentTrack (track))
    {
        project.undo.beginNewTransaction ("New Drummer Region");
        auto r = studio.createDrummerRegion (track, studio.engine().getPosition());
        studio.openInDrummer ((int) r[ids::id]);
    }
    sync();
}

//==============================================================================
void DrummerEditor::resized()
{
    auto header = getLocalBounds().removeFromTop (headerHeight).reduced (12, 7);
    regenerate.setBounds (header.removeFromRight (104));
    header.removeFromRight (8);
    bars.setBounds (header.removeFromRight (92));
    header.removeFromRight (8);
    timekeeper.setBounds (header.removeFromRight (168));

    const auto body = getLocalBounds().withTrimmedTop (headerHeight).reduced (16, 12);
    make.setBounds (body.withSizeKeepingCentre (220, 30).translated (0, 24));

    const auto l = layoutFor (body);
    styleGrid->setBounds (l.styles);
    pad->setBounds (l.pad);

    auto kit = l.kit;
    const int w = (kit.getWidth() - 6) / 2;
    juce::TextButton* pieces[] { &kickSnare, &hiHat, &cymbals, &toms, &percussion };
    for (int i = 0; i < 5; ++i)
        pieces[i]->setBounds (kit.getX() + (i % 2) * (w + 6), kit.getY() + (i / 2) * 32, w, 26);

    auto knobs = l.knobs;
    for (auto* k : { &swing, &fill, &feel })
        k->setBounds (knobs.removeFromLeft (76).removeFromTop (54).withSizeKeepingCentre (50, 50));
}

void DrummerEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (theme::bar);
    g.fillRect (header);
    g.setColour (theme::highlight);
    g.fillRect (header.withHeight (1));
    g.setColour (theme::separator);
    g.fillRect (header.removeFromBottom (1));

    auto title = header.reduced (12, 0).removeFromLeft (juce::jmin (300, header.getWidth() / 3));
    g.setFont (theme::font (13.0f, theme::Weight::semibold));
    if (region.isValid())
    {
        g.setColour (arrange::trackColour (region.getParent()));
        g.fillEllipse (title.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat());
        title.removeFromLeft (8);
    }
    g.setColour (region.isValid() ? theme::text : theme::dimText);
    g.drawFittedText (region.isValid() ? region[ids::name].toString() + "  -  Drummer" : juce::String ("Drummer"),
                      title, juce::Justification::centredLeft, 1, 0.85f);

    const auto body = getLocalBounds().withTrimmedTop (headerHeight).reduced (16, 12);
    if (! isEditing())
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (13.0f));
        const auto text = region.isValid() ? juce::String ("This region isn't a Drummer region. Make it one to have a drummer play it (its notes are replaced).")
                        : daw::Project::isInstrumentTrack (studio.getSelectedTrack()) ? juce::String ("No region here yet. Make a Drummer region at the playhead.")
                        : juce::String ("Choose New Track > Drummer, or select a drum track's region.");
        g.drawFittedText (text, body.withSizeKeepingCentre (body.getWidth(), 40).translated (0, -20), juce::Justification::centred, 2);
        return;
    }

    const auto l = layoutFor (body);
    auto label = [&g] (juce::Rectangle<int> r, const juce::String& text, const juce::String& value = {})
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (11.0f, theme::Weight::semibold));
        g.drawText (text.toUpperCase(), r, juce::Justification::topLeft);
        if (value.isNotEmpty())
        {
            g.setColour (theme::faintText);
            g.setFont (theme::readout (10.5f, theme::Weight::medium));
            g.drawText (value, r, juce::Justification::topRight);
        }
    };
    label (l.styleLabel, "Style");
    label (l.padLabel, "Pattern", juce::String (juce::roundToInt (settings.complexity * 100.0f)) + " / "
                                  + juce::String (juce::roundToInt (settings.loudness * 100.0f)));
    label (l.kitLabel, "Kit Pieces");

    // Knob names and values.
    auto knobs = l.knobs;
    const std::pair<juce::Slider*, const char*> named[] { { &swing, "Swing" }, { &fill, "Fills" }, { &feel, "Feel" } };
    for (auto& [k, name] : named)
    {
        auto cell = knobs.removeFromLeft (76);
        cell.removeFromTop (54);
        g.setColour (theme::text);
        g.setFont (theme::font (11.0f, theme::Weight::medium));
        g.drawText (name, cell.removeFromTop (15), juce::Justification::centred);
        g.setColour (theme::dimText);
        g.setFont (theme::readout (10.0f, theme::Weight::medium));
        g.drawText (percent (k->getValue()), cell.removeFromTop (14), juce::Justification::centred);
    }

    // What the settings mean for the phrase.
    g.setColour (theme::faintText);
    g.setFont (theme::font (11.0f));
    const int phrase = dr::phraseBars (settings);
    const auto grid = studio.project().grid();
    g.drawFittedText (juce::String (grid.beatsPerBar) + "/" + juce::String (grid.beatUnit) + "  -  "
                          + (phrase == 0 ? juce::String ("no fills") : "a fill every " + juce::String (phrase) + " bars"),
                      l.knobs.withTrimmedTop (l.knobs.getHeight()).withHeight (18).translated (0, 4), juce::Justification::topLeft, 1);
}

} // namespace woodshed::app
