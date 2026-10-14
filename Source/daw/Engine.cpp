#include "Engine.h"

namespace woodshed::daw
{

namespace
{
constexpr int edgeFadeSamples = 96;            // de-click every region start and end (2 ms at 48k)
constexpr double bounceTailSeconds = 4.0;      // let delays and reverbs ring out past the last note
constexpr int bounceBlock = 512;

float dbToGain (double db)                     { return db <= -96.0 ? 0.0f : juce::Decibels::decibelsToGain ((float) db); }

void restoreRig (juce::AudioProcessor& rig, const juce::ValueTree& track)
{
    const auto encoded = track.getChildWithName (ids::RIG)[ids::state].toString();
    if (encoded.isEmpty())
        return;

    juce::MemoryBlock data;
    if (data.fromBase64Encoding (encoded))
        rig.setStateInformation (data.getData(), (int) data.getSize());
}

void storeRig (juce::AudioProcessor& rig, juce::ValueTree track)
{
    juce::MemoryBlock data;
    rig.getStateInformation (data);
    track.getOrCreateChildWithName (ids::RIG, nullptr).setProperty (ids::state, data.toBase64Encoding(), nullptr);
}

/** The rig picks its own input channel; in a track it always gets the track's signal on channel 0. */
void pointRigAtChannelZero (juce::AudioProcessor& rig)
{
    for (auto* p : rig.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p); withId != nullptr && withId->paramID == "input")
            withId->setValueNotifyingHost (0.0f);
}
}

//==============================================================================
AudioPool::AudioPool()
{
    formats.registerBasicFormats();
}

AudioPool::Audio AudioPool::get (const juce::File& file)
{
    const auto key = file.getFullPathName();
    if (auto it = cache.find (key); it != cache.end())
        return it->second;

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max())
        return nullptr;

    const int length = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> read ((int) reader->numChannels, length);
    reader->read (&read, 0, length, 0, true, true);

    // Takes are mono DI; a stereo file is folded to mono so it can feed a rig.
    auto mono = std::make_shared<juce::AudioBuffer<float>> (1, length);
    mono->copyFrom (0, 0, read, 0, 0, length);
    if (read.getNumChannels() > 1)
    {
        for (int ch = 1; ch < read.getNumChannels(); ++ch)
            mono->addFrom (0, 0, read, ch, 0, length);
        mono->applyGain (1.0f / (float) read.getNumChannels());
    }

    Audio audio = std::move (mono);
    cache[key] = audio;
    return audio;
}

void AudioPool::forget (const juce::File& file)
{
    cache.erase (file.getFullPathName());
}

//==============================================================================
void Mixer::render (const Snapshot& snap, const Block& b)
{
    const int n = b.numSamples;
    juce::FloatVectorOperations::clear (b.left, n);
    juce::FloatVectorOperations::clear (b.right, n);

    for (auto& track : snap.tracks)
    {
        auto& rt = *track.runtime;
        jassert (rt.work.getNumSamples() >= n);
        float* a = rt.work.getWritePointer (0);
        float* c = rt.work.getWritePointer (1);
        juce::FloatVectorOperations::clear (a, n);

        const bool live = (track.armed || track.monitor) && b.inputs != nullptr;

        if (live)
        {
            if (juce::isPositiveAndBelow (track.input, b.numInputs) && b.inputs[track.input] != nullptr)
                juce::FloatVectorOperations::copy (a, b.inputs[track.input], n);
        }
        else if (b.playing)
        {
            for (auto& r : track.regions)
            {
                const auto from = juce::jmax (r.start, b.position);
                const auto to = juce::jmin (r.start + r.length, b.position + n);
                if (from >= to)
                    continue;

                const float* src = r.audio->getReadPointer (0);
                for (auto t = from; t < to; ++t)
                {
                    const auto intoRegion = t - r.start;
                    const auto fromEnd = r.start + r.length - 1 - t;
                    float g = r.gain;
                    if (intoRegion < edgeFadeSamples) g *= (float) intoRegion / edgeFadeSamples;
                    if (fromEnd < edgeFadeSamples)    g *= (float) fromEnd / edgeFadeSamples;
                    a[t - b.position] += src[r.offset + intoRegion] * g;
                }
            }
        }

        juce::FloatVectorOperations::copy (c, a, n);

        if (track.rigOn && rt.rig != nullptr)
        {
            juce::AudioBuffer<float> view (rt.work.getArrayOfWritePointers(), 2, n);
            juce::MidiBuffer noMidi;
            rt.rig->processBlock (view, noMidi);
        }

        // Constant-power pan, unity in the middle.
        const float angle = (juce::jlimit (-1.0f, 1.0f, track.pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        const float g = track.audible ? track.gain : 0.0f;
        rt.gainLeft.setTargetValue (g * std::cos (angle) * juce::MathConstants<float>::sqrt2);
        rt.gainRight.setTargetValue (g * std::sin (angle) * juce::MathConstants<float>::sqrt2);

        float peakL = 0.0f, peakR = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float l = a[i] * rt.gainLeft.getNextValue();
            const float r = c[i] * rt.gainRight.getNextValue();
            b.left[i] += l;
            b.right[i] += r;
            peakL = juce::jmax (peakL, std::abs (l));
            peakR = juce::jmax (peakR, std::abs (r));
        }

        rt.peakLeft.store (juce::jmax (rt.peakLeft.load (std::memory_order_relaxed), peakL), std::memory_order_relaxed);
        rt.peakRight.store (juce::jmax (rt.peakRight.load (std::memory_order_relaxed), peakR), std::memory_order_relaxed);
    }
}

void Mixer::addClick (const Snapshot& snap, const Block& b, float level)
{
    // A short pitched blip on every beat, higher on the downbeat. Stateless, so it works for
    // negative positions (the count-in) and right after a jump.
    const double spb = snap.grid.samplesPerBeat();
    const double sr = snap.grid.sampleRate;
    const int clickLength = (int) (sr * 0.03);

    for (int i = 0; i < b.numSamples; ++i)
    {
        const double t = (double) (b.position + i);
        const double beat = std::floor (t / spb);
        const double since = t - beat * spb;
        if (since >= clickLength)
            continue;

        const bool downbeat = ((juce::int64) beat % snap.grid.beatsPerBar + snap.grid.beatsPerBar) % snap.grid.beatsPerBar == 0;
        const double hz = downbeat ? 1660.0 : 1100.0;
        const float v = level * (float) (std::sin (juce::MathConstants<double>::twoPi * hz * since / sr) * std::exp (-since / (sr * 0.007)));
        b.left[i] += v;
        b.right[i] += v;
    }
}

//==============================================================================
Engine::Engine (juce::AudioDeviceManager& dm, RigFactory factory)
    : deviceManager (dm), makeRig (std::move (factory))
{
    writerThread.startThread();
    deviceManager.addAudioCallback (this);
    startTimerHz (30);
}

Engine::~Engine()
{
    stopTimer();
    if (bounceThread != nullptr)
        bounceThread->stopThread (5000);

    deviceManager.removeAudioCallback (this);
    setProject (nullptr);

    delete pending.exchange (nullptr);
    delete current;
    current = nullptr;
    freeRetired();
    writerThread.stopThread (2000);
}

void Engine::setProject (Project* p)
{
    if (project != nullptr)
    {
        if (isRecording() || isPlaying())
            stop();
        if (finishPending)
            finishRecording();
        project->state.removeListener (this);
    }

    project = p;
    position = 0;
    requestedPosition = 0;

    // Every runtime belongs to the old project; snapshots still holding them free them later.
    runtimes.clear();
    pool.clear();

    if (project != nullptr)
        project->state.addListener (this);

    handleAsyncUpdate();
    sendChangeMessage();
}

//==============================================================================
bool Engine::isCountingIn() const
{
    return isRecording() && getPosition() < recordStart.load();
}

void Engine::setPosition (juce::int64 p)
{
    p = juce::jmax ((juce::int64) 0, p);
    requestedPosition = p;
    position = p;
    sendChangeMessage();
}

void Engine::play()
{
    if (project == nullptr || isPlaying())
        return;

    playing = true;
    sendChangeMessage();
}

void Engine::stop()
{
    if (isRecording())
    {
        recording = false;
        for (auto& t : takes)
            if (auto it = runtimes.find (t.trackId); it != runtimes.end())
                it->second->recorder = nullptr;

        stopRequestedAt = callbackCount.load();
        finishPending = true;
    }

    playing = false;
    sendChangeMessage();
}

void Engine::record()
{
    if (project == nullptr || isRecording() || finishPending)
        return;

    // Nothing armed: arm the selected track (or the first one).
    bool anyArmed = false;
    for (auto t : project->state)
        anyArmed = anyArmed || (t.hasType (ids::TRACK) && (bool) t[ids::arm]);

    if (! anyArmed)
    {
        auto t = project->trackById ((int) project->state[ids::selectedTrack]);
        if (! t.isValid() && project->numTracks() > 0)
            t = project->track (0);
        if (! t.isValid())
            return;
        t.setProperty (ids::arm, true, nullptr);
    }

    handleAsyncUpdate();    // runtimes and the snapshot must know about the armed tracks now
    project->audioFolder().createDirectory();

    takes.clear();
    for (auto t : project->state)
    {
        if (! t.hasType (ids::TRACK) || ! (bool) t[ids::arm])
            continue;

        const int id = (int) t[ids::id];
        auto it = runtimes.find (id);
        if (it == runtimes.end())
            continue;

        auto file = project->audioFolder().getNonexistentChildFile (juce::File::createLegalFileName (t[ids::name].toString()) + " take", ".wav", true);
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        if (stream == nullptr)
            continue;

        juce::WavAudioFormat wav;
        const auto options = juce::AudioFormatWriterOptions{}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (24);
        auto writer = wav.createWriterFor (stream, options);
        if (writer == nullptr)
            continue;

        Take take;
        take.trackId = id;
        take.file = file;
        take.writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), writerThread, 1 << 17);
        it->second->recorder = take.writer.get();
        takes.push_back (std::move (take));
    }

    if (takes.empty())
        return;

    latencyAtRecord = getRecordLatencySamples();
    recordedSamples = 0;

    const auto grid = project->grid();
    const auto from = getPosition();
    recordStart = from;

    // Recording from a stop gets a bar of count-in (when it's on); punching in while playing doesn't.
    if (! isPlaying() && (bool) project->state[ids::countIn])
    {
        const auto preroll = from - (juce::int64) grid.samplesPerBar();
        requestedPosition = preroll;
        position = preroll;
    }

    recording = true;
    playing = true;
    sendChangeMessage();
}

int Engine::getRecordLatencySamples() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();
    return 0;
}

void Engine::placeRecording (juce::int64 start, int latency, juce::int64& regionStart, juce::int64& offset)
{
    // The input arrives `latency` late relative to the click you played along with.
    regionStart = start - latency;
    offset = 0;
    if (regionStart < 0)
    {
        offset = -regionStart;
        regionStart = 0;
    }
}

void Engine::finishRecording()
{
    finishPending = false;
    const auto samples = recordedSamples.load();

    juce::int64 start = 0, offset = 0;
    placeRecording (recordStart.load(), latencyAtRecord, start, offset);

    bool added = false;
    for (auto& t : takes)
    {
        t.writer.reset();   // flushes and closes the file

        auto track = project != nullptr ? project->trackById (t.trackId) : juce::ValueTree();
        const auto length = samples - offset;

        if (! track.isValid() || length <= 0)
        {
            t.file.deleteFile();
            continue;
        }

        if (! added)
            project->undo.beginNewTransaction ("Record");
        added = true;

        pool.forget (t.file);
        project->addRegion (track, t.file.getRelativePathFrom (project->folder).replaceCharacter ('\\', '/'),
                            start, offset, length, track[ids::name].toString());
    }

    takes.clear();
    sendChangeMessage();
}

//==============================================================================
juce::AudioProcessor* Engine::getRig (int trackId) const
{
    if (auto it = runtimes.find (trackId); it != runtimes.end())
        return it->second->rig.get();
    return nullptr;
}

void Engine::syncRigStates()
{
    if (project == nullptr)
        return;

    for (auto& [id, rt] : runtimes)
        if (auto track = project->trackById (id); track.isValid() && rt->rig != nullptr)
            storeRig (*rt->rig, track);
}

void Engine::removeTrack (juce::ValueTree track)
{
    if (project == nullptr || ! track.isValid())
        return;

    if (auto* rig = getRig ((int) track[ids::id]))
        storeRig (*rig, track);

    project->undo.beginNewTransaction ("Delete Track");
    project->removeTrack (track);
}

Engine::Levels Engine::takeTrackLevels (int trackId)
{
    if (auto it = runtimes.find (trackId); it != runtimes.end())
        return { it->second->peakLeft.exchange (0.0f), it->second->peakRight.exchange (0.0f) };
    return {};
}

Engine::Levels Engine::takeMasterLevels()
{
    return { masterPeakLeft.exchange (0.0f), masterPeakRight.exchange (0.0f) };
}

//==============================================================================
void Engine::prepareRig (TrackRuntime& rt)
{
    rt.work.setSize (2, blockSize, false, true, true);
    rt.gainLeft.reset (sampleRate, 0.03);
    rt.gainRight.reset (sampleRate, 0.03);

    if (rt.rig != nullptr)
    {
        rt.rig->setRateAndBufferSizeDetails (sampleRate, blockSize);
        rt.rig->prepareToPlay (sampleRate, blockSize);
    }
}

void Engine::syncRuntimes()
{
    std::map<int, std::shared_ptr<TrackRuntime>> next;

    if (project != nullptr)
    {
        for (auto t : project->state)
        {
            if (! t.hasType (ids::TRACK))
                continue;

            const int id = (int) t[ids::id];
            if (auto it = runtimes.find (id); it != runtimes.end())
            {
                next[id] = it->second;
                continue;
            }

            auto rt = std::make_shared<TrackRuntime>();
            rt->id = id;
            if (makeRig)
            {
                rt->rig = makeRig();
                restoreRig (*rt->rig, t);
                pointRigAtChannelZero (*rt->rig);
            }
            prepareRig (*rt);
            next[id] = rt;
        }
    }

    // Removed tracks' runtimes live on in the current snapshot until the audio thread lets it go.
    runtimes = std::move (next);
}

std::unique_ptr<Snapshot> Engine::buildSnapshot (const Project& project, AudioPool& pool,
                                                 const std::map<int, std::shared_ptr<TrackRuntime>>& runtimes)
{
    auto snap = std::make_unique<Snapshot>();
    const auto& s = project.state;
    snap->grid = project.grid();
    snap->masterGain = dbToGain ((double) s.getProperty (ids::masterVolume, 0.0));
    snap->click = (bool) s.getProperty (ids::click, true);
    snap->loopOn = (bool) s.getProperty (ids::loopOn, false);
    snap->loopStart = (juce::int64) s.getProperty (ids::loopStart, 0);
    snap->loopEnd = (juce::int64) s.getProperty (ids::loopEnd, 0);

    bool anySolo = false;
    for (auto t : s)
        anySolo = anySolo || (t.hasType (ids::TRACK) && (bool) t[ids::solo]);

    for (auto t : s)
    {
        if (! t.hasType (ids::TRACK))
            continue;

        auto it = runtimes.find ((int) t[ids::id]);
        if (it == runtimes.end())
            continue;

        TrackPlayback tp;
        tp.runtime = it->second;
        tp.gain = dbToGain ((double) t.getProperty (ids::volume, 0.0));
        tp.pan = (float) (double) t.getProperty (ids::pan, 0.0);
        tp.audible = ! (bool) t[ids::mute] && (! anySolo || (bool) t[ids::solo]);
        tp.armed = (bool) t[ids::arm];
        tp.monitor = (bool) t[ids::monitor];
        tp.rigOn = (bool) t.getProperty (ids::rigOn, true);
        tp.input = (int) t.getProperty (ids::input, 1);

        for (auto r : t)
        {
            if (! r.hasType (ids::REGION))
                continue;

            RegionPlayback rp;
            rp.audio = pool.get (project.resolve (r[ids::file].toString()));
            if (rp.audio == nullptr)
                continue;

            rp.start = (juce::int64) r[ids::start];
            rp.offset = juce::jlimit ((juce::int64) 0, (juce::int64) rp.audio->getNumSamples(), (juce::int64) r[ids::offset]);
            rp.length = juce::jmin ((juce::int64) r[ids::length], (juce::int64) rp.audio->getNumSamples() - rp.offset);
            rp.gain = dbToGain ((double) r.getProperty (ids::gain, 0.0));
            if (rp.length > 0)
                tp.regions.push_back (std::move (rp));
        }

        snap->tracks.push_back (std::move (tp));
    }

    return snap;
}

void Engine::handleAsyncUpdate()
{
    cancelPendingUpdate();
    syncRuntimes();
    publish();
}

void Engine::publish()
{
    auto snap = project != nullptr ? buildSnapshot (*project, pool, runtimes) : std::make_unique<Snapshot>();
    // An unclaimed older snapshot was never seen by the audio thread, so it can go right away.
    delete pending.exchange (snap.release());
    freeRetired();
}

void Engine::freeRetired()
{
    const auto scope = retireFifo.read (retireFifo.getNumReady());
    for (int i = 0; i < scope.blockSize1; ++i) { delete retired[(size_t) (scope.startIndex1 + i)]; retired[(size_t) (scope.startIndex1 + i)] = nullptr; }
    for (int i = 0; i < scope.blockSize2; ++i) { delete retired[(size_t) (scope.startIndex2 + i)]; retired[(size_t) (scope.startIndex2 + i)] = nullptr; }
}

void Engine::timerCallback()
{
    freeRetired();

    // Close the takes once the audio thread has certainly stopped writing to them.
    const bool deviceRunning = deviceManager.getCurrentAudioDevice() != nullptr && deviceManager.getCurrentAudioDevice()->isPlaying();
    if (finishPending && (callbackCount.load() > stopRequestedAt + 1 || ! deviceRunning))
        finishRecording();
}

//==============================================================================
void Engine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    prepare (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
}

void Engine::prepare (double newSampleRate, int newBlockSize)
{
    sampleRate = newSampleRate;
    blockSize = juce::jmax (64, newBlockSize);
    mixBuffer.setSize (2, blockSize);

    // No callbacks run until this returns, so the rigs can be re-prepared for the new settings.
    for (auto& [id, rt] : runtimes)
        prepareRig (*rt);
    if (current != nullptr)
        for (auto& t : current->tracks)
            prepareRig (*t.runtime);
}

void Engine::audioDeviceStopped()
{
    playing = false;
}

void Engine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs,
                                               int numOutputs, int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;
    callbackCount.fetch_add (1);

    if (auto* next = pending.exchange (nullptr))
    {
        if (current != nullptr && retireFifo.getFreeSpace() > 0)
        {
            const auto scope = retireFifo.write (1);
            retired[(size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)] = current;
        }
        current = next;
    }

    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputs[ch], numSamples);

    if (current == nullptr || mixBuffer.getNumSamples() == 0)
        return;

    // Every runtime in this snapshot was prepared for `blockSize`; a stale snapshot from before a
    // device change may not be, so check before using it.
    for (auto& t : current->tracks)
        if (t.runtime->work.getNumSamples() < juce::jmin (numSamples, mixBuffer.getNumSamples()))
            return;

    if (const auto jump = requestedPosition.exchange (-1); jump != -1)
        position.store (jump);

    auto pos = position.load();
    const bool isPlaying = playing.load();
    const bool isRec = recording.load();
    const auto& snap = *current;
    const bool looping = isPlaying && ! isRec && snap.loopOn && snap.loopEnd > snap.loopStart;

    std::array<const float*, 64> ins {};
    float masterL = 0.0f, masterR = 0.0f;

    for (int done = 0; done < numSamples;)
    {
        int n = juce::jmin (numSamples - done, mixBuffer.getNumSamples());
        if (looping && pos < snap.loopEnd && pos + n > snap.loopEnd)
            n = (int) (snap.loopEnd - pos);

        const int numIns = juce::jmin (numInputs, (int) ins.size());
        for (int ch = 0; ch < numIns; ++ch)
            ins[(size_t) ch] = inputs[ch] != nullptr ? inputs[ch] + done : nullptr;

        Mixer::Block b;
        b.position = pos;
        b.numSamples = n;
        b.playing = isPlaying;
        b.inputs = ins.data();
        b.numInputs = numIns;
        b.left = mixBuffer.getWritePointer (0);
        b.right = mixBuffer.getWritePointer (1);
        Mixer::render (snap, b);

        // Recording: the dry input of every armed track, from the record start onwards.
        if (isRec && isPlaying)
        {
            const auto from = (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, recordStart.load() - pos);
            if (from < n)
            {
                for (auto& t : snap.tracks)
                {
                    auto* w = t.runtime->recorder.load();
                    if (w == nullptr || ! juce::isPositiveAndBelow (t.input, numIns) || ins[(size_t) t.input] == nullptr)
                        continue;
                    const float* channel = ins[(size_t) t.input] + from;
                    w->write (&channel, n - from);
                }
                recordedSamples.fetch_add (n - from);
            }
        }

        if (isPlaying && (snap.click || (isRec && pos < recordStart.load())))
            Mixer::addClick (snap, b, 0.3f);

        for (int i = 0; i < n; ++i)
        {
            b.left[i] *= snap.masterGain;
            b.right[i] *= snap.masterGain;
            masterL = juce::jmax (masterL, std::abs (b.left[i]));
            masterR = juce::jmax (masterR, std::abs (b.right[i]));
        }

        if (numOutputs >= 2)
        {
            if (outputs[0] != nullptr) juce::FloatVectorOperations::copy (outputs[0] + done, b.left, n);
            if (outputs[1] != nullptr) juce::FloatVectorOperations::copy (outputs[1] + done, b.right, n);
        }
        else if (numOutputs == 1 && outputs[0] != nullptr)
        {
            juce::FloatVectorOperations::copyWithMultiply (outputs[0] + done, b.left, 0.5f, n);
            juce::FloatVectorOperations::addWithMultiply (outputs[0] + done, b.right, 0.5f, n);
        }

        if (isPlaying)
        {
            pos += n;
            if (looping && pos >= snap.loopEnd)
                pos = snap.loopStart;
        }

        done += n;
    }

    // A jump requested mid-callback wins over our advance.
    if (requestedPosition.load() == -1)
        position.store (pos);

    masterPeakLeft.store (juce::jmax (masterPeakLeft.load (std::memory_order_relaxed), masterL), std::memory_order_relaxed);
    masterPeakRight.store (juce::jmax (masterPeakRight.load (std::memory_order_relaxed), masterR), std::memory_order_relaxed);
}

//==============================================================================
namespace
{
class BounceThread final : public juce::Thread
{
public:
    BounceThread (std::unique_ptr<Snapshot> s, std::unique_ptr<juce::AudioFormatWriter> w,
                  juce::int64 from, juce::int64 to, std::atomic<float>& p, std::function<void (bool)> finished)
        : juce::Thread ("Woodshed bounce"), snap (std::move (s)), writer (std::move (w)),
          start (from), end (to), progress (p), onFinished (std::move (finished)) {}

    void run() override
    {
        juce::AudioBuffer<float> out (2, bounceBlock);
        const auto tail = (juce::int64) (snap->grid.sampleRate * bounceTailSeconds);
        const auto total = juce::jmax ((juce::int64) 1, end + tail - start);
        bool ok = true;

        for (auto pos = start; pos < end + tail && ! threadShouldExit(); pos += bounceBlock)
        {
            const int n = (int) juce::jmin ((juce::int64) bounceBlock, end + tail - pos);
            Mixer::Block b;
            b.position = pos;
            b.numSamples = n;
            b.playing = true;
            b.left = out.getWritePointer (0);
            b.right = out.getWritePointer (1);
            Mixer::render (*snap, b);
            out.applyGain (0, n, snap->masterGain);
            ok = ok && writer->writeFromAudioSampleBuffer (out, 0, n);
            progress = (float) (pos - start) / (float) total;
        }

        writer.reset();
        progress = 1.0f;
        ok = ok && ! threadShouldExit();
        auto done = onFinished;
        juce::MessageManager::callAsync ([done, ok] { done (ok); });
    }

    std::unique_ptr<Snapshot> snap;    // owns the bounce's private rigs through its runtimes

private:
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::int64 start, end;
    std::atomic<float>& progress;
    std::function<void (bool)> onFinished;
};
}

void Engine::startBounce (const juce::File& wav, juce::int64 start, juce::int64 end, std::function<void (bool)> done)
{
    if (project == nullptr || bounceThread != nullptr || end <= start)
    {
        if (done) done (false);
        return;
    }

    syncRigStates();

    // Private copies of every rig, so the bounce doesn't fight the live tracks for them.
    std::map<int, std::shared_ptr<TrackRuntime>> fresh;
    for (auto t : project->state)
    {
        if (! t.hasType (ids::TRACK))
            continue;

        auto rt = std::make_shared<TrackRuntime>();
        rt->id = (int) t[ids::id];
        rt->work.setSize (2, bounceBlock);
        rt->gainLeft.reset (sampleRate, 0.03);
        rt->gainRight.reset (sampleRate, 0.03);

        if (makeRig)
        {
            rt->rig = makeRig();
            restoreRig (*rt->rig, t);
            pointRigAtChannelZero (*rt->rig);
            rt->rig->setRateAndBufferSizeDetails (sampleRate, bounceBlock);
            rt->rig->prepareToPlay (sampleRate, bounceBlock);

            // One silent block installs the restored board; settling makes its knobs live at once.
            juce::AudioBuffer<float> silence (2, bounceBlock);
            silence.clear();
            juce::MidiBuffer midi;
            rt->rig->processBlock (silence, midi);
            if (settleRig)
                settleRig (*rt->rig);
            rt->rig->reset();
        }

        fresh[rt->id] = rt;
    }

    auto snap = buildSnapshot (*project, pool, fresh);
    for (auto& t : snap->tracks)
    {
        t.armed = t.monitor = false;      // a bounce plays the song, never the live input
        t.runtime->gainLeft.setCurrentAndTargetValue (t.audible ? t.gain : 0.0f);
        t.runtime->gainRight.setCurrentAndTargetValue (t.audible ? t.gain : 0.0f);
    }

    wav.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (wav.createOutputStream());
    juce::WavAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions{}.withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (24);
    std::unique_ptr<juce::AudioFormatWriter> writer;
    if (stream != nullptr)
        writer = format.createWriterFor (stream, options);

    if (writer == nullptr)
    {
        if (done) done (false);
        return;
    }

    bounceProgress = 0.0f;
    bounceThread = std::make_unique<BounceThread> (std::move (snap), std::move (writer), start, end, bounceProgress,
                                                   [this, done] (bool ok)
                                                   {
                                                       if (bounceThread != nullptr)
                                                           bounceThread->stopThread (2000);
                                                       bounceThread.reset();   // frees the private rigs here, on the message thread
                                                       if (done) done (ok);
                                                   });
    bounceThread->startThread();
}

} // namespace woodshed::daw
