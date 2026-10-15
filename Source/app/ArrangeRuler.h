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
    Bar numbers (and beats when zoomed in) over the lanes. Click or drag in the numbers to move
    the playhead. The strip underneath holds the loop range: drag in empty space to make one,
    drag it to move it, drag its edges to resize, click it to switch looping on or off.
    At the bottom is the marker lane: double-click to add a marker, click one to go there,
    drag it to move it, double-click it to rename it, right-click for more.
*/
class Ruler final : public juce::Component
{
public:
    explicit Ruler (ArrangeView&);

    static constexpr int numbersHeight = 22, markerHeight = 18;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;

    /** Repaints just the playhead's old and new columns. */
    void playheadMoved (float oldX, float newX);

private:
    enum class Mode { none, scrub, loopCreate, loopMove, loopStart, loopEnd, markerEmpty, markerDrag };

    Mode loopModeAt (juce::Point<int>) const;
    bool inMarkerLane (juce::Point<int> p) const          { return p.y >= getHeight() - markerHeight; }
    /** The marker whose tag is under `p`, or 0. */
    int markerAt (juce::Point<int>) const;
    void drawMarkers (juce::Graphics&, juce::Rectangle<float> lane) const;
    void showMarkerMenu (int markerId);
    void startRename (int markerId);
    void finishRename (bool commit);
    void setLoop (juce::int64 start, juce::int64 end, bool turnOn);
    void scrubTo (float x);

    ArrangeView& view;
    Mode mode = Mode::none;
    juce::int64 anchor = 0, originalStart = 0, originalEnd = 0;
    bool changed = false;
    int grabbedMarker = 0;
    juce::int64 grabbedMarkerPosition = 0;

    juce::TextEditor renameEditor;
    int renamingMarker = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Ruler)
};

} // namespace woodshed::app::arrange
