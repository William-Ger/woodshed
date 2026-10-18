#include "MovieMedia.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace woodshed::video
{

bool extractAudioWithJuce (const juce::File& movie, const juce::File& wav, double sampleRate, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (movie));
    if (reader == nullptr || reader->lengthInSamples <= 0)
    {
        error = "This system can't read the movie's sound";
        return false;
    }

    wav.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (wav.createOutputStream());
    if (stream == nullptr)
    {
        error = "Couldn't write " + wav.getFileName();
        return false;
    }
    const int channels = (int) juce::jlimit (1u, 2u, reader->numChannels);
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (sampleRate)
                                                      .withNumChannels (channels)
                                                      .withBitsPerSample (32)
                                                      .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer == nullptr)
    {
        error = "Couldn't convert the movie's sound";
        return false;
    }

    // Through a resampler to the project's rate.
    juce::AudioFormatReaderSource source (reader.get(), false);
    juce::ResamplingAudioSource resampler (&source, false, channels);
    resampler.setResamplingRatio (reader->sampleRate / sampleRate);
    constexpr int block = 4096;
    resampler.prepareToPlay (block, sampleRate);
    juce::AudioBuffer<float> buffer (channels, block);
    const auto total = (juce::int64) std::llround ((double) reader->lengthInSamples * sampleRate / reader->sampleRate);
    for (juce::int64 done = 0; done < total; done += block)
    {
        const int n = (int) juce::jmin ((juce::int64) block, total - done);
        resampler.getNextAudioBlock (juce::AudioSourceChannelInfo (&buffer, 0, n));
        if (! writer->writeFromAudioSampleBuffer (buffer, 0, n))
        {
            error = "Couldn't write " + wav.getFileName();
            return false;
        }
    }
    return true;
}

#if ! JUCE_MAC
MovieInfo probe (const juce::File& movie)
{
    MovieInfo info;
    info.ok = movie.existsAsFile();
    return info;
}

bool canMakeThumbnails()                                        { return false; }
juce::Image frameAt (const juce::File&, double, int)            { return {}; }

bool extractAudio (const juce::File& movie, const juce::File& wav, double sampleRate, juce::String& error)
{
    return extractAudioWithJuce (movie, wav, sampleRate, error);
}
#endif

} // namespace woodshed::video
