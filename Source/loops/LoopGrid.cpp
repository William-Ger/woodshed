#include "LoopGrid.h"

namespace woodshed::loops
{

namespace dids = daw::ids;

LoopGrid::LoopGrid (daw::Engine& e) : engine (e)
{
    events.reserve (256);
    startTimerHz (30);
}

LoopGrid::~LoopGrid()
{
    stopTimer();
}

void LoopGrid::startTransportIfStopped()
{
    if (engine.isPlaying() || project() == nullptr)
        return;

    // From the start of the current bar, so the first launch lands as the transport starts.
    const auto& grid = project()->grid();
    const auto bar = (int) std::floor (grid.barAt (grid.tickAt ((double) engine.getPosition())) + 1.0e-9);
    engine.setPosition (grid.barStartSample (juce::jmax (0, bar)));
    engine.play();
}

void LoopGrid::launchCell (int trackId, int column)
{
    auto* p = project();
    if (p == nullptr)
        return;
    const auto cell = cellAt (p->trackById (trackId), column);
    if (! cell.isValid())
        return;

    startTransportIfStopped();
    LoopCommand c;
    c.kind = LoopCommand::launch;
    c.trackId = trackId;
    c.column = column;
    c.quantize = quantizeOf (cell, p->state);
    engine.getLoopPlayer().send (c);
}

void LoopGrid::stopTrack (int trackId)
{
    if (project() == nullptr || ! engine.isPlaying())
        return;
    LoopCommand c;
    c.kind = LoopCommand::stop;
    c.trackId = trackId;
    c.quantize = gridQuantize (project()->state);
    engine.getLoopPlayer().send (c);
}

void LoopGrid::launchScene (int column)
{
    if (project() == nullptr)
        return;
    startTransportIfStopped();
    LoopCommand c;
    c.kind = LoopCommand::launchScene;
    c.column = column;
    c.quantize = gridQuantize (project()->state);
    engine.getLoopPlayer().send (c);
}

void LoopGrid::stopAll()
{
    if (project() == nullptr || ! engine.isPlaying())
        return;
    LoopCommand c;
    c.kind = LoopCommand::stopAll;
    c.quantize = gridQuantize (project()->state);
    engine.getLoopPlayer().send (c);
}

bool LoopGrid::recordCell (int trackId, int column, int bars)
{
    auto* p = project();
    if (p == nullptr)
        return false;
    const auto track = p->trackById (trackId);
    if (! track.isValid() || daw::Project::isAuxTrack (track) || cellAt (track, column).isValid())
        return false;

    const auto& grid = p->grid();
    const bool midi = daw::Project::isInstrumentTrack (track);
    startTransportIfStopped();
    return engine.getLoopPlayer().startRecording (trackId, column, gridQuantize (p->state), bars, midi,
                                                  engine.getRecordLatencySamples(), grid.sampleRate, grid.samplesPerBar());
}

void LoopGrid::finishRecording()
{
    LoopCommand c;
    c.kind = LoopCommand::finishRecord;
    engine.getLoopPlayer().send (c);
}

bool LoopGrid::isRecordingCell (int trackId, int column) const
{
    const auto& player = engine.getLoopPlayer();
    const auto s = player.getRecordState();
    return (s == LoopPlayer::RecordState::waiting || s == LoopPlayer::RecordState::recording)
           && player.getRecordTrack() == trackId && player.getRecordColumn() == column;
}

//==============================================================================
int LoopGrid::playingColumn (int trackId) const
{
    if (auto rt = engine.getRuntime (trackId))
        return rt->loop.shownColumn.load (std::memory_order_relaxed);
    return -1;
}

bool LoopGrid::isStopQueued (int trackId) const
{
    if (auto rt = engine.getRuntime (trackId))
        return rt->loop.shownNext.load (std::memory_order_relaxed) == -1;
    return false;
}

LoopGrid::Status LoopGrid::statusOf (int trackId, int column) const
{
    Status s;
    auto rt = engine.getRuntime (trackId);
    if (rt == nullptr)
        return s;

    const auto& st = rt->loop;
    const int playing = st.shownColumn.load (std::memory_order_relaxed);
    const int next = st.shownNext.load (std::memory_order_relaxed);
    const bool nextRecord = st.shownNextIsRecord.load (std::memory_order_relaxed);
    const auto length = st.shownLength.load (std::memory_order_relaxed);
    const auto elapsed = st.shownElapsed.load (std::memory_order_relaxed);

    if (playing == column)
    {
        if (st.shownRecording.load (std::memory_order_relaxed))   s.state = Status::recording;
        else if (next == column)                                  s.state = Status::queued;      // a restart
        else if (next != -2)                                      s.state = Status::stopping;
        else                                                      s.state = Status::playing;
        if (length > 0)
            s.progress = (float) (elapsed % length) / (float) length;
        else if (auto* p = project())
            s.progress = (float) std::fmod ((double) elapsed / p->grid().samplesPerBar(), 1.0);
    }
    else if (next == column)
        s.state = nextRecord ? Status::recordQueued : Status::queued;
    else if (isRecordingCell (trackId, column))
        s.state = Status::recordQueued;
    return s;
}

//==============================================================================
void LoopGrid::setPerformRecording (bool on)
{
    if (on == perform)
        return;
    perform = on;
    auto* p = project();
    if (p == nullptr)
        return;

    const auto now = engine.getPosition();
    if (on)
    {
        // What's already playing is recorded from here on.
        open.clear();
        for (int i = 0; i < p->numTracks(); ++i)
        {
            const int id = (int) p->track (i)[dids::id];
            if (const int column = playingColumn (id); column >= 0)
                open[id] = { column, now };
        }
    }
    else
    {
        service();
        bool began = false;
        std::vector<int> ids;
        for (auto& [id, o] : open)
            ids.push_back (id);
        for (int id : ids)
            close (id, now, began);
        open.clear();
    }
}

void LoopGrid::close (int trackId, juce::int64 at, bool& began)
{
    auto it = open.find (trackId);
    if (it == open.end())
        return;
    const auto o = it->second;
    open.erase (it);

    auto* p = project();
    const auto cell = p != nullptr ? cellAt (p->trackById (trackId), o.column) : juce::ValueTree();
    if (! cell.isValid() || at <= o.from)
        return;
    if (! began)
    {
        p->undo.beginNewTransaction ("Perform Recording");
        began = true;
    }
    placeInArrange (*p, cell, o.from, at);
}

void LoopGrid::service()
{
    if (project() != attached)
    {
        // Another project: whatever was in flight belonged to the old one.
        attached = project();
        open.clear();
        events.clear();
        engine.getLoopPlayer().discardRecording();
    }

    events.clear();
    engine.getLoopPlayer().takeEvents (events);
    bool changed = ! events.empty();

    if (perform && attached != nullptr)
    {
        bool began = false;
        for (auto& e : events)
        {
            close (e.trackId, e.at, began);
            if (e.column >= 0)
                open[e.trackId] = { e.column, e.at };
        }
    }

    if (engine.getLoopPlayer().getRecordState() == LoopPlayer::RecordState::finished)
    {
        commitRecording();
        changed = true;
    }

    if (changed && onChange)
        onChange();
}

void LoopGrid::commitRecording()
{
    auto& player = engine.getLoopPlayer();
    auto* p = project();
    auto track = p != nullptr ? p->trackById (player.getRecordTrack()) : juce::ValueTree();
    const auto length = player.getRecordedLength();
    if (! track.isValid() || length <= 0)
    {
        player.discardRecording();
        return;
    }

    const auto& grid = p->grid();
    const int bars = juce::jlimit (1, maxCellBars, (int) std::llround ((double) length / grid.samplesPerBar()));
    const int column = player.getRecordColumn();

    if (player.isMidiRecording())
    {
        // Pair each note-on with the next note-off of its pitch.
        const auto& stamps = player.getRecordedNotes();
        p->undo.beginNewTransaction ("Record Loop");
        auto cell = addMidiCell (*p, track, column, bars);
        auto region = contentOf (cell);
        for (size_t i = 0; i < stamps.size(); ++i)
        {
            if (! stamps[i].on)
                continue;
            auto end = length;
            for (size_t j = i + 1; j < stamps.size(); ++j)
                if (! stamps[j].on && stamps[j].pitch == stamps[i].pitch)
                {
                    end = stamps[j].time;
                    break;
                }
            daw::Note n;
            n.pitch = stamps[i].pitch;
            n.velocity = juce::jlimit (1, 127, juce::roundToInt (stamps[i].velocity * 127.0f));
            n.start = (juce::int64) std::llround (grid.toTicks ((double) stamps[i].time));
            n.length = juce::jmax ((juce::int64) 1, (juce::int64) std::llround (grid.toTicks ((double) (end - stamps[i].time))));
            p->addNote (region, n);
        }
        player.markCommitted();
        return;
    }

    // The take, with the latency it was recorded late by in front.
    const auto& audio = player.getRecordedAudio();
    const auto total = (int) juce::jmin ((juce::int64) audio.getNumSamples(), length + player.getRecordLatency());
    p->audioFolder().createDirectory();
    const auto file = p->audioFolder().getNonexistentChildFile (juce::File::createLegalFileName ("Loop " + track[dids::name].toString()), ".wav", false);
    bool ok = false;
    if (std::unique_ptr<juce::OutputStream> stream (file.createOutputStream()); stream != nullptr)
    {
        juce::WavAudioFormat wav;
        if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (grid.sampleRate)
                                                                                        .withNumChannels (1).withBitsPerSample (24)))
            ok = writer->writeFromAudioSampleBuffer (audio, 0, total);
    }
    if (! ok)
    {
        player.discardRecording();
        return;
    }

    p->undo.beginNewTransaction ("Record Loop");
    addAudioCell (*p, track, column, "Audio/" + file.getFileName(), player.getRecordLatency(), length, bars);
    player.markCommitted();
}

} // namespace woodshed::loops
