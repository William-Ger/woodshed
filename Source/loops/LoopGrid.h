#pragma once

#include "LoopModel.h"
#include "LoopPlayer.h"
#include "../daw/Engine.h"

namespace woodshed::loops
{

/**
    The Loop Grid's message-thread side: launches and stops cells and scenes, records into cells,
    and perform recording (the launches written into the arrange as regions). Commands go to
    the engine's LoopPlayer, which makes them land on the launch quantize grid sample-accurately.
    Message thread only.
*/
class LoopGrid final : private juce::Timer
{
public:
    explicit LoopGrid (daw::Engine&);
    ~LoopGrid() override;

    // Launching. Each one starts the transport (from the current bar) if it's stopped.
    /** Plays the cell at `column` on the track from its next launch point (replacing the
        track's playing cell, or restarting it). */
    void launchCell (int trackId, int column);
    /** Back to the arrange, at the next launch point. */
    void stopTrack (int trackId);
    /** Every track plays its cell in this column, or stops if it has none, all on one sample. */
    void launchScene (int column);
    void stopAll();

    /** Records the track's input (or, on an instrument track, the notes played) into an empty
        cell from the next bar: `bars` long, or until finishRecording when 0. Then it loops. */
    bool recordCell (int trackId, int column, int bars = 0);
    /** Ends an open-ended recording on its next bar line. */
    void finishRecording();
    bool isRecordingCell (int trackId, int column) const;

    // Perform recording: while on, everything the grid plays is also written into the arrange.
    void setPerformRecording (bool);
    bool isPerformRecording() const                     { return perform; }

    /** What a cell is doing, for drawing. */
    struct Status
    {
        enum State { idle, queued, playing, stopping, recording, recordQueued } state = idle;
        float progress = 0.0f;      // through the current pass, 0..1
        int passBars = 0;
    };
    Status statusOf (int trackId, int column) const;
    /** The column playing on the track, or -1. */
    int playingColumn (int trackId) const;
    bool isStopQueued (int trackId) const;

    /** Drains the player's events (perform recording) and saves finished recordings. The
        timer calls it 30 times a second; tests call it directly. */
    void service();

    /** Told whenever something the grid shows has changed (a recording was saved...). */
    std::function<void()> onChange;

private:
    void timerCallback() override                       { service(); }
    daw::Project* project() const                       { return engine.getProject(); }
    void startTransportIfStopped();
    void commitRecording();
    void close (int trackId, juce::int64 at, bool& began);

    daw::Engine& engine;
    daw::Project* attached = nullptr;
    bool perform = false;
    std::vector<LoopEvent> events;
    struct Open { int column = -1; juce::int64 from = 0; };
    std::map<int, Open> open;       // perform recording: what each track has played since when

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopGrid)
};

} // namespace woodshed::loops
