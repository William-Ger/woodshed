#include "Icons.h"

namespace woodshed::icons
{

namespace
{
Icon filled (juce::Path p)      { return { std::move (p), Style::fill }; }
Icon outline (juce::Path p)     { return { std::move (p), Style::stroke }; }

juce::Path roundedTriangle (juce::Point<float> a, juce::Point<float> b, juce::Point<float> c, float corner)
{
    juce::Path p;
    p.addTriangle (a, b, c);
    return p.createPathWithRoundedCorners (corner);
}
}

Icon play()     { return filled (roundedTriangle ({ 7.0f, 4.5f }, { 19.5f, 12.0f }, { 7.0f, 19.5f }, 2.0f)); }

Icon stop()
{
    juce::Path p;
    p.addRoundedRectangle (6.0f, 6.0f, 12.0f, 12.0f, 2.0f);
    return filled (p);
}

Icon record()
{
    juce::Path p;
    p.addEllipse (5.5f, 5.5f, 13.0f, 13.0f);
    return filled (p);
}

Icon toStart()
{
    juce::Path p;
    p.addRoundedRectangle (5.0f, 6.0f, 2.4f, 12.0f, 1.0f);
    p.addPath (roundedTriangle ({ 19.0f, 6.0f }, { 19.0f, 18.0f }, { 9.0f, 12.0f }, 1.6f));
    return filled (p);
}

Icon back()
{
    juce::Path p;
    p.addPath (roundedTriangle ({ 12.0f, 6.0f }, { 12.0f, 18.0f }, { 3.5f, 12.0f }, 1.4f));
    p.addPath (roundedTriangle ({ 20.5f, 6.0f }, { 20.5f, 18.0f }, { 12.0f, 12.0f }, 1.4f));
    return filled (p);
}

Icon forward()
{
    juce::Path p;
    p.addPath (roundedTriangle ({ 3.5f, 6.0f }, { 3.5f, 18.0f }, { 12.0f, 12.0f }, 1.4f));
    p.addPath (roundedTriangle ({ 12.0f, 6.0f }, { 12.0f, 18.0f }, { 20.5f, 12.0f }, 1.4f));
    return filled (p);
}

Icon loop()
{
    // Two arrows chasing each other round a rounded rectangle.
    juce::Path p;
    p.startNewSubPath (6.0f, 15.5f);
    p.lineTo (6.0f, 11.0f);
    p.quadraticTo (6.0f, 8.0f, 9.0f, 8.0f);
    p.lineTo (18.0f, 8.0f);
    p.startNewSubPath (15.5f, 5.5f);
    p.lineTo (18.0f, 8.0f);
    p.lineTo (15.5f, 10.5f);
    p.startNewSubPath (18.0f, 8.5f);
    p.lineTo (18.0f, 13.0f);
    p.quadraticTo (18.0f, 16.0f, 15.0f, 16.0f);
    p.lineTo (6.0f, 16.0f);
    p.startNewSubPath (8.5f, 13.5f);
    p.lineTo (6.0f, 16.0f);
    p.lineTo (8.5f, 18.5f);
    return outline (p);
}

Icon metronome()
{
    juce::Path p;
    p.startNewSubPath (9.5f, 4.0f);
    p.lineTo (14.5f, 4.0f);
    p.lineTo (18.5f, 20.0f);
    p.lineTo (5.5f, 20.0f);
    p.closeSubPath();
    p.startNewSubPath (7.0f, 15.5f);
    p.lineTo (17.0f, 15.5f);
    p.startNewSubPath (12.0f, 15.5f);
    p.lineTo (17.5f, 6.0f);
    return outline (p);
}

Icon countIn()
{
    juce::Path p;
    for (int i = 0; i < 4; ++i)
        p.addRoundedRectangle (3.5f + (float) i * 4.6f, i == 0 ? 6.0f : 9.0f, 3.0f, i == 0 ? 12.0f : 9.0f, 1.0f);
    return filled (p);
}

Icon gear()
{
    juce::Path p;
    const juce::Point<float> c { 12.0f, 12.0f };
    const int teeth = 8;
    for (int i = 0; i < teeth * 2; ++i)
    {
        const float a = juce::MathConstants<float>::twoPi * ((float) i + 0.5f) / (teeth * 2.0f);
        const float r = (i % 2 == 0) ? 9.0f : 7.0f;
        const float half = juce::MathConstants<float>::pi / (teeth * 2.0f) * 0.8f;
        const auto p1 = c.getPointOnCircumference (r, a - half);
        const auto p2 = c.getPointOnCircumference (r, a + half);
        if (i == 0) p.startNewSubPath (p1); else p.lineTo (p1);
        p.lineTo (p2);
    }
    p.closeSubPath();
    p = p.createPathWithRoundedCorners (0.8f);
    p.addEllipse (8.8f, 8.8f, 6.4f, 6.4f);
    p.setUsingNonZeroWinding (false);
    return filled (p);
}

Icon tuningFork()
{
    juce::Path p;
    p.startNewSubPath (8.0f, 3.5f);
    p.lineTo (8.0f, 11.0f);
    p.quadraticTo (8.0f, 15.0f, 12.0f, 15.0f);
    p.quadraticTo (16.0f, 15.0f, 16.0f, 11.0f);
    p.lineTo (16.0f, 3.5f);
    p.startNewSubPath (12.0f, 15.0f);
    p.lineTo (12.0f, 21.0f);
    return outline (p);
}

Icon mixer()
{
    juce::Path p;
    const float xs[] { 6.0f, 12.0f, 18.0f };
    const float knobs[] { 14.0f, 8.0f, 16.0f };
    for (int i = 0; i < 3; ++i)
    {
        p.startNewSubPath (xs[i], 4.0f);
        p.lineTo (xs[i], 20.0f);
    }
    juce::Path caps;
    for (int i = 0; i < 3; ++i)
        caps.addRoundedRectangle (xs[i] - 2.6f, knobs[i] - 1.6f, 5.2f, 3.2f, 1.0f);
    juce::PathStrokeType (1.6f).createStrokedPath (p, p);
    p.addPath (caps);
    return filled (p);
}

Icon rig()
{
    juce::Path p;
    p.addRoundedRectangle (6.0f, 3.5f, 12.0f, 17.0f, 2.5f);
    juce::PathStrokeType (1.6f).createStrokedPath (p, p);
    p.addEllipse (10.0f, 14.0f, 4.0f, 4.0f);
    p.addEllipse (8.6f, 6.6f, 2.6f, 2.6f);
    p.addEllipse (12.8f, 6.6f, 2.6f, 2.6f);
    return filled (p);
}

Icon tracks()
{
    juce::Path p;
    p.addRoundedRectangle (4.0f, 5.0f, 16.0f, 3.2f, 1.2f);
    p.addRoundedRectangle (4.0f, 10.4f, 11.0f, 3.2f, 1.2f);
    p.addRoundedRectangle (4.0f, 15.8f, 14.0f, 3.2f, 1.2f);
    return filled (p);
}

Icon plus()
{
    juce::Path p;
    p.startNewSubPath (12.0f, 5.0f); p.lineTo (12.0f, 19.0f);
    p.startNewSubPath (5.0f, 12.0f); p.lineTo (19.0f, 12.0f);
    return outline (p);
}

Icon chevronDown()
{
    juce::Path p;
    p.startNewSubPath (7.0f, 10.0f);
    p.lineTo (12.0f, 15.0f);
    p.lineTo (17.0f, 10.0f);
    return outline (p);
}

Icon headphones()
{
    juce::Path p;
    p.startNewSubPath (5.0f, 15.0f);
    p.lineTo (5.0f, 12.5f);
    p.cubicTo (5.0f, 3.0f, 19.0f, 3.0f, 19.0f, 12.5f);
    p.lineTo (19.0f, 15.0f);
    juce::PathStrokeType (1.6f).createStrokedPath (p, p);
    p.addRoundedRectangle (4.0f, 13.0f, 4.5f, 7.0f, 1.6f);
    p.addRoundedRectangle (15.5f, 13.0f, 4.5f, 7.0f, 1.6f);
    return filled (p);
}

Icon input()
{
    juce::Path p;
    p.addRoundedRectangle (9.5f, 3.0f, 5.0f, 9.0f, 1.2f);
    p.addRoundedRectangle (10.8f, 12.0f, 2.4f, 4.0f, 0.6f);
    p.addRoundedRectangle (11.2f, 16.0f, 1.6f, 5.0f, 0.8f);
    return filled (p);
}

Icon power()
{
    juce::Path p;
    p.addCentredArc (12.0f, 13.0f, 7.0f, 7.0f, 0.0f, 0.75f, juce::MathConstants<float>::twoPi - 0.75f, true);
    p.startNewSubPath (12.0f, 4.0f);
    p.lineTo (12.0f, 12.0f);
    return outline (p);
}

Icon speaker()
{
    juce::Path p;
    p.startNewSubPath (4.0f, 9.5f);
    p.lineTo (8.0f, 9.5f);
    p.lineTo (13.0f, 5.0f);
    p.lineTo (13.0f, 19.0f);
    p.lineTo (8.0f, 14.5f);
    p.lineTo (4.0f, 14.5f);
    p.closeSubPath();
    p = p.createPathWithRoundedCorners (1.0f);
    juce::Path waves;
    waves.addCentredArc (13.0f, 12.0f, 4.0f, 4.0f, 0.0f, 0.6f, juce::MathConstants<float>::pi - 0.6f, true);
    waves.addCentredArc (13.0f, 12.0f, 7.5f, 7.5f, 0.0f, 0.6f, juce::MathConstants<float>::pi - 0.6f, true);
    juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (waves, waves);
    p.addPath (waves);
    return filled (p);
}

Icon keyboard()
{
    juce::Path p;
    p.addRoundedRectangle (3.0f, 5.0f, 18.0f, 14.0f, 2.0f);
    for (float x : { 7.5f, 12.0f, 16.5f })
    {
        p.startNewSubPath (x, 11.5f);
        p.lineTo (x, 19.0f);
    }
    for (float x : { 6.2f, 10.7f, 15.2f })
        p.addRectangle (x, 5.0f, 2.6f, 6.5f);
    return outline (p);
}

Icon waveform()
{
    juce::Path p;
    const float heights[] { 3.0f, 8.0f, 14.0f, 6.0f, 11.0f, 16.0f, 9.0f, 4.0f };
    for (int i = 0; i < 8; ++i)
    {
        const float x = 4.0f + (float) i * 2.3f;
        p.startNewSubPath (x, 12.0f - heights[i] * 0.5f);
        p.lineTo (x, 12.0f + heights[i] * 0.5f);
    }
    return outline (p);
}

Icon pianoRoll()
{
    juce::Path p;
    p.addRoundedRectangle (4.0f, 6.0f, 7.0f, 3.0f, 1.0f);
    p.addRoundedRectangle (10.0f, 11.0f, 9.0f, 3.0f, 1.0f);
    p.addRoundedRectangle (6.0f, 16.0f, 6.0f, 3.0f, 1.0f);
    return filled (p);
}

void draw (juce::Graphics& g, const Icon& icon, juce::Rectangle<float> area, juce::Colour colour, float strokeWidth)
{
    const float side = juce::jmin (area.getWidth(), area.getHeight());
    const auto box = juce::Rectangle<float> (side, side).withCentre (area.getCentre());
    const auto t = juce::AffineTransform::scale (side / 24.0f).translated (box.getX(), box.getY());

    g.setColour (colour);
    if (icon.style == Style::fill)
    {
        g.fillPath (icon.path, t);
    }
    else
    {
        g.strokePath (icon.path, juce::PathStrokeType (strokeWidth * side / 24.0f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded), t);
    }
}

} // namespace woodshed::icons
