#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

namespace woodshed::app::arrange
{

/**
    One AudioThumbnail per audio file, built in the background and shared by every region that
    plays the file, plus each file's length and sample rate (read once from its header).
    Broadcasts a change whenever a thumbnail has more data to show.
*/
class WaveformCache final : public juce::ChangeBroadcaster,
                            private juce::ChangeListener
{
public:
    WaveformCache();
    ~WaveformCache() override;

    struct Info
    {
        juce::int64 length = -1;        // samples in the file, -1 if unreadable
        double sampleRate = 0.0;
    };

    /** Null if the file can't be read. */
    juce::AudioThumbnail* thumbnail (const juce::File&);
    Info info (const juce::File&);

    /** Drops everything (the project was replaced). */
    void clear();

    juce::AudioFormatManager formats;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { sendChangeMessage(); }

    juce::AudioThumbnailCache thumbCache { 32 };
    std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbs;
    std::map<juce::String, Info> infos;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformCache)
};

} // namespace woodshed::app::arrange
