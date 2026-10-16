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
    drag an edge to trim, Alt-drag to copy, drag a top corner to fade in or out (as Logic does).
    Snaps to beats unless Cmd is held. Audio files dropped
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
    enum class Part { none, body, leftEdge, rightEdge, fadeIn, fadeOut };
    enum class Gesture { none, pendingRegion, move, trimLeft, trimRight, fadeIn, fadeOut, pendingEmpty, rubber, automation };

    struct Hit
    {
        juce::ValueTree region;
        int trackIndex = -1;
        Part part = Part::none;
    };

    struct RegionLook
    {
        juce::int64 start = 0, offset = 0, length = 0;
        juce::int64 fadeIn = 0, fadeOut = 0;
        bool selected = false, ghost = false, muted = false;
    };

    /** The fades as stored, fitted to `length` the way the engine fits them. */
    static void fitFades (juce::int64 length, juce::int64& fadeIn, juce::int64& fadeOut);

    Hit regionAt (juce::Point<int>) const;
    juce::Rectangle<float> regionRect (int trackIndex, juce::int64 start, juce::int64 length) const;
    void drawGrid (juce::Graphics&, int contentBottom) const;
    void drawRegion (juce::Graphics&, const juce::ValueTree& region, int trackIndex, const RegionLook&);
    void drawWave (juce::Graphics&, const juce::File&, juce::int64 fileSampleAtLeft, juce::Rectangle<float> area,
                   juce::Colour, const RegionLook&, const juce::ValueTree& region);
    void drawNotes (juce::Graphics&, const juce::ValueTree& region, juce::Rectangle<float> rect,
                    juce::Rectangle<float> area, juce::Colour, const RegionLook&) const;
    void drawFades (juce::Graphics&, juce::Rectangle<float> rect, juce::Rectangle<float> wave, const RegionLook&) const;
    void drawTakes (juce::Graphics&);
    void drawLivePeaks (juce::Graphics&, const daw::LivePeaks&, juce::Rectangle<float> wave, juce::int64 start) const;
    void drawHint (juce::Graphics&, int contentBottom) const;
    void showRegionMenu();
    void showRegionSettings();
    void drawLoopRepeats (juce::Graphics&, const juce::ValueTree& region, int trackIndex, const RegionLook&);

    // Automation view: each lane shows its track's chosen parameter as a line of points.
    void drawAutomation (juce::Graphics&);
    bool automationMouseDown (const juce::MouseEvent&);
    void automationDrag (const juce::MouseEvent&);
    float autoNorm (const daw::Project::AutoParam&, float value) const;
    float autoValue (const daw::Project::AutoParam&, float norm) const;
    juce::Range<float> autoSpan (int trackIndex) const;
    static const daw::Project::AutoParam* findParam (const std::vector<daw::Project::AutoParam>&, const juce::String& id);
    int autoTrack = -1, autoIndex = -1;
    juce::String autoParamId;

    juce::int64 fileLengthOf (const juce::ValueTree& region);
    juce::int64 minimumLength() const;
    void commitMove (bool copy);
    void commitTrim();
    void commitFade();
    void removeFadesFromSelection();
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
    juce::int64 moveDelta = 0, trimStart = 0, trimOffset = 0, trimLength = 0, fadeLength = 0;
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
