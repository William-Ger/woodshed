#pragma once

#include <juce_core/juce_core.h>

namespace woodshed::video
{

/**
    Keeps a movie player on the song's playhead. The player is a black box that runs on its own
    clock (the system's movie playback), so every UI tick the window asks this what to do:
    start or stop it with the transport, jump on a locate, and while both run, pull small drift
    back by nudging the play speed and only re-seek (which can stutter) when it's far off.
    No UI or video code, so the bench can check it.
*/
struct MovieSync
{
    /** What the player says about itself. */
    struct Player
    {
        bool open = false, playing = false;
        double position = 0.0, duration = 0.0, speed = 1.0;     // seconds
    };

    /** What to do to it. */
    struct Action
    {
        bool play = false, stop = false, seek = false;
        double seekTo = 0.0, speed = 1.0;
    };

    double seekThreshold = 0.08;        // further off than this (seconds): seek
    double driftThreshold = 0.008;      // closer than this: play at normal speed
    double correctionPerSecond = 1.5;   // speed change per second of error
    double maxNudge = 0.06;             // never faster or slower than this
    double stoppedTolerance = 1.0 / 120.0;

    /** `movieTime`: the playhead in the movie's own time (seconds, may be negative before it starts). */
    Action update (bool transportPlaying, double movieTime, const Player& player) const
    {
        Action a;
        if (! player.open)
            return a;

        const double clamped = juce::jlimit (0.0, juce::jmax (0.0, player.duration), movieTime);
        const bool inside = movieTime >= 0.0 && movieTime < player.duration;

        if (! transportPlaying || ! inside)
        {
            // Stopped (or outside the movie): hold the frame under the playhead.
            a.stop = player.playing;
            if (std::abs (player.position - clamped) > stoppedTolerance)
            {
                a.seek = true;
                a.seekTo = clamped;
            }
            return a;
        }

        if (! player.playing)
        {
            a.play = true;
            a.seek = std::abs (player.position - movieTime) > stoppedTolerance;
            a.seekTo = movieTime;
            return a;
        }

        const double error = player.position - movieTime;     // positive: the picture is early
        if (std::abs (error) > seekThreshold)
        {
            a.seek = true;
            a.seekTo = movieTime;
        }
        else if (std::abs (error) > driftThreshold)
            a.speed = 1.0 - juce::jlimit (-maxNudge, maxNudge, error * correctionPerSecond);
        return a;
    }

    /** SMPTE-style "HH:MM:SS:FF" (non-drop frame), with a minus sign before the movie starts. */
    static juce::String timecode (double seconds, double fps)
    {
        fps = juce::jmax (1.0, fps);
        const bool negative = seconds < 0.0;
        const auto frames = (juce::int64) std::floor (std::abs (seconds) * fps + 1.0e-6);
        const auto perSecond = (juce::int64) std::llround (fps);
        const auto ff = frames % perSecond;
        const auto totalSeconds = frames / perSecond;
        auto two = [] (juce::int64 v) { return juce::String (v).paddedLeft ('0', 2); };
        return (negative ? "-" : "") + two (totalSeconds / 3600) + ":" + two ((totalSeconds / 60) % 60) + ":"
             + two (totalSeconds % 60) + ":" + two (ff);
    }
};

} // namespace woodshed::video
