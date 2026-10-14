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
*/
class Ruler final : public juce::Component
{
public:
    explicit Ruler (ArrangeView&);

    static constexpr int numbersHeight = 24;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;

    /** Repaints just the playhead's old and new columns. */
    void playheadMoved (float oldX, float newX);

private:
    enum class Mode { none, scrub, loopCreate, loopMove, loopStart, loopEnd };

    Mode loopModeAt (juce::Point<int>) const;
    void setLoop (juce::int64 start, juce::int64 end, bool turnOn);
    void scrubTo (float x);

    ArrangeView& view;
    Mode mode = Mode::none;
    juce::int64 anchor = 0, originalStart = 0, originalEnd = 0;
    bool changed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Ruler)
};

} // namespace woodshed::app::arrange
