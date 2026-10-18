#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>

/**
    3D panning: where a track's sound sits around the listener, heard on headphones (binaural) or
    speakers (stereo), and placed on 5.1 or 7.1.4 speakers for a surround bounce.

    The binaural model is built from the cues themselves rather than measured head responses, so
    there's no dataset to ship: the interaural time difference (Woodworth's spherical head), the
    head's shadow on the far ear (Brown and Duda's one-pole shelf), a pinna notch that moves up
    with elevation, a dull top end for sources behind, and for distance the inverse-distance law,
    air absorption and (optionally) a few early reflections.

    Angles are in degrees: azimuth 0 straight ahead, positive to the right, +-180 behind;
    elevation 0 at ear level, positive up. Distance is in metres (1 m plays at unity gain).
*/
namespace woodshed::spatial
{

/** Where a source is, and how wide it is (0: a point; 1: its left and right channels 90 degrees apart). */
struct Position
{
    float azimuth = 0.0f, elevation = 0.0f, distance = 1.0f, spread = 0.0f;
    float room = 0.0f;      // early reflections, 0..1

    bool operator== (const Position& o) const
    {
        return juce::exactlyEqual (azimuth, o.azimuth) && juce::exactlyEqual (elevation, o.elevation)
            && juce::exactlyEqual (distance, o.distance) && juce::exactlyEqual (spread, o.spread)
            && juce::exactlyEqual (room, o.room);
    }
};

/** How a 3D track reaches the stereo master: amplitude panning for speakers, or binaural cues for headphones. */
enum class Monitoring { stereo, binaural };

constexpr float minDistance = 0.25f, maxDistance = 50.0f;
constexpr double headRadius = 0.0875, speedOfSound = 343.0;
/** Each channel's offset from the centre at full spread. */
constexpr float spreadDegrees = 45.0f;

/** The interaural time difference in seconds (Woodworth): positive when the right ear hears it first. */
double woodworthItd (double azimuthDegrees, double elevationDegrees);

/** A unit vector: x right, y front, z up. */
struct Direction { float x = 0.0f, y = 1.0f, z = 0.0f; };
Direction directionOf (float azimuthDegrees, float elevationDegrees);
float azimuthOf (Direction);
float elevationOf (Direction);

/** Inverse distance, unity at 1 m, at most +12 dB up close. */
float distanceGain (float metres);
/** The stereo pan (-1..1) a 3D position collapses to on speakers. */
float stereoPanFor (float azimuthDegrees, float elevationDegrees);
/** A stereo-panned track as a front pair, for the surround bounce: its two channels on the
    left and right speakers (+-30 degrees) when centred, both on one side when panned hard. */
Position fromStereoPan (float pan);

//==============================================================================
/** A speaker layout for the surround bounce, in the order the channels are written. */
struct SpeakerLayout
{
    juce::String name;
    juce::AudioChannelSet channels;     // WAV channel mask order
    struct Speaker { juce::String label; float azimuth = 0.0f, elevation = 0.0f; bool lfe = false; };
    std::vector<Speaker> speakers;      // one per channel, in the same order

    int size() const                                        { return (int) speakers.size(); }

    /** Amplitude gains (one per channel, unit power, nothing into the LFE) for a direction:
        pairwise VBAP around the ear-level ring, and around the height ring for elevated
        sources, cross-faded by elevation. */
    void gains (float azimuthDegrees, float elevationDegrees, float* out) const;

    static const SpeakerLayout& fivePointOne();
    static const SpeakerLayout& sevenPointOnePointFour();
    /** "5.1" or "7.1.4"; null for anything else (stereo). */
    static const SpeakerLayout* find (const juce::String& name);
};

//==============================================================================
/**
    One track's 3D panner. Its left and right channels are two virtual sources (one, when the
    spread is 0). Positions arrive once a block; everything that moves is smoothed per sample
    and the filters are recomputed every few samples, so automation never zips.

    prepare() allocates (message thread); everything else is real-time safe.
*/
class Panner
{
public:
    Panner();

    void prepare (double sampleRate);
    bool isPrepared() const                                 { return prepared; }
    /** Forgets the past (delay lines, filters) and jumps straight to the next position. */
    void reset();

    /** Where to be by the end of the next ~20 ms. */
    void setTarget (const Position&);

    /** In place: the faded track's left and right in, the panned mix out. */
    void render (float* left, float* right, int numSamples, Monitoring);
    /** Adds the track to the layout's channels (`out` has layout.size() channels). */
    void renderSurround (const float* left, const float* right, int numSamples, const SpeakerLayout&,
                         float* const* out);

    static constexpr int maxChannels = 16;
    static constexpr int controlInterval = 16;     // samples between filter updates

private:
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float process (float x) noexcept
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void peak (double sampleRate, double hz, double q, double gainDb);
        void highShelf (double sampleRate, double hz, double gainDb);
    };

    struct OnePole      // first-order section: y = b0 x + b1 x[-1] - a1 y[-1]
    {
        float b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;
        float process (float x) noexcept
        {
            const float y = b0 * x + b1 * x1 - a1 * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };

    /** One virtual source: a mono signal at one place. */
    struct Voice
    {
        std::vector<float> ring;
        int mask = 0, write = 0;
        Biquad notch, rearShelf;
        float air = 0.0f, airCoeff = 1.0f;
        OnePole shadowLeft, shadowRight;
        // Per-sample ramps between control updates: delays (samples) and gains.
        float delayLeft = 0.0f, delayRight = 0.0f, stepDelayLeft = 0.0f, stepDelayRight = 0.0f;
        float gain = 1.0f, stepGain = 0.0f, panLeft = 1.0f, panRight = 1.0f, stepPanLeft = 0.0f, stepPanRight = 0.0f;
        float roomGain = 0.0f, stepRoomGain = 0.0f;
        std::array<float, maxChannels> speaker {}, stepSpeaker {};
        bool started = false;

        void clear();
        float read (float delay) const noexcept;
    };

    /** Points the voice at a direction for the next control interval of `n` samples. */
    void aim (Voice&, Direction, float distance, float room, Monitoring, const SpeakerLayout*, int n, bool binaural);
    void advance (int n);
    void process (Voice&, const float* in, float inGain, float* left, float* right, int n, Monitoring);
    void processSurround (Voice&, const float* in, float inGain, int n, int numChannels, float* const* out, int offset);

    double sampleRate = 48000.0;
    bool prepared = false, fresh = true;
    std::array<Voice, 2> voices;
    juce::SmoothedValue<float> dirX, dirY, dirZ, distance, spread, room;

    // Early reflections: path lengths beyond the direct sound (metres), which ear, and gain.
    struct Reflection { float extra; int ear; float gain; };
    static constexpr std::array<Reflection, 6> reflections { { { 1.1f, 0, 0.55f }, { 1.6f, 1, 0.5f }, { 2.4f, 0, 0.4f },
                                                               { 3.1f, 1, 0.38f }, { 4.3f, 0, 0.3f }, { 5.2f, 1, 0.28f } } };
};

} // namespace woodshed::spatial
