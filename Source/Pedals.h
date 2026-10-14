#pragma once

#include <juce_dsp/juce_dsp.h>

// Each pedal processes a mono block in place. The chain owns bypass crossfades,
// so a pedal only needs to know how to sound when it is on.

namespace woodshed
{

//==============================================================================
class Gate
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        gate.prepare ({ sampleRate, (juce::uint32) maxBlock, 1 });
        gate.setRatio (12.0f);
        gate.setAttack (0.5f);
    }

    void reset() { gate.reset(); }

    void setParams (float thresholdDb, float releaseMs)
    {
        gate.setThreshold (thresholdDb);
        gate.setRelease (releaseMs);
    }

    void process (float* data, int numSamples)
    {
        float* channels[] { data };
        juce::dsp::AudioBlock<float> block (channels, 1, (size_t) numSamples);
        gate.process (juce::dsp::ProcessContextReplacing<float> (block));
    }

private:
    juce::dsp::NoiseGate<float> gate;
};

//==============================================================================
// Overdrive: tightened low end, asymmetric soft clipping at 4x oversampling,
// then a tone control and output level.
class Drive
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        oversampling.initProcessing ((size_t) maxBlock);
        preHighPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass (sr, 140.0f);
        dcBlock.coefficients     = juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass (sr, 12.0f);
        gain.reset (sr, 0.03);
        level.reset (sr, 0.03);
        tone.reset (sr, 0.03);
        reset();
    }

    void reset()
    {
        oversampling.reset();
        preHighPass.reset();
        dcBlock.reset();
        toneState = 0.0f;
    }

    int getLatencySamples() const { return (int) std::ceil (oversampling.getLatencyInSamples()); }

    // drive 0..1, toneAmount 0..1, levelDb
    void setParams (float drive, float toneAmount, float levelDb)
    {
        gain.setTargetValue (juce::Decibels::decibelsToGain (juce::jmap (drive, 3.0f, 45.0f)));
        tone.setTargetValue (700.0f * std::pow (12.0f, toneAmount));   // ~700 Hz .. 8.4 kHz
        level.setTargetValue (juce::Decibels::decibelsToGain (levelDb));
    }

    void process (float* data, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
            data[i] = preHighPass.processSample (data[i]) * gain.getNextValue();

        float* channels[] { data };
        juce::dsp::AudioBlock<float> block (channels, 1, (size_t) numSamples);
        auto up = oversampling.processSamplesUp (block);
        auto* x = up.getChannelPointer (0);

        constexpr float bias = 0.18f;
        const float biasOffset = std::tanh (bias);

        for (size_t i = 0; i < up.getNumSamples(); ++i)
            x[i] = std::tanh (x[i] + bias) - biasOffset;

        oversampling.processSamplesDown (block);

        for (int i = 0; i < numSamples; ++i)
        {
            const float cutoff = tone.getNextValue();
            const float a = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * cutoff / (float) sr);
            toneState += a * (dcBlock.processSample (data[i]) - toneState);
            data[i] = toneState * level.getNextValue() * 0.6f;
        }
    }

private:
    double sr = 44100.0;
    juce::dsp::Oversampling<float> oversampling { 1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
    juce::dsp::IIR::Filter<float> preHighPass, dcBlock;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gain { 1.0f }, tone { 4000.0f };
    juce::SmoothedValue<float> level { 1.0f };
    float toneState = 0.0f;
};

//==============================================================================
// Analog-flavoured delay: repeats darken as they feed back.
class Delay
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        line.prepare ({ sampleRate, (juce::uint32) maxBlock, 1 });
        line.setMaximumDelayInSamples ((int) (sampleRate * 2.1));
        timeSamples.reset (sampleRate, 0.12);
        feedback.reset (sampleRate, 0.03);
        mix.reset (sampleRate, 0.03);
        damping = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 3800.0f / (float) sampleRate);
        reset();
    }

    void reset()
    {
        line.reset();
        lowPassState = 0.0f;
    }

    void setParams (float timeMs, float feedbackAmount, float mixAmount)
    {
        timeSamples.setTargetValue ((float) (timeMs * 0.001 * sr));
        feedback.setTargetValue (feedbackAmount);
        mix.setTargetValue (mixAmount);
    }

    void process (float* data, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float dry = data[i];
            const float wet = line.popSample (0, timeSamples.getNextValue());
            lowPassState += damping * (wet - lowPassState);
            line.pushSample (0, dry + feedback.getNextValue() * lowPassState);
            data[i] = dry + mix.getNextValue() * wet;
        }
    }

private:
    double sr = 44100.0;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> line { 1 };
    juce::SmoothedValue<float> timeSamples, feedback, mix;
    float damping = 0.3f, lowPassState = 0.0f;
};

//==============================================================================
// Stereo room reverb (the one pedal that takes mono in and gives stereo out).
class Room
{
public:
    void prepare (double sampleRate) { reverb.setSampleRate (sampleRate); reset(); }
    void reset()                     { reverb.reset(); }

    void setParams (float size, float damping, float mixAmount)
    {
        juce::Reverb::Parameters p;
        p.roomSize = size;
        p.damping  = damping;
        p.wetLevel = mixAmount * 0.75f;
        p.dryLevel = 1.0f - 0.35f * mixAmount;
        p.width    = 1.0f;
        reverb.setParameters (p);
    }

    void process (float* left, float* right, int numSamples) { reverb.processStereo (left, right, numSamples); }

private:
    juce::Reverb reverb;
};

} // namespace woodshed
