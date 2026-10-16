#include "StepPattern.h"
#include "Drummer.h"

namespace woodshed::daw
{

namespace
{
/** splitmix64, so a seed gives the same pattern on every machine. */
struct Rng
{
    juce::uint64 state;
    explicit Rng (juce::uint32 seed) : state ((juce::uint64) seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    juce::uint64 next()
    {
        state += 0x9E3779B97F4A7C15ull;
        auto z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float uniform()     { return (float) (next() >> 40) / 16777216.0f; }
    int below (int n)   { return n > 0 ? (int) (next() % (juce::uint64) n) : 0; }
};

constexpr juce::int64 barTicks = ticksPerQuarter * 4;

/** How strong a position in the bar is: 1 on the downbeat down to nearly nothing between 16ths. */
float metricWeight (juce::int64 pos)
{
    pos %= barTicks;
    if (pos == 0)                          return 1.0f;
    if (pos % (ticksPerQuarter * 2) == 0)  return 0.8f;
    if (pos % ticksPerQuarter == 0)        return 0.6f;
    if (pos % (ticksPerQuarter / 2) == 0)  return 0.4f;
    if (pos % (ticksPerQuarter / 4) == 0)  return 0.2f;
    return 0.1f;
}

int velocityFor (juce::int64 pos, Rng& rng)
{
    const float w = metricWeight (pos);
    return juce::jlimit (1, 127, juce::roundToInt (62.0f + 52.0f * w + (rng.uniform() - 0.5f) * 14.0f));
}
}

//==============================================================================
int StepPattern::velocityAt (int row, int step) const
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return 0;
    const auto& v = rows[(size_t) row].velocity;
    return juce::isPositiveAndBelow (step, (int) v.size()) ? v[(size_t) step] : 0;
}

void StepPattern::set (int row, int step, int velocity)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()) || ! juce::isPositiveAndBelow (step, numSteps))
        return;
    auto& v = rows[(size_t) row].velocity;
    if ((int) v.size() < numSteps)
        v.resize ((size_t) numSteps, 0);
    v[(size_t) step] = juce::jlimit (0, 127, velocity);
}

void StepPattern::setNumSteps (int n)
{
    numSteps = juce::jlimit (1, maxSteps, n);
    for (auto& r : rows)
        r.velocity.resize ((size_t) numSteps, 0);
}

void StepPattern::clear()
{
    for (auto& r : rows)
        std::fill (r.velocity.begin(), r.velocity.end(), 0);
}

bool StepPattern::isEmpty() const
{
    for (auto& r : rows)
        for (int v : r.velocity)
            if (v > 0)
                return false;
    return true;
}

bool StepPattern::isAudible (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()) || rows[(size_t) row].mute)
        return false;
    const bool anySolo = std::any_of (rows.begin(), rows.end(), [] (const Row& r) { return r.solo; });
    return ! anySolo || rows[(size_t) row].solo;
}

std::vector<Note> StepPattern::toNotes (juce::int64 endTick) const
{
    std::vector<Note> notes;
    const auto len = lengthTicks();
    if (len <= 0 || stepTicks <= 0)
        return notes;

    std::vector<int> audible;
    for (int r = 0; r < (int) rows.size(); ++r)
        if (isAudible (r))
            audible.push_back (r);

    const auto gate = juce::jmax ((juce::int64) 1, (juce::int64) stepTicks - stepTicks / 8);
    for (juce::int64 rep = 0; rep * len < endTick; ++rep)
        for (int s = 0; s < numSteps; ++s)
        {
            const auto start = rep * len + (juce::int64) s * stepTicks;
            if (start >= endTick)
                break;
            for (int r : audible)
                if (const int v = velocityAt (r, s); v > 0)
                    notes.push_back ({ rows[(size_t) r].pitch, v, start, juce::jmin (gate, endTick - start) });
        }
    return notes;
}

StepPattern StepPattern::fromNotes (const std::vector<Note>& notes, const std::vector<int>& rowPitches, int stepTicks, int numSteps)
{
    StepPattern p;
    p.stepTicks = juce::jmax (1, stepTicks);
    p.numSteps = juce::jlimit (1, maxSteps, numSteps);
    for (int pitch : rowPitches)
        p.rows.push_back ({ pitch, false, false, std::vector<int> ((size_t) p.numSteps, 0) });

    for (auto& n : notes)
    {
        const auto step = (int) ((n.start + p.stepTicks / 2) / p.stepTicks);
        if (n.start < 0 || step >= p.numSteps)
            continue;
        for (auto& r : p.rows)
            if (r.pitch == n.pitch)
                r.velocity[(size_t) step] = juce::jmax (r.velocity[(size_t) step], juce::jlimit (1, 127, n.velocity));
    }
    return p;
}

juce::String StepPattern::toJson() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("steps", numSteps);
    o->setProperty ("stepTicks", stepTicks);
    juce::Array<juce::var> list;
    for (auto& r : rows)
    {
        auto* row = new juce::DynamicObject();
        row->setProperty ("pitch", r.pitch);
        if (r.mute) row->setProperty ("mute", true);
        if (r.solo) row->setProperty ("solo", true);
        juce::StringArray v;
        for (int x : r.velocity)
            v.add (juce::String (x));
        row->setProperty ("v", v.joinIntoString (","));
        list.add (juce::var (row));
    }
    o->setProperty ("rows", list);
    return juce::JSON::toString (juce::var (o), true);
}

bool StepPattern::fromJson (const juce::String& json, StepPattern& p)
{
    const auto v = juce::JSON::parse (json);
    if (! v.isObject() || ! v["rows"].isArray())
        return false;
    StepPattern out;
    out.numSteps = juce::jlimit (1, maxSteps, (int) v.getProperty ("steps", 16));
    out.stepTicks = juce::jlimit (1, ticksPerQuarter * 4, (int) v.getProperty ("stepTicks", ticksPerQuarter / 4));
    for (auto& r : *v["rows"].getArray())
    {
        Row row;
        row.pitch = juce::jlimit (0, 127, (int) r["pitch"]);
        row.mute = (bool) r.getProperty ("mute", false);
        row.solo = (bool) r.getProperty ("solo", false);
        for (auto& x : juce::StringArray::fromTokens (r["v"].toString(), ",", {}))
            row.velocity.push_back (juce::jlimit (0, 127, x.getIntValue()));
        row.velocity.resize ((size_t) out.numSteps, 0);
        out.rows.push_back (std::move (row));
    }
    p = std::move (out);
    return true;
}

bool StepPattern::operator== (const StepPattern& o) const
{
    if (numSteps != o.numSteps || stepTicks != o.stepTicks || rows.size() != o.rows.size())
        return false;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].pitch != o.rows[i].pitch || rows[i].mute != o.rows[i].mute || rows[i].solo != o.rows[i].solo
            || rows[i].velocity != o.rows[i].velocity)
            return false;
    return true;
}

//==============================================================================
void StepPattern::randomize (juce::uint32 seed, bool drums, float density)
{
    clear();
    Rng rng (seed);
    density = juce::jlimit (0.0f, 1.0f, density);
    const float more = 0.5f + density;      // 0.5 .. 1.5

    if (drums)
    {
        for (int r = 0; r < (int) rows.size(); ++r)
        {
            const int pitch = rows[(size_t) r].pitch;
            for (int s = 0; s < numSteps; ++s)
            {
                const auto pos = (juce::int64) s * stepTicks;
                const auto inBar = pos % barTicks;
                const bool quarter = inBar % ticksPerQuarter == 0, eighth = inBar % (ticksPerQuarter / 2) == 0,
                           sixteenth = inBar % (ticksPerQuarter / 4) == 0;
                const bool backbeat = inBar % (ticksPerQuarter * 2) == ticksPerQuarter;
                float p = 0.0f;
                bool ghost = false;
                switch (pitch)
                {
                    case 35: case 36:
                        p = inBar == 0 ? 1.0f : inBar == ticksPerQuarter * 2 ? 0.6f
                          : backbeat ? 0.0f : eighth ? 0.2f * more : sixteenth ? 0.06f * more : 0.0f;
                        break;
                    case 38: case 40:
                        if (backbeat) p = 1.0f;
                        else if (sixteenth && ! eighth) { p = 0.12f * more; ghost = true; }
                        break;
                    case 37: case 39:
                        p = backbeat ? 0.3f : 0.0f;
                        break;
                    case 42:
                        p = eighth ? 0.92f : sixteenth ? 0.45f * density : 0.0f;
                        break;
                    case 46:
                        p = eighth && ! quarter ? 0.1f * more : 0.0f;
                        break;
                    case 49: case 57:
                        p = pos == 0 ? 0.35f : 0.0f;
                        break;
                    case 41: case 43: case 45: case 47: case 48: case 50:
                        p = inBar >= barTicks - ticksPerQuarter && sixteenth ? 0.14f * more : 0.0f;
                        break;
                    case 44: case 51: case 53: case 52: case 55: case 59:
                        p = 0.0f;
                        break;
                    default:    // hand percussion: off-beats
                        p = eighth && ! quarter ? 0.35f * more : sixteenth && ! eighth ? 0.12f * more : 0.0f;
                        break;
                }
                const float u = rng.uniform();
                const int v = velocityFor (pos, rng);
                if (u < p)
                    set (r, s, ghost ? juce::jmax (1, v / 3) : v);
            }
        }
        return;
    }

    // Melodic: a stepwise line in the rows' scale, coming home to the lowest root on downbeats.
    if (rows.empty())
        return;
    const int n = (int) rows.size();
    int root = n - 1;                           // rows run high to low
    for (int r = n - 1; r >= 0; --r)
        if (rows[(size_t) r].pitch % 12 == rows[(size_t) (n - 1)].pitch % 12)
        {
            root = r;
            if (n - 1 - r >= 3) break;          // a root a little above the bottom row
        }
    int at = root;
    for (int s = 0; s < numSteps; ++s)
    {
        const auto pos = (juce::int64) s * stepTicks;
        const float w = metricWeight (pos);
        const float u = rng.uniform();
        const int move = rng.below (7) - 3;     // -3..3 rows
        const float home = rng.uniform();
        const int v = velocityFor (pos, rng);
        if (u >= juce::jmin (0.95f, w * 0.9f * more))
            continue;
        if ((pos % barTicks) == 0 && home < 0.6f)
            at = root;
        else
            at = juce::jlimit (0, n - 1, at + (std::abs (move) == 3 ? move / 3 : move));
        set (at, s, v);
    }
}

//==============================================================================
std::vector<int> StepPattern::drumRows()
{
    return { 36, 38, 37, 39, 42, 44, 46, 50, 47, 45, 43, 49, 51, 53, 54, 56, 70, 75 };
}

juce::String StepPattern::rowName (int pitch, bool drums)
{
    if (drums)
    {
        switch (pitch)
        {
            case 35: return "Kick 2";        case 36: return "Kick";          case 37: return "Side Stick";
            case 38: return "Snare";         case 39: return "Clap";          case 40: return "Snare 2";
            case 41: return "Low Floor Tom"; case 42: return "Closed Hat";    case 43: return "Floor Tom";
            case 44: return "Pedal Hat";     case 45: return "Low Tom";       case 46: return "Open Hat";
            case 47: return "Mid Tom";       case 48: return "Hi-Mid Tom";    case 49: return "Crash";
            case 50: return "High Tom";      case 51: return "Ride";          case 52: return "China";
            case 53: return "Ride Bell";     case 54: return "Tambourine";    case 55: return "Splash";
            case 56: return "Cowbell";       case 57: return "Crash 2";       case 59: return "Ride 2";
            case 60: return "Hi Bongo";      case 61: return "Low Bongo";     case 62: return "Mute Conga";
            case 63: return "Open Conga";    case 64: return "Low Conga";     case 69: return "Cabasa";
            case 70: return "Shaker";        case 75: return "Claves";        case 76: return "Hi Block";
            case 77: return "Low Block";     case 80: return "Mute Triangle"; case 81: return "Triangle";
            default: break;
        }
    }
    static const char* const names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[((pitch % 12) + 12) % 12]) + juce::String (pitch / 12 - 2);     // Logic: 60 is C3
}

juce::StringArray StepPattern::scaleNames()
{
    return { "Chromatic", "Major", "Minor", "Pentatonic" };
}

std::vector<int> StepPattern::scaleRows (Scale scale, int lowest, int count)
{
    static const std::vector<int> major { 0, 2, 4, 5, 7, 9, 11 }, minor { 0, 2, 3, 5, 7, 8, 10 }, penta { 0, 3, 5, 7, 10 };
    const std::vector<int>* degrees = scale == Scale::major ? &major : scale == Scale::minor ? &minor
                                    : scale == Scale::pentatonic ? &penta : nullptr;
    std::vector<int> up;
    for (int p = juce::jlimit (0, 127, lowest); p <= 127 && (int) up.size() < count; ++p)
        if (degrees == nullptr || std::find (degrees->begin(), degrees->end(), p % 12) != degrees->end())
            up.push_back (p);
    std::reverse (up.begin(), up.end());
    return up;
}

void StepPattern::setRows (const std::vector<int>& pitches)
{
    std::vector<Row> next;
    for (int pitch : pitches)
    {
        Row row { pitch, false, false, std::vector<int> ((size_t) numSteps, 0) };
        for (auto& r : rows)
            if (r.pitch == pitch)
            {
                row = r;
                row.velocity.resize ((size_t) numSteps, 0);
            }
        next.push_back (std::move (row));
    }
    rows = std::move (next);
}

void StepPattern::applyTo (Project& project, juce::ValueTree region, juce::UndoManager* undo) const
{
    if (! region.isValid())
        return;
    const auto grid = project.grid();
    const auto end = (juce::int64) std::llround (grid.toTicks ((double) ((juce::int64) region[ids::offset] + (juce::int64) region[ids::length])));
    replaceRegionNotes (region, toNotes (end), undo);
    region.setProperty (property, toJson(), undo);
}

} // namespace woodshed::daw
