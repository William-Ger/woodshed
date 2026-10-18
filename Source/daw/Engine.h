#pragma once

#include "Project.h"
#include "../instruments/Instrument.h"
#include "../engine/Registry.h"
#include "PluginWrappers.h"
#include "../loops/LoopPlayer.h"
#include "../spatial/Spatial.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace woodshed::daw
{

/** Recordings held in memory, shared by every region that plays them. Message thread. */
class AudioPool
{
public:
    AudioPool();
    using Audio = std::shared_ptr<const juce::AudioBuffer<float>>;

    /** Loads the file the first time it's asked for (mono or the first channel). Null if unreadable. */
    Audio get (const juce::File&);
    /** The same file in stereo (a mono file on both channels), for frozen tracks. */
    Audio getStereo (const juce::File&);
    void forget (const juce::File&);
    void clear()                                    { cache.clear(); }

    juce::AudioFormatManager formats;

private:
    std::map<juce::String, Audio> cache;
};

/** How to bounce: the file format ("wav", "aiff" or "flac"), bits (16, 24, or 32 for float),
    normalizing the peak to -0.1 dBFS, and stems (a file per track instead of the mix).
    surround: "5.1" or "7.1.4" for a multichannel WAV panned from the tracks' 3D positions
    (stereo tracks as a front pair; no LFE content, master effects skipped); empty for stereo. */
struct BounceOptions
{
    juce::String format { "wav" };
    int bits = 24;
    bool normalize = false;
    bool stems = false;
    juce::String surround;
};

/** Makes the guitar rig that sits on every audio track (the pedalboard, amp and cab). */
using RigFactory = std::function<std::unique_ptr<juce::AudioProcessor>()>;
/** Makes a software instrument by type id (InstrumentRegistry), or null if unknown. */
using InstrumentFactory = std::function<std::unique_ptr<Instrument> (const juce::String& type)>;

//==============================================================================
/** The waveform of a take while it records: one peak per `hop` samples, written by the audio
    thread and read by the arrange view. Preallocated, so the audio thread never allocates. */
struct LivePeaks
{
    static constexpr int hop = 256;

    explicit LivePeaks (double sampleRate, double maxMinutes = 60.0)
        : peaks ((size_t) juce::jmax (1.0, sampleRate * 60.0 * maxMinutes / hop), 0.0f) {}

    /** Audio thread. */
    void add (const float* data, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            pending = juce::jmax (pending, std::abs (data[i]));
            if (++inHop == hop)
            {
                const auto c = count.load (std::memory_order_relaxed);
                if (c < (int) peaks.size())
                {
                    peaks[(size_t) c] = pending;
                    count.store (c + 1, std::memory_order_release);
                }
                pending = 0.0f;
                inHop = 0;
            }
        }
    }

    /** Message thread: how many peaks are ready to read. */
    int size() const noexcept                       { return count.load (std::memory_order_acquire); }
    float operator[] (int i) const noexcept         { return peaks[(size_t) i]; }

private:
    std::vector<float> peaks;
    std::atomic<int> count { 0 };
    float pending = 0.0f;
    int inHop = 0;
};

//==============================================================================
/** A stereo history of a track's signal for delay compensation: written once per block, read
    back at any delay up to its capacity less the block. Fixed size, so the audio thread never
    allocates; one that was never allocated reads as no delay. */
struct DelayRing
{
    /** The longest compensation, in samples (about 340 ms at 48 kHz); longer latencies are clamped. */
    static constexpr int capacity = 1 << 14;

    void allocate()                                 { data.setSize (2, capacity); data.clear(); write = 0; }
    bool isAllocated() const noexcept               { return data.getNumSamples() == capacity; }
    void clear() noexcept                           { if (isAllocated()) data.clear(); write = 0; }

    /** Audio thread: appends a block. */
    void push (const float* left, const float* right, int n) noexcept;
    /** Audio thread: adds the block just pushed, `delay` samples late, times `gain`. */
    void addDelayed (int delay, float* left, float* right, int n, float gain) const noexcept;

    juce::AudioBuffer<float> data;
    int write = 0;
};

//==============================================================================
/** One insert effect's pedal, kept while its FX node keeps its id and type. */
struct InsertRuntime
{
    int id = 0;
    juce::String type;
    std::shared_ptr<Pedal> pedal;
};

/** Per-track state that lives as long as the track does, shared by successive snapshots. */
struct TrackRuntime
{
    int id = 0;
    std::unique_ptr<juce::AudioProcessor> rig;
    juce::String instrumentType;
    std::shared_ptr<Instrument> instrument;                             // instrument tracks
    std::vector<InsertRuntime> inserts;                                 // message thread
    std::vector<NoteEvent> events;                                      // audio thread scratch
    juce::int64 expectedPosition = -1;                                  // audio thread: where the next block should start
    bool notesSounding = false;                                         // audio thread
    juce::AudioBuffer<float> work;                                      // audio thread scratch
    juce::AudioBuffer<float> busInput;                                  // aux tracks: the sends they receive
    DelayRing preFader, postFader;                                      // delay compensation (audio thread)
    juce::SmoothedValue<float> gainLeft, gainRight;
    std::unique_ptr<spatial::Panner> panner;                            // 3D tracks and surround bounces
    std::atomic<float> peakLeft { 0.0f }, peakRight { 0.0f };          // audio thread accumulates
    float shownLeft = 0.0f, shownRight = 0.0f;                          // message thread: last 30 Hz frame
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> recorder { nullptr };
    std::atomic<LivePeaks*> livePeaks { nullptr };                      // set together with recorder
    loops::TrackLoopState loop;                                         // the Loop Grid on this track
};

struct RegionPlayback
{
    AudioPool::Audio audio;
    juce::int64 start = 0, offset = 0, length = 0;
    juce::int64 fadeIn = 0, fadeOut = 0;    // at least the de-click edge, together at most `length`
    float gain = 1.0f;
};

/** A note-on or note-off of a MIDI region, at a timeline sample. */
struct MidiPlayback
{
    juce::int64 time = 0;
    bool on = true;
    std::uint8_t pitch = 60;
    float velocity = 0.8f;
};

struct InsertPlayback
{
    int id = 0;
    std::shared_ptr<Pedal> pedal;
    bool on = true;
    std::array<float, Pedal::maxKnobs> knobs {};
};

struct TrackPlayback
{
    std::shared_ptr<TrackRuntime> runtime;
    std::vector<RegionPlayback> regions;
    std::shared_ptr<Instrument> instrument;                             // instrument tracks
    std::array<float, Instrument::maxKnobs> instrumentKnobs {};
    std::vector<MidiPlayback> midi;                                     // in time order, offs first
    std::vector<InsertPlayback> inserts;

    /** A send: this track's signal also goes to the aux at `target` (an index into the snapshot's
        tracks, always later than this one, so it plays after every track feeding it). */
    struct Send { int target = -1; float gain = 1.0f; bool pre = false; int delay = 0; };
    std::vector<Send> sends;
    bool aux = false;

    // Mixing: stacks, VCAs, delay compensation and freeze.
    /** False for a summing stack's member: it reaches the Stereo Out through its main, the last of
        its sends (post-fader, unity). */
    bool toMaster = true;
    /** Gains on the fader from VCAs and folder stack mains: a fixed one, or a line (in dB). */
    struct Scale { float gain = 1.0f; std::vector<AutoPoint> points; };
    std::vector<Scale> scalers;
    /** How late the track's own chain makes its signal (rig, instrument, inserts), and how much
        more its output is delayed so every path reaches the Stereo Out together. */
    int latency = 0, outputDelay = 0;
    /** A frozen track: plays this stereo render (pre-fader, from sample 0) instead of itself. */
    AudioPool::Audio frozen;

    /** An automation line and what it drives. */
    struct AutoLine
    {
        enum Target { volume, pan, instrumentKnob, insertKnob, sendLevel, spatialParam } target = volume;
        int insert = 0, knob = 0;           // insertKnob: index into `inserts`; knobs: knob index; sendLevel: index into `sends`;
                                            // spatialParam: 0 azimuth, 1 elevation, 2 distance, 3 spread
        std::vector<AutoPoint> points;      // at least one, in time order
    };
    std::vector<AutoLine> automation;
    float gain = 1.0f, pan = 0.0f;
    bool spatial = false;           // placed in 3D (`position`) instead of panned
    spatial::Position position;
    bool audible = true;            // mute and solo resolved
    bool armed = false, monitor = false, rigOn = true;
    int input = 0;
    std::vector<loops::CellPlayback> cells;                             // the Loop Grid's cells, by column
};

/** An immutable picture of the song for the audio thread. */
struct Snapshot
{
    std::vector<TrackPlayback> tracks;
    std::vector<InsertPlayback> masterInserts;      // on the summed mix, before the master fader
    TimeGrid grid;
    float masterGain = 1.0f;
    bool click = true, loopOn = false;
    juce::int64 loopStart = 0, loopEnd = 0;
    spatial::Monitoring monitoring = spatial::Monitoring::stereo;     // how 3D tracks reach the stereo master
    /** How late the mix reaches the output once every path is lined up (the longest path plus the
        master's inserts). The click and recordings are moved by the same amount. */
    int latency = 0;

    /** Delay compensation: works out each track's output and send delays from the tracks'
        `latency`, so every path reaches the Stereo Out (and each bus) at the same time, and sets
        `latency`. buildSnapshot calls it; call it again after changing the routing. */
    void alignLatencies();
};

/** Mixes one block of a snapshot. Shared by live playback and offline bounces. */
struct Mixer
{
    struct Block
    {
        juce::int64 position = 0;       // timeline sample of the block's first sample
        int numSamples = 0;
        bool playing = false;
        bool recording = false;
        const float* const* inputs = nullptr;
        int numInputs = 0;
        bool monitorInputs = true;      // false: feedback protection, the live input isn't heard
        float* left = nullptr;
        float* right = nullptr;         // cleared, then the mix is added

        // A surround bounce: every track pans into these channels (cleared first) instead of left
        // and right, and the master effects are skipped.
        const spatial::SpeakerLayout* surround = nullptr;
        float* const* surroundOut = nullptr;

        // Notes played live (a MIDI keyboard or musical typing), for one track.
        int liveTrackId = -1;
        const NoteEvent* liveEvents = nullptr;     // offsets within this block, in order
        int numLiveEvents = 0;

        loops::LoopPlayer* loops = nullptr;         // the Loop Grid (live playback only; bounces leave it out)
    };

    static void render (const Snapshot&, const Block&);
    static void addClick (const Snapshot&, const Block&, float level);
};

//==============================================================================
/**
    Plays and records a Project through the audio interface.

    A track hears its interface input live through its rig when input monitoring is on, or when
    it's armed and the transport is stopped or recording; otherwise it plays its regions through
    its rig. So an armed track plays back its take, and goes live again to record. Recording writes the dry input to a WAV per
    armed track and, on stop, adds a region placed back by the interface's round-trip latency
    so the take lines up with what you were hearing.
*/
class Engine final : public juce::AudioIODeviceCallback,
                     public juce::ChangeBroadcaster,
                     private juce::ValueTree::Listener,
                     private juce::AsyncUpdater,
                     private juce::Timer
{
public:
    Engine (juce::AudioDeviceManager&, RigFactory, InstrumentFactory = {});
    ~Engine() override;

    /** Message thread. The engine doesn't own the project; pass nullptr before deleting it. */
    void setProject (Project*);
    Project* getProject() const                             { return project; }

    // Transport (message thread). Changes are broadcast.
    void play();
    void stop();
    void togglePlay()                                       { isPlaying() ? stop() : play(); }
    void record();
    void toggleRecord()                                     { isRecording() ? stop() : record(); }
    bool isPlaying() const                                  { return playing.load(); }
    bool isRecording() const                                { return recording.load(); }
    bool isCountingIn() const;
    /** Where the current (or last) recording starts on the timeline, and how much it has so far. */
    juce::int64 getRecordStart() const                      { return recordStart.load(); }
    juce::int64 getRecordedLength() const                   { return recordedSamples.load(); }
    /** The waveform recorded so far on this track, while recording (else null). Message thread;
        don't keep the pointer past the current call. Peak i covers the recording's samples
        [i * hop, (i + 1) * hop), counted from getRecordStart(). */
    const LivePeaks* getLivePeaks (int trackId) const;
    /** Where the take being recorded will land once latency is taken out (see placeRecording). */
    juce::int64 getLiveTakeOffset() const                   { return latencyAtRecord; }
    juce::int64 getPosition() const                         { return position.load(); }
    void setPosition (juce::int64);

    // Tracks
    juce::AudioProcessor* getRig (int trackId) const;
    /** Removes a track through the undo manager, keeping its rig settings so undo brings them back. */
    void removeTrack (juce::ValueTree track);
    /** Copies every rig's settings into its track's RIG child; call before saving. */
    void syncRigStates();

    /** The instrument on an instrument track (for its panel), or null. */
    Instrument* getInstrument (int trackId) const;
    /** The same, shared, so a plug-in window can keep it alive while it shows its editor. */
    std::shared_ptr<Instrument> shareInstrument (int trackId) const;
    /** An insert's pedal (a plug-in's wrapper, for its window); trackId -1 for the master. */
    std::shared_ptr<Pedal> shareInsert (int trackId, int fxId) const;

    /** Where Audio Units and VST3s come from. Without one, plug-in tracks and inserts are silent. */
    void setPluginFactory (PluginFactory f)                 { makePlugin = std::move (f); }
    /** Why the last plug-in failed to load, if one did. */
    juce::String getPluginError() const                     { return pluginError; }

    // Live MIDI. Every enabled MIDI input, and musical typing, plays the target track (the
    // selected one, when it's an instrument track) and records into it when it's armed.
    void setMidiTarget (int trackId)                        { midiTarget = trackId; }
    int getMidiTarget() const                               { return midiTarget.load(); }
    /** Message thread or a MIDI thread. */
    void sendLiveMidi (const juce::MidiMessage&);
    /** Turns on every MIDI input the system has (new ones too); call now and then. */
    void enableMidiInputs();
    /** Notes recorded so far in the current take (for drawing), at timeline samples. */
    struct RecordedNote { juce::int64 start = 0, end = -1; int pitch = 60, velocity = 100; };
    const std::vector<RecordedNote>& getRecordedNotes() const   { return recordedNotes; }

    // Meters: the peak over the last 30 Hz frame, linear. Any number of views can read them.
    struct Levels { float left = 0.0f, right = 0.0f; };
    Levels takeTrackLevels (int trackId);
    Levels takeMasterLevels();

    /** Feedback protection: while on, no track plays its live input (recording still works).
        For a computer's own microphone and speakers, which would howl. */
    void setFeedbackProtection (bool on)                    { feedbackProtection = on; }
    bool getFeedbackProtection() const                      { return feedbackProtection.load(); }

    /** Metronome loudness, linear (Settings). */
    void setClickLevel (float level)                        { clickLevel = level; }
    float getClickLevel() const                             { return clickLevel.load(); }

    double getSampleRate() const                            { return sampleRate; }
    int getBlockSize() const                                { return blockSize; }
    int getRecordLatencySamples() const;
    /** Audio callbacks so far: if this stops moving while a device is open, the device has died. */
    juce::uint64 getCallbackCount() const                   { return callbackCount.load(); }

    // Bounce: renders [start, end) plus a tail with fresh copies of every rig, on a background
    // thread. `done` is called on the message thread. `target` is the file, or for stems the
    // folder that gets a file per track.
    void startBounce (const juce::File& target, juce::int64 start, juce::int64 end, const BounceOptions&,
                      std::function<void (bool ok)> done);
    bool isBouncing() const                                 { return bounceThread != nullptr; }
    float getBounceProgress() const                         { return bounceProgress.load(); }

    // Freeze: a track's instrument, rig and inserts rendered (pre-fader) to a file in the
    // project's Freeze folder, which it then plays instead, for none of their CPU.
    /** Renders `track` to `file` right here (the bench uses this; the UI uses freezeTrack). */
    bool renderFreeze (const juce::ValueTree& track, const juce::File& file);
    /** Renders the track on a background thread, then freezes it (one undo step). */
    void freezeTrack (juce::ValueTree track, std::function<void (bool ok)> done = {});
    /** Plays the track live again (one undo step); its freeze file stays for undo. */
    void unfreezeTrack (juce::ValueTree track);
    /** Where a track's freeze file goes. */
    juce::File freezeFileFor (const juce::ValueTree& track) const;

    /** How late playback reaches the output because of plug-in latency (delay compensation). */
    int getPlaybackLatency() const                          { return playbackLatency.load(); }

    /** Called on each bounce's private rigs after their first block, to make a restored board
        take effect immediately (the rig otherwise does it on its own timer). */
    std::function<void (juce::AudioProcessor&)> settleRig;

    AudioPool& getAudioPool()                               { return pool; }
    /** The Loop Grid's audio-thread side (see loops::LoopGrid for the message thread's). */
    loops::LoopPlayer& getLoopPlayer()                      { return loopPlayer; }
    /** A track's runtime (its Loop Grid state, for drawing), or null. Message thread. */
    std::shared_ptr<TrackRuntime> getRuntime (int trackId) const
    {
        auto it = runtimes.find (trackId);
        return it != runtimes.end() ? it->second : nullptr;
    }
    juce::AudioDeviceManager& getDeviceManager()            { return deviceManager; }

    /** Builds a snapshot of `project` using the given runtimes (by track id); used by tests too. */
    static std::unique_ptr<Snapshot> buildSnapshot (const Project&, AudioPool&,
                                                    const std::map<int, std::shared_ptr<TrackRuntime>>&,
                                                    const TrackRuntime* master = nullptr);

    /** One pass of a cycle recording: where it belongs on the timeline once latency is taken out
        (it runs a little past the cycle's end, by the latency, so it covers the whole cycle). */
    struct Pass { juce::int64 start = 0, fileOffset = 0, length = 0; bool complete = true; };
    /** A recording of `recorded` samples that started at `recordStart` while cycling over
        [loopStart, loopEnd): the passes it made, the first one from recordStart. */
    static std::vector<Pass> cyclePasses (juce::int64 recordStart, juce::int64 loopStart, juce::int64 loopEnd,
                                          juce::int64 recorded, int latency);

    /** Renders [start, end) of a snapshot to a stereo file, dropping its first snap.latency
        samples so it lines up with the timeline; `progress` gets 0..1, `shouldStop` aborts. */
    static bool renderToFile (Snapshot&, const juce::File&, juce::int64 start, juce::int64 end, const BounceOptions&,
                              double sampleRate, const std::function<void (float)>& progress = {},
                              const std::function<bool()>& shouldStop = {});

    /** Where a recording that started at `recordStart` belongs once latency is taken out. */
    static void placeRecording (juce::int64 recordStart, int latency, juce::int64& start, juce::int64& offset);

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs,
                                           int numOutputs, int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    /** What the engine's 30 Hz timer does (meters, freeing old snapshots, closing finished
        takes); tests call it directly. Message thread. */
    void service()                                          { timerCallback(); }
    /** What audioDeviceAboutToStart does; tests call it directly. No callbacks may be running. */
    void prepare (double sampleRate, int blockSize);
    void audioDeviceStopped() override;

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override             { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override      { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override              { triggerAsyncUpdate(); }
    void valueTreeRedirected (juce::ValueTree&) override                               { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;
    void timerCallback() override;

    void syncRuntimes();
    void prepareRig (TrackRuntime&);
    /** Brings a runtime's instrument and inserts in line with its track (message thread). */
    void syncTrackDevices (TrackRuntime&, const juce::ValueTree& track, double rate, int block);
    /** A snapshot of the song with private rigs and plug-ins for a bounce: everything, or one track. */
    std::unique_ptr<Snapshot> makeBounceSnapshot (int onlyTrackId, bool withMasterEffects);
    /** A bounce snapshot of one track pre-fader, straight to the output: what freezing renders. */
    std::unique_ptr<Snapshot> makeFreezeSnapshot (int trackId);
    /** Renders one snapshot to a file on the bounce thread (start to end plus the tail); `done`
        runs on the message thread. */
    void startRender (std::unique_ptr<Snapshot>, const juce::File&, juce::int64 start, juce::int64 end,
                      const BounceOptions&, std::function<void (bool ok)> done);
    /** Every runtime's reported latencies, summed: when it changes the snapshot is rebuilt. */
    int latencyKey() const;
    void drainRecordedMidi();
    void finishMidiTake (int trackId, juce::int64 stopAt);
    void publish();
    void finishRecording();
    void freeRetired();

    juce::AudioDeviceManager& deviceManager;
    RigFactory makeRig;
    InstrumentFactory makeInstrument;
    PluginFactory makePlugin;
    juce::String pluginError;
    /** Restores a plug-in's saved state (the node's `state`). */
    static void restorePluginState (juce::AudioPluginInstance&, const juce::ValueTree& node);
    Project* project = nullptr;
    AudioPool pool;

    std::map<int, std::shared_ptr<TrackRuntime>> runtimes;      // message thread
    std::shared_ptr<TrackRuntime> masterRuntime;                // the master's inserts

    // Snapshot hand-off, like the rig's chain: publish to `pending`, the audio thread swaps it
    // into `current` and hands the old one back through `retired` for deletion here.
    std::atomic<Snapshot*> pending { nullptr };
    Snapshot* current = nullptr;
    juce::AbstractFifo retireFifo { 64 };
    std::array<Snapshot*, 64> retired {};

    double sampleRate = 48000.0;
    int blockSize = 512;
    juce::AudioBuffer<float> mixBuffer;

    std::atomic<bool> playing { false }, recording { false };
    std::atomic<juce::int64> position { 0 }, recordStart { 0 };
    std::atomic<juce::int64> requestedPosition { -1 };
    std::atomic<float> masterPeakLeft { 0.0f }, masterPeakRight { 0.0f };
    std::atomic<float> clickLevel { 0.3f };
    std::atomic<bool> feedbackProtection { false };
    Levels masterShown;
    std::atomic<juce::uint64> callbackCount { 0 };

    // Live MIDI in, and what the audio thread records of it for the message thread.
    juce::MidiMessageCollector liveMidi;
    juce::MidiBuffer liveBuffer;                                    // audio thread
    std::vector<NoteEvent> liveEvents;                              // audio thread
    std::atomic<int> midiTarget { -1 };
    struct MidiStamp { juce::int64 time; std::uint8_t pitch, velocity; bool on; };
    juce::AbstractFifo midiRecordFifo { 8192 };
    std::array<MidiStamp, 8192> midiRecordRing {};
    std::atomic<int> midiRecordTrack { -1 };                        // the take's track while recording
    std::vector<RecordedNote> recordedNotes;                        // message thread
    int midiLatencyAtRecord = 0;

    // Recording bookkeeping (message thread).
    struct Take { int trackId; juce::File file; std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
                  std::unique_ptr<LivePeaks> peaks; juce::int64 samples = 0; bool midi = false; };
    std::vector<Take> takes;
    std::atomic<juce::int64> recordedSamples { 0 };
    juce::uint64 stopRequestedAt = 0;
    bool finishPending = false;
    int latencyAtRecord = 0;
    std::atomic<bool> cycleRecording { false };
    juce::int64 cycleStart = 0, cycleEnd = 0;

    // Autopunch: while recording, input is only written (and heard) in [punchFrom, punchTo).
    std::atomic<bool> punching { false };
    std::atomic<juce::int64> punchFrom { 0 }, punchTo { 0 };
    std::atomic<bool> punchDone { false };                          // set by the audio thread once past punchTo
    // Delay compensation: the current snapshot's latency, and what it was built from.
    std::atomic<int> playbackLatency { 0 };
    int publishedLatencyKey = 0;
    juce::TimeSliceThread writerThread { "Woodshed recorder" };

    loops::LoopPlayer loopPlayer;

    std::unique_ptr<juce::Thread> bounceThread;
    std::atomic<float> bounceProgress { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};

} // namespace woodshed::daw
