#pragma once

#include "LoopModel.h"
#include "../instruments/Instrument.h"
#include <bitset>

namespace woodshed::daw
{
class AudioPool;
struct Snapshot;
struct TrackPlayback;
struct TrackRuntime;
}

namespace woodshed::loops
{

//==============================================================================
/** A cell as the audio thread plays it: one pass of `length` samples from the cell's start. */
struct CellPlayback
{
    struct Clip
    {
        std::shared_ptr<const juce::AudioBuffer<float>> audio;
        juce::int64 start = 0, offset = 0, length = 0;     // start within the pass; offset into the file
        juce::int64 fadeIn = 0, fadeOut = 0;
        float gain = 1.0f;
    };
    struct Event
    {
        juce::int64 time = 0;       // within the pass
        bool on = true;
        std::uint8_t pitch = 60;
        float velocity = 0.8f;
    };

    juce::int64 length = 0;         // 0: no cell in this column
    bool oneShot = false;
    std::vector<Clip> audio;
    std::vector<Event> midi;        // in time order, offs first; every note ends inside the pass

    bool isValid() const            { return length > 0; }
};

/** The cells of a track's grid, by column, for a snapshot (message thread). */
void buildCells (const daw::Project&, daw::AudioPool&, const juce::ValueTree& track, std::vector<CellPlayback>& cells);

//==============================================================================
/** What a track's grid is doing; lives on the track's runtime, so it survives new snapshots. */
struct TrackLoopState
{
    static constexpr int none = -1;

    // Audio thread ------------------------------------------------------------------
    int playing = none;                     // the playing (or recording) cell's column
    juce::int64 elapsed = 0;                // samples since it started
    bool recording = false, fallback = false;   // recording it; playing the fresh recording until the snapshot has it
    enum class Next { nothing, launch, stop, record } next = Next::nothing;
    int nextColumn = none;
    Quantize nextQuantize = Quantize::bar;
    juce::int64 nextAt = 0;                 // timeline sample
    std::bitset<128> sounding;              // the cell's notes now on

    /** This block, as computed by LoopPlayer::beginBlock: stretches that play one thing, and
        the switches between them. No segments: the arrange plays the whole block. */
    struct Segment { int from = 0, to = 0, column = none; juce::int64 elapsed = 0; bool recording = false, fallback = false; };
    struct Switch { int at = 0; bool fromCell = false, toCell = false; };
    std::array<Segment, 8> segments {};
    std::array<Switch, 8> switches {};
    int numSegments = 0, numSwitches = 0;

    // For the UI (written by the audio thread every block) --------------------------
    std::atomic<int> shownColumn { none }, shownNext { -2 };    // next: -2 nothing, -1 stop, else a column
    std::atomic<bool> shownRecording { false }, shownNextIsRecord { false };
    std::atomic<juce::int64> shownElapsed { 0 }, shownLength { 0 };
};

/** A request from the message thread, applied at the start of the next block. */
struct LoopCommand
{
    enum Kind : std::uint8_t { launch, stop, launchScene, stopAll, record, finishRecord } kind = launch;
    int trackId = -1, column = -1;
    Quantize quantize = Quantize::bar;
};

/** Something that happened, at an exact timeline sample, for the message thread (perform
    recording, and the record button's state). column -1: the track went back to the arrange. */
struct LoopEvent
{
    int trackId = -1, column = -1;
    juce::int64 at = 0;
};

//==============================================================================
/**
    The Loop Grid's audio-thread side. The message thread sends commands through a lock-free
    FIFO; Mixer::render calls beginBlock, which turns them into sample-accurate switches (on the
    launch quantize grid, computed from the block's own position), then the render hooks below,
    then endBlock. While a track plays a cell it doesn't play its arrange regions.

    Cells play only while the transport runs; stopping it stops every cell.

    Recording into a cell: one track at a time, into a buffer the message thread prepares. When
    the cell's length is reached the cell starts looping straight from that buffer, until the
    message thread has saved it and the snapshot carries the real cell.
*/
class LoopPlayer
{
public:
    LoopPlayer();

    // Message thread ------------------------------------------------------------------
    /** False if the FIFO is full. */
    bool send (const LoopCommand&);
    /** Moves the events since last time into `out`. */
    void takeEvents (std::vector<LoopEvent>& out);

    enum class RecordState : int { idle, waiting, recording, tail, finished, committed, discarded };
    RecordState getRecordState() const                  { return (RecordState) recordState.load(); }
    int getRecordTrack() const                          { return recordTrack; }
    int getRecordColumn() const                         { return recordColumn; }
    /** Starts a recording on `trackId` into `column` once the next bar comes (or the cell's
        quantize, if that's longer): `bars` long, or until finishRecord when 0. Audio is
        latency-compensated by `latency` samples; MIDI cells take the live notes. Only while
        idle. */
    bool startRecording (int trackId, int column, Quantize, int bars, bool midi, int latency, double sampleRate, double samplesPerBar);
    /** After finished: what was recorded. The audio covers [0, length + latency). */
    juce::int64 getRecordedLength() const               { return recordLength.load(); }
    int getRecordLatency() const                        { return recordLatency; }
    bool isMidiRecording() const                        { return recordMidi; }
    const juce::AudioBuffer<float>& getRecordedAudio() const    { return *recordBuffer; }
    const std::vector<CellPlayback::Event>& getRecordedNotes() const { return fallbackCell.midi; }
    /** The message thread saved the recording into the model. */
    void markCommitted()                                { recordState = (int) RecordState::committed; }
    /** Throws a finished recording away. */
    void discardRecording();

    // Audio thread ------------------------------------------------------------------
    void beginBlock (const daw::Snapshot&, juce::int64 position, int numSamples, bool playing);
    void endBlock (const daw::Snapshot&, juce::int64 position, int numSamples);

    /** True when the track's grid has something to do this block (else render as usual). */
    static bool isActive (const daw::TrackRuntime&);
    /** Adds the cell segments' audio. Arrange segments are left to the caller. */
    void renderAudio (const daw::TrackPlayback&, float* out) const;
    /** `events` holds the arrange's events for the block: drops those inside cell segments and
        adds the cells' own (with note-offs at every switch away from a cell). True if a note
        started. */
    bool mixMidi (const daw::TrackPlayback&, std::vector<NoteEvent>& events) const;
    /** True while the track records into a cell this block (it hears its input). */
    bool hearsInput (const daw::TrackRuntime&) const;
    void captureAudio (const daw::TrackRuntime&, const float* input, int numSamples);
    void captureMidi (const daw::TrackRuntime&, const NoteEvent* events, int numEvents);

private:
    const CellPlayback* cellFor (const daw::TrackPlayback&, const TrackLoopState&) const;
    void applyCommand (const daw::Snapshot&, const LoopCommand&, juce::int64 position);
    void queue (TrackLoopState&, TrackLoopState::Next, int column, Quantize, const daw::TimeGrid&, juce::int64 position);
    void log (int trackId, int column, juce::int64 at);
    void cancelRecording (TrackLoopState*);
    static void emitCell (const CellPlayback&, const TrackLoopState::Segment&, std::vector<NoteEvent>&, TrackLoopState&);

    juce::AbstractFifo commandFifo { 256 };
    std::array<LoopCommand, 256> commands {};
    juce::AbstractFifo eventFifo { 1024 };
    std::array<LoopEvent, 1024> events {};

    juce::int64 expectedPosition = -1;

    // Recording. The message thread sets these up while idle; then only the audio thread
    // writes until it's finished.
    std::atomic<int> recordState { (int) RecordState::idle };
    int recordTrack = -1, recordColumn = -1, recordLatency = 0, recordBars = 0;
    bool recordMidi = false;
    Quantize recordQuantize = Quantize::bar;
    double recordSamplesPerBar = 96000.0;
    std::shared_ptr<juce::AudioBuffer<float>> recordBuffer;
    std::atomic<juce::int64> recordLength { -1 };       // -1 until known (open-ended)
    juce::int64 written = 0;                            // samples captured so far
    int captureFrom = 0;                                // where capture starts in this block
    std::bitset<128> held;                              // MIDI recording: notes down
    CellPlayback fallbackCell;                          // plays the recording until the snapshot has it

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopPlayer)
};

} // namespace woodshed::loops
