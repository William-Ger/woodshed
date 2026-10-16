#include "MidiFiles.h"

namespace woodshed::daw::midifile
{

bool read (const juce::File& file, Contents& out)
{
    juce::FileInputStream in (file);
    juce::MidiFile midi;
    if (! in.openedOk() || ! midi.readFrom (in, true))
        return false;

    const int ppq = midi.getTimeFormat() > 0 ? midi.getTimeFormat() : 480;
    const double scale = (double) ticksPerQuarter / (double) ppq;
    out = {};

    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        const auto* seq = midi.getTrack (t);
        juce::String trackName;
        std::map<int, Part> byChannel;

        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& m = seq->getEventPointer (i)->message;
            const auto when = (juce::int64) std::llround (m.getTimeStamp() * scale);

            if (m.isTempoMetaEvent() && out.tempo <= 0.0)
                out.tempo = 60.0 / m.getTempoSecondsPerQuarterNote();
            else if (m.isTimeSignatureMetaEvent() && out.beatsPerBar == 0)
                m.getTimeSignatureInfo (out.beatsPerBar, out.beatUnit);
            else if (m.isTrackNameEvent())
                trackName = m.getTextFromTextMetaEvent();
            else if (m.isProgramChange())
            {
                auto& part = byChannel[m.getChannel()];
                if (part.notes.empty())
                    part.program = m.getProgramChangeNumber();
            }
            else if (m.isNoteOn())
            {
                auto& part = byChannel[m.getChannel()];
                part.channel = m.getChannel();
                const auto* off = seq->getEventPointer (i)->noteOffObject;
                const auto end = off != nullptr ? (juce::int64) std::llround (off->message.getTimeStamp() * scale) : when + ticksPerQuarter;
                part.notes.push_back ({ m.getNoteNumber(), juce::jlimit (1, 127, (int) m.getVelocity()), when, juce::jmax ((juce::int64) 1, end - when) });
                part.endTicks = juce::jmax (part.endTicks, end);
            }
        }

        for (auto& [channel, part] : byChannel)
        {
            if (part.notes.empty())
                continue;
            part.channel = channel;
            part.name = trackName.isNotEmpty() ? trackName : channel == 10 ? juce::String ("Drums") : "Track " + juce::String (out.parts.size() + 1);
            if (byChannel.size() > 1 && trackName.isNotEmpty())
                part.name << " " << channel;
            out.parts.push_back (std::move (part));
        }
    }
    return true;
}

bool write (const Project& project, const juce::Array<juce::ValueTree>& regions, const juce::File& file)
{
    const auto grid = project.grid();
    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (ticksPerQuarter);

    juce::MidiMessageSequence conductor;
    conductor.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::llround (60000000.0 / grid.tempo)), 0.0);
    conductor.addEvent (juce::MidiMessage::timeSignatureMetaEvent (grid.beatsPerBar, grid.beatUnit), 0.0);
    midi.addTrack (conductor);

    int channel = 1;
    for (auto& region : regions)
    {
        if (! Project::isMidiRegion (region))
            continue;
        const auto start = (juce::int64) region[ids::start];
        const auto end = start + (juce::int64) region[ids::length];
        const double originTicks = grid.toTicks ((double) Project::contentOrigin (region));
        const auto endTicks = grid.toTicks ((double) end);
        const auto startTicks = grid.toTicks ((double) start);
        const bool drums = region.getParent().getChildWithName (ids::INSTRUMENT)[ids::type].toString().startsWith ("drums");
        const int ch = drums ? 10 : channel;

        juce::MidiMessageSequence seq;
        auto name = juce::MidiMessage::textMetaEvent (3, region.getParent()[ids::name].toString());
        seq.addEvent (name, 0.0);
        for (auto n : region)
        {
            if (! n.hasType (ids::NOTE))
                continue;
            const auto note = Project::noteFrom (n);
            const double on = originTicks + (double) note.start;
            if (on < startTicks - 0.5 || on >= endTicks)
                continue;
            const double off = juce::jmin (endTicks, on + (double) note.length);
            seq.addEvent (juce::MidiMessage::noteOn (ch, note.pitch, (juce::uint8) note.velocity), on);
            seq.addEvent (juce::MidiMessage::noteOff (ch, note.pitch), off);
        }
        seq.updateMatchedPairs();
        midi.addTrack (seq);
        if (! drums)
            channel = channel % 16 + 1 == 10 ? 11 : channel % 16 + 1;
    }

    file.deleteFile();
    juce::FileOutputStream out (file);
    return out.openedOk() && midi.writeTo (out);
}

juce::ValueTree addPart (Project& project, juce::ValueTree track, const Part& part, juce::int64 at)
{
    const auto grid = project.grid();
    const auto bar = grid.samplesPerBar();
    const auto lengthSamples = (juce::int64) std::ceil (grid.fromTicks ((double) part.endTicks) / bar) * (juce::int64) std::llround (bar);
    auto region = project.addMidiRegion (track, at, juce::jmax ((juce::int64) std::llround (bar), lengthSamples), part.name);
    for (auto& n : part.notes)
        project.addNote (region, n);
    return region;
}

} // namespace woodshed::daw::midifile
