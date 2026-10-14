#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>

using APVTS = juce::AudioProcessorValueTreeState;
using woodshed::Registry;

namespace
{
const juce::Identifier boardId ("Board"), slotsId ("slots");

// Transparent below -2 dBFS, then a soft knee into full scale instead of a hard wall.
float safetyClip (float x)
{
    constexpr float knee = 0.8f, room = 1.0f - knee;
    const float mag = std::abs (x);
    return mag <= knee ? x : std::copysign (knee + room * std::tanh ((mag - knee) / room), x);
}

const juce::StringArray defaultBoard { "dyn.gate", "drive.overdrive", "amp.head", "cab.speaker", "delay.analog", "reverb.room" };
}

WoodshedProcessor::WoodshedProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "Woodshed", createLayout())
{
    params.state.addListener (this);

    loadDefaultBoard();
    startTimerHz (10);
}

void WoodshedProcessor::loadDefaultBoard()
{
    setBoard (defaultBoard);
    params.getParameter (onParamId (defaultBoard.indexOf ("delay.analog")))->setValueNotifyingHost (0.0f);
}

WoodshedProcessor::~WoodshedProcessor()
{
    stopTimer();
    params.state.removeListener (this);
    freeRetiredChains();
    delete pending.exchange (nullptr);
    delete current;
}

APVTS::ParameterLayout WoodshedProcessor::createLayout()
{
    using namespace juce;
    APVTS::ParameterLayout layout;

    // Scarlett Solo: input 1 is the XLR mic jack, input 2 is the 1/4" instrument jack.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "input", 1 }, "Input", StringArray { "Input 1", "Input 2" }, 1));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "input_gain", 1 }, "Input Gain",
                                                       NormalisableRange<float> (-12.0f, 24.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "master", 1 }, "Master",
                                                       NormalisableRange<float> (-48.0f, 12.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));

    for (int s = 0; s < maxSlots; ++s)
    {
        const auto prefix = "Slot " + String (s + 1) + " ";
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { onParamId (s), 1 }, prefix + "On", true));

        for (int k = 0; k < woodshed::Pedal::maxKnobs; ++k)
            layout.add (std::make_unique<AudioParameterFloat> (ParameterID { knobParamId (s, k), 1 }, prefix + "Knob " + String (k + 1),
                                                               NormalisableRange<float> (0.0f, 1.0f), 0.5f));
    }

    return layout;
}

bool WoodshedProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto monoOrStereo = [] (const juce::AudioChannelSet& s)
    {
        return s == juce::AudioChannelSet::mono() || s == juce::AudioChannelSet::stereo();
    };
    return monoOrStereo (layouts.getMainInputChannelSet()) && monoOrStereo (layouts.getMainOutputChannelSet());
}

//==============================================================================
juce::ValueTree WoodshedProcessor::boardTree()
{
    return params.state.getOrCreateChildWithName (boardId, nullptr);
}

juce::StringArray WoodshedProcessor::getBoard() const
{
    auto tree = params.state.getChildWithName (boardId);
    auto types = juce::StringArray::fromTokens (tree.getProperty (slotsId).toString(), "|", "");
    types.removeEmptyStrings();
    return types;
}

void WoodshedProcessor::writeBoard (const juce::StringArray& types)
{
    const auto joined = types.joinIntoString ("|");
    auto tree = boardTree();

    if (tree.getProperty (slotsId).toString() == joined)
        rebuildChain();   // same list of types, but instances may have moved
    else
        tree.setProperty (slotsId, joined, nullptr);
}

void WoodshedProcessor::copySlotValues (int from, int to)
{
    auto copy = [this] (const juce::String& src, const juce::String& dst)
    {
        params.getParameter (dst)->setValueNotifyingHost (params.getParameter (src)->getValue());
    };

    copy (onParamId (from), onParamId (to));
    for (int k = 0; k < woodshed::Pedal::maxKnobs; ++k)
        copy (knobParamId (from, k), knobParamId (to, k));
}

void WoodshedProcessor::resetSlotValues (int slot, const juce::String& type)
{
    const auto* t = Registry::get().find (type);
    params.getParameter (onParamId (slot))->setValueNotifyingHost (1.0f);

    for (int k = 0; k < woodshed::Pedal::maxKnobs; ++k)
    {
        const float norm = (t != nullptr && k < (int) t->info.knobs.size()) ? t->info.knobs[(size_t) k].defaultNorm() : 0.5f;
        params.getParameter (knobParamId (slot, k))->setValueNotifyingHost (norm);
    }
}

void WoodshedProcessor::insertPedal (const juce::String& type, int position)
{
    auto board = getBoard();
    if (board.size() >= maxSlots || Registry::get().find (type) == nullptr)
        return;

    position = juce::jlimit (0, board.size(), position);

    for (int s = board.size(); s > position; --s)
    {
        copySlotValues (s - 1, s);
        instances[(size_t) s] = std::move (instances[(size_t) s - 1]);
        instanceTypes[(size_t) s] = instanceTypes[(size_t) s - 1];
    }

    instances[(size_t) position].reset();
    instanceTypes[(size_t) position] = {};
    resetSlotValues (position, type);

    board.insert (position, type);
    writeBoard (board);
}

void WoodshedProcessor::removePedal (int position)
{
    auto board = getBoard();
    if (! juce::isPositiveAndBelow (position, board.size()))
        return;

    for (int s = position; s < board.size() - 1; ++s)
    {
        copySlotValues (s + 1, s);
        instances[(size_t) s] = std::move (instances[(size_t) s + 1]);
        instanceTypes[(size_t) s] = instanceTypes[(size_t) s + 1];
    }

    const int last = board.size() - 1;
    instances[(size_t) last].reset();
    instanceTypes[(size_t) last] = {};

    board.remove (position);
    writeBoard (board);
}

void WoodshedProcessor::movePedal (int from, int to)
{
    auto board = getBoard();
    to = juce::jlimit (0, board.size() - 1, to);

    if (! juce::isPositiveAndBelow (from, board.size()) || from == to)
        return;

    std::array<float, woodshed::Pedal::maxKnobs + 1> saved {};
    saved[0] = params.getParameter (onParamId (from))->getValue();
    for (int k = 0; k < woodshed::Pedal::maxKnobs; ++k)
        saved[(size_t) k + 1] = params.getParameter (knobParamId (from, k))->getValue();

    auto instance = std::move (instances[(size_t) from]);
    auto instanceType = instanceTypes[(size_t) from];
    const int step = to > from ? 1 : -1;

    for (int s = from; s != to; s += step)
    {
        copySlotValues (s + step, s);
        instances[(size_t) s] = std::move (instances[(size_t) (s + step)]);
        instanceTypes[(size_t) s] = instanceTypes[(size_t) (s + step)];
    }

    params.getParameter (onParamId (to))->setValueNotifyingHost (saved[0]);
    for (int k = 0; k < woodshed::Pedal::maxKnobs; ++k)
        params.getParameter (knobParamId (to, k))->setValueNotifyingHost (saved[(size_t) k + 1]);

    instances[(size_t) to] = std::move (instance);
    instanceTypes[(size_t) to] = instanceType;

    board.move (from, to);
    writeBoard (board);
}

void WoodshedProcessor::setBoard (const juce::StringArray& types)
{
    for (int s = 0; s < maxSlots; ++s)
    {
        instances[(size_t) s].reset();
        instanceTypes[(size_t) s] = {};
        resetSlotValues (s, s < types.size() ? types[s] : juce::String());
    }

    writeBoard (types);
}

void WoodshedProcessor::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType (boardId) && property == slotsId)
        rebuildChain();
}

void WoodshedProcessor::rebuildChain()
{
    const auto types = getBoard();
    auto chain = std::make_unique<woodshed::Chain>();

    for (int s = 0; s < maxSlots; ++s)
    {
        const auto type = s < types.size() ? types[s] : juce::String();
        const auto* t = type.isNotEmpty() ? Registry::get().find (type) : nullptr;

        if (t == nullptr)
        {
            instances[(size_t) s].reset();
            instanceTypes[(size_t) s] = {};
            continue;
        }

        if (instances[(size_t) s] == nullptr || instanceTypes[(size_t) s] != type)
        {
            const bool isOn = params.getRawParameterValue (onParamId (s))->load() > 0.5f;
            instances[(size_t) s] = t->create();
            instances[(size_t) s]->prepare (currentSampleRate, preparedBlockSize);
            instances[(size_t) s]->engaged.reset (currentSampleRate, 0.015);
            instances[(size_t) s]->engaged.setCurrentAndTargetValue (isOn ? 1.0f : 0.0f);
            instanceTypes[(size_t) s] = type;
        }

        std::array<const std::atomic<float>*, woodshed::Pedal::maxKnobs> knobs {};
        for (int k = 0; k < woodshed::Pedal::maxKnobs; ++k)
            knobs[(size_t) k] = params.getRawParameterValue (knobParamId (s, k));

        chain->entries.push_back (woodshed::Chain::makeEntry (instances[(size_t) s], t->info,
                                                              params.getRawParameterValue (onParamId (s)), knobs.data()));
        chain->latencySamples += instances[(size_t) s]->getLatencySamples();
    }

    const int latency = chain->latencySamples;
    delete pending.exchange (chain.release());
    setLatencySamples (latency);
    sendChangeMessage();
}

void WoodshedProcessor::freeRetiredChains()
{
    const auto scope = retireFifo.read (retireFifo.getNumReady());
    for (int i = 0; i < scope.blockSize1; ++i) delete retired[(size_t) (scope.startIndex1 + i)];
    for (int i = 0; i < scope.blockSize2; ++i) delete retired[(size_t) (scope.startIndex2 + i)];
}

void WoodshedProcessor::timerCallback()
{
    freeRetiredChains();
}

//==============================================================================
void WoodshedProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    preparedBlockSize = juce::jmax (samplesPerBlock, 64);

    work.setSize (2, preparedBlockSize);
    scratch.setSize (2, preparedBlockSize);

    for (int s = 0; s < maxSlots; ++s)
    {
        if (auto& p = instances[(size_t) s])
        {
            p->prepare (sampleRate, preparedBlockSize);
            p->engaged.reset (sampleRate, 0.015);
            p->engaged.setCurrentAndTargetValue (params.getRawParameterValue (onParamId (s))->load() > 0.5f ? 1.0f : 0.0f);
        }
    }

    inputGain.reset (sampleRate, 0.03);
    masterGain.reset (sampleRate, 0.03);

    captureState.store (CaptureState::idle);
    captureBuffer.setSize (2, (int) (maxCaptureSeconds * sampleRate));
}

void WoodshedProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Pick up a newly published chain; the old one goes back to the message thread to be freed.
    if (retireFifo.getFreeSpace() > 0)
    {
        if (auto* next = pending.exchange (nullptr))
        {
            if (current != nullptr)
            {
                const auto scope = retireFifo.write (1);
                retired[(size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)] = current;
            }

            current = next;
        }
    }

    const int total = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int source = juce::jmin ((int) param ("input"), juce::jmax (numIn - 1, 0));

    inputGain.setTargetValue (juce::Decibels::decibelsToGain (param ("input_gain")));
    masterGain.setTargetValue (juce::Decibels::decibelsToGain (param ("master")));

    float inPeak = 0.0f, outPeak = 0.0f;

    // Hosts may hand us more samples than they promised; never exceed what pedals prepared for.
    for (int offset = 0; offset < total; offset += preparedBlockSize)
    {
        const int n = juce::jmin (preparedBlockSize, total - offset);
        auto* left = work.getWritePointer (0);
        auto* right = work.getWritePointer (1);

        if (numIn > 0)
            juce::FloatVectorOperations::copy (left, buffer.getReadPointer (source, offset), n);
        else
            juce::FloatVectorOperations::clear (left, n);

        inputGain.applyGain (left, n);

        for (int i = 0; i < n; ++i)
            inPeak = juce::jmax (inPeak, std::abs (left[i]));

        const bool capturing = captureState.load() == CaptureState::recording;
        const int captureCount = capturing ? juce::jmin (n, captureLength - capturePos) : 0;
        if (capturing)
            captureBuffer.copyFrom (0, capturePos, left, captureCount);

        if (current != nullptr)
            current->process (left, right, scratch.getWritePointer (0), scratch.getWritePointer (1), n);
        else
            juce::FloatVectorOperations::copy (right, left, n);

        for (int i = 0; i < n; ++i)
        {
            const float g = masterGain.getNextValue();
            left[i] = safetyClip (left[i] * g);
            right[i] = safetyClip (right[i] * g);
            outPeak = juce::jmax (outPeak, std::abs (left[i]), std::abs (right[i]));
        }

        if (numOut >= 2)
        {
            buffer.copyFrom (0, offset, left, n);
            buffer.copyFrom (1, offset, right, n);
        }
        else if (numOut == 1)
        {
            buffer.copyFrom (0, offset, left, n, 0.5f);
            buffer.addFrom (0, offset, right, n, 0.5f);
        }

        if (capturing)
        {
            captureBuffer.copyFrom (1, capturePos, left, captureCount);
            capturePos += captureCount;

            if (capturePos >= captureLength)
                captureState.store (CaptureState::done);
        }
    }

    for (int ch = 2; ch < numOut; ++ch)
        buffer.clear (ch, 0, total);

    inputPeak.store (juce::jmax (inputPeak.load(), inPeak));
    outputPeak.store (juce::jmax (outputPeak.load(), outPeak));
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
juce::File WoodshedProcessor::presetFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
          #if JUCE_MAC
           .getChildFile ("Application Support")
          #endif
           .getChildFile ("Woodshed").getChildFile ("Presets");
}

bool WoodshedProcessor::savePreset (const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    if (auto xml = params.copyState().createXml())
        return xml->writeTo (file);
    return false;
}

bool WoodshedProcessor::loadPreset (const juce::File& file)
{
    if (auto xml = juce::XmlDocument::parse (file))
    {
        if (xml->hasTagName (params.state.getType()))
        {
            for (auto& i : instances) i.reset();
            for (auto& t : instanceTypes) t = {};
            params.replaceState (juce::ValueTree::fromXml (*xml));
            return true;
        }
    }

    return false;
}

void WoodshedProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = params.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void WoodshedProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        if (xml->hasTagName (params.state.getType()))
        {
            for (auto& i : instances) i.reset();
            for (auto& t : instanceTypes) t = {};
            params.replaceState (juce::ValueTree::fromXml (*xml));
        }
    }
}

juce::AudioProcessorEditor* WoodshedProcessor::createEditor() { return new WoodshedEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new WoodshedProcessor(); }
