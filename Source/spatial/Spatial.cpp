#include "Spatial.h"

namespace woodshed::spatial
{

namespace
{
constexpr double pi = juce::MathConstants<double>::pi;
constexpr float degToRad = juce::MathConstants<float>::pi / 180.0f;
constexpr float radToDeg = 180.0f / juce::MathConstants<float>::pi;
constexpr double smoothingSeconds = 0.02;

/** Brown and Duda's head-shadow coefficient for an ear: 2 (+6 dB of treble) facing the
    source, 0.1 (-20 dB) at 150 degrees round the back of the head. */
double shadowAlpha (double degreesFromEar)
{
    constexpr double alphaMin = 0.1, thetaMin = 150.0;
    return (1.0 + alphaMin / 2.0) + (1.0 - alphaMin / 2.0) * std::cos (degreesFromEar / thetaMin * pi);
}

/** H(s) = (1 + alpha s / 2w0) / (1 + s / 2w0), w0 = c / a, through the bilinear transform. */
void setShadow (float& b0, float& b1, float& a1, double alpha, double sampleRate)
{
    const double beta = 2.0 * speedOfSound / headRadius;
    const double k = 2.0 * sampleRate;
    const double norm = 1.0 / (beta + k);
    b0 = (float) ((beta + alpha * k) * norm);
    b1 = (float) ((beta - alpha * k) * norm);
    a1 = (float) ((beta - k) * norm);
}

/** Each channel's level: half while the two coincide (so a mono track sums to unity), full once
    they're 60 degrees apart (a centred stereo pair, like the pan knob's unity at each side). */
float voiceGain (float spread)
{
    return 0.5f + 0.5f * juce::jmin (1.0f, spread * 2.0f * spreadDegrees / 60.0f);
}

float wrap180 (float degrees)
{
    degrees = std::fmod (degrees + 180.0f, 360.0f);
    if (degrees < 0.0f)
        degrees += 360.0f;
    return degrees - 180.0f;
}

float wrap360 (float degrees)
{
    degrees = std::fmod (degrees, 360.0f);
    return degrees < 0.0f ? degrees + 360.0f : degrees;
}

/** Pairwise 2D VBAP around a ring of speakers (indices into `speakers`), adding `weight`
    times the unit-power gains into `out`. */
void ringGains (const std::vector<SpeakerLayout::Speaker>& speakers, const std::vector<int>& ring,
                float azimuth, float weight, float* out)
{
    if (ring.empty() || weight <= 0.0f)
        return;
    if (ring.size() == 1)
    {
        out[ring[0]] += weight;
        return;
    }

    for (size_t i = 0; i < ring.size(); ++i)
    {
        const int a = ring[i], b = ring[(i + 1) % ring.size()];
        const float from = speakers[(size_t) a].azimuth;
        const float span = wrap360 (speakers[(size_t) b].azimuth - from);
        const float into = wrap360 (azimuth - from);
        if (into > span + 1.0e-4f)
            continue;

        // Solve g1 l1 + g2 l2 = p for the two speakers' unit vectors.
        const float a1 = speakers[(size_t) a].azimuth * degToRad, a2 = speakers[(size_t) b].azimuth * degToRad, p = azimuth * degToRad;
        const float l1x = std::sin (a1), l1y = std::cos (a1), l2x = std::sin (a2), l2y = std::cos (a2);
        const float px = std::sin (p), py = std::cos (p);
        const float det = l1x * l2y - l2x * l1y;
        float g1 = 1.0f, g2 = 0.0f;
        if (std::abs (det) > 1.0e-6f)
        {
            g1 = juce::jmax (0.0f, (px * l2y - py * l2x) / det);
            g2 = juce::jmax (0.0f, (py * l1x - px * l1y) / det);
        }
        const float norm = std::sqrt (g1 * g1 + g2 * g2);
        if (norm > 0.0f)
        {
            out[a] += weight * g1 / norm;
            out[b] += weight * g2 / norm;
        }
        return;
    }
}
}

//==============================================================================
double woodworthItd (double azimuthDegrees, double elevationDegrees)
{
    // The lateral angle (from the median plane) is what the ears can tell apart; a source
    // behind has the same time difference as its mirror image in front.
    const double lateral = std::asin (juce::jlimit (-1.0, 1.0, std::sin (azimuthDegrees * pi / 180.0)
                                                                    * std::cos (elevationDegrees * pi / 180.0)));
    return headRadius / speedOfSound * (lateral + std::sin (lateral));
}

Direction directionOf (float azimuthDegrees, float elevationDegrees)
{
    const float az = azimuthDegrees * degToRad, el = elevationDegrees * degToRad;
    return { std::sin (az) * std::cos (el), std::cos (az) * std::cos (el), std::sin (el) };
}

float azimuthOf (Direction d)       { return std::abs (d.x) + std::abs (d.y) < 1.0e-6f ? 0.0f : std::atan2 (d.x, d.y) * radToDeg; }
float elevationOf (Direction d)     { return std::asin (juce::jlimit (-1.0f, 1.0f, d.z)) * radToDeg; }

float distanceGain (float metres)
{
    return 1.0f / juce::jlimit (minDistance, maxDistance, metres);
}

float stereoPanFor (float azimuthDegrees, float elevationDegrees)
{
    return juce::jlimit (-1.0f, 1.0f, directionOf (azimuthDegrees, elevationDegrees).x);
}

Position fromStereoPan (float pan)
{
    pan = juce::jlimit (-1.0f, 1.0f, pan);
    Position p;
    p.azimuth = pan * 30.0f;
    p.spread = (1.0f - std::abs (pan)) * 30.0f / spreadDegrees;
    return p;
}

//==============================================================================
void SpeakerLayout::gains (float azimuthDegrees, float elevationDegrees, float* out) const
{
    std::fill (out, out + size(), 0.0f);
    std::vector<int> ear, height;
    for (int i = 0; i < size(); ++i)
    {
        const auto& s = speakers[(size_t) i];
        if (s.lfe)
            continue;
        (s.elevation > 10.0f ? height : ear).push_back (i);
    }
    auto byAzimuth = [this] (int a, int b) { return wrap360 (speakers[(size_t) a].azimuth) < wrap360 (speakers[(size_t) b].azimuth); };
    std::sort (ear.begin(), ear.end(), byAzimuth);
    std::sort (height.begin(), height.end(), byAzimuth);

    // Up to the height ring's elevation, cross-fade (constant power) from the ear ring to it.
    const float ringElevation = height.empty() ? 90.0f : speakers[(size_t) height[0]].elevation;
    const float up = height.empty() ? 0.0f : juce::jlimit (0.0f, 1.0f, elevationDegrees / ringElevation);
    const float az = wrap180 (azimuthDegrees);
    ringGains (speakers, ear, az, std::cos (up * juce::MathConstants<float>::halfPi), out);
    ringGains (speakers, height, az, std::sin (up * juce::MathConstants<float>::halfPi), out);
}

const SpeakerLayout& SpeakerLayout::fivePointOne()
{
    static const SpeakerLayout layout = []
    {
        using CT = juce::AudioChannelSet::ChannelType;
        SpeakerLayout l;
        l.name = "5.1";
        for (auto t : { CT::left, CT::right, CT::centre, CT::LFE, CT::leftSurround, CT::rightSurround })
            l.channels.addChannel (t);
        l.speakers = { { "L", -30.0f, 0.0f }, { "R", 30.0f, 0.0f }, { "C", 0.0f, 0.0f }, { "LFE", 0.0f, 0.0f, true },
                       { "Ls", -110.0f, 0.0f }, { "Rs", 110.0f, 0.0f } };
        return l;
    }();
    return layout;
}

const SpeakerLayout& SpeakerLayout::sevenPointOnePointFour()
{
    // WAV order follows the channel mask: front, centre, LFE, rear surrounds (back left and
    // right), side surrounds, then the four heights.
    static const SpeakerLayout layout = []
    {
        using CT = juce::AudioChannelSet::ChannelType;
        SpeakerLayout l;
        l.name = "7.1.4";
        for (auto t : { CT::left, CT::right, CT::centre, CT::LFE, CT::leftSurround, CT::rightSurround,
                        CT::leftSurroundSide, CT::rightSurroundSide,
                        CT::topFrontLeft, CT::topFrontRight, CT::topRearLeft, CT::topRearRight })
            l.channels.addChannel (t);
        l.speakers = { { "L", -30.0f, 0.0f }, { "R", 30.0f, 0.0f }, { "C", 0.0f, 0.0f }, { "LFE", 0.0f, 0.0f, true },
                       { "Lrs", -135.0f, 0.0f }, { "Rrs", 135.0f, 0.0f }, { "Lss", -90.0f, 0.0f }, { "Rss", 90.0f, 0.0f },
                       { "Ltf", -45.0f, 45.0f }, { "Rtf", 45.0f, 45.0f }, { "Ltr", -135.0f, 45.0f }, { "Rtr", 135.0f, 45.0f } };
        return l;
    }();
    return layout;
}

const SpeakerLayout* SpeakerLayout::find (const juce::String& name)
{
    if (name == "5.1")      return &fivePointOne();
    if (name == "7.1.4")    return &sevenPointOnePointFour();
    return nullptr;
}

//==============================================================================
void Panner::Biquad::peak (double sr, double hz, double q, double gainDb)
{
    // RBJ peaking EQ.
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w = 2.0 * pi * juce::jmin (hz, sr * 0.45) / sr;
    const double alpha = std::sin (w) / (2.0 * q);
    const double a0 = 1.0 + alpha / A;
    b0 = (float) ((1.0 + alpha * A) / a0);
    b1 = (float) (-2.0 * std::cos (w) / a0);
    b2 = (float) ((1.0 - alpha * A) / a0);
    a1 = b1;
    a2 = (float) ((1.0 - alpha / A) / a0);
}

void Panner::Biquad::highShelf (double sr, double hz, double gainDb)
{
    // RBJ high shelf, slope 1.
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w = 2.0 * pi * juce::jmin (hz, sr * 0.45) / sr;
    const double cw = std::cos (w), alpha = std::sin (w) / 2.0 * std::sqrt (2.0);
    const double sq = 2.0 * std::sqrt (A) * alpha;
    const double a0 = (A + 1.0) - (A - 1.0) * cw + sq;
    b0 = (float) (A * ((A + 1.0) + (A - 1.0) * cw + sq) / a0);
    b1 = (float) (-2.0 * A * ((A - 1.0) + (A + 1.0) * cw) / a0);
    b2 = (float) (A * ((A + 1.0) + (A - 1.0) * cw - sq) / a0);
    a1 = (float) (2.0 * ((A - 1.0) - (A + 1.0) * cw) / a0);
    a2 = (float) (((A + 1.0) - (A - 1.0) * cw - sq) / a0);
}

void Panner::Voice::clear()
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    write = 0;
    notch.z1 = notch.z2 = rearShelf.z1 = rearShelf.z2 = 0.0f;
    air = 0.0f;
    shadowLeft.x1 = shadowLeft.y1 = shadowRight.x1 = shadowRight.y1 = 0.0f;
    started = false;
}

float Panner::Voice::read (float delay) const noexcept
{
    const int whole = (int) delay;
    const float frac = delay - (float) whole;
    const float a = ring[(size_t) ((write - whole) & mask)];
    const float b = ring[(size_t) ((write - whole - 1) & mask)];
    return a + (b - a) * frac;
}

Panner::Panner()
{
    for (auto* s : { &dirX, &dirY, &dirZ, &distance, &spread, &room })
        s->reset (sampleRate, smoothingSeconds);
}

void Panner::prepare (double rate)
{
    sampleRate = rate;
    // The longest path: the farthest reflection plus the largest ear delay, with room to spare.
    const int needed = (int) std::ceil ((reflections.back().extra / speedOfSound + 0.002) * rate) + controlInterval + 4;
    int size = 1;
    while (size < needed)
        size <<= 1;
    for (auto& v : voices)
    {
        v.ring.assign ((size_t) size, 0.0f);
        v.mask = size - 1;
    }
    for (auto* s : { &dirX, &dirY, &dirZ, &distance, &spread, &room })
        s->reset (sampleRate, smoothingSeconds);
    prepared = true;
    reset();
}

void Panner::reset()
{
    for (auto& v : voices)
        v.clear();
    fresh = true;
}

void Panner::setTarget (const Position& p)
{
    const auto d = directionOf (p.azimuth, juce::jlimit (-90.0f, 90.0f, p.elevation));
    const float dist = juce::jlimit (minDistance, maxDistance, p.distance);
    const float sp = juce::jlimit (0.0f, 1.0f, p.spread);
    const float rm = juce::jlimit (0.0f, 1.0f, p.room);
    if (fresh)
    {
        dirX.setCurrentAndTargetValue (d.x);
        dirY.setCurrentAndTargetValue (d.y);
        dirZ.setCurrentAndTargetValue (d.z);
        distance.setCurrentAndTargetValue (dist);
        spread.setCurrentAndTargetValue (sp);
        room.setCurrentAndTargetValue (rm);
        fresh = false;
        return;
    }
    dirX.setTargetValue (d.x);
    dirY.setTargetValue (d.y);
    dirZ.setTargetValue (d.z);
    distance.setTargetValue (dist);
    spread.setTargetValue (sp);
    room.setTargetValue (rm);
}

void Panner::advance (int n)
{
    for (auto* s : { &dirX, &dirY, &dirZ, &distance, &spread, &room })
        s->skip (n);
}

void Panner::aim (Voice& v, Direction d, float dist, float roomAmount, Monitoring mode, const SpeakerLayout* layout, int n, bool binaural)
{
    const float inv = 1.0f / (float) n;
    auto ramp = [&v, inv] (float& value, float& step, float target)
    {
        if (! v.started) { value = target; step = 0.0f; }
        else             step = (target - value) * inv;
    };

    // Distance: the inverse-distance law and air absorption (the far top end goes first).
    const float airHz = (float) juce::jmin (20000.0, 22000.0 / (1.0 + 0.06 * juce::jmax (0.0, (double) dist - 1.0)));
    v.airCoeff = dist <= 1.0f ? 1.0f : 1.0f - std::exp (-juce::MathConstants<float>::twoPi * airHz / (float) sampleRate);
    ramp (v.gain, v.stepGain, distanceGain (dist));
    ramp (v.roomGain, v.stepRoomGain, roomAmount * distanceGain (dist + 2.0f));

    if (layout != nullptr)
    {
        std::array<float, maxChannels> g {};
        layout->gains (azimuthOf (d), elevationOf (d), g.data());
        for (int ch = 0; ch < juce::jmin (maxChannels, layout->size()); ++ch)
            ramp (v.speaker[(size_t) ch], v.stepSpeaker[(size_t) ch], g[(size_t) ch]);
        v.started = true;
        return;
    }

    if (binaural && mode == Monitoring::binaural)
    {
        // Time: the far ear hears it later (Woodworth).
        const double lateral = std::asin (juce::jlimit (-1.0f, 1.0f, d.x));
        const double itd = headRadius / speedOfSound * (lateral + std::sin (lateral)) * sampleRate;
        ramp (v.delayLeft, v.stepDelayLeft, (float) juce::jmax (0.0, itd));
        ramp (v.delayRight, v.stepDelayRight, (float) juce::jmax (0.0, -itd));
        // A real head also shades the far ear a little at low frequencies (about 3 dB at 90 degrees),
        // which the treble shelf below doesn't: a broadband level difference on top.
        const float shade = 1.0f - 0.3f * (float) std::abs (lateral) / juce::MathConstants<float>::halfPi;
        ramp (v.panLeft, v.stepPanLeft, d.x > 0.0f ? shade : 1.0f);
        ramp (v.panRight, v.stepPanRight, d.x < 0.0f ? shade : 1.0f);

        // Level: the head shadows the far ear's treble.
        const double fromRight = std::acos (juce::jlimit (-1.0, 1.0, (double) d.x)) * 180.0 / pi;
        setShadow (v.shadowRight.b0, v.shadowRight.b1, v.shadowRight.a1, shadowAlpha (fromRight), sampleRate);
        setShadow (v.shadowLeft.b0, v.shadowLeft.b1, v.shadowLeft.a1, shadowAlpha (180.0 - fromRight), sampleRate);

        // Elevation: the pinna's first notch climbs from about 5 kHz below to 12 kHz above, and
        // fills in as the source rises. Behind: the pinna dulls the top end.
        const double elevation = elevationOf (d);
        v.notch.peak (sampleRate, 7000.0 * std::pow (2.0, elevation / 90.0 * 0.8), 3.0,
                      -9.0 + 4.0 * juce::jmax (0.0, elevation / 90.0));
        v.rearShelf.highShelf (sampleRate, 3500.0, -10.0 * juce::jmax (0.0f, -d.y));
    }
    else
    {
        // Speakers: amplitude panning, the same law as the stereo pan knob (unity in the middle).
        ramp (v.delayLeft, v.stepDelayLeft, 0.0f);
        ramp (v.delayRight, v.stepDelayRight, 0.0f);
        const float angle = (juce::jlimit (-1.0f, 1.0f, d.x) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        ramp (v.panLeft, v.stepPanLeft, std::cos (angle) * juce::MathConstants<float>::sqrt2);
        ramp (v.panRight, v.stepPanRight, std::sin (angle) * juce::MathConstants<float>::sqrt2);
        v.shadowLeft.b0 = v.shadowRight.b0 = 1.0f;
        v.shadowLeft.b1 = v.shadowRight.b1 = v.shadowLeft.a1 = v.shadowRight.a1 = 0.0f;
        v.notch = {};
        v.rearShelf = {};
    }
    v.started = true;
}

void Panner::process (Voice& v, const float* in, float inGain, float* left, float* right, int n, Monitoring)
{
    const float sr = (float) sampleRate;
    for (int i = 0; i < n; ++i)
    {
        float s = in[i] * inGain;
        v.air += v.airCoeff * (s - v.air);
        s = v.rearShelf.process (v.notch.process (v.air));
        v.ring[(size_t) v.write] = s;

        const float l = v.shadowLeft.process (v.read (v.delayLeft));
        const float r = v.shadowRight.process (v.read (v.delayRight));
        left[i] += l * v.gain * v.panLeft;
        right[i] += r * v.gain * v.panRight;

        if (v.roomGain > 1.0e-6f)
            for (auto& refl : reflections)
            {
                const float e = v.read (refl.extra / (float) speedOfSound * sr) * v.roomGain * refl.gain;
                (refl.ear == 0 ? left : right)[i] += e;
                (refl.ear == 0 ? right : left)[i] += e * 0.3f;
            }

        v.write = (v.write + 1) & v.mask;
        v.delayLeft += v.stepDelayLeft;
        v.delayRight += v.stepDelayRight;
        v.gain += v.stepGain;
        v.panLeft += v.stepPanLeft;
        v.panRight += v.stepPanRight;
        v.roomGain += v.stepRoomGain;
    }
}

void Panner::processSurround (Voice& v, const float* in, float inGain, int n, int numChannels, float* const* out, int offset)
{
    for (int i = 0; i < n; ++i)
    {
        v.air += v.airCoeff * (in[i] * inGain - v.air);
        const float s = v.air * v.gain;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            out[ch][offset + i] += s * v.speaker[(size_t) ch];
            v.speaker[(size_t) ch] += v.stepSpeaker[(size_t) ch];
        }
        v.gain += v.stepGain;
    }
}

void Panner::render (float* left, float* right, int numSamples, Monitoring mode)
{
    jassert (prepared);
    std::array<float, controlInterval> inLeft {}, inRight {};

    for (int done = 0; done < numSamples;)
    {
        const int n = juce::jmin (controlInterval, numSamples - done);
        advance (n);

        Direction d { dirX.getCurrentValue(), dirY.getCurrentValue(), dirZ.getCurrentValue() };
        const float len = std::sqrt (d.x * d.x + d.y * d.y + d.z * d.z);
        if (len > 1.0e-3f)
            d = { d.x / len, d.y / len, d.z / len };
        else
            d = { 0.0f, 1.0f, 0.0f };

        // The left channel turns one way and the right the other, by the spread.
        const float half = spread.getCurrentValue() * spreadDegrees * degToRad;
        const float c = std::cos (half), s = std::sin (half);
        const Direction dl { d.x * c - d.y * s, d.y * c + d.x * s, d.z };
        const Direction dr { d.x * c + d.y * s, d.y * c - d.x * s, d.z };
        aim (voices[0], dl, distance.getCurrentValue(), room.getCurrentValue(), mode, nullptr, n, true);
        aim (voices[1], dr, distance.getCurrentValue(), room.getCurrentValue(), mode, nullptr, n, true);

        std::copy (left + done, left + done + n, inLeft.begin());
        std::copy (right + done, right + done + n, inRight.begin());
        std::fill (left + done, left + done + n, 0.0f);
        std::fill (right + done, right + done + n, 0.0f);
        const float g = voiceGain (spread.getCurrentValue());
        process (voices[0], inLeft.data(), g, left + done, right + done, n, mode);
        process (voices[1], inRight.data(), g, left + done, right + done, n, mode);
        done += n;
    }
}

void Panner::renderSurround (const float* left, const float* right, int numSamples, const SpeakerLayout& layout, float* const* out)
{
    jassert (prepared);
    const int channels = juce::jmin (maxChannels, layout.size());

    for (int done = 0; done < numSamples;)
    {
        const int n = juce::jmin (controlInterval, numSamples - done);
        advance (n);

        Direction d { dirX.getCurrentValue(), dirY.getCurrentValue(), dirZ.getCurrentValue() };
        const float len = std::sqrt (d.x * d.x + d.y * d.y + d.z * d.z);
        d = len > 1.0e-3f ? Direction { d.x / len, d.y / len, d.z / len } : Direction {};

        const float half = spread.getCurrentValue() * spreadDegrees * degToRad;
        const float c = std::cos (half), s = std::sin (half);
        const Direction dl { d.x * c - d.y * s, d.y * c + d.x * s, d.z };
        const Direction dr { d.x * c + d.y * s, d.y * c - d.x * s, d.z };
        aim (voices[0], dl, distance.getCurrentValue(), 0.0f, Monitoring::stereo, &layout, n, false);
        aim (voices[1], dr, distance.getCurrentValue(), 0.0f, Monitoring::stereo, &layout, n, false);

        const float g = voiceGain (spread.getCurrentValue());
        processSurround (voices[0], left + done, g, n, channels, out, done);
        processSurround (voices[1], right + done, g, n, channels, out, done);
        done += n;
    }
}

} // namespace woodshed::spatial
