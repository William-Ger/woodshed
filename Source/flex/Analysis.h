#pragma once

#include "Flex.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <map>
#include <memory>
#include <mutex>

namespace woodshed::flex
{

/**
    What Time Stretch, Pitch Edit and Tempo Detect need to know about a recording: where its
    transients are, how strongly something starts at each moment, its tempo and beats, and its
    pitch over time. Pure functions of the audio, so they give the same answer every time.
*/

/** Spectral flux, one value per `hop` samples (frame i is centred on sample i * hop). */
struct OnsetCurve
{
    int hop = 256;
    std::vector<float> flux;            // all frequencies
    std::vector<float> low;             // below about 200 Hz (kick drums, bass): finds downbeats
};

OnsetCurve onsetCurve (const float* audio, int numSamples, double sampleRate);

/** Transients (sample positions where a note or hit starts), in order. */
std::vector<double> detectTransients (const float* audio, int numSamples, double sampleRate);
std::vector<double> detectTransients (const OnsetCurve&, const float* audio, int numSamples, double sampleRate);

/** The tempo of a recording: its beats (sample positions), where the bars start, and the bpm. */
struct TempoEstimate
{
    double bpm = 0.0;                   // 0 when there's no steady pulse
    double confidence = 0.0;            // 0..1
    std::vector<double> beats;          // in order
    int firstDownbeat = 0;              // index into beats of the first bar's downbeat
    bool variable = false;              // the tempo drifts: see barTempos
    std::vector<double> barTempos;      // bpm of each bar from the first downbeat on
};

TempoEstimate detectTempo (const float* audio, int numSamples, double sampleRate, int beatsPerBar = 4);

/** The pitch over time, one frame every `hop` samples (frame i analyses the window centred on
    sample i * hop). Unvoiced frames have pitch 0. */
struct PitchTrack
{
    int hop = 256;
    std::vector<float> pitch;           // MIDI note number (fractional), 0 where unvoiced
    std::vector<float> level;           // RMS

    /** The pitch at a sample (0 where unvoiced), interpolated between frames. */
    float at (double sample) const;
    /** The slow movement of the pitch (about 150 ms average) at a sample, voiced frames only. */
    float trendAt (double sample) const;
    std::vector<float> trend;           // filled by trackPitch
};

PitchTrack trackPitch (const float* audio, int numSamples, double sampleRate);

/** Notes in a monophonic recording (a voice, a solo instrument), with nothing edited yet. */
std::vector<PitchNote> detectNotes (const PitchTrack&, double sampleRate);

/** Pitch-edit targets: the semitones each note's samples move by, for a file sample. */
float transposeAt (const std::vector<PitchNote>&, const PitchTrack&, double sample, double sampleRate);
float formantAt (const std::vector<PitchNote>&, double sample, double sampleRate);
float gainAt (const std::vector<PitchNote>&, double sample, double sampleRate);

/** Analyses shared by every region that plays a file, made once per file. Thread-safe. */
class AnalysisCache
{
public:
    using Audio = std::shared_ptr<const juce::AudioBuffer<float>>;

    std::shared_ptr<const std::vector<double>> transients (const juce::String& key, const Audio&, double sampleRate);
    std::shared_ptr<const PitchTrack> pitch (const juce::String& key, const Audio&, double sampleRate);
    std::shared_ptr<const TempoEstimate> tempo (const juce::String& key, const Audio&, double sampleRate, int beatsPerBar);
    void clear();

private:
    std::mutex lock;
    std::map<juce::String, std::shared_ptr<const std::vector<double>>> transientMap;
    std::map<juce::String, std::shared_ptr<const PitchTrack>> pitchMap;
    std::map<juce::String, std::shared_ptr<const TempoEstimate>> tempoMap;
};

} // namespace woodshed::flex
