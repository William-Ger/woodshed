#include "Drummer.h"
#include "StepPattern.h"
#include <numeric>

namespace woodshed::daw
{

//==============================================================================
std::vector<Note> regionNotes (const juce::ValueTree& region)
{
    std::vector<Note> out;
    for (auto n : region)
        if (n.hasType (ids::NOTE))
            out.push_back (Project::noteFrom (n));
    return out;
}

void replaceRegionNotes (juce::ValueTree region, const std::vector<Note>& notes, juce::UndoManager* undo)
{
    if (! region.isValid())
        return;

    for (int i = region.getNumChildren(); --i >= 0;)
        if (region.getChild (i).hasType (ids::NOTE))
            region.removeChild (i, undo);

    auto sorted = notes;
    std::stable_sort (sorted.begin(), sorted.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    for (auto& n : sorted)
    {
        juce::ValueTree note (ids::NOTE);
        note.setProperty (ids::pitch, juce::jlimit (0, 127, n.pitch), nullptr);
        note.setProperty (ids::velocity, juce::jlimit (1, 127, n.velocity), nullptr);
        note.setProperty (ids::start, n.start, nullptr);
        note.setProperty (ids::length, juce::jmax ((juce::int64) 1, n.length), nullptr);
        region.appendChild (note, undo);
    }
}

void RegionSnapshot::take (const juce::ValueTree& r)
{
    region = r;
    notes = regionNotes (r);
    properties.clear();
    for (int i = 0; i < r.getNumProperties(); ++i)
        properties.set (r.getPropertyName (i), r.getProperty (r.getPropertyName (i)));
}

void RegionSnapshot::restore()
{
    if (! region.isValid())
        return;
    replaceRegionNotes (region, notes, nullptr);
    juce::ValueTree source (region.getType());
    for (auto& p : properties)
        source.setProperty (p.name, p.value, nullptr);
    region.copyPropertiesFrom (source, nullptr);
}

namespace drummer
{
namespace
{
//==============================================================================
/** splitmix64: small, fast and the same everywhere, so a seed always gives the same groove. */
struct Rng
{
    juce::uint64 state;

    Rng (juce::uint32 seed, int a, int b)
        : state ((juce::uint64) seed * 0x9E3779B97F4A7C15ull
                 ^ ((juce::uint64) (juce::uint32) (a + 1) * 0xBF58476D1CE4E5B9ull)
                 ^ ((juce::uint64) (juce::uint32) (b + 7) * 0x94D049BB133111EBull))
    {
        next();
    }

    juce::uint64 next()
    {
        state += 0x9E3779B97F4A7C15ull;
        auto z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    float uniform()                 { return (float) (next() >> 40) / 16777216.0f; }      // [0, 1)
    float bipolar()                 { return uniform() * 2.0f - 1.0f; }
    int below (int n)               { return n > 0 ? (int) (next() % (juce::uint64) n) : 0; }
};

//==============================================================================
// GM drum notes.
constexpr int kick = 36, snare = 38, sideStick = 37, clap = 39, closedHat = 42, pedalHat = 44, openHat = 46,
              crash1 = 49, crash2 = 57, ride = 51, rideBell = 53, tambourine = 54, cowbell = 56, maracas = 70;

/**
    One style. Pattern strings are one bar of 4/4 (or two, alternating): 16 sixteenths, or 12
    triplet eighths when `triplet`. Spaces are for reading only. Characters:
      .  rest     x  hit     X  accent     g  ghost
      o  open hat (or the ride's bell)     r  a three-stroke roll     t  two thirty-seconds
    Three of each: simple, medium, complex.
*/
struct StyleDef
{
    const char* name;
    bool triplet;
    bool sixteenthSwing;    // the swing knob swings 16ths (else 8ths)
    float swing;            // the style's own swing
    float looseness;        // how much humanizing moves it
    int backbeat;           // what the snare lane's hits play
    bool softRim;           // played soft, the backbeat goes to the side stick
    bool ghosts;            // ghost notes on the snare as it gets more complex
    bool rideByDefault, rideWhenLoud, percussionByDefault;
    int percNote;
    int lateSnare;          // ticks the backbeat sits behind the beat
    const char* kick[3];
    const char* snare[3];
    const char* time[3];
    const char* perc[3];
    const char* foot;       // hat pedal pattern, or nullptr
};

const StyleDef styles[numStyles] {
    { "Rock", false, true, 0.0f, 0.5f, snare, false, true, false, true, false, tambourine, 0,
      { "x... .... x... ....", "x... .... x.x. ....", "x... ...x x.x. ..x." },
      { ".... x... .... x...", ".... x... .... x...", ".... x... .... x..." },
      { "X.x. X.x. X.x. X.x.", "X.x. X.x. X.x. X.x.", "X.x. X.x. X.x. X.o." },
      { ".... x... .... x...", "..x. ..x. ..x. ..x.", "x.x. x.x. x.x. x.x." }, nullptr },

    { "Pop", false, true, 0.0f, 0.4f, snare, true, true, false, false, true, tambourine, 0,
      { "x... .... x... ....", "x... ...x x... ....", "x..x ..x. x... ..x." },
      { ".... x... .... x...", ".... x... .... x...", ".... x... .... x..." },
      { "X.x. X.x. X.x. X.x.", "X.x. X.x. X.x. X.x.", "Xgxg Xgxg Xgxg Xgxg" },
      { ".... x... .... x...", "..x. ..x. ..x. ..x.", "xgxg xgxg xgxg xgxg" }, nullptr },

    { "Funk", false, true, 0.12f, 0.35f, snare, false, true, false, false, false, cowbell, 0,
      { "x... .... ..x. ....", "x..x .... ..x. .x..", "x.xx ...x ..x. .x.." },
      { ".... x... .... x...", ".... x..g .g.. x...", "..g. x..g .g.g x.g." },
      { "X.x. X.x. X.x. X.x.", "Xgxg Xgxg Xgxg Xgxg", "Xgxg Xgxo Xgxg Xgxg" },
      { "x... x... x... x...", "x.x. x.x. x.x. x.x.", "x.xx x.x. x.xx x.x." }, nullptr },

    { "Hip-Hop", false, true, 0.3f, 0.5f, snare, false, true, false, false, false, clap, 0,
      { "x... .... x.x. ....", "x... ...x x.x. ....", "x..x ...x x.x. ..x." },
      { ".... x... .... x...", ".... x... .... x...", ".... x... .... x..." },
      { "x.x. x.x. x.x. x.x.", "X.x. X.x. X.x. X.xx", "Xgxg Xgx. Xgxg X.xo" },
      { ".... x... .... x...", ".... x... .... x...", ".... x... .... x..." }, nullptr },

    { "Trap", false, true, 0.0f, 0.1f, snare, false, false, false, false, true, clap, 0,
      { "x... .... ..x. ....", "x... ...x .x.. ..x.", "x..x ...x .x.. x.x." },
      { ".... .... x... ....", ".... .... x... ....", ".... .... x... ...g" },
      { "x.x. x.x. x.x. x.x.", "Xxxx Xxxx Xxxx Xxxx", "Xxxx Xxrx Xxxx Xtxr" },
      { ".... .... x... ....", ".... .... x... ....", ".... .... x... ...." }, nullptr },

    { "House", false, true, 0.1f, 0.15f, clap, false, false, false, false, true, maracas, 0,
      { "x... x... x... x...", "x... x... x... x...", "x... x... x... x..x" },
      { ".... x... .... x...", ".... x... .... x...", ".... x... .... x..g" },
      { "..o. ..o. ..o. ..o.", "x.o. x.o. x.o. x.o.", "xgox xgox xgox xgox" },
      { "x.x. x.x. x.x. x.x.", "xgxg xgxg xgxg xgxg", "Xgxg Xgxg Xgxg Xgxx" }, nullptr },

    { "Disco", false, true, 0.0f, 0.2f, snare, false, false, false, false, true, tambourine, 0,
      { "x... x... x... x...", "x... x... x... x...", "x... x... x..x x..." },
      { ".... x... .... x...", ".... x... .... x...", ".... x... .... x.g." },
      { "x.o. x.o. x.o. x.o.", "xxo. xxo. xxo. xxo.", "xxox xxox xxox xxox" },
      { "x.x. x.x. x.x. x.x.", "xgxg xgxg xgxg xgxg", "xgxg xgxg xgxg xgxg" }, nullptr },

    { "Reggae", false, true, 0.25f, 0.4f, sideStick, false, true, false, false, false, tambourine, 10,
      { ".... .... x... ....", ".... .... x... ....", "x... x... x... x..." },
      { ".... .... x... ....", ".... .... x... ....", ".... .... x... ...." },
      { "x.x. x.x. x.x. x.x.", "x.xx x.x. x.xx x.x.", "x.xx x.xx x.xx x.xo" },
      { "..x. ..x. ..x. ..x.", "..x. ..x. ..x. ..x.", "..xx ..x. ..xx ..x." }, nullptr },

    { "Jazz Swing", true, false, 0.0f, 0.45f, snare, true, true, true, false, false, tambourine, 0,
      { "g.. ... g.. ...", "g.. g.. g.. g..", "g.. g.. g.. g.x" },
      { "... ... ... ...", "... ..g ... ..g", "..g ..x .g. ..g" },
      { "X.. X.. X.. X..", "x.. X.x x.. X.x", "x.. X.x x.. X.x" },
      { nullptr, nullptr, nullptr }, "... x.. ... x.." },

    { "Bossa Nova", false, true, 0.0f, 0.3f, sideStick, false, false, false, false, true, maracas, 0,
      { "x..x x... x..x x...", "x..x x... x..x x...", "x..x x..x x..x x..x" },
      { "x... ..x. .... x... .... x... ..x. ....", "x... ..x. .... x... .... x... ..x. ....",
        "x... ..x. ...g x... .... x... ..x. ...g" },
      { "x.x. x.x. x.x. x.x.", "xgxg xgxg xgxg xgxg", "Xgxg Xgxg Xgxg Xgxg" },
      { "xgxg xgxg xgxg xgxg", "xgxg xgxg xgxg xgxg", "Xgxx Xgxg Xgxx Xgxg" }, nullptr },

    { "Metal", false, true, 0.0f, 0.1f, snare, false, false, false, true, false, cowbell, 0,
      { "x.x. ..x. x.x. ..x.", "x.x. x.x. x.x. x.x.", "xxxx xxxx xxxx xxxx" },
      { ".... X... .... X...", ".... X... .... X...", ".... X... .... X..." },
      { "X.x. X.x. X.x. X.x.", "X.X. X.X. X.X. X.X.", "X.X. X.X. X.X. X.X." },
      { "x... x... x... x...", "x... x... x... x...", "x.x. x.x. x.x. x.x." }, nullptr },

    { "Shuffle", true, false, 0.0f, 0.35f, snare, false, true, false, false, false, tambourine, 0,
      { "x.. ... x.. ...", "x.. ..x x.. ...", "x.x ..x x.. ..x" },
      { "... x.. ... x..", "... x.. ... x..", "... x.g ... x.g" },
      { "X.x x.x X.x x.x", "X.x x.x X.x x.x", "X.x x.x X.x x.o" },
      { "... x.. ... x..", "..x ..x ..x ..x", "x.x x.x x.x x.x" }, nullptr },

    { "Punk", false, true, 0.0f, 0.25f, snare, false, false, false, true, false, tambourine, 0,
      { "x... .... x.x. ....", "x.x. .... x.x. ....", "x.x. ..x. x.x. ..x." },
      { ".... X... .... X...", ".... X... .... X...", ".... X... .... X.x." },
      { "X.X. X.X. X.X. X.X.", "X.X. X.X. X.X. X.X.", "X.X. X.X. X.X. X.X." },
      { "x.x. x.x. x.x. x.x.", "x.x. x.x. x.x. x.x.", "x.x. x.x. x.x. x.x." }, nullptr },

    { "Ballad", false, true, 0.0f, 0.6f, snare, true, true, false, false, false, tambourine, 6,
      { "x... .... .... ....", "x... .... x... ....", "x... .... x..x ...." },
      { ".... x... .... x...", ".... x... .... x...", ".... x... .... x..." },
      { "x... x... x... x...", "x.x. x.x. x.x. x.x.", "Xgxg Xgxg Xgxg Xgxg" },
      { ".... x... .... x...", ".... x... .... x...", "..x. x.x. ..x. x.x." }, nullptr },

    { "Afrobeat", false, true, 0.1f, 0.35f, snare, false, true, false, false, true, cowbell, 0,
      { "x... ..x. ..x. ....", "x... ..x. ..x. ..x.", "x..x ..x. ..x. x.x." },
      { ".... x... .... x...", ".... x..g ..g. x...", "..g. x..g .gg. x.g." },
      { "x.x. x.x. x.x. x.x.", "x.xx x.x. x.xx x.x.", "x.xx x.xx x.xx x.xo" },
      { "x.x. xx.x .xx. x.x.", "x.x. xx.x .xx. x.x.", "x.x. xx.x .xx. x.xx" }, nullptr },

    { "Lo-Fi", false, true, 0.5f, 0.7f, snare, false, true, false, false, false, clap, 14,
      { "x... .... ..x. ....", "x... ...x ..x. ....", "x..x ...x ..x. .x.." },
      { ".... x... .... x...", ".... x... .... x...", ".... x... .... x..." },
      { "x.x. x.x. x.x. x.x.", "x.xg x.xg x.xg x.xg", "xgxg xgxg xgxg xgxo" },
      { ".... x... .... x...", ".... x... .... x...", ".... x... .... x..." }, nullptr },
};

const StyleDef& def (Style s)        { return styles[juce::jlimit (0, numStyles - 1, (int) s)]; }

std::string clean (const char* p)
{
    std::string s;
    for (; p != nullptr && *p != 0; ++p)
        if (*p != ' ' && *p != '|')
            s += *p;
    return s;
}

/** The character for (bar, source beat, step) of a pattern, '.' if there isn't one. */
char at (const char* pattern, int stepsPerBeat, int bar, int beat, int step)
{
    if (pattern == nullptr)
        return '.';
    const auto s = clean (pattern);
    const int barSteps = stepsPerBeat * 4;
    const int bars = juce::jmax (1, (int) s.size() / barSteps);
    const size_t i = (size_t) ((bar % bars) * barSteps + beat * stepsPerBeat + step);
    return i < s.size() ? s[i] : '.';
}

int level (float complexity)        { return juce::jlimit (0, 2, (int) (complexity * 3.0f)); }

//==============================================================================
/** A part of the bar that plays one beat of the 4/4 source bar. */
struct Pulse
{
    juce::int64 start = 0, length = ticksPerQuarter;
    bool compound = false;      // three eighths (6/8, 9/8, 12/8, the 3 in 7/8)
    int source = 0;             // which beat of the source bar
};

std::vector<Pulse> pulsesFor (const Meter& m, bool halfTime)
{
    std::vector<Pulse> pulses;
    const int beats = juce::jmax (1, m.beatsPerBar);

    if (m.beatUnit == 8)
    {
        // Eighths in groups of three (compound) or two, a three first when it doesn't divide.
        std::vector<int> groups;
        if (beats % 3 == 0)
            groups.assign ((size_t) (beats / 3), 3);
        else if (beats == 1)
            groups = { 1 };
        else
        {
            if (beats % 2 == 1) groups.push_back (3);
            while (std::accumulate (groups.begin(), groups.end(), 0) < beats) groups.push_back (2);
        }
        juce::int64 t = 0;
        for (int g : groups)
        {
            pulses.push_back ({ t, (juce::int64) g * ticksPerQuarter / 2, g == 3, 0 });
            t += (juce::int64) g * ticksPerQuarter / 2;
        }
    }
    else
    {
        const auto beatTicks = (juce::int64) ticksPerQuarter * 4 / juce::jmax (1, m.beatUnit);
        for (int i = 0; i < beats; ++i)
            pulses.push_back ({ (juce::int64) i * beatTicks, beatTicks, false, 0 });
    }

    // Which source beat each pulse plays: pulses in twos (beats 1-2, then 3-4) with one group of
    // three (1, 2, 4: the backbeat twice, like a waltz) when the count is odd.
    // A half-time groove (its backbeat on 3) spreads the source bar over the pulses instead.
    const int n = (int) pulses.size();
    std::vector<int> source;
    if (halfTime)
        for (int i = 0; i < n; ++i)
            source.push_back (n == 1 ? 0 : i * 4 / n);
    else if (n == 1)
        source = { 0 };
    else
    {
        if (n % 2 == 1) source.insert (source.end(), { 0, 1, 3 });
        while ((int) source.size() < n)
            source.insert (source.end(), source.empty() ? std::initializer_list<int> { 0, 1 } : std::initializer_list<int> { 2, 3 });
    }
    for (int i = 0; i < n; ++i)
        pulses[(size_t) i].source = source[(size_t) i];
    return pulses;
}

/** Where step `s` of a source beat lands inside a pulse. */
juce::int64 stepTick (const Pulse& p, bool triplet, int s)
{
    if (triplet)
        return p.length * s / 3;
    if (! p.compound)
        return p.length * s / 4;
    // Straight sixteenths over three eighths: 1 -> the second eighth, & -> the third, a -> just before the next pulse.
    static const int sixths[] { 0, 2, 4, 5 };
    return p.length * sixths[juce::jlimit (0, 3, s)] / 6;
}

juce::int64 stepLength (const Pulse& p, bool triplet)
{
    return triplet || p.compound ? p.length / (p.compound && ! triplet ? 6 : 3) : p.length / 4;
}

/** Delays the off-beats: amount 1 moves them to the last triplet. */
juce::int64 swingTick (juce::int64 t, juce::int64 length, float amount, bool sixteenths)
{
    if (amount <= 0.0f)
        return t;
    const double cell = sixteenths ? (double) length / 2.0 : (double) length;
    const double half = cell / 2.0, d = (double) amount * cell / 6.0;
    const double inCell = std::fmod ((double) t, cell), base = (double) t - inCell;
    const double mapped = inCell < half ? inCell * (half + d) / half
                                        : half + d + (inCell - half) * (half - d) / half;
    return (juce::int64) std::llround (base + mapped);
}

enum class Lane { kick, snare, time, foot, perc, fill, crash };

struct Hit
{
    juce::int64 tick;
    int pitch;
    float strength;     // 0..1 before loudness: accent 1, hit 0.8, ghost 0.3
    bool ghost;
    Lane lane;
};

float strengthOf (char c)
{
    switch (c)
    {
        case 'X': return 1.0f;
        case 'g': return 0.3f;
        case 'o': return 0.85f;
        case 'r': case 't': return 0.55f;
        default:  return 0.8f;
    }
}

float laneScale (Lane l)
{
    switch (l)
    {
        case Lane::time:  return 0.82f;
        case Lane::foot:  return 0.6f;
        case Lane::perc:  return 0.72f;
        case Lane::fill:  return 0.95f;
        case Lane::crash: return 0.95f;
        case Lane::kick: case Lane::snare: default: return 1.0f;
    }
}
} // namespace

//==============================================================================
juce::String styleName (Style s)        { return def (s).name; }

Style styleFromName (const juce::String& name)
{
    for (int i = 0; i < numStyles; ++i)
        if (name.equalsIgnoreCase (styles[i].name))
            return (Style) i;
    return Style::rock;
}

bool isGeneralMidiDrum (int pitch)      { return pitch >= 35 && pitch <= 81; }

juce::String Settings::toJson() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("style", styleName (style));
    o->setProperty ("complexity", complexity);
    o->setProperty ("loudness", loudness);
    o->setProperty ("swing", swing);
    o->setProperty ("fill", fill);
    o->setProperty ("humanize", humanize);
    o->setProperty ("kickSnare", kickSnare);
    o->setProperty ("hiHat", hiHat);
    o->setProperty ("cymbals", cymbals);
    o->setProperty ("toms", toms);
    o->setProperty ("percussion", percussion);
    o->setProperty ("timekeeper", timekeeper == Timekeeper::hiHat ? "hihat" : timekeeper == Timekeeper::ride ? "ride" : "auto");
    o->setProperty ("bars", bars);
    o->setProperty ("seed", (juce::int64) seed);
    return juce::JSON::toString (juce::var (o), true);
}

Settings Settings::fromJson (const juce::String& json)
{
    Settings s;
    const auto v = juce::JSON::parse (json);
    if (! v.isObject())
        return s;
    auto num = [&v] (const char* key, float fallback)
    {
        return v.hasProperty (key) ? juce::jlimit (0.0f, 1.0f, (float) (double) v[key]) : fallback;
    };
    auto flag = [&v] (const char* key, bool fallback) { return v.hasProperty (key) ? (bool) v[key] : fallback; };
    s.style = styleFromName (v["style"].toString());
    s.complexity = num ("complexity", s.complexity);
    s.loudness = num ("loudness", s.loudness);
    s.swing = num ("swing", s.swing);
    s.fill = num ("fill", s.fill);
    s.humanize = num ("humanize", s.humanize);
    s.kickSnare = flag ("kickSnare", s.kickSnare);
    s.hiHat = flag ("hiHat", s.hiHat);
    s.cymbals = flag ("cymbals", s.cymbals);
    s.toms = flag ("toms", s.toms);
    s.percussion = flag ("percussion", s.percussion);
    const auto tk = v["timekeeper"].toString();
    s.timekeeper = tk == "hihat" ? Timekeeper::hiHat : tk == "ride" ? Timekeeper::ride : Timekeeper::automatic;
    if (v.hasProperty ("bars")) s.bars = juce::jlimit (1, 64, (int) v["bars"]);
    if (v.hasProperty ("seed")) s.seed = (juce::uint32) (juce::int64) v["seed"];
    return s;
}

bool Settings::operator== (const Settings& o) const
{
    return toJson() == o.toJson();
}

Settings withStyle (Settings s, Style style)
{
    s.style = style;
    s.swing = def (style).swing;
    s.percussion = def (style).percussionByDefault;
    return s;
}

int phraseBars (const Settings& s)
{
    if (s.fill < 0.02f)
        return 0;
    return s.fill < 0.4f ? 8 : 4;
}

bool isFillBar (const Settings& s, int bar)
{
    const int phrase = phraseBars (s);
    return phrase > 0 && (bar + 1) % phrase == 0 && (s.kickSnare || s.toms);
}

//==============================================================================
std::vector<Note> generate (const Settings& s, const Meter& meter)
{
    const auto& d = def (s.style);
    const int spb = d.triplet ? 3 : 4;
    const bool halfTime = at (d.snare[0], spb, 0, 2, 0) != '.' && at (d.snare[0], spb, 0, 1, 0) == '.';
    const auto pulses = pulsesFor (meter, halfTime);
    const int numPulses = (int) pulses.size();
    const auto barTicks = meter.barTicks();
    const int bars = juce::jmax (1, s.bars);
    const auto total = barTicks * bars;
    const int lvl = level (s.complexity);

    const bool rideTime = s.timekeeper == Timekeeper::ride
                       || (s.timekeeper == Timekeeper::automatic && (d.rideByDefault || (d.rideWhenLoud && s.loudness > 0.8f)));
    const bool timeOn = rideTime ? s.cymbals : s.hiHat;
    const int backbeat = d.softRim && s.loudness < 0.3f ? sideStick : d.backbeat;
    const bool percOn = s.percussion && d.perc[0] != nullptr;

    std::vector<Hit> hits;
    auto add = [&hits] (juce::int64 t, int pitch, float strength, bool ghost, Lane lane) { hits.push_back ({ t, pitch, strength, ghost, lane }); };

    auto timePitch = [rideTime] (char c) { return rideTime ? (c == 'o' ? rideBell : ride) : (c == 'o' ? openHat : c == 'p' ? pedalHat : closedHat); };

    for (int bar = 0; bar < bars; ++bar)
    {
        const auto barStart = barTicks * bar;
        const bool fillBar = isFillBar (s, bar);
        const int fillPulses = fillBar ? juce::jlimit (1, numPulses, juce::roundToInt (s.fill * (float) numPulses)) : 0;
        const int firstFill = numPulses - fillPulses;
        const bool crashHere = bar > 0 && isFillBar (s, bar - 1) && s.cymbals;

        // Variation: some bars borrow the next level's kick, more often the more complex it is.
        Rng variation (s.seed, bar, 1);
        const int kickLevel = juce::jmin (2, lvl + (variation.uniform() < s.complexity * 0.45f ? 1 : 0));
        const float pickup = variation.uniform();

        for (int pi = 0; pi < firstFill; ++pi)
        {
            const auto& p = pulses[(size_t) pi];
            const auto start = barStart + p.start;
            const int src = p.source;
            Rng ghosts (s.seed, bar * 64 + pi, 2);

            auto lanePattern = [&] (Lane lane, const char* pattern, auto pitchFor)
            {
                for (int step = 0; step < spb; ++step)
                {
                    const char c = at (pattern, spb, bar, src, step);
                    if (c == '.')
                        continue;
                    const auto t = start + stepTick (p, d.triplet, step);
                    const auto len = stepLength (p, d.triplet);
                    const int pitch = pitchFor (c);
                    if (c == 'r')
                        for (int k = 0; k < 3; ++k) add (t + len * k / 3, pitch, strengthOf (c) + 0.1f * (float) k, false, lane);
                    else if (c == 't')
                        for (int k = 0; k < 2; ++k) add (t + len * k / 2, pitch, strengthOf (c) + 0.1f * (float) k, false, lane);
                    else
                        add (t, pitch, strengthOf (c), c == 'g', lane);
                }
            };

            if (s.kickSnare)
            {
                lanePattern (Lane::kick, d.kick[kickLevel], [] (char) { return kick; });
                lanePattern (Lane::snare, d.snare[lvl], [backbeat] (char c) { return c == 'g' ? snare : backbeat; });

                // A kick pickup into the next bar.
                if (pi == numPulses - 1 && ! d.triplet && ! p.compound && pickup < (s.complexity - 0.5f) * 0.8f
                    && at (d.kick[kickLevel], spb, bar, src, 3) == '.')
                    add (start + stepTick (p, false, 3), kick, 0.7f, false, Lane::kick);

                // Ghost notes around the backbeat as it gets busier.
                if (d.ghosts)
                {
                    const float amount = juce::jmax (0.0f, (s.complexity - 0.35f) / 0.65f);
                    const int nextSrc = pi + 1 < numPulses ? pulses[(size_t) pi + 1].source : pulses[0].source;
                    const bool backbeatHere = juce::String ("xX").containsChar (at (d.snare[lvl], spb, bar, src, 0));
                    const bool backbeatNext = juce::String ("xX").containsChar (at (d.snare[lvl], spb, bar, nextSrc, 0));
                    for (int step = 1; step < spb; ++step)
                    {
                        const float u = ghosts.uniform();      // drawn for every step, so more complexity only adds
                        if (at (d.snare[lvl], spb, bar, src, step) != '.')
                            continue;
                        const bool near = (step == 1 && backbeatHere) || (step == spb - 1 && backbeatNext);
                        if (u < amount * (near ? 0.55f : 0.22f))
                            add (start + stepTick (p, d.triplet, step), snare, 0.3f, true, Lane::snare);
                    }
                }
            }

            if (timeOn)
            {
                const char* pattern = d.time[lvl];
                if (p.compound && ! d.triplet)
                {
                    // Straight time over three eighths: eighths (or sixteenths) through the pulse.
                    int count = 0;
                    bool open = false;
                    char first = '.';
                    for (int step = 0; step < 4; ++step)
                    {
                        const char c = at (pattern, 4, bar, src, step);
                        if (c == '.') continue;
                        ++count;
                        open = open || c == 'o';
                        if (first == '.') first = c;
                    }
                    const int perPulse = count == 0 ? 0 : count == 1 ? 1 : count == 2 ? 3 : 6;
                    for (int k = 0; k < perPulse; ++k)
                    {
                        const char c = k == 0 ? (first == 'o' ? 'x' : first) : (perPulse == 6 && k % 2 == 1) ? 'g'
                                     : (open && k == perPulse - (perPulse == 6 ? 2 : 1)) ? 'o' : 'x';
                        if (! (crashHere && pi == 0 && k == 0))
                            add (start + p.length * k / perPulse, timePitch (c), strengthOf (c), c == 'g', Lane::time);
                    }
                }
                else
                {
                    for (int step = 0; step < spb; ++step)
                    {
                        const char c = at (pattern, spb, bar, src, step);
                        if (c == '.' || (crashHere && pi == 0 && step == 0))
                            continue;
                        const auto t = start + stepTick (p, d.triplet, step);
                        const auto len = stepLength (p, d.triplet);
                        if (c == 'r')
                            for (int k = 0; k < 3; ++k) add (t + len * k / 3, timePitch ('x'), 0.5f + 0.1f * (float) k, false, Lane::time);
                        else if (c == 't')
                            for (int k = 0; k < 2; ++k) add (t + len * k / 2, timePitch ('x'), 0.5f + 0.1f * (float) k, false, Lane::time);
                        else
                            add (t, timePitch (c), strengthOf (c), c == 'g', Lane::time);
                    }
                }
            }

            if (s.hiHat && d.foot != nullptr)
                lanePattern (Lane::foot, d.foot, [] (char) { return pedalHat; });

            if (percOn)
                lanePattern (Lane::perc, d.perc[lvl], [&d] (char) { return d.percNote; });
        }

        // The fill: the last pulses of the phrase's last bar, from the snare down the toms.
        if (fillPulses > 0)
        {
            Rng fr (s.seed, bar, 3);
            const int type = fr.below (3);
            static const int tomSets[2][3] { { 50, 47, 43 }, { 48, 45, 41 } };
            const auto& toms = tomSets[fr.below (2)];
            std::vector<int> voices;
            if (s.kickSnare) voices.push_back (snare);
            if (s.toms) voices.insert (voices.end(), std::begin (toms), std::end (toms));
            const int nv = (int) voices.size();

            struct Slot { juce::int64 t; int pulse, sub, subs; };
            std::vector<Slot> slots;
            for (int pi = firstFill; pi < numPulses; ++pi)
            {
                const auto& p = pulses[(size_t) pi];
                const int subs = d.triplet || p.compound ? 3 : s.complexity > 0.35f ? 4 : 2;
                for (int k = 0; k < subs; ++k)
                    slots.push_back ({ barStart + p.start + p.length * k / subs, pi - firstFill, k, subs });
            }
            const int n = (int) slots.size();
            for (int i = 0; i < n; ++i)
            {
                const auto& sl = slots[(size_t) i];
                const float rise = 0.68f + 0.32f * (float) i / (float) juce::jmax (1, n - 1);
                int voice = 0;
                switch (type)
                {
                    case 0:  voice = i * nv / juce::jmax (1, n); break;                                     // roll down the kit
                    case 1:  voice = i < n / 2 || nv == 1 ? 0 : 1 + (i - n / 2) * (nv - 1) / juce::jmax (1, n - n / 2); break;
                    default:                                                                                  // accents, a drum per pulse
                        if (sl.subs == 4 && sl.sub == 1) continue;
                        voice = sl.pulse * nv / juce::jmax (1, fillPulses);
                        break;
                }
                const bool accent = sl.sub == 0;
                add (sl.t, voices[(size_t) juce::jlimit (0, nv - 1, voice)], juce::jmin (1.0f, rise + (accent ? 0.08f : 0.0f)), false, Lane::fill);
                if (s.kickSnare && accent && fr.uniform() < 0.5f)
                    add (sl.t, kick, 0.8f, false, Lane::kick);
            }
        }

        if (crashHere)
        {
            add (barStart, (bar / phraseBars (s)) % 2 == 0 ? crash1 : crash2, 1.0f, false, Lane::crash);
            if (s.kickSnare)
                add (barStart, kick, 1.0f, false, Lane::kick);
        }
    }

    // Swing, feel, velocity and humanizing.
    const auto pulseAt = [&pulses, barTicks] (juce::int64 t) -> const Pulse&
    {
        const auto inBar = t % barTicks;
        for (auto it = pulses.rbegin(); it != pulses.rend(); ++it)
            if (it->start <= inBar)
                return *it;
        return pulses.front();
    };

    const float loud = juce::jlimit (0.0f, 1.0f, s.loudness);
    const float jitter = (1.0f + 12.0f * d.looseness) * s.humanize * 2.0f;
    std::vector<Note> notes;
    notes.reserve (hits.size());
    int lastBar = -1;
    Rng human (s.seed, 0, 4);
    std::stable_sort (hits.begin(), hits.end(), [] (const Hit& a, const Hit& b) { return a.tick < b.tick; });
    for (auto& h : hits)
    {
        const int bar = (int) (h.tick / barTicks);
        if (bar != lastBar)
        {
            human = Rng (s.seed, bar, 4);
            lastBar = bar;
        }
        const float vNoise = human.bipolar(), tNoise = human.bipolar();

        auto t = h.tick;
        const auto& p = pulseAt (t);
        if (! d.triplet && ! p.compound)
        {
            const auto barStart = t - t % barTicks;
            const auto pulseStart = barStart + p.start;
            t = pulseStart + swingTick (t - pulseStart, p.length, s.swing, d.sixteenthSwing);
        }
        if (h.lane == Lane::snare && ! h.ghost)
            t += d.lateSnare;
        t += (juce::int64) std::llround (tNoise * jitter);

        float v = h.ghost ? 127.0f * (0.13f + 0.17f * loud)
                          : 127.0f * h.strength * laneScale (h.lane) * (0.42f + 0.58f * loud);
        v *= 1.0f + vNoise * 0.12f * s.humanize;

        Note n;
        n.pitch = h.pitch;
        n.velocity = juce::jlimit (1, 127, juce::roundToInt (v));
        n.start = juce::jlimit ((juce::int64) 0, total - 1, t);
        n.length = juce::jmax ((juce::int64) 1, juce::jmin ((juce::int64) ticksPerQuarter / 8, total - n.start));
        notes.push_back (n);
    }

    // One hit per drum per moment: a ghost under a backbeat, a kick under a fill accent.
    std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b)
    {
        return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
    });
    std::vector<Note> out;
    for (auto& n : notes)
    {
        bool merged = false;
        for (auto it = out.rbegin(); it != out.rend() && n.start - it->start < 20; ++it)
            if (it->pitch == n.pitch)
            {
                it->velocity = juce::jmax (it->velocity, n.velocity);
                merged = true;
                break;
            }
        if (! merged)
            out.push_back (n);
    }
    return out;
}

//==============================================================================
bool isDrummerRegion (const juce::ValueTree& region)
{
    return Project::isMidiRegion (region) && region.hasProperty (property);
}

Settings settingsOf (const juce::ValueTree& region)
{
    return Settings::fromJson (region[property].toString());
}

void apply (Project& project, juce::ValueTree region, const Settings& s, juce::UndoManager* undo)
{
    if (! region.isValid())
        return;

    // The region keeps its length (splits and trims stay put); the groove fills it from the
    // content origin, and `bars` records how many bars that is.
    const auto grid = project.grid();
    const Meter meter { grid.beatsPerBar, grid.beatUnit };
    const auto length = (juce::int64) region[ids::length];
    const double originTick = grid.tickAt ((double) Project::contentOrigin (region));
    const double startTick = grid.tickAt ((double) (juce::int64) region[ids::start]);
    const double endTick = grid.tickAt ((double) ((juce::int64) region[ids::start] + length));

    auto generated = s;
    generated.bars = juce::jmax (1, (int) std::ceil ((endTick - originTick) / (double) meter.barTicks() - 1.0e-6));
    replaceRegionNotes (region, generate (generated, meter), undo);

    auto stored = s;
    stored.bars = juce::jlimit (1, 64, juce::roundToInt (grid.barAt (endTick) - grid.barAt (startTick)));
    region.setProperty (property, stored.toJson(), undo);
    // The Drummer writes it now, not the step sequencer.
    if (region.hasProperty (StepPattern::property))
        region.removeProperty (StepPattern::property, undo);
}

} // namespace drummer
} // namespace woodshed::daw
