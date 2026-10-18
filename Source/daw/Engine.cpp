#include "Engine.h"
#include "Stacks.h"

namespace woodshed::daw
{

namespace
{
constexpr int edgeFadeSamples = 96;            // de-click every region start and end (2 ms at 48k)
constexpr double bounceTailSeconds = 4.0;      // let delays and reverbs ring out past the last note
constexpr int bounceBlock = 512;
constexpr int punchCrossfade = 480;            // Autopunch's crossfades at each locator (10 ms at 48k)

float dbToGain (double db)                     { return db <= -96.0 ? 0.0f : juce::Decibels::decibelsToGain ((float) db); }

/** Equal-power: a fade-out over a fade-in of the same length keeps the level steady. */
float fadeCurve (float x)                      { return std::sin (juce::jlimit (0.0f, 1.0f, x) * juce::MathConstants<float>::halfPi); }

/** A region's fades as played: never shorter than the de-click edge, and the two together never
    longer than the region (a fade-in that would overlap the fade-out gives way to it). */
void clampFades (juce::int64 length, juce::int64 wantIn, juce::int64 wantOut, juce::int64& in, juce::int64& out)
{
    const auto edge = juce::jmin ((juce::int64) edgeFadeSamples, length / 2);
    out = juce::jlimit (edge, juce::jmax (edge, length - edge), wantOut);
    in = juce::jlimit (edge, juce::jmax (edge, length - out), wantIn);
}

/** Autopunch: the new take replaces what was on its track inside its range. Each older region
    keeps its audio outside, cut so it fades out over the take's fade-in and back in over its
    fade-out (equal-power crossfades of `crossfade` samples); a region wholly inside goes. */
void punchInTake (Project& p, juce::ValueTree take, int crossfade)
{
    const auto from = (juce::int64) take[ids::start];
    const auto to = from + (juce::int64) take[ids::length];
    const auto fade = (juce::int64) juce::jmin ((juce::int64) crossfade, (to - from) / 2);
    take.setProperty (ids::fadeIn, fade, &p.undo);
    take.setProperty (ids::fadeOut, fade, &p.undo);
    const auto leftEnd = from + fade, rightStart = to - fade;

    auto track = take.getParent();
    juce::Array<juce::ValueTree> older;
    for (auto r : track)
        if (r.hasType (ids::REGION) && r != take && ! Project::isMidiRegion (r))
            older.add (r);

    for (auto r : older)
    {
        const auto start = (juce::int64) r[ids::start];
        const auto end = start + (juce::int64) r[ids::length];
        if (end <= from || start >= to)
            continue;

        // The part after the range.
        if (start < rightStart && end > rightStart)
            if (auto right = p.splitRegion (r, rightStart); right.isValid())
                right.setProperty (ids::fadeIn, fade, &p.undo);

        // The part before it, and what's left in between.
        if (start < leftEnd)
        {
            if (auto middle = p.splitRegion (r, leftEnd); middle.isValid())
                track.removeChild (middle, &p.undo);
            r.setProperty (ids::fadeOut, fade, &p.undo);
        }
        else if (start < rightStart)
            track.removeChild (r, &p.undo);
    }
}

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

AudioPool::Audio AudioPool::getStereo (const juce::File& file)
{
    const auto key = "stereo:" + file.getFullPathName();
    if (auto it = cache.find (key); it != cache.end())
        return it->second;

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max())
        return nullptr;

    const int length = (int) reader->lengthInSamples;
    auto stereo = std::make_shared<juce::AudioBuffer<float>> (2, length);
    reader->read (stereo.get(), 0, length, 0, true, true);      // a mono file fills both channels

    Audio audio = std::move (stereo);
    cache[key] = audio;
    return audio;
}

void AudioPool::forget (const juce::File& file)
{
    cache.erase (file.getFullPathName());
    cache.erase ("stereo:" + file.getFullPathName());
}

//==============================================================================
void DelayRing::push (const float* left, const float* right, int n) noexcept
{
    if (! isAllocated())
        return;
    auto* l = data.getWritePointer (0);
    auto* r = data.getWritePointer (1);
    constexpr int mask = capacity - 1;
    for (int i = 0; i < n; ++i)
    {
        const int at = (write + i) & mask;
        l[at] = left[i];
        r[at] = right[i];
    }
    write = (write + n) & mask;
}

void DelayRing::addDelayed (int delay, float* left, float* right, int n, float gain) const noexcept
{
    if (! isAllocated())
        return;
    delay = juce::jlimit (0, capacity - n, delay);
    const auto* l = data.getReadPointer (0);
    const auto* r = data.getReadPointer (1);
    constexpr int mask = capacity - 1;
    const int from = write - n - delay;
    for (int i = 0; i < n; ++i)
    {
        const int at = (from + i) & mask;
        left[i] += l[at] * gain;
        right[i] += r[at] * gain;
    }
}

//==============================================================================
namespace
{
/** The instrument's events for this block: an all-notes-off after a jump or stop, the regions'
    notes, and the live ones merged in, all in time order. */
void collectNoteEvents (const TrackPlayback& track, TrackRuntime& rt, const Mixer::Block& b)
{
    auto& ev = rt.events;
    ev.clear();
    auto push = [&ev] (const NoteEvent& e) { if (ev.size() < ev.capacity()) ev.push_back (e); };

    const int n = b.numSamples;
    if (b.playing)
    {
        if (rt.notesSounding && rt.expectedPosition != b.position)
        {
            push ({ 0, NoteEvent::allNotesOff, 0, 0.0f });
            rt.notesSounding = false;
        }

        auto it = std::lower_bound (track.midi.begin(), track.midi.end(), b.position,
                                    [] (const MidiPlayback& m, juce::int64 t) { return m.time < t; });
        for (; it != track.midi.end() && it->time < b.position + n; ++it)
        {
            push ({ (int) (it->time - b.position), it->on ? NoteEvent::noteOn : NoteEvent::noteOff, it->pitch, it->velocity });
            rt.notesSounding = rt.notesSounding || it->on;
        }
        rt.expectedPosition = b.position + n;
    }
    else
    {
        if (rt.notesSounding)
            push ({ 0, NoteEvent::allNotesOff, 0, 0.0f });
        rt.notesSounding = false;
        rt.expectedPosition = -1;
    }

    if (b.liveTrackId == rt.id && b.numLiveEvents > 0)
    {
        // Both lists are in time order; merge the live ones in.
        const auto regionCount = ev.size();
        for (int i = 0; i < b.numLiveEvents; ++i)
            push (b.liveEvents[i]);
        std::inplace_merge (ev.begin(), ev.begin() + (std::ptrdiff_t) regionCount, ev.end(),
                            [] (const NoteEvent& x, const NoteEvent& y) { return x.offset < y.offset; });
    }
}

void runInserts (const TrackPlayback& track, float* left, float* right, int n, juce::int64 position)
{
    for (size_t i = 0; i < track.inserts.size(); ++i)
    {
        const auto& fx = track.inserts[i];
        if (! fx.on || fx.pedal == nullptr)
            continue;

        auto& pedal = *fx.pedal;
        for (int k = 0; k < Pedal::maxKnobs; ++k)
            pedal.setKnob (k, fx.knobs[(size_t) k]);
        for (auto& line : track.automation)
            if (line.target == TrackPlayback::AutoLine::insertKnob && line.insert == (int) i && line.knob < Pedal::maxKnobs)
                pedal.setKnob (line.knob, Project::evaluate (line.points, position));
        pedal.update();

        if (pedal.producesStereo())
            pedal.processStereo (left, right, n);
        else
        {
            pedal.processChannel (0, left, n);
            pedal.processChannel (1, right, n);
        }
    }
}
}

void Mixer::render (const Snapshot& snap, const Block& b)
{
    const int n = b.numSamples;
    juce::FloatVectorOperations::clear (b.left, n);
    juce::FloatVectorOperations::clear (b.right, n);

    for (auto& track : snap.tracks)
        if (track.aux && track.runtime->busInput.getNumSamples() >= n)
            track.runtime->busInput.clear (0, n);

    for (auto& track : snap.tracks)
    {
        auto& rt = *track.runtime;
        jassert (rt.work.getNumSamples() >= n);
        float* a = rt.work.getWritePointer (0);
        float* c = rt.work.getWritePointer (1);
        juce::FloatVectorOperations::clear (a, n);

        if (track.frozen != nullptr)
        {
            // Frozen: its render plays instead (pre-fader, from sample 0); none of its devices run.
            juce::FloatVectorOperations::clear (c, n);
            const auto& f = *track.frozen;
            const auto from = juce::jlimit ((juce::int64) 0, (juce::int64) n, -b.position);
            const auto to = juce::jlimit ((juce::int64) 0, (juce::int64) n, (juce::int64) f.getNumSamples() - b.position);
            if (b.playing && to > from)
            {
                juce::FloatVectorOperations::copy (a + from, f.getReadPointer (0) + b.position + from, (int) (to - from));
                juce::FloatVectorOperations::copy (c + from, f.getReadPointer (juce::jmin (1, f.getNumChannels() - 1)) + b.position + from, (int) (to - from));
            }
        }
        else if (track.aux)
        {
            // What the sends brought in this block.
            if (rt.busInput.getNumSamples() >= n)
            {
                juce::FloatVectorOperations::copy (a, rt.busInput.getReadPointer (0), n);
                juce::FloatVectorOperations::copy (c, rt.busInput.getReadPointer (1), n);
            }
            else
                juce::FloatVectorOperations::clear (c, n);
        }
        else if (track.instrument != nullptr)
        {
            juce::FloatVectorOperations::clear (c, n);
            collectNoteEvents (track, rt, b);

            auto& instrument = *track.instrument;
            for (int k = 0; k < Instrument::maxKnobs; ++k)
                instrument.setKnob (k, track.instrumentKnobs[(size_t) k]);
            for (auto& line : track.automation)
                if (line.target == TrackPlayback::AutoLine::instrumentKnob && line.knob < Instrument::maxKnobs)
                    instrument.setKnob (line.knob, Project::evaluate (line.points, b.position));
            instrument.update();
            instrument.render (a, c, n, rt.events.data(), (int) rt.events.size());
        }
        else
        {
            // Input monitoring (IN) always adds the live input. Armed tracks also go live while
            // stopped or recording. Takes play whenever the song plays, except on a track that is
            // recording a new one.
            const bool hearInput = b.monitorInputs && (track.monitor || (track.armed && (! b.playing || b.recording))) && b.inputs != nullptr;
            const bool playTakes = b.playing && ! (track.armed && b.recording);

            if (hearInput && juce::isPositiveAndBelow (track.input, b.numInputs) && b.inputs[track.input] != nullptr)
                juce::FloatVectorOperations::copy (a, b.inputs[track.input], n);

            if (playTakes)
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
                        if (intoRegion < r.fadeIn) g *= fadeCurve ((float) intoRegion / (float) r.fadeIn);
                        if (fromEnd < r.fadeOut)   g *= fadeCurve ((float) fromEnd / (float) r.fadeOut);
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
        }

        if (track.frozen == nullptr)
            runInserts (track, a, c, n, b.position);

        // Volume and pan follow their automation lines, if they have them.
        float gain = track.gain, pan = track.pan;
        for (auto& line : track.automation)
        {
            if (line.target == TrackPlayback::AutoLine::volume) gain = dbToGain ((double) Project::evaluate (line.points, b.position));
            else if (line.target == TrackPlayback::AutoLine::pan) pan = Project::evaluate (line.points, b.position);
        }
        // VCAs and folder stack mains scale the fader.
        for (auto& scale : track.scalers)
            gain *= scale.points.empty() ? scale.gain : dbToGain ((double) Project::evaluate (scale.points, b.position));

        // Constant-power pan, unity in the middle.
        const float angle = (juce::jlimit (-1.0f, 1.0f, pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        const float g = track.audible ? gain : 0.0f;
        rt.gainLeft.setTargetValue (g * std::cos (angle) * juce::MathConstants<float>::sqrt2);
        rt.gainRight.setTargetValue (g * std::sin (angle) * juce::MathConstants<float>::sqrt2);

        // Sends: their levels, automated or not, and the pre-fader ones before the fader.
        std::array<float, 32> sendGains {};
        const size_t numSends = juce::jmin (track.sends.size(), sendGains.size());
        for (size_t i = 0; i < numSends; ++i)
            sendGains[i] = track.audible ? track.sends[i].gain : 0.0f;
        for (auto& line : track.automation)
            if (line.target == TrackPlayback::AutoLine::sendLevel && juce::isPositiveAndBelow (line.knob, (int) numSends))
                sendGains[(size_t) line.knob] = track.audible ? dbToGain ((double) Project::evaluate (line.points, b.position)) : 0.0f;

        // Delay compensation: a send or output that must wait reads the signal back from the
        // track's ring (pre- or post-fader) instead of taking it as it is.
        auto delayed = [] (int delay, const DelayRing& ring) { return delay > 0 && ring.isAllocated(); };
        bool ringPre = false, ringPost = delayed (track.outputDelay, rt.postFader) && track.toMaster;
        for (size_t i = 0; i < numSends; ++i)
            (track.sends[i].pre ? ringPre : ringPost) |= delayed (track.sends[i].delay, track.sends[i].pre ? rt.preFader : rt.postFader);

        auto sendTo = [&] (size_t i)
        {
            const auto& send = track.sends[i];
            if (sendGains[i] <= 0.0f || ! juce::isPositiveAndBelow (send.target, (int) snap.tracks.size()))
                return;
            auto& bus = snap.tracks[(size_t) send.target].runtime->busInput;
            if (bus.getNumSamples() < n)
                return;
            const auto& ring = send.pre ? rt.preFader : rt.postFader;
            if (delayed (send.delay, ring))
                ring.addDelayed (send.delay, bus.getWritePointer (0), bus.getWritePointer (1), n, sendGains[i]);
            else
            {
                juce::FloatVectorOperations::addWithMultiply (bus.getWritePointer (0), a, sendGains[i], n);
                juce::FloatVectorOperations::addWithMultiply (bus.getWritePointer (1), c, sendGains[i], n);
            }
        };
        if (ringPre)
            rt.preFader.push (a, c, n);
        for (size_t i = 0; i < numSends; ++i)
            if (track.sends[i].pre)
                sendTo (i);

        // A summing stack's member reaches the Stereo Out only through its main (a send).
        const bool direct = track.toMaster && ! (track.outputDelay > 0 && rt.postFader.isAllocated());
        float peakL = 0.0f, peakR = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float l = a[i] * rt.gainLeft.getNextValue();
            const float r = c[i] * rt.gainRight.getNextValue();
            a[i] = l;
            c[i] = r;
            if (direct)
            {
                b.left[i] += l;
                b.right[i] += r;
            }
            peakL = juce::jmax (peakL, std::abs (l));
            peakR = juce::jmax (peakR, std::abs (r));
        }

        if (ringPost)
            rt.postFader.push (a, c, n);
        if (track.toMaster && ! direct)
            rt.postFader.addDelayed (track.outputDelay, b.left, b.right, n, 1.0f);
        for (size_t i = 0; i < numSends; ++i)
            if (! track.sends[i].pre)
                sendTo (i);

        rt.peakLeft.store (juce::jmax (rt.peakLeft.load (std::memory_order_relaxed), peakL), std::memory_order_relaxed);
        rt.peakRight.store (juce::jmax (rt.peakRight.load (std::memory_order_relaxed), peakR), std::memory_order_relaxed);
    }

    // The Stereo Out's effects, on the whole mix.
    for (auto& fx : snap.masterInserts)
    {
        if (! fx.on || fx.pedal == nullptr)
            continue;
        for (int k = 0; k < Pedal::maxKnobs; ++k)
            fx.pedal->setKnob (k, fx.knobs[(size_t) k]);
        fx.pedal->update();
        if (fx.pedal->producesStereo())
            fx.pedal->processStereo (b.left, b.right, n);
        else
        {
            fx.pedal->processChannel (0, b.left, n);
            fx.pedal->processChannel (1, b.right, n);
        }
    }
}

void Mixer::addClick (const Snapshot& snap, const Block& b, float level)
{
    // A short pitched blip on every beat of the meter in force, higher on the downbeat. It
    // follows the tempo map; stateless, so it works for negative positions (the count-in) and
    // right after a jump.
    const auto& grid = snap.grid;
    const double sr = grid.sampleRate;
    const double clickLength = sr * 0.03;
    const double blockEnd = (double) (b.position + b.numSamples);

    // Start from the beat that may still be ringing at the block's start.
    const double firstTick = grid.tickAt ((double) b.position - clickLength);
    auto meter = grid.meterAt (firstTick);
    double beatTick = meter.tick + std::floor ((firstTick - meter.tick) / meter.ticksPerBeat()) * meter.ticksPerBeat();

    for (int guard = 0; guard < 256; ++guard)
    {
        meter = grid.meterAt (beatTick);
        const double beatSample = grid.sampleAt (beatTick);
        if (beatSample >= blockEnd)
            break;

        const auto beatInMeter = (juce::int64) std::llround ((beatTick - meter.tick) / meter.ticksPerBeat());
        const bool downbeat = (beatInMeter % meter.beatsPerBar + meter.beatsPerBar) % meter.beatsPerBar == 0;
        const double hz = downbeat ? 1660.0 : 1100.0;

        const auto from = juce::jmax ((juce::int64) b.position, (juce::int64) std::ceil (beatSample));
        const auto to = juce::jmin ((juce::int64) blockEnd, (juce::int64) std::ceil (beatSample + clickLength));
        for (auto t = from; t < to; ++t)
        {
            const double since = (double) t - beatSample;
            const float v = level * (float) (std::sin (juce::MathConstants<double>::twoPi * hz * since / sr) * std::exp (-since / (sr * 0.007)));
            b.left[t - b.position] += v;
            b.right[t - b.position] += v;
        }
        beatTick += meter.ticksPerBeat();
    }
}

//==============================================================================
Engine::Engine (juce::AudioDeviceManager& dm, RigFactory factory, InstrumentFactory instruments)
    : deviceManager (dm), makeRig (std::move (factory)), makeInstrument (std::move (instruments))
{
    liveMidi.reset (sampleRate);
    liveBuffer.ensureSize (4096);
    liveEvents.reserve (1024);
    recordedNotes.reserve (4096);
    writerThread.startThread();
    deviceManager.addAudioCallback (this);
    deviceManager.addMidiInputDeviceCallback ({}, &liveMidi);
    startTimerHz (30);
}

Engine::~Engine()
{
    stopTimer();
    if (bounceThread != nullptr)
        bounceThread->stopThread (5000);

    deviceManager.removeMidiInputDeviceCallback ({}, &liveMidi);
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
    masterRuntime.reset();
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
            {
                it->second->recorder = nullptr;
                it->second->livePeaks = nullptr;
            }

        midiRecordTrack = -1;
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
    recordedNotes.clear();
    midiRecordFifo.reset();

    // MIDI records into the track it plays: the MIDI target, if it's an armed instrument track,
    // else the first armed instrument track (which then becomes the target).
    {
        auto target = project->trackById (midiTarget.load());
        if (! (target.isValid() && Project::isInstrumentTrack (target) && (bool) target[ids::arm]))
            for (auto t : project->state)
                if (t.hasType (ids::TRACK) && Project::isInstrumentTrack (t) && (bool) t[ids::arm])
                {
                    midiTarget = (int) t[ids::id];
                    break;
                }
    }

    for (auto t : project->state)
    {
        if (! t.hasType (ids::TRACK) || ! (bool) t[ids::arm])
            continue;

        const int id = (int) t[ids::id];
        auto it = runtimes.find (id);
        if (it == runtimes.end())
            continue;

        if (Project::isInstrumentTrack (t))
        {
            if (id == midiTarget.load())
            {
                Take take;
                take.trackId = id;
                take.midi = true;
                takes.push_back (std::move (take));
            }
            continue;
        }

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
        take.peaks = std::make_unique<LivePeaks> (sampleRate);
        it->second->recorder = take.writer.get();
        it->second->livePeaks = take.peaks.get();
        takes.push_back (std::move (take));
    }

    if (takes.empty())
        return;

    // What you hear comes late by the interface and by delay compensation; takes move back by both.
    latencyAtRecord = getRecordLatencySamples() + playbackLatency.load();
    midiLatencyAtRecord = playbackLatency.load();
    if (auto* device = deviceManager.getCurrentAudioDevice())
        midiLatencyAtRecord += device->getOutputLatencyInSamples();
    for (auto& t : takes)
        if (t.midi)
            midiRecordTrack = t.trackId;
    recordedSamples = 0;

    const auto& grid = project->grid();
    auto from = getPosition();

    // Autopunch (below) wins over the cycle: it records its range once.
    const auto punchIn = (juce::int64) project->state.getProperty (ids::punchIn, 0);
    const auto punchOut = (juce::int64) project->state.getProperty (ids::punchOut, 0);
    punching = (bool) project->state[ids::punchOn] && punchOut > punchIn;
    punchDone = false;

    // Cycle recording: with the cycle on, recording loops it, a take per pass. It starts at the
    // cycle's start if the playhead is outside the cycle.
    const auto loopStart = (juce::int64) project->state.getProperty (ids::loopStart, 0);
    const auto loopEnd = (juce::int64) project->state.getProperty (ids::loopEnd, 0);
    cycleRecording = (bool) project->state[ids::loopOn] && loopEnd > loopStart && ! punching.load();
    if (cycleRecording.load())
    {
        cycleStart = loopStart;
        cycleEnd = loopEnd;
        if (from < loopStart || from >= loopEnd)
        {
            from = loopStart;
            if (isPlaying())
                requestedPosition = from;
        }
    }

    // Autopunch: only the range between the punch locators records (a little either side, for the
    // crossfades); the song plays up to it as a pre-roll, from the playhead if that's before the
    // range, else from a bar before it.
    if (punching.load())
    {
        punchFrom = juce::jmax ((juce::int64) 0, punchIn - punchCrossfade / 2);
        punchTo = punchOut + punchCrossfade / 2;
        from = punchFrom.load();
        if (getPosition() >= from)
        {
            const double tick = grid.tickAt ((double) punchIn);
            const auto preroll = juce::jmax ((juce::int64) 0, (juce::int64) std::llround (grid.sampleAt (tick - grid.meterAt (tick).ticksPerBar())));
            requestedPosition = preroll;
            position = preroll;
        }
    }
    recordStart = from;

    // Recording from a stop gets a bar of count-in (when it's on); punching in while playing doesn't.
    if (! isPlaying() && ! punching.load() && (bool) project->state[ids::countIn])
    {
        const auto preroll = (juce::int64) std::llround (grid.sampleAt (grid.tickAt ((double) from) - grid.meterAt (grid.tickAt ((double) from)).ticksPerBar()));
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
    drainRecordedMidi();

    for (auto& t : takes)
    {
        if (t.midi)
        {
            auto track = project != nullptr ? project->trackById (t.trackId) : juce::ValueTree();
            if (! track.isValid() || recordedNotes.empty())
                continue;

            if (! added)
                project->undo.beginNewTransaction ("Record");
            added = true;
            finishMidiTake (t.trackId, recordStart.load() + samples);
            continue;
        }

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
        const auto relative = t.file.getRelativePathFrom (project->folder).replaceCharacter ('\\', '/');

        // Two or more passes around the cycle: a take folder, playing the last complete take.
        const auto passes = cycleRecording.load() ? cyclePasses (recordStart.load(), cycleStart, cycleEnd, samples, latencyAtRecord)
                                                  : std::vector<Pass>();
        if (passes.size() >= 2)
        {
            auto folder = project->addTakeFolder (track, cycleStart, cycleEnd - cycleStart, track[ids::name].toString());
            int lastComplete = 0;
            for (size_t i = 0; i < passes.size(); ++i)
            {
                project->addTake (folder, relative, passes[i].start - cycleStart, passes[i].fileOffset, passes[i].length,
                                  "Take " + juce::String ((int) i + 1));
                if (passes[i].complete)
                    lastComplete = (int) i;
            }
            project->setCompRange (folder, 0, cycleEnd - cycleStart, lastComplete);
            continue;
        }

        auto take = project->addRegion (track, relative, start, offset, length, track[ids::name].toString());
        if (punching.load())
            punchInTake (*project, take, punchCrossfade);
    }
    cycleRecording = false;
    punching = false;

    takes.clear();
    sendChangeMessage();
}

std::vector<Engine::Pass> Engine::cyclePasses (juce::int64 recordStart, juce::int64 loopStart, juce::int64 loopEnd,
                                               juce::int64 recorded, int latency)
{
    std::vector<Pass> passes;
    const auto cycle = loopEnd - loopStart;
    if (cycle <= 0 || recorded <= 0)
        return passes;

    juce::int64 file = 0;
    auto transport = recordStart;
    while (file < recorded)
    {
        const auto wanted = loopEnd - transport;
        const auto got = juce::jmin (wanted, recorded - file);
        Pass p;
        p.start = transport - latency;
        p.fileOffset = file;
        p.length = juce::jmin (got + latency, recorded - file);     // the file runs on into the next pass
        p.complete = got == wanted;
        if (p.start < 0) { p.fileOffset -= p.start; p.length += p.start; p.start = 0; }
        // A sliver of a last pass (under a quarter of the cycle) isn't a take.
        if (p.complete || got >= cycle / 4 || passes.empty())
            if (p.length > 0)
                passes.push_back (p);
        file += got;
        transport = loopStart;
    }
    return passes;
}

void Engine::finishMidiTake (int trackId, juce::int64 stopAt)
{
    auto track = project->trackById (trackId);
    const auto& grid = project->grid();

    // Notes still held when recording stopped end there; everything moves back by the output
    // latency, since you played along with what you heard.
    std::vector<RecordedNote> notes;
    juce::int64 last = stopAt;
    for (auto n : recordedNotes)
    {
        if (n.end < 0) n.end = stopAt;
        if (punching.load())
        {
            // Autopunch: only notes that start inside the range, cut off at its end.
            if (n.start < punchFrom.load() || n.start >= punchTo.load())
                continue;
            n.end = juce::jmin (n.end, punchTo.load());
        }
        n.start = juce::jmax ((juce::int64) 0, n.start - midiLatencyAtRecord);
        n.end = juce::jmax (n.start + 1, n.end - midiLatencyAtRecord);
        last = juce::jmax (last, n.end);
        notes.push_back (n);
    }

    // Like Logic, the region runs from the bar the recording started in to the end of the bar
    // it stopped in.
    const int firstBar = (int) std::floor (grid.barAt (grid.tickAt ((double) juce::jmax ((juce::int64) 0, recordStart.load()))) + 1.0e-9);
    const int lastBar = juce::jmax (firstBar + 1, (int) std::ceil (grid.barAt (grid.tickAt ((double) last)) - 1.0e-9));
    const auto from = grid.barStartSample (firstBar);
    const auto to = grid.barStartSample (lastBar);
    const double fromTick = grid.barStartTick (firstBar);

    auto region = project->addMidiRegion (track, from, to - from, track[ids::name].toString());
    for (auto& n : notes)
    {
        Note note;
        note.pitch = n.pitch;
        note.velocity = n.velocity;
        const double on = grid.tickAt ((double) n.start), off = grid.tickAt ((double) n.end);
        note.start = std::llround (on - fromTick);
        note.length = juce::jmax ((juce::int64) 1, (juce::int64) std::llround (off - on));
        project->addNote (region, note);
    }
}

void Engine::drainRecordedMidi()
{
    const auto scope = midiRecordFifo.read (midiRecordFifo.getNumReady());
    auto take = [this] (const MidiStamp& m)
    {
        if (m.on)
        {
            if (recordedNotes.size() < recordedNotes.capacity())
                recordedNotes.push_back ({ m.time, -1, m.pitch, m.velocity });
            return;
        }
        for (auto it = recordedNotes.rbegin(); it != recordedNotes.rend(); ++it)
            if (it->pitch == m.pitch && it->end < 0)
            {
                it->end = juce::jmax (it->start + 1, m.time);
                break;
            }
    };
    for (int i = 0; i < scope.blockSize1; ++i) take (midiRecordRing[(size_t) (scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i) take (midiRecordRing[(size_t) (scope.startIndex2 + i)]);
}

void Engine::sendLiveMidi (const juce::MidiMessage& message)
{
    auto m = message;
    m.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    liveMidi.addMessageToQueue (m);
}

void Engine::enableMidiInputs()
{
    for (auto& device : juce::MidiInput::getAvailableDevices())
        if (! deviceManager.isMidiInputDeviceEnabled (device.identifier))
            deviceManager.setMidiInputDeviceEnabled (device.identifier, true);
}

Instrument* Engine::getInstrument (int trackId) const
{
    if (auto it = runtimes.find (trackId); it != runtimes.end())
        return it->second->instrument.get();
    return nullptr;
}

const LivePeaks* Engine::getLivePeaks (int trackId) const
{
    if (! isRecording())
        return nullptr;
    for (auto& t : takes)
        if (t.trackId == trackId)
            return t.peaks.get();
    return nullptr;
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

    // Plug-ins keep their own settings; copy them into their nodes so they're saved.
    auto store = [] (juce::AudioPluginInstance& plugin, juce::ValueTree node)
    {
        juce::MemoryBlock data;
        plugin.getStateInformation (data);
        node.setProperty (ids::state, data.toBase64Encoding(), nullptr);
    };
    auto storeInserts = [&store] (const TrackRuntime& rt, const juce::ValueTree& owner)
    {
        for (auto& fx : rt.inserts)
            if (auto* p = dynamic_cast<PluginEffect*> (fx.pedal.get()))
                if (auto node = owner.getChildWithProperty (ids::id, fx.id); node.hasType (ids::FX))
                    store (p->getPlugin(), node);
    };
    for (auto& [id, rt] : runtimes)
    {
        auto track = project->trackById (id);
        if (! track.isValid())
            continue;
        if (auto* p = dynamic_cast<PluginInstrument*> (rt->instrument.get()))
            store (p->getPlugin(), track.getChildWithName (ids::INSTRUMENT));
        storeInserts (*rt, track);
    }
    if (masterRuntime != nullptr)
        storeInserts (*masterRuntime, project->state.getChildWithName (ids::MASTER));
}

std::shared_ptr<Instrument> Engine::shareInstrument (int trackId) const
{
    if (auto it = runtimes.find (trackId); it != runtimes.end())
        return it->second->instrument;
    return nullptr;
}

std::shared_ptr<Pedal> Engine::shareInsert (int trackId, int fxId) const
{
    const TrackRuntime* rt = nullptr;
    if (trackId < 0)
        rt = masterRuntime.get();
    else if (auto it = runtimes.find (trackId); it != runtimes.end())
        rt = it->second.get();
    if (rt != nullptr)
        for (auto& fx : rt->inserts)
            if (fx.id == fxId)
                return fx.pedal;
    return nullptr;
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
        return { it->second->shownLeft, it->second->shownRight };
    return {};
}

Engine::Levels Engine::takeMasterLevels()
{
    return masterShown;
}

//==============================================================================
void Engine::prepareRig (TrackRuntime& rt)
{
    rt.work.setSize (2, blockSize, false, true, true);
    rt.busInput.setSize (2, blockSize, false, true, true);
    if (! rt.preFader.isAllocated()) rt.preFader.allocate();
    if (! rt.postFader.isAllocated()) rt.postFader.allocate();
    rt.preFader.clear();
    rt.postFader.clear();
    rt.gainLeft.reset (sampleRate, 0.03);
    rt.gainRight.reset (sampleRate, 0.03);

    if (rt.rig != nullptr)
    {
        rt.rig->setRateAndBufferSizeDetails (sampleRate, blockSize);
        rt.rig->prepareToPlay (sampleRate, blockSize);
    }

    if (rt.instrument != nullptr)
        rt.instrument->prepare (sampleRate, blockSize);
    for (auto& fx : rt.inserts)
        fx.pedal->prepare (sampleRate, blockSize);
    rt.expectedPosition = -1;
    rt.notesSounding = false;
}

void Engine::syncTrackDevices (TrackRuntime& rt, const juce::ValueTree& track, double rate, int block)
{
    if (rt.events.capacity() < 4096)
        rt.events.reserve (4096);

    // The instrument: replaced when the track switches to another one. A snapshot still playing
    // the old one keeps it alive until the audio thread lets go.
    // A plug-in's identity is its description, so choosing another plug-in replaces it.
    auto deviceKey = [] (const juce::ValueTree& node)
    {
        const auto type = node[ids::type].toString();
        return type == "plugin" ? "plugin:" + juce::String::toHexString (node[ids::plugin].toString().hashCode64()) : type;
    };

    const auto instNode = track.getChildWithName (ids::INSTRUMENT);
    const auto type = Project::isInstrumentTrack (track) ? deviceKey (instNode) : juce::String();
    if (type != rt.instrumentType)
    {
        rt.instrumentType = type;
        rt.instrument.reset();
        if (type.startsWith ("plugin:"))
        {
            if (makePlugin)
                if (auto plugin = makePlugin (instNode[ids::plugin].toString(), rate, block, pluginError))
                {
                    restorePluginState (*plugin, instNode);
                    auto made = std::make_shared<PluginInstrument> (std::move (plugin));
                    made->prepare (rate, block);
                    rt.instrument = std::move (made);
                }
        }
        else if (type.isNotEmpty() && makeInstrument)
            if (auto made = makeInstrument (type))
            {
                made->prepare (rate, block);
                rt.instrument = std::move (made);
            }
    }

    // Inserts: keep each pedal (and its tails) while its FX node keeps its id and type.
    std::vector<InsertRuntime> next;
    for (auto fx : Project::inserts (track))
    {
        const int id = (int) fx[ids::id];
        const auto pedalType = deviceKey (fx);
        auto existing = std::find_if (rt.inserts.begin(), rt.inserts.end(),
                                      [&] (const InsertRuntime& r) { return r.id == id && r.type == pedalType; });
        if (existing != rt.inserts.end())
        {
            next.push_back (*existing);
            continue;
        }

        if (pedalType.startsWith ("plugin:"))
        {
            if (makePlugin)
                if (auto plugin = makePlugin (fx[ids::plugin].toString(), rate, block, pluginError))
                {
                    restorePluginState (*plugin, fx);
                    std::shared_ptr<Pedal> pedal = std::make_shared<PluginEffect> (std::move (plugin));
                    pedal->prepare (rate, block);
                    next.push_back ({ id, pedalType, std::move (pedal) });
                }
        }
        else if (auto* pt = Registry::get().find (pedalType))
        {
            std::shared_ptr<Pedal> pedal = pt->create();
            pedal->prepare (rate, block);
            next.push_back ({ id, pedalType, std::move (pedal) });
        }
    }
    rt.inserts = std::move (next);
}

void Engine::restorePluginState (juce::AudioPluginInstance& plugin, const juce::ValueTree& node)
{
    juce::MemoryBlock data;
    const auto encoded = node[ids::state].toString();
    if (encoded.isNotEmpty() && data.fromBase64Encoding (encoded))
        plugin.setStateInformation (data.getData(), (int) data.getSize());
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
                syncTrackDevices (*it->second, t, sampleRate, blockSize);
                next[id] = it->second;
                continue;
            }

            auto rt = std::make_shared<TrackRuntime>();
            rt->id = id;
            syncTrackDevices (*rt, t, sampleRate, blockSize);
            if (makeRig && ! Project::isInstrumentTrack (t) && ! Project::isAuxTrack (t))
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

    if (project != nullptr)
    {
        if (masterRuntime == nullptr)
        {
            masterRuntime = std::make_shared<TrackRuntime>();
            masterRuntime->id = -1;
        }
        syncTrackDevices (*masterRuntime, project->state.getChildWithName (ids::MASTER), sampleRate, blockSize);
    }
    else
        masterRuntime.reset();
}

namespace
{
std::vector<InsertPlayback> insertsFor (const juce::ValueTree& owner, const TrackRuntime& rt)
{
    std::vector<InsertPlayback> list;
    for (auto node : Project::inserts (owner))
        for (auto& fx : rt.inserts)
            if (fx.id == (int) node[ids::id])
            {
                InsertPlayback ip;
                ip.id = fx.id;
                ip.pedal = fx.pedal;
                ip.on = (bool) node.getProperty (ids::on, true);
                const auto values = Project::decodeValues (node[ids::knobs].toString());
                if (auto* pt = Registry::get().find (fx.type))
                    for (size_t k = 0; k < pt->info.knobs.size() && k < ip.knobs.size(); ++k)
                        ip.knobs[k] = k < values.size() ? values[k] : pt->info.knobs[k].def;
                list.push_back (std::move (ip));
            }
    return list;
}
}

std::unique_ptr<Snapshot> Engine::buildSnapshot (const Project& project, AudioPool& pool,
                                                 const std::map<int, std::shared_ptr<TrackRuntime>>& runtimes,
                                                 const TrackRuntime* master)
{
    auto snap = std::make_unique<Snapshot>();
    const auto& s = project.state;
    snap->grid = project.grid();
    snap->masterGain = dbToGain ((double) s.getProperty (ids::masterVolume, 0.0));
    snap->click = (bool) s.getProperty (ids::click, true);
    snap->loopOn = (bool) s.getProperty (ids::loopOn, false);
    snap->loopStart = (juce::int64) s.getProperty (ids::loopStart, 0);
    snap->loopEnd = (juce::int64) s.getProperty (ids::loopEnd, 0);

    // Solo and mute through stacks and VCAs: a stack main or VCA soloed (or muted) solos (mutes)
    // its tracks, and a summing main plays while anything inside it is soloed.
    auto controllers = [&project] (const juce::ValueTree& t)
    {
        juce::Array<juce::ValueTree> list;      // folder and summing mains above it, then its VCA
        int guard = 0;
        for (auto up = stacks::mainOf (project, t); up.isValid() && guard < 32; up = stacks::mainOf (project, up), ++guard)
            list.add (up);
        if (auto vca = project.trackById ((int) t.getProperty (ids::vca, -1)); stacks::isVca (vca))
            list.add (vca);
        return list;
    };
    auto soloed = [&] (const juce::ValueTree& t)
    {
        if ((bool) t[ids::solo])
            return true;
        for (auto& c : controllers (t))
            if ((bool) c[ids::solo])
                return true;
        if (stacks::isSumming (t))
            for (auto& m : stacks::descendants (project, t))
                if ((bool) m[ids::solo])
                    return true;
        return false;
    };
    auto muted = [&] (const juce::ValueTree& t)
    {
        if ((bool) t[ids::mute])
            return true;
        for (auto& c : controllers (t))
            if ((bool) c[ids::mute] && ! stacks::isSumming (c))     // a summing main's own mute silences it, and so them
                return true;
        return false;
    };

    bool anySolo = false;
    for (auto t : s)
        anySolo = anySolo || (t.hasType (ids::TRACK) && soloed (t));

    for (auto t : s)
    {
        if (! t.hasType (ids::TRACK))
            continue;
        // VCAs and folder stack mains have no audio: they only scale their tracks (below).
        if (stacks::isVca (t) || stacks::isFolder (t))
            continue;

        auto it = runtimes.find ((int) t[ids::id]);
        if (it == runtimes.end())
            continue;

        TrackPlayback tp;
        tp.runtime = it->second;
        tp.gain = dbToGain ((double) t.getProperty (ids::volume, 0.0));
        tp.pan = (float) (double) t.getProperty (ids::pan, 0.0);
        tp.audible = ! muted (t) && (! anySolo || soloed (t));
        for (auto& c : controllers (t))
            if (stacks::isFolder (c) || stacks::isVca (c))
                tp.scalers.push_back ({ dbToGain ((double) c.getProperty (ids::volume, 0.0)), Project::getAutomation (c, "volume") });
        if ((bool) t[ids::frozen] && t[ids::freezeFile].toString().isNotEmpty())
            tp.frozen = pool.getStereo (project.resolve (t[ids::freezeFile].toString()));
        tp.armed = (bool) t[ids::arm];
        tp.monitor = (bool) t[ids::monitor];
        tp.rigOn = (bool) t.getProperty (ids::rigOn, true);
        tp.input = (int) t.getProperty (ids::input, 0);
        tp.aux = Project::isAuxTrack (t);
        if (tp.aux)
            tp.rigOn = false;

        const auto& rt = *it->second;
        for (auto& fx : rt.inserts)
        {
            auto node = t.getChildWithProperty (ids::id, fx.id);
            if (! node.hasType (ids::FX))
                continue;
            InsertPlayback ip;
            ip.id = fx.id;
            ip.pedal = fx.pedal;
            ip.on = (bool) node.getProperty (ids::on, true);
            const auto values = Project::decodeValues (node[ids::knobs].toString());
            if (auto* pt = Registry::get().find (fx.type))
                for (size_t k = 0; k < pt->info.knobs.size() && k < ip.knobs.size(); ++k)
                    ip.knobs[k] = k < values.size() ? values[k] : pt->info.knobs[k].def;
            tp.inserts.push_back (std::move (ip));
        }
        // Inserts in the FX nodes' order.
        {
            const auto order = Project::inserts (t);
            std::vector<InsertPlayback> sorted;
            for (auto& node : order)
                for (size_t i = 0; i < rt.inserts.size(); ++i)
                    if (rt.inserts[i].id == (int) node[ids::id])
                        for (auto& ip : tp.inserts)
                            if (ip.pedal == rt.inserts[i].pedal)
                                sorted.push_back (ip);
            tp.inserts = std::move (sorted);
        }

        // How late its own chain makes it (a frozen track's render is already lined up).
        if (tp.frozen == nullptr)
        {
            for (auto& ip : tp.inserts)
                if (ip.on && ip.pedal != nullptr)
                    tp.latency += juce::jmax (0, ip.pedal->getLatencySamples());
            if (Project::isInstrumentTrack (t) && rt.instrument != nullptr)
                tp.latency += juce::jmax (0, rt.instrument->getLatencySamples());
            if (tp.rigOn && rt.rig != nullptr && ! Project::isInstrumentTrack (t))
                tp.latency += juce::jmax (0, rt.rig->getLatencySamples());
        }

        for (auto node : t)
        {
            if (! node.hasType (ids::AUTOMATION))
                continue;
            TrackPlayback::AutoLine line;
            line.points = Project::decodePoints (node[ids::points].toString());
            if (line.points.empty())
                continue;
            const auto param = node[ids::param].toString();
            if (param == "volume")            line.target = TrackPlayback::AutoLine::volume;
            else if (param == "pan")          line.target = TrackPlayback::AutoLine::pan;
            else if (param.startsWith ("inst:"))
            {
                line.target = TrackPlayback::AutoLine::instrumentKnob;
                line.knob = param.fromFirstOccurrenceOf (":", false, false).getIntValue();
            }
            else if (param.startsWith ("send:"))
            {
                line.target = TrackPlayback::AutoLine::sendLevel;
                line.knob = -1;
                const int busId = param.fromFirstOccurrenceOf (":", false, false).getIntValue();
                int index = 0;
                for (auto send : Project::sends (t))
                {
                    if ((int) send[ids::bus] == busId)
                        line.knob = index;
                    ++index;
                }
                if (line.knob < 0)
                    continue;
            }
            else if (param.startsWith ("fx:"))
            {
                const auto rest = param.fromFirstOccurrenceOf (":", false, false);
                const int fxId = rest.upToFirstOccurrenceOf (":", false, false).getIntValue();
                line.target = TrackPlayback::AutoLine::insertKnob;
                line.knob = rest.fromFirstOccurrenceOf (":", false, false).getIntValue();
                line.insert = -1;
                for (size_t i = 0; i < tp.inserts.size(); ++i)
                    if (tp.inserts[i].id == fxId)
                        line.insert = (int) i;
                if (line.insert < 0)
                    continue;
            }
            else
                continue;
            tp.automation.push_back (std::move (line));
        }

        if (Project::isInstrumentTrack (t))
        {
            tp.instrument = rt.instrument;
            // Zones, samples and anything else an instrument keeps beyond its knobs.
            if (tp.instrument != nullptr)
                tp.instrument->syncState (t.getChildWithName (ids::INSTRUMENT), project.folder);
            // Stored values, with the instrument's defaults for any knob the project doesn't have
            // (a project saved before the instrument grew that knob).
            const auto inst = t.getChildWithName (ids::INSTRUMENT);
            const auto values = Project::decodeValues (inst[ids::knobs].toString());
            if (auto* type = InstrumentRegistry::get().find (inst[ids::type].toString()))
                for (size_t k = 0; k < type->info.knobs.size() && k < tp.instrumentKnobs.size(); ++k)
                    tp.instrumentKnobs[k] = type->info.knobs[k].def;
            for (size_t k = 0; k < values.size() && k < tp.instrumentKnobs.size(); ++k)
                tp.instrumentKnobs[k] = values[k];

            const auto& grid = project.grid();
            for (auto r : t)
            {
                if (! r.hasType (ids::REGION) || ! Project::isMidiRegion (r) || (bool) r[ids::mute])
                    continue;

                const auto regionStart = (juce::int64) r[ids::start];
                const auto length = (juce::int64) r[ids::length];
                const auto loopsTo = project.loopEnd (r);
                const int transpose = (int) r.getProperty (ids::transpose, 0);
                const int velocityShift = (int) r.getProperty (ids::velocity, 0);

                // The region, then (looped) its repeats up to the next region.
                for (juce::int64 shift = 0; regionStart + shift < loopsTo; shift += juce::jmax ((juce::int64) 1, length))
                {
                    const auto start = regionStart + shift;
                    const auto end = juce::jmin (start + length, loopsTo);
                    const auto origin = Project::contentOrigin (r) + shift;
                    const double originTick = grid.tickAt ((double) origin);
                    for (auto note : r)
                    {
                        if (! note.hasType (ids::NOTE))
                            continue;
                        const auto n = Project::noteFrom (note);
                        const int pitch = n.pitch + transpose;
                        if (pitch < 0 || pitch > 127)
                            continue;
                        const auto on = (juce::int64) std::llround (grid.sampleAt (originTick + (double) n.start));
                        if (on < start || on >= end)
                            continue;
                        const auto off = juce::jmin (end, (juce::int64) std::llround (grid.sampleAt (originTick + (double) (n.start + n.length))));
                        tp.midi.push_back ({ on, true, (std::uint8_t) pitch, (float) juce::jlimit (1, 127, n.velocity + velocityShift) / 127.0f });
                        tp.midi.push_back ({ juce::jmax (on + 1, off), false, (std::uint8_t) pitch, 0.0f });
                    }
                    if (! (bool) r[ids::loop])
                        break;
                }
            }
            std::stable_sort (tp.midi.begin(), tp.midi.end(), [] (const MidiPlayback& x, const MidiPlayback& y)
                              { return x.time != y.time ? x.time < y.time : (! x.on && y.on); });

            snap->tracks.push_back (std::move (tp));
            continue;
        }

        for (auto r : t)
        {
            if (! r.hasType (ids::REGION) || Project::isMidiRegion (r) || (bool) r[ids::mute])
                continue;

            // Looped: copies of what the region plays, back to back up to the next region.
            const size_t firstPlayback = tp.regions.size();
            auto addRepeats = [&]
            {
                if (! (bool) r[ids::loop])
                    return;
                const auto length = (juce::int64) r[ids::length];
                const auto loopsTo = project.loopEnd (r);
                const auto count = tp.regions.size();
                for (juce::int64 shift = length; length > 0 && (juce::int64) r[ids::start] + shift < loopsTo; shift += length)
                    for (size_t i = firstPlayback; i < count; ++i)
                    {
                        auto copy = tp.regions[i];
                        copy.start += shift;
                        if (copy.start >= loopsTo)
                            continue;
                        if (copy.start + copy.length > loopsTo)
                        {
                            copy.length = loopsTo - copy.start;
                            clampFades (copy.length, copy.fadeIn, 0, copy.fadeIn, copy.fadeOut);
                        }
                        tp.regions.push_back (copy);
                    }
            };

            // A take folder plays its comp: each segment from its take, a short crossfade at
            // every switch (the de-click edges), the folder's fades at its ends.
            if (Project::isTakeFolder (r))
            {
                const auto takeList = Project::takes (r);
                const auto origin = Project::contentOrigin (r);
                const auto visibleFrom = (juce::int64) r[ids::start];
                const auto visibleTo = visibleFrom + (juce::int64) r[ids::length];
                const auto gain = dbToGain ((double) r.getProperty (ids::gain, 0.0));
                for (auto& c : Project::getComp (r))
                {
                    if (! juce::isPositiveAndBelow (c.take, takeList.size()))
                        continue;
                    const auto& take = takeList.getReference (c.take);
                    auto audio = pool.get (project.resolve (take[ids::file].toString()));
                    if (audio == nullptr)
                        continue;
                    const auto takeFrom = origin + (juce::int64) take[ids::start];
                    const auto takeTo = takeFrom + (juce::int64) take[ids::length];
                    const auto from = juce::jmax (origin + c.from, takeFrom, visibleFrom);
                    const auto to = juce::jmin (origin + c.to, takeTo, visibleTo);
                    if (to <= from)
                        continue;

                    RegionPlayback rp;
                    rp.audio = audio;
                    rp.start = from;
                    rp.offset = juce::jlimit ((juce::int64) 0, (juce::int64) audio->getNumSamples(), (juce::int64) take[ids::offset] + (from - takeFrom));
                    rp.length = juce::jmin (to - from, (juce::int64) audio->getNumSamples() - rp.offset);
                    rp.gain = gain;
                    clampFades (rp.length, from == visibleFrom ? (juce::int64) r.getProperty (ids::fadeIn, 0) : 0,
                                to == visibleTo ? (juce::int64) r.getProperty (ids::fadeOut, 0) : 0, rp.fadeIn, rp.fadeOut);
                    if (rp.length > 0)
                        tp.regions.push_back (std::move (rp));
                }
                addRepeats();
                continue;
            }

            RegionPlayback rp;
            rp.audio = pool.get (project.resolve (r[ids::file].toString()));
            if (rp.audio == nullptr)
                continue;

            rp.start = (juce::int64) r[ids::start];
            rp.offset = juce::jlimit ((juce::int64) 0, (juce::int64) rp.audio->getNumSamples(), (juce::int64) r[ids::offset]);
            rp.length = juce::jmin ((juce::int64) r[ids::length], (juce::int64) rp.audio->getNumSamples() - rp.offset);
            rp.gain = dbToGain ((double) r.getProperty (ids::gain, 0.0));
            clampFades (rp.length, (juce::int64) r.getProperty (ids::fadeIn, 0), (juce::int64) r.getProperty (ids::fadeOut, 0),
                        rp.fadeIn, rp.fadeOut);
            if (rp.length > 0)
                tp.regions.push_back (std::move (rp));
            addRepeats();
        }

        snap->tracks.push_back (std::move (tp));
    }

    // Aux tracks play after every other track (so their sends have arrived), in project order
    // except that a bus feeding another (a summing stack inside another, an aux sending to an
    // aux above it) plays first; then each send finds its aux. A send to an aux that still plays
    // earlier (a loop) is dropped.
    std::stable_partition (snap->tracks.begin(), snap->tracks.end(), [] (const TrackPlayback& t) { return ! t.aux; });
    {
        const auto firstAux = (size_t) std::distance (snap->tracks.begin(),
                                                      std::find_if (snap->tracks.begin(), snap->tracks.end(), [] (auto& t) { return t.aux; }));
        const size_t numAux = snap->tracks.size() - firstAux;
        std::map<int, size_t> auxAt;
        for (size_t i = firstAux; i < snap->tracks.size(); ++i)
            auxAt[snap->tracks[i].runtime->id] = i - firstAux;

        // Which buses each bus feeds: its sends and, for a stack main inside another, its main.
        std::vector<std::vector<size_t>> feeds (numAux);
        std::vector<int> waiting (numAux, 0);
        for (size_t i = 0; i < numAux; ++i)
        {
            const auto t = project.trackById (snap->tracks[firstAux + i].runtime->id);
            juce::Array<int> to;
            for (auto send : Project::sends (t))
                to.add ((int) send[ids::bus]);
            if (auto main = stacks::mainOf (project, t); stacks::isSumming (main))
                to.add ((int) main[ids::id]);
            for (int id : to)
                if (auto it = auxAt.find (id); it != auxAt.end() && it->second != i)
                {
                    feeds[i].push_back (it->second);
                    ++waiting[it->second];
                }
        }

        // Kahn's sort, earliest in the project first; whatever is left in a loop keeps its order.
        std::vector<size_t> order;
        std::vector<bool> placed (numAux, false);
        while (order.size() < numAux)
        {
            size_t next = numAux;
            for (size_t i = 0; i < numAux && next == numAux; ++i)
                if (! placed[i] && waiting[i] == 0)
                    next = i;
            if (next == numAux)
                for (size_t i = 0; i < numAux && next == numAux; ++i)
                    if (! placed[i])
                        next = i;
            placed[next] = true;
            order.push_back (next);
            for (auto f : feeds[next])
                --waiting[f];
        }

        std::vector<TrackPlayback> buses;
        for (auto i : order)
            buses.push_back (std::move (snap->tracks[firstAux + i]));
        for (size_t i = 0; i < numAux; ++i)
            snap->tracks[firstAux + i] = std::move (buses[i]);
    }

    if (master != nullptr)
        snap->masterInserts = insertsFor (s.getChildWithName (ids::MASTER), *master);
    std::map<int, int> indexOfTrack;
    for (size_t i = 0; i < snap->tracks.size(); ++i)
        indexOfTrack[snap->tracks[i].runtime->id] = (int) i;

    for (size_t i = 0; i < snap->tracks.size(); ++i)
    {
        auto& tp = snap->tracks[i];
        for (auto send : Project::sends (project.trackById (tp.runtime->id)))
        {
            TrackPlayback::Send sp;
            sp.gain = dbToGain ((double) send.getProperty (ids::level, 0.0));
            sp.pre = (bool) send[ids::pre];
            if (auto it = indexOfTrack.find ((int) send[ids::bus]); it != indexOfTrack.end()
                && it->second > (int) i && snap->tracks[(size_t) it->second].aux)
                sp.target = it->second;
            tp.sends.push_back (sp);
        }

        // A summing stack's member goes out through its main: a last, unity, post-fader send.
        if (auto main = stacks::mainOf (project, project.trackById (tp.runtime->id)); stacks::isSumming (main))
            if (auto it = indexOfTrack.find ((int) main[ids::id]); it != indexOfTrack.end() && it->second > (int) i)
            {
                TrackPlayback::Send out;
                out.target = it->second;
                tp.sends.push_back (out);
                tp.toMaster = false;
            }
    }

    snap->alignLatencies();
    return snap;
}

void Snapshot::alignLatencies()
{
    // Tracks are in playing order, so every source comes before the buses it feeds. Each path's
    // arrival is where its input lines up plus its own latency; every input to a bus (or to the
    // Stereo Out) waits for the latest one.
    const auto count = tracks.size();
    std::vector<int> inputAt (count, 0), outputAt (count, 0);
    auto feeds = [count] (const TrackPlayback::Send& send, size_t from)
    {
        return send.target > (int) from && send.target < (int) count;
    };

    int masterAt = 0;
    for (size_t i = 0; i < count; ++i)
    {
        auto& t = tracks[i];
        outputAt[i] = (t.aux ? inputAt[i] : 0) + t.latency;
        for (auto& send : t.sends)
            if (feeds (send, i))
                inputAt[(size_t) send.target] = juce::jmax (inputAt[(size_t) send.target], outputAt[i]);
        if (t.toMaster)
            masterAt = juce::jmax (masterAt, outputAt[i]);
    }

    for (size_t i = 0; i < count; ++i)
    {
        auto& t = tracks[i];
        t.outputDelay = t.toMaster ? masterAt - outputAt[i] : 0;
        for (auto& send : t.sends)
            send.delay = feeds (send, i) ? inputAt[(size_t) send.target] - outputAt[i] : 0;
    }

    latency = masterAt;
    for (auto& fx : masterInserts)
        if (fx.on && fx.pedal != nullptr)
            latency += juce::jmax (0, fx.pedal->getLatencySamples());
}

void Engine::handleAsyncUpdate()
{
    cancelPendingUpdate();
    syncRuntimes();
    publish();
}

void Engine::publish()
{
    auto snap = project != nullptr ? buildSnapshot (*project, pool, runtimes, masterRuntime.get()) : std::make_unique<Snapshot>();
    playbackLatency = snap->latency;
    publishedLatencyKey = latencyKey();
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

int Engine::latencyKey() const
{
    int key = 0;
    auto add = [&key] (int v) { key = key * 31 + v; };
    for (auto& [id, rt] : runtimes)
    {
        if (rt->rig != nullptr)         add (rt->rig->getLatencySamples());
        if (rt->instrument != nullptr)  add (rt->instrument->getLatencySamples());
        for (auto& fx : rt->inserts)    add (fx.pedal != nullptr ? fx.pedal->getLatencySamples() : 0);
    }
    if (masterRuntime != nullptr)
        for (auto& fx : masterRuntime->inserts)
            add (fx.pedal != nullptr ? fx.pedal->getLatencySamples() : 0);
    return key;
}

void Engine::timerCallback()
{
    freeRetired();
    // A rig or plug-in that changed its latency (a new board, a plug-in setting) without the
    // project changing: line everything up again.
    if (project != nullptr && latencyKey() != publishedLatencyKey)
        publish();
    if (isRecording())
        drainRecordedMidi();

    // One meter frame: collect the audio thread's peaks so every view sees the same values.
    masterShown = { masterPeakLeft.exchange (0.0f), masterPeakRight.exchange (0.0f) };
    for (auto& [id, rt] : runtimes)
    {
        rt->shownLeft = rt->peakLeft.exchange (0.0f);
        rt->shownRight = rt->peakRight.exchange (0.0f);
    }

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
    liveMidi.reset (sampleRate);

    // No callbacks run until this returns, so the rigs can be re-prepared for the new settings.
    for (auto& [id, rt] : runtimes)
        prepareRig (*rt);
    if (masterRuntime != nullptr)
        prepareRig (*masterRuntime);
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
    const bool looping = isPlaying && snap.loopOn && snap.loopEnd > snap.loopStart && (! isRec || cycleRecording.load());

    std::array<const float*, 64> ins {};
    float masterL = 0.0f, masterR = 0.0f;

    // Live MIDI for the whole callback, as instrument events with offsets into it.
    liveBuffer.clear();
    liveMidi.removeNextBlockOfMessages (liveBuffer, numSamples);
    liveEvents.clear();
    for (const auto meta : liveBuffer)
    {
        const auto m = meta.getMessage();
        NoteEvent e;
        e.offset = juce::jlimit (0, numSamples - 1, meta.samplePosition);
        if (m.isNoteOn())                                   { e.kind = NoteEvent::noteOn;  e.note = m.getNoteNumber(); e.value = m.getFloatVelocity(); }
        else if (m.isNoteOff())                             { e.kind = NoteEvent::noteOff; e.note = m.getNoteNumber(); }
        else if (m.isSustainPedalOn() || m.isSustainPedalOff()) { e.kind = NoteEvent::sustain; e.value = m.isSustainPedalOn() ? 1.0f : 0.0f; }
        else if (m.isPitchWheel())                          { e.kind = NoteEvent::pitchBend; e.value = (float) (m.getPitchWheelValue() - 8192) / 8192.0f; }
        else if (m.isControllerOfType (1))                  { e.kind = NoteEvent::modWheel; e.value = (float) m.getControllerValue() / 127.0f; }
        else if (m.isAllNotesOff() || m.isAllSoundOff())    { e.kind = NoteEvent::allNotesOff; }
        else continue;
        if (liveEvents.size() < liveEvents.capacity())
            liveEvents.push_back (e);
    }
    const int liveTarget = midiTarget.load();
    const int recordingMidiInto = midiRecordTrack.load();
    size_t liveIndex = 0;
    const bool punch = isRec && punching.load();
    const auto punchStart = punchFrom.load(), punchEnd = punchTo.load();

    for (int done = 0; done < numSamples;)
    {
        int n = juce::jmin (numSamples - done, mixBuffer.getNumSamples());
        if (looping && pos < snap.loopEnd && pos + n > snap.loopEnd)
            n = (int) (snap.loopEnd - pos);

        // Autopunch: blocks break at the locators, and only the ones between them record.
        if (punch && isPlaying)
        {
            if (pos < punchStart && pos + n > punchStart)    n = (int) (punchStart - pos);
            else if (pos < punchEnd && pos + n > punchEnd)   n = (int) (punchEnd - pos);
        }
        const bool punchedIn = ! punch || (isPlaying && pos >= punchStart && pos < punchEnd && ! punchDone.load());

        const int numIns = juce::jmin (numInputs, (int) ins.size());
        for (int ch = 0; ch < numIns; ++ch)
            ins[(size_t) ch] = inputs[ch] != nullptr ? inputs[ch] + done : nullptr;

        Mixer::Block b;
        b.position = pos;
        b.numSamples = n;
        b.playing = isPlaying;
        b.recording = isRec && punchedIn;
        b.inputs = ins.data();
        b.numInputs = numIns;
        b.monitorInputs = ! feedbackProtection.load (std::memory_order_relaxed);
        b.left = mixBuffer.getWritePointer (0);
        b.right = mixBuffer.getWritePointer (1);

        // This sub-block's share of the live events, re-based to its start.
        const size_t firstLive = liveIndex;
        while (liveIndex < liveEvents.size() && liveEvents[liveIndex].offset < done + n)
            liveEvents[liveIndex++].offset -= done;
        b.liveTrackId = liveTarget;
        b.liveEvents = liveEvents.data() + firstLive;
        b.numLiveEvents = (int) (liveIndex - firstLive);

        // Recording MIDI: the live notes, at timeline samples, from the record start on.
        if (isRec && punchedIn && isPlaying && recordingMidiInto == liveTarget && recordingMidiInto >= 0)
        {
            for (int i = 0; i < b.numLiveEvents; ++i)
            {
                const auto& e = b.liveEvents[i];
                const auto when = pos + e.offset;
                if ((e.kind != NoteEvent::noteOn && e.kind != NoteEvent::noteOff) || (e.kind == NoteEvent::noteOn && recordedSamples.load() == 0 && when < recordStart.load()))
                    continue;
                if (midiRecordFifo.getFreeSpace() > 0)
                {
                    const auto scope = midiRecordFifo.write (1);
                    midiRecordRing[(size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)]
                        = { when, (std::uint8_t) e.note, (std::uint8_t) juce::jlimit (1, 127, juce::roundToInt (e.value * 127.0f)),
                            e.kind == NoteEvent::noteOn };
                }
            }
        }

        Mixer::render (snap, b);

        // Recording: the dry input of every armed track, from the record start onwards.
        if (isRec && isPlaying && punchedIn)
        {
            // Until the first sample is in, wait for the record start (the count-in plays first);
            // after that every block counts, wherever the cycle has taken the playhead.
            const auto from = recordedSamples.load() > 0 ? 0 : (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, recordStart.load() - pos);
            if (from < n)
            {
                for (auto& t : snap.tracks)
                {
                    auto* w = t.runtime->recorder.load();
                    if (w == nullptr || ! juce::isPositiveAndBelow (t.input, numIns) || ins[(size_t) t.input] == nullptr)
                        continue;
                    const float* channel = ins[(size_t) t.input] + from;
                    w->write (&channel, n - from);
                    if (auto* peaks = t.runtime->livePeaks.load())
                        peaks->add (channel, n - from);
                }
                recordedSamples.fetch_add (n - from);
            }
        }
        if (punch && isPlaying && pos + n >= punchEnd)
            punchDone = true;           // a jump back doesn't record over the take again

        // The click keeps time with what you hear: as late as delay compensation makes the mix.
        if (isPlaying && (snap.click || (isRec && ! punch && pos < recordStart.load())))
        {
            auto clickBlock = b;
            clickBlock.position = pos - snap.latency;
            Mixer::addClick (snap, clickBlock, clickLevel.load (std::memory_order_relaxed));
        }

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
std::unique_ptr<juce::AudioFormatWriter> makeWriter (const juce::File& file, const BounceOptions& options, double rate)
{
    std::unique_ptr<juce::AudioFormat> format;
    if (options.format == "aiff")       format = std::make_unique<juce::AiffAudioFormat>();
    else if (options.format == "flac")  format = std::make_unique<juce::FlacAudioFormat>();
    else                                format = std::make_unique<juce::WavAudioFormat>();

    int bits = options.bits;
    if (options.format == "flac")
        bits = juce::jmin (bits, 24);

    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return nullptr;
    return format->createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (2)
                                                                             .withBitsPerSample (bits)
                                                                             .withSampleFormat (bits == 32 ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                                                                           : juce::AudioFormatWriterOptions::SampleFormat::integral));
}

/** Renders each job's snapshot to its file in turn, on a background thread. */
class BounceThread final : public juce::Thread
{
public:
    struct Job { std::unique_ptr<Snapshot> snap; juce::File file; };

    BounceThread (std::vector<Job> j, BounceOptions o, juce::int64 from, juce::int64 to, double rate,
                  std::atomic<float>& p, std::function<void (bool)> finished)
        : juce::Thread ("Woodshed bounce"), jobs (std::move (j)), options (std::move (o)),
          start (from), end (to), sampleRate (rate), progress (p), onFinished (std::move (finished)) {}

    void run() override
    {
        bool ok = true;
        for (size_t i = 0; i < jobs.size() && ok && ! threadShouldExit(); ++i)
            ok = render (jobs[i], (float) i / (float) jobs.size(), 1.0f / (float) jobs.size());

        progress = 1.0f;
        ok = ok && ! threadShouldExit();
        auto done = onFinished;
        juce::MessageManager::callAsync ([done, ok] { done (ok); });
    }

    std::vector<Job> jobs;     // own the bounce's private rigs and plug-ins through their runtimes

private:
    bool render (Job& job, float progressFrom, float progressSpan)
    {
        return Engine::renderToFile (*job.snap, job.file, start, end + (juce::int64) (sampleRate * bounceTailSeconds), options, sampleRate,
                                     [&] (float p) { progress = progressFrom + progressSpan * p; },
                                     [this] { return threadShouldExit(); });
    }

    BounceOptions options;
    juce::int64 start, end;
    double sampleRate;
    std::atomic<float>& progress;
    std::function<void (bool)> onFinished;
};
}

bool Engine::renderToFile (Snapshot& snap, const juce::File& file, juce::int64 start, juce::int64 end, const BounceOptions& options,
                           double sampleRate, const std::function<void (float)>& progress, const std::function<bool()>& shouldStop)
{
    // Delay compensation makes the whole mix `latency` late: render that much further and drop
    // the start, so the file lines up with the timeline.
    const auto skip = (juce::int64) juce::jmax (0, snap.latency);
    const auto total = juce::jmax ((juce::int64) 1, end - start);
    juce::AudioBuffer<float> out (2, bounceBlock);
    auto stopping = [&] { return shouldStop && shouldStop(); };

    // Normalising needs the whole thing first; otherwise it streams to the file.
    juce::AudioBuffer<float> whole;
    if (options.normalize)
        whole.setSize (2, (int) juce::jmin (total, (juce::int64) std::numeric_limits<int>::max() / 2));

    auto writer = makeWriter (file, options, sampleRate);
    if (writer == nullptr)
        return false;

    bool ok = true;
    for (auto pos = start; pos < end + skip && ! stopping(); pos += bounceBlock)
    {
        const int n = (int) juce::jmin ((juce::int64) bounceBlock, end + skip - pos);
        Mixer::Block b;
        b.position = pos;
        b.numSamples = n;
        b.playing = true;
        b.left = out.getWritePointer (0);
        b.right = out.getWritePointer (1);
        Mixer::render (snap, b);
        out.applyGain (0, n, snap.masterGain);

        // The part of this block that belongs in the file.
        const auto keepFrom = (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, start + skip - pos);
        const auto at = pos + keepFrom - start - skip;
        if (keepFrom < n)
        {
            if (options.normalize)
            {
                if (at + (n - keepFrom) <= whole.getNumSamples())
                    for (int ch = 0; ch < 2; ++ch)
                        whole.copyFrom (ch, (int) at, out, ch, keepFrom, n - keepFrom);
            }
            else
                ok = ok && writer->writeFromAudioSampleBuffer (out, keepFrom, n - keepFrom);
        }
        if (progress)
            progress ((float) juce::jlimit (0.0, 1.0, (double) (pos - start) / (double) (total + skip)));
    }

    if (options.normalize && ! stopping())
    {
        const auto peak = whole.getMagnitude (0, whole.getNumSamples());
        if (peak > 0.0f)
            whole.applyGain (juce::Decibels::decibelsToGain (-0.1f) / peak);
        ok = writer->writeFromAudioSampleBuffer (whole, 0, whole.getNumSamples());
    }
    return ok && ! stopping();
}

std::unique_ptr<Snapshot> Engine::makeBounceSnapshot (int onlyTrackId, bool withMasterEffects)
{
    // Private copies of every rig and plug-in, so the bounce doesn't fight the live tracks for them.
    std::map<int, std::shared_ptr<TrackRuntime>> fresh;
    for (auto t : project->state)
    {
        if (! t.hasType (ids::TRACK))
            continue;

        auto rt = std::make_shared<TrackRuntime>();
        rt->id = (int) t[ids::id];
        rt->work.setSize (2, bounceBlock);
        rt->busInput.setSize (2, bounceBlock);
        rt->preFader.allocate();
        rt->postFader.allocate();
        rt->gainLeft.reset (sampleRate, 0.03);
        rt->gainRight.reset (sampleRate, 0.03);
        syncTrackDevices (*rt, t, sampleRate, bounceBlock);

        if (makeRig && ! Project::isInstrumentTrack (t) && ! Project::isAuxTrack (t))
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

    TrackRuntime freshMaster;
    if (withMasterEffects)
        syncTrackDevices (freshMaster, project->state.getChildWithName (ids::MASTER), sampleRate, bounceBlock);
    auto snap = buildSnapshot (*project, pool, fresh, withMasterEffects ? &freshMaster : nullptr);

    // A stem of a summing stack's main has its members in it; a member's own stem goes straight out.
    const auto only = project->trackById (onlyTrackId);
    const auto inside = stacks::isSumming (only) ? stacks::descendants (*project, only) : juce::Array<juce::ValueTree>();
    for (auto& t : snap->tracks)
    {
        t.armed = t.monitor = false;      // a bounce plays the song, never the live input
        const bool member = inside.contains (project->trackById (t.runtime->id));
        if (onlyTrackId >= 0 && t.runtime->id != onlyTrackId && ! member)
            t.audible = false;            // a stem: just this track (its sends go nowhere)
        if (t.runtime->id == onlyTrackId && ! t.toMaster)
        {
            t.sends.pop_back();           // its way into the stack main
            t.toMaster = true;
        }
        t.runtime->gainLeft.setCurrentAndTargetValue (t.audible ? t.gain : 0.0f);
        t.runtime->gainRight.setCurrentAndTargetValue (t.audible ? t.gain : 0.0f);
    }
    snap->alignLatencies();
    return snap;
}

void Engine::startRender (std::unique_ptr<Snapshot> snap, const juce::File& file, juce::int64 start, juce::int64 end,
                          const BounceOptions& options, std::function<void (bool)> done)
{
    std::vector<BounceThread::Job> jobs;
    jobs.push_back ({ std::move (snap), file });
    bounceProgress = 0.0f;
    bounceThread = std::make_unique<BounceThread> (std::move (jobs), options, start, end, sampleRate, bounceProgress,
                                                   [this, done] (bool ok)
                                                   {
                                                       if (bounceThread != nullptr)
                                                           bounceThread->stopThread (2000);
                                                       bounceThread.reset();   // frees the private rigs here, on the message thread
                                                       if (done) done (ok);
                                                   });
    bounceThread->startThread();
}

void Engine::startBounce (const juce::File& target, juce::int64 start, juce::int64 end, const BounceOptions& options,
                          std::function<void (bool)> done)
{
    if (project == nullptr || bounceThread != nullptr || end <= start)
    {
        if (done) done (false);
        return;
    }

    syncRigStates();
    const auto extension = options.format == "aiff" ? ".aif" : options.format == "flac" ? ".flac" : ".wav";

    std::vector<BounceThread::Job> jobs;
    if (options.stems)
    {
        // One file per track (and aux), named after it, in the target folder.
        target.createDirectory();
        for (auto t : project->state)
        {
            if (! t.hasType (ids::TRACK) || stacks::isVca (t) || stacks::isFolder (t))
                continue;       // no audio of their own
            const auto index = project->state.indexOf (t) + 1;
            auto name = juce::String (index).paddedLeft ('0', 2) + " " + juce::File::createLegalFileName (t[ids::name].toString());
            jobs.push_back ({ makeBounceSnapshot ((int) t[ids::id], false), target.getChildFile (name + extension) });
        }
    }
    else
        jobs.push_back ({ makeBounceSnapshot (-1, true), target.withFileExtension (extension) });

    bounceProgress = 0.0f;
    bounceThread = std::make_unique<BounceThread> (std::move (jobs), options, start, end, sampleRate, bounceProgress,
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
