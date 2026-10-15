#include "Theme.h"
#include "FontData.h"

namespace woodshed::theme
{

namespace
{
juce::Typeface::Ptr load (const void* data, int size)
{
    return juce::Typeface::createSystemTypefaceFor (data, (size_t) size);
}

struct Faces
{
    juce::Typeface::Ptr regular  = load (FontData::InterRegular_ttf, FontData::InterRegular_ttfSize);
    juce::Typeface::Ptr medium   = load (FontData::InterMedium_ttf, FontData::InterMedium_ttfSize);
    juce::Typeface::Ptr semibold = load (FontData::InterSemiBold_ttf, FontData::InterSemiBold_ttfSize);
    juce::Typeface::Ptr bold     = load (FontData::InterBold_ttf, FontData::InterBold_ttfSize);
    juce::Typeface::Ptr monoMedium = load (FontData::JetBrainsMonoMedium_ttf, FontData::JetBrainsMonoMedium_ttfSize);
    juce::Typeface::Ptr monoSemibold = load (FontData::JetBrainsMonoSemiBold_ttf, FontData::JetBrainsMonoSemiBold_ttfSize);

    static Faces& get()
    {
        static Faces faces;
        return faces;
    }
};

juce::Typeface::Ptr faceFor (Weight w)
{
    auto& f = Faces::get();
    switch (w)
    {
        case Weight::medium:   return f.medium;
        case Weight::semibold: return f.semibold;
        case Weight::bold:     return f.bold;
        case Weight::regular:  break;
    }
    return f.regular;
}
}

juce::Typeface::Ptr typeface (Weight w)          { return faceFor (w); }

juce::Typeface::Ptr monoTypeface (Weight w)
{
    auto& f = Faces::get();
    return w == Weight::regular || w == Weight::medium ? f.monoMedium : f.monoSemibold;
}

juce::Font font (float height, Weight weight)
{
    return juce::FontOptions (faceFor (weight)).withHeight (height);
}

juce::Font readout (float height, Weight weight)
{
    return juce::FontOptions (monoTypeface (weight)).withHeight (height);
}

const juce::Array<juce::Colour>& trackColours()
{
    static const juce::Array<juce::Colour> colours {
        juce::Colour (0xffff9f0a), juce::Colour (0xff0a84ff), juce::Colour (0xff32d74b), juce::Colour (0xffff375f),
        juce::Colour (0xffbf5af2), juce::Colour (0xffffd60a), juce::Colour (0xff64d2ff), juce::Colour (0xffff6b3d),
        juce::Colour (0xffac8e68), juce::Colour (0xff5e5ce6)
    };
    return colours;
}

void drawRaised (juce::Graphics& g, juce::Rectangle<float> r, float radius, juce::Colour fill)
{
    g.setColour (fill);
    g.fillRoundedRectangle (r, radius);
    g.setColour (separator.withAlpha (0.7f));
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);

    // A single line of light along the top edge.
    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (r.withHeight (radius + 1.0f).toNearestInt());
    g.setColour (highlight);
    g.drawRoundedRectangle (r.reduced (1.0f), radius - 1.0f, 1.0f);
}

void drawInset (juce::Graphics& g, juce::Rectangle<float> r, float radius, juce::Colour fill)
{
    g.setColour (fill);
    g.fillRoundedRectangle (r, radius);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    // The lit lower lip of a recess.
    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (r.withTrimmedTop (r.getHeight() - radius - 1.0f).toNearestInt());
    g.setColour (highlight);
    g.drawRoundedRectangle (r.reduced (0.5f).translated (0.0f, 0.5f), radius, 1.0f);
}

} // namespace woodshed::theme
