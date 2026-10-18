#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{
class ArrangeView;
}

namespace woodshed::app::arrange
{

/** The colour swatches offered for a track (the project's own palette, plus a few). */
const juce::Array<juce::Colour>& trackSwatches();
juce::Colour trackColour (const juce::ValueTree& track);

/**
    One track's header: colour strip, name (double-click to rename), mute / solo / record-arm /
    input-monitor buttons. Click selects, right-click opens the track menu, drag reorders.
*/
class TrackHeader final : public juce::Component,
                          private juce::ValueTree::Listener
{
public:
    TrackHeader (ArrangeView&, juce::ValueTree track);
    ~TrackHeader() override;

    const juce::ValueTree& getTrack() const                 { return track; }
    void startRename();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    /** A small toggle drawn in the theme's colours, bound to a boolean track property. */
    class Toggle final : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        enum class Glyph { text, dot, headphones, snowflake };
        Toggle (TrackHeader&, const juce::Identifier&, juce::String label, Glyph, juce::Colour on, juce::String undoName);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }

        /** Instead of flipping the property (freeze renders first). */
        std::function<void()> onClick;

    private:
        TrackHeader& owner;
        juce::Identifier property;
        juce::String label, undoName;
        Glyph glyph;
        juce::Colour onColour;
    };

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    juce::Rectangle<int> automationChip() const;
    void showAutomationMenu();

    /** Name weight and colour follow selection and mute. */
    void updateNameStyle (bool selected);

    // Track Stacks: members sit indented under their main, which has a disclosure triangle.
    int indent() const;
    juce::Rectangle<int> disclosureArea() const;
    juce::Rectangle<int> groupBadge() const;

    ArrangeView& view;
    juce::ValueTree track;
    juce::Label name;
    Toggle mute, solo, arm, monitor, freeze;
    bool dragging = false, styledSelected = false, styledMuted = false, styled = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeader)
};

} // namespace woodshed::app::arrange
