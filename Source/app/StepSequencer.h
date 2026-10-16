#pragma once

#include "Studio.h"
#include "Theme.h"
#include "../daw/Drummer.h"
#include "../daw/StepPattern.h"

namespace woodshed::app
{

/**
    The Step Sequencer (Logic's), in the bottom pane: the selected MIDI region as a grid of steps.
    Rows are the kit pieces on a drum track, or a scale's notes on any other instrument; columns
    are steps of the chosen length. Click a step to turn it on or off, drag across steps to paint,
    drag an on step up or down for its velocity (or use the velocity lane for the selected row).
    Each row can be muted or soloed. The pattern repeats to fill the region, and is kept on the
    region so it opens again as a pattern. Every edit (a whole drag included) is one undo step.
*/
class StepSequencer final : public juce::Component,
                            private juce::ChangeListener,
                            private juce::ValueTree::Listener,
                            private Studio::ProjectSwapListener,
                            private juce::Timer
{
public:
    explicit StepSequencer (Studio&);
    ~StepSequencer() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void projectAboutToBeReplaced() override;
    void timerCallback() override;

    void sync();
    void loadPattern();
    void showControls();
    bool drums() const;

    /** Writes the pattern: previewed during a drag, one undo step otherwise (and at the drag's end). */
    void edited (const juce::String& undoName);
    void commit (const juce::String& undoName);

    // Geometry.
    juce::Rectangle<int> rowHeaderArea() const;
    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> velocityArea() const;
    float rowHeight() const;
    float stepWidth() const;
    int rowAt (float y) const;
    int stepAt (float x) const;
    juce::Rectangle<float> cell (int row, int step) const;
    int visibleRows() const;
    int playingStep() const;

    Studio& studio;
    juce::ValueTree watched, region;
    daw::StepPattern pattern;
    daw::RegionSnapshot snapshot;
    bool writing = false, previewPending = false;
    int selectedRow = 0, firstRow = 0, lastPlayingStep = -1;
    juce::uint32 seed = 1;

    // Gestures.
    enum class Gesture { none, paintOn, paintOff, maybeOff, velocity, lane };
    Gesture gesture = Gesture::none;
    int downRow = -1, downStep = -1, downVelocity = 100, lastVelocity = 100;
    juce::String gestureName;

    juce::ComboBox stepLength, stepCount, rowSet;
    juce::TextButton octaveDown { "-" }, octaveUp { "+" }, clearButton { "Clear" }, randomButton { "Randomize" };

    static constexpr int headerHeight = 40, rowHeaderWidth = 168, velocityHeight = 64;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StepSequencer)
};

} // namespace woodshed::app
