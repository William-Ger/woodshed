#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PresetData.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <numeric>

using APVTS = juce::AudioProcessorValueTreeState;
using woodshed::Registry;

namespace
{
const juce::Identifier boardId ("Board"), slotsId ("slots"), spotsId ("spots");

// Transparent below -2 dBFS, then a soft knee into full scale instead of a hard wall.
float safetyClip (float x)
{
    constexpr float knee = 0.8f, room = 1.0f - knee;
    const float mag = std::abs (x);
    return mag <= knee ? x : std::copysign (knee + room * std::tanh ((mag - knee) / room), x);
}

struct DefaultPedal { const char* type; int spot; bool on; };

// A full board to explore, with only a sensible core switched on.
const DefaultPedal defaultBoard[] {
    { "dyn.gate", 0, true },     { "dyn.compressor", 1, false }, { "drive.overdrive", 2, true },
    { "drive.fuzz", 3, false },  { "filter.wah", 4, false },
    { "mod.chorus", 5, false },  { "mod.phaser", 6, false },     { "delay.analog", 7, false },
    { "delay.tape", 8, false },  { "reverb.room", 9, true },
    { "amp.head", -1, true },    { "cab.speaker", -1, true }
};
}

WoodshedProcessor::WoodshedProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "Woodshed", createLayout())
{
    params.state.addListener (this);
    loadDefaultBoard();
    startTimerHz (30);
}

void WoodshedProcessor::loadDefaultBoard()
{
    for (auto& i : instances) i.reset();
    for (auto& t : instanceTypes) t = {};

    std::vector<Placement> places;
    for (auto& d : defaultBoard)
        places.push_back ({ d.type, d.spot });

    applyPlacements (places, std::vector<int> (places.size(), -1));

    const auto board = getBoard();
    for (auto& d : defaultBoard)
        if (const int slot = board.indexOf (d.type); slot >= 0)
            params.getParameter (onParamId (slot))->setValueNotifyingHost (d.on ? 1.0f : 0.0f);
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

juce::String WoodshedProcessor::rigCategory (const juce::String& type)
{
    if (const auto* t = Registry::get().find (type))
        if (t->info.category == "Amp" || t->info.category == "Cab")
            return t->info.category;

    return {};
}

bool WoodshedProcessor::isRigType (const juce::String& type)
{
    return rigCategory (type).isNotEmpty();
}

std::vector<WoodshedProcessor::Placement> WoodshedProcessor::getPlacements() const
{
    const auto types = getBoard();
    const auto spotTokens = juce::StringArray::fromTokens (params.state.getChildWithName (boardId).getProperty (spotsId).toString(), "|", "");

    std::vector<Placement> places;
    std::array<bool, boardSpots> used {};

    // First pass: keep every valid, unique stored spot.
    for (int i = 0; i < types.size(); ++i)
    {
        Placement p { types[i], -1 };

        if (! isRigType (p.type) && i < spotTokens.size() && spotTokens[i].containsOnly ("0123456789"))
        {
            const int spot = spotTokens[i].getIntValue();
            if (juce::isPositiveAndBelow (spot, boardSpots) && ! used[(size_t) spot])
            {
                p.spot = spot;
                used[(size_t) spot] = true;
            }
        }

        places.push_back (p);
    }

    // Second pass: older presets had no spots; pack those pedals into the free spots in order.
    for (auto& p : places)
    {
        if (isRigType (p.type) || p.spot >= 0)
            continue;

        for (int s = 0; s < boardSpots; ++s)
        {
            if (! used[(size_t) s])
            {
                p.spot = s;
                used[(size_t) s] = true;
                break;
            }
        }
    }

    return places;
}

std::vector<int> WoodshedProcessor::getSpots() const
{
    std::vector<int> spots;
    for (auto& p : getPlacements())
        spots.push_back (p.spot);
    return spots;
}

int WoodshedProcessor::findFreeSpot() const
{
    std::array<bool, boardSpots> used {};
    for (auto& p : getPlacements())
        if (p.spot >= 0) used[(size_t) p.spot] = true;

    for (int s = 0; s < boardSpots; ++s)
        if (! used[(size_t) s])
            return s;

    return -1;
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

bool WoodshedProcessor::addPedal (const juce::String& type, int spot)
{
    if (Registry::get().find (type) == nullptr)
        return false;

    auto places = getPlacements();
    std::vector<int> sources;
    for (int i = 0; i < (int) places.size(); ++i)
        sources.push_back (i);

    if (const auto category = rigCategory (type); category.isNotEmpty())
    {
        // One amp and one cab: a new one replaces the old.
        for (int i = (int) places.size(); --i >= 0;)
        {
            if (rigCategory (places[(size_t) i].type) == category)
            {
                places.erase (places.begin() + i);
                sources.erase (sources.begin() + i);
            }
        }

        spot = -1;
    }
    else
    {
        if (spot < 0)
            spot = findFreeSpot();

        const bool taken = std::any_of (places.begin(), places.end(), [spot] (const Placement& p) { return p.spot == spot; });
        if (! juce::isPositiveAndBelow (spot, boardSpots) || taken)
            return false;
    }

    if ((int) places.size() >= maxSlots)
        return false;

    places.push_back ({ type, spot });
    sources.push_back (-1);
    applyPlacements (places, sources);
    return true;
}

void WoodshedProcessor::removePedal (int slot)
{
    auto places = getPlacements();
    if (! juce::isPositiveAndBelow (slot, (int) places.size()))
        return;

    std::vector<int> sources;
    for (int i = 0; i < (int) places.size(); ++i)
        if (i != slot)
            sources.push_back (i);

    places.erase (places.begin() + slot);
    applyPlacements (places, sources);
}

void WoodshedProcessor::movePedal (int slot, int spot)
{
    auto places = getPlacements();
    if (! juce::isPositiveAndBelow (slot, (int) places.size()) || ! juce::isPositiveAndBelow (spot, boardSpots)
        || places[(size_t) slot].spot < 0 || places[(size_t) slot].spot == spot)
        return;

    // Dropping onto an occupied spot swaps the two pedals.
    for (auto& p : places)
        if (p.spot == spot)
            p.spot = places[(size_t) slot].spot;

    places[(size_t) slot].spot = spot;

    std::vector<int> sources;
    for (int i = 0; i < (int) places.size(); ++i)
        sources.push_back (i);

    applyPlacements (places, sources);
}

void WoodshedProcessor::holdAllKnobs()
{
    for (auto& i : instances)
        if (i != nullptr)
            i->knobsHeld = true;

    releaseHoldAtGeneration = -1;   // set again once the new chain is published
}

void WoodshedProcessor::applyPlacements (std::vector<Placement> places, std::vector<int> sources)
{
    jassert (places.size() == sources.size() && (int) places.size() <= maxSlots);

    // Chain order: board pedals by spot, then the amp, then the cab.
    std::vector<int> order ((size_t) places.size());
    std::iota (order.begin(), order.end(), 0);

    auto key = [&places] (int i)
    {
        const auto& p = places[(size_t) i];
        const auto category = rigCategory (p.type);
        return category == "Amp" ? 1000 : category == "Cab" ? 1001 : p.spot;
    };

    std::stable_sort (order.begin(), order.end(), [&key] (int a, int b) { return key (a) < key (b); });

    // Freeze every pedal's knobs while values move between slots underneath the running chain.
    holdAllKnobs();

    std::array<std::array<float, woodshed::Pedal::maxKnobs + 1>, maxSlots> saved {};
    for (int s = 0; s < maxSlots; ++s)
    {
        saved[(size_t) s][0] = params.getParameter (onParamId (s))->getValue();
        for (int k = 0; k < woodshed::Pedal::maxKnobs; ++k)
            saved[(size_t) s][(size_t) k + 1] = params.getParameter (knobParamId (s, k))->getValue();
    }

    const auto oldInstances = instances;
    const auto oldTypes = instanceTypes;

    auto write = [this] (const juce::String& id, float value)
    {
        auto* p = params.getParameter (id);
        if (! juce::exactlyEqual (p->getValue(), value))
            p->setValueNotifyingHost (value);
    };

    juce::StringArray types, spots;

    for (int slot = 0; slot < maxSlots; ++slot)
    {
        if (slot >= (int) order.size())
        {
            instances[(size_t) slot].reset();
            instanceTypes[(size_t) slot] = {};
            continue;
        }

        const auto& p = places[(size_t) order[(size_t) slot]];
        const int from = sources[(size_t) order[(size_t) slot]];
        types.add (p.type);
        spots.add (juce::String (p.spot));

        if (from < 0)
        {
            resetSlotValues (slot, p.type);
            instances[(size_t) slot].reset();
            instanceTypes[(size_t) slot] = {};
        }
        else
        {
            write (onParamId (slot), saved[(size_t) from][0]);
            for (int k = 0; k < woodshed::Pedal::maxKnobs; ++k)
                write (knobParamId (slot, k), saved[(size_t) from][(size_t) k + 1]);

            instances[(size_t) slot] = oldInstances[(size_t) from];
            instanceTypes[(size_t) slot] = oldTypes[(size_t) from];
        }
    }

    auto tree = boardTree();
    const auto joinedTypes = types.joinIntoString ("|");
    const bool typesChanged = tree.getProperty (slotsId).toString() != joinedTypes;

    tree.setProperty (spotsId, spots.joinIntoString ("|"), nullptr);

    if (typesChanged)
        tree.setProperty (slotsId, joinedTypes, nullptr);   // listener rebuilds the chain
    else
        rebuildChain();
}

void WoodshedProcessor::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! tree.hasType (boardId))
        return;

    if (property == slotsId)
        rebuildChain();
    else if (property == spotsId)
        sendChangeMessage();
}

void WoodshedProcessor::valueTreeRedirected (juce::ValueTree&)
{
    // A preset or saved session arrived: put it in chain order (older files may lack spots
    // or have the amp in the middle of the list), then rebuild.
    auto places = getPlacements();
    std::vector<int> sources ((size_t) places.size());
    std::iota (sources.begin(), sources.end(), 0);
    applyPlacements (places, sources);
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
    chain->generation = ++publishedGeneration;
    releaseHoldAtGeneration = chain->generation;
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

    // The audio thread is now running a chain built after the last rearrangement.
    if (releaseHoldAtGeneration >= 0 && installedGeneration.load() >= releaseHoldAtGeneration)
    {
        for (auto& i : instances)
            if (i != nullptr)
                i->knobsHeld = false;

        releaseHoldAtGeneration = -1;
    }
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
            installedGeneration.store (current->generation);
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

        if (tunerOpen.load (std::memory_order_relaxed))
        {
            const auto scope = tunerFifo.write (juce::jmin (n, tunerFifo.getFreeSpace()));
            if (scope.blockSize1 > 0) std::copy (left, left + scope.blockSize1, tunerRing.begin() + scope.startIndex1);
            if (scope.blockSize2 > 0) std::copy (left + scope.blockSize1, left + scope.blockSize1 + scope.blockSize2, tunerRing.begin() + scope.startIndex2);
        }

        const bool capturing = captureState.load() == CaptureState::recording;
        const int captureCount = capturing ? juce::jmin (n, captureLength - capturePos) : 0;
        if (capturing)
            captureBuffer.copyFrom (0, capturePos, left, captureCount);

        if (current != nullptr)
            current->process (left, right, scratch.getWritePointer (0), scratch.getWritePointer (1), n);
        else
            juce::FloatVectorOperations::copy (right, left, n);

        if (tunerOpen.load (std::memory_order_relaxed) && tunerMutes.load (std::memory_order_relaxed))
        {
            juce::FloatVectorOperations::clear (left, n);
            juce::FloatVectorOperations::clear (right, n);
        }

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
int WoodshedProcessor::readTunerInput (float* dest, int maxSamples)
{
    const auto scope = tunerFifo.read (juce::jmin (maxSamples, tunerFifo.getNumReady()));
    std::copy (tunerRing.begin() + scope.startIndex1, tunerRing.begin() + scope.startIndex1 + scope.blockSize1, dest);
    std::copy (tunerRing.begin() + scope.startIndex2, tunerRing.begin() + scope.startIndex2 + scope.blockSize2, dest + scope.blockSize1);
    return scope.blockSize1 + scope.blockSize2;
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
            replaceStateKeepingLevels (juce::ValueTree::fromXml (*xml));
            return true;
        }
    }

    return false;
}

void WoodshedProcessor::replaceStateKeepingLevels (const juce::ValueTree& state)
{
    // A preset is a sound, not a setup: the interface input and the levels stay put.
    std::vector<std::pair<juce::String, float>> kept;
    for (auto* id : { "input", "input_gain", "master" })
        if (auto* p = params.getParameter (id))
            kept.emplace_back (id, p->getValue());

    for (auto& i : instances) i.reset();
    for (auto& t : instanceTypes) t = {};
    params.replaceState (state);

    for (auto& [id, value] : kept)
        params.getParameter (id)->setValueNotifyingHost (value);
}

const juce::StringArray& WoodshedProcessor::factoryGenres()
{
    // Menu order; a genre file not listed here goes at the end.
    static const juce::StringArray genres { "Clean", "Blues", "Classic Rock", "Hard Rock & Metal", "Punk & Alt",
                                            "Indie & Dream Pop", "Funk & Soul", "Jazz & Country", "Leads",
                                            "Ambient & Shoegaze", "Psych & Doom", "Cinematic", "Weird", "Bass" };
    return genres;
}

const std::vector<woodshed::Preset>& WoodshedProcessor::factoryPresets()
{
    static const auto presets = []
    {
        std::vector<woodshed::Preset> all;

        for (int i = 0; i < PresetData::namedResourceListSize; ++i)
        {
            int size = 0;
            const auto* data = PresetData::getNamedResource (PresetData::namedResourceList[i], size);
            juce::StringArray errors;
            auto parsed = woodshed::parsePresets (juce::String::fromUTF8 (data, size), errors);
            jassert (errors.isEmpty());     // woodshed-bench presets catches these before they ship
            all.insert (all.end(), parsed.begin(), parsed.end());
        }

        auto rank = [] (const juce::String& genre)
        {
            const int i = factoryGenres().indexOf (genre);
            return i < 0 ? factoryGenres().size() : i;
        };

        std::stable_sort (all.begin(), all.end(), [&] (auto& a, auto& b) { return rank (a.genre) < rank (b.genre); });
        return all;
    }();

    return presets;
}

void WoodshedProcessor::loadFactoryPreset (const woodshed::Preset& preset)
{
    for (auto& i : instances) i.reset();
    for (auto& t : instanceTypes) t = {};

    std::vector<Placement> places;
    for (auto& p : preset.pedals)
        places.push_back ({ p.type, p.spot });

    applyPlacements (places, std::vector<int> (places.size(), -1));

    const auto board = getBoard();
    const auto spots = getSpots();

    for (int slot = 0; slot < board.size(); ++slot)
    {
        for (auto& p : preset.pedals)
        {
            if (p.type != board[slot] || p.spot != spots[(size_t) slot])
                continue;

            params.getParameter (onParamId (slot))->setValueNotifyingHost (p.on ? 1.0f : 0.0f);
            for (int k = 0; k < woodshed::Pedal::maxKnobs; ++k)
                params.getParameter (knobParamId (slot, k))->setValueNotifyingHost (p.knobs[(size_t) k]);
            break;
        }
    }
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
