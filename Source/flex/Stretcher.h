#pragma once

#include "Analysis.h"
#include <atomic>

namespace woodshed::flex
{

/**
    Renders a region's file through its flex markers and pitch edits into new audio, laid out in
    region time: sample i of the result plays at the region's content origin + i.

    The audio between each pair of markers (and, in the Monophonic and Polyphonic modes, between
    the transients inside a stretched stretch of them) is stretched on its own and placed exactly
    where it belongs, so every marker and transient lands on its sample; neighbouring pieces
    cross-fade over 3 ms just before each one. Stretching and pitch shifting use Signalsmith
    Stretch (MIT, third_party/); Speed is plain resampling and Rhythmic cuts without stretching.

    This is slow work (a few times faster than real time to a hundred times, depending on the
    mode and the edits): it runs on a background thread, never the audio thread (see Renderer).
*/
struct RenderRequest
{
    std::shared_ptr<const juce::AudioBuffer<float>> source;    // mono
    double sampleRate = 48000.0;
    Mode mode = Mode::polyphonic;
    WarpMap warp;
    std::vector<PitchNote> notes;                               // Pitch Edit; only edited ones matter
    std::shared_ptr<const PitchTrack> pitch;                    // needed when notes change drift or vibrato
    std::shared_ptr<const std::vector<double>> transients;      // where to cut inside stretched stretches
    long seed = 1;                                              // the stretcher's phase randomisation
};

using Rendered = std::shared_ptr<juce::AudioBuffer<float>>;

/** Null if cancelled or there's no source. */
Rendered render (const RenderRequest&, const std::atomic<bool>* cancel = nullptr);

/** Speed (varispeed) rendering: quick enough for the message thread, so it stands in while a
    proper render is on its way. */
Rendered renderVarispeed (const juce::AudioBuffer<float>& source, const WarpMap&);

/** How long the rendered audio is (where the end of the file lands). */
juce::int64 renderedLength (const WarpMap&, juce::int64 fileLength);

} // namespace woodshed::flex
