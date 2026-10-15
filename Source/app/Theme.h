#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/**
    The studio's design system. Graphite neutrals in the spirit of a modern desktop app, with the
    rig's amber kept as the one accent colour. The pedalboard itself stays photographic; the app
    around it is flat, quiet and precise.

    Spacing runs on a 4 px grid. Corner radii: 6 for controls, 8 for panels and regions' outer
    shape, 4 for small chips. Type is Inter (UI) and JetBrains Mono (readouts), bundled so the
    app looks the same on the Mac and on Windows.
*/
namespace woodshed::theme
{
    // Surfaces, back to front.
    const juce::Colour window      { 0xff17171a };     // behind everything
    const juce::Colour bar         { 0xff222226 };     // toolbar, pane headers, footers
    const juce::Colour barTop      { 0xff2a2a2f };     // toolbar gradient top
    const juce::Colour panel       { 0xff1f1f23 };     // track headers, mixer strips, sidebars
    const juce::Colour panelLight  { 0xff2b2b30 };     // raised: selected rows, cards
    const juce::Colour control     { 0xff36363c };     // button / field fill
    const juce::Colour controlHover{ 0xff414148 };
    const juce::Colour controlDown { 0xff2c2c31 };
    const juce::Colour lane        { 0xff1a1a1d };     // timeline lanes
    const juce::Colour laneAlt     { 0xff1d1d21 };     // every other lane
    const juce::Colour gridBar     { 0xff303036 };
    const juce::Colour gridBeat    { 0xff242428 };

    // Lines.
    const juce::Colour outline     { 0xff34343a };     // control borders
    const juce::Colour separator   { 0xff0c0c0e };     // hairlines between areas
    const juce::Colour highlight   { 0x0dffffff };     // 1 px top light on raised surfaces

    // Text.
    const juce::Colour text        { 0xffececf0 };
    const juce::Colour dimText     { 0xffa0a0a8 };     // secondary
    const juce::Colour faintText   { 0xff6c6c74 };     // tertiary, disabled

    // Meaning.
    const juce::Colour accent      { 0xfff2a33a };     // selection, playhead, primary, "on"
    const juce::Colour accentDim   { 0x33f2a33a };     // selection fills
    const juce::Colour record      { 0xffff453a };
    const juce::Colour mute        { 0xff5ac8fa };
    const juce::Colour solo        { 0xffffd60a };
    const juce::Colour loop        { 0xff32d74b };
    const juce::Colour monitor     { 0xffff9f0a };
    const juce::Colour meterLow    { 0xff32d74b };
    const juce::Colour meterMid    { 0xffffd60a };
    const juce::Colour meterHigh   { 0xffff453a };

    // Geometry.
    constexpr float radiusControl = 6.0f, radiusPanel = 8.0f, radiusChip = 4.0f;
    constexpr int toolbarHeight = 56;          // includes the window's traffic lights on the Mac
    constexpr int trafficLightInset = 84;      // keep this much clear at the toolbar's left (Mac)

    enum class Weight { regular, medium, semibold, bold };

    /** Inter at the given size and weight. */
    juce::Font font (float height, Weight weight = Weight::regular);
    /** Old call sites: bold = semibold. */
    inline juce::Font font (float height, bool bold)    { return font (height, bold ? Weight::semibold : Weight::regular); }
    /** JetBrains Mono, for numbers that change while you watch them (position, levels). */
    juce::Font readout (float height, Weight weight = Weight::semibold);

    juce::Typeface::Ptr typeface (Weight);
    juce::Typeface::Ptr monoTypeface (Weight = Weight::semibold);

    /** Track colour palette for new tracks and the colour menu. */
    const juce::Array<juce::Colour>& trackColours();

    /** A raised surface: fill, 1 px top highlight, hairline border. */
    void drawRaised (juce::Graphics&, juce::Rectangle<float>, float radius, juce::Colour fill);
    /** A recessed surface (fields, readouts, meter wells). */
    void drawInset (juce::Graphics&, juce::Rectangle<float>, float radius, juce::Colour fill = juce::Colour (0xff111113));
}
