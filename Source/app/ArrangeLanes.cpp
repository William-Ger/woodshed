#include "ArrangeLanes.h"
#include "ArrangeHeader.h"
#include "ArrangeView.h"
#include "ChannelStrip.h"

namespace woodshed::app::arrange
{

namespace ids = daw::ids;

namespace
{
    constexpr float regionInset = 2.0f, nameBarHeight = 16.0f, regionRadius = 5.0f;
    const juce::Colour darkInk { 0xff141416 };

    /** White or near-black, whichever reads better on `fill`. */
    juce::Colour inkFor (juce::Colour fill)
    {
        return fill.getPerceivedBrightness() > 0.62f ? darkInk : juce::Colours::white;
    }

    bool isAudioFile (const juce::String& path)
    {
        return juce::File (path).hasFileExtension ("wav;aif;aiff;flac");
    }

    juce::Array<juce::ValueTree> regionsOf (const juce::ValueTree& track)
    {
        juce::Array<juce::ValueTree> list;
        for (auto r : track)
            if (r.hasType (ids::REGION))
                list.add (r);
        return list;
    }
}

Lanes::Lanes (ArrangeView& v) : view (v)
{
    setOpaque (true);
}

//==============================================================================
juce::Rectangle<float> Lanes::regionRect (int trackIndex, juce::int64 start, juce::int64 length) const
{
    const float x1 = view.xForSample ((double) start);
    const float x2 = view.xForSample ((double) (start + length));
    const float y = (float) view.yForTrack (trackIndex) + regionInset;
    return { x1, y, juce::jmax (1.0f, x2 - x1), (float) ArrangeView::trackHeight - regionInset * 2.0f };
}

Lanes::Hit Lanes::regionAt (juce::Point<int> p) const
{
    Hit hit;
    const int index = view.trackIndexAtY (p.y);
    if (index < 0)
        return hit;

    hit.trackIndex = index;
    auto regions = regionsOf (view.project().track (index));
    for (int i = regions.size(); --i >= 0;)     // the last drawn is on top
    {
        auto& r = regions.getReference (i);
        const auto rect = regionRect (index, (juce::int64) r[ids::start], (juce::int64) r[ids::length]);
        if (! rect.contains (p.toFloat()))
            continue;

        hit.region = r;

        // Fade handles: in the name strip, at the end of each fade (the top corners when there's none).
        // MIDI regions have no fades.
        if ((float) p.y < rect.getY() + nameBarHeight && ! daw::Project::isMidiRegion (r))
        {
            const auto length = (juce::int64) r[ids::length];
            auto fadeIn = (juce::int64) r.getProperty (ids::fadeIn, 0), fadeOut = (juce::int64) r.getProperty (ids::fadeOut, 0);
            fitFades (length, fadeIn, fadeOut);
            const float pxPerSample = rect.getWidth() / (float) juce::jmax ((juce::int64) 1, length);
            const float zone = juce::jmin (8.0f, rect.getWidth() / 4.0f);
            const float inX = rect.getX() + (float) fadeIn * pxPerSample;
            const float outX = rect.getRight() - (float) fadeOut * pxPerSample;

            // The nearer handle wins, so a fade that reaches the far end can still be pulled back.
            const float toIn = std::abs ((float) p.x - inX), toOut = std::abs ((float) p.x - outX);
            if (juce::jmin (toIn, toOut) <= zone)
            {
                hit.part = toIn <= toOut ? Part::fadeIn : Part::fadeOut;
                return hit;
            }
        }

        const float grab = juce::jmin (6.0f, rect.getWidth() / 4.0f);
        if ((float) p.x < rect.getX() + grab)            hit.part = Part::leftEdge;
        else if ((float) p.x > rect.getRight() - grab)   hit.part = Part::rightEdge;
        else                                             hit.part = Part::body;
        return hit;
    }
    return hit;
}

void Lanes::fitFades (juce::int64 length, juce::int64& fadeIn, juce::int64& fadeOut)
{
    fadeOut = juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, length), fadeOut);
    fadeIn = juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, length - fadeOut), fadeIn);
}

juce::int64 Lanes::fileLengthOf (const juce::ValueTree& region)
{
    if (daw::Project::isMidiRegion (region) || daw::Project::isTakeFolder (region))
        return 0;       // no end to run into
    return view.waveforms.info (view.project().resolve (region[ids::file].toString())).length;
}

juce::int64 Lanes::minimumLength() const
{
    return juce::jmax ((juce::int64) 64, (juce::int64) std::llround (view.getSamplesPerPixel() * 6.0));
}

//==============================================================================
void Lanes::paint (juce::Graphics& g)
{
    auto& p = view.project();
    auto& state = p.state;
    const auto clip = g.getClipBounds();
    const int numTracks = p.numTracks();
    const int contentBottom = view.yForTrack (numTracks);
    const int selectedTrack = view.studio.getSelectedTrackId();

    // Lane backgrounds, alternating; the selected track's lane is tinted with its colour.
    g.fillAll (theme::window);
    for (int i = 0; i < numTracks; ++i)
    {
        const auto row = juce::Rectangle<int> (0, view.yForTrack (i), getWidth(), ArrangeView::trackHeight);
        if (! row.intersects (clip))
            continue;
        auto t = p.track (i);
        g.setColour (i % 2 == 0 ? theme::lane : theme::laneAlt);
        g.fillRect (row);
        if ((int) t[ids::id] == selectedTrack)
        {
            g.setColour (juce::Colours::white.withAlpha (0.018f));
            g.fillRect (row);
        }
        g.setColour (theme::separator.withAlpha (0.55f));
        g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);
    }

    // Loop range, behind the grid.
    if ((bool) state[ids::loopOn])
    {
        const auto loopStart = (juce::int64) state[ids::loopStart], loopEnd = (juce::int64) state[ids::loopEnd];
        if (loopEnd > loopStart)
        {
            const float x1 = view.xForSample ((double) loopStart), x2 = view.xForSample ((double) loopEnd);
            g.setColour (theme::loop.withAlpha (0.035f));
            g.fillRect (juce::Rectangle<float> (x1, 0.0f, x2 - x1, (float) getHeight()));
            g.setColour (theme::loop.withAlpha (0.22f));
            g.fillRect (juce::Rectangle<float> (x1, 0.0f, 1.0f, (float) getHeight()));
            g.fillRect (juce::Rectangle<float> (x2 - 1.0f, 0.0f, 1.0f, (float) getHeight()));
        }
    }

    drawGrid (g, contentBottom);

    // Regions. Ones being moved are drawn at their new place after the rest.
    const auto& selected = view.studio.getSelectedRegions();
    const bool moving = gesture == Gesture::move;
    const bool trimming = gesture == Gesture::trimLeft || gesture == Gesture::trimRight;
    const bool fading = gesture == Gesture::fadeIn || gesture == Gesture::fadeOut;

    for (int i = 0; i < numTracks; ++i)
    {
        const auto row = juce::Rectangle<int> (0, view.yForTrack (i), getWidth(), ArrangeView::trackHeight);
        if (! row.intersects (clip))
            continue;

        auto t = p.track (i);
        const bool muted = (bool) t[ids::mute];
        for (auto& r : regionsOf (t))
        {
            RegionLook look;
            look.start = (juce::int64) r[ids::start];
            look.offset = (juce::int64) r[ids::offset];
            look.length = (juce::int64) r[ids::length];
            look.fadeIn = (juce::int64) r.getProperty (ids::fadeIn, 0);
            look.fadeOut = (juce::int64) r.getProperty (ids::fadeOut, 0);
            look.selected = selected.contains ((int) r[ids::id]);
            look.muted = muted || (bool) r[ids::mute];

            if (moving && look.selected)
            {
                if (! copying)
                    look.ghost = true;      // an outline where it was
                else
                    look.selected = false;
            }
            else if (trimming && r == grabbed)
            {
                look.start = trimStart;
                look.offset = trimOffset;
                look.length = trimLength;
            }
            else if (fading && r == grabbed)
            {
                (gesture == Gesture::fadeIn ? look.fadeIn : look.fadeOut) = fadeLength;
            }

            drawLoopRepeats (g, r, i, look);
            drawRegion (g, r, i, look);
        }
    }

    if (moving)
    {
        for (int i = 0; i < numTracks; ++i)
            for (auto& r : regionsOf (p.track (i)))
                if (selected.contains ((int) r[ids::id]))
                {
                    const int target = juce::jlimit (0, numTracks - 1, i + trackDelta);
                    RegionLook look;
                    look.start = (juce::int64) r[ids::start] + moveDelta;
                    look.offset = (juce::int64) r[ids::offset];
                    look.length = (juce::int64) r[ids::length];
                    look.fadeIn = (juce::int64) r.getProperty (ids::fadeIn, 0);
                    look.fadeOut = (juce::int64) r.getProperty (ids::fadeOut, 0);
                    look.selected = true;
                    look.muted = (bool) p.track (target)[ids::mute] || (bool) r[ids::mute];
                    drawRegion (g, r, target, look);
                }
    }

    drawTakes (g);
    drawAutomation (g);

    // Rubber band.
    if (gesture == Gesture::rubber)
    {
        g.setColour (theme::accentDim.withMultipliedAlpha (0.45f));
        g.fillRoundedRectangle (rubber.toFloat(), 3.0f);
        g.setColour (theme::accent.withAlpha (0.7f));
        g.drawRoundedRectangle (rubber.toFloat().reduced (0.5f), 3.0f, 1.0f);
    }

    // Where a dropped file will land.
    if (dropping)
    {
        const int y = dropTrack >= 0 ? view.yForTrack (dropTrack) : contentBottom;
        const auto row = juce::Rectangle<int> (0, y, getWidth(), ArrangeView::trackHeight);
        g.setColour (theme::accent.withAlpha (0.08f));
        g.fillRect (row);
        g.setColour (theme::accent.withAlpha (0.6f));
        g.drawRect (row, 1);
        const float x = view.xForSample ((double) dropAt);
        g.setColour (theme::accent);
        g.fillRect (juce::Rectangle<float> (x, (float) row.getY(), 2.0f, (float) row.getHeight()));
        g.setFont (theme::font (12.0f, theme::Weight::semibold));
        g.drawText (dropTrack >= 0 ? "Drop to add to " + p.track (dropTrack)[ids::name].toString() : "Drop to add a new track",
                    row.withTrimmedLeft (juce::roundToInt (x) + 8).withWidth (260), juce::Justification::centredLeft);
    }

    drawHint (g, contentBottom);

    // Playhead.
    auto& engine = view.studio.engine();
    const float px = std::round (view.xForSample ((double) engine.getPosition()));
    if (px > -4.0f && px < (float) getWidth() + 4.0f)
    {
        const auto colour = engine.isRecording() ? theme::record : theme::accent;
        g.setColour (colour);
        g.fillRect (px, 0.0f, 1.0f, (float) getHeight());
    }
}

void Lanes::drawGrid (juce::Graphics& g, int contentBottom) const
{
    const auto grid = view.project().grid();
    const double spb = grid.samplesPerBeat();
    const double step = view.gridStepBeats();
    const int bpb = juce::jmax (1, grid.beatsPerBar);
    const auto clip = g.getClipBounds();

    const double firstBeat = std::floor (view.sampleAtX ((double) clip.getX()) / spb / step) * step;
    const double lastBeat = view.sampleAtX ((double) clip.getRight()) / spb;
    const float bottom = (float) juce::jmax (0, juce::jmin (getHeight(), contentBottom));

    for (double beat = juce::jmax (0.0, firstBeat); beat <= lastBeat + step; beat += step)
    {
        const float x = std::round (view.xForSample (beat * spb));
        const double inBar = std::fmod (beat, (double) bpb);
        const bool isBar = inBar < 1.0e-6 || bpb - inBar < 1.0e-6;
        const bool isBeat = std::abs (beat - std::round (beat)) < 1.0e-6;

        g.setColour (isBar ? theme::gridBar : (isBeat ? theme::gridBeat : theme::gridBeat.withAlpha (0.55f)));
        g.fillRect (x, 0.0f, 1.0f, bottom);

        // Faint continuation below the last track, so the empty space still reads as a timeline.
        if (isBar && bottom < (float) getHeight())
        {
            g.setColour (theme::gridBeat.withAlpha (0.5f));
            g.fillRect (x, bottom, 1.0f, (float) getHeight() - bottom);
        }
    }
}

void Lanes::drawRegion (juce::Graphics& g, const juce::ValueTree& region, int trackIndex, const RegionLook& look)
{
    const auto rect = regionRect (trackIndex, look.start, look.length);
    const auto clip = g.getClipBounds().toFloat();
    if (rect.getRight() < clip.getX() - 4.0f || rect.getX() > clip.getRight() + 4.0f)
        return;

    auto colour = trackColour (view.project().track (trackIndex));
    if (look.muted)
        colour = colour.withSaturation (0.12f).withMultipliedBrightness (0.7f);

    if (look.ghost)
    {
        g.setColour (colour.withAlpha (0.07f));
        g.fillRoundedRectangle (rect, regionRadius);
        g.setColour (colour.withAlpha (0.35f));
        g.drawRoundedRectangle (rect.reduced (0.5f), regionRadius, 1.0f);
        return;
    }

    // Keep enormous regions to the area being painted, with a margin so cut ends stay out of view.
    const auto body = rect.getIntersection ({ clip.getX() - 12.0f, rect.getY(), clip.getWidth() + 24.0f, rect.getHeight() });

    // Selected regions sit a little higher: a soft shadow under them.
    if (look.selected)
    {
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (body.translated (0.0f, 1.5f).expanded (0.5f), regionRadius + 0.5f);
    }

    // Body: the track colour, dark and translucent over the lane.
    g.setColour (theme::lane);
    g.fillRoundedRectangle (body, regionRadius);
    g.setColour (colour.withAlpha (look.selected ? 0.34f : 0.24f));
    g.fillRoundedRectangle (body, regionRadius);

    // Header strip.
    const auto strip = body.withHeight (nameBarHeight);
    const auto stripColour = colour.withMultipliedBrightness (look.selected ? 1.0f : 0.82f)
                                   .withMultipliedSaturation (look.selected ? 1.0f : 0.9f);
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (strip.toNearestInt());
        g.setColour (stripColour);
        g.fillRoundedRectangle (body, regionRadius);
    }

    // Name: stays in view when the region starts off the left edge.
    {
        const float textX = juce::jmax (rect.getX(), 0.0f) + 6.0f;
        const float textW = rect.getRight() - textX - 5.0f;
        if (textW > 14.0f)
        {
            g.setColour (inkFor (stripColour).withAlpha (0.92f));
            g.setFont (theme::font (11.0f, theme::Weight::semibold));
            auto title = region[ids::name].toString();
            if (daw::Project::isTakeFolder (region))
                title << "  (" << daw::Project::takes (region).size() << " takes)";
            g.drawText (title, juce::Rectangle<float> (textX, strip.getY(), textW, nameBarHeight),
                        juce::Justification::centredLeft, true);
        }
    }

    const auto wave = body.withTrimmedTop (nameBarHeight).reduced (0.0f, 3.0f);

    if (daw::Project::isMidiRegion (region))
    {
        drawNotes (g, region, rect, wave, colour, look);
    }
    else
    {
    if (daw::Project::isTakeFolder (region))
    {
        // The comp: each segment's slice of its take; a hairline where the takes switch.
        const auto takeList = daw::Project::takes (region);
        const auto origin = look.start - look.offset;
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (wave.toNearestInt());
        for (auto& c : daw::Project::getComp (region))
        {
            if (! juce::isPositiveAndBelow (c.take, takeList.size()))
                continue;
            const auto& take = takeList.getReference (c.take);
            const auto takeFrom = origin + (juce::int64) take[ids::start];
            const auto from = juce::jmax (origin + c.from, takeFrom, look.start);
            const auto to = juce::jmin (origin + c.to, takeFrom + (juce::int64) take[ids::length], look.start + look.length);
            if (to <= from)
                continue;
            const float x1 = view.xForSample ((double) from), x2 = view.xForSample ((double) to);
            drawWave (g, view.project().resolve (take[ids::file].toString()), (juce::int64) take[ids::offset] + (from - takeFrom),
                      wave.withX (x1).withWidth (x2 - x1), colour, look, region);
            if (from > look.start)
            {
                g.setColour (colour.withAlpha (0.7f));
                g.fillRect (juce::Rectangle<float> (x1, wave.getY(), 1.0f, wave.getHeight()));
            }
        }
    }
    else
    {
        drawWave (g, view.project().resolve (region[ids::file].toString()), look.offset, wave.withX (rect.getX()).withRight (wave.getRight()),
                  colour, look, region);
    }

    drawFades (g, rect, body.withTrimmedTop (nameBarHeight), look);
    }

    // Border: crisp in the track colour; brighter when selected.
    if (look.selected)
    {
        g.setColour (colour.interpolatedWith (juce::Colours::white, 0.75f));
        g.drawRoundedRectangle (body.reduced (0.5f), regionRadius, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawRoundedRectangle (body.reduced (1.5f), regionRadius - 1.0f, 1.0f);
    }
    else
    {
        g.setColour (colour.withAlpha (0.75f));
        g.drawRoundedRectangle (body.reduced (0.5f), regionRadius, 1.0f);
    }
}

void Lanes::drawWave (juce::Graphics& g, const juce::File& file, juce::int64 fileSampleAtLeft, juce::Rectangle<float> area,
                      juce::Colour colour, const RegionLook& look, const juce::ValueTree& region)
{
    // Only the part that's on screen; `area` starts where file sample `fileSampleAtLeft` plays.
    const auto clip = g.getClipBounds().toFloat();
    auto wave = area.getIntersection (clip.withY (area.getY()).withHeight (area.getHeight()));
    const auto info = view.waveforms.info (file);
    auto* thumb = info.length > 0 ? view.waveforms.thumbnail (file) : nullptr;

    if (thumb != nullptr && wave.getWidth() >= 1.0f && wave.getHeight() > 4.0f)
    {
        const double rate = info.sampleRate > 0.0 ? info.sampleRate : view.project().grid().sampleRate;
        const double sppx = view.getSamplesPerPixel();
        const double fromSample = (double) fileSampleAtLeft + (wave.getX() - area.getX()) * sppx;
        const double toSample = fromSample + wave.getWidth() * sppx;
        // Quiet DI takes are scaled up for display (by the file's peak), then by the region's gain.
        const auto peak = thumb->getApproximatePeak();
        const auto normalise = peak > 0.0f ? juce::jlimit (1.0f, 8.0f, 0.85f / peak) : 1.0f;
        const auto gain = normalise * juce::jlimit (0.25f, 4.0f, juce::Decibels::decibelsToGain ((float) (double) region.getProperty (ids::gain, 0.0)));

        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (wave.toNearestInt());
        g.setColour (colour.withAlpha (0.3f));
        g.fillRect (wave.withHeight (1.0f).withCentre (wave.getCentre()));
        g.setColour (colour.interpolatedWith (juce::Colours::white, look.selected ? 0.6f : 0.4f)
                           .withAlpha (look.muted ? 0.6f : 0.95f));
        thumb->drawChannel (g, wave.toNearestInt(), fromSample / rate, toSample / rate, 0, gain);
    }
    else if (wave.getHeight() > 10.0f && wave.getWidth() > 20.0f)
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (11.0f));
        g.drawText (file.existsAsFile() ? "Reading..." : "Missing audio file", wave.reduced (6.0f, 0.0f),
                    juce::Justification::centredLeft, true);
    }
}

void Lanes::drawLoopRepeats (juce::Graphics& g, const juce::ValueTree& region, int trackIndex, const RegionLook& look)
{
    if (! (bool) region[ids::loop] || look.ghost || look.length <= 0)
        return;

    // Lighter copies after the region, up to where its loop stops, each with a notch at its start.
    const auto end = view.project().loopEnd (region) + (look.start - (juce::int64) region[ids::start]);
    auto colour = trackColour (view.project().track (trackIndex));
    if (look.muted)
        colour = colour.withSaturation (0.12f);
    for (auto from = look.start + look.length; from < end; from += look.length)
    {
        const auto rect = regionRect (trackIndex, from, juce::jmin (look.length, end - from));
        if (rect.getX() > (float) getWidth())
            break;
        if (rect.getRight() < 0.0f)
            continue;
        g.setColour (colour.withAlpha (0.12f));
        g.fillRoundedRectangle (rect, regionRadius);
        g.setColour (colour.withAlpha (0.4f));
        g.drawRoundedRectangle (rect.reduced (0.5f), regionRadius, 1.0f);
        g.fillRect (juce::Rectangle<float> (rect.getX(), rect.getY() + 3.0f, 2.0f, nameBarHeight - 6.0f));
    }
}

void Lanes::drawNotes (juce::Graphics& g, const juce::ValueTree& region, juce::Rectangle<float> rect,
                       juce::Rectangle<float> area, juce::Colour colour, const RegionLook& look) const
{
    if (area.getHeight() < 4.0f || area.getWidth() < 1.0f)
        return;

    // The notes' pitch range, at least an octave, fills the region's height.
    int low = 127, high = 0;
    for (auto n : region)
        if (n.hasType (ids::NOTE))
        {
            low = juce::jmin (low, (int) n[ids::pitch]);
            high = juce::jmax (high, (int) n[ids::pitch]);
        }
    if (low > high)
        return;
    if (high - low < 12)
    {
        const int pad = (12 - (high - low)) / 2;
        low -= pad;
        high = low + 12;
    }

    const auto grid = view.project().grid();
    const double origin = (double) (look.start - look.offset);
    const double regionEnd = (double) (look.start + look.length);
    const float rowHeight = juce::jlimit (1.5f, 5.0f, area.getHeight() / (float) (high - low + 1));
    const float span = area.getHeight() - rowHeight;
    const auto clip = g.getClipBounds().toFloat();

    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (area.toNearestInt());
    g.setColour (colour.interpolatedWith (juce::Colours::white, look.selected ? 0.7f : 0.5f).withAlpha (look.muted ? 0.5f : 0.95f));

    for (auto n : region)
    {
        if (! n.hasType (ids::NOTE))
            continue;
        const auto note = daw::Project::noteFrom (n);
        const double on = origin + grid.fromTicks ((double) note.start);
        if (on < (double) look.start || on >= regionEnd)
            continue;
        const double off = juce::jmin (regionEnd, origin + grid.fromTicks ((double) (note.start + note.length)));
        const float x1 = view.xForSample (on), x2 = view.xForSample (off);
        if (x2 < clip.getX() || x1 > clip.getRight())
            continue;
        const float y = area.getY() + span * (1.0f - (float) (note.pitch - low) / (float) juce::jmax (1, high - low));
        g.fillRect (juce::Rectangle<float> (x1, y, juce::jmax (1.5f, x2 - x1 - 0.5f), rowHeight));
    }
    juce::ignoreUnused (rect);
}

void Lanes::drawFades (juce::Graphics& g, juce::Rectangle<float> rect, juce::Rectangle<float> wave, const RegionLook& look) const
{
    auto fadeIn = look.fadeIn, fadeOut = look.fadeOut;
    fitFades (look.length, fadeIn, fadeOut);
    const float pxPerSample = rect.getWidth() / (float) juce::jmax ((juce::int64) 1, look.length);

    // The faded part is shaded above the gain curve (equal-power, like the engine's).
    const auto drawFade = [&] (float fromX, float width, bool rising)
    {
        if (width < 1.0f)
            return;
        const int steps = juce::jlimit (8, 64, juce::roundToInt (width / 3.0f));
        juce::Path curve, shade;
        for (int i = 0; i <= steps; ++i)
        {
            const float t = (float) i / (float) steps;
            const float level = std::sin ((rising ? t : 1.0f - t) * juce::MathConstants<float>::halfPi);
            const juce::Point<float> pt (fromX + width * t, wave.getBottom() - level * wave.getHeight());
            if (i == 0) { curve.startNewSubPath (pt); shade.startNewSubPath (pt); }
            else        { curve.lineTo (pt); shade.lineTo (pt); }
        }
        shade.lineTo (fromX + width, wave.getY());
        shade.lineTo (fromX, wave.getY());
        shade.closeSubPath();

        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillPath (shade);
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.strokePath (curve, juce::PathStrokeType (1.2f));
    };

    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (wave.toNearestInt());
        drawFade (rect.getX(), (float) fadeIn * pxPerSample, true);
        drawFade (rect.getRight() - (float) fadeOut * pxPerSample, (float) fadeOut * pxPerSample, false);
    }

    // Handles on a selected region, in the name strip where they're grabbed.
    if (look.selected && rect.getWidth() > 24.0f)
    {
        const float y = rect.getY() + nameBarHeight * 0.5f;
        for (auto x : { rect.getX() + (float) fadeIn * pxPerSample, rect.getRight() - (float) fadeOut * pxPerSample })
        {
            const auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ juce::jlimit (rect.getX() + 4.0f, rect.getRight() - 4.0f, x), y });
            g.setColour (juce::Colours::white);
            g.fillEllipse (dot);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.drawEllipse (dot, 1.0f);
        }
    }
}

void Lanes::drawTakes (juce::Graphics& g)
{
    auto& engine = view.studio.engine();
    if (! engine.isRecording())
        return;

    // Drawn where the take will land once the interface latency is taken out, so it doesn't jump on stop.
    const auto recordStart = engine.getRecordStart();
    const auto start = recordStart - engine.getLiveTakeOffset();
    const auto length = juce::jmax ((juce::int64) 1, engine.getRecordedLength());
    auto& p = view.project();

    for (int i = 0; i < p.numTracks(); ++i)
    {
        if (! (bool) p.track (i)[ids::arm])
            continue;

        const bool instrument = daw::Project::isInstrumentTrack (p.track (i));
        const auto trackStart = instrument ? recordStart : start;
        const auto rect = regionRect (i, trackStart, length);
        g.setColour (theme::lane);
        g.fillRoundedRectangle (rect, regionRadius);
        g.setColour (theme::record.withAlpha (0.22f));
        g.fillRoundedRectangle (rect, regionRadius);
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (rect.withHeight (nameBarHeight).toNearestInt());
            g.setColour (theme::record.withMultipliedBrightness (0.9f));
            g.fillRoundedRectangle (rect, regionRadius);
        }
        if (auto* peaks = engine.getLivePeaks ((int) p.track (i)[ids::id]))
            drawLivePeaks (g, *peaks, rect.withTrimmedTop (nameBarHeight).reduced (0.0f, 3.0f), start);

        // Notes played so far, on the track MIDI is recording into.
        if (instrument && (int) p.track (i)[ids::id] == engine.getMidiTarget())
        {
            const auto area = rect.withTrimmedTop (nameBarHeight).reduced (0.0f, 3.0f);
            const auto& notes = engine.getRecordedNotes();
            int low = 127, high = 0;
            for (auto& n : notes) { low = juce::jmin (low, n.pitch); high = juce::jmax (high, n.pitch); }
            if (low <= high)
            {
                if (high - low < 12) { low -= (12 - (high - low)) / 2; high = low + 12; }
                const float row = juce::jlimit (1.5f, 5.0f, area.getHeight() / (float) (high - low + 1));
                const auto now = recordStart + length;
                g.setColour (juce::Colours::white.withAlpha (0.9f));
                for (auto& n : notes)
                {
                    const float x1 = view.xForSample ((double) n.start);
                    const float x2 = view.xForSample ((double) (n.end >= 0 ? n.end : now));
                    const float y = area.getY() + (area.getHeight() - row) * (1.0f - (float) (n.pitch - low) / (float) (high - low));
                    g.fillRect (juce::Rectangle<float> (x1, y, juce::jmax (1.5f, x2 - x1), row));
                }
            }
        }

        g.setColour (theme::record.withAlpha (0.9f));
        g.drawRoundedRectangle (rect.reduced (0.5f), regionRadius, 1.0f);
        if (rect.getWidth() > 60.0f)
        {
            const float textX = juce::jmax (rect.getX(), 0.0f) + 6.0f;
            g.setColour (juce::Colours::white);
            g.setFont (theme::font (11.0f, theme::Weight::semibold));
            g.drawText ("Recording", juce::Rectangle<float> (textX, rect.getY(), rect.getRight() - textX - 5.0f, nameBarHeight),
                        juce::Justification::centredLeft, true);
        }
    }
}

void Lanes::drawLivePeaks (juce::Graphics& g, const daw::LivePeaks& peaks, juce::Rectangle<float> wave, juce::int64 start) const
{
    const int count = peaks.size();
    if (count == 0 || wave.getHeight() < 4.0f)
        return;

    // Quiet DI scaled up for display, as finished takes are (by the loudest peak so far).
    float loudest = 0.0f;
    for (int k = 0; k < count; ++k)
        loudest = juce::jmax (loudest, peaks[k]);
    const float scale = loudest > 0.0f ? juce::jlimit (1.0f, 8.0f, 0.85f / loudest) : 1.0f;

    const auto clip = g.getClipBounds().toFloat().getIntersection (wave);
    if (clip.isEmpty())
        return;

    const double hop = daw::LivePeaks::hop;
    const float mid = wave.getCentreY(), half = wave.getHeight() * 0.5f;
    juce::RectangleList<float> bars;

    for (float x = std::floor (clip.getX()); x < clip.getRight(); x += 1.0f)
    {
        const int from = (int) std::floor ((view.sampleAtX ((double) x) - (double) start) / hop);
        const int to = juce::jmax (from + 1, (int) std::ceil ((view.sampleAtX ((double) x + 1.0) - (double) start) / hop));
        if (to <= 0 || from >= count)
            continue;

        float peak = 0.0f;
        for (int k = juce::jmax (0, from); k < juce::jmin (count, to); ++k)
            peak = juce::jmax (peak, peaks[k]);

        const float h = juce::jmax (0.5f, juce::jmin (1.0f, peak * scale) * half);
        bars.addWithoutMerging ({ x, mid - h, 1.0f, h * 2.0f });
    }

    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.fillRectList (bars);
}

void Lanes::drawHint (juce::Graphics& g, int contentBottom) const
{
    auto& p = view.project();
    if (view.studio.engine().isRecording() || dropping || p.contentEnd() > 0)
        return;

#if JUCE_MAC
    const juce::StringArray newTrackKeys { "Cmd", "Option", "N" };
#else
    const juce::StringArray newTrackKeys { "Ctrl", "Alt", "N" };
#endif

    auto area = getLocalBounds().withTrimmedTop (juce::jmax (0, contentBottom));
    if (area.getHeight() < 76)
        area = getLocalBounds().withTrimmedTop (juce::jmax (0, getHeight() - 84));

    // One line of key caps and labels, centred; a quieter line under it.
    struct Piece { juce::String text; bool key; };
    std::vector<Piece> pieces { { "Press", false }, { "R", true }, { "to record, or", false } };
    for (auto& k : newTrackKeys)
        pieces.push_back ({ k, true });
    pieces.push_back ({ "for a new track", false });

    const auto labelFont = theme::font (13.0f, theme::Weight::medium);
    const auto keyFont = theme::font (11.0f, theme::Weight::semibold);
    constexpr float gap = 6.0f, keyPad = 6.0f, keyHeight = 20.0f;

    auto widthOf = [&] (const Piece& piece)
    {
        return piece.key ? juce::jmax (keyHeight, juce::GlyphArrangement::getStringWidth (keyFont, piece.text) + keyPad * 2.0f)
                         : juce::GlyphArrangement::getStringWidth (labelFont, piece.text);
    };

    // Keys of one shortcut sit closer together.
    auto gapAfter = [&] (size_t i)
    {
        return i + 1 < pieces.size() && pieces[i].key && pieces[i + 1].key ? 3.0f : gap;
    };

    float total = 0.0f;
    for (size_t i = 0; i < pieces.size(); ++i)
        total += widthOf (pieces[i]) + (i + 1 < pieces.size() ? gapAfter (i) : 0.0f);

    const auto centre = area.toFloat().getCentre();
    float x = centre.x - total * 0.5f;
    const float y = centre.y - 16.0f;

    for (size_t i = 0; i < pieces.size(); ++i)
    {
        const auto& piece = pieces[i];
        const float w = widthOf (piece);
        const juce::Rectangle<float> box (x, y, w, keyHeight);
        if (piece.key)
        {
            theme::drawRaised (g, box, theme::radiusChip, theme::control);
            g.setColour (theme::text);
            g.setFont (keyFont);
            g.drawText (piece.text, box, juce::Justification::centred);
        }
        else
        {
            g.setColour (theme::text.withAlpha (0.85f));
            g.setFont (labelFont);
            g.drawText (piece.text, box, juce::Justification::centred);
        }
        x += w + gapAfter (i);
    }

    g.setColour (theme::faintText);
    g.setFont (theme::font (12.0f));
    g.drawText ("You can also drop WAV, AIFF or FLAC files here",
                juce::Rectangle<float> (area.toFloat().getX(), y + keyHeight + 8.0f, area.toFloat().getWidth(), 18.0f),
                juce::Justification::centred, true);
}

void Lanes::playheadMoved (float oldX, float newX)
{
    repaint (juce::Rectangle<int> ((int) oldX - 2, 0, 5, getHeight()));
    repaint (juce::Rectangle<int> ((int) newX - 2, 0, 5, getHeight()));
}

//==============================================================================
void Lanes::mouseMove (const juce::MouseEvent& e)
{
    switch (regionAt (e.getPosition()).part)
    {
        case Part::leftEdge:  setMouseCursor (juce::MouseCursor::LeftEdgeResizeCursor); break;
        case Part::rightEdge: setMouseCursor (juce::MouseCursor::RightEdgeResizeCursor); break;
        case Part::fadeIn:
        case Part::fadeOut:   setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case Part::body:
        case Part::none:
        default:              setMouseCursor (juce::MouseCursor::NormalCursor); break;
    }
}

void Lanes::mouseDown (const juce::MouseEvent& e)
{
    if (view.studio.isShowingAutomation() && automationMouseDown (e))
        return;

    auto& studio = view.studio;
    const auto hit = regionAt (e.getPosition());
    shiftAtDown = e.mods.isShiftDown();
    downSample = view.sampleAtX (e.x);
    moveDelta = 0;
    trackDelta = 0;
    copying = false;

    if (hit.trackIndex >= 0)
        studio.selectTrack ((int) view.project().track (hit.trackIndex)[ids::id]);

    if (! hit.region.isValid())
    {
        if (e.mods.isPopupMenu())
        {
            studio.setSelectedRegions ({});
            juce::PopupMenu m;
            m.addItem ("Paste", studio.canPaste(), false, [&studio] { studio.paste(); });
            if (hit.trackIndex >= 0 && daw::Project::isInstrumentTrack (view.project().track (hit.trackIndex)))
            {
                auto track = view.project().track (hit.trackIndex);
                const auto at = (juce::int64) view.sampleAtX (e.x);
                m.addItem ("Create MIDI Region", [&studio, track, at] { studio.createMidiRegion (track, at); });
            }
            m.addItem ("Create Marker", [&studio] { studio.commands.invokeDirectly (cmd::createMarker, false); });
            m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
            return;
        }
        gesture = Gesture::pendingEmpty;
        selectionAtDown = shiftAtDown ? studio.getSelectedRegions() : juce::Array<int>();
        if (! shiftAtDown)
            studio.setSelectedRegions ({});
        return;
    }

    const int id = (int) hit.region[ids::id];
    auto selection = studio.getSelectedRegions();

    if (e.mods.isPopupMenu())
    {
        if (! selection.contains (id))
            studio.setSelectedRegions ({ id });
        showRegionMenu();
        return;
    }

    if (shiftAtDown)
    {
        if (selection.contains (id)) selection.removeFirstMatchingValue (id);
        else                         selection.add (id);
        studio.setSelectedRegions (selection);
    }
    else if (! selection.contains (id))
    {
        studio.setSelectedRegions ({ id });
    }

    grabbed = hit.region;
    grabbedTrack = hit.trackIndex;
    grabbedPart = hit.part;
    grabStart = (juce::int64) grabbed[ids::start];
    grabOffset = (juce::int64) grabbed[ids::offset];
    grabLength = (juce::int64) grabbed[ids::length];
    gesture = Gesture::pendingRegion;
}

void Lanes::mouseDrag (const juce::MouseEvent& e)
{
    if (gesture == Gesture::automation)
    {
        automationDrag (e);
        return;
    }

    auto& p = view.project();
    const bool snapOn = ! e.mods.isCommandDown();

    if (gesture == Gesture::pendingEmpty)
    {
        if (e.getDistanceFromDragStart() < 3)
            return;
        gesture = Gesture::rubber;
        view.setUserGesture (true);
    }

    if (gesture == Gesture::rubber)
    {
        updateRubberBand (e);
        return;
    }

    if (gesture == Gesture::pendingRegion)
    {
        if (e.getDistanceFromDragStart() < 3)
            return;
        // Shift-click that deselected the region: nothing to drag.
        if (! view.studio.getSelectedRegions().contains ((int) grabbed[ids::id]))
        {
            gesture = Gesture::none;
            return;
        }
        gesture = grabbedPart == Part::leftEdge  ? Gesture::trimLeft
                : grabbedPart == Part::rightEdge ? Gesture::trimRight
                : grabbedPart == Part::fadeIn    ? Gesture::fadeIn
                : grabbedPart == Part::fadeOut   ? Gesture::fadeOut
                                                 : Gesture::move;
        view.setUserGesture (true);
        if (gesture == Gesture::move)
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    const double mouseSample = view.sampleAtX (e.x);

    if (gesture == Gesture::move)
    {
        // Snap the grabbed region's start; everything selected moves by the same amount.
        const auto newStart = view.snap ((double) grabStart + (mouseSample - downSample), snapOn);
        juce::int64 earliest = std::numeric_limits<juce::int64>::max();
        int lowestTrack = std::numeric_limits<int>::max(), highestTrack = -1;
        const auto& selected = view.studio.getSelectedRegions();

        for (int i = 0; i < p.numTracks(); ++i)
            for (auto& r : regionsOf (p.track (i)))
                if (selected.contains ((int) r[ids::id]))
                {
                    earliest = juce::jmin (earliest, (juce::int64) r[ids::start]);
                    lowestTrack = juce::jmin (lowestTrack, i);
                    highestTrack = juce::jmax (highestTrack, i);
                }

        moveDelta = juce::jmax (newStart - grabStart, -earliest);

        int overTrack = view.trackIndexAtY (e.y);
        if (overTrack < 0)
            overTrack = e.y < 0 ? 0 : p.numTracks() - 1;
        trackDelta = juce::jlimit (-lowestTrack, p.numTracks() - 1 - highestTrack, overTrack - grabbedTrack);
        copying = e.mods.isAltDown();
        repaint();
        return;
    }

    const auto grabEnd = grabStart + grabLength;
    const auto minLength = minimumLength();

    if (gesture == Gesture::fadeIn || gesture == Gesture::fadeOut)
    {
        // Fades follow the mouse freely (no snapping) and stop where the other fade starts.
        auto fadeIn = (juce::int64) grabbed.getProperty (ids::fadeIn, 0), fadeOut = (juce::int64) grabbed.getProperty (ids::fadeOut, 0);
        fitFades (grabLength, fadeIn, fadeOut);
        const bool in = gesture == Gesture::fadeIn;
        const auto wanted = in ? (juce::int64) mouseSample - grabStart : grabEnd - (juce::int64) mouseSample;
        fadeLength = juce::jlimit ((juce::int64) 0, grabLength - (in ? fadeOut : fadeIn), wanted);
        repaint();
        return;
    }

    if (gesture == Gesture::trimLeft)
    {
        auto newStart = view.snap (mouseSample, snapOn);
        if (! daw::Project::isMidiRegion (grabbed) && ! daw::Project::isTakeFolder (grabbed))
            newStart = juce::jmax (newStart, grabStart - grabOffset);       // not before the file starts
        newStart = juce::jmin (newStart, grabEnd - minLength);
        newStart = juce::jmax ((juce::int64) 0, newStart);
        trimStart = newStart;
        trimOffset = grabOffset + (newStart - grabStart);
        trimLength = grabEnd - newStart;
        repaint();
    }
    else if (gesture == Gesture::trimRight)
    {
        auto newEnd = view.snap (mouseSample, snapOn);
        if (const auto fileLength = fileLengthOf (grabbed); fileLength > 0)
            newEnd = juce::jmin (newEnd, grabStart + (fileLength - grabOffset)); // not past the file's end
        newEnd = juce::jmax (newEnd, grabStart + minLength);
        trimStart = grabStart;
        trimOffset = grabOffset;
        trimLength = newEnd - grabStart;
        repaint();
    }
}

void Lanes::mouseUp (const juce::MouseEvent& e)
{
    switch (gesture)
    {
        case Gesture::move:      commitMove (e.mods.isAltDown()); break;
        case Gesture::trimLeft:
        case Gesture::trimRight: commitTrim(); break;
        case Gesture::fadeIn:
        case Gesture::fadeOut:   commitFade(); break;
        case Gesture::pendingRegion:
            // A plain click on a region that was part of a bigger selection selects just it.
            if (! shiftAtDown && grabbed.isValid())
                view.studio.setSelectedRegions ({ (int) grabbed[ids::id] });
            break;
        case Gesture::none:
        case Gesture::pendingEmpty:
        case Gesture::rubber:
        case Gesture::automation:
        default: break;
    }

    gesture = Gesture::none;
    grabbed = {};
    moveDelta = 0;
    trackDelta = 0;
    copying = false;
    view.setUserGesture (false);
    setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
}

void Lanes::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Automation view: double-click a point to delete it.
    if (view.studio.isShowingAutomation())
    {
        const int index = view.trackIndexAtY (e.y);
        if (index < 0)
            return;
        auto& p = view.project();
        auto track = p.track (index);
        const auto param = Studio::shownAutomationParam (track);
        const auto params = daw::Project::automatableParams (track);
        const auto* spec = findParam (params, param);
        auto points = daw::Project::getAutomation (track, param);
        if (spec == nullptr)
            return;
        const auto span = autoSpan (index);
        for (size_t i = 0; i < points.size(); ++i)
        {
            const float x = view.xForSample ((double) points[i].time);
            const float y = span.getEnd() - autoNorm (*spec, points[i].value) * span.getLength();
            if (juce::Point<float> (x, y).getDistanceFrom (e.position) < 7.0f)
            {
                points.erase (points.begin() + (std::ptrdiff_t) i);
                p.undo.beginNewTransaction ("Delete Automation Point");
                p.setAutomation (track, param, points);
                return;
            }
        }
        return;
    }

    const auto hit = regionAt (e.getPosition());

    // A MIDI region opens in the piano roll.
    if (hit.region.isValid())
    {
        if (daw::Project::isMidiRegion (hit.region))
            view.studio.openInEditor ((int) hit.region[ids::id]);
        else if (daw::Project::isTakeFolder (hit.region))
            view.studio.openTakes ((int) hit.region[ids::id]);
        return;
    }

    // An empty spot on an instrument track: a new one-bar MIDI region there.
    if (hit.trackIndex >= 0)
    {
        auto track = view.project().track (hit.trackIndex);
        if (daw::Project::isInstrumentTrack (track))
            view.studio.createMidiRegion (track, (juce::int64) view.sampleAtX (e.x));
        return;
    }

    // Double-click below the last track for a new one.
    view.addTrackAfterSelection();
}

const daw::Project::AutoParam* Lanes::findParam (const std::vector<daw::Project::AutoParam>& params, const juce::String& id)
{
    for (auto& p : params)
        if (p.id == id)
            return &p;
    return params.empty() ? nullptr : &params.front();
}

float Lanes::autoNorm (const daw::Project::AutoParam& spec, float value) const
{
    if (spec.id == "volume")
        return mixer::faderPos ((double) value);
    if (spec.max <= spec.min)
        return 0.0f;
    juce::NormalisableRange<float> range (spec.min, spec.max);
    range.skew = spec.skew;
    return range.convertTo0to1 (juce::jlimit (spec.min, spec.max, value));
}

float Lanes::autoValue (const daw::Project::AutoParam& spec, float norm) const
{
    norm = juce::jlimit (0.0f, 1.0f, norm);
    if (spec.id == "volume")
        return (float) juce::jmax (-96.0, mixer::faderDb (norm));
    if (spec.max <= spec.min)
        return spec.min;
    juce::NormalisableRange<float> range (spec.min, spec.max);
    range.skew = spec.skew;
    return range.convertFrom0to1 (norm);
}

juce::Range<float> Lanes::autoSpan (int trackIndex) const
{
    const float top = (float) view.yForTrack (trackIndex) + 8.0f;
    return { top, top + (float) ArrangeView::trackHeight - 16.0f };
}

void Lanes::drawAutomation (juce::Graphics& g)
{
    if (! view.studio.isShowingAutomation())
        return;

    auto& p = view.project();
    const auto clip = g.getClipBounds();
    for (int i = 0; i < p.numTracks(); ++i)
    {
        const auto row = juce::Rectangle<int> (0, view.yForTrack (i), getWidth(), ArrangeView::trackHeight);
        if (! row.intersects (clip))
            continue;

        auto track = p.track (i);
        const auto params = daw::Project::automatableParams (track);
        const auto* spec = findParam (params, Studio::shownAutomationParam (track));
        if (spec == nullptr)
            continue;
        const auto points = daw::Project::getAutomation (track, spec->id);
        const auto span = autoSpan (i);
        const auto colour = trackColour (track).interpolatedWith (juce::Colours::white, 0.35f);

        // Regions recede so the line reads.
        g.setColour (juce::Colours::black.withAlpha (0.38f));
        g.fillRect (row.withTrimmedBottom (1));

        auto yFor = [&] (float value) { return span.getEnd() - autoNorm (*spec, value) * span.getLength(); };

        if (points.empty())
        {
            // No line yet: the fixed value, faint and dashed.
            const float y = yFor (spec->def);
            const float dashes[] { 4.0f, 4.0f };
            g.setColour (colour.withAlpha (0.45f));
            g.drawDashedLine (juce::Line<float> (0.0f, y, (float) getWidth(), y), dashes, 2, 1.0f);
        }
        else
        {
            juce::Path line;
            line.startNewSubPath (0.0f, yFor (points.front().value));
            for (auto& pt : points)
                line.lineTo (view.xForSample ((double) pt.time), yFor (pt.value));
            line.lineTo ((float) getWidth(), yFor (points.back().value));
            g.setColour (colour);
            g.strokePath (line, juce::PathStrokeType (1.6f));
            for (size_t k = 0; k < points.size(); ++k)
            {
                const float x = view.xForSample ((double) points[k].time);
                if (x < (float) clip.getX() - 6.0f || x > (float) clip.getRight() + 6.0f)
                    continue;
                const bool active = gesture == Gesture::automation && autoTrack == i && autoIndex == (int) k;
                g.setColour (active ? juce::Colours::white : colour);
                g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, yFor (points[k].value) }));
            }
        }

        // The parameter's name and value at the playhead, top left of the lane.
        const auto now = points.empty() ? spec->def : daw::Project::evaluate (points, view.studio.engine().getPosition());
        juce::String valueText = spec->id == "volume" ? mixer::dbText ((double) now) + " dB"
                               : spec->id == "pan" ? (std::abs (now) < 0.01f ? juce::String ("C") : (now < 0 ? "L" : "R") + juce::String (juce::roundToInt (std::abs (now) * 64.0f)))
                               : juce::String (now, 2) + (spec->unit.isNotEmpty() ? " " + spec->unit : juce::String());
        g.setFont (theme::font (11.0f, theme::Weight::semibold));
        const auto label = row.withTrimmedLeft (8).removeFromBottom (17);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (label.withWidth (juce::jmin (label.getWidth(), 12 + juce::roundToInt (juce::GlyphArrangement::getStringWidth (theme::font (11.0f, theme::Weight::semibold), spec->name + "  " + valueText)))).toFloat().reduced (0.0f, 1.0f), 3.0f);
        g.setColour (colour);
        g.drawText (spec->name + "  " + valueText, label.withTrimmedLeft (5), juce::Justification::centredLeft, true);
    }
}

bool Lanes::automationMouseDown (const juce::MouseEvent& e)
{
    const int index = view.trackIndexAtY (e.y);
    if (index < 0)
        return false;

    auto& p = view.project();
    auto track = p.track (index);
    view.studio.selectTrack ((int) track[ids::id]);
    const auto params = daw::Project::automatableParams (track);
    const auto* spec = findParam (params, Studio::shownAutomationParam (track));
    if (spec == nullptr)
        return true;
    auto points = daw::Project::getAutomation (track, spec->id);
    const auto span = autoSpan (index);
    const auto paramId = spec->id;

    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addItem ("Delete " + spec->name + " Automation", ! points.empty(), false, [this, track, paramId]() mutable
        {
            auto& pr = view.project();
            pr.undo.beginNewTransaction ("Delete Automation");
            pr.setAutomation (track, paramId, {});
        });
        m.addItem ("Hide Automation (A)", [this] { view.studio.setShowingAutomation (false); });
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
        return true;
    }

    // Grab the point under the mouse, or add one there (Logic: click on the line to add).
    autoIndex = -1;
    for (size_t i = 0; i < points.size(); ++i)
    {
        const float x = view.xForSample ((double) points[i].time);
        const float y = span.getEnd() - autoNorm (*spec, points[i].value) * span.getLength();
        if (juce::Point<float> (x, y).getDistanceFrom (e.position) < 7.0f)
            autoIndex = (int) i;
    }

    p.undo.beginNewTransaction (autoIndex >= 0 ? "Move Automation Point" : "Add Automation Point");
    if (autoIndex < 0)
    {
        daw::AutoPoint pt;
        pt.time = view.snap (view.sampleAtX (e.x), ! e.mods.isCommandDown());
        pt.value = autoValue (*spec, (span.getEnd() - (float) e.y) / span.getLength());
        // The first point on an empty line keeps the track where it was before it.
        points.push_back (pt);
        std::stable_sort (points.begin(), points.end(), [] (auto& a, auto& b) { return a.time < b.time; });
        p.setAutomation (track, paramId, points);
        for (size_t i = 0; i < points.size(); ++i)
            if (points[i] == pt)
                autoIndex = (int) i;
    }

    gesture = Gesture::automation;
    autoTrack = index;
    autoParamId = paramId;
    view.setUserGesture (true);
    repaint();
    return true;
}

void Lanes::automationDrag (const juce::MouseEvent& e)
{
    auto& p = view.project();
    if (! juce::isPositiveAndBelow (autoTrack, p.numTracks()))
        return;
    auto track = p.track (autoTrack);
    const auto params = daw::Project::automatableParams (track);
    const auto* spec = findParam (params, autoParamId);
    auto points = daw::Project::getAutomation (track, autoParamId);
    if (spec == nullptr || ! juce::isPositiveAndBelow (autoIndex, (int) points.size()))
        return;

    // A point stays between its neighbours.
    const auto span = autoSpan (autoTrack);
    auto& pt = points[(size_t) autoIndex];
    auto time = view.snap (view.sampleAtX (e.x), ! e.mods.isCommandDown());
    const auto low = autoIndex > 0 ? points[(size_t) autoIndex - 1].time : (juce::int64) 0;
    const auto high = autoIndex + 1 < (int) points.size() ? points[(size_t) autoIndex + 1].time : std::numeric_limits<juce::int64>::max();
    pt.time = juce::jlimit (low, high, time);
    pt.value = autoValue (*spec, (span.getEnd() - (float) e.y) / span.getLength());
    p.setAutomation (track, autoParamId, points);
}

void Lanes::commitMove (bool copy)
{
    if (moveDelta == 0 && trackDelta == 0 && ! copy)
        return;

    auto& p = view.project();
    auto& studio = view.studio;
    const auto selected = studio.getSelectedRegions();

    struct Item { juce::ValueTree region; int track; };
    juce::Array<Item> items;
    for (int i = 0; i < p.numTracks(); ++i)
        for (auto& r : regionsOf (p.track (i)))
            if (selected.contains ((int) r[ids::id]))
                items.add ({ r, i });

    if (items.isEmpty())
        return;

    p.undo.beginNewTransaction (copy ? (items.size() > 1 ? "Copy Regions" : "Copy Region")
                                     : (items.size() > 1 ? "Move Regions" : "Move Region"));
    juce::Array<int> newSelection;
    for (auto& item : items)
    {
        auto target = p.track (juce::jlimit (0, p.numTracks() - 1, item.track + trackDelta));
        const auto newStart = (juce::int64) item.region[ids::start] + moveDelta;

        if (copy)
        {
            auto c = item.region.createCopy();
            c.setProperty (ids::id, p.nextId(), nullptr);
            c.setProperty (ids::start, newStart, nullptr);
            target.addChild (c, -1, &p.undo);
            newSelection.add ((int) c[ids::id]);
        }
        else
        {
            item.region.setProperty (ids::start, newStart, &p.undo);
            if (auto parent = item.region.getParent(); parent != target)
            {
                parent.removeChild (item.region, &p.undo);
                target.addChild (item.region, -1, &p.undo);
            }
            newSelection.add ((int) item.region[ids::id]);
        }
    }

    studio.setSelectedRegions (newSelection);
    if (trackDelta != 0)
        studio.selectTrack ((int) p.track (juce::jlimit (0, p.numTracks() - 1, grabbedTrack + trackDelta))[ids::id]);
}

void Lanes::commitTrim()
{
    if (! grabbed.isValid() || (trimStart == grabStart && trimLength == grabLength) || trimLength <= 0)
        return;

    auto& p = view.project();
    p.undo.beginNewTransaction ("Trim Region");
    grabbed.setProperty (ids::start, trimStart, &p.undo);
    grabbed.setProperty (ids::offset, trimOffset, &p.undo);
    grabbed.setProperty (ids::length, trimLength, &p.undo);
}

void Lanes::commitFade()
{
    const auto& property = gesture == Gesture::fadeIn ? ids::fadeIn : ids::fadeOut;
    if (! grabbed.isValid() || fadeLength == (juce::int64) grabbed.getProperty (property, 0))
        return;

    auto& p = view.project();
    p.undo.beginNewTransaction (gesture == Gesture::fadeIn ? "Fade In" : "Fade Out");
    if (fadeLength > 0) grabbed.setProperty (property, fadeLength, &p.undo);
    else                grabbed.removeProperty (property, &p.undo);
}

void Lanes::removeFadesFromSelection()
{
    auto& p = view.project();
    const auto& selected = view.studio.getSelectedRegions();
    p.undo.beginNewTransaction ("Remove Fades");
    for (int i = 0; i < p.numTracks(); ++i)
        for (auto r : regionsOf (p.track (i)))
            if (selected.contains ((int) r[ids::id]))
            {
                r.removeProperty (ids::fadeIn, &p.undo);
                r.removeProperty (ids::fadeOut, &p.undo);
            }
}

void Lanes::updateRubberBand (const juce::MouseEvent& e)
{
    rubber = juce::Rectangle<int> (e.getMouseDownPosition(), e.getPosition());
    auto& p = view.project();
    auto selection = selectionAtDown;

    for (int i = 0; i < p.numTracks(); ++i)
        for (auto& r : regionsOf (p.track (i)))
            if (regionRect (i, (juce::int64) r[ids::start], (juce::int64) r[ids::length]).intersects (rubber.toFloat()))
                selection.addIfNotAlreadyThere ((int) r[ids::id]);

    view.studio.setSelectedRegions (selection);
    repaint();
}

void Lanes::showRegionMenu()
{
    auto& studio = view.studio;
    juce::PopupMenu m;
    m.addItem ("Cut", [&studio] { studio.cutSelectedRegions(); });
    m.addItem ("Copy", [&studio] { studio.copySelectedRegions(); });
    m.addItem ("Paste", studio.canPaste(), false, [&studio] { studio.paste(); });
    m.addSeparator();
    m.addItem ("Split at Playhead", [&studio] { studio.splitSelectedAtPlayhead(); });
    m.addItem ("Repeat", [&studio] { studio.repeatSelectedRegions(); });
    m.addItem ("Mute / Unmute", [&studio] { studio.toggleMuteSelectedRegions(); });
    m.addItem ("Loop / Unloop", [&studio] { studio.toggleLoopSelectedRegions(); });
    m.addItem ("Region Settings...", [this] { showRegionSettings(); });
    m.addItem ("Loop These Regions", [&studio] { studio.commands.invokeDirectly (cmd::loopToSelection, false); });
    m.addItem ("Remove Fades", [this] { removeFadesFromSelection(); });

    // Take folders: comp them, or make the comp plain regions.
    for (int id : studio.getSelectedRegions())
        if (auto r = studio.findRegion (id); daw::Project::isTakeFolder (r))
        {
            m.addSeparator();
            m.addItem ("Open Takes (Comp)", [&studio, id] { studio.openTakes (id); });
            m.addItem ("Flatten Take Folder", [&studio, id]
            {
                auto& p = studio.project();
                p.undo.beginNewTransaction ("Flatten");
                studio.setSelectedRegions (p.flattenTakeFolder (studio.findRegion (id)));
            });
            break;
        }

    // MIDI regions: open the editor, quantize.
    juce::Array<int> midi;
    for (int id : studio.getSelectedRegions())
        if (auto r = studio.findRegion (id); r.isValid() && daw::Project::isMidiRegion (r))
            midi.add (id);
    if (! midi.isEmpty())
    {
        m.addSeparator();
        m.addItem ("Open in Piano Roll", [&studio, first = midi[0]] { studio.openInEditor (first); });
        juce::PopupMenu q;
        const std::pair<const char*, int> grids[] { { "1/4 Note", 960 }, { "1/8 Note", 480 }, { "1/16 Note", 240 },
                                                    { "1/32 Note", 120 }, { "1/8 Triplet", 320 }, { "1/16 Triplet", 160 } };
        for (auto& [label, ticks] : grids)
            q.addItem (label, [&studio, midi, t = ticks]
            {
                for (int id : midi)
                    studio.quantizeNotes (studio.findRegion (id), {}, t);
            });
        m.addSubMenu ("Quantize Notes", q);
    }
    m.addSeparator();
    m.addItem ("Delete", [&studio] { studio.deleteSelectedRegions(); });
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void Lanes::showRegionSettings()
{
    auto& studio = view.studio;
    if (studio.getSelectedRegions().isEmpty())
        return;
    auto region = studio.findRegion (studio.getSelectedRegions().getFirst());
    if (! region.isValid())
        return;

    // Logic's region inspector, as a popover: name, loop, gain or (MIDI) transpose and velocity.
    struct Settings final : public juce::Component
    {
        Settings (Studio& s, juce::ValueTree r) : studio (s), region (std::move (r))
        {
            const bool midi = daw::Project::isMidiRegion (region);
            name.setText (region[ids::name].toString(), juce::dontSendNotification);
            name.onReturnKey = name.onFocusLost = [this]
            {
                if (name.getText().isNotEmpty() && name.getText() != region[ids::name].toString())
                {
                    studio.project().undo.beginNewTransaction ("Rename Region");
                    region.setProperty (ids::name, name.getText(), &studio.project().undo);
                }
            };
            loop.setToggleState ((bool) region[ids::loop], juce::dontSendNotification);
            loop.onClick = [this]
            {
                studio.project().undo.beginNewTransaction ("Loop Region");
                region.setProperty (ids::loop, loop.getToggleState(), &studio.project().undo);
            };
            auto setup = [this] (juce::Slider& slider, juce::Label& label, const juce::String& text, double lo, double hi, double step,
                                 const juce::Identifier& prop, const juce::String& suffix)
            {
                label.setText (text, juce::dontSendNotification);
                label.setColour (juce::Label::textColourId, theme::dimText);
                slider.setSliderStyle (juce::Slider::LinearHorizontal);
                slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
                slider.setRange (lo, hi, step);
                slider.setTextValueSuffix (suffix);
                slider.setValue ((double) region.getProperty (prop, 0.0), juce::dontSendNotification);
                slider.setDoubleClickReturnValue (true, 0.0);
                slider.onDragStart = [this, text] { studio.project().undo.beginNewTransaction ("Region " + text); };
                slider.onValueChange = [this, &slider, prop]
                {
                    if (juce::approximatelyEqual (slider.getValue(), 0.0) && prop != ids::gain) region.removeProperty (prop, &studio.project().undo);
                    else region.setProperty (prop, slider.getValue(), &studio.project().undo);
                };
                addAndMakeVisible (slider);
                addAndMakeVisible (label);
            };
            if (midi)
            {
                setup (first, firstLabel, "Transpose", -36.0, 36.0, 1.0, ids::transpose, " st");
                setup (second, secondLabel, "Velocity", -64.0, 64.0, 1.0, ids::velocity, "");
            }
            else
                setup (first, firstLabel, "Gain", -24.0, 12.0, 0.1, ids::gain, " dB");
            addAndMakeVisible (name);
            addAndMakeVisible (loop);
            setSize (280, midi ? 150 : 118);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            name.setBounds (r.removeFromTop (24));
            r.removeFromTop (8);
            loop.setBounds (r.removeFromTop (22));
            r.removeFromTop (6);
            auto row = r.removeFromTop (24);
            firstLabel.setBounds (row.removeFromLeft (70));
            first.setBounds (row);
            r.removeFromTop (6);
            row = r.removeFromTop (24);
            secondLabel.setBounds (row.removeFromLeft (70));
            second.setBounds (row);
        }

        Studio& studio;
        juce::ValueTree region;
        juce::TextEditor name;
        juce::ToggleButton loop { "Loop (repeat until the next region)" };
        juce::Slider first, second;
        juce::Label firstLabel, secondLabel;
    };

    const auto rect = regionRect (view.project().state.indexOf (region.getParent()), (juce::int64) region[ids::start], (juce::int64) region[ids::length]);
    juce::CallOutBox::launchAsynchronously (std::make_unique<Settings> (studio, region),
                                            localAreaToGlobal (rect.toNearestInt().withHeight (24)), nullptr);
}

void Lanes::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    view.handleWheel (e, w, (float) e.x);
}

void Lanes::mouseMagnify (const juce::MouseEvent& e, float scale)
{
    view.zoomAround ((float) e.x, (double) scale);
}

//==============================================================================
bool Lanes::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (isAudioFile (f) || juce::File (f).hasFileExtension ("mid;midi"))
            return true;
    return false;
}

void Lanes::dropTarget (int x, int y)
{
    dropTrack = view.trackIndexAtY (y);
    dropAt = view.snap (view.sampleAtX (x));
    repaint();
}

void Lanes::fileDragEnter (const juce::StringArray&, int x, int y)
{
    dropping = true;
    dropTarget (x, y);
}

void Lanes::fileDragMove (const juce::StringArray&, int x, int y)
{
    dropTarget (x, y);
}

void Lanes::fileDragExit (const juce::StringArray&)
{
    dropping = false;
    repaint();
}

void Lanes::filesDropped (const juce::StringArray& files, int x, int y)
{
    dropping = false;
    dropTarget (x, y);

    // MIDI files: each part on a new instrument track, where it was dropped.
    for (auto& path : files)
        if (juce::File (path).hasFileExtension ("mid;midi"))
        {
            view.studio.importMidiFile (juce::File (path), dropAt);
            return;
        }

    auto& p = view.project();
    auto& studio = view.studio;
    const auto projectRate = p.grid().sampleRate;
    p.audioFolder().createDirectory();

    p.undo.beginNewTransaction ("Import Audio");
    auto track = dropTrack >= 0 ? p.track (dropTrack) : juce::ValueTree();
    auto at = dropAt;
    juce::Array<int> added;
    juce::String problem;

    for (auto& path : files)
    {
        const juce::File source (path);
        if (! isAudioFile (path))
            continue;

        const auto info = view.waveforms.info (source);
        if (info.length <= 0)
        {
            problem = "Couldn't read " + source.getFileName();
            continue;
        }

        // Copy it into the project so the project stays self-contained.
        auto dest = source;
        if (! source.isAChildOf (p.audioFolder()))
        {
            dest = p.audioFolder().getNonexistentChildFile (source.getFileNameWithoutExtension(), source.getFileExtension(), false);
            if (! source.copyFileTo (dest))
            {
                problem = "Couldn't copy " + source.getFileName() + " into the project";
                continue;
            }
        }

        if (! track.isValid())
            track = p.addTrack (source.getFileNameWithoutExtension());

        const auto relative = dest.getRelativePathFrom (p.folder).replaceCharacter ('\\', '/');
        auto r = p.addRegion (track, relative, at, 0, info.length, source.getFileNameWithoutExtension());
        added.add ((int) r[ids::id]);
        at += info.length;      // several files go end to end

        if (info.sampleRate > 0.0 && std::abs (info.sampleRate - projectRate) > 1.0)
            problem = source.getFileName() + " is " + juce::String (info.sampleRate / 1000.0, 1) + " kHz; the project runs at "
                    + juce::String (projectRate / 1000.0, 1) + " kHz, so it will play at the wrong speed";
    }

    if (track.isValid())
        studio.selectTrack ((int) track[ids::id]);
    if (! added.isEmpty())
        studio.setSelectedRegions (added);

    if (problem.isNotEmpty())
        studio.flash (problem);
    else if (! added.isEmpty())
        studio.flash (added.size() == 1 ? "Imported 1 file" : "Imported " + juce::String (added.size()) + " files");

    repaint();
}

} // namespace woodshed::app::arrange
