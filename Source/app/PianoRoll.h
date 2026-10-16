#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

/**
    The MIDI editor (Logic's Piano Roll), in the bottom pane: a keyboard down the left, the
    region's notes on a pitch-by-time grid, and their velocities underneath.

    Double-click (or Cmd-click) on the grid adds a note; drag a note to move it (Option copies),
    drag its ends to change its length; drag on empty grid to select with a rubber band. Snaps
    to the quantize grid unless Cmd is held. Delete removes the selection, the arrow keys move it
    (up/down a semitone, Shift an octave; left/right a grid step) and Q quantizes it. Clicking
    the keyboard plays the note. Every edit is one undo step.
*/
class PianoRoll final : public juce::Component,
                        private juce::ChangeListener,
                        private juce::ValueTree::Listener,
                        private Studio::ProjectSwapListener,
                        private juce::Timer
{
public:
    explicit PianoRoll (Studio&);
    ~PianoRoll() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;
    bool keyPressed (const juce::KeyPress&) override;

    /** Quantizes the selected notes (all of them if none are selected) to the grid. */
    void quantizeSelection();

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void projectAboutToBeReplaced() override;
    void timerCallback() override;

    void sync();
    void focusOnNotes();

    // Geometry. Ticks are the region's own (from its content origin).
    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> keyboardArea() const;
    juce::Rectangle<int> velocityArea() const;
    juce::Rectangle<int> rulerArea() const;
    float xForTick (double tick) const;
    double tickAtX (float x) const;
    float yForPitch (int pitch) const;
    int pitchAtY (float y) const;
    double snapTicks (double tick, bool enabled) const;
    int gridTicks() const;
    juce::int64 playheadTick() const;

    enum class Part { none, body, leftEdge, rightEdge };
    struct Hit { juce::ValueTree note; Part part = Part::none; };
    Hit noteAt (juce::Point<int>) const;
    juce::Rectangle<float> noteRect (const daw::Note&) const;

    juce::Array<juce::ValueTree> notes() const;
    void pruneSelection();
    bool isSelected (const juce::ValueTree& n) const        { return selection.contains (n); }
    juce::ValueTree addNoteAt (juce::Point<int>);
    void deleteSelection();
    void moveSelection (juce::int64 deltaTicks, int deltaPitch, const juce::String& undoName);
    void setVelocityAt (juce::Point<int>, bool start);

    void drawRuler (juce::Graphics&);
    void drawKeyboard (juce::Graphics&);
    void drawGrid (juce::Graphics&);
    void drawNotes (juce::Graphics&);
    void drawVelocities (juce::Graphics&);

    Studio& studio;
    juce::ValueTree watched, region;
    juce::Array<juce::ValueTree> selection;

    // View.
    double viewStartTick = 0.0, pixelsPerTick = 0.06;
    float rowHeight = 12.0f, scrollY = 0.0f;
    int lastRegionId = -1;

    // Gestures.
    enum class Gesture { none, move, resizeLeft, resizeRight, rubber, velocity, keyboard };
    Gesture gesture = Gesture::none;
    struct Original { juce::ValueTree note; daw::Note value; };
    std::vector<Original> originals;
    juce::ValueTree grabbed;
    double downTick = 0.0;
    int downPitch = 60, lastAuditionPitch = -1, keyboardPitch = -1;
    juce::Rectangle<int> rubber;
    juce::Array<juce::ValueTree> selectionAtDown;
    juce::int64 defaultLength = daw::ticksPerQuarter / 4;
    float lastPlayheadX = -1.0f;

    juce::ComboBox quantizeChoice;
    juce::TextButton quantizeButton { "Q" };
    juce::Slider velocitySlider;
    juce::Label velocityLabel;

    static constexpr int headerHeight = 36, keyboardWidth = 64, rulerHeight = 20, velocityHeight = 64;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRoll)
};

} // namespace woodshed::app
