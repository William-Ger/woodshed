#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace woodshed
{

/**
    The rendered UI art (art/build_assets.py) plus the layout data that came
    with it. Rects are normalised to their image (x, y, w, h; y down).
    Message thread only; loads lazily on first use.
*/
class Art
{
public:
    static Art& get();

    juce::Image floor, board, knob, footswitch, ledOn, ledOff, ampBody, cabBody;

    juce::Rectangle<float> enclosureBody, boardFrame, ampRect, ampPanel, ampGrille, cabRect, cabPanel, cabGrille;
    std::array<juce::Rectangle<float>, 2> boardRows;
    float jackY = 0.3f, knobSkirt = 0.37f, knobBody = 0.28f, knobCap = 0.16f, footswitchRadius = 0.41f;

    /** A pedal enclosure painted `colour`, at render resolution (cached). */
    const juce::Image& enclosure (juce::Colour colour);
    const juce::Image& ampPanelIn (juce::Colour colour)     { return tinted (ampPanelCache, ampPanelShade, ampPanelSpec, {}, colour); }
    const juce::Image& cabPanelIn (juce::Colour colour)     { return tinted (cabPanelCache, cabPanelShade, cabPanelSpec, {}, colour); }

    /** Where an image must be drawn so that its normalised `region` lands exactly on `target`. */
    static juce::Rectangle<float> imageBoundsFor (juce::Rectangle<float> target, juce::Rectangle<float> region);
    static juce::Rectangle<float> regionIn (juce::Rectangle<float> imageBounds, juce::Rectangle<float> region);

private:
    Art();

    using Cache = std::map<juce::uint32, juce::Image>;
    const juce::Image& tinted (Cache&, const juce::Image& shade, const juce::Image& spec, const juce::Image& hardware, juce::Colour);

    juce::Image encShade, encSpec, encHardware, ampPanelShade, ampPanelSpec, cabPanelShade, cabPanelSpec;
    Cache enclosureCache, ampPanelCache, cabPanelCache;
};

} // namespace woodshed
