#include "Flex.h"

namespace woodshed::flex
{

juce::String modeId (Mode m)
{
    switch (m)
    {
        case Mode::monophonic: return "mono";
        case Mode::rhythmic:   return "rhythmic";
        case Mode::speed:      return "speed";
        case Mode::polyphonic:
        default:               return "poly";
    }
}

juce::String modeName (Mode m)
{
    switch (m)
    {
        case Mode::monophonic: return "Monophonic";
        case Mode::rhythmic:   return "Rhythmic (slicing)";
        case Mode::speed:      return "Speed (varispeed)";
        case Mode::polyphonic:
        default:               return "Polyphonic";
    }
}

Mode modeFromId (const juce::String& id)
{
    if (id == "mono")     return Mode::monophonic;
    if (id == "rhythmic") return Mode::rhythmic;
    if (id == "speed")    return Mode::speed;
    return Mode::polyphonic;
}

Mode modeOf (const juce::ValueTree& track)
{
    return modeFromId (track[ids::flexMode].toString());
}

//==============================================================================
bool WarpMap::isIdentity() const
{
    for (auto& m : markers)
        if (std::abs (m.src - m.dst) > 0.5)
            return false;
    return true;
}

double WarpMap::toDst (double src) const
{
    const auto n = markers.size();
    if (n == 0) return src;
    if (n == 1) return src + markers[0].dst - markers[0].src;

    // The segment the sample is in (the edge ones carry on past the ends).
    size_t i = 1;
    while (i < n - 1 && markers[i].src <= src)
        ++i;
    const auto& a = markers[i - 1];
    const auto& b = markers[i];
    return a.dst + (src - a.src) * (b.dst - a.dst) / (b.src - a.src);
}

double WarpMap::toSrc (double dst) const
{
    const auto n = markers.size();
    if (n == 0) return dst;
    if (n == 1) return dst - markers[0].dst + markers[0].src;

    size_t i = 1;
    while (i < n - 1 && markers[i].dst <= dst)
        ++i;
    const auto& a = markers[i - 1];
    const auto& b = markers[i];
    return a.src + (dst - a.dst) * (b.src - a.src) / (b.dst - a.dst);
}

double WarpMap::ratioAt (double src) const
{
    const auto n = markers.size();
    if (n < 2) return 1.0;
    size_t i = 1;
    while (i < n - 1 && markers[i].src <= src)
        ++i;
    return (markers[i].dst - markers[i - 1].dst) / (markers[i].src - markers[i - 1].src);
}

void WarpMap::tidy()
{
    std::sort (markers.begin(), markers.end(), [] (const Marker& a, const Marker& b) { return a.src < b.src; });
    std::vector<Marker> kept;
    for (auto& m : markers)
        if (kept.empty() || (m.src > kept.back().src + 1.0 && m.dst > kept.back().dst + 1.0))
            kept.push_back (m);
    markers = std::move (kept);
}

WarpMap WarpMap::decode (const juce::String& text)
{
    WarpMap w;
    for (auto& item : juce::StringArray::fromTokens (text, ";", {}))
        if (item.containsChar (':'))
            w.markers.push_back ({ item.upToFirstOccurrenceOf (":", false, false).getDoubleValue(),
                                   item.fromFirstOccurrenceOf (":", false, false).getDoubleValue() });
    w.tidy();
    return w;
}

juce::String WarpMap::encode() const
{
    juce::StringArray parts;
    for (auto& m : markers)
        parts.add (juce::String (m.src, 1) + ":" + juce::String (m.dst, 1));
    return parts.joinIntoString (";");
}

WarpMap warpOf (const juce::ValueTree& region)
{
    return WarpMap::decode (region[ids::flexMarkers].toString());
}

void setWarp (juce::ValueTree region, const WarpMap& w, juce::UndoManager* undo)
{
    if (w.markers.empty())
        region.removeProperty (ids::flexMarkers, undo);
    else
        region.setProperty (ids::flexMarkers, w.encode(), undo);
}

//==============================================================================
bool PitchNote::isEdited() const
{
    return std::abs (shift) > 0.001f || std::abs (drift - 1.0f) > 0.001f || std::abs (vibrato - 1.0f) > 0.001f
        || std::abs (gain) > 0.01f || std::abs (formant) > 0.001f;
}

juce::String encodeNotes (const std::vector<PitchNote>& notes)
{
    juce::StringArray parts;
    for (auto& n : notes)
        parts.add (juce::String (n.start, 0) + ":" + juce::String (n.end, 0) + ":" + juce::String (n.pitch, 3) + ":"
                   + juce::String (n.shift, 3) + ":" + juce::String (n.drift, 3) + ":" + juce::String (n.vibrato, 3) + ":"
                   + juce::String (n.gain, 2) + ":" + juce::String (n.formant, 3));
    return parts.joinIntoString (";");
}

std::vector<PitchNote> decodeNotes (const juce::String& text)
{
    std::vector<PitchNote> notes;
    for (auto& item : juce::StringArray::fromTokens (text, ";", {}))
    {
        const auto f = juce::StringArray::fromTokens (item, ":", {});
        if (f.size() < 3)
            continue;
        PitchNote n;
        n.start = f[0].getDoubleValue();
        n.end = f[1].getDoubleValue();
        n.pitch = f[2].getFloatValue();
        if (f.size() >= 8)
        {
            n.shift = f[3].getFloatValue();
            n.drift = f[4].getFloatValue();
            n.vibrato = f[5].getFloatValue();
            n.gain = f[6].getFloatValue();
            n.formant = f[7].getFloatValue();
        }
        if (n.end > n.start)
            notes.push_back (n);
    }
    std::sort (notes.begin(), notes.end(), [] (auto& a, auto& b) { return a.start < b.start; });
    return notes;
}

std::vector<PitchNote> notesOf (const juce::ValueTree& region)
{
    return decodeNotes (region[ids::pitchNotes].toString());
}

void setNotes (juce::ValueTree region, const std::vector<PitchNote>& notes, juce::UndoManager* undo)
{
    if (notes.empty())
        region.removeProperty (ids::pitchNotes, undo);
    else
        region.setProperty (ids::pitchNotes, encodeNotes (notes), undo);
}

bool isProcessed (const juce::ValueTree& region)
{
    if (! (bool) region.getParent()[ids::flexOn] || region[juce::Identifier ("kind")].toString().isNotEmpty())
        return false;       // MIDI regions and take folders aren't stretched
    if (! warpOf (region).isIdentity())
        return true;
    for (auto& n : notesOf (region))
        if (n.isEdited())
            return true;
    return false;
}

} // namespace woodshed::flex
