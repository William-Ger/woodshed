#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{
class ArrangeView;
}

namespace woodshed::app::arrange
{

/**
    The track lanes: grid, regions with their waveforms, the loop range, the playhead and takes
    being recorded. Click / shift-click / rubber-band to select; drag to move (across tracks too),
    drag an edge to trim, Alt-drag to copy. Snaps to beats unless Cmd is held. Audio files dropped
    from the desktop are copied into the project and placed where they land.
*/
class Lanes final : public juce::Component,
                    public juce::FileDragAndDropTarget
{
public:
    explicit Lanes (ArrangeView&);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    void playheadMoved (float oldX, float newX);

    /** True while a mouse gesture (move, trim, rubber band) is in progress. */
    bool isDragging() const                                 { return gesture != Gesture::none; }

private:
    enum class Part { none, body, leftEdge, rightEdge };
    enum class Gesture { none, pendingRegion, move, trimLeft, trimRight, pendingEmpty, rubber };

    struct Hit
    {
        juce::ValueTree region;
        int trackIndex = -1;
        Part part = Part::none;
    };

    struct RegionLook
    {
        juce::int64 start = 0, offset = 0, length = 0;
        bool selected = false, ghost = false, muted = false;
    };

    Hit regionAt (juce::Point<int>) const;
    juce::Rectangle<float> regionRect (int trackIndex, juce::int64 start, juce::int64 length) const;
    void drawGrid (juce::Graphics&, int contentBottom) const;
    void drawRegion (juce::Graphics&, const juce::ValueTree& region, int trackIndex, const RegionLook&);
    void drawTakes (juce::Graphics&);
    void drawHint (juce::Graphics&, int contentBottom) const;
    void showRegionMenu();

    juce::int64 fileLengthOf (const juce::ValueTree& region);
    juce::int64 minimumLength() const;
    void commitMove (bool copy);
    void commitTrim();
    void updateRubberBand (const juce::MouseEvent&);
    void dropTarget (int x, int y);

    ArrangeView& view;

    Gesture gesture = Gesture::none;
    Part grabbedPart = Part::none;
    juce::ValueTree grabbed;
    int grabbedTrack = -1;
    juce::int64 grabStart = 0, grabOffset = 0, grabLength = 0;
    double downSample = 0.0;
    bool shiftAtDown = false;

    // Live preview of the gesture; committed on mouse up as one undo step.
    juce::int64 moveDelta = 0, trimStart = 0, trimOffset = 0, trimLength = 0;
    int trackDelta = 0;
    bool copying = false;

    juce::Rectangle<int> rubber;
    juce::Array<int> selectionAtDown;

    bool dropping = false;
    int dropTrack = -1;
    juce::int64 dropAt = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Lanes)
};

} // namespace woodshed::app::arrange
