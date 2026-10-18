#include "Analysis.h"
#include "../engine/PitchDetector.h"
#include <juce_dsp/juce_dsp.h>
#include <numeric>

namespace woodshed::flex
{

namespace
{
constexpr int fftOrder = 10, fftSize = 1 << fftOrder, onsetHop = 256;

float sampleAt (const float* audio, int numSamples, int i)
{
    return i >= 0 && i < numSamples ? audio[i] : 0.0f;
}

/** The mean of v over [i - radius, i + radius], clipped to the ends. */
std::vector<float> movingAverage (const std::vector<float>& v, int radius)
{
    std::vector<double> sum (v.size() + 1, 0.0);
    for (size_t i = 0; i < v.size(); ++i)
        sum[i + 1] = sum[i] + v[i];
    std::vector<float> out (v.size());
    for (int i = 0; i < (int) v.size(); ++i)
    {
        const int a = juce::jmax (0, i - radius), b = juce::jmin ((int) v.size(), i + radius + 1);
        out[(size_t) i] = (float) ((sum[(size_t) b] - sum[(size_t) a]) / juce::jmax (1, b - a));
    }
    return out;
}

double median (std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return v[v.size() / 2];
}
}

//==============================================================================
OnsetCurve onsetCurve (const float* audio, int numSamples, double sampleRate)
{
    OnsetCurve c;
    c.hop = onsetHop;
    const int frames = numSamples / onsetHop + 1;
    c.flux.assign ((size_t) frames, 0.0f);
    c.low.assign ((size_t) frames, 0.0f);

    juce::dsp::FFT fft (fftOrder);
    std::vector<float> window ((size_t) fftSize), work ((size_t) fftSize * 2), prev ((size_t) fftSize / 2 + 1, 0.0f);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
    const int lowBins = juce::jmax (2, (int) std::lround (200.0 * fftSize / sampleRate));

    for (int f = 0; f < frames; ++f)
    {
        const int from = f * onsetHop - fftSize / 2;
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < fftSize; ++i)
            work[(size_t) i] = sampleAt (audio, numSamples, from + i) * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (work.data(), true);

        float flux = 0.0f, low = 0.0f;
        for (int k = 1; k <= fftSize / 2; ++k)
        {
            const float level = std::log1p (100.0f * work[(size_t) k]);
            const float rise = juce::jmax (0.0f, level - prev[(size_t) k]);
            flux += rise;
            if (k <= lowBins)
                low += rise;
            prev[(size_t) k] = level;
        }
        c.flux[(size_t) f] = flux;
        c.low[(size_t) f] = low;
    }
    return c;
}

std::vector<double> detectTransients (const float* audio, int numSamples, double sampleRate)
{
    return detectTransients (onsetCurve (audio, numSamples, sampleRate), audio, numSamples, sampleRate);
}

std::vector<double> detectTransients (const OnsetCurve& c, const float* audio, int numSamples, double sampleRate)
{
    std::vector<double> found;
    const auto& f = c.flux;
    if (f.size() < 8)
        return found;

    const float top = *std::max_element (f.begin(), f.end());
    if (top <= 1.0e-3f)
        return found;

    const int radius = juce::jmax (4, (int) (0.12 * sampleRate / c.hop));
    const auto mean = movingAverage (f, radius);
    const int peakRadius = 3;
    const int minGap = (int) (0.04 * sampleRate / c.hop);
    int last = -minGap - 1;

    // The first frame that's the highest around it and well above the local average.
    for (int i = 1; i < (int) f.size(); ++i)
    {
        const float v = f[(size_t) i];
        if (v < 1.5f * mean[(size_t) i] + 0.06f * top || i - last < minGap)
            continue;
        bool isPeak = true;
        for (int j = juce::jmax (0, i - peakRadius); j <= juce::jmin ((int) f.size() - 1, i + peakRadius) && isPeak; ++j)
            isPeak = j == i || f[(size_t) j] < v || (j > i && f[(size_t) j] <= v);
        if (! isPeak)
            continue;

        // Refine to the sample: where the short-term level (of the slope, so low notes still
        // ringing don't hide what starts over them) rises fastest, then back to where it starts.
        const int centre = i * c.hop;
        const int from = juce::jmax (1, centre - fftSize), to = juce::jmin (numSamples, centre + fftSize * 3 / 4);
        constexpr int window = 48;
        if (to - from < window * 4)
            continue;
        std::vector<float> level ((size_t) (to - from), 0.0f);
        double sum = 0.0;
        for (int k = from; k < to; ++k)
        {
            const double d = audio[k] - 0.95 * audio[k - 1];
            sum += d * d;
            if (k - window >= from)
            {
                const double old = audio[k - window] - 0.95 * audio[k - window - 1];
                sum -= old * old;
            }
            level[(size_t) (k - from)] = (float) std::sqrt (juce::jmax (0.0, sum) / window);
        }
        int rise = window;
        float steepest = 0.0f;
        const float levelFloor = 0.05f * *std::max_element (level.begin(), level.end()) + 1.0e-6f;
        for (int k = window * 2; k < (int) level.size(); ++k)
        {
            const float before = level[(size_t) (k - window)];
            const float r = (level[(size_t) k] - before) / (before + levelFloor);
            if (r > steepest) { steepest = r; rise = k; }
        }
        float background = level[(size_t) (rise - window)];
        for (int k = juce::jmax (0, rise - window * 4); k < rise - window; ++k)
            background = juce::jmin (background, level[(size_t) k]);
        const float threshold = background + 0.2f * (level[(size_t) rise] - background);
        // The level at k covers the `window` samples up to k: the start is a window before the
        // first point over the threshold, plus how far into that window the rise begins.
        int start = rise;
        while (start > window && level[(size_t) start - 1] > threshold)
            --start;
        start = juce::jmax (0, start - window / 4);

        const double at = from + start;
        if (! found.empty() && at - found.back() < 0.03 * sampleRate)
            continue;
        found.push_back (at);
        last = i;
    }
    return found;
}

//==============================================================================
TempoEstimate detectTempo (const float* audio, int numSamples, double sampleRate, int beatsPerBar)
{
    TempoEstimate t;
    const auto curve = onsetCurve (audio, numSamples, sampleRate);
    const int n = (int) curve.flux.size();
    const double fps = sampleRate / curve.hop;
    if (n < (int) (fps * 3.0))
        return t;       // under three seconds: not enough to go on

    // The onset strength with its slow average taken out.
    auto o = curve.flux;
    {
        const auto avg = movingAverage (o, (int) (fps * 0.1));
        double sd = 0.0;
        for (int i = 0; i < n; ++i)
        {
            o[(size_t) i] = juce::jmax (0.0f, o[(size_t) i] - avg[(size_t) i]);
            sd += (double) o[(size_t) i] * o[(size_t) i];
        }
        sd = std::sqrt (sd / n);
        if (sd < 1.0e-6)
            return t;
        for (auto& v : o) v = (float) (v / sd);
    }

    // Autocorrelation over 60..200 bpm, with its multiples, weighted towards 120.
    const int minLag = (int) std::floor (fps * 60.0 / 200.0), maxLag = (int) std::ceil (fps * 60.0 / 60.0);
    std::vector<double> ac ((size_t) maxLag * 4 + 2, 0.0);
    for (int lag = 1; lag < (int) ac.size() && lag < n; ++lag)
    {
        double s = 0.0;
        for (int i = 0; i + lag < n; ++i)
            s += (double) o[(size_t) i] * o[(size_t) (i + lag)];
        ac[(size_t) lag] = s / (n - lag);
    }
    auto acAt = [&] (double lag)
    {
        const int i = (int) lag;
        if (i < 1 || i + 1 >= (int) ac.size()) return 0.0;
        const double f = lag - i;
        return ac[(size_t) i] * (1.0 - f) + ac[(size_t) i + 1] * f;
    };
    auto score = [&] (int lag)
    {
        const double bpm = 60.0 * fps / lag;
        const double octaves = std::log2 (bpm / 120.0);
        const double prior = std::exp (-0.5 * (octaves / 0.8) * (octaves / 0.8));
        return (acAt (lag) + 0.5 * acAt (2.0 * lag) + 0.25 * acAt (4.0 * lag)) * prior;
    };

    int bestLag = minLag;
    double bestScore = -1.0;
    for (int lag = minLag; lag <= maxLag; ++lag)
        if (const double s = score (lag); s > bestScore)
        {
            bestScore = s;
            bestLag = lag;
        }
    double period = bestLag;
    {
        const double a = score (bestLag - 1), b = score (bestLag), c = score (bestLag + 1);
        const double d = a - 2.0 * b + c;
        if (std::abs (d) > 1.0e-12)
            period += juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / d);
    }

    // Beat tracking by dynamic programming: each beat follows one about a period earlier.
    std::vector<double> best ((size_t) n, 0.0);
    std::vector<int> back ((size_t) n, -1);
    const double tightness = 100.0;
    for (int i = 0; i < n; ++i)
    {
        double bestPrev = 0.0;
        int from = -1;
        for (int j = i - (int) std::ceil (2.0 * period); j <= i - (int) std::floor (period / 2.0); ++j)
        {
            if (j < 0) continue;
            const double gap = std::log ((i - j) / period);
            const double s = best[(size_t) j] - tightness * gap * gap;
            if (from < 0 || s > bestPrev) { bestPrev = s; from = j; }
        }
        best[(size_t) i] = o[(size_t) i] + (from >= 0 ? juce::jmax (0.0, bestPrev) : 0.0);
        back[(size_t) i] = from >= 0 && bestPrev > 0.0 ? from : -1;
    }

    int end = n - 1;
    for (int i = juce::jmax (0, n - (int) std::ceil (period)); i < n; ++i)
        if (best[(size_t) i] > best[(size_t) end]) end = i;
    std::vector<int> beatFrames;
    for (int i = end; i >= 0; i = back[(size_t) i])
        beatFrames.push_back (i);
    std::reverse (beatFrames.begin(), beatFrames.end());
    if (beatFrames.size() < 4)
        return t;

    // Hi-hats on the off-beats can pull the tracker half a beat out (their onsets are as sharp
    // as the kick's and snare's): the beats are where the energy is.
    auto energyAt = [audio, numSamples, sampleRate] (double from)
    {
        double e = 0.0;
        const int a = juce::jlimit (0, numSamples, (int) from), b = juce::jlimit (0, numSamples, (int) (from + 0.05 * sampleRate));
        for (int i = a; i < b; ++i) e += (double) audio[i] * audio[i];
        return e;
    };
    double onBeats = 0.0, offBeats = 0.0;
    for (int b : beatFrames)
    {
        onBeats += energyAt ((double) b * curve.hop);
        offBeats += energyAt ((b + period * 0.5) * curve.hop);
    }
    const double shift = offBeats > 1.3 * onBeats ? period * 0.5 : 0.0;

    // Each beat moved onto the transient it sits on, if there is one.
    const auto transients = detectTransients (curve, audio, numSamples, sampleRate);
    const double window = 0.2 * period * curve.hop;
    for (int b : beatFrames)
    {
        double at = (b + shift) * curve.hop;
        if (at >= numSamples)
            continue;
        auto it = std::lower_bound (transients.begin(), transients.end(), at - window);
        double nearest = -1.0;
        for (; it != transients.end() && *it <= at + window; ++it)
            if (nearest < 0.0 || std::abs (*it - at) < std::abs (nearest - at))
                nearest = *it;
        t.beats.push_back (nearest >= 0.0 ? nearest : at);
    }

    // The tempo: the straight line through the beats.
    const int count = (int) t.beats.size();
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (int k = 0; k < count; ++k)
    {
        sx += k; sy += t.beats[(size_t) k];
        sxx += (double) k * k; sxy += k * t.beats[(size_t) k];
    }
    const double slope = (count * sxy - sx * sy) / (count * sxx - sx * sx);
    if (slope <= 0.0)
        return t;
    t.bpm = 60.0 * sampleRate / slope;

    // How much the beats stand out from everything else.
    double atBeats = 0.0, overall = 0.0;
    for (int b : beatFrames) atBeats += o[(size_t) b];
    for (auto v : o) overall += v;
    atBeats /= count;
    overall /= n;
    t.confidence = juce::jlimit (0.0, 1.0, (atBeats / (overall + 1.0e-9) - 1.0) / 4.0);

    // The downbeat: the beat of the bar with the most low end (kick drums) and the loudest
    // accent, measured as energy (not onset strength, which hears every kick alike).
    std::vector<float> lowBand ((size_t) numSamples);
    {
        const float a = (float) std::exp (-juce::MathConstants<double>::twoPi * 150.0 / sampleRate);
        float y1 = 0.0f, y2 = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            y1 = audio[i] + a * (y1 - audio[i]);
            y2 = y1 + a * (y2 - y1);
            lowBand[(size_t) i] = y2;
        }
    }
    auto energy = [numSamples] (const float* x, double from, double seconds, double rateHz)
    {
        double e = 0.0;
        const int a = juce::jlimit (0, numSamples, (int) from), b = juce::jlimit (0, numSamples, (int) (from + seconds * rateHz));
        for (int i = a; i < b; ++i) e += (double) x[i] * x[i];
        return e;
    };
    double bestPhase = -1.0;
    for (int phase = 0; phase < juce::jmin (beatsPerBar, count); ++phase)
    {
        double s = 0.0;
        int m = 0;
        for (int k = phase; k < count; k += beatsPerBar)
        {
            const double at = t.beats[(size_t) k];
            s += energy (lowBand.data(), at, 0.08, sampleRate) + 0.25 * energy (audio, at, 0.03, sampleRate);
            ++m;
        }
        s /= juce::jmax (1, m);
        if (s > bestPhase) { bestPhase = s; t.firstDownbeat = phase; }
    }

    // A tempo that drifts (played without a click) gets a tempo for every bar.
    for (int k = t.firstDownbeat; k + beatsPerBar < count; k += beatsPerBar)
        t.barTempos.push_back (60.0 * sampleRate * beatsPerBar / (t.beats[(size_t) (k + beatsPerBar)] - t.beats[(size_t) k]));
    if (t.barTempos.size() >= 2)
    {
        double worst = 0.0;
        for (auto b : t.barTempos)
            worst = juce::jmax (worst, std::abs (b - t.bpm));
        t.variable = worst > 2.0 && t.barTempos.size() >= 3;
    }
    return t;
}

//==============================================================================
float PitchTrack::at (double sample) const
{
    if (pitch.empty()) return 0.0f;
    const double f = sample / hop;
    const int i = juce::jlimit (0, (int) pitch.size() - 1, (int) std::floor (f));
    const int j = juce::jmin ((int) pitch.size() - 1, i + 1);
    if (pitch[(size_t) i] <= 0.0f || pitch[(size_t) j] <= 0.0f)
        return pitch[(size_t) (f - i < 0.5 ? i : j)];
    const float w = (float) juce::jlimit (0.0, 1.0, f - i);
    return pitch[(size_t) i] * (1.0f - w) + pitch[(size_t) j] * w;
}

float PitchTrack::trendAt (double sample) const
{
    if (trend.empty()) return 0.0f;
    const double f = sample / hop;
    const int i = juce::jlimit (0, (int) trend.size() - 1, (int) std::floor (f));
    const int j = juce::jmin ((int) trend.size() - 1, i + 1);
    if (trend[(size_t) i] <= 0.0f || trend[(size_t) j] <= 0.0f)
        return trend[(size_t) (f - i < 0.5 ? i : j)];
    const float w = (float) juce::jlimit (0.0, 1.0, f - i);
    return trend[(size_t) i] * (1.0f - w) + trend[(size_t) j] * w;
}

PitchTrack trackPitch (const float* audio, int numSamples, double sampleRate)
{
    PitchTrack t;
    t.hop = 256;
    const int window = 1024;        // 21 ms: short enough to follow vibrato, long enough for a low voice
    const int frames = numSamples / t.hop + 1;
    PitchDetector detector (window);
    std::vector<float> buffer ((size_t) window);
    std::vector<float> raw ((size_t) frames, 0.0f), clarity ((size_t) frames, 0.0f);
    t.level.assign ((size_t) frames, 0.0f);

    float loudest = 0.0f;
    for (int f = 0; f < frames; ++f)
    {
        const int from = f * t.hop - window / 2;
        for (int i = 0; i < window; ++i)
            buffer[(size_t) i] = sampleAt (audio, numSamples, from + i);
        const auto r = detector.analyse (buffer.data(), window, sampleRate, 70.0f, 1400.0f);
        t.level[(size_t) f] = r.level;
        loudest = juce::jmax (loudest, r.level);
        clarity[(size_t) f] = r.clarity;
        raw[(size_t) f] = r.frequency > 0.0f ? 69.0f + 12.0f * std::log2 (r.frequency / 440.0f) : 0.0f;
    }

    // Voiced: periodic and no more than 30 dB under the loudest moment.
    t.pitch.assign ((size_t) frames, 0.0f);
    for (int f = 0; f < frames; ++f)
        if (raw[(size_t) f] > 0.0f && clarity[(size_t) f] > 0.85f && t.level[(size_t) f] > loudest * 0.03f)
            t.pitch[(size_t) f] = raw[(size_t) f];

    // Octave slips: a frame an octave off both neighbours is put back.
    for (int f = 1; f + 1 < frames; ++f)
    {
        auto& p = t.pitch[(size_t) f];
        const float a = t.pitch[(size_t) f - 1], b = t.pitch[(size_t) f + 1];
        if (p > 0.0f && a > 0.0f && b > 0.0f && std::abs (a - b) < 1.0f && std::abs (p - a) > 6.0f)
            p = 0.5f * (a + b);
    }

    // The trend: a 150 ms average of the voiced frames around each one.
    const int radius = juce::jmax (1, (int) (0.075 * sampleRate / t.hop));
    t.trend.assign ((size_t) frames, 0.0f);
    for (int f = 0; f < frames; ++f)
    {
        if (t.pitch[(size_t) f] <= 0.0f)
            continue;
        double s = 0.0;
        int m = 0;
        for (int j = f; j >= juce::jmax (0, f - radius) && t.pitch[(size_t) j] > 0.0f; --j) { s += t.pitch[(size_t) j]; ++m; }
        for (int j = f + 1; j <= juce::jmin (frames - 1, f + radius) && t.pitch[(size_t) j] > 0.0f; ++j) { s += t.pitch[(size_t) j]; ++m; }
        t.trend[(size_t) f] = (float) (s / m);
    }
    return t;
}

std::vector<PitchNote> detectNotes (const PitchTrack& t, double sampleRate)
{
    std::vector<PitchNote> notes;
    const int frames = (int) t.pitch.size();
    const int minFrames = juce::jmax (3, (int) (0.06 * sampleRate / t.hop));
    const int settle = juce::jmax (2, (int) (0.02 * sampleRate / t.hop));

    auto finish = [&] (int from, int to)       // frames [from, to)
    {
        if (to - from < minFrames)
            return;
        std::vector<double> values;
        for (int f = from; f < to; ++f)
            if (t.pitch[(size_t) f] > 0.0f) values.push_back (t.pitch[(size_t) f]);
        PitchNote n;
        n.start = juce::jmax (0.0, (from - 0.5) * t.hop);
        n.end = (to - 0.5) * t.hop;
        n.pitch = (float) median (values);
        notes.push_back (n);
    };

    int f = 0;
    while (f < frames)
    {
        while (f < frames && t.pitch[(size_t) f] <= 0.0f) ++f;
        if (f >= frames) break;

        // A run of voiced frames, cut where the pitch moves to a new note and stays there.
        int from = f;
        std::vector<double> recent;
        while (f < frames && t.pitch[(size_t) f] > 0.0f)
        {
            const double centre = median (std::vector<double> (recent.end() - (long) juce::jmin ((size_t) 12, recent.size()), recent.end()));
            bool moved = recent.size() >= (size_t) minFrames && std::abs (t.pitch[(size_t) f] - centre) > 0.7;
            if (moved)
            {
                // Only if it stays away for a while (vibrato and scoops come back).
                for (int k = 1; k < settle && f + k < frames && moved; ++k)
                    moved = t.pitch[(size_t) (f + k)] > 0.0f && std::abs (t.pitch[(size_t) (f + k)] - centre) > 0.7;
            }
            if (moved)
            {
                finish (from, f);
                from = f;
                recent.clear();
            }
            recent.push_back (t.pitch[(size_t) f]);
            ++f;
        }
        finish (from, f);
    }
    return notes;
}

//==============================================================================
namespace
{
/** A value that holds over each note and eases (over `ramp` seconds) into its neighbours,
    or to `outside` where there's a gap. */
template <typename ValueOf>
float noteValue (const std::vector<PitchNote>& notes, double sample, double sampleRate, float outside, ValueOf valueOf)
{
    if (notes.empty()) return outside;
    const double ramp = 0.008 * sampleRate;
    auto it = std::upper_bound (notes.begin(), notes.end(), sample, [] (double s, const PitchNote& n) { return s < n.start; });
    // `it` is the first note starting after the sample; the one before may contain it.
    const PitchNote* inside = nullptr;
    if (it != notes.begin() && sample < std::prev (it)->end)
        inside = &*std::prev (it);

    if (inside == nullptr)
    {
        // In a gap: ease out of the previous note and into the next.
        float v = outside;
        if (it != notes.begin())
        {
            const auto& p = *std::prev (it);
            if (sample - p.end < ramp) v = juce::jmap ((float) ((sample - p.end) / ramp), 0.0f, 1.0f, 0.5f * (valueOf (p, sample) + outside), outside);
        }
        if (it != notes.end() && it->start - sample < ramp)
            v = juce::jmap ((float) ((it->start - sample) / ramp), 0.0f, 1.0f, 0.5f * (valueOf (*it, sample) + outside), v);
        return v;
    }

    const float here = valueOf (*inside, sample);
    const auto idx = (size_t) (inside - notes.data());
    if (sample - inside->start < ramp)
    {
        float before = outside;
        if (idx > 0 && inside->start - notes[idx - 1].end < 2.0 * ramp) before = valueOf (notes[idx - 1], sample);
        return juce::jmap ((float) ((sample - inside->start) / ramp), 0.0f, 1.0f, 0.5f * (before + here), here);
    }
    if (inside->end - sample < ramp)
    {
        float after = outside;
        if (idx + 1 < notes.size() && notes[idx + 1].start - inside->end < 2.0 * ramp) after = valueOf (notes[idx + 1], sample);
        return juce::jmap ((float) ((inside->end - sample) / ramp), 0.0f, 1.0f, 0.5f * (after + here), here);
    }
    return here;
}
}

float transposeAt (const std::vector<PitchNote>& notes, const PitchTrack& track, double sample, double sampleRate)
{
    return noteValue (notes, sample, sampleRate, 0.0f, [&track] (const PitchNote& n, double s)
    {
        float v = n.shift;
        const float p = track.at (s), trend = track.trendAt (s);
        if (p > 0.0f && trend > 0.0f)
            v += (n.drift - 1.0f) * (trend - n.pitch) + (n.vibrato - 1.0f) * (p - trend);
        return v;
    });
}

float formantAt (const std::vector<PitchNote>& notes, double sample, double sampleRate)
{
    return noteValue (notes, sample, sampleRate, 0.0f, [] (const PitchNote& n, double) { return n.formant; });
}

float gainAt (const std::vector<PitchNote>& notes, double sample, double sampleRate)
{
    return noteValue (notes, sample, sampleRate, 0.0f, [] (const PitchNote& n, double) { return n.gain; });
}

//==============================================================================
std::shared_ptr<const std::vector<double>> AnalysisCache::transients (const juce::String& key, const Audio& audio, double sampleRate)
{
    {
        std::lock_guard<std::mutex> l (lock);
        if (auto it = transientMap.find (key); it != transientMap.end())
            return it->second;
    }
    if (audio == nullptr)
        return std::make_shared<std::vector<double>>();
    auto found = std::make_shared<const std::vector<double>> (detectTransients (audio->getReadPointer (0), audio->getNumSamples(), sampleRate));
    std::lock_guard<std::mutex> l (lock);
    transientMap[key] = found;
    return found;
}

std::shared_ptr<const PitchTrack> AnalysisCache::pitch (const juce::String& key, const Audio& audio, double sampleRate)
{
    {
        std::lock_guard<std::mutex> l (lock);
        if (auto it = pitchMap.find (key); it != pitchMap.end())
            return it->second;
    }
    if (audio == nullptr)
        return std::make_shared<PitchTrack>();
    auto track = std::make_shared<const PitchTrack> (trackPitch (audio->getReadPointer (0), audio->getNumSamples(), sampleRate));
    std::lock_guard<std::mutex> l (lock);
    pitchMap[key] = track;
    return track;
}

std::shared_ptr<const TempoEstimate> AnalysisCache::tempo (const juce::String& key, const Audio& audio, double sampleRate, int beatsPerBar)
{
    const auto fullKey = key + "#" + juce::String (beatsPerBar);
    {
        std::lock_guard<std::mutex> l (lock);
        if (auto it = tempoMap.find (fullKey); it != tempoMap.end())
            return it->second;
    }
    if (audio == nullptr)
        return std::make_shared<TempoEstimate>();
    auto estimate = std::make_shared<const TempoEstimate> (detectTempo (audio->getReadPointer (0), audio->getNumSamples(), sampleRate, beatsPerBar));
    std::lock_guard<std::mutex> l (lock);
    tempoMap[fullKey] = estimate;
    return estimate;
}

void AnalysisCache::clear()
{
    std::lock_guard<std::mutex> l (lock);
    transientMap.clear();
    pitchMap.clear();
    tempoMap.clear();
}

} // namespace woodshed::flex
