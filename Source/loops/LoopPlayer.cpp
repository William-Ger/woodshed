#include "LoopPlayer.h"
#include "../daw/Engine.h"

namespace woodshed::loops
{

namespace dids = daw::ids;

namespace
{
constexpr int edgeFadeSamples = 96;         // the arrange's de-click edge
float fadeCurve (float x)                   { return std::sin (juce::jlimit (0.0f, 1.0f, x) * juce::MathConstants<float>::halfPi); }

void fitFades (juce::int64 length, juce::int64 wantIn, juce::int64 wantOut, juce::int64& in, juce::int64& out)
{
    const auto edge = juce::jmin ((juce::int64) edgeFadeSamples, length / 2);
    out = juce::jlimit (edge, juce::jmax (edge, length - edge), wantOut);
    in = juce::jlimit (edge, juce::jmax (edge, length - out), wantIn);
}

int priority (const NoteEvent& e)
{
    return e.kind == NoteEvent::allNotesOff ? 0 : e.kind == NoteEvent::noteOff ? 1 : 2;
}

void push (std::vector<NoteEvent>& ev, const NoteEvent& e)
{
    if (ev.size() < ev.capacity())
        ev.push_back (e);
}
}

//==============================================================================
void buildCells (const daw::Project& project, daw::AudioPool& pool, const juce::ValueTree& track, std::vector<CellPlayback>& cells)
{
    cells.clear();
    const auto& grid = project.grid();

    for (auto cell : gridOf (track))
    {
        const int column = (int) cell[ids::column];
        if (! cell.hasType (ids::CELL) || ! juce::isPositiveAndBelow (column, maxScenes))
            continue;

        CellPlayback cp;
        cp.length = cellLength (grid, cell);
        cp.oneShot = isOneShot (cell);
        const auto pass = cp.length;

        for (auto r : cell)
        {
            if (! r.hasType (dids::REGION) || (bool) r[dids::mute])
                continue;

            const auto start = (juce::int64) r[dids::start];
            const auto length = (juce::int64) r[dids::length];

            if (daw::Project::isMidiRegion (r))
            {
                // As on the arrange: only notes starting inside the region play, cut at its end
                // (and here at the end of the pass, so every note ends before it wraps).
                const auto origin = start - (juce::int64) r[dids::offset];
                const auto visibleFrom = juce::jmax ((juce::int64) 0, start);
                const auto visibleTo = juce::jmin (pass, start + length);
                const int transpose = (int) r.getProperty (dids::transpose, 0);
                const int velocityShift = (int) r.getProperty (dids::velocity, 0);
                for (auto note : r)
                {
                    if (! note.hasType (dids::NOTE))
                        continue;
                    const auto n = daw::Project::noteFrom (note);
                    const int pitch = n.pitch + transpose;
                    if (pitch < 0 || pitch > 127)
                        continue;
                    const auto on = origin + (juce::int64) std::llround (grid.fromTicks ((double) n.start));
                    if (on < visibleFrom || on >= visibleTo)
                        continue;
                    const auto off = juce::jmin (juce::jmin (visibleTo, pass - 1),
                                                 origin + (juce::int64) std::llround (grid.fromTicks ((double) (n.start + n.length))));
                    if (off <= on)
                        continue;
                    const auto velocity = (float) juce::jlimit (1, 127, n.velocity + velocityShift) / 127.0f;
                    cp.midi.push_back ({ on, true, (std::uint8_t) pitch, velocity });
                    cp.midi.push_back ({ off, false, (std::uint8_t) pitch, 0.0f });
                }
                continue;
            }

            CellPlayback::Clip clip;
            clip.audio = pool.get (project.resolve (r[dids::file].toString()));
            if (clip.audio == nullptr)
                continue;
            clip.start = juce::jmax ((juce::int64) 0, start);
            clip.offset = juce::jlimit ((juce::int64) 0, (juce::int64) clip.audio->getNumSamples(), (juce::int64) r[dids::offset]);
            clip.length = juce::jmin (length, (juce::int64) clip.audio->getNumSamples() - clip.offset, pass - clip.start);
            clip.gain = juce::Decibels::decibelsToGain ((float) (double) r.getProperty (dids::gain, 0.0), -96.0f);
            fitFades (clip.length, (juce::int64) r.getProperty (dids::fadeIn, 0), (juce::int64) r.getProperty (dids::fadeOut, 0),
                      clip.fadeIn, clip.fadeOut);
            if (clip.length > 0)
                cp.audio.push_back (std::move (clip));
        }

        std::stable_sort (cp.midi.begin(), cp.midi.end(), [] (const CellPlayback::Event& x, const CellPlayback::Event& y)
                          { return x.time != y.time ? x.time < y.time : (! x.on && y.on); });

        if ((int) cells.size() <= column)
            cells.resize ((size_t) column + 1);
        cells[(size_t) column] = std::move (cp);
    }
}

//==============================================================================
LoopPlayer::LoopPlayer()
{
    fallbackCell.audio.reserve (1);
    fallbackCell.midi.reserve (8192);
}

bool LoopPlayer::send (const LoopCommand& c)
{
    if (commandFifo.getFreeSpace() < 1)
        return false;
    const auto scope = commandFifo.write (1);
    commands[(size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)] = c;
    return true;
}

void LoopPlayer::takeEvents (std::vector<LoopEvent>& out)
{
    const auto scope = eventFifo.read (eventFifo.getNumReady());
    for (int i = 0; i < scope.blockSize1; ++i) out.push_back (events[(size_t) (scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i) out.push_back (events[(size_t) (scope.startIndex2 + i)]);
}

void LoopPlayer::log (int trackId, int column, juce::int64 at)
{
    if (eventFifo.getFreeSpace() < 1)
        return;
    const auto scope = eventFifo.write (1);
    events[(size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)] = { trackId, column, at };
}

bool LoopPlayer::startRecording (int trackId, int column, Quantize q, int bars, bool midi, int latency,
                                 double sampleRate, double samplesPerBar)
{
    if (getRecordState() != RecordState::idle || samplesPerBar <= 0.0)
        return false;

    recordTrack = trackId;
    recordColumn = column;
    recordBars = bars;
    recordMidi = midi;
    recordLatency = midi ? 0 : juce::jmax (0, latency);
    recordQuantize = q < Quantize::bar ? Quantize::bar : q;
    recordSamplesPerBar = samplesPerBar;
    recordLength = bars > 0 ? (juce::int64) std::llround (bars * samplesPerBar) : -1;

    // Room for the cell, or for an open-ended take up to the longest cell (or two minutes).
    const int maxBars = bars > 0 ? bars : juce::jlimit (1, maxCellBars, (int) (sampleRate * 120.0 / samplesPerBar));
    const auto capacity = (int) std::llround (maxBars * samplesPerBar) + recordLatency + 1;
    if (! midi && (recordBuffer == nullptr || recordBuffer->getNumSamples() < capacity))
        recordBuffer = std::make_shared<juce::AudioBuffer<float>> (1, capacity);
    if (recordBuffer != nullptr)
        recordBuffer->clear();

    fallbackCell.length = 0;
    fallbackCell.oneShot = false;
    fallbackCell.midi.clear();
    fallbackCell.audio.clear();
    if (! midi)
    {
        CellPlayback::Clip clip;
        clip.audio = recordBuffer;
        clip.offset = recordLatency;
        fallbackCell.audio.push_back (clip);
    }
    written = 0;
    held.reset();

    recordState = (int) RecordState::waiting;
    LoopCommand c;
    c.kind = LoopCommand::record;
    c.trackId = trackId;
    c.column = column;
    c.quantize = recordQuantize;
    if (! send (c))
    {
        recordState = (int) RecordState::idle;
        return false;
    }
    return true;
}

void LoopPlayer::discardRecording()
{
    if (getRecordState() == RecordState::finished)
        recordState = (int) RecordState::discarded;     // the audio thread lets go of it, then goes idle
}

//==============================================================================
const CellPlayback* LoopPlayer::cellFor (const daw::TrackPlayback& tp, const TrackLoopState& st) const
{
    if (st.fallback)
        return &fallbackCell;
    if (juce::isPositiveAndBelow (st.playing, (int) tp.cells.size()) && tp.cells[(size_t) st.playing].isValid())
        return &tp.cells[(size_t) st.playing];
    return nullptr;
}

void LoopPlayer::queue (TrackLoopState& st, TrackLoopState::Next kind, int column, Quantize q, const daw::TimeGrid& grid,
                        juce::int64 position)
{
    st.next = kind;
    st.nextColumn = column;
    st.nextQuantize = q;
    st.nextAt = nextBoundary (grid, position, q);
}

void LoopPlayer::cancelRecording (TrackLoopState* st)
{
    if (st != nullptr)
    {
        if (st->recording)
            st->playing = TrackLoopState::none;
        if (st->next == TrackLoopState::Next::record)
            st->next = TrackLoopState::Next::nothing;
        st->recording = false;
    }
    recordState = (int) RecordState::idle;
}

void LoopPlayer::applyCommand (const daw::Snapshot& snap, const LoopCommand& c, juce::int64 position)
{
    using Next = TrackLoopState::Next;
    const auto state = getRecordState();
    const bool recordPending = state == RecordState::waiting || state == RecordState::recording;

    auto stateOf = [&snap] (int trackId) -> TrackLoopState*
    {
        for (auto& tp : snap.tracks)
            if (tp.runtime->id == trackId)
                return &tp.runtime->loop;
        return nullptr;
    };

    switch (c.kind)
    {
        case LoopCommand::launch:
            if (auto* st = stateOf (c.trackId))
            {
                if (recordPending && recordTrack == c.trackId)
                    cancelRecording (st);
                queue (*st, Next::launch, c.column, c.quantize, snap.grid, position);
            }
            break;

        case LoopCommand::stop:
            if (auto* st = stateOf (c.trackId))
            {
                if (recordPending && recordTrack == c.trackId)
                    cancelRecording (st);
                if (st->playing == TrackLoopState::none)
                    st->next = Next::nothing;
                else
                    queue (*st, Next::stop, TrackLoopState::none, c.quantize, snap.grid, position);
            }
            break;

        case LoopCommand::launchScene:
        case LoopCommand::stopAll:
        {
            // Every track switches on the same sample.
            for (auto& tp : snap.tracks)
            {
                if (tp.aux)
                    continue;
                auto& st = tp.runtime->loop;
                if (recordPending && recordTrack == tp.runtime->id)
                {
                    if (c.kind == LoopCommand::stopAll)
                        cancelRecording (&st);
                    else
                        continue;       // a scene leaves a recording alone
                }
                const bool hasCell = c.kind == LoopCommand::launchScene && juce::isPositiveAndBelow (c.column, (int) tp.cells.size())
                                     && tp.cells[(size_t) c.column].isValid();
                if (hasCell)
                    queue (st, Next::launch, c.column, c.quantize, snap.grid, position);
                else if (st.playing != TrackLoopState::none)
                    queue (st, Next::stop, TrackLoopState::none, c.quantize, snap.grid, position);
                else
                    st.next = Next::nothing;
            }
            break;
        }

        case LoopCommand::record:
            if (auto* st = stateOf (c.trackId); st != nullptr && state == RecordState::waiting)
                queue (*st, Next::record, c.column, c.quantize, snap.grid, position);
            else if (state == RecordState::waiting)
                recordState = (int) RecordState::idle;
            break;

        case LoopCommand::finishRecord:
            if (auto* st = stateOf (recordTrack))
            {
                if (st->recording && recordLength.load() < 0)
                {
                    // On the next bar line of the take (at least one bar).
                    const auto bars = juce::jmax ((juce::int64) 1, (juce::int64) std::ceil ((double) st->elapsed / recordSamplesPerBar - 1.0e-9));
                    recordLength = (juce::int64) std::llround ((double) bars * recordSamplesPerBar);
                }
                else if (state == RecordState::waiting)
                    cancelRecording (st);
            }
            break;

        default: break;
    }
}

void LoopPlayer::beginBlock (const daw::Snapshot& snap, juce::int64 pos, int n, bool playing)
{
    using Next = TrackLoopState::Next;
    const bool jumped = pos != expectedPosition;

    // Commands wait while the transport is stopped (the message thread starts it to launch).
    if (playing)
    {
        const auto scope = commandFifo.read (commandFifo.getNumReady());
        for (int i = 0; i < scope.blockSize1; ++i) applyCommand (snap, commands[(size_t) (scope.startIndex1 + i)], pos);
        for (int i = 0; i < scope.blockSize2; ++i) applyCommand (snap, commands[(size_t) (scope.startIndex2 + i)], pos);
    }

    auto state = getRecordState();
    if (! playing && (state == RecordState::waiting || state == RecordState::recording))
        state = RecordState::idle, recordState = (int) RecordState::idle;
    if (! playing && state == RecordState::tail)
        state = RecordState::finished, recordState = (int) RecordState::finished;
    if (state == RecordState::waiting || state == RecordState::recording)
    {
        // The track went away (or the project did): nothing to record into.
        bool present = false;
        for (auto& tp : snap.tracks)
            present = present || tp.runtime->id == recordTrack;
        if (! present)
            state = RecordState::idle, recordState = (int) RecordState::idle;
    }
    captureFrom = state == RecordState::recording || state == RecordState::tail ? 0 : n;

    // A finished recording is in the model once committed: let go of the buffer when nothing
    // plays it any more (or it was thrown away).
    if (state == RecordState::committed || state == RecordState::discarded)
    {
        bool inUse = false;
        for (auto& tp : snap.tracks)
        {
            auto& st = tp.runtime->loop;
            if (st.fallback && juce::isPositiveAndBelow (st.playing, (int) tp.cells.size()) && tp.cells[(size_t) st.playing].isValid())
                st.fallback = false;
            if (st.fallback && state == RecordState::discarded)
            {
                // A take thrown away never reaches the snapshot: stop playing it.
                log (tp.runtime->id, TrackLoopState::none, pos);
                st.fallback = false;
                st.playing = TrackLoopState::none;
            }
            inUse = inUse || st.fallback;
        }
        if (! inUse)
            state = RecordState::idle, recordState = (int) RecordState::idle;
    }

    for (auto& tp : snap.tracks)
    {
        auto& st = tp.runtime->loop;
        st.numSegments = st.numSwitches = 0;

        if (! playing)
        {
            if (st.playing != TrackLoopState::none)
                log (tp.runtime->id, TrackLoopState::none, pos);
            st.playing = TrackLoopState::none;
            st.next = Next::nothing;
            st.recording = false;
            st.fallback = false;
            st.sounding.reset();        // the engine sends all-notes-off when the transport stops
            continue;
        }

        if (st.fallback && juce::isPositiveAndBelow (st.playing, (int) tp.cells.size()) && tp.cells[(size_t) st.playing].isValid())
            st.fallback = false;
        if (jumped && st.next != Next::nothing)
            st.nextAt = nextBoundary (snap.grid, pos, st.nextQuantize);
        if (st.playing == TrackLoopState::none && st.next == Next::nothing)
            continue;

        // An open-ended take stops when its buffer is full.
        if (st.recording && recordLength.load() < 0 && recordBuffer != nullptr && ! recordMidi)
        {
            const auto room = (juce::int64) recordBuffer->getNumSamples() - recordLatency - 1;
            const auto fullBars = (juce::int64) std::floor ((double) room / recordSamplesPerBar);
            if (st.elapsed + n >= (juce::int64) std::llround ((double) fullBars * recordSamplesPerBar))
                recordLength = (juce::int64) std::llround ((double) juce::jmax ((juce::int64) 1, fullBars) * recordSamplesPerBar);
        }

        int cursor = 0;
        for (int guard = 0; guard < 8 && cursor < n; ++guard)
        {
            int k = n;
            bool atNext = false, atEnd = false;
            if (st.next != Next::nothing)
            {
                const auto at = juce::jlimit ((juce::int64) cursor, (juce::int64) n, st.nextAt - pos);
                if (at < n) { k = (int) at; atNext = true; }
            }
            if (st.playing != TrackLoopState::none)
            {
                const auto* cell = cellFor (tp, st);
                juce::int64 limit = -1;
                if (st.recording)               limit = recordLength.load();
                else if (cell == nullptr)       limit = st.fallback ? -1 : st.elapsed;     // its cell was removed: stop
                else if (cell->oneShot)         limit = cell->length;
                if (limit >= 0)
                {
                    const auto at = cursor + juce::jmax ((juce::int64) 0, limit - st.elapsed);
                    if (at < n && at <= k)
                    {
                        // The end comes first; a launch due on the same sample follows it.
                        k = (int) at;
                        atEnd = true;
                        atNext = false;
                    }
                }
            }

            if (k > cursor && st.numSegments < (int) st.segments.size())
            {
                st.segments[(size_t) st.numSegments++] = { cursor, k, st.playing, st.elapsed, st.recording, st.fallback };
                if (st.playing != TrackLoopState::none)
                    st.elapsed += k - cursor;
            }
            cursor = k;
            if (cursor >= n || ! (atNext || atEnd))
                break;

            const bool fromCell = st.playing != TrackLoopState::none && ! st.recording;
            auto addSwitch = [&st, cursor] (bool from, bool to)
            {
                if (st.numSwitches < (int) st.switches.size())
                    st.switches[(size_t) st.numSwitches++] = { cursor, from, to };
            };

            if (atEnd)
            {
                if (st.recording)
                {
                    // The take is complete: loop it from the recording until the snapshot has it.
                    const auto length = recordLength.load();
                    fallbackCell.length = length;
                    for (auto& clip : fallbackCell.audio)
                    {
                        clip.offset = recordLatency;
                        clip.length = length;
                        fitFades (length, 0, 0, clip.fadeIn, clip.fadeOut);
                    }
                    st.recording = false;
                    st.fallback = true;
                    st.elapsed = 0;
                    recordState = (int) RecordState::tail;      // this block's input (and the latency) still comes in
                    log (tp.runtime->id, st.playing, pos + cursor);
                }
                else
                {
                    addSwitch (fromCell, false);
                    log (tp.runtime->id, TrackLoopState::none, pos + cursor);
                    st.playing = TrackLoopState::none;
                    st.fallback = false;
                }
                continue;
            }

            // The queued launch, stop or record start.
            const auto next = st.next;
            st.next = Next::nothing;
            if (next == Next::launch && juce::isPositiveAndBelow (st.nextColumn, (int) tp.cells.size())
                && tp.cells[(size_t) st.nextColumn].isValid())
            {
                addSwitch (fromCell, true);
                st.playing = st.nextColumn;
                st.elapsed = 0;
                st.fallback = false;
                log (tp.runtime->id, st.playing, pos + cursor);
            }
            else if (next == Next::record)
            {
                addSwitch (fromCell, false);
                if (st.playing != TrackLoopState::none)
                    log (tp.runtime->id, TrackLoopState::none, pos + cursor);
                st.playing = st.nextColumn;
                st.elapsed = 0;
                st.recording = true;
                st.fallback = false;
                written = 0;
                captureFrom = cursor;
                recordState = (int) RecordState::recording;
            }
            else if (st.playing != TrackLoopState::none)
            {
                addSwitch (fromCell, false);
                log (tp.runtime->id, TrackLoopState::none, pos + cursor);
                st.playing = TrackLoopState::none;
                st.fallback = false;
            }
        }

        bool anyCell = false;
        for (int i = 0; i < st.numSegments; ++i)
            anyCell = anyCell || st.segments[(size_t) i].column != TrackLoopState::none;
        if (! anyCell && st.numSwitches == 0)
            st.numSegments = 0;
    }
}

void LoopPlayer::endBlock (const daw::Snapshot& snap, juce::int64 pos, int n)
{
    const auto state = getRecordState();
    if ((state == RecordState::recording || state == RecordState::tail) && captureFrom < n)
    {
        written += n - captureFrom;
        if (state == RecordState::tail && written >= recordLength.load() + recordLatency)
        {
            // Notes still held end with the take.
            for (int p = 0; p < 128; ++p)
                if (held[(size_t) p] && fallbackCell.midi.size() < fallbackCell.midi.capacity())
                    fallbackCell.midi.push_back ({ juce::jmax ((juce::int64) 0, recordLength.load() - 1), false, (std::uint8_t) p, 0.0f });
            held.reset();
            recordState = (int) RecordState::finished;
        }
    }
    expectedPosition = pos + n;

    for (auto& tp : snap.tracks)
    {
        auto& st = tp.runtime->loop;
        const auto* cell = st.playing != TrackLoopState::none ? cellFor (tp, st) : nullptr;
        st.shownColumn.store (st.playing, std::memory_order_relaxed);
        st.shownNext.store (st.next == TrackLoopState::Next::nothing ? -2 : st.next == TrackLoopState::Next::stop ? -1 : st.nextColumn,
                            std::memory_order_relaxed);
        st.shownNextIsRecord.store (st.next == TrackLoopState::Next::record, std::memory_order_relaxed);
        st.shownRecording.store (st.recording, std::memory_order_relaxed);
        st.shownElapsed.store (st.elapsed, std::memory_order_relaxed);
        st.shownLength.store (st.recording ? recordLength.load() : cell != nullptr ? cell->length : 0, std::memory_order_relaxed);
    }
}

//==============================================================================
bool LoopPlayer::isActive (const daw::TrackRuntime& rt)
{
    return rt.loop.numSegments > 0;
}

bool LoopPlayer::hearsInput (const daw::TrackRuntime& rt) const
{
    for (int i = 0; i < rt.loop.numSegments; ++i)
        if (rt.loop.segments[(size_t) i].recording)
            return true;
    return false;
}

void LoopPlayer::renderAudio (const daw::TrackPlayback& tp, float* out) const
{
    const auto& st = tp.runtime->loop;
    for (int s = 0; s < st.numSegments; ++s)
    {
        const auto& seg = st.segments[(size_t) s];
        if (seg.column == TrackLoopState::none || seg.recording)
            continue;
        const CellPlayback* cell = seg.fallback ? &fallbackCell
                                 : juce::isPositiveAndBelow (seg.column, (int) tp.cells.size()) ? &tp.cells[(size_t) seg.column] : nullptr;
        if (cell == nullptr || ! cell->isValid())
            continue;

        const auto pass = cell->length;
        auto elapsed = seg.elapsed;
        for (int i = seg.from; i < seg.to;)
        {
            const auto phase = cell->oneShot ? elapsed : elapsed % pass;
            if (phase >= pass)
                break;
            const auto chunk = juce::jmin ((juce::int64) (seg.to - i), pass - phase);
            for (auto& clip : cell->audio)
            {
                const auto from = juce::jmax (phase, clip.start);
                const auto to = juce::jmin (phase + chunk, clip.start + clip.length);
                if (from >= to || clip.audio == nullptr)
                    continue;
                const float* src = clip.audio->getReadPointer (0);
                for (auto t = from; t < to; ++t)
                {
                    const auto intoClip = t - clip.start;
                    const auto fromEnd = clip.start + clip.length - 1 - t;
                    float g = clip.gain;
                    if (intoClip < clip.fadeIn) g *= fadeCurve ((float) intoClip / (float) clip.fadeIn);
                    if (fromEnd < clip.fadeOut) g *= fadeCurve ((float) fromEnd / (float) clip.fadeOut);
                    out[i + (int) (t - phase)] += src[clip.offset + intoClip] * g;
                }
            }
            i += (int) chunk;
            elapsed += chunk;
        }
    }
}

void LoopPlayer::emitCell (const CellPlayback& cell, const TrackLoopState::Segment& seg, std::vector<NoteEvent>& ev, TrackLoopState& st)
{
    const auto pass = cell.length;
    auto elapsed = seg.elapsed;
    for (int i = seg.from; i < seg.to;)
    {
        const auto phase = cell.oneShot ? elapsed : elapsed % pass;
        if (phase >= pass)
            break;
        const auto chunk = juce::jmin ((juce::int64) (seg.to - i), pass - phase);
        auto it = std::lower_bound (cell.midi.begin(), cell.midi.end(), phase,
                                    [] (const CellPlayback::Event& e, juce::int64 t) { return e.time < t; });
        for (; it != cell.midi.end() && it->time < phase + chunk; ++it)
        {
            const int at = i + (int) (it->time - phase);
            if (it->on)
            {
                push (ev, { at, NoteEvent::noteOn, it->pitch, it->velocity });
                st.sounding.set (it->pitch);
            }
            else if (st.sounding[it->pitch])
            {
                push (ev, { at, NoteEvent::noteOff, it->pitch, 0.0f });
                st.sounding.reset (it->pitch);
            }
        }
        i += (int) chunk;
        elapsed += chunk;

        // The end of a pass: whatever is still on ends with it.
        if (phase + chunk >= pass && st.sounding.any())
        {
            for (int p = 0; p < 128; ++p)
                if (st.sounding[(size_t) p])
                    push (ev, { juce::jmin (i, seg.to - 1), NoteEvent::noteOff, p, 0.0f });
            st.sounding.reset();
        }
    }
}

bool LoopPlayer::mixMidi (const daw::TrackPlayback& tp, std::vector<NoteEvent>& ev) const
{
    auto& st = tp.runtime->loop;
    if (st.numSegments == 0)
        return false;

    // The arrange is silent wherever a cell plays (or records).
    auto overridden = [&st] (int offset)
    {
        for (int i = 0; i < st.numSegments; ++i)
        {
            const auto& seg = st.segments[(size_t) i];
            if (offset >= seg.from && offset < seg.to)
                return seg.column != TrackLoopState::none;
        }
        return false;
    };
    ev.erase (std::remove_if (ev.begin(), ev.end(), [&] (const NoteEvent& e) { return e.kind != NoteEvent::allNotesOff && overridden (e.offset); }),
              ev.end());

    const auto before = ev.size();
    int nextSwitch = 0;
    auto applySwitch = [&] (const TrackLoopState::Switch& s)
    {
        if (s.fromCell)
        {
            // Away from a cell: a note-off for every note it has on.
            for (int p = 0; p < 128; ++p)
                if (st.sounding[(size_t) p])
                    push (ev, { s.at, NoteEvent::noteOff, p, 0.0f });
            st.sounding.reset();
        }
        else if (s.toCell)
            push (ev, { s.at, NoteEvent::allNotesOff, 0, 0.0f });     // from the arrange: its notes stop
    };

    for (int i = 0; i < st.numSegments; ++i)
    {
        const auto& seg = st.segments[(size_t) i];
        while (nextSwitch < st.numSwitches && st.switches[(size_t) nextSwitch].at <= seg.from)
            applySwitch (st.switches[(size_t) nextSwitch++]);
        if (seg.column == TrackLoopState::none || seg.recording)
            continue;
        const CellPlayback* cell = seg.fallback ? &fallbackCell
                                 : juce::isPositiveAndBelow (seg.column, (int) tp.cells.size()) ? &tp.cells[(size_t) seg.column] : nullptr;
        if (cell != nullptr && cell->isValid())
            emitCell (*cell, seg, ev, st);
    }
    while (nextSwitch < st.numSwitches)
        applySwitch (st.switches[(size_t) nextSwitch++]);

    bool started = false;
    for (auto i = before; i < ev.size(); ++i)
        started = started || ev[i].kind == NoteEvent::noteOn;
    if (ev.size() != before)
        std::sort (ev.begin(), ev.end(), [] (const NoteEvent& x, const NoteEvent& y)
                   { return x.offset != y.offset ? x.offset < y.offset : priority (x) < priority (y); });
    return started;
}

void LoopPlayer::captureAudio (const daw::TrackRuntime& rt, const float* input, int n)
{
    const auto state = getRecordState();
    if (rt.id != recordTrack || recordMidi || input == nullptr || recordBuffer == nullptr
        || (state != RecordState::recording && state != RecordState::tail))
        return;

    const auto length = recordLength.load();
    const auto limit = juce::jmin ((juce::int64) recordBuffer->getNumSamples(), length >= 0 ? length + recordLatency : std::numeric_limits<juce::int64>::max());
    float* dest = recordBuffer->getWritePointer (0);
    for (int i = captureFrom; i < n; ++i)
    {
        const auto at = written + (i - captureFrom);
        if (at >= limit)
            break;
        dest[at] = input[i];
    }
}

void LoopPlayer::captureMidi (const daw::TrackRuntime& rt, const NoteEvent* ev, int count)
{
    const auto state = getRecordState();
    if (rt.id != recordTrack || ! recordMidi || (state != RecordState::recording && state != RecordState::tail))
        return;

    const auto length = recordLength.load();
    for (int i = 0; i < count; ++i)
    {
        const auto& e = ev[i];
        if (e.offset < captureFrom || (e.kind != NoteEvent::noteOn && e.kind != NoteEvent::noteOff) || ! juce::isPositiveAndBelow (e.note, 128))
            continue;
        const auto at = written + (e.offset - captureFrom);
        if (length >= 0 && at >= length)
            continue;
        if (fallbackCell.midi.size() + 1 >= fallbackCell.midi.capacity())
            break;
        if (e.kind == NoteEvent::noteOn)
        {
            fallbackCell.midi.push_back ({ at, true, (std::uint8_t) e.note, e.value });
            held.set ((size_t) e.note);
        }
        else if (held[(size_t) e.note])
        {
            fallbackCell.midi.push_back ({ at, false, (std::uint8_t) e.note, 0.0f });
            held.reset ((size_t) e.note);
        }
    }
}

} // namespace woodshed::loops
