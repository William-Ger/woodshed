#pragma once

#include "Project.h"
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
    void forget (const juce::File&);
    void clear()                                    { cache.clear(); }

    juce::AudioFormatManager formats;

private:
    std::map<juce::String, Audio> cache;
};

/** Makes the guitar rig that sits on every audio track (the pedalboard, amp and cab). */
using RigFactory = std::function<std::unique_ptr<juce::AudioProcessor>()>;

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
/** Per-track state that lives as long as the track does, shared by successive snapshots. */
struct TrackRuntime
{
    int id = 0;
    std::unique_ptr<juce::AudioProcessor> rig;
    juce::AudioBuffer<float> work;                                      // audio thread scratch
    juce::SmoothedValue<float> gainLeft, gainRight;
    std::atomic<float> peakLeft { 0.0f }, peakRight { 0.0f };          // audio thread accumulates
    float shownLeft = 0.0f, shownRight = 0.0f;                          // message thread: last 30 Hz frame
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> recorder { nullptr };
    std::atomic<LivePeaks*> livePeaks { nullptr };                      // set together with recorder
};

struct RegionPlayback
{
    AudioPool::Audio audio;
    juce::int64 start = 0, offset = 0, length = 0;
    juce::int64 fadeIn = 0, fadeOut = 0;    // at least the de-click edge, together at most `length`
    float gain = 1.0f;
};

struct TrackPlayback
{
    std::shared_ptr<TrackRuntime> runtime;
    std::vector<RegionPlayback> regions;
    float gain = 1.0f, pan = 0.0f;
    bool audible = true;            // mute and solo resolved
    bool armed = false, monitor = false, rigOn = true;
    int input = 0;
};

/** An immutable picture of the song for the audio thread. */
struct Snapshot
{
    std::vector<TrackPlayback> tracks;
    TimeGrid grid;
    float masterGain = 1.0f;
    bool click = true, loopOn = false;
    juce::int64 loopStart = 0, loopEnd = 0;
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
    Engine (juce::AudioDeviceManager&, RigFactory);
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
    // thread. `done` is called on the message thread.
    void startBounce (const juce::File& wav, juce::int64 start, juce::int64 end, std::function<void (bool ok)> done);
    bool isBouncing() const                                 { return bounceThread != nullptr; }
    float getBounceProgress() const                         { return bounceProgress.load(); }

    /** Called on each bounce's private rigs after their first block, to make a restored board
        take effect immediately (the rig otherwise does it on its own timer). */
    std::function<void (juce::AudioProcessor&)> settleRig;

    AudioPool& getAudioPool()                               { return pool; }
    juce::AudioDeviceManager& getDeviceManager()            { return deviceManager; }

    /** Builds a snapshot of `project` using the given runtimes (by track id); used by tests too. */
    static std::unique_ptr<Snapshot> buildSnapshot (const Project&, AudioPool&,
                                                    const std::map<int, std::shared_ptr<TrackRuntime>>&);

    /** Where a recording that started at `recordStart` belongs once latency is taken out. */
    static void placeRecording (juce::int64 recordStart, int latency, juce::int64& start, juce::int64& offset);

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs,
                                           int numOutputs, int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
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
    void publish();
    void finishRecording();
    void freeRetired();

    juce::AudioDeviceManager& deviceManager;
    RigFactory makeRig;
    Project* project = nullptr;
    AudioPool pool;

    std::map<int, std::shared_ptr<TrackRuntime>> runtimes;      // message thread

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

    // Recording bookkeeping (message thread).
    struct Take { int trackId; juce::File file; std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
                  std::unique_ptr<LivePeaks> peaks; juce::int64 samples = 0; };
    std::vector<Take> takes;
    std::atomic<juce::int64> recordedSamples { 0 };
    juce::uint64 stopRequestedAt = 0;
    bool finishPending = false;
    int latencyAtRecord = 0;
    juce::TimeSliceThread writerThread { "Woodshed recorder" };

    std::unique_ptr<juce::Thread> bounceThread;
    std::atomic<float> bounceProgress { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};

} // namespace woodshed::daw
