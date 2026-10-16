#include "ScoreLayout.h"
#include <map>

namespace woodshed::score
{

namespace
{
constexpr int naturalPc[7] { 0, 2, 4, 5, 7, 9, 11 };
constexpr int sharpOrder[7] { 3, 0, 4, 1, 5, 2, 6 };     // F C G D A E B
constexpr int flatOrder[7]  { 6, 2, 5, 1, 4, 0, 3 };     // B E A D G C F

int floorDiv (int a, int b)                        { return a >= 0 ? a / b : -((-a + b - 1) / b); }
juce::int64 floorDiv64 (juce::int64 a, juce::int64 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
int mod (int a, int b)                             { return ((a % b) + b) % b; }

bool isDiatonic (int pitch, int keyFifths)
{
    const auto s = spell (pitch, keyFifths);
    return s.alter == keyAlter (mod (s.step, 7), keyFifths);
}

//==============================================================================
/** The meter's rhythm rules. */
struct Meter
{
    juce::int64 bar = wholeNote, beat = quarter, beam = quarter;
    bool compound = false, evenSimple = false;

    explicit Meter (const Options& o)
    {
        const int unit = juce::jlimit (1, 64, o.beatUnit);
        const int beats = juce::jmax (1, o.beatsPerBar);
        bar = (juce::int64) beats * wholeNote / unit;
        compound = unit == 8 && beats % 3 == 0;
        beat = compound ? quarter * 3 / 2 : (juce::int64) wholeNote / unit;
        beam = compound ? beat : unit >= 8 ? quarter : beat;
        evenSimple = ! compound && beats % 2 == 0 && beats >= 4;
        if (bar % beat != 0)
            beat = bar;
        if (bar % beam != 0)
            beam = beat;
    }
};

struct Value
{
    int ticks, base, dots;
    bool tuplet;
};

constexpr Value values[] {
    { 3840, 3840, 0, false }, { 2880, 1920, 1, false }, { 1920, 1920, 0, false }, { 1440, 960, 1, false },
    { 960, 960, 0, false }, { 720, 480, 1, false }, { 640, 960, 0, true }, { 480, 480, 0, false },
    { 360, 240, 1, false }, { 320, 480, 0, true }, { 240, 240, 0, false }, { 160, 240, 0, true }, { 120, 120, 0, false }
};

struct Piece
{
    juce::int64 offset, ticks;
    int base, dots;
    bool tuplet;
};

/** Is a value of `v` written at `s` (ticks into the bar) acceptable? */
bool allowed (const Meter& m, juce::int64 s, const Value& v, bool rest, bool straightOk)
{
    const juce::int64 d = v.ticks;
    if (s + d > m.bar)
        return false;
    if (s == 0 && d == m.bar && ! v.tuplet)
        return true;

    if (v.tuplet)
        return s % 160 == 0 && (s % m.beat) + d <= m.beat;
    if (! straightOk)
        return false;

    if (d < m.beat)
    {
        if ((s % m.beat) + d > m.beat)
            return false;
        return ! rest || s % (v.dots > 0 ? v.ticks : v.base) == 0;
    }

    if (s % m.beat != 0)
        return false;

    if (rest)
    {
        if (s % (m.compound && v.dots > 0 ? m.beat : (juce::int64) v.base) != 0)
            return false;
        if (! m.compound && v.dots > 0)
            return false;
        if (m.evenSimple && s < m.bar / 2 && s + d > m.bar / 2)
            return false;
        return true;
    }

    if (m.evenSimple && s < m.bar / 2 && s + d > m.bar / 2)
        return s == 0 || (v.dots == 0 && d == 2 * m.beat && s == m.beat);
    return true;
}

/** Splits [s, e) (ticks into the bar) into written values. */
std::vector<Piece> decompose (const Meter& m, juce::int64 s, juce::int64 e, bool rest, bool triplets)
{
    std::vector<Piece> out;
    while (s < e)
    {
        const bool straightOk = s % 120 == 0 && (e - s) % 120 == 0;
        const Value* chosen = nullptr;
        for (auto& v : values)
            if ((triplets || ! v.tuplet) && v.ticks <= e - s && allowed (m, s, v, rest, straightOk))
            {
                chosen = &v;
                break;
            }
        if (chosen == nullptr)      // nothing fits the rules: the largest value that fits at all
            for (auto& v : values)
                if ((triplets || ! v.tuplet) && v.ticks <= e - s)
                {
                    chosen = &v;
                    break;
                }
        if (chosen == nullptr)
        {
            out.push_back ({ s, e - s, 120, 0, false });
            break;
        }
        out.push_back ({ s, chosen->ticks, chosen->base, chosen->dots, chosen->tuplet });
        s += chosen->ticks;
    }
    return out;
}

struct Chord
{
    juce::int64 start = 0, end = 0;
    std::vector<Notehead> notes;
};
} // namespace

//==============================================================================
int keyAlter (int letter, int keyFifths)
{
    letter = mod (letter, 7);
    for (int i = 0; i < juce::jmin (7, keyFifths); ++i)
        if (sharpOrder[i] == letter)
            return 1;
    for (int i = 0; i < juce::jmin (7, -keyFifths); ++i)
        if (flatOrder[i] == letter)
            return -1;
    return 0;
}

Spelling spell (int pitch, int keyFifths)
{
    const int pc = mod (pitch, 12);
    int letter = -1, alter = 0;

    for (int l = 0; l < 7 && letter < 0; ++l)
        if (mod (naturalPc[l] + keyAlter (l, keyFifths), 12) == pc)
        {
            letter = l;
            alter = keyAlter (l, keyFifths);
        }
    for (int l = 0; l < 7 && letter < 0; ++l)
        if (naturalPc[l] == pc)
            letter = l;
    for (int l = 0; l < 7 && letter < 0; ++l)
        if (mod (naturalPc[l] + (keyFifths >= 0 ? 1 : -1), 12) == pc)
        {
            letter = l;
            alter = keyFifths >= 0 ? 1 : -1;
        }

    const int naturalPitch = pitch - alter;
    const int octave = floorDiv (naturalPitch, 12) - 1;
    return { octave * 7 + juce::jmax (0, letter), alter };
}

int pitchForStep (int step, int keyFifths)
{
    const int letter = mod (step, 7), octave = floorDiv (step, 7);
    return (octave + 1) * 12 + naturalPc[letter] + keyAlter (letter, keyFifths);
}

int scaleStep (int pitch, int keyFifths, int direction)
{
    direction = direction >= 0 ? 1 : -1;
    int p = pitch + direction;
    while (p > 0 && p < 127 && ! isDiatonic (p, keyFifths))
        p += direction;
    return juce::jlimit (0, 127, p);
}

juce::String keyName (int keyFifths)
{
    static const char* const names[] { "Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#" };
    return names[juce::jlimit (-7, 7, keyFifths) + 7];
}

DrumPosition drumPosition (int pitch)
{
    switch (pitch)
    {
        case 35: case 36:                   return { 31, false };   // kick: F4
        case 38: case 40:                   return { 35, false };   // snare: C5
        case 37: case 39:                   return { 35, true };    // side stick, clap
        case 41: case 43:                   return { 33, false };   // floor tom: A4
        case 45: case 47:                   return { 36, false };   // mid tom: D5
        case 48: case 50:                   return { 37, false };   // high tom: E5
        case 42: case 46:                   return { 39, true };    // hi-hat: G5
        case 44:                            return { 29, true };    // pedal hi-hat: D4
        case 49: case 52: case 55: case 57: return { 40, true };    // crash: A5
        case 51: case 53: case 59:          return { 38, true };    // ride: F5
        default:                            return { 37, true };    // percussion: E5
    }
}

int drumPitchForStep (int step)
{
    switch (step)
    {
        case 29: return 44;
        case 31: return 36;
        case 33: return 41;
        case 35: return 38;
        case 36: return 45;
        case 37: return 48;
        case 38: return 51;
        case 39: return 42;
        case 40: return 49;
        default: return step < 31 ? 36 : step > 40 ? 49 : 38;
    }
}

int topLineStep (Clef c)
{
    switch (c)
    {
        case Clef::bass:        return 26;      // A3
        case Clef::treble:
        case Clef::percussion:
        default:                return 38;      // F5
    }
}

int Element::flags() const
{
    if (rest || base >= quarter)
        return 0;
    return base >= 480 ? 1 : base >= 240 ? 2 : 3;
}

std::vector<const Element*> Layout::on (int staff, int barIndex) const
{
    std::vector<const Element*> list;
    for (auto& e : elements)
        if (e.staff == staff && e.bar == barIndex)
            list.push_back (&e);
    return list;
}

int Layout::barAt (juce::int64 tick) const
{
    if (bars.empty())
        return 0;
    const auto index = floorDiv64 (tick - bars.front().start, barLength);
    return (int) juce::jlimit ((juce::int64) 0, (juce::int64) bars.size() - 1, index);
}

//==============================================================================
Layout layout (const std::vector<InputNote>& input, const Options& o)
{
    Layout out;
    const Meter m (o);
    out.keyFifths = o.percussion ? 0 : juce::jlimit (-7, 7, o.keyFifths);
    out.beatsPerBar = o.beatsPerBar;
    out.beatUnit = o.beatUnit;
    out.barLength = m.bar;
    const juce::int64 res = juce::jmax (1, o.resolution);
    const bool triplets = res % 120 != 0;

    // Quantize both ends for display; drop what lies outside the range.
    struct Q { InputNote note; juce::int64 start, end; };
    std::vector<Q> notes;
    juce::int64 last = 0;
    for (auto& n : input)
    {
        auto s = (juce::int64) std::llround ((double) n.start / (double) res) * res;
        auto e = (juce::int64) std::llround ((double) (n.start + n.length) / (double) res) * res;
        if (e <= s)
            e = s + res;
        s = juce::jmax (s, o.from);
        if (o.to >= 0)
            e = juce::jmin (e, o.to);
        if (e <= s)
            continue;
        notes.push_back ({ n, s, e });
        last = juce::jmax (last, e);
    }

    // Staves.
    if (o.percussion)
        out.staves = { Clef::percussion };
    else
    {
        auto mode = o.staff;
        if (mode == StaffMode::automatic)
        {
            int low = 127, high = 0;
            for (auto& q : notes)
            {
                low = juce::jmin (low, q.note.pitch);
                high = juce::jmax (high, q.note.pitch);
            }
            mode = low > high || low >= 55 ? StaffMode::treble : high <= 64 ? StaffMode::bass : StaffMode::grand;
        }
        out.staves = mode == StaffMode::grand ? std::vector<Clef> { Clef::treble, Clef::bass }
                   : mode == StaffMode::bass  ? std::vector<Clef> { Clef::bass }
                                              : std::vector<Clef> { Clef::treble };
    }
    const int numStaves = (int) out.staves.size();

    // Bars.
    const juce::int64 to = o.to >= 0 ? o.to : juce::jmax (last, o.from + 1);
    const juce::int64 firstBar = floorDiv64 (juce::jmax ((juce::int64) 0, o.from), m.bar);
    const juce::int64 endBar = juce::jmax (firstBar + 1, floorDiv64 (to + m.bar - 1, m.bar));
    for (auto b = firstBar; b < endBar; ++b)
        out.bars.push_back ({ b * m.bar, m.bar, (int) b + 1 });
    const juce::int64 layoutEnd = endBar * m.bar;

    for (int staff = 0; staff < numStaves; ++staff)
    {
        // Chords: the notes on this staff that start together.
        std::map<juce::int64, Chord> byStart;
        for (auto& q : notes)
        {
            const bool here = numStaves == 1 || (staff == 0 ? q.note.pitch >= 60 : q.note.pitch < 60);
            if (! here)
                continue;
            auto& c = byStart[q.start];
            c.start = q.start;
            c.end = juce::jmax (c.end, q.end);

            Notehead h;
            h.pitch = q.note.pitch;
            h.id = q.note.id;
            if (o.percussion)
            {
                const auto d = drumPosition (q.note.pitch);
                h.step = d.step;
                h.cross = d.cross;
            }
            else
            {
                const auto sp = spell (q.note.pitch, out.keyFifths);
                h.step = sp.step;
                h.alter = sp.alter;
            }
            const bool duplicate = std::any_of (c.notes.begin(), c.notes.end(), [&] (const Notehead& x)
                                                { return x.pitch == h.pitch || (o.percussion && x.step == h.step); });
            if (! duplicate)
                c.notes.push_back (h);
        }

        std::vector<Chord> chords;
        for (auto& [start, c] : byStart)
        {
            std::sort (c.notes.begin(), c.notes.end(), [] (auto& a, auto& b) { return a.step != b.step ? a.step < b.step : a.pitch < b.pitch; });
            chords.push_back (c);
        }
        for (size_t i = 0; i < chords.size(); ++i)
        {
            auto& c = chords[i];
            const juce::int64 next = i + 1 < chords.size() ? chords[i + 1].start : layoutEnd;
            if (o.interpret)
            {
                const auto beatEnd = (floorDiv64 (c.end - 1, m.beat) + 1) * m.beat;
                c.end = juce::jmax (c.end, beatEnd);
            }
            c.end = juce::jmin (c.end, next);
            if (o.to >= 0 && ! o.interpret)
                c.end = juce::jmin (c.end, juce::jmax (c.start + res, o.to));
        }

        // Bar by bar: chords and the rests between them, split into written values.
        size_t ci = 0;
        for (int bi = 0; bi < (int) out.bars.size(); ++bi)
        {
            const auto barStart = out.bars[(size_t) bi].start, barEnd = barStart + m.bar;
            while (ci < chords.size() && chords[ci].end <= barStart)
                ++ci;

            std::vector<const Chord*> inBar;
            for (size_t k = ci; k < chords.size() && chords[k].start < barEnd; ++k)
                inBar.push_back (&chords[k]);

            if (inBar.empty())
            {
                Element e;
                e.staff = staff;
                e.bar = bi;
                e.start = barStart;
                e.duration = m.bar;
                e.base = wholeNote;
                e.rest = e.barRest = true;
                out.elements.push_back (e);
                continue;
            }

            auto emit = [&] (juce::int64 s, juce::int64 e, const Chord* chord)
            {
                const auto pieces = decompose (m, s - barStart, e - barStart, chord == nullptr, triplets);
                for (size_t p = 0; p < pieces.size(); ++p)
                {
                    Element el;
                    el.staff = staff;
                    el.bar = bi;
                    el.start = barStart + pieces[p].offset;
                    el.duration = pieces[p].ticks;
                    el.base = pieces[p].base;
                    el.dots = pieces[p].dots;
                    el.tuplet = pieces[p].tuplet;
                    el.rest = chord == nullptr;
                    if (chord != nullptr)
                    {
                        el.notes = chord->notes;
                        const bool first = el.start == chord->start;
                        const bool lastPiece = el.start + el.duration >= chord->end;
                        for (auto& h : el.notes)
                        {
                            h.tiedFromPrevious = ! first;
                            h.tieToNext = ! lastPiece;
                        }
                    }
                    out.elements.push_back (el);
                }
            };

            juce::int64 at = barStart;
            for (auto* c : inBar)
            {
                const auto s = juce::jmax (c->start, barStart), e = juce::jmin (c->end, barEnd);
                if (s > at)
                    emit (at, s, nullptr);
                if (e > s)
                    emit (s, e, c);
                at = juce::jmax (at, e);
            }
            if (at < barEnd)
                emit (at, barEnd, nullptr);
        }
    }

    // Accidentals, carried through each bar on each staff (per written step), reset at bar lines.
    if (! o.percussion)
    {
        int currentStaff = -1, currentBar = -1;
        std::map<int, int> carried;
        for (auto& e : out.elements)
        {
            if (e.staff != currentStaff || e.bar != currentBar)
            {
                currentStaff = e.staff;
                currentBar = e.bar;
                carried.clear();
            }
            for (auto& h : e.notes)
            {
                if (h.tiedFromPrevious)
                    continue;
                const auto it = carried.find (h.step);
                const int now = it != carried.end() ? it->second : keyAlter (mod (h.step, 7), out.keyFifths);
                if (h.alter != now)
                    h.accidental = h.alter > 0 ? Accidental::sharp : h.alter < 0 ? Accidental::flat : Accidental::natural;
                carried[h.step] = h.alter;
            }
        }
    }

    // Stems: away from the note furthest from the middle line (drums: always up).
    auto stemUpFor = [&] (int staff, int lowest, int highest)
    {
        if (out.staves[(size_t) staff] == Clef::percussion)
            return true;
        const int middle = middleLineStep (out.staves[(size_t) staff]);
        return middle - lowest > highest - middle;
    };
    for (auto& e : out.elements)
        if (! e.notes.empty())
            e.stemUp = stemUpFor (e.staff, e.notes.front().step, e.notes.back().step);

    // Beams: eighths and shorter within a beat (a dotted-quarter group in compound time).
    for (size_t i = 0; i < out.elements.size();)
    {
        auto& first = out.elements[i];
        if (first.rest || first.flags() == 0)
        {
            ++i;
            continue;
        }
        const auto group = floorDiv64 (first.start - out.bars[(size_t) first.bar].start, m.beam);
        size_t j = i + 1;
        while (j < out.elements.size())
        {
            auto& e = out.elements[j];
            if (e.staff != first.staff || e.bar != first.bar || e.rest || e.flags() == 0
                || floorDiv64 (e.start - out.bars[(size_t) e.bar].start, m.beam) != group)
                break;
            ++j;
        }
        if (j - i >= 2)
        {
            Beam b;
            b.staff = first.staff;
            int lowest = 1000, highest = -1000;
            for (size_t k = i; k < j; ++k)
            {
                b.elements.push_back ((int) k);
                lowest = juce::jmin (lowest, out.elements[k].notes.front().step);
                highest = juce::jmax (highest, out.elements[k].notes.back().step);
            }
            b.stemUp = stemUpFor (first.staff, lowest, highest);
            for (size_t k = i; k < j; ++k)
            {
                out.elements[k].beam = (int) out.beams.size();
                out.elements[k].stemUp = b.stemUp;
            }
            out.beams.push_back (b);
        }
        i = j;
    }

    return out;
}

//==============================================================================
juce::String stepName (const Notehead& h)
{
    static const char* const letters[] { "C", "D", "E", "F", "G", "A", "B" };
    return juce::String (letters[mod (h.step, 7)]) + (h.alter > 0 ? "#" : h.alter < 0 ? "b" : "")
         + juce::String (floorDiv (h.step, 7));
}

juce::String describe (const Layout& l, int staff, int barIndex, bool withPitches)
{
    juce::StringArray parts;
    for (auto* e : l.on (staff, barIndex))
    {
        if (e->barRest)
        {
            parts.add ("W");
            continue;
        }
        juce::String s = e->rest ? "r" : "";
        switch (e->base)
        {
            case 3840: s << "w"; break;
            case 1920: s << "h"; break;
            case 960:  s << "q"; break;
            case 480:  s << "8"; break;
            case 240:  s << "16"; break;
            default:   s << "32"; break;
        }
        for (int d = 0; d < e->dots; ++d)
            s << ".";
        if (e->tuplet)
            s << "t";
        if (withPitches && ! e->notes.empty())
        {
            juce::StringArray names;
            for (auto& h : e->notes)
                names.add (stepName (h));
            s << "[" << names.joinIntoString (" ") << "]";
        }
        if (! e->notes.empty() && e->notes.front().tieToNext)
            s << "~";
        parts.add (s);
    }
    return parts.joinIntoString (" ");
}

} // namespace woodshed::score
