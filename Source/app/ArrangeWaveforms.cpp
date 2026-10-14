#include "ArrangeWaveforms.h"

namespace woodshed::app::arrange
{

WaveformCache::WaveformCache()
{
    formats.registerBasicFormats();
}

WaveformCache::~WaveformCache()
{
    clear();
}

void WaveformCache::clear()
{
    for (auto& [path, thumb] : thumbs)
        thumb->removeChangeListener (this);
    thumbs.clear();
    infos.clear();
    thumbCache.clear();
}

WaveformCache::Info WaveformCache::info (const juce::File& file)
{
    const auto key = file.getFullPathName();
    if (auto it = infos.find (key); it != infos.end())
        return it->second;

    Info i;
    if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) })
    {
        i.length = reader->lengthInSamples;
        i.sampleRate = reader->sampleRate;
    }

    // A file still being written (a take in progress) isn't remembered until it's readable.
    if (i.length > 0)
        infos[key] = i;
    return i;
}

juce::AudioThumbnail* WaveformCache::thumbnail (const juce::File& file)
{
    const auto key = file.getFullPathName();
    if (auto it = thumbs.find (key); it != thumbs.end())
        return it->second.get();

    if (info (file).length <= 0)
        return nullptr;

    auto thumb = std::make_unique<juce::AudioThumbnail> (64, formats, thumbCache);
    thumb->setSource (new juce::FileInputSource (file));
    thumb->addChangeListener (this);
    return (thumbs[key] = std::move (thumb)).get();
}

} // namespace woodshed::app::arrange
