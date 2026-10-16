#pragma once

#include "Studio.h"
#include "Theme.h"
#include "ScoreRender.h"

namespace woodshed::app
{

/**
    The Score editor (Logic's Score, in its linear view), in the bottom pane: the MIDI region the
    piano roll would edit, shown as notation on one horizontally scrolling system. A grand staff
    for piano-range parts, one treble or bass staff when the notes fit, a percussion staff for
    drum kits.

    The notes are never changed by the display settings, which belong to the region:
      scoreKey        key signature in fifths (-7 Cb ... 0 C ... 7 C#), default 0
      scoreQuantize   display resolution in ticks (960, 480, 240, 120, or triplets 320, 160), default 240
      scoreStaff      0 automatic, 1 treble, 2 bass, 3 grand staff
      scoreInterpret  Logic's Interpretation: short notes shown lengthened up to the next one (default on)

    Click a note to select it (and hear it), Shift-click to add to the selection. Drag up or down
    to move it by steps of the key's scale (Shift: by semitones). The arrow keys do the same.
    Delete removes the selection; double-click on a staff adds a note there, one display
    resolution long. Every edit is one undo step.
*/
class ScoreView final : public juce::Component,
                        private juce::ChangeListener,
                        private juce::ValueTree::Listener,
                        private Studio::ProjectSwapListener,
                        private juce::Timer
{
public:
    explicit ScoreView (Studio&);
    ~ScoreView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void projectAboutToBeReplaced() override;
    void timerCallback() override;

    void sync();
    void rebuild();
    void syncControls();
    bool isDrumTrack() const;
    int resolution() const;
    int keyFifths() const;
    void setRegionProperty (const juce::Identifier&, const juce::var&, const juce::String& undoName);
    void setZoom (float newZoom, float anchorX);

    // Geometry: the renderer's coordinates sit inside scoreArea(), scrolled.
    juce::Rectangle<int> scoreArea() const;
    juce::Point<float> toScore (juce::Point<float> local) const;
    float toLocalX (float scoreX) const;
    float offsetY() const;
    void clampScroll();
    double originTicks() const;
    double playheadTick() const;

    void pruneSelection();
    bool isSelected (const juce::ValueTree& n) const        { return selection.contains (n); }
    void deleteSelection();
    /** Moves the selection by `steps` (scale steps, or semitones when `chromatic`) from `from`. */
    void transpose (const std::vector<std::pair<juce::ValueTree, int>>& from, int steps, bool chromatic);
    int shiftPitch (int pitch, int steps, bool chromatic) const;

    Studio& studio;
    juce::ValueTree watched, region;
    juce::Array<juce::ValueTree> selection;
    std::vector<juce::ValueTree> noteTrees;     // InputNote ids index this
    score::Renderer renderer;
    int lastRegionId = -1;

    float zoom = 1.0f, scrollX = 0.0f, scrollY = 0.0f, lastPlayheadX = -1.0f;

    // Dragging notes.
    std::vector<std::pair<juce::ValueTree, int>> dragFrom;
    int dragSteps = 0;
    bool dragging = false, dragChanged = false;

    juce::ComboBox resolutionChoice, keyChoice, staffChoice;
    juce::TextButton interpretButton { "Interpretation" }, zoomOutButton { "-" }, zoomInButton { "+" };

    static constexpr int headerHeight = 36;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScoreView)
};

} // namespace woodshed::app
