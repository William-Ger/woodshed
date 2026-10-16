#include "Engine.h"

namespace woodshed::daw
{

namespace
{
constexpr int edgeFadeSamples = 96;            // de-click every region start and end (2 ms at 48k)
constexpr double bounceTailSeconds = 4.0;      // let delays and reverbs ring out past the last note
constexpr int bounceBlock = 512;

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

        if (track.aux)
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

        runInserts (track, a, c, n, b.position);

        // Volume and pan follow their automation lines, if they have them.
        float gain = track.gain, pan = track.pan;
        for (auto& line : track.automation)
        {
            if (line.target == TrackPlayback::AutoLine::volume) gain = dbToGain ((double) Project::evaluate (line.points, b.position));
            else if (line.target == TrackPlayback::AutoLine::pan) pan = Project::evaluate (line.points, b.position);
        }

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

        auto sendTo = [&] (size_t i)
        {
            const auto& send = track.sends[i];
            if (sendGains[i] <= 0.0f || ! juce::isPositiveAndBelow (send.target, (int) snap.tracks.size()))
                return;
            auto& bus = snap.tracks[(size_t) send.target].runtime->busInput;
            if (bus.getNumSamples() < n)
                return;
            juce::FloatVectorOperations::addWithMultiply (bus.getWritePointer (0), a, sendGains[i], n);
            juce::FloatVectorOperations::addWithMultiply (bus.getWritePointer (1), c, sendGains[i], n);
        };
        for (size_t i = 0; i < numSends; ++i)
            if (track.sends[i].pre)
                sendTo (i);

        float peakL = 0.0f, peakR = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float l = a[i] * rt.gainLeft.getNextValue();
            const float r = c[i] * rt.gainRight.getNextValue();
            a[i] = l;
            c[i] = r;
            b.left[i] += l;
            b.right[i] += r;
            peakL = juce::jmax (peakL, std::abs (l));
            peakR = juce::jmax (peakR, std::abs (r));
        }

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

    latencyAtRecord = getRecordLatencySamples();
    midiLatencyAtRecord = 0;
    if (auto* device = deviceManager.getCurrentAudioDevice())
        midiLatencyAtRecord = device->getOutputLatencyInSamples();
    for (auto& t : takes)
        if (t.midi)
            midiRecordTrack = t.trackId;
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
        project->addRegion (track, t.file.getRelativePathFrom (project->folder).replaceCharacter ('\\', '/'),
                            start, offset, length, track[ids::name].toString());
    }

    takes.clear();
    sendChangeMessage();
}

void Engine::finishMidiTake (int trackId, juce::int64 stopAt)
{
    auto track = project->trackById (trackId);
    const auto grid = project->grid();

    // Notes still held when recording stopped end there; everything moves back by the output
    // latency, since you played along with what you heard.
    std::vector<RecordedNote> notes;
    juce::int64 last = stopAt;
    for (auto n : recordedNotes)
    {
        if (n.end < 0) n.end = stopAt;
        n.start = juce::jmax ((juce::int64) 0, n.start - midiLatencyAtRecord);
        n.end = juce::jmax (n.start + 1, n.end - midiLatencyAtRecord);
        last = juce::jmax (last, n.end);
        notes.push_back (n);
    }

    // Like Logic, the region runs from the bar the recording started in to the end of the bar
    // it stopped in.
    const auto bar = grid.samplesPerBar();
    const auto from = (juce::int64) (std::floor ((double) juce::jmax ((juce::int64) 0, recordStart.load()) / bar) * bar);
    const auto to = (juce::int64) (std::ceil ((double) last / bar) * bar);

    auto region = project->addMidiRegion (track, from, juce::jmax ((juce::int64) bar, to - from), track[ids::name].toString());
    for (auto& n : notes)
    {
        Note note;
        note.pitch = n.pitch;
        note.velocity = n.velocity;
        note.start = std::llround (grid.toTicks ((double) (n.start - from)));
        note.length = juce::jmax ((juce::int64) 1, (juce::int64) std::llround (grid.toTicks ((double) (n.end - n.start))));
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
    const auto type = Project::isInstrumentTrack (track) ? track.getChildWithName (ids::INSTRUMENT)[ids::type].toString() : juce::String();
    if (type != rt.instrumentType)
    {
        rt.instrumentType = type;
        rt.instrument.reset();
        if (type.isNotEmpty() && makeInstrument)
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
        const auto pedalType = fx[ids::type].toString();
        auto existing = std::find_if (rt.inserts.begin(), rt.inserts.end(),
                                      [&] (const InsertRuntime& r) { return r.id == id && r.type == pedalType; });
        if (existing != rt.inserts.end())
        {
            next.push_back (*existing);
            continue;
        }

        if (auto* pt = Registry::get().find (pedalType))
        {
            std::shared_ptr<Pedal> pedal = pt->create();
            pedal->prepare (rate, block);
            next.push_back ({ id, pedalType, std::move (pedal) });
        }
    }
    rt.inserts = std::move (next);
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
            // Stored values, with the instrument's defaults for any knob the project doesn't have
            // (a project saved before the instrument grew that knob).
            const auto inst = t.getChildWithName (ids::INSTRUMENT);
            const auto values = Project::decodeValues (inst[ids::knobs].toString());
            if (auto* type = InstrumentRegistry::get().find (inst[ids::type].toString()))
                for (size_t k = 0; k < type->info.knobs.size() && k < tp.instrumentKnobs.size(); ++k)
                    tp.instrumentKnobs[k] = type->info.knobs[k].def;
            for (size_t k = 0; k < values.size() && k < tp.instrumentKnobs.size(); ++k)
                tp.instrumentKnobs[k] = values[k];

            const auto grid = project.grid();
            for (auto r : t)
            {
                if (! r.hasType (ids::REGION) || ! Project::isMidiRegion (r) || (bool) r[ids::mute])
                    continue;

                const auto start = (juce::int64) r[ids::start];
                const auto end = start + (juce::int64) r[ids::length];
                const auto origin = Project::contentOrigin (r);
                for (auto note : r)
                {
                    if (! note.hasType (ids::NOTE))
                        continue;
                    const auto n = Project::noteFrom (note);
                    const auto on = origin + (juce::int64) std::llround (grid.fromTicks ((double) n.start));
                    if (on < start || on >= end)
                        continue;
                    const auto off = juce::jmin (end, origin + (juce::int64) std::llround (grid.fromTicks ((double) (n.start + n.length))));
                    tp.midi.push_back ({ on, true, (std::uint8_t) n.pitch, (float) n.velocity / 127.0f });
                    tp.midi.push_back ({ juce::jmax (on + 1, off), false, (std::uint8_t) n.pitch, 0.0f });
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
        }

        snap->tracks.push_back (std::move (tp));
    }

    // Aux tracks play after every other track (so their sends have arrived), in project order;
    // then each send finds its aux. A send to an aux that plays earlier (aux to aux, upwards) is
    // dropped, so there are never loops.
    std::stable_partition (snap->tracks.begin(), snap->tracks.end(), [] (const TrackPlayback& t) { return ! t.aux; });

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
    auto snap = project != nullptr ? buildSnapshot (*project, pool, runtimes, masterRuntime.get()) : std::make_unique<Snapshot>();
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
    const bool looping = isPlaying && ! isRec && snap.loopOn && snap.loopEnd > snap.loopStart;

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
        b.recording = isRec;
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
        if (isRec && isPlaying && recordingMidiInto == liveTarget && recordingMidiInto >= 0)
        {
            for (int i = 0; i < b.numLiveEvents; ++i)
            {
                const auto& e = b.liveEvents[i];
                const auto when = pos + e.offset;
                if ((e.kind != NoteEvent::noteOn && e.kind != NoteEvent::noteOff) || (e.kind == NoteEvent::noteOn && when < recordStart.load()))
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
                    if (auto* peaks = t.runtime->livePeaks.load())
                        peaks->add (channel, n - from);
                }
                recordedSamples.fetch_add (n - from);
            }
        }

        if (isPlaying && (snap.click || (isRec && pos < recordStart.load())))
            Mixer::addClick (snap, b, clickLevel.load (std::memory_order_relaxed));

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
        rt->busInput.setSize (2, bounceBlock);
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
    syncTrackDevices (freshMaster, project->state.getChildWithName (ids::MASTER), sampleRate, bounceBlock);
    auto snap = buildSnapshot (*project, pool, fresh, &freshMaster);
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
