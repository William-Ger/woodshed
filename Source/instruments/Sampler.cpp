#include "Sampler.h"
#include "InstrumentDsp.h"
#include "../engine/PitchDetector.h"

namespace woodshed::sampler
{

//==============================================================================
// Names

juce::String modeId (Mode m)
{
    switch (m)
    {
        case Mode::oneShot: return "oneshot";
        case Mode::slice:   return "slice";
        case Mode::multi:   return "multi";
        case Mode::classic:
        default:            return "classic";
    }
}

juce::String modeName (Mode m)
{
    switch (m)
    {
        case Mode::oneShot: return "One Shot";
        case Mode::slice:   return "Slice";
        case Mode::multi:   return "Multi";
        case Mode::classic:
        default:            return "Classic";
    }
}

Mode modeFrom (const juce::String& id)
{
    if (id == "oneshot") return Mode::oneShot;
    if (id == "slice")   return Mode::slice;
    if (id == "multi")   return Mode::multi;
    return Mode::classic;
}

juce::String loopId (LoopMode m)
{
    return m == LoopMode::forward ? "forward" : m == LoopMode::alternate ? "alternate" : "off";
}

LoopMode loopFrom (const juce::String& id)
{
    return id == "forward" ? LoopMode::forward : id == "alternate" ? LoopMode::alternate : LoopMode::off;
}

Zone Zone::fromTree (const juce::ValueTree& t)
{
    Zone z;
    z.file = t[ids::file].toString();
    z.name = t[ids::name].toString();
    z.root = juce::jlimit (0, 127, (int) t.getProperty (ids::root, 60));
    z.lo = juce::jlimit (0, 127, (int) t.getProperty (ids::lo, 0));
    z.hi = juce::jlimit (z.lo, 127, (int) t.getProperty (ids::hi, 127));
    z.velLo = juce::jlimit (1, 127, (int) t.getProperty (ids::velLo, 1));
    z.velHi = juce::jlimit (z.velLo, 127, (int) t.getProperty (ids::velHi, 127));
    z.start = (juce::int64) t.getProperty (ids::start, 0);
    z.end = (juce::int64) t.getProperty (ids::end, -1);
    z.loop = loopFrom (t[ids::loop].toString());
    z.loopStart = (juce::int64) t.getProperty (ids::loopStart, 0);
    z.loopEnd = (juce::int64) t.getProperty (ids::loopEnd, 0);
    z.crossfade = (juce::int64) t.getProperty (ids::crossfade, 0);
    z.tune = (float) (double) t.getProperty (ids::tune, 0.0);
    z.fine = (float) (double) t.getProperty (ids::fine, 0.0);
    z.gain = (float) (double) t.getProperty (ids::gain, 0.0);
    z.pan = juce::jlimit (-1.0f, 1.0f, (float) (double) t.getProperty (ids::pan, 0.0));
    z.group = juce::jlimit (0, 63, (int) t.getProperty (ids::group, 0));
    return z;
}

juce::ValueTree Zone::toTree() const
{
    juce::ValueTree t (ids::ZONE);
    t.setProperty (ids::file, file, nullptr);
    t.setProperty (ids::name, name, nullptr);
    t.setProperty (ids::root, root, nullptr);
    t.setProperty (ids::lo, lo, nullptr);
    t.setProperty (ids::hi, hi, nullptr);
    t.setProperty (ids::velLo, velLo, nullptr);
    t.setProperty (ids::velHi, velHi, nullptr);
    t.setProperty (ids::start, start, nullptr);
    t.setProperty (ids::end, end, nullptr);
    t.setProperty (ids::loop, loopId (loop), nullptr);
    t.setProperty (ids::loopStart, loopStart, nullptr);
    t.setProperty (ids::loopEnd, loopEnd, nullptr);
    t.setProperty (ids::crossfade, crossfade, nullptr);
    t.setProperty (ids::tune, tune, nullptr);
    t.setProperty (ids::fine, fine, nullptr);
    t.setProperty (ids::gain, gain, nullptr);
    t.setProperty (ids::pan, pan, nullptr);
    t.setProperty (ids::group, group, nullptr);
    return t;
}

//==============================================================================
// Files

namespace
{
juce::AudioFormatManager& formats()
{
    static juce::AudioFormatManager f;
    static const bool registered = [] { f.registerBasicFormats(); return true; }();
    juce::ignoreUnused (registered);
    return f;
}

juce::CriticalSection cacheLock;      // the caches are used from the message thread; this is belt and braces

juce::String fileKey (const juce::File& f)
{
    return f.getFullPathName() + "|" + juce::String (f.getLastModificationTime().toMilliseconds()) + "|" + juce::String (f.getSize());
}
}

std::shared_ptr<const Source> loadSource (const juce::File& file)
{
    static std::map<juce::String, std::weak_ptr<const Source>> cache;
    const juce::ScopedLock sl (cacheLock);

    const auto key = fileKey (file);
    if (auto it = cache.find (key); it != cache.end())
        if (auto alive = it->second.lock())
            return alive;

    std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > (juce::int64) std::numeric_limits<int>::max() / 2)
        return nullptr;

    auto src = std::make_shared<Source>();
    const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
    src->rate = reader->sampleRate > 0.0 ? reader->sampleRate : 48000.0;
    src->audio.setSize (channels, (int) reader->lengthInSamples);
    if (! reader->read (&src->audio, 0, (int) reader->lengthInSamples, 0, true, channels > 1))
        return nullptr;

    // Drop old entries whose sources have gone.
    for (auto it = cache.begin(); it != cache.end();)
        it = it->second.expired() ? cache.erase (it) : std::next (it);
    cache[key] = src;
    return src;
}

juce::String audioFileWildcard()
{
    return "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.caf;*.m4a";
}

bool isAudioFile (const juce::File& f)
{
    return f.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;caf;m4a");
}

juce::String importIntoProject (const juce::File& file, const juce::File& projectFolder)
{
    if (file.isAChildOf (projectFolder))
        return file.getRelativePathFrom (projectFolder).replaceCharacter ('\\', '/');

    auto audio = projectFolder.getChildFile ("Audio");
    audio.createDirectory();
    auto to = audio.getNonexistentChildFile (file.getFileNameWithoutExtension(), file.getFileExtension(), false);
    if (! file.copyFileTo (to))
        return {};
    return to.getRelativePathFrom (projectFolder).replaceCharacter ('\\', '/');
}

//==============================================================================
// Resampling: windowed-sinc interpolation over octave-down copies of each sample.
//
// A voice reads the copy (level) at which it steps at most two frames per output sample, through a
// 64-tap Kaiser-windowed sinc whose cutoff is lowered to 1/step when it steps faster than one, so
// transposing up doesn't fold the top of the sample back down as aliases.

namespace
{
constexpr int taps = 64, halfTaps = 32, phases = 256, bands = 16;
constexpr int pad = 64;                 // zeros around each level, so reads never leave the buffer
constexpr int decimTaps = 95;           // the half-band-ish lowpass that makes each level
constexpr int maxLevels = 6;
constexpr int loopGuard = 64;           // frames after a loop's end that repeat its start, for the kernel

double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1.0e-12 * sum)
            break;
    }
    return sum;
}

double kaiser (double x, double halfWidth, double beta)
{
    const double r = x / halfWidth;
    return std::abs (r) >= 1.0 ? 0.0 : besselI0 (beta * std::sqrt (1.0 - r * r)) / besselI0 (beta);
}

double sinc (double x)
{
    return std::abs (x) < 1.0e-9 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
}

/** The step at the top of each band: band 0 is any step up to 1, the others share (1, 2]. */
double bandTop (int band)                       { return 1.0 + (double) band / (double) (bands - 1); }

struct Kernels
{
    std::vector<float> table;           // [band][phase 0..phases][tap]

    Kernels() : table ((size_t) bands * (phases + 1) * taps)
    {
        for (int b = 0; b < bands; ++b)
        {
            // Faster than one frame a step, the cutoff sits a little under 1/step so the kernel's
            // transition band (about 0.12 of Nyquist wide) ends close to the fold-over point.
            const double fc = b == 0 ? 1.0 : 1.0 / bandTop (b) - 0.03;
            for (int p = 0; p <= phases; ++p)
            {
                const double frac = (double) p / phases;
                auto* row = table.data() + ((size_t) b * (phases + 1) + (size_t) p) * taps;
                double sum = 0.0;
                for (int j = 0; j < taps; ++j)
                {
                    const double d = (double) (j - (halfTaps - 1)) - frac;
                    const double h = fc * sinc (fc * d) * kaiser (d, halfTaps, 5.65);
                    row[j] = (float) h;
                    sum += h;
                }
                for (int j = 0; j < taps; ++j)
                    row[j] = (float) (row[j] / sum);
            }
        }
    }

    const float* at (int band, int phase) const { return table.data() + ((size_t) band * (phases + 1) + (size_t) phase) * taps; }
};

const Kernels& kernels()
{
    static const Kernels k;
    return k;
}

const std::vector<float>& decimationFilter()
{
    static const std::vector<float> h = []
    {
        std::vector<float> c ((size_t) decimTaps);
        const int mid = decimTaps / 2;
        double sum = 0.0;
        for (int i = 0; i < decimTaps; ++i)
        {
            const double d = i - mid;
            const double v = 0.45 * sinc (0.45 * d) * kaiser (d, mid + 1, 7.86);
            c[(size_t) i] = (float) v;
            sum += v;
        }
        for (auto& v : c) v = (float) (v / sum);
        return c;
    }();
    return h;
}
} // namespace

/** A zone's audio ready to play: the sample (with its loop baked in) and its octave-down copies. */
struct Playable
{
    struct Level
    {
        std::array<std::vector<float>, 2> ch;
        juce::int64 frames = 0;
        const float* data (int c) const         { return ch[(size_t) c].data() + pad; }
    };

    std::vector<Level> levels;
    int channels = 1;
    double rate = 48000.0;

    /** frames: how much of `src` (from its start) to keep. */
    static std::shared_ptr<const Playable> make (const juce::AudioBuffer<float>& src, double rate, juce::int64 frames,
                                                 LoopMode loop, juce::int64 loopStart, juce::int64 loopEnd, juce::int64 crossfade)
    {
        auto p = std::make_shared<Playable>();
        p->channels = juce::jlimit (1, 2, src.getNumChannels());
        p->rate = rate;

        const bool looping = loop != LoopMode::off && loopEnd - loopStart >= 32;
        const juce::int64 length = looping ? loopEnd + loopGuard : frames;

        Level l0;
        l0.frames = length;
        for (int c = 0; c < p->channels; ++c)
        {
            auto& v = l0.ch[(size_t) c];
            v.assign ((size_t) (length + 2 * pad), 0.0f);
            const auto copy = juce::jmin (length, (juce::int64) src.getNumSamples());
            std::copy (src.getReadPointer (c), src.getReadPointer (c) + copy, v.begin() + pad);
            float* d = v.data() + pad;

            if (looping)
            {
                const auto* s = src.getReadPointer (c);
                const auto n = (juce::int64) src.getNumSamples();
                auto at = [&] (juce::int64 i) { return i >= 0 && i < n ? s[i] : 0.0f; };

                // An equal-power crossfade into the loop's end from just before its start, so the
                // jump back is seamless; then the guard after the end for the kernel to read.
                const auto xf = juce::jlimit ((juce::int64) 0, juce::jmin (loopStart, (loopEnd - loopStart) / 2), crossfade);
                for (juce::int64 i = 0; i < xf; ++i)
                {
                    const double t = ((double) i + 0.5) / (double) xf;
                    const auto a = (float) std::cos (t * juce::MathConstants<double>::halfPi);
                    const auto b = (float) std::sin (t * juce::MathConstants<double>::halfPi);
                    d[loopEnd - xf + i] = a * at (loopEnd - xf + i) + b * at (loopStart - xf + i);
                }
                for (juce::int64 k = 0; k < loopGuard; ++k)
                    d[loopEnd + k] = loop == LoopMode::forward ? d[loopStart + k]
                                                               : d[juce::jmax (loopStart, loopEnd - k)];
            }
        }
        p->levels.push_back (std::move (l0));

        // Octave-down copies, each through the decimation lowpass.
        const auto& h = decimationFilter();
        const int mid = decimTaps / 2;
        while ((int) p->levels.size() < maxLevels && p->levels.back().frames > 256)
        {
            const auto& prev = p->levels.back();
            Level next;
            next.frames = (prev.frames + 1) / 2;
            for (int c = 0; c < p->channels; ++c)
            {
                auto& v = next.ch[(size_t) c];
                v.assign ((size_t) (next.frames + 2 * pad), 0.0f);
                const auto& in = prev.ch[(size_t) c];
                const auto inSize = (juce::int64) in.size();
                for (juce::int64 m = 0; m < next.frames; ++m)
                {
                    const juce::int64 centre = pad + 2 * m;
                    float acc = 0.0f;
                    for (int i = 0; i < decimTaps; ++i)
                    {
                        const auto idx = centre + i - mid;
                        if (idx >= 0 && idx < inSize)
                            acc += h[(size_t) i] * in[(size_t) idx];
                    }
                    v[(size_t) (pad + m)] = acc;
                }
            }
            p->levels.push_back (std::move (next));
        }
        return p;
    }
};

namespace
{
std::shared_ptr<const Playable> playableFor (const juce::File& file, const Source& src, LoopMode loop,
                                             juce::int64 loopStart, juce::int64 loopEnd, juce::int64 crossfade)
{
    static std::map<juce::String, std::weak_ptr<const Playable>> cache;
    const juce::ScopedLock sl (cacheLock);

    const bool looping = loop != LoopMode::off;
    const auto key = fileKey (file) + (looping ? "|" + loopId (loop) + ":" + juce::String (loopStart) + ":" + juce::String (loopEnd)
                                                 + ":" + juce::String (crossfade)
                                               : juce::String ("|raw"));
    if (auto it = cache.find (key); it != cache.end())
        if (auto alive = it->second.lock())
            return alive;

    auto p = Playable::make (src.audio, src.rate, src.length(), loop, loopStart, loopEnd, crossfade);
    for (auto it = cache.begin(); it != cache.end();)
        it = it->second.expired() ? cache.erase (it) : std::next (it);
    cache[key] = p;
    return p;
}
}

//==============================================================================
// Analysis

namespace
{
std::vector<float> monoOf (const Source& src, juce::int64 from, juce::int64 to)
{
    from = juce::jlimit ((juce::int64) 0, src.length(), from);
    to = juce::jlimit (from, src.length(), to < 0 ? src.length() : to);
    std::vector<float> m ((size_t) (to - from), 0.0f);
    const float g = 1.0f / (float) src.audio.getNumChannels();
    for (int c = 0; c < src.audio.getNumChannels(); ++c)
    {
        const auto* s = src.audio.getReadPointer (c) + from;
        for (size_t i = 0; i < m.size(); ++i)
            m[i] += g * s[i];
    }
    return m;
}
}

Pitch detectPitch (const Source& src, juce::int64 from, juce::int64 to)
{
    Pitch result;
    const auto mono = monoOf (src, from, to);
    const int n = (int) mono.size();
    if (n < 512)
        return result;

    float peak = 0.0f;
    for (auto v : mono) peak = juce::jmax (peak, std::abs (v));
    if (peak <= 1.0e-5f)
        return result;

    // Windows through the body of the sound (past the attack), at most a couple of seconds' worth.
    PitchDetector detector;
    const int window = juce::jmin (PitchDetector::windowSize, n);
    const int first = juce::jmin (n - window, n / 10);
    const int last = juce::jmin (n - window, first + (int) (src.rate * 2.0));
    std::vector<float> found;
    int tried = 0;
    for (int at = first; at <= last; at += juce::jmax (256, window / 2))
    {
        ++tried;
        const auto r = detector.analyse (mono.data() + at, window, src.rate, 30.0f, 2000.0f);
        if (r.frequency > 0.0f && r.clarity > 0.85f && r.level > peak * 0.02f)
            found.push_back (r.frequency);
    }
    if (found.empty() || (int) found.size() * 3 < tried)
        return result;

    std::sort (found.begin(), found.end());
    const float hz = found[found.size() / 2];
    const float midi = 69.0f + 12.0f * std::log2 (hz / 440.0f);
    result.note = juce::jlimit (0, 127, juce::roundToInt (midi));
    result.cents = (midi - (float) result.note) * 100.0f;
    result.found = true;
    return result;
}

std::vector<juce::int64> detectSlices (const Source& src, juce::int64 from, juce::int64 to, float sensitivity)
{
    from = juce::jlimit ((juce::int64) 0, src.length(), from);
    to = juce::jlimit (from, src.length(), to < 0 ? src.length() : to);
    std::vector<juce::int64> slices { from };
    const auto mono = monoOf (src, from, to);
    const int hop = juce::jmax (32, (int) std::lround (src.rate * 0.005));
    const int win = hop * 2;
    const int frames = ((int) mono.size() - win) / hop;
    if (frames < 4)
        return slices;

    // Level per 5 ms frame, the waveform plus its slope so bright hits (hats) count too.
    std::vector<float> db ((size_t) frames);
    float top = -200.0f;
    for (int k = 0; k < frames; ++k)
    {
        double e = 0.0;
        const auto* x = mono.data() + (size_t) k * (size_t) hop;
        for (int i = 1; i < win; ++i)
        {
            const double d = x[i] - x[i - 1];
            e += (double) x[i] * x[i] + d * d;
        }
        db[(size_t) k] = (float) (10.0 * std::log10 (e / win + 1.0e-12));
        top = juce::jmax (top, db[(size_t) k]);
    }

    const float s = juce::jlimit (0.0f, 1.0f, sensitivity);
    const float rise = juce::jmap (s, 15.0f, 3.0f);
    const float floor = top - juce::jmap (s, 30.0f, 60.0f);
    const int minGap = juce::jmax (1, (int) (juce::jmap (s, 0.12f, 0.03f) * src.rate / hop));

    int last = 0;
    for (int k = 3; k < frames; ++k)
    {
        const float before = juce::jmin (db[(size_t) k - 1], db[(size_t) k - 2], db[(size_t) k - 3]);
        if (db[(size_t) k] < floor || db[(size_t) k] - before < rise || k - last < minGap)
            continue;

        // The hit itself: the first sample near the frame that reaches a quarter of its peak, less 1 ms.
        const int lo = juce::jmax (0, (k - 1) * hop), hi = juce::jmin ((int) mono.size(), k * hop + win);
        float p = 0.0f;
        for (int i = lo; i < hi; ++i) p = juce::jmax (p, std::abs (mono[(size_t) i]));
        int at = lo;
        while (at < hi && std::abs (mono[(size_t) at]) < 0.25f * p) ++at;
        const auto pos = from + juce::jmax ((juce::int64) 0, (juce::int64) at - (juce::int64) (src.rate * 0.001));
        if (pos - slices.back() >= (juce::int64) minGap * hop)
            slices.push_back (pos);
        last = k;
    }
    return slices;
}

//==============================================================================
// Editing

juce::ValueTree samplerNode (juce::ValueTree instrumentNode, juce::UndoManager* undo)
{
    return instrumentNode.getOrCreateChildWithName (ids::SAMPLER, undo);
}

namespace
{
void removeZones (juce::ValueTree sampler, juce::UndoManager* undo)
{
    for (int i = sampler.getNumChildren(); --i >= 0;)
        if (sampler.getChild (i).hasType (ids::ZONE))
            sampler.removeChild (i, undo);
}

juce::String nameFor (const juce::String& relativeFile)
{
    return juce::File::createFileWithoutCheckingPath (relativeFile.replaceCharacter ('\\', '/')).getFileNameWithoutExtension();
}

void addSlices (juce::ValueTree sampler, const juce::String& file, const Source& src, juce::int64 from, juce::int64 to,
                juce::UndoManager* undo)
{
    const auto sens = (float) (double) sampler.getProperty (ids::sensitivity, 0.5);
    auto cuts = detectSlices (src, from, to, sens);
    if ((int) cuts.size() > 128 - firstSliceNote)
        cuts.resize ((size_t) (128 - firstSliceNote));
    for (size_t i = 0; i < cuts.size(); ++i)
    {
        Zone z;
        z.file = file;
        z.name = nameFor (file) + " " + juce::String ((int) i + 1);
        z.root = z.lo = z.hi = firstSliceNote + (int) i;
        z.start = cuts[i];
        z.end = i + 1 < cuts.size() ? cuts[i + 1] : to;
        sampler.appendChild (z.toTree(), undo);
    }
}
}

void loadSample (juce::ValueTree sampler, Mode mode, const juce::String& relativeFile, const Source& src,
                 juce::int64 from, juce::int64 to, juce::UndoManager* undo)
{
    from = juce::jlimit ((juce::int64) 0, src.length(), from);
    to = juce::jlimit (from, src.length(), to < 0 ? src.length() : to);
    sampler.setProperty (ids::mode, modeId (mode), undo);

    if (mode == Mode::slice)
    {
        removeZones (sampler, undo);
        addSlices (sampler, relativeFile, src, from, to, undo);
        return;
    }

    Zone z;
    z.file = relativeFile;
    z.name = nameFor (relativeFile);
    z.start = from;
    z.end = to;
    if (mode != Mode::oneShot)
        z.root = detectPitch (src, from, to).note;

    if (mode != Mode::multi)
    {
        removeZones (sampler, undo);
        sampler.appendChild (z.toTree(), undo);
        return;
    }

    // Multi: the new zone joins the others, and the keyboard is shared out between their roots.
    sampler.appendChild (z.toTree(), undo);
    std::vector<juce::ValueTree> zones;
    for (auto c : sampler)
        if (c.hasType (ids::ZONE))
            zones.push_back (c);
    std::stable_sort (zones.begin(), zones.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b)
                      { return (int) a[ids::root] < (int) b[ids::root]; });
    for (size_t i = 0; i < zones.size(); ++i)
    {
        const int root = zones[i][ids::root];
        const int lo = i == 0 ? 0 : ((int) zones[i - 1][ids::root] + root) / 2 + 1;
        const int hi = i + 1 == zones.size() ? 127 : (root + (int) zones[i + 1][ids::root]) / 2;
        zones[i].setProperty (ids::lo, juce::jmin (lo, root), undo);
        zones[i].setProperty (ids::hi, juce::jmax (hi, root), undo);
    }
}

void reslice (juce::ValueTree sampler, const Source& src, juce::UndoManager* undo)
{
    juce::String file;
    juce::int64 from = -1, to = -1;
    for (auto c : sampler)
        if (c.hasType (ids::ZONE))
        {
            const auto z = Zone::fromTree (c);
            if (file.isEmpty())
            {
                file = z.file;
                from = z.start;
            }
            if (z.file == file)
                to = juce::jmax (to, z.end < 0 ? src.length() : z.end);
        }
    if (file.isEmpty())
        return;
    removeZones (sampler, undo);
    addSlices (sampler, file, src, from, to, undo);
}

//==============================================================================
// The instrument

namespace
{
using namespace idsp;

/** A zone as the audio thread plays it. */
struct ZonePlay
{
    std::shared_ptr<const Playable> audio;
    juce::int64 fileHash = 0;
    int root = 60, lo = 0, hi = 127, velLo = 1, velHi = 127, group = 0;
    double start = 0.0, end = 0.0, loopStart = 0.0, loopEnd = 0.0;
    LoopMode loop = LoopMode::off;
    float tune = 0.0f, gain = 1.0f, pan = 0.0f;
};

/** Everything the audio thread needs to play a SAMPLER node. Immutable once published. */
struct Map
{
    std::vector<ZonePlay> zones;
    Mode mode = Mode::classic;
    bool gate = false;

    bool ignoresNoteOff() const                 { return mode == Mode::oneShot || (mode == Mode::slice && ! gate); }
    bool loops() const                          { return mode != Mode::oneShot && mode != Mode::slice; }
};

//==============================================================================
// Built-in sounds, generated once: what the Sampler plays before a sample is loaded.

/** One second of loop holds exactly `cycles` periods of every partial, so it repeats seamlessly. */
Map builtInMap (int which)
{
    constexpr double rate = 48000.0;
    constexpr int cycles = 262;                 // 262 Hz: middle C, 2.4 cents sharp (fine-tuned back below)
    const bool stereo = which == 1;
    const int length = which == 2 ? 96000 : 72000;
    juce::AudioBuffer<float> audio (stereo ? 2 : 1, length);
    audio.clear();

    for (int c = 0; c < audio.getNumChannels(); ++c)
    {
        auto* d = audio.getWritePointer (c);
        for (int i = 0; i < length; ++i)
        {
            const double t = i / rate;
            double v = 0.0;
            if (which == 0)
            {
                // Keys: a bright strike settling into a mellow tone.
                const double strike = std::exp (-t * 7.0);
                for (int h = 1; h <= 8; ++h)
                {
                    const double a = (h == 1 ? 1.0 : 0.5 / std::pow ((double) h, 1.3)) * (h <= 3 ? 1.0 : strike);
                    v += a * std::sin (juce::MathConstants<double>::twoPi * cycles * h * t);
                }
                v *= 0.09;
            }
            else if (which == 1)
            {
                // Pad: two saw-ish tones a cycle apart, beating slowly, spread across the channels.
                const int detune = c == 0 ? 1 : -1;
                for (int h = 1; h <= 12; ++h)
                {
                    const double a = 1.0 / h;
                    v += a * (std::sin (juce::MathConstants<double>::twoPi * cycles * h * t)
                              + 0.8 * std::sin (juce::MathConstants<double>::twoPi * (cycles + detune) * h * t + h));
                }
                v *= 0.075;
            }
            else
            {
                // Pluck: a decaying string, upper partials dying first.
                for (int h = 1; h <= 10; ++h)
                    v += (1.0 / h) * std::exp (-t * (1.5 + 0.9 * h)) * std::sin (juce::MathConstants<double>::twoPi * cycles * h * t);
                v *= 0.08;
            }
            d[i] = (float) v;
        }
    }

    // The attack ramps in over a millisecond so nothing clicks.
    for (int i = 0; i < 48; ++i)
        for (int c = 0; c < audio.getNumChannels(); ++c)
            audio.getWritePointer (c)[i] *= (float) i / 48.0f;

    Map m;
    ZonePlay z;
    const bool loop = which != 2;
    z.audio = Playable::make (audio, rate, length, loop ? LoopMode::forward : LoopMode::off, 24000, 72000, 0);
    z.root = 60;
    z.start = 0.0;
    z.end = length;
    z.loop = loop ? LoopMode::forward : LoopMode::off;
    z.loopStart = 24000.0;
    z.loopEnd = 72000.0;
    z.tune = -0.02476f;                  // 262 Hz back to 261.63 Hz (2.48 cents flat)
    z.fileHash = -1 - which;
    m.zones.push_back (std::move (z));
    return m;
}

const Map& builtIn (int which)
{
    static const std::array<Map, 3> maps { builtInMap (0), builtInMap (1), builtInMap (2) };
    return maps[(size_t) juce::jlimit (0, 2, which)];
}

constexpr int maxSlots = 48;                // voices sounding plus voices fading out after a steal
constexpr int maxPolyphony = 32;
constexpr int modChunk = 16;                // samples between modulation updates

//==============================================================================
class SamplerInstrument final : public Instrument
{
public:
    SamplerInstrument()
    {
        builtIn (0);                        // generate the built-in sounds now, not on the audio thread
    }

    ~SamplerInstrument() override
    {
        drainRetired();
        delete pending.exchange (nullptr);
        delete active;
        if (fading != active)
            delete fading;
    }

    void reset() override
    {
        for (auto& v : slots)
            v.active = false;
        heldCount = 0;
        sustainDown = false;
        bendTarget = bendNow = 0.0f;
        modValue = 0.0f;
        lfoPhase = 0.0f;
        lastNote = -1;
        roundRobin.fill (0);
    }

    void update() override
    {
        auto& p = params;
        p.attack = juce::jlimit (0.0005f, 20.0f, knob (param::attack) * 0.001f);
        p.decay = juce::jlimit (0.005f, 30.0f, knob (param::decay) * 0.001f);
        p.sustain = juce::jlimit (0.0f, 1.0f, knob (param::sustain));
        p.release = juce::jlimit (0.005f, 20.0f, knob (param::release) * 0.001f);
        p.filterType = juce::jlimit (0, 3, juce::roundToInt (knob (param::filterType)));
        p.cutoff = juce::jlimit (20.0f, 20000.0f, knob (param::cutoff));
        p.resonance = juce::jlimit (0.0f, 1.0f, knob (param::resonance));
        p.envAmount = juce::jlimit (-1.0f, 1.0f, knob (param::envAmount));
        p.fAttack = juce::jlimit (0.0005f, 20.0f, knob (param::filterAttack) * 0.001f);
        p.fDecay = juce::jlimit (0.005f, 30.0f, knob (param::filterDecay) * 0.001f);
        p.fSustain = juce::jlimit (0.0f, 1.0f, knob (param::filterSustain));
        p.fRelease = juce::jlimit (0.005f, 20.0f, knob (param::filterRelease) * 0.001f);
        p.pitchEnv = juce::jlimit (-48.0f, 48.0f, knob (param::pitchEnv));
        p.pitchTime = juce::jlimit (0.001f, 10.0f, knob (param::pitchTime) * 0.001f);
        p.lfoRate = juce::jlimit (0.01f, 40.0f, knob (param::lfoRate));
        p.lfoPitch = juce::jlimit (0.0f, 12.0f, knob (param::lfoPitch));
        p.lfoFilter = juce::jlimit (0.0f, 1.0f, knob (param::lfoFilter));
        p.lfoAmp = juce::jlimit (0.0f, 1.0f, knob (param::lfoAmp));
        p.glide = juce::jlimit (0.0f, 5.0f, knob (param::glide) * 0.001f);
        p.voiceMode = juce::jlimit (0, 2, juce::roundToInt (knob (param::voiceMode)));
        p.voices = juce::jlimit (1, maxPolyphony, juce::roundToInt (knob (param::voices)));
        p.velocity = juce::jlimit (0.0f, 1.0f, knob (param::velocity));
        p.level = dbToGain (juce::jlimit (-60.0f, 12.0f, knob (param::level)));
        p.builtIn = juce::jlimit (0, 2, juce::roundToInt (knob (param::builtIn)));
    }

    void render (float* left, float* right, int numSamples, const NoteEvent* events, int numEvents) override
    {
        takePendingMap();

        const int chunk = (int) scratchL.size();
        int start = 0, e = 0;
        while (chunk > 0 && start < numSamples)
        {
            const int n = juce::jmin (chunk, numSamples - start);
            std::fill (scratchL.begin(), scratchL.begin() + n, 0.0f);
            std::fill (scratchR.begin(), scratchR.begin() + n, 0.0f);

            int done = 0;
            for (; e < numEvents && events[e].offset < start + n; ++e)
            {
                const int until = juce::jlimit (done, n, events[e].offset - start);
                if (until > done)
                    renderVoices (scratchL.data() + done, scratchR.data() + done, until - done);
                done = until;
                handle (events[e]);
            }
            if (n > done)
                renderVoices (scratchL.data() + done, scratchR.data() + done, n - done);

            // No safety clip: a sample plays back exactly as recorded (Convert to Sampler Track relies on it).
            juce::FloatVectorOperations::add (left + start, scratchL.data(), n);
            juce::FloatVectorOperations::add (right + start, scratchR.data(), n);
            start += n;
        }
        for (; e < numEvents; ++e)
            handle (events[e]);
    }

    void syncState (const juce::ValueTree& instrumentNode, const juce::File& projectFolder) override
    {
        drainRetired();
        const auto node = instrumentNode.getChildWithName (ids::SAMPLER);
        if (synced && node.isEquivalentTo (applied) && projectFolder == appliedFolder)
            return;

        synced = true;
        applied = node.createCopy();
        appliedFolder = projectFolder;
        publish (buildMap (node, projectFolder));
    }

protected:
    void prepared() override
    {
        scratchL.assign ((size_t) juce::jmax (64, maxBlock), 0.0f);
        scratchR.assign ((size_t) juce::jmax (64, maxBlock), 0.0f);
        update();
    }

private:
    //==========================================================================
    struct Params
    {
        float attack = 0.002f, decay = 0.3f, sustain = 1.0f, release = 0.2f;
        int filterType = 0;
        float cutoff = 20000.0f, resonance = 0.0f, envAmount = 0.0f;
        float fAttack = 0.002f, fDecay = 0.3f, fSustain = 1.0f, fRelease = 0.2f;
        float pitchEnv = 0.0f, pitchTime = 0.1f;
        float lfoRate = 5.0f, lfoPitch = 0.0f, lfoFilter = 0.0f, lfoAmp = 0.0f;
        float glide = 0.0f;
        int voiceMode = 0, voices = 16;
        float velocity = 0.5f, level = 1.0f;
        int builtIn = 0;
    };

    struct Voice
    {
        bool active = false, held = false, sustained = false, releasing = false, killing = false;
        int note = -1;
        float velocity = 1.0f;
        juce::uint64 age = 0;

        const Map* map = nullptr;
        const ZonePlay* zone = nullptr;
        int zoneIndex = 0;

        double pos = 0.0;
        int dir = 1;
        float pitchNow = 60.0f, pitchTarget = 60.0f;
        float elapsed = 0.0f;               // seconds since the start, for the pitch envelope
        float fadeIn = 1.0f, kill = 1.0f;
        Adsr amp, filt;
        Svf svf[2];
    };

    //==========================================================================
    // State hand-off. The message thread publishes a new Map through `pending`; the audio
    // thread takes it at the start of a block and hands the old one back through `retired`
    // for deletion on the message thread. Voices playing a zone that's still there carry on
    // (so dragging a loop marker while a note holds doesn't cut it); others fade out over a
    // few milliseconds on the old map, which is kept until they've gone.

    std::unique_ptr<Map> buildMap (const juce::ValueTree& node, const juce::File& folder)
    {
        auto map = std::make_unique<Map>();
        map->mode = modeFrom (node[ids::mode].toString());
        map->gate = (bool) node.getProperty (ids::gate, false);

        for (auto c : node)
        {
            if (! c.hasType (ids::ZONE) || (int) map->zones.size() >= maxZones)
                continue;
            const auto z = Zone::fromTree (c);
            if (z.file.isEmpty())
                continue;
            const auto file = juce::File::isAbsolutePath (z.file) ? juce::File (z.file) : folder.getChildFile (z.file);
            auto src = loadSource (file);
            if (src == nullptr)
                continue;

            ZonePlay p;
            const auto length = src->length();
            const auto end = z.end < 0 || z.end > length ? length : z.end;
            const auto start = juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, end - 1), z.start);
            auto loopStart = juce::jlimit (start, end, z.loopStart);
            auto loopEnd = juce::jlimit (loopStart, end, z.loopEnd);
            auto loop = z.loop;
            if (loopEnd - loopStart < 32)
                loop = LoopMode::off;
            if (loop == LoopMode::off)
                loopStart = loopEnd = 0;

            p.audio = playableFor (file, *src, loop, loopStart, loopEnd, z.crossfade);
            p.fileHash = file.getFullPathName().hashCode64();
            p.root = z.root;
            p.lo = z.lo;
            p.hi = z.hi;
            p.velLo = z.velLo;
            p.velHi = z.velHi;
            p.group = z.group;
            p.start = (double) start;
            p.end = (double) end;
            p.loop = loop;
            p.loopStart = (double) loopStart;
            p.loopEnd = (double) loopEnd;
            p.tune = z.tune + z.fine * 0.01f;
            p.gain = dbToGain (juce::jlimit (-60.0f, 24.0f, z.gain));
            p.pan = z.pan;
            map->zones.push_back (std::move (p));
        }
        return map;
    }

    void publish (std::unique_ptr<Map> next)
    {
        delete pending.exchange (next.release());       // a map the audio thread never took
    }

    void drainRetired()
    {
        const auto scope = retireFifo.read (retireFifo.getNumReady());
        for (int i = 0; i < scope.blockSize1; ++i) delete retired[(size_t) (scope.startIndex1 + i)];
        for (int i = 0; i < scope.blockSize2; ++i) delete retired[(size_t) (scope.startIndex2 + i)];
    }

    void retire (Map* m)
    {
        if (m == nullptr)
            return;
        const auto scope = retireFifo.write (1);
        if (scope.blockSize1 > 0)       retired[(size_t) scope.startIndex1] = m;
        else if (scope.blockSize2 > 0)  retired[(size_t) scope.startIndex2] = m;
        // (takePendingMap only swaps while there's room, so this always lands.)
    }

    bool anyVoiceOn (const Map* m) const
    {
        for (auto& v : slots)
            if (v.active && v.map == m)
                return true;
        return false;
    }

    void takePendingMap()
    {
        if (fading != nullptr)
        {
            if (anyVoiceOn (fading))
                return;
            retire (fading);
            fading = nullptr;
        }
        if (pending.load (std::memory_order_acquire) == nullptr || retireFifo.getFreeSpace() < 2)
            return;

        Map* next = pending.exchange (nullptr);
        Map* old = active;
        active = next;
        if (old == nullptr)
            return;

        for (auto& v : slots)
        {
            if (! v.active || v.map != old)
                continue;
            if (v.zoneIndex < (int) next->zones.size() && next->zones[(size_t) v.zoneIndex].fileHash == v.zone->fileHash)
            {
                v.zone = &next->zones[(size_t) v.zoneIndex];
                v.map = next;
                v.pos = juce::jlimit (0.0, v.zone->end, v.pos);
            }
            else
                v.killing = true;
        }
        if (anyVoiceOn (old))
            fading = old;
        else
            retire (old);
    }

    const Map& playingMap() const
    {
        return active != nullptr && ! active->zones.empty() ? *active : builtIn (params.builtIn);
    }

    //==========================================================================
    void handle (const NoteEvent& ev)
    {
        switch (ev.kind)
        {
            case NoteEvent::noteOn:
                if (ev.value <= 0.0f) noteOff (ev.note);
                else                  noteOn (ev.note, juce::jlimit (0.0f, 1.0f, ev.value));
                break;
            case NoteEvent::noteOff:      noteOff (ev.note); break;
            case NoteEvent::allNotesOff:
                for (auto& v : slots)
                    if (v.active) { v.held = v.sustained = false; release (v); }
                heldCount = 0;
                sustainDown = false;
                break;
            case NoteEvent::sustain:
                sustainDown = ev.value >= 0.5f;
                if (! sustainDown)
                    for (auto& v : slots)
                        if (v.active && v.sustained) { v.sustained = false; release (v); }
                break;
            case NoteEvent::pitchBend:    bendTarget = juce::jlimit (-1.0f, 1.0f, ev.value) * 2.0f; break;
            case NoteEvent::modWheel:     modValue = juce::jlimit (0.0f, 1.0f, ev.value); break;
        }
    }

    void release (Voice& v)
    {
        if (v.releasing)
            return;
        v.releasing = true;
        v.amp.noteOff();
        v.filt.noteOff();
    }

    void pushHeld (int note)
    {
        removeHeld (note);
        if (heldCount < (int) held.size())
            held[(size_t) heldCount++] = note;
    }

    void removeHeld (int note)
    {
        int w = 0;
        for (int i = 0; i < heldCount; ++i)
            if (held[(size_t) i] != note)
                held[(size_t) w++] = held[(size_t) i];
        heldCount = w;
    }

    static bool matches (const ZonePlay& z, int note, int vel127)
    {
        return note >= z.lo && note <= z.hi && vel127 >= z.velLo && vel127 <= z.velHi;
    }

    void noteOn (int note, float velocity)
    {
        const auto& map = playingMap();
        const int vel127 = juce::jlimit (1, 127, juce::roundToInt (velocity * 127.0f));
        const bool mono = params.voiceMode != 0;
        pushHeld (note);

        if (mono)
        {
            // Legato: a new key while one is held glides the sounding voice when the same zone covers it.
            if (params.voiceMode == 2)
                for (auto& v : slots)
                    if (v.active && ! v.killing && ! v.releasing && v.map == &map && matches (*v.zone, note, vel127))
                    {
                        v.note = note;
                        v.held = true;
                        v.pitchTarget = (float) note;
                        if (params.glide <= 0.0f) v.pitchNow = v.pitchTarget;
                        lastNote = note;
                        return;
                    }
            for (auto& v : slots)
                if (v.active)
                    v.killing = true;
        }

        // Which zones play: every match outside a round-robin group, and one match per group in turn.
        std::array<int, 64> perGroup {};
        for (auto& z : map.zones)
            if (z.group > 0 && matches (z, note, vel127))
                ++perGroup[(size_t) z.group];
        std::array<int, 64> pick {};
        for (size_t g = 1; g < perGroup.size(); ++g)
            if (perGroup[g] > 0)
                pick[g] = (int) (roundRobin[g]++ % (juce::uint32) perGroup[g]);

        std::array<int, 64> seen {};
        for (int i = 0; i < (int) map.zones.size(); ++i)
        {
            const auto& z = map.zones[(size_t) i];
            if (! matches (z, note, vel127))
                continue;
            if (z.group > 0 && seen[(size_t) z.group]++ != pick[(size_t) z.group])
                continue;
            startVoice (map, i, note, velocity, mono);
        }
        lastNote = note;
    }

    void startVoice (const Map& map, int zoneIndex, int note, float velocity, bool mono)
    {
        // Keep within the voice count: the oldest released voice goes first, else the oldest.
        int sounding = 0;
        for (auto& v : slots)
            if (v.active && ! v.killing) ++sounding;
        while (sounding >= (mono ? 1 : params.voices))
        {
            Voice* oldest = nullptr;
            for (auto& v : slots)
                if (v.active && ! v.killing && v.releasing && (oldest == nullptr || v.age < oldest->age)) oldest = &v;
            if (oldest == nullptr)
                for (auto& v : slots)
                    if (v.active && ! v.killing && (oldest == nullptr || v.age < oldest->age)) oldest = &v;
            if (oldest == nullptr)
                break;
            oldest->killing = true;
            --sounding;
        }

        Voice* slot = nullptr;
        for (auto& v : slots)
            if (! v.active) { slot = &v; break; }
        if (slot == nullptr)
        {
            // Every slot busy fading out: take the quietest-to-be (the oldest).
            slot = &slots[0];
            for (auto& v : slots)
                if (v.age < slot->age) slot = &v;
        }

        auto& v = *slot;
        const auto& z = map.zones[(size_t) zoneIndex];
        v.active = true;
        v.held = true;
        v.sustained = v.releasing = v.killing = false;
        v.note = note;
        v.velocity = velocity;
        v.age = ++clock;
        v.map = &map;
        v.zone = &z;
        v.zoneIndex = zoneIndex;
        v.pos = z.start;
        v.dir = 1;
        v.pitchTarget = (float) note;
        v.pitchNow = params.glide > 0.0f && lastNote >= 0 ? (float) lastNote : (float) note;
        v.elapsed = 0.0f;
        v.fadeIn = z.start > 0.5 ? 0.0f : 1.0f;
        v.kill = 1.0f;
        v.amp.set (sampleRate, params.attack, params.decay, params.sustain, params.release);
        v.filt.set (sampleRate, params.fAttack, params.fDecay, params.fSustain, params.fRelease);
        v.amp.value = 0.0f;
        v.filt.value = 0.0f;
        v.amp.trigger();
        v.filt.trigger();
        v.svf[0].reset();
        v.svf[1].reset();
        juce::ignoreUnused (mono);
    }

    void noteOff (int note)
    {
        const auto& map = playingMap();
        removeHeld (note);

        // Mono: back to the last key still held, gliding there.
        if (params.voiceMode != 0 && heldCount > 0)
        {
            const int back = held[(size_t) heldCount - 1];
            for (auto& v : slots)
                if (v.active && ! v.killing && v.note == note && v.map == &map && matches (*v.zone, back, 64))
                {
                    v.note = back;
                    v.pitchTarget = (float) back;
                    if (params.glide <= 0.0f) v.pitchNow = v.pitchTarget;
                    return;
                }
        }

        for (auto& v : slots)
            if (v.active && v.note == note && v.held)
            {
                v.held = false;
                if (sustainDown)                          v.sustained = true;
                else if (! v.map->ignoresNoteOff())       release (v);
            }
    }

    //==========================================================================
    void renderVoices (float* l, float* r, int n)
    {
        for (int at = 0; at < n; at += modChunk)
        {
            const int len = juce::jmin (modChunk, n - at);
            const float secs = (float) len / (float) sampleRate;
            bendNow += (bendTarget - bendNow) * juce::jmin (1.0f, secs / 0.01f);
            lfoPhase = wrap01 (lfoPhase + params.lfoRate * secs);
            const float lfo = sinTurns (lfoPhase);
            for (auto& v : slots)
                if (v.active)
                    renderVoice (v, l + at, r + at, len, lfo);
        }
    }

    void renderVoice (Voice& v, float* outL, float* outR, int n, float lfo)
    {
        const auto& z = *v.zone;
        const auto& p = params;
        const auto& audio = *z.audio;
        const float secs = (float) n / (float) sampleRate;

        // Pitch: glide, envelope, LFO (plus vibrato from the mod wheel) and bend.
        if (p.glide > 0.0f)
            v.pitchNow += (v.pitchTarget - v.pitchNow) * juce::jmin (1.0f, secs / p.glide * 2.3f);
        else
            v.pitchNow = v.pitchTarget;
        const float penv = std::abs (p.pitchEnv) > 1.0e-3f ? p.pitchEnv * std::exp (-v.elapsed / p.pitchTime) : 0.0f;
        v.elapsed += secs;
        const float semis = v.pitchNow - (float) z.root + z.tune + bendNow + penv + lfo * (p.lfoPitch + 0.5f * modValue);
        const double step = std::exp2 ((double) semis / 12.0) * audio.rate / sampleRate;

        // The level to read and the kernel band for this step.
        int level = 0;
        double scale = 1.0;
        while (step / scale > 2.0 && level + 1 < (int) audio.levels.size())
        {
            ++level;
            scale *= 2.0;
        }
        const double stepAtLevel = step / scale;
        const int band = stepAtLevel <= 1.0 ? 0 : juce::jlimit (1, bands - 1, (int) std::ceil ((stepAtLevel - 1.0) * (bands - 1) - 1.0e-9));
        const auto& lv = audio.levels[(size_t) level];
        const float* d0 = lv.data (0);
        const float* d1 = lv.data (audio.channels > 1 ? 1 : 0);
        const bool stereo = audio.channels > 1;
        const auto& k = kernels();

        // Filter: cutoff from the knob, the filter envelope (sampled once per chunk) and the LFO.
        const float fenv = v.filt.value;
        float g = 0.0f, kRes = 2.0f;
        if (p.filterType != 0)
        {
            const float oct = p.envAmount * 6.0f * fenv + p.lfoFilter * 2.0f * lfo;
            g = Svf::gFor (p.cutoff * std::exp2 (oct), sampleRate);
            kRes = 2.0f - 1.96f * p.resonance;
        }
        for (int i = 0; i < n; ++i) v.filt.next();

        const auto [panL, panR] = panGains (z.pan);
        const float gain = z.gain * velocityGain (v.velocity, p.velocity) * p.level * (1.0f - p.lfoAmp * (0.5f + 0.5f * lfo));
        const bool looping = z.loop != LoopMode::off && v.map->loops();
        const double fadeOutLen = 64.0 * step;
        const float killStep = 1.0f / (float) (0.003 * sampleRate);
        const float fadeInStep = 1.0f / 32.0f;

        for (int i = 0; i < n; ++i)
        {
            // Read at pos through the windowed sinc.
            const double lp = v.pos / scale;
            const auto i0 = (juce::int64) std::floor (lp);
            const double frac = lp - (double) i0;
            const double ph = frac * phases;
            const int phase = juce::jmin (phases - 1, (int) ph);
            const float t = (float) (ph - phase);
            const float* ka = k.at (band, phase);
            const float* kb = ka + taps;
            const auto first = juce::jlimit ((juce::int64) -pad, lv.frames + pad - taps, i0 - (halfTaps - 1));
            const float* x0 = d0 + first;
            float sl = 0.0f, sr = 0.0f;
            if (stereo)
            {
                const float* x1 = d1 + first;
                for (int j = 0; j < taps; ++j)
                {
                    const float c = ka[j] + t * (kb[j] - ka[j]);
                    sl += c * x0[j];
                    sr += c * x1[j];
                }
            }
            else
            {
                for (int j = 0; j < taps; ++j)
                    sl += (ka[j] + t * (kb[j] - ka[j])) * x0[j];
                sr = sl;
            }

            if (p.filterType != 0)
            {
                const auto a = v.svf[0].process (sl, g, kRes);
                sl = p.filterType == 1 ? a.low : p.filterType == 2 ? a.high : a.band;
                if (stereo)
                {
                    const auto b = v.svf[1].process (sr, g, kRes);
                    sr = p.filterType == 1 ? b.low : p.filterType == 2 ? b.high : b.band;
                }
                else
                    sr = sl;
            }

            float env = v.amp.next() * gain;
            if (v.fadeIn < 1.0f)
            {
                env *= v.fadeIn;
                v.fadeIn = juce::jmin (1.0f, v.fadeIn + fadeInStep);
            }
            if (v.killing)
            {
                env *= v.kill;
                v.kill -= killStep;
            }
            if (! looping)
            {
                const double left = z.end - v.pos;
                if (left < fadeOutLen)
                    env *= (float) juce::jmax (0.0, left / fadeOutLen);
            }

            outL[i] += sl * env * panL;
            outR[i] += sr * env * panR;

            // Advance, looping or stopping at the end.
            v.pos += step * v.dir;
            if (looping)
            {
                const double len = z.loopEnd - z.loopStart;
                if (z.loop == LoopMode::forward)
                {
                    while (v.pos >= z.loopEnd) v.pos -= len;
                }
                else
                {
                    if (v.dir > 0 && v.pos >= z.loopEnd)   { v.pos = juce::jmax (z.loopStart, 2.0 * z.loopEnd - v.pos); v.dir = -1; }
                    if (v.dir < 0 && v.pos < z.loopStart)  { v.pos = juce::jmin (z.loopEnd, 2.0 * z.loopStart - v.pos); v.dir = 1; }
                }
            }
            else if (v.pos >= z.end)
            {
                v.active = false;
                return;
            }

            if (! v.amp.isActive() || (v.killing && v.kill <= 0.0f))
            {
                v.active = false;
                return;
            }
        }
    }

    //==========================================================================
    Params params;
    std::array<Voice, (size_t) maxSlots> slots;
    std::array<int, 32> held {};
    int heldCount = 0, lastNote = -1;
    std::array<juce::uint32, 64> roundRobin {};
    bool sustainDown = false;
    float bendTarget = 0.0f, bendNow = 0.0f, modValue = 0.0f, lfoPhase = 0.0f;
    juce::uint64 clock = 0;
    std::vector<float> scratchL, scratchR;

    std::atomic<Map*> pending { nullptr };
    Map* active = nullptr;                  // audio thread
    Map* fading = nullptr;                  // audio thread: the previous map, while voices on it fade
    juce::AbstractFifo retireFifo { 32 };
    std::array<Map*, 32> retired {};

    juce::ValueTree applied;                // message thread: the SAMPLER node last published
    juce::File appliedFolder;
    bool synced = false;
};
} // namespace

//==============================================================================
InstrumentInfo makeInfo()
{
    using P = InstrumentInfo::Patch;
    return { typeId, "Sampler", "Sampler",
             "Plays your samples across the keyboard: Classic, One Shot, Slice and multi-zone, with filter, envelopes and LFO",
             juce::Colour (0xff3fa7a0),
             { knob ("attack", "Attack", 0.0f, 5000.0f, 2.0f, "ms", 0.3f),
               knob ("decay", "Decay", 5.0f, 10000.0f, 400.0f, "ms", 0.3f),
               knob ("sustain", "Sustain", 0.0f, 1.0f, 1.0f, "%"),
               knob ("release", "Release", 5.0f, 8000.0f, 180.0f, "ms", 0.3f),
               choice ("filter", "Filter", { "Off", "Lowpass", "Highpass", "Bandpass" }, 0),
               knob ("cutoff", "Cutoff", 20.0f, 20000.0f, 12000.0f, "Hz", 0.25f),
               knob ("resonance", "Resonance", 0.0f, 1.0f, 0.1f, "%"),
               knob ("envamount", "Env Amount", -1.0f, 1.0f, 0.0f, "%"),
               knob ("fattack", "F Attack", 0.0f, 5000.0f, 2.0f, "ms", 0.3f),
               knob ("fdecay", "F Decay", 5.0f, 10000.0f, 500.0f, "ms", 0.3f),
               knob ("fsustain", "F Sustain", 0.0f, 1.0f, 0.0f, "%"),
               knob ("frelease", "F Release", 5.0f, 8000.0f, 300.0f, "ms", 0.3f),
               knob ("pitchenv", "Pitch Env", -24.0f, 24.0f, 0.0f, "st"),
               knob ("pitchtime", "Pitch Time", 1.0f, 2000.0f, 80.0f, "ms", 0.3f),
               knob ("lforate", "LFO Rate", 0.05f, 20.0f, 5.0f, "Hz", 0.4f),
               knob ("lfopitch", "LFO Pitch", 0.0f, 2.0f, 0.0f, "st"),
               knob ("lfofilter", "LFO Filter", 0.0f, 1.0f, 0.0f, "%"),
               knob ("lfoamp", "LFO Amp", 0.0f, 1.0f, 0.0f, "%"),
               knob ("glide", "Glide", 0.0f, 2000.0f, 0.0f, "ms", 0.4f),
               choice ("voicemode", "Voices", { "Poly", "Mono", "Legato" }, 0),
               knob ("voices", "Polyphony", 1.0f, 32.0f, 16.0f),
               knob ("vel", "Velocity", 0.0f, 1.0f, 0.5f, "%"),
               knob ("level", "Level", -24.0f, 6.0f, 0.0f, "dB"),
               choice ("builtin", "Built-in", { "Keys", "Pad", "Pluck" }, 0) },
             { P { "Sampler", "Sampler", {} },
               P { "Soft Pad", "Sampler", { { "builtin", 1 }, { "attack", 600.0f }, { "release", 1500.0f }, { "filter", 1 },
                                            { "cutoff", 3500.0f }, { "lfofilter", 0.2f }, { "lforate", 0.3f }, { "level", -2.0f } } },
               P { "Plucked", "Sampler", { { "builtin", 2 }, { "release", 400.0f }, { "filter", 1 }, { "cutoff", 1200.0f },
                                           { "envamount", 0.6f }, { "fdecay", 300.0f }, { "vel", 0.7f } } },
               P { "Drum Hits", "Sampler", { { "sustain", 1.0f }, { "release", 60.0f }, { "vel", 0.8f } } },
               P { "Mono Glide", "Sampler", { { "voicemode", 2 }, { "glide", 120.0f }, { "filter", 1 }, { "cutoff", 2500.0f },
                                              { "resonance", 0.4f }, { "envamount", 0.4f }, { "lfopitch", 0.15f }, { "lforate", 5.5f } } },
               P { "Lo-Fi Tape", "Sampler", { { "filter", 1 }, { "cutoff", 2800.0f }, { "lfopitch", 0.08f }, { "lforate", 0.7f },
                                              { "release", 300.0f } } } } };
}

} // namespace woodshed::sampler

namespace woodshed
{
void registerSamplerInstruments (InstrumentRegistry& r)
{
    r.add<sampler::SamplerInstrument> (sampler::makeInfo());
}
} // namespace woodshed
