#include "Art.h"
#include "BinaryData.h"

namespace woodshed
{

namespace
{
juce::Image loadImage (const char* resource)
{
    int size = 0;
    if (const auto* data = BinaryData::getNamedResource (resource, size))
        return juce::ImageFileFormat::loadFrom (data, (size_t) size).convertedToFormat (juce::Image::ARGB);

    jassertfalse;
    return {};
}

juce::var loadJson (const char* resource)
{
    int size = 0;
    if (const auto* data = BinaryData::getNamedResource (resource, size))
        return juce::JSON::parse (juce::String::fromUTF8 (data, size));

    jassertfalse;
    return {};
}

juce::Rectangle<float> rect (const juce::var& v)
{
    if (! v.isArray() || v.size() < 4)
        return {};
    return { (float) v[0], (float) v[1], (float) v[2], (float) v[3] };
}

// The white paint in the shade layer was rendered at this albedo.
constexpr float renderedWhite = 0.82f;
}

Art& Art::get()
{
    static Art instance;
    return instance;
}

Art::Art()
{
    floor       = loadImage ("floor_jpg");
    board       = loadImage ("board_png");
    knob        = loadImage ("knob_png");
    footswitch  = loadImage ("footswitch_png");
    ledOn       = loadImage ("led_on_png");
    ledOff      = loadImage ("led_off_png");
    ampBody     = loadImage ("amp_body_png");
    cabBody     = loadImage ("cab_body_png");
    encShade    = loadImage ("enclosure_shade_png");
    encSpec     = loadImage ("enclosure_spec_png");
    encHardware = loadImage ("enclosure_hw_png");
    ampPanelShade = loadImage ("amp_panel_shade_png");
    ampPanelSpec  = loadImage ("amp_panel_spec_png");
    cabPanelShade = loadImage ("cab_panel_shade_png");
    cabPanelSpec  = loadImage ("cab_panel_spec_png");

    const auto enc = loadJson ("enclosure_json");
    enclosureBody = rect (enc["body"]);
    jackY = (float) enc.getProperty ("jackY", 0.3);

    const auto b = loadJson ("board_json");
    boardFrame = rect (b["frame"]);
    for (int i = 0; i < 2; ++i)
        boardRows[(size_t) i] = rect (b["rows"][i]);

    const auto k = loadJson ("knob_json");
    knobSkirt = (float) k["skirt"];
    knobBody  = (float) k["body"];
    knobCap   = (float) k["cap"];

    footswitchRadius = (float) loadJson ("footswitch_json")["radius"];

    const auto a = loadJson ("amp_json");
    ampRect = rect (a["body"]); ampPanel = rect (a["panel"]); ampGrille = rect (a["grille"]);

    const auto c = loadJson ("cab_json");
    cabRect = rect (c["body"]); cabPanel = rect (c["panel"]); cabGrille = rect (c["grille"]);
}

const juce::Image& Art::enclosure (juce::Colour colour)
{
    return tinted (enclosureCache, encShade, encSpec, encHardware, colour);
}

const juce::Image& Art::tinted (Cache& cache, const juce::Image& shade, const juce::Image& spec,
                                const juce::Image& hardware, juce::Colour colour)
{
    const auto key = colour.getARGB();
    if (auto it = cache.find (key); it != cache.end())
        return it->second;

    const int w = shade.getWidth(), h = shade.getHeight();
    juce::Image out (juce::Image::ARGB, w, h, true);

    const juce::Image::BitmapData src (shade, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData gloss (spec, juce::Image::BitmapData::readOnly);
    std::unique_ptr<juce::Image::BitmapData> hw;
    if (hardware.isValid())
        hw = std::make_unique<juce::Image::BitmapData> (hardware, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData dst (out, juce::Image::BitmapData::writeOnly);

    const float cr = colour.getFloatRed() / renderedWhite, cg = colour.getFloatGreen() / renderedWhite, cb = colour.getFloatBlue() / renderedWhite;

    // Pixels are premultiplied: paint = colour x diffuse shading, plus the clear-coat highlights,
    // then the screws and jacks composited over the top.
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const auto s = src.getPixelColour (x, y).getPixelARGB();
            const auto g = gloss.getPixelColour (x, y).getPixelARGB();

            float a = s.getAlpha() / 255.0f;
            float r = juce::jmin (a, s.getRed() / 255.0f * cr + g.getRed() / 255.0f);
            float gg = juce::jmin (a, s.getGreen() / 255.0f * cg + g.getGreen() / 255.0f);
            float b = juce::jmin (a, s.getBlue() / 255.0f * cb + g.getBlue() / 255.0f);

            if (hw != nullptr)
            {
                const auto m = hw->getPixelColour (x, y).getPixelARGB();
                const float ma = m.getAlpha() / 255.0f;
                r  = m.getRed() / 255.0f + r * (1.0f - ma);
                gg = m.getGreen() / 255.0f + gg * (1.0f - ma);
                b  = m.getBlue() / 255.0f + b * (1.0f - ma);
                a  = ma + a * (1.0f - ma);
            }

            juce::PixelARGB p;
            p.setARGB ((juce::uint8) juce::roundToInt (a * 255.0f), (juce::uint8) juce::roundToInt (r * 255.0f),
                       (juce::uint8) juce::roundToInt (gg * 255.0f), (juce::uint8) juce::roundToInt (b * 255.0f));
            *reinterpret_cast<juce::PixelARGB*> (dst.getPixelPointer (x, y)) = p;
        }
    }

    return cache.emplace (key, std::move (out)).first->second;
}

juce::Rectangle<float> Art::imageBoundsFor (juce::Rectangle<float> target, juce::Rectangle<float> region)
{
    const float w = target.getWidth() / region.getWidth();
    const float h = target.getHeight() / region.getHeight();
    return { target.getX() - region.getX() * w, target.getY() - region.getY() * h, w, h };
}

juce::Rectangle<float> Art::regionIn (juce::Rectangle<float> imageBounds, juce::Rectangle<float> region)
{
    return { imageBounds.getX() + region.getX() * imageBounds.getWidth(),
             imageBounds.getY() + region.getY() * imageBounds.getHeight(),
             region.getWidth() * imageBounds.getWidth(),
             region.getHeight() * imageBounds.getHeight() };
}

} // namespace woodshed
