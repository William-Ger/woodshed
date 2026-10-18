#include "FlexLane.h"
#include "ArrangeView.h"
#include "../flex/Edits.h"
#include "../flex/Renderer.h"

namespace woodshed::app::arrange
{

namespace ids = daw::ids;
namespace fids = woodshed::flex::ids;

namespace
{
constexpr float handleRadius = 5.0f, transientReach = 10.0f;

bool isFlexRegion (const juce::ValueTree& region)
{
    return region.hasProperty (ids::file) && ! daw::Project::isMidiRegion (region) && ! daw::Project::isTakeFolder (region)
        && (bool) region.getParent()[fids::flexOn];
}
}

FlexLane::FlexLane (ArrangeView& v) : view (v) {}

float FlexLane::xForRegionTime (double dst, juce::int64 start, juce::int64 offset) const
{
    return view.xForSample ((double) (start - offset) + dst);
}

std::vector<double> FlexLane::transientsOf (const juce::ValueTree& region) const
{
    auto& p = view.project();
    const auto file = p.resolve (region[ids::file].toString());
    auto& engine = view.studio.engine();
    auto found = engine.getFlexRenderer().analysis().transients (file.getFullPathName(), engine.getAudioPool().get (file), p.grid().sampleRate);
    return found != nullptr ? *found : std::vector<double>();
}

juce::int64 FlexLane::playedLength (const juce::ValueTree& region, juce::int64 fileLength) const
{
    if (! woodshed::flex::isProcessed (region))
        return fileLength;
    return woodshed::flex::renderedLength (woodshed::flex::warpOf (region), fileLength);
}

//==============================================================================
bool FlexLane::drawWave (juce::Graphics& g, const juce::ValueTree& region, juce::Rectangle<float> rect, juce::Rectangle<float> wave,
                         juce::Colour colour, juce::int64 lookStart, juce::int64 lookOffset)
{
    if (! woodshed::flex::isProcessed (region))
        return false;

    auto& p = view.project();
    auto& engine = view.studio.engine();
    const auto file = p.resolve (region[ids::file].toString());
    const auto audio = engine.getFlexRenderer().audioFor (region, file, engine.getAudioPool().get (file), p.grid().sampleRate, false);
    if (audio == nullptr)
        return false;

    // What it plays: min and max of the render under each pixel.
    const auto clip = g.getClipBounds().toFloat();
    const auto area = wave.withX (rect.getX()).withRight (wave.getRight()).getIntersection (clip.withY (wave.getY()).withHeight (wave.getHeight()));
    if (area.getWidth() < 1.0f || area.getHeight() < 4.0f)
        return true;

    const double origin = (double) (lookStart - lookOffset);
    const float* x = audio->getReadPointer (0);
    const int n = audio->getNumSamples();
    const int px0 = (int) area.getX(), px1 = (int) area.getRight();

    std::vector<std::pair<float, float>> columns;
    float loudest = 1.0e-6f;
    for (int px = px0; px < px1; ++px)
    {
        const auto from = (juce::int64) (view.sampleAtX (px) - origin);
        const auto to = juce::jmax (from + 1, (juce::int64) (view.sampleAtX (px + 1) - origin));
        float lo = 0.0f, hi = 0.0f;
        const auto step = juce::jmax ((juce::int64) 1, (to - from) / 64);
        for (auto i = juce::jmax ((juce::int64) 0, from); i < juce::jmin ((juce::int64) n, to); i += step)
        {
            lo = juce::jmin (lo, x[i]);
            hi = juce::jmax (hi, x[i]);
        }
        loudest = juce::jmax (loudest, -lo, hi);
        columns.push_back ({ lo, hi });
    }

    const float scale = juce::jlimit (1.0f, 8.0f, 0.85f / loudest) * area.getHeight() * 0.5f;
    const float mid = area.getCentreY();
    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (area.toNearestInt());
    g.setColour (colour.withAlpha (0.3f));
    g.fillRect (area.withHeight (1.0f).withCentre (area.getCentre()));
    g.setColour (colour.interpolatedWith (juce::Colours::white, 0.45f).withAlpha (0.95f));
    for (size_t i = 0; i < columns.size(); ++i)
    {
        const float top = mid - columns[i].second * scale, bottom = mid - columns[i].first * scale;
        g.fillRect ((float) (px0 + (int) i), top, 1.0f, juce::jmax (1.0f, bottom - top));
    }
    return true;
}

void FlexLane::drawMarkers (juce::Graphics& g, const juce::ValueTree& region, juce::Rectangle<float> rect, juce::Rectangle<float> wave,
                            juce::int64 lookStart, juce::int64 lookOffset)
{
    if (! isFlexRegion (region) || rect.getWidth() < 12.0f)
        return;

    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (rect.toNearestInt());
    const auto warp = dragIndex >= 0 && region == dragRegion ? dragWarp : woodshed::flex::warpOf (region);

    // Transients: short ticks along the bottom.
    g.setColour (juce::Colours::white.withAlpha (0.28f));
    const auto clip = g.getClipBounds().toFloat();
    for (auto t : transientsOf (region))
    {
        const float x = xForRegionTime (warp.toDst (t), lookStart, lookOffset);
        if (x >= clip.getX() - 1.0f && x <= clip.getRight() + 1.0f)
            g.fillRect (juce::Rectangle<float> (x, wave.getBottom() - 6.0f, 1.0f, 6.0f));
    }

    // Markers: a line through the waveform with a handle at the top.
    for (size_t i = 0; i < warp.markers.size(); ++i)
    {
        const float x = xForRegionTime (warp.markers[i].dst, lookStart, lookOffset);
        if (x < rect.getX() + 1.0f || x > rect.getRight() - 1.0f)
            continue;
        const bool active = dragIndex == (int) i && region == dragRegion;
        g.setColour ((active ? theme::accent : juce::Colours::white).withAlpha (0.85f));
        g.fillRect (juce::Rectangle<float> (x - 0.5f, wave.getY(), 1.0f, wave.getHeight()));
        g.fillEllipse (juce::Rectangle<float> (handleRadius * 2.0f, handleRadius * 2.0f).withCentre ({ x, wave.getY() + handleRadius }));
        const double ratio = warp.ratioAt (warp.markers[i].src - 1.0);
        if (i > 0 && std::abs (ratio - 1.0) > 0.005 && wave.getHeight() > 20.0f)
        {
            // How much the stretch before it is sped up or slowed down.
            const float before = xForRegionTime (warp.markers[i - 1].dst, lookStart, lookOffset);
            if (x - before > 46.0f)
            {
                g.setFont (theme::readout (9.5f));
                g.setColour (juce::Colours::white.withAlpha (0.6f));
                g.drawText (juce::String (100.0 / ratio, 0) + "%", juce::Rectangle<float> (before, wave.getY(), x - before, 12.0f),
                            juce::Justification::centred, false);
            }
        }
    }
}

//==============================================================================
int FlexLane::markerAt (const juce::ValueTree& region, float x, juce::int64 start, juce::int64 offset) const
{
    const auto warp = woodshed::flex::warpOf (region);
    int best = -1;
    float bestDistance = handleRadius + 2.0f;
    for (size_t i = 0; i < warp.markers.size(); ++i)
        if (const float d = std::abs (xForRegionTime (warp.markers[i].dst, start, offset) - x); d < bestDistance)
        {
            bestDistance = d;
            best = (int) i;
        }
    return best;
}

bool FlexLane::wantsCursor (juce::Point<int> pos, const juce::ValueTree& region, juce::Rectangle<float> rect, float nameBarHeight) const
{
    if (! isFlexRegion (region))
        return false;
    const float top = rect.getY() + nameBarHeight;
    if ((float) pos.y < top || (float) pos.y > top + (rect.getBottom() - top) * 0.5f)
        return false;
    return markerAt (region, (float) pos.x, (juce::int64) region[ids::start], (juce::int64) region[ids::offset]) >= 0;
}

bool FlexLane::mouseDown (const juce::MouseEvent& e, const juce::ValueTree& region, juce::Rectangle<float> rect, float nameBarHeight)
{
    if (! isFlexRegion (region) || e.mods.isPopupMenu())
        return false;
    const float top = rect.getY() + nameBarHeight;
    if ((float) e.y < top || (float) e.y > top + (rect.getBottom() - top) * 0.5f)
        return false;       // the lower half moves the region as usual

    auto& p = view.project();
    const auto start = (juce::int64) region[ids::start], offset = (juce::int64) region[ids::offset];
    int index = markerAt (region, (float) e.x, start, offset);

    if (index >= 0 && e.mods.isAltDown())
    {
        p.undo.beginNewTransaction ("Remove Flex Marker");
        woodshed::flex::removeMarker (p, region, index);
        view.repaint();
        return true;
    }

    if (index < 0)
    {
        // A new marker: on the transient nearest the click, if there's one close by.
        const auto warp = woodshed::flex::warpOf (region);
        double src = warp.toSrc (view.sampleAtX (e.x) - (double) (start - offset));
        float nearest = transientReach;
        for (auto t : transientsOf (region))
            if (const float d = std::abs (xForRegionTime (warp.toDst (t), start, offset) - (float) e.x); d < nearest)
            {
                nearest = d;
                src = t;
            }
        p.undo.beginNewTransaction ("Add Flex Marker");
        index = woodshed::flex::addMarker (p, region, src);
    }

    if (! view.studio.getSelectedRegions().contains ((int) region[ids::id]))
        view.studio.setSelectedRegions ({ (int) region[ids::id] });
    dragRegion = region;
    dragIndex = index;
    dragWarp = woodshed::flex::warpOf (region);
    dragFrom = (double) e.x;
    dragDst = juce::isPositiveAndBelow (index, (int) dragWarp.markers.size()) ? dragWarp.markers[(size_t) index].dst : 0.0;
    view.setUserGesture (true);
    view.repaint();
    return index >= 0;
}

void FlexLane::mouseDrag (const juce::MouseEvent& e)
{
    if (dragIndex < 0 || ! juce::isPositiveAndBelow (dragIndex, (int) dragWarp.markers.size()))
        return;
    const double wanted = dragDst + ((double) e.x - dragFrom) * view.getSamplesPerPixel();
    dragWarp.markers[(size_t) dragIndex].dst = woodshed::flex::markerRange (dragWarp, dragIndex, view.project().grid().sampleRate).clipValue (wanted);
    view.repaint();
}

void FlexLane::mouseUp (const juce::MouseEvent&)
{
    if (dragIndex >= 0 && juce::isPositiveAndBelow (dragIndex, (int) dragWarp.markers.size())
        && std::abs (dragWarp.markers[(size_t) dragIndex].dst - dragDst) >= 1.0)
    {
        auto& p = view.project();
        p.undo.beginNewTransaction ("Move Flex Marker");
        woodshed::flex::moveMarker (p, dragRegion, dragIndex, dragWarp.markers[(size_t) dragIndex].dst);
    }
    dragIndex = -1;
    dragRegion = {};
    view.setUserGesture (false);
    view.repaint();
}

//==============================================================================
void FlexLane::addMenuItems (juce::PopupMenu& m, const juce::ValueTree& region)
{
    if (! region.hasProperty (ids::file) || daw::Project::isMidiRegion (region) || daw::Project::isTakeFolder (region))
        return;
    auto* cm = &view.studio.commands;
    m.addSeparator();
    m.addCommandItem (cm, cmd::showPitchEdit, "Open in Pitch Edit");
    m.addCommandItem (cm, cmd::quantizeAudio);
    m.addCommandItem (cm, cmd::followTempo);
    m.addCommandItem (cm, cmd::detectTempo);
    m.addCommandItem (cm, cmd::conformTempo);
    m.addCommandItem (cm, cmd::resetTiming);
}

} // namespace woodshed::app::arrange
