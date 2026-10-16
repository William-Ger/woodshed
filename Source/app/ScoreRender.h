#pragma once

#include "ScoreLayout.h"
#include <juce_graphics/juce_graphics.h>
#include <functional>

/**
    Draws a score::Layout as notation: one horizontal system (Logic's linear view) with staff lines,
    clefs, key and time signatures, bar lines and numbers, noteheads, stems, flags, beams, rests,
    accidentals, dots, ties and ledger lines. Every glyph is a juce::Path drawn here, in staff
    spaces, so the score scales cleanly with zoom.

    No Component code: the Score editor positions it, and the bench renders it to an image.
    Coordinates are the renderer's own, starting at (0, 0) at the system's top left.
*/
namespace woodshed::score
{

namespace glyphs
{
    /** Built in staff spaces; scale by the staff space and translate to draw. Each one's origin
        is described where it's built (ScoreRender.cpp). */
    const juce::Path& trebleClef();
    const juce::Path& bassClef();
    const juce::Path& percussionClef();
    const juce::Path& filledHead();
    const juce::Path& halfHead();
    const juce::Path& wholeHead();
    const juce::Path& crossHead();
    const juce::Path& sharp();
    const juce::Path& flat();
    const juce::Path& natural();
    const juce::Path& flag();
    const juce::Path& wholeRest();
    const juce::Path& halfRest();
    const juce::Path& quarterRest();
    /** Eighth (1), sixteenth (2) and thirty-second (3) rests. */
    const juce::Path& shortRest (int flags);
    /** Time signature digits 0..9, about two staff spaces tall, centred on the origin. */
    const juce::Path& digit (int d);
    /** A brace for a grand staff, one unit tall (scale to the system). */
    const juce::Path& brace();

    constexpr float headWidth = 1.18f, wholeHeadWidth = 1.62f;
}

struct Style
{
    juce::Colour paper { 0xfff5f2ea }, ink { 0xff1c1b1a }, lines { 0xff8a8782 }, faint { 0xff8a8782 },
                 selected { 0xffe08a12 };
    juce::Font numberFont { juce::FontOptions (11.0f) };
};

class Renderer
{
public:
    /** Lays out the drawing: `staffSpace` px between staff lines, `pixelsPerTick` horizontally. */
    void setLayout (const Layout&, float staffSpace, double pixelsPerTick);
    const Layout& getLayout() const                     { return layout; }

    float getWidth() const                              { return width; }
    float getHeight() const                             { return height; }
    float getStaffSpace() const                         { return sp; }
    /** Where the first bar starts (after the clef, key and time signature). */
    float getContentLeft() const                        { return contentLeft; }

    float xForTick (double songTick) const;
    double tickAtX (float x) const;
    float yForStep (int staff, int step) const;
    float staffTop (int staff) const                    { return staffTops[(size_t) staff]; }
    /** The staff nearest a y position, and the staff step there. */
    int staffAt (float y) const;
    int stepAt (int staff, float y) const;

    /** The note (its InputNote id) whose notehead is at a point, or -1. */
    int noteAt (juce::Point<float>) const;
    /** The notehead of a note, for scrolling it into view. */
    juce::Rectangle<float> noteBounds (int id) const;

    void paint (juce::Graphics&, const Style&, const std::function<bool (int id)>& isSelected,
                juce::Range<float> visibleX) const;

private:
    struct Head { float x, y; int index; };
    struct Geometry
    {
        float x = 0.0f, stemX = 0.0f, stemNoteY = 0.0f, stemTipY = 0.0f;
        std::vector<Head> heads;           // in Element::notes order
        float left = 0.0f, right = 0.0f;   // notehead extent
    };

    void placeElements();
    void placeBeams();
    float headWidthOf (const Element&) const;
    float middleY (int staff) const;

    void drawSystem (juce::Graphics&, const Style&, juce::Range<float>) const;
    void drawElement (juce::Graphics&, const Style&, size_t index, const std::function<bool (int)>&) const;
    void drawBeam (juce::Graphics&, const Beam&) const;
    void drawTies (juce::Graphics&, const Style&, size_t index) const;
    void drawTuplets (juce::Graphics&, const Style&, juce::Range<float>) const;
    void fill (juce::Graphics&, const juce::Path&, float x, float y, float scale = 1.0f, bool flipY = false) const;

    Layout layout;
    float sp = 9.0f;
    double ppt = 0.1;
    float width = 0.0f, height = 0.0f, contentLeft = 0.0f, clefX = 0.0f, keyX = 0.0f, timeX = 0.0f;
    float padLeft = 0.0f, padRight = 0.0f;
    std::vector<float> staffTops, barX, barWidth;
    std::vector<Geometry> geometry;
    struct Hit { juce::Rectangle<float> area; int id; };
    std::vector<Hit> hits;
};

} // namespace woodshed::score
