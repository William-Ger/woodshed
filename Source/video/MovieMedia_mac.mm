#include "MovieMedia.h"
#include <juce_audio_formats/juce_audio_formats.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

// The synchronous asset calls are deprecated in favour of async ones from macOS 13 and 15, but
// the app supports macOS 12, and every caller here is already on a background thread.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace woodshed::video
{

namespace
{
AVURLAsset* assetFor (const juce::File& movie)
{
    NSString* path = [NSString stringWithUTF8String: movie.getFullPathName().toRawUTF8()];
    if (path == nil)
        path = @"";
    return [AVURLAsset URLAssetWithURL: [NSURL fileURLWithPath: path] options: @{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
}

juce::Image toImage (CGImageRef cg)
{
    const int w = (int) CGImageGetWidth (cg), h = (int) CGImageGetHeight (cg);
    if (w <= 0 || h <= 0)
        return {};

    juce::Image image (juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
    juce::Image::BitmapData pixels (image, juce::Image::BitmapData::writeOnly);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate (pixels.data, (size_t) w, (size_t) h, 8, (size_t) pixels.lineStride, space,
                                                  (CGBitmapInfo) kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    if (context != nullptr)
    {
        CGContextDrawImage (context, CGRectMake (0, 0, w, h), cg);
        CGContextRelease (context);
    }
    CGColorSpaceRelease (space);
    return image;
}
}

MovieInfo probe (const juce::File& movie)
{
    MovieInfo info;
    if (! movie.existsAsFile())
        return info;

    @autoreleasepool
    {
        AVURLAsset* asset = assetFor (movie);
        info.duration = CMTimeGetSeconds (asset.duration);
        info.ok = std::isfinite (info.duration) && info.duration > 0.0;
        if (AVAssetTrack* video = [[asset tracksWithMediaType: AVMediaTypeVideo] firstObject])
        {
            info.fps = video.nominalFrameRate;
            const auto size = CGSizeApplyAffineTransform (video.naturalSize, video.preferredTransform);
            info.width = (int) std::abs (size.width);
            info.height = (int) std::abs (size.height);
        }
        info.hasAudio = [[asset tracksWithMediaType: AVMediaTypeAudio] count] > 0;
    }
    return info;
}

bool canMakeThumbnails()    { return true; }

juce::Image frameAt (const juce::File& movie, double seconds, int height)
{
    juce::Image result;
    @autoreleasepool
    {
        AVAssetImageGenerator* generator = [AVAssetImageGenerator assetImageGeneratorWithAsset: assetFor (movie)];
        generator.appliesPreferredTrackTransform = YES;
        generator.maximumSize = CGSizeMake (height * 4, height);
        // A thumbnail doesn't need the exact frame; the nearest key frame is much faster.
        generator.requestedTimeToleranceBefore = CMTimeMakeWithSeconds (0.25, 600);
        generator.requestedTimeToleranceAfter = CMTimeMakeWithSeconds (0.25, 600);

        NSError* error = nil;
        CGImageRef cg = [generator copyCGImageAtTime: CMTimeMakeWithSeconds (juce::jmax (0.0, seconds), 600) actualTime: nil error: &error];
        if (cg != nullptr)
        {
            result = toImage (cg);
            CGImageRelease (cg);
        }
    }
    return result;
}

bool extractAudio (const juce::File& movie, const juce::File& wav, double sampleRate, juce::String& error)
{
    @autoreleasepool
    {
        AVURLAsset* asset = assetFor (movie);
        AVAssetTrack* track = [[asset tracksWithMediaType: AVMediaTypeAudio] firstObject];
        if (track == nil)
        {
            error = "The movie has no sound";
            return false;
        }

        const double rate = sampleRate;
        int channels = 2;
        if (id description = [track.formatDescriptions firstObject])
            if (const auto* asbd = CMAudioFormatDescriptionGetStreamBasicDescription ((CMAudioFormatDescriptionRef) description))
                channels = (int) juce::jlimit (1u, 2u, (unsigned int) asbd->mChannelsPerFrame);

        NSError* nsError = nil;
        AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset: asset error: &nsError];
        if (reader == nil)
            return extractAudioWithJuce (movie, wav, sampleRate, error);

        // Interleaved 32-bit float at the project's rate, mixed down to (at most) stereo.
        NSDictionary* settings = @{ AVFormatIDKey: @(kAudioFormatLinearPCM), AVLinearPCMBitDepthKey: @32,
                                    AVLinearPCMIsFloatKey: @YES, AVLinearPCMIsNonInterleaved: @NO,
                                    AVLinearPCMIsBigEndianKey: @NO, AVSampleRateKey: @(rate), AVNumberOfChannelsKey: @(channels) };
        AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack: track outputSettings: settings];
        [reader addOutput: output];
        if (! [reader startReading])
        {
            [reader release];
            return extractAudioWithJuce (movie, wav, sampleRate, error);
        }

        wav.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (wav.createOutputStream());
        juce::WavAudioFormat format;
        auto writer = stream != nullptr ? format.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (channels)
                                                                      .withBitsPerSample (32)
                                                                      .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint))
                                        : nullptr;
        if (writer == nullptr)
        {
            [reader cancelReading];
            [reader release];
            error = "Couldn't write " + wav.getFileName();
            return false;
        }

        std::vector<float> interleaved;
        juce::AudioBuffer<float> block;
        juce::int64 written = 0;
        while (CMSampleBufferRef sample = [output copyNextSampleBuffer])
        {
            if (CMBlockBufferRef data = CMSampleBufferGetDataBuffer (sample))
            {
                const size_t bytes = CMBlockBufferGetDataLength (data);
                const int frames = (int) (bytes / sizeof (float) / (size_t) channels);
                interleaved.resize ((size_t) frames * (size_t) channels);
                if (frames > 0 && CMBlockBufferCopyDataBytes (data, 0, (size_t) frames * (size_t) channels * sizeof (float), interleaved.data()) == kCMBlockBufferNoErr)
                {
                    block.setSize (channels, frames, false, false, true);
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        auto* dest = block.getWritePointer (ch);
                        for (int i = 0; i < frames; ++i)
                            dest[i] = interleaved[(size_t) (i * channels + ch)];
                    }
                    writer->writeFromAudioSampleBuffer (block, 0, frames);
                    written += frames;
                }
            }
            CFRelease (sample);
        }

        const bool completed = reader.status == AVAssetReaderStatusCompleted;
        [reader release];
        writer.reset();
        if (! completed || written == 0)
        {
            wav.deleteFile();
            return extractAudioWithJuce (movie, wav, sampleRate, error);
        }
        return true;
    }
}

} // namespace woodshed::video

#pragma clang diagnostic pop
