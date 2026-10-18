#pragma once

#include "Stretcher.h"
#include <juce_events/juce_events.h>
#include <set>

namespace woodshed::flex
{

/**
    The rendered audio that stretched and pitch-edited regions play, made on a background thread.

    Real-time safety: the audio thread never stretches anything. The engine asks for a region's
    audio while it builds a snapshot (message thread); a finished render is a shared buffer the
    snapshot holds like any recording, so the audio thread only reads samples. An edit the
    renderer hasn't caught up with plays a varispeed stand-in (right timing, pitch moving with
    the speed) until the proper render lands a moment later and the engine rebuilds its snapshot.
    Bounces wait for the proper render instead.

    Renders are keyed by the file and everything that shapes the result, so regions split from
    one recording share theirs, and undoing an edit finds the earlier render still there.
*/
class Renderer
{
public:
    using Audio = std::shared_ptr<const juce::AudioBuffer<float>>;

    Renderer();
    ~Renderer();

    /** What `region` plays: `fileAudio` if it isn't processed, else its render (or a stand-in
        while that's under way; with `wait`, the render, made now). Message thread, or any one
        thread at a time with `wait`. */
    Audio audioFor (const juce::ValueTree& region, const juce::File& file, const Audio& fileAudio, double sampleRate, bool wait);

    /** Called on the message thread when a background render has finished. */
    std::function<void()> onRendered;

    /** Renders still under way. */
    int pending() const;
    /** Blocks until the background renders are done (tests, closing). */
    void waitForAll();

    AnalysisCache& analysis()                       { return analyses; }
    /** Drops every render and analysis (the project was replaced). */
    void clear();

    /** Everything that shapes a region's render, as one string. */
    static juce::String keyFor (const juce::ValueTree& region, const juce::File& file, double sampleRate);
    /** The request the region's render is made from; `analyse` adds the transients and pitch
        it needs (slow: the background thread does it). */
    RenderRequest requestFor (const juce::ValueTree& region, const Audio& fileAudio, double sampleRate);
    void analyse (RenderRequest&, const juce::String& path);

private:
    struct Job;
    void store (const juce::String& key, const Audio&);

    juce::ThreadPool workers { 1 };
    AnalysisCache analyses;
    mutable std::mutex lock;
    std::map<juce::String, Audio> done;
    std::map<juce::String, Audio> standIns;
    std::vector<juce::String> recent;           // oldest first, for dropping renders nobody needs
    std::set<juce::String> queued;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    std::map<int, std::shared_ptr<std::atomic<bool>>> latest;     // each region's newest render, to cancel when superseded
    void cancelAll();

    JUCE_DECLARE_NON_COPYABLE (Renderer)
};

} // namespace woodshed::flex
