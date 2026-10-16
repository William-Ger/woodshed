#include "InstrumentPanel.h"

namespace woodshed::app
{

namespace ids = daw::ids;

namespace
{
void makeQuiet (juce::Component& c)
{
    // Space must keep starting playback after a click in the pane, not press the control again.
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

std::vector<float> knobValuesOf (const juce::ValueTree& node, const std::vector<KnobSpec>& specs)
{
    auto values = daw::Project::decodeValues (node[ids::knobs].toString());
    for (size_t i = values.size(); i < specs.size(); ++i)
        values.push_back (specs[i].def);
    return values;
}
}

//==============================================================================
struct ParamGrid::Cell final : public juce::Component
{
    Cell (ParamGrid& g, int i, const KnobSpec& s) : grid (g), index (i), spec (s)
    {
        if (spec.isChoice())
        {
            box.addItemList (spec.choices, 1);
            box.onChange = [this]
            {
                if (updating) return;
                if (grid.onGestureStart) grid.onGestureStart();
                if (grid.onChange) grid.onChange (index, (float) (box.getSelectedId() - 1));
            };
            makeQuiet (box);
            addAndMakeVisible (box);
        }
        else
        {
            slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            auto range = spec.range();
            slider.setNormalisableRange ({ (double) range.start, (double) range.end,
                                           [range] (double, double, double n) { return (double) range.convertFrom0to1 ((float) n); },
                                           [range] (double, double, double v) { return (double) range.convertTo0to1 ((float) v); } });
            slider.setDoubleClickReturnValue (true, spec.def);
            slider.setColour (juce::Slider::rotarySliderFillColourId, grid.accent);
            slider.onDragStart = [this] { if (grid.onGestureStart) grid.onGestureStart(); };
            slider.onValueChange = [this]
            {
                repaint();
                if (! updating && grid.onChange)
                    grid.onChange (index, (float) slider.getValue());
            };
            slider.setPopupMenuEnabled (false);
            makeQuiet (slider);
            addAndMakeVisible (slider);
        }
        setTooltip();
    }

    void setTooltip()
    {
        slider.setTooltip (spec.label);
        box.setTooltip (spec.label);
    }

    void setValue (float v)
    {
        juce::ScopedValueSetter<bool> s (updating, true);
        if (spec.isChoice())
            box.setSelectedId (juce::jlimit (0, spec.choices.size() - 1, juce::roundToInt (v)) + 1, juce::dontSendNotification);
        else
            slider.setValue (v, juce::dontSendNotification);
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 2);
        r.removeFromBottom (30);
        if (spec.isChoice())
            box.setBounds (r.withSizeKeepingCentre (r.getWidth(), 24));
        else
            slider.setBounds (r.withSizeKeepingCentre (52, 52));
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (2, 2);
        auto text = r.removeFromBottom (30);
        g.setColour (theme::text);
        g.setFont (theme::font (11.0f, theme::Weight::medium));
        g.drawFittedText (spec.label, text.removeFromTop (15), juce::Justification::centred, 1, 0.8f);
        if (! spec.isChoice())
        {
            g.setColour (theme::dimText);
            g.setFont (theme::readout (10.0f, theme::Weight::medium));
            g.drawFittedText (spec.format ((float) slider.getValue()), text, juce::Justification::centredTop, 1, 0.8f);
        }
    }

    ParamGrid& grid;
    int index;
    KnobSpec spec;
    juce::Slider slider;
    juce::ComboBox box;
    bool updating = false;
};

ParamGrid::ParamGrid()
{
    setInterceptsMouseClicks (false, true);
}

void ParamGrid::setSpecs (const std::vector<KnobSpec>& specs, juce::Colour newAccent)
{
    juce::StringArray idsNow;
    for (auto& s : specs)
        idsNow.add (s.id + "/" + s.label + "/" + juce::String (s.min) + "/" + juce::String (s.max));
    if (idsNow == shownIds && newAccent == accent)
        return;

    shownIds = idsNow;
    accent = newAccent;
    cells.clear();
    for (size_t i = 0; i < specs.size(); ++i)
    {
        cells.push_back (std::make_unique<Cell> (*this, (int) i, specs[i]));
        addAndMakeVisible (*cells.back());
    }
    resized();
}

void ParamGrid::setValues (const std::vector<float>& values)
{
    for (size_t i = 0; i < cells.size(); ++i)
        cells[i]->setValue (i < values.size() ? values[i] : cells[i]->spec.def);
}

int ParamGrid::heightFor (int width) const
{
    const int perRow = juce::jmax (1, width / cellWidth);
    const int rows = ((int) cells.size() + perRow - 1) / perRow;
    return rows * cellHeight;
}

void ParamGrid::resized()
{
    const int perRow = juce::jmax (1, getWidth() / cellWidth);
    for (size_t i = 0; i < cells.size(); ++i)
    {
        const int row = (int) i / perRow, col = (int) i % perRow;
        cells[i]->setBounds (col * cellWidth, row * cellHeight, cellWidth, cellHeight);
    }
}

void ParamGrid::paint (juce::Graphics&) {}

//==============================================================================
InsertsEditor::InsertsEditor (Studio& s) : studio (s)
{
    addButton.setTooltip ("Add an effect after the instrument (or the rig)");
    addButton.onClick = [this] { showAddMenu(); };
    makeQuiet (addButton);
    addAndMakeVisible (addButton);

    knobs.onGestureStart = [this] { studio.project().undo.beginNewTransaction ("Effect Knob"); };
    knobs.onChange = [this] (int index, float value)
    {
        auto node = track.getChildWithProperty (ids::id, selectedFx);
        auto* type = Registry::get().find (node[ids::type].toString());
        if (! node.isValid() || type == nullptr)
            return;
        auto values = knobValuesOf (node, type->info.knobs);
        if (juce::isPositiveAndBelow (index, (int) values.size()))
            values[(size_t) index] = value;
        node.setProperty (ids::knobs, daw::Project::encodeValues (values), &studio.project().undo);
    };
    knobView.setViewedComponent (&knobs, false);
    knobView.setScrollBarsShown (true, false);
    knobView.setScrollBarThickness (8);
    addAndMakeVisible (knobView);
}

InsertsEditor::~InsertsEditor()
{
    track.removeListener (this);
}

void InsertsEditor::setTrack (juce::ValueTree t)
{
    if (t == track)
        return;
    track.removeListener (this);
    track = t;
    track.addListener (this);
    selectedFx = -1;
    refresh();
}

void InsertsEditor::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (tree.hasType (ids::FX))
        refresh();
}

void InsertsEditor::refresh()
{
    fx = daw::Project::inserts (track);
    bool found = false;
    for (auto& f : fx)
        found = found || (int) f[ids::id] == selectedFx;
    if (! found)
        selectedFx = fx.isEmpty() ? -1 : (int) fx.getLast()[ids::id];

    auto node = track.getChildWithProperty (ids::id, selectedFx);
    if (auto* type = node.hasType (ids::FX) ? Registry::get().find (node[ids::type].toString()) : nullptr)
    {
        knobs.setSpecs (type->info.knobs, type->info.body);
        knobs.setValues (knobValuesOf (node, type->info.knobs));
        knobs.setVisible (true);
    }
    else
    {
        knobs.setSpecs ({}, theme::accent);
        knobs.setVisible (false);
    }
    resized();
    repaint();
}

juce::Rectangle<int> InsertsEditor::rowBounds (int index) const
{
    return { 8, titleHeight + index * rowHeight, getWidth() - 16, rowHeight - 2 };
}

int InsertsEditor::rowAt (juce::Point<int> p) const
{
    for (int i = 0; i < fx.size(); ++i)
        if (rowBounds (i).contains (p))
            return i;
    return -1;
}

void InsertsEditor::resized()
{
    addButton.setBounds (getWidth() - 8 - 104, 3, 104, 22);
    const int top = titleHeight + juce::jmax (1, fx.size()) * rowHeight + 8;
    knobView.setBounds (0, top, getWidth(), juce::jmax (0, getHeight() - top));
    const int w = knobView.getWidth() - knobView.getScrollBarThickness();
    knobs.setSize (juce::jmax (1, w), knobs.heightFor (juce::jmax (1, w)));
}

void InsertsEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::separator);
    g.fillRect (0, 0, 1, getHeight());

    g.setColour (theme::dimText);
    g.setFont (theme::font (11.0f, theme::Weight::semibold));
    g.drawText ("AUDIO FX", juce::Rectangle<int> (10, 0, 120, titleHeight), juce::Justification::centredLeft);

    if (fx.isEmpty())
    {
        g.setColour (theme::faintText);
        g.setFont (theme::font (12.0f));
        g.drawText ("No effects. Add EQ, compression, reverb, delay...", rowBounds (0).withTrimmedLeft (4),
                    juce::Justification::centredLeft, true);
    }

    for (int i = 0; i < fx.size(); ++i)
    {
        auto r = rowBounds (i);
        const auto& node = fx.getReference (i);
        auto* type = Registry::get().find (node[ids::type].toString());
        const bool on = (bool) node.getProperty (ids::on, true);
        const bool selected = (int) node[ids::id] == selectedFx;

        theme::drawRaised (g, r.toFloat(), theme::radiusChip, selected ? theme::panelLight : theme::control.withAlpha (0.5f));
        if (i == dragTo && dragFrom >= 0 && dragTo != dragFrom)
        {
            g.setColour (theme::accent);
            g.fillRect (r.getX(), dragTo > dragFrom ? r.getBottom() : r.getY() - 1, r.getWidth(), 2);
        }

        auto power = r.removeFromLeft (24).toFloat().withSizeKeepingCentre (10.0f, 10.0f);
        g.setColour (on ? (type != nullptr ? type->info.body : theme::accent) : theme::faintText.withAlpha (0.5f));
        g.fillEllipse (power);

        auto remove = r.removeFromRight (22);
        g.setColour (theme::faintText);
        g.setFont (theme::font (13.0f));
        g.drawText (juce::CharPointer_UTF8 ("\xc3\x97"), remove, juce::Justification::centred);

        g.setColour (on ? theme::text : theme::dimText);
        g.setFont (theme::font (12.0f, theme::Weight::semibold));
        const auto name = type != nullptr ? type->info.name : node[ids::type].toString();
        g.drawText (name, r.removeFromLeft (juce::jmin (r.getWidth(), 92)), juce::Justification::centredLeft, true);
        if (type != nullptr)
        {
            g.setColour (theme::faintText);
            g.setFont (theme::font (11.0f));
            g.drawText (type->info.subtitle, r, juce::Justification::centredLeft, true);
        }
    }
}

void InsertsEditor::mouseDown (const juce::MouseEvent& e)
{
    dragFrom = dragTo = -1;
    const int row = rowAt (e.getPosition());
    if (row < 0)
        return;

    auto node = fx[row];
    const auto r = rowBounds (row);
    auto& p = studio.project();

    if (e.x < r.getX() + 24)
    {
        p.undo.beginNewTransaction ("Effect On/Off");
        node.setProperty (ids::on, ! (bool) node.getProperty (ids::on, true), &p.undo);
        return;
    }
    if (e.x > r.getRight() - 22)
    {
        p.undo.beginNewTransaction ("Remove Effect");
        track.removeChild (node, &p.undo);
        return;
    }

    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addItem ((bool) node.getProperty (ids::on, true) ? "Bypass" : "Turn On", [this, node]() mutable
        {
            auto& pr = studio.project();
            pr.undo.beginNewTransaction ("Effect On/Off");
            node.setProperty (ids::on, ! (bool) node.getProperty (ids::on, true), &pr.undo);
        });
        m.addItem ("Reset Knobs", [this, node]() mutable
        {
            if (auto* type = Registry::get().find (node[ids::type].toString()))
            {
                std::vector<float> defaults;
                for (auto& k : type->info.knobs)
                    defaults.push_back (k.def);
                auto& pr = studio.project();
                pr.undo.beginNewTransaction ("Reset Effect");
                node.setProperty (ids::knobs, daw::Project::encodeValues (defaults), &pr.undo);
            }
        });
        m.addItem ("Remove", [this, node]
        {
            auto& pr = studio.project();
            pr.undo.beginNewTransaction ("Remove Effect");
            track.removeChild (node, &pr.undo);
        });
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
        return;
    }

    selectedFx = (int) node[ids::id];
    dragFrom = dragTo = row;
    refresh();
}

void InsertsEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragFrom < 0)
        return;
    const int over = juce::jlimit (0, fx.size() - 1, (e.y - titleHeight) / rowHeight);
    if (over != dragTo)
    {
        dragTo = over;
        repaint();
    }
}

void InsertsEditor::mouseUp (const juce::MouseEvent&)
{
    if (dragFrom >= 0 && dragTo >= 0 && dragTo != dragFrom)
    {
        auto& p = studio.project();
        p.undo.beginNewTransaction ("Move Effect");
        track.moveChild (track.indexOf (fx[dragFrom]), track.indexOf (fx[dragTo]), &p.undo);
    }
    dragFrom = dragTo = -1;
    repaint();
}

void InsertsEditor::showAddMenu()
{
    if (! track.isValid())
        return;

    juce::PopupMenu m;
    for (auto& category : Registry::categories())
    {
        juce::PopupMenu sub;
        for (auto& t : Registry::get().all())
        {
            if (t.info.category != category)
                continue;
            const auto type = t.info.type;
            sub.addItem (t.info.name + "  " + t.info.subtitle, [this, type]
            {
                auto* pt = Registry::get().find (type);
                if (pt == nullptr || ! track.isValid())
                    return;
                std::vector<float> defaults;
                for (auto& k : pt->info.knobs)
                    defaults.push_back (k.def);
                auto& p = studio.project();
                p.undo.beginNewTransaction ("Add " + pt->info.name);
                auto node = p.addInsert (track, type, defaults);
                selectedFx = (int) node[ids::id];
                refresh();
            });
        }
        if (sub.getNumItems() > 0)
            m.addSubMenu (category, sub);
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addButton));
}

//==============================================================================
InstrumentPanel::InstrumentPanel (Studio& s) : studio (s), inserts (s)
{
    instrumentButton.setTooltip ("Choose the instrument");
    instrumentButton.onClick = [this] { showInstrumentMenu(); };
    patchButton.setTooltip ("Choose a sound");
    patchButton.onClick = [this] { showPatchMenu(); };
    previousPatch.setTooltip ("Previous sound");
    previousPatch.onClick = [this] { stepPatch (-1); };
    nextPatch.setTooltip ("Next sound");
    nextPatch.onClick = [this] { stepPatch (1); };
    typingButton.setTooltip ("Play with your computer keyboard (Cmd+K)");
    typingButton.onClick = [this] { if (onMusicalTyping) onMusicalTyping(); };
    for (auto* b : { &instrumentButton, &patchButton, &previousPatch, &nextPatch, &typingButton })
    {
        makeQuiet (*b);
        addAndMakeVisible (*b);
    }

    knobs.onGestureStart = [this] { studio.project().undo.beginNewTransaction ("Instrument Knob"); };
    knobs.onChange = [this] (int index, float value)
    {
        auto inst = shownTrack.getChildWithName (ids::INSTRUMENT);
        auto* type = shownType();
        if (! inst.isValid() || type == nullptr)
            return;
        auto values = knobValuesOf (inst, type->info.knobs);
        if (juce::isPositiveAndBelow (index, (int) values.size()))
            values[(size_t) index] = value;
        inst.setProperty (ids::knobs, daw::Project::encodeValues (values), &studio.project().undo);
    };
    knobView.setViewedComponent (&knobs, false);
    knobView.setScrollBarsShown (true, false);
    knobView.setScrollBarThickness (8);
    addAndMakeVisible (knobView);
    addAndMakeVisible (inserts);

    studio.addChangeListener (this);
    studio.addProjectSwapListener (this);
    sync();
}

InstrumentPanel::~InstrumentPanel()
{
    watched.removeListener (this);
    studio.removeProjectSwapListener (this);
    studio.removeChangeListener (this);
}

void InstrumentPanel::projectAboutToBeReplaced()
{
    watched.removeListener (this);
    watched = {};
    shownTrack = {};
    inserts.setTrack ({});
}

void InstrumentPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    sync();
}

void InstrumentPanel::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (tree == shownTrack || tree.getParent() == shownTrack)
        sync();
}

void InstrumentPanel::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent == shownTrack)
        sync();
}

const InstrumentType* InstrumentPanel::shownType() const
{
    return InstrumentRegistry::get().find (shownTrack.getChildWithName (ids::INSTRUMENT)[ids::type].toString());
}

int InstrumentPanel::currentPatchIndex() const
{
    if (auto* type = shownType())
    {
        const auto name = shownTrack.getChildWithName (ids::INSTRUMENT)[ids::patch].toString();
        for (size_t i = 0; i < type->info.patches.size(); ++i)
            if (type->info.patches[i].name == name)
                return (int) i;
    }
    return -1;
}

void InstrumentPanel::sync()
{
    if (watched != studio.project().state)
    {
        watched.removeListener (this);
        watched = studio.project().state;
        watched.addListener (this);
    }

    auto track = studio.getSelectedTrack();
    if (! daw::Project::isInstrumentTrack (track))
        track = {};

    shownTrack = track;
    inserts.setTrack (track);

    auto* type = shownType();
    if (type != nullptr)
    {
        knobs.setSpecs (type->info.knobs, type->info.colour);
        knobs.setValues (knobValuesOf (shownTrack.getChildWithName (ids::INSTRUMENT), type->info.knobs));
        instrumentButton.setButtonText (type->info.name + juce::String (juce::CharPointer_UTF8 ("  \xe2\x96\xbe")));
        const auto patch = shownTrack.getChildWithName (ids::INSTRUMENT)[ids::patch].toString();
        patchButton.setButtonText ((patch.isNotEmpty() ? patch : juce::String ("Custom")) + juce::String (juce::CharPointer_UTF8 ("  \xe2\x96\xbe")));
    }
    else
    {
        knobs.setSpecs ({}, theme::accent);
        instrumentButton.setButtonText (track.isValid() ? juce::String ("Missing instrument") : juce::String ("Instrument"));
        patchButton.setButtonText ("-");
    }

    for (auto* b : { &instrumentButton, &patchButton, &previousPatch, &nextPatch, &typingButton })
        b->setEnabled (track.isValid());
    resized();
    repaint();
}

void InstrumentPanel::showInstrumentMenu()
{
    if (! shownTrack.isValid())
        return;
    juce::PopupMenu m;
    const auto current = shownTrack.getChildWithName (ids::INSTRUMENT)[ids::type].toString();
    for (auto& category : InstrumentRegistry::categories())
    {
        bool header = false;
        for (auto& t : InstrumentRegistry::get().all())
        {
            if (t.info.category != category)
                continue;
            if (! header)
            {
                m.addSectionHeader (category);
                header = true;
            }
            const auto type = t.info.type;
            juce::PopupMenu::Item item (t.info.name);
            item.isTicked = type == current;
            item.action = [this, type] { studio.setInstrument (shownTrack, type, 0); };
            m.addItem (std::move (item));
        }
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (instrumentButton));
}

void InstrumentPanel::showPatchMenu()
{
    auto* type = shownType();
    if (type == nullptr)
        return;

    const int current = currentPatchIndex();
    const auto typeId = type->info.type;
    juce::PopupMenu m;
    juce::String lastCategory;
    for (int i = 0; i < (int) type->info.patches.size(); ++i)
    {
        const auto& patch = type->info.patches[(size_t) i];
        if (patch.category.isNotEmpty() && patch.category != lastCategory)
        {
            m.addSectionHeader (patch.category);
            lastCategory = patch.category;
        }
        juce::PopupMenu::Item item (patch.name);
        item.isTicked = i == current;
        item.action = [this, typeId, i] { studio.setInstrument (shownTrack, typeId, i); };
        m.addItem (std::move (item));
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (patchButton));
}

void InstrumentPanel::stepPatch (int delta)
{
    auto* type = shownType();
    if (type == nullptr || type->info.patches.empty())
        return;
    const int count = (int) type->info.patches.size();
    const int next = ((juce::jmax (0, currentPatchIndex()) + delta) % count + count) % count;
    studio.setInstrument (shownTrack, type->info.type, next);
}

void InstrumentPanel::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (headerHeight).reduced (12, 9);

    typingButton.setBounds (header.removeFromRight (128));
    header.removeFromRight (12);
    nameArea = header.removeFromLeft (juce::jmin (200, header.getWidth() / 4));
    header.removeFromLeft (8);
    instrumentButton.setBounds (header.removeFromLeft (170));
    header.removeFromLeft (12);
    previousPatch.setBounds (header.removeFromLeft (26));
    header.removeFromLeft (4);
    patchButton.setBounds (header.removeFromLeft (juce::jmin (220, juce::jmax (80, header.getWidth() - 40))));
    header.removeFromLeft (4);
    nextPatch.setBounds (header.removeFromLeft (26));

    inserts.setBounds (r.removeFromRight (juce::jlimit (240, 340, r.getWidth() / 3)));
    knobView.setBounds (r.reduced (12, 8));
    const int w = knobView.getWidth() - knobView.getScrollBarThickness();
    knobs.setSize (juce::jmax (1, w), knobs.heightFor (juce::jmax (1, w)));
}

void InstrumentPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (theme::bar);
    g.fillRect (header);
    g.setColour (theme::highlight);
    g.fillRect (header.withHeight (1));
    g.setColour (theme::separator);
    g.fillRect (header.removeFromBottom (1));

    auto name = nameArea;
    if (shownTrack.isValid())
    {
        const auto colour = juce::Colour ((juce::uint32) (juce::int64) shownTrack.getProperty (ids::colour, (juce::int64) 0xff8d857a));
        g.setColour (colour);
        g.fillEllipse (name.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat());
        name.removeFromLeft (8);
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, theme::Weight::semibold));
        g.drawFittedText (shownTrack[ids::name].toString(), name, juce::Justification::centredLeft, 1, 0.85f);
    }
    else
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (13.0f, theme::Weight::semibold));
        g.drawText ("Instrument", name, juce::Justification::centredLeft);

        auto body = getLocalBounds().withTrimmedTop (headerHeight);
        g.setColour (theme::dimText);
        g.setFont (theme::font (13.0f));
        g.drawText ("Select an instrument track to see its sound.", body, juce::Justification::centred);
    }
}

} // namespace woodshed::app
