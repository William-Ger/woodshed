#include "Stretcher.h"

// Signalsmith Stretch and its Linear library (MIT; third_party/), quietened: they aren't written
// for our warning flags.
#if defined (_MSC_VER)
 #pragma warning (push, 0)
#elif defined (__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Weverything"
#elif defined (__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wconversion"
 #pragma GCC diagnostic ignored "-Wsign-conversion"
 #pragma GCC diagnostic ignored "-Wshadow"
 #pragma GCC diagnostic ignored "-Wfloat-conversion"
 #pragma GCC diagnostic ignored "-Wunused-parameter"
 #pragma GCC diagnostic ignored "-Wunused-variable"
 #pragma GCC diagnostic ignored "-Wold-style-cast"
 #pragma GCC diagnostic ignored "-Wpedantic"
 #pragma GCC diagnostic ignored "-Wextra"
 #pragma GCC diagnostic ignored "-Wredundant-decls"
 #pragma GCC diagnostic ignored "-Wswitch-enum"
 #pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif
#include "signalsmith-stretch.h"
#if defined (_MSC_VER)
 #pragma warning (pop)
#elif defined (__clang__)
 #pragma clang diagnostic pop
#elif defined (__GNUC__)
 #pragma GCC diagnostic pop
#endif

namespace woodshed::flex
{

namespace
{
using Stretch = signalsmith::stretch::SignalsmithStretch<float, std::mt19937>;

constexpr double crossfadeSeconds = 0.003;

/** Cubic (Catmull-Rom) read between samples. */
float readAt (const float* x, int n, double pos)
{
    const int i = (int) std::floor (pos);
    const float f = (float) (pos - i);
    auto s = [x, n] (int k) { return k >= 0 && k < n ? x[k] : 0.0f; };
    const float y0 = s (i - 1), y1 = s (i), y2 = s (i + 1), y3 = s (i + 2);
    return y1 + 0.5f * f * (y2 - y0 + f * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3 + f * (3.0f * (y1 - y2) + y3 - y0)));
}

/** One piece of the result: file samples [srcFrom, srcTo) land on region samples [dstFrom, dstTo). */
struct Piece
{
    double srcFrom = 0.0, srcTo = 0.0, dstFrom = 0.0, dstTo = 0.0;
    double ratio() const        { return (dstTo - dstFrom) / (srcTo - srcFrom); }
};

/** Adds `piece` (with a cross-fade lead-in of `lead` output samples before dstFrom, and a fade
    out over its last `tail` samples) into `out`, starting at output sample `outFrom`. `data` is
    the rendered piece, lead-in first. */
void place (juce::AudioBuffer<float>& out, const std::vector<float>& data, juce::int64 outFrom, int lead, int tail)
{
    const int total = (int) data.size();
    auto* o = out.getWritePointer (0);
    const auto outLength = (juce::int64) out.getNumSamples();
    for (int i = 0; i < total; ++i)
    {
        const auto at = outFrom + i;
        if (at < 0 || at >= outLength)
            continue;
        float g = 1.0f;
        if (i < lead)            g *= (float) (i + 1) / (float) (lead + 1);
        if (i >= total - tail)   g *= (float) (total - i) / (float) (tail + 1);
        o[at] += data[(size_t) i] * g;
    }
}

/** Copies file samples from `from` for `count` samples (silence outside the file). */
std::vector<float> copySource (const float* x, int n, juce::int64 from, int count)
{
    std::vector<float> v ((size_t) juce::jmax (0, count), 0.0f);
    for (int i = 0; i < count; ++i)
        if (const auto k = from + i; k >= 0 && k < n)
            v[(size_t) i] = x[k];
    return v;
}

struct BufferIO
{
    float* data;
    float* operator[] (int) const      { return data; }
};

/** Stretches file samples [inFrom, inFrom + inCount) to `outCount` samples, with pitch, formant
    and gain following the notes. False if the piece is too short for the stretcher.

    Pitch moves by stretching longer (or shorter) by the pitch factor and resampling back,
    chunk by chunk: the resampling makes the new pitch exact (the stretcher's own transposition
    is a few cents out on held notes), and a matching formant shift inside the stretcher puts
    the vowel back where it was. */
bool stretchPiece (const RenderRequest& rq, const float* x, int n, juce::int64 inFrom, int inCount, int outCount,
                   bool pitchEdits, std::vector<float>& result)
{
    const double rate = inCount / (double) outCount;      // input samples per output sample
    const double sr = rq.sampleRate;
    double block = rq.mode == Mode::monophonic ? 0.07 : 0.12;

    auto factorAt = [&] (double outPos, float& formant)
    {
        formant = 0.0f;
        if (! pitchEdits)
            return 1.0;
        const double src = (double) inFrom + outPos * rate;
        formant = formantAt (rq.notes, src, sr);
        const float semis = rq.pitch != nullptr ? transposeAt (rq.notes, *rq.pitch, src, sr) : 0.0f;
        return std::pow (2.0, semis / 12.0);
    };

    float formant0 = 0.0f;
    const double f0 = factorAt (0.0, formant0);
    Stretch stretch (rq.seed);
    for (;; block *= 0.7)
    {
        if (block < 0.012)
            return false;
        const int blockSamples = (int) (sr * block);
        stretch.configure (1, blockSamples, blockSamples / 4);
        const auto seek = stretch.outputSeekLength ((float) (rate / f0));
        if (seek + 8 <= inCount && (int) (seek / rate) + 8 <= outCount)
            break;
    }

    // After the piece the stretcher gets the piece's own end played backwards, so it runs on
    // smoothly to the cut without hearing what comes after it (the next transient would ring
    // ahead of its cut).
    const int lead = stretch.inputLatency() + (int) std::ceil (stretch.outputLatency() * rate * (pitchEdits ? 2.0 : 1.0)) + stretch.intervalSamples() + 16;
    auto input = copySource (x, n, inFrom, inCount);
    for (int i = 0; i < lead; ++i)
        input.push_back (input[(size_t) juce::jmax (0, inCount - 1 - (i % inCount))]);

    // Seek so the stretcher's output starts on the first input sample (as exact() does).
    stretch.setFormantFactor (std::pow (2.0f, formant0 / 12.0f) / (float) f0, false);
    int fed = stretch.outputSeekLength ((float) (rate / f0));
    stretch.outputSeek (BufferIO { input.data() }, fed);

    // The stretched stream, read back at the pitch factor.
    std::vector<float> stream, scratch;
    stream.reserve ((size_t) (outCount * 2.5) + 64);
    double readPos = 0.0;
    result.assign ((size_t) outCount, 0.0f);
    const int chunk = juce::jmax (16, stretch.intervalSamples() / 2);

    for (int o = 0; o < outCount; o += chunk)
    {
        const int count = juce::jmin (chunk, outCount - o);
        float formant = 0.0f;
        const double f = factorAt (o + 0.5 * count, formant);
        const double streamRate = rate / f;                 // input samples per stretched sample
        stretch.setFormantFactor (std::pow (2.0f, formant / 12.0f) / (float) f, false);

        // Enough stretched samples to read `count` at speed f (and the interpolator's lookahead).
        const int make = juce::jmax (0, (int) std::ceil (readPos + count * f + 3.0) - (int) stream.size());
        if (make > 0)
        {
            // The input stays ahead of the stretched output by what the stretcher needs at this rate.
            const double madeBy = (double) (stream.size() + (size_t) make);
            const int target = (int) std::llround ((madeBy + stretch.outputLatency()) * streamRate + stretch.inputLatency());
            const int give = juce::jlimit (0, (int) input.size() - fed, target - fed);
            scratch.assign ((size_t) make, 0.0f);
            stretch.process (BufferIO { input.data() + fed }, give, BufferIO { scratch.data() }, make);
            fed += give;
            stream.insert (stream.end(), scratch.begin(), scratch.end());
        }

        for (int i = 0; i < count; ++i)
        {
            result[(size_t) (o + i)] = readAt (stream.data(), (int) stream.size(), readPos);
            readPos += f;
        }
    }

    if (pitchEdits)
        for (int i = 0; i < outCount; ++i)
            if (const float db = gainAt (rq.notes, (double) inFrom + i * rate, sr); std::abs (db) > 0.01f)
                result[(size_t) i] *= juce::Decibels::decibelsToGain (db);
    return true;
}
}

//==============================================================================
juce::int64 renderedLength (const WarpMap& warp, juce::int64 fileLength)
{
    const auto end = (juce::int64) std::ceil (warp.toDst ((double) fileLength));
    return juce::jlimit ((juce::int64) 0, fileLength * 8 + 48000, end);
}

Rendered renderVarispeed (const juce::AudioBuffer<float>& source, const WarpMap& warp)
{
    const int n = source.getNumSamples();
    const auto length = renderedLength (warp, n);
    auto out = std::make_shared<juce::AudioBuffer<float>> (1, (int) length);
    const float* x = source.getReadPointer (0);
    auto* o = out->getWritePointer (0);

    // Walk the segments so each sample costs an add, not a search.
    for (juce::int64 i = 0; i < length; ++i)
        o[i] = 0.0f;
    const auto& m = warp.markers;
    if (m.size() < 2)
    {
        const double shift = m.empty() ? 0.0 : m[0].src - m[0].dst;
        for (juce::int64 i = 0; i < length; ++i)
            o[i] = readAt (x, n, (double) i + shift);
        return out;
    }
    for (size_t seg = 1; seg < m.size(); ++seg)
    {
        const auto& a = m[seg - 1];
        const auto& b = m[seg];
        const double slope = (b.src - a.src) / (b.dst - a.dst);
        const auto from = seg == 1 ? (juce::int64) 0 : (juce::int64) std::ceil (a.dst);
        const auto to = seg + 1 == m.size() ? length : juce::jmin (length, (juce::int64) std::ceil (b.dst));
        for (juce::int64 i = juce::jmax ((juce::int64) 0, from); i < to; ++i)
            o[i] = readAt (x, n, a.src + ((double) i - a.dst) * slope);
    }
    return out;
}

Rendered render (const RenderRequest& rq, const std::atomic<bool>* cancel)
{
    if (rq.source == nullptr || rq.source->getNumSamples() == 0)
        return nullptr;
    const auto& source = *rq.source;
    const int n = source.getNumSamples();
    const float* x = source.getReadPointer (0);
    const double sr = rq.sampleRate;

    if (rq.mode == Mode::speed)
        return renderVarispeed (source, rq.warp);

    const auto length = renderedLength (rq.warp, n);
    auto out = std::make_shared<juce::AudioBuffer<float>> (1, (int) length);
    out->clear();

    std::vector<PitchNote> edited;
    for (auto& note : rq.notes)
        if (note.isEdited())
            edited.push_back (note);
    RenderRequest request = rq;
    request.notes = edited;

    // Where the pieces start: the file's ends, the markers, and (when stretching) the
    // transients, so each hit starts cleanly on its spot.
    std::vector<double> cuts { 0.0, (double) n };
    for (auto& m : rq.warp.markers)
        if (m.src > 0.0 && m.src < n)
            cuts.push_back (m.src);
    std::sort (cuts.begin(), cuts.end());
    if (rq.transients != nullptr && rq.mode != Mode::speed)
    {
        const double gap = 0.03 * sr;
        auto markerCuts = cuts;
        for (auto t : *rq.transients)
        {
            if (t <= 0.0 || t >= n || std::abs (rq.warp.ratioAt (t) - 1.0) < 1.0e-4)
                continue;
            auto it = std::lower_bound (markerCuts.begin(), markerCuts.end(), t);
            const bool near = (it != markerCuts.end() && *it - t < gap) || (it != markerCuts.begin() && t - *std::prev (it) < gap);
            if (! near)
                cuts.push_back (t);
        }
        std::sort (cuts.begin(), cuts.end());
    }

    const int fade = juce::jmax (8, (int) (crossfadeSeconds * sr));
    for (size_t k = 0; k + 1 < cuts.size(); ++k)
    {
        if (cancel != nullptr && cancel->load())
            return nullptr;

        Piece p { cuts[k], cuts[k + 1], rq.warp.toDst (cuts[k]), rq.warp.toDst (cuts[k + 1]) };
        if (p.srcTo - p.srcFrom < 1.0 || p.dstTo <= 0.0 || p.dstFrom >= (double) length)
            continue;
        const bool first = k == 0, last = k + 2 == cuts.size();
        const double ratio = p.ratio();
        const int lead = first ? 0 : fade;
        const int tail = last ? 0 : fade;

        // The output span: from `lead` samples before the cut (the cross-fade) to the next cut.
        const auto outFrom = (juce::int64) std::llround (p.dstFrom) - lead;
        const int outCount = (int) ((juce::int64) std::llround (p.dstTo) - outFrom);
        const double inStart = p.srcFrom - lead / ratio;
        bool notesHere = false;
        for (auto& note : edited)
            notesHere = notesHere || (note.end > inStart && note.start < p.srcTo);
        if (outCount <= 0)
            continue;

        std::vector<float> data;
        if (std::abs (ratio - 1.0) < 1.0e-4 && ! notesHere)
        {
            data = copySource (x, n, (juce::int64) std::llround (inStart), outCount);
        }
        else if (rq.mode == Mode::rhythmic)
        {
            // Slicing: the audio at its own speed from the cut, stopping (with a short fade)
            // at the next cut if the piece got longer.
            const int available = (int) std::llround (p.srcTo - inStart);
            data = copySource (x, n, (juce::int64) std::llround (inStart), juce::jmin (outCount, available));
            for (int i = 0; i < fade && i < (int) data.size(); ++i)
                data[data.size() - 1 - (size_t) i] *= (float) i / (float) fade;
            data.resize ((size_t) outCount, 0.0f);
        }
        else
        {
            const auto inFrom = (juce::int64) std::llround (inStart);
            const int inCount = (int) ((juce::int64) std::llround (p.srcTo) - inFrom);
            if (! stretchPiece (request, x, n, inFrom, inCount, outCount, notesHere, data))
            {
                // Too short to stretch (a flam, a grace note): resample it, a moment of pitch
                // change being far less noticeable than a gap.
                data.assign ((size_t) outCount, 0.0f);
                const double step = inCount / (double) outCount;
                for (int i = 0; i < outCount; ++i)
                    data[(size_t) i] = readAt (x, n, (double) inFrom + i * step);
            }
        }
        place (*out, data, outFrom, lead, tail);
    }
    return out;
}

} // namespace woodshed::flex
