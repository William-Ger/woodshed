#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>

using APVTS = juce::AudioProcessorValueTreeState;

// Transparent below -2 dBFS, then a soft knee into full scale instead of a hard wall.
static float safetyClip (float x)
{
    constexpr float knee = 0.8f, room = 1.0f - knee;
    const float mag = std::abs (x);
    return mag <= knee ? x : std::copysign (knee + room * std::tanh ((mag - knee) / room), x);
}

WoodshedProcessor::WoodshedProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "Woodshed", createLayout())
{
    for (int i = 0; i < pedalIds.size(); ++i)
        bypass[(size_t) i].onParam = params.getRawParameterValue (pedalIds[i] + "_on");
}

APVTS::ParameterLayout WoodshedProcessor::createLayout()
{
    using namespace juce;
    APVTS::ParameterLayout layout;

    auto db = [] (const char* id, const char* name, float lo, float hi, float def)
    {
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (lo, hi, 0.1f), def,
                                                      AudioParameterFloatAttributes().withLabel ("dB"));
    };
    auto unit = [] (const char* id, const char* name, float def)
    {
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 1.0f), def);
    };
    auto onOff = [] (const char* id, const char* name, bool def)
    {
        return std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def);
    };

    // Scarlett Solo: input 1 is the XLR mic jack, input 2 is the 1/4" instrument jack.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "input", 1 }, "Input", StringArray { "Input 1", "Input 2" }, 1));
    layout.add (db ("input_gain", "Input Gain", -12.0f, 24.0f, 0.0f));

    layout.add (onOff ("gate_on", "Gate On", true));
    layout.add (db ("gate_threshold", "Gate Threshold", -90.0f, -20.0f, -62.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "gate_release", 1 }, "Gate Release",
                                                       NormalisableRange<float> (10.0f, 500.0f, 1.0f, 0.5f), 90.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms")));

    layout.add (onOff ("drive_on", "Drive On", true));
    layout.add (unit ("drive_gain", "Drive", 0.45f));
    layout.add (unit ("drive_tone", "Tone", 0.55f));
    layout.add (db ("drive_level", "Level", -24.0f, 12.0f, 0.0f));

    layout.add (onOff ("delay_on", "Delay On", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "delay_time", 1 }, "Delay Time",
                                                       NormalisableRange<float> (40.0f, 1200.0f, 1.0f, 0.6f), 380.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "delay_feedback", 1 }, "Feedback",
                                                       NormalisableRange<float> (0.0f, 0.92f), 0.35f));
    layout.add (unit ("delay_mix", "Delay Mix", 0.3f));

    layout.add (onOff ("room_on", "Reverb On", true));
    layout.add (unit ("room_size", "Size", 0.55f));
    layout.add (unit ("room_damp", "Damping", 0.45f));
    layout.add (unit ("room_mix", "Reverb Mix", 0.22f));

    layout.add (db ("master", "Master", -48.0f, 12.0f, 0.0f));
    return layout;
}

bool WoodshedProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    const auto monoOrStereo = [] (const juce::AudioChannelSet& s)
    {
        return s == juce::AudioChannelSet::mono() || s == juce::AudioChannelSet::stereo();
    };
    return monoOrStereo (in) && monoOrStereo (out);
}

void WoodshedProcessor::ensureCapacity (int numSamples)
{
    if (mono.getNumSamples() >= numSamples)
        return;

    mono.setSize (1, numSamples, false, true, true);
    scratch.setSize (1, numSamples, false, true, true);
    stereo.setSize (2, numSamples, false, true, true);
}

void WoodshedProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    mono.setSize (0, 0);
    ensureCapacity (juce::jmax (samplesPerBlock, 32));

    const int maxBlock = mono.getNumSamples();
    gate.prepare (sampleRate, maxBlock);
    drive.prepare (sampleRate, maxBlock);
    delay.prepare (sampleRate, maxBlock);
    room.prepare (sampleRate);

    for (auto& b : bypass)
    {
        b.amount.reset (sampleRate, 0.015);
        b.amount.setCurrentAndTargetValue (b.onParam->load() > 0.5f ? 1.0f : 0.0f);
    }

    inputGain.reset (sampleRate, 0.03);
    masterGain.reset (sampleRate, 0.03);

    captureState.store (CaptureState::idle);
    captureBuffer.setSize (2, (int) (maxCaptureSeconds * sampleRate));
    setLatencySamples (drive.getLatencySamples());
}

void WoodshedProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    ensureCapacity (n);

    // 1. Pick the guitar channel into a mono working buffer.
    const int source = juce::jmin ((int) param ("input"), juce::jmax (numIn - 1, 0));
    auto* x = mono.getWritePointer (0);

    if (numIn > 0)
        juce::FloatVectorOperations::copy (x, buffer.getReadPointer (source), n);
    else
        juce::FloatVectorOperations::clear (x, n);

    inputGain.setTargetValue (juce::Decibels::decibelsToGain (param ("input_gain")));
    inputGain.applyGain (x, n);

    float inPeak = 0.0f;
    for (int i = 0; i < n; ++i)
        inPeak = juce::jmax (inPeak, std::abs (x[i]));
    inputPeak.store (juce::jmax (inputPeak.load(), inPeak));

    const bool capturing = captureState.load() == CaptureState::recording;
    const int captureStart = capturePos;
    if (capturing)
        captureBuffer.copyFrom (0, captureStart, x, juce::jmin (n, captureLength - captureStart));

    // 2. Mono pedals, each crossfaded against its own input when switched.
    gate.setParams (param ("gate_threshold"), param ("gate_release"));
    drive.setParams (param ("drive_gain"), param ("drive_tone"), param ("drive_level"));
    delay.setParams (param ("delay_time"), param ("delay_feedback"), param ("delay_mix"));
    room.setParams (param ("room_size"), param ("room_damp"), param ("room_mix"));

    auto runMono = [&] (size_t index, auto&& processFn)
    {
        auto& b = bypass[index];
        b.amount.setTargetValue (b.onParam->load() > 0.5f ? 1.0f : 0.0f);

        if (! b.amount.isSmoothing() && b.amount.getCurrentValue() < 0.5f)
            return;

        auto* wet = scratch.getWritePointer (0);
        juce::FloatVectorOperations::copy (wet, x, n);
        processFn (wet);

        for (int i = 0; i < n; ++i)
        {
            const float a = b.amount.getNextValue();
            x[i] += a * (wet[i] - x[i]);
        }
    };

    runMono (0, [&] (float* d) { gate.process (d, n); });
    runMono (1, [&] (float* d) { drive.process (d, n); });
    runMono (2, [&] (float* d) { delay.process (d, n); });

    // 3. Reverb goes mono -> stereo.
    auto* left = stereo.getWritePointer (0);
    auto* right = stereo.getWritePointer (1);
    juce::FloatVectorOperations::copy (left, x, n);
    juce::FloatVectorOperations::copy (right, x, n);

    {
        auto& b = bypass[3];
        b.amount.setTargetValue (b.onParam->load() > 0.5f ? 1.0f : 0.0f);

        if (b.amount.isSmoothing() || b.amount.getCurrentValue() > 0.5f)
        {
            room.process (left, right, n);

            for (int i = 0; i < n; ++i)
            {
                const float a = b.amount.getNextValue();
                left[i]  = x[i] + a * (left[i] - x[i]);
                right[i] = x[i] + a * (right[i] - x[i]);
            }
        }
    }

    // 4. Master and output.
    masterGain.setTargetValue (juce::Decibels::decibelsToGain (param ("master")));
    float outPeak = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float g = masterGain.getNextValue();
        left[i] = safetyClip (left[i] * g);
        right[i] = safetyClip (right[i] * g);
        outPeak = juce::jmax (outPeak, std::abs (left[i]), std::abs (right[i]));
    }

    outputPeak.store (juce::jmax (outputPeak.load(), outPeak));

    if (numOut >= 2)
    {
        buffer.copyFrom (0, 0, left, n);
        buffer.copyFrom (1, 0, right, n);
    }
    else if (numOut == 1)
    {
        buffer.copyFrom (0, 0, left, n);
        buffer.addFrom (0, 0, right, n);
        buffer.applyGain (0, 0, n, 0.5f);
    }

    for (int ch = 2; ch < numOut; ++ch)
        buffer.clear (ch, 0, n);

    if (capturing)
    {
        const int count = juce::jmin (n, captureLength - captureStart);
        captureBuffer.copyFrom (1, captureStart, left, count);
        capturePos = captureStart + count;

        if (capturePos >= captureLength)
            captureState.store (CaptureState::done);
    }
}

//==============================================================================
void WoodshedProcessor::armCapture (double seconds)
{
    if (captureState.load() == CaptureState::recording)
        return;

    captureState.store (CaptureState::idle);
    captureLength = juce::jmin ((int) (seconds * currentSampleRate), captureBuffer.getNumSamples());
    captureBuffer.clear();
    capturePos = 0;
    captureState.store (CaptureState::recording);
}

bool WoodshedProcessor::writeCapture (const juce::File& file)
{
    if (captureState.load() != CaptureState::done)
        return false;

    file.getParentDirectory().createDirectory();
    file.deleteFile();

    bool ok = false;
    if (auto stream = std::unique_ptr<juce::OutputStream> (file.createOutputStream()))
    {
        juce::WavAudioFormat wav;
        const auto options = juce::AudioFormatWriterOptions{}.withSampleRate (currentSampleRate)
                                                             .withNumChannels (2)
                                                             .withBitsPerSample (24);
        if (auto writer = wav.createWriterFor (stream, options))
            ok = writer->writeFromAudioSampleBuffer (captureBuffer, 0, captureLength);
    }

    captureState.store (CaptureState::idle);
    return ok;
}

//==============================================================================
void WoodshedProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = params.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void WoodshedProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (params.state.getType()))
            params.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* WoodshedProcessor::createEditor() { return new WoodshedEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new WoodshedProcessor(); }
