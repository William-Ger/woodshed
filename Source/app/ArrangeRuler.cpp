#include "ArrangeRuler.h"
#include "ArrangeView.h"

namespace woodshed::app::arrange
{

namespace ids = daw::ids;

Ruler::Ruler (ArrangeView& v) : view (v)
{
    setOpaque (true);
}

void Ruler::paint (juce::Graphics& g)
{
    auto& state = view.project().state;
    const auto grid = view.project().grid();
    const float width = (float) getWidth();

    g.fillAll (theme::bar);

    // Loop strip.
    const auto strip = getLocalBounds().withTrimmedTop (numbersHeight).toFloat();
    g.setColour (theme::window);
    g.fillRect (strip);

    const auto loopStart = (juce::int64) state[ids::loopStart];
    const auto loopEnd = (juce::int64) state[ids::loopEnd];
    if (loopEnd > loopStart)
    {
        const bool on = (bool) state[ids::loopOn];
        const float x1 = view.xForSample ((double) loopStart), x2 = view.xForSample ((double) loopEnd);
        auto range = juce::Rectangle<float> (x1, strip.getY() + 3.0f, x2 - x1, strip.getHeight() - 6.0f);
        if (range.getRight() > -10.0f && range.getX() < width + 10.0f)
        {
            g.setColour (on ? theme::loop.withAlpha (0.85f) : theme::loop.withAlpha (0.14f));
            g.fillRoundedRectangle (range, 3.0f);
            g.setColour (on ? theme::loop.brighter (0.3f) : theme::loop.withAlpha (0.45f));
            g.drawRoundedRectangle (range.reduced (0.5f), 3.0f, 1.0f);

            // Grips at each end.
            g.setColour (on ? juce::Colour (0xff15130f).withAlpha (0.45f) : theme::loop.withAlpha (0.5f));
            for (float gx : { range.getX() + 4.0f, range.getRight() - 6.0f })
                if (range.getWidth() > 20.0f)
                    g.fillRect (juce::Rectangle<float> (gx, range.getCentreY() - 3.0f, 2.0f, 6.0f));
        }
    }

    // Bar numbers and ticks.
    const double spb = grid.samplesPerBeat();
    const double step = view.gridStepBeats(), labelStep = view.labelStepBeats();
    const double firstBeat = std::floor (view.sampleAtX (0.0) / spb / step) * step;
    const double lastBeat = view.sampleAtX (width) / spb;
    const int bpb = juce::jmax (1, grid.beatsPerBar);

    g.setFont (theme::font (11.5f));
    for (double beat = juce::jmax (0.0, firstBeat); beat <= lastBeat + step; beat += step)
    {
        const float x = std::round (view.xForSample (beat * spb));
        const double inBar = std::fmod (beat, (double) bpb);
        const bool isBar = inBar < 1.0e-6 || bpb - inBar < 1.0e-6;
        const bool isBeat = std::abs (beat - std::round (beat)) < 1.0e-6;
        const float tickTop = isBar ? 10.0f : (isBeat ? 17.0f : 20.0f);

        g.setColour (isBar ? theme::dimText.withAlpha (0.7f) : theme::gridBar);
        g.fillRect (x, tickTop, 1.0f, (float) numbersHeight - tickTop);

        const double labelPhase = std::fmod (beat, labelStep);
        if (labelPhase > 1.0e-6 && labelStep - labelPhase > 1.0e-6)
            continue;

        const int bar = (int) std::floor ((beat + 1.0e-6) / bpb) + 1;
        const int beatInBar = (int) std::floor (inBar + 1.0e-6) + 1;
        if (isBar)
        {
            g.setColour (theme::text.withAlpha (0.85f));
            g.drawText (juce::String (bar), juce::Rectangle<float> (x + 4.0f, 1.0f, 40.0f, 16.0f), juce::Justification::centredLeft);
        }
        else if (isBeat)
        {
            g.setColour (theme::dimText);
            g.drawText (juce::String (bar) + "." + juce::String (beatInBar),
                        juce::Rectangle<float> (x + 3.0f, 3.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
        }
    }

    g.setColour (theme::outline);
    g.fillRect (0, numbersHeight - 1, getWidth(), 1);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);

    // Playhead: a marker in the numbers and a line down through the strip.
    const float px = std::round (view.xForSample ((double) view.studio.engine().getPosition()));
    if (px > -8.0f && px < width + 8.0f)
    {
        const auto colour = view.studio.engine().isRecording() ? theme::record : theme::accent;
        g.setColour (colour);
        g.fillRect (px, 0.0f, 1.0f, (float) getHeight());
        juce::Path marker;
        marker.addTriangle (px - 5.5f, 0.0f, px + 6.5f, 0.0f, px + 0.5f, 8.0f);
        g.fillPath (marker);
    }
}

void Ruler::playheadMoved (float oldX, float newX)
{
    repaint (juce::Rectangle<int> ((int) oldX - 8, 0, 18, getHeight()));
    repaint (juce::Rectangle<int> ((int) newX - 8, 0, 18, getHeight()));
}

//==============================================================================
Ruler::Mode Ruler::loopModeAt (juce::Point<int> p) const
{
    if (p.y < numbersHeight)
        return Mode::scrub;

    auto& state = view.project().state;
    const auto loopStart = (juce::int64) state[ids::loopStart];
    const auto loopEnd = (juce::int64) state[ids::loopEnd];
    if (loopEnd <= loopStart)
        return Mode::loopCreate;

    const float x1 = view.xForSample ((double) loopStart), x2 = view.xForSample ((double) loopEnd);
    const float grab = juce::jmin (6.0f, (x2 - x1) / 3.0f);
    if (std::abs ((float) p.x - x1) <= grab) return Mode::loopStart;
    if (std::abs ((float) p.x - x2) <= grab) return Mode::loopEnd;
    if ((float) p.x > x1 && (float) p.x < x2) return Mode::loopMove;
    return Mode::loopCreate;
}

void Ruler::mouseMove (const juce::MouseEvent& e)
{
    switch (loopModeAt (e.getPosition()))
    {
        case Mode::loopStart:
        case Mode::loopEnd:   setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case Mode::loopMove:  setMouseCursor (juce::MouseCursor::PointingHandCursor); break;
        case Mode::none:
        case Mode::scrub:
        case Mode::loopCreate:
        default:              setMouseCursor (juce::MouseCursor::NormalCursor); break;
    }
}

void Ruler::scrubTo (float x)
{
    view.studio.engine().setPosition ((juce::int64) juce::jmax (0.0, view.sampleAtX (x)));
}

void Ruler::setLoop (juce::int64 start, juce::int64 end, bool turnOn)
{
    auto& p = view.project();
    if (! changed)
    {
        p.undo.beginNewTransaction ("Set Loop");
        changed = true;
    }
    p.state.setProperty (ids::loopStart, start, &p.undo);
    p.state.setProperty (ids::loopEnd, end, &p.undo);
    if (turnOn && ! (bool) p.state[ids::loopOn])
        p.state.setProperty (ids::loopOn, true, &p.undo);
}

void Ruler::mouseDown (const juce::MouseEvent& e)
{
    changed = false;
    mode = loopModeAt (e.getPosition());
    auto& state = view.project().state;
    originalStart = (juce::int64) state[ids::loopStart];
    originalEnd = (juce::int64) state[ids::loopEnd];
    anchor = view.snap (view.sampleAtX (e.x), ! e.mods.isCommandDown());
    view.setUserGesture (true);

    if (mode == Mode::scrub)
        scrubTo ((float) e.x);
}

void Ruler::mouseDrag (const juce::MouseEvent& e)
{
    const bool snapOn = ! e.mods.isCommandDown();
    const auto at = view.snap (view.sampleAtX (e.x), snapOn);
    const auto minLength = snapOn ? (juce::int64) std::llround (view.project().grid().samplesPerBeat())
                                  : (juce::int64) std::llround (view.getSamplesPerPixel() * 4.0);

    switch (mode)
    {
        case Mode::scrub:
            scrubTo ((float) e.x);
            break;

        case Mode::loopCreate:
            if (e.getDistanceFromDragStartX() == 0 || at == anchor)
                break;
            setLoop (juce::jmin (anchor, at), juce::jmax (anchor, at), true);
            break;

        case Mode::loopMove:
        {
            if (! changed && std::abs (e.getDistanceFromDragStartX()) < 3)
                break;
            const auto delta = juce::jmax (-originalStart, at - anchor);
            if (delta != 0 || changed)
                setLoop (originalStart + delta, originalEnd + delta, false);
            break;
        }

        case Mode::loopStart:
            setLoop (juce::jmin (at, originalEnd - minLength), originalEnd, false);
            break;

        case Mode::loopEnd:
            setLoop (originalStart, juce::jmax (at, originalStart + minLength), false);
            break;

        case Mode::none:
        default:
            break;
    }
}

void Ruler::mouseUp (const juce::MouseEvent&)
{
    // A click on the range (no drag) switches looping on or off, like the C key.
    if (mode == Mode::loopMove && ! changed)
        view.studio.commands.invokeDirectly (cmd::toggleLoop, false);

    mode = Mode::none;
    changed = false;
    view.setUserGesture (false);
}

void Ruler::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    view.handleWheel (e, w, (float) e.x);
}

void Ruler::mouseMagnify (const juce::MouseEvent& e, float scale)
{
    view.zoomAround ((float) e.x, (double) scale);
}

} // namespace woodshed::app::arrange
