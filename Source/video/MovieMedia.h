#pragma once

#include <juce_graphics/juce_graphics.h>

/**
    What the studio needs from a movie file besides playing it: its length and frame rate,
    still frames for the thumbnail strip, and its sound as a WAV. On macOS these come from the
    system's media framework (MovieMedia_mac.mm); elsewhere frames aren't available and the
    sound is read with whatever audio formats JUCE has on the platform.
*/
namespace woodshed::video
{

struct MovieInfo
{
    bool ok = false;
    double duration = 0.0, fps = 0.0;       // seconds; frames per second (0 if unknown)
    int width = 0, height = 0;
    bool hasAudio = false;
};

/** Reads the movie's header (fast; message thread is fine). */
MovieInfo probe (const juce::File& movie);

/** True where frameAt can return pictures. */
bool canMakeThumbnails();

/** The frame at `seconds`, `height` pixels tall; an invalid image if it can't. Slow: call it
    from a background thread. */
juce::Image frameAt (const juce::File& movie, double seconds, int height);

/** Decodes the movie's sound track into a WAV file (32-bit float, mono or stereo) at
    `sampleRate`, so it plays at the right speed in the project. False, with a reason, if
    there's no sound or this system can't read it. Slow: call it from a background thread. */
bool extractAudio (const juce::File& movie, const juce::File& wav, double sampleRate, juce::String& error);

/** The fallback for extractAudio: any audio format JUCE can open (MPEG-4 sound on macOS and
    Windows Media formats on Windows, when the system has them), resampled. */
bool extractAudioWithJuce (const juce::File& movie, const juce::File& wav, double sampleRate, juce::String& error);

} // namespace woodshed::video
