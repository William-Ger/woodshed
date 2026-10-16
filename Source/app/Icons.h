#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/**
    The studio's icon set, drawn as paths in a 24 x 24 box (like a symbol font) so they stay sharp
    at any size. Fill the "filled" ones; stroke the "outline" ones at about 1.6 px per 24 units.
    Use icons::draw() to fit one into a rectangle.
*/
namespace woodshed::icons
{
    enum class Style { fill, stroke };

    struct Icon
    {
        juce::Path path;
        Style style = Style::fill;
    };

    Icon play();
    Icon stop();
    Icon record();
    Icon toStart();
    Icon back();          // one bar back
    Icon forward();       // one bar forward
    Icon loop();
    Icon metronome();
    Icon countIn();       // "1 2 3 4" bar
    Icon gear();
    Icon tuningFork();
    Icon mixer();         // three faders
    Icon rig();           // a stomp box
    Icon tracks();        // stacked lanes
    Icon plus();
    Icon chevronDown();
    Icon headphones();
    Icon input();         // a jack plug
    Icon power();
    Icon speaker();
    Icon keyboard();      // a few piano keys: instrument tracks
    Icon waveform();      // audio tracks
    Icon pianoRoll();     // the note editor: bars on a grid

    /** Draws an icon centred in `area`, scaled to fit, in `colour`. */
    void draw (juce::Graphics&, const Icon&, juce::Rectangle<float> area, juce::Colour colour, float strokeWidth = 1.6f);
}
