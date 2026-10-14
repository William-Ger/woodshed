#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** Colours and type shared by every studio view; the rig's palette, extended. */
namespace woodshed::theme
{
    const juce::Colour window      { 0xff141311 };     // behind everything
    const juce::Colour bar         { 0xff12110f };     // control bar, footers
    const juce::Colour panel       { 0xff1c1a17 };     // track headers, mixer strips
    const juce::Colour panelLight  { 0xff24221f };     // raised controls, hover
    const juce::Colour lane        { 0xff191816 };     // timeline lanes
    const juce::Colour laneAlt     { 0xff1d1c19 };     // every other lane
    const juce::Colour gridBar     { 0xff2f2c28 };
    const juce::Colour gridBeat    { 0xff242220 };
    const juce::Colour outline     { 0xff2c2925 };
    const juce::Colour text        { 0xffe9e2d4 };
    const juce::Colour dimText     { 0xff8d857a };
    const juce::Colour accent      { 0xffe8a33d };     // selection, playhead, primary buttons
    const juce::Colour record      { 0xffe5483b };
    const juce::Colour mute        { 0xff5aa7d6 };
    const juce::Colour solo        { 0xffe3c34a };
    const juce::Colour loop        { 0xff5fbf7a };
    const juce::Colour meterLow    { 0xff5fbf7a };
    const juce::Colour meterMid    { 0xffe3c34a };
    const juce::Colour meterHigh   { 0xffe5483b };

    inline juce::Font font (float height, bool bold = false)
    {
        return juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain);
    }

    /** The digital readout in the control bar. */
    inline juce::Font readout (float height)
    {
        return juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, juce::Font::bold);
    }
}
