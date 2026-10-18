#pragma once

#include "../instruments/Instrument.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace woodshed::daw
{

/** Makes an Audio Unit or VST3 from its PluginDescription (as XML text), ready to prepare.
    Null (with `error` set) if it can't be loaded. Message thread. */
using PluginFactory = std::function<std::unique_ptr<juce::AudioPluginInstance> (const juce::String& descriptionXml,
                                                                               double sampleRate, int blockSize,
                                                                               juce::String& error)>;

/** A third-party instrument plug-in playing on an instrument track. */
class PluginInstrument final : public Instrument
{
public:
    explicit PluginInstrument (std::unique_ptr<juce::AudioPluginInstance> p) : plugin (std::move (p)) {}

    juce::AudioPluginInstance& getPlugin()                      { return *plugin; }

    void reset() override
    {
        plugin->reset();
        pendingAllOff = true;
    }

    int getLatencySamples() const override                      { return plugin->getLatencySamples(); }

    void render (float* left, float* right, int numSamples, const NoteEvent* events, int numEvents) override
    {
        midi.clear();
        if (pendingAllOff)
        {
            for (int ch = 1; ch <= 16; ++ch)
                midi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
            pendingAllOff = false;
        }

        for (int i = 0; i < numEvents; ++i)
        {
            const auto& e = events[i];
            const int at = juce::jlimit (0, numSamples - 1, e.offset);
            switch (e.kind)
            {
                case NoteEvent::noteOn:      midi.addEvent (juce::MidiMessage::noteOn (1, e.note, juce::jlimit (0.0f, 1.0f, e.value)), at); break;
                case NoteEvent::noteOff:     midi.addEvent (juce::MidiMessage::noteOff (1, e.note), at); break;
                case NoteEvent::allNotesOff: midi.addEvent (juce::MidiMessage::allNotesOff (1), at);
                                             midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), at); break;
                case NoteEvent::sustain:     midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, e.value >= 0.5f ? 127 : 0), at); break;
                case NoteEvent::pitchBend:   midi.addEvent (juce::MidiMessage::pitchWheel (1, juce::jlimit (0, 16383, (int) ((e.value + 1.0f) * 8192.0f))), at); break;
                case NoteEvent::modWheel:    midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, juce::roundToInt (e.value * 127.0f)), at); break;
                default: break;
            }
        }

        for (int done = 0; done < numSamples;)
        {
            const int n = juce::jmin (numSamples - done, buffer.getNumSamples());
            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), n);
            view.clear();
            if (done == 0)
                plugin->processBlock (view, midi);
            else
            {
                juce::MidiBuffer none;
                plugin->processBlock (view, none);
            }
            const int outs = juce::jmin (plugin->getTotalNumOutputChannels(), view.getNumChannels());
            if (outs >= 2)
            {
                juce::FloatVectorOperations::add (left + done, view.getReadPointer (0), n);
                juce::FloatVectorOperations::add (right + done, view.getReadPointer (1), n);
            }
            else if (outs == 1)
            {
                juce::FloatVectorOperations::add (left + done, view.getReadPointer (0), n);
                juce::FloatVectorOperations::add (right + done, view.getReadPointer (0), n);
            }
            done += n;
        }
    }

protected:
    void prepared() override
    {
        // Stereo out where the plug-in allows it; no audio in.
        auto layout = plugin->getBusesLayout();
        if (layout.outputBuses.size() > 0)
        {
            layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
            for (auto& in : layout.inputBuses)
                in = juce::AudioChannelSet::disabled();
            if (plugin->checkBusesLayoutSupported (layout))
                plugin->setBusesLayout (layout);
        }
        plugin->setRateAndBufferSizeDetails (sampleRate, maxBlock);
        plugin->prepareToPlay (sampleRate, maxBlock);
        buffer.setSize (juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()), maxBlock);
        midi.ensureSize (4096);
    }

private:
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    bool pendingAllOff = false;
};

/** A third-party effect plug-in in a track's (or the master's) inserts. Stereo in and out. */
class PluginEffect final : public Pedal
{
public:
    explicit PluginEffect (std::unique_ptr<juce::AudioPluginInstance> p) : plugin (std::move (p)) {}

    juce::AudioPluginInstance& getPlugin()                      { return *plugin; }

    void reset() override                                       { plugin->reset(); }
    bool producesStereo() const override                        { return true; }
    int getLatencySamples() const override                      { return plugin->getLatencySamples(); }

    void processChannel (int channel, float* data, int numSamples) override
    {
        // Only reached if the chain runs it in mono: treat the one channel as both sides.
        juce::ignoreUnused (channel);
        processStereo (data, data, numSamples);
    }

    void processStereo (float* left, float* right, int numSamples) override
    {
        for (int done = 0; done < numSamples;)
        {
            const int n = juce::jmin (numSamples - done, buffer.getNumSamples());
            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), n);
            view.clear();
            juce::FloatVectorOperations::copy (view.getWritePointer (0), left + done, n);
            if (view.getNumChannels() > 1)
                juce::FloatVectorOperations::copy (view.getWritePointer (1), right + done, n);
            midi.clear();
            plugin->processBlock (view, midi);
            juce::FloatVectorOperations::copy (left + done, view.getReadPointer (0), n);
            if (left != right)
                juce::FloatVectorOperations::copy (right + done, view.getReadPointer (view.getNumChannels() > 1 ? 1 : 0), n);
            done += n;
        }
    }

protected:
    void prepared() override
    {
        auto layout = plugin->getBusesLayout();
        if (layout.inputBuses.size() > 0 && layout.outputBuses.size() > 0)
        {
            layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
            layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
            if (plugin->checkBusesLayoutSupported (layout))
                plugin->setBusesLayout (layout);
        }
        plugin->setRateAndBufferSizeDetails (sampleRate, maxBlock);
        plugin->prepareToPlay (sampleRate, maxBlock);
        buffer.setSize (juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()), maxBlock);
        midi.ensureSize (256);
    }

private:
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
};

} // namespace woodshed::daw
