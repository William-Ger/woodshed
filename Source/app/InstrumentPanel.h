#pragma once

#include "Studio.h"
#include "Theme.h"
#include "Icons.h"

namespace woodshed::app
{

//==============================================================================
/**
    A grid of knobs (and stepped selectors) for a list of KnobSpecs, in real units. The owner
    supplies the values and hears about every change; a drag is one undo step because the owner
    begins a transaction on `onGestureStart`.
*/
class ParamGrid final : public juce::Component
{
public:
    ParamGrid();

    /** Rebuilds the controls (only when the specs actually change). */
    void setSpecs (const std::vector<KnobSpec>&, juce::Colour accent);
    /** Shows these values (real units) without notifying. */
    void setValues (const std::vector<float>&);

    std::function<void()> onGestureStart;
    std::function<void (int index, float realValue)> onChange;

    /** Height the grid needs at `width`. */
    int heightFor (int width) const;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Cell;
    std::vector<std::unique_ptr<Cell>> cells;
    juce::StringArray shownIds;
    juce::Colour accent;

    static constexpr int cellWidth = 78, cellHeight = 92;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamGrid)
};

//==============================================================================
/**
    A track's insert effects (Logic's Audio FX slots), built from the pedal registry: a list in
    signal order with power, name and remove, an Add Effect menu, and the selected one's knobs.
*/
class InsertsEditor final : public juce::Component,
                            private juce::ValueTree::Listener
{
public:
    explicit InsertsEditor (Studio&);
    ~InsertsEditor() override;

    void setTrack (juce::ValueTree track);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override        { refresh(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override  { refresh(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override          { refresh(); }

    void refresh();
    void showAddMenu();
    juce::Rectangle<int> rowBounds (int index) const;
    int rowAt (juce::Point<int>) const;

    Studio& studio;
    juce::ValueTree track;
    juce::Array<juce::ValueTree> fx;
    int selectedFx = -1;            // FX id
    int dragFrom = -1, dragTo = -1;
    juce::TextButton addButton { "+ Add Effect" };
    ParamGrid knobs;
    juce::Viewport knobView;

    static constexpr int rowHeight = 26, titleHeight = 28;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InsertsEditor)
};

//==============================================================================
/**
    The selected instrument track's sound: its instrument and patch in the header (with Musical
    Typing), the instrument's controls on the left and its Audio FX on the right. Takes the rig's
    place in the bottom pane when an instrument track is selected.
*/
class InstrumentPanel final : public juce::Component,
                              private juce::ChangeListener,
                              private juce::ValueTree::Listener,
                              private Studio::ProjectSwapListener
{
public:
    explicit InstrumentPanel (Studio&);
    ~InstrumentPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Fired by the keyboard button; MainComponent shows its musical typing keyboard. */
    std::function<void()> onMusicalTyping;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void projectAboutToBeReplaced() override;

    void sync();
    void showInstrumentMenu();
    void showPatchMenu();
    void stepPatch (int delta);
    const InstrumentType* shownType() const;
    int currentPatchIndex() const;

    Studio& studio;
    juce::ValueTree watched, shownTrack;
    juce::String shownTypeId;

    juce::TextButton instrumentButton, patchButton, previousPatch { "<" }, nextPatch { ">" }, typingButton { "Musical Typing" };
    ParamGrid knobs;
    juce::Viewport knobView;
    InsertsEditor inserts;
    juce::Rectangle<int> nameArea;

    static constexpr int headerHeight = 44;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentPanel)
};

} // namespace woodshed::app
