#pragma once

#include "Studio.h"
#include "Theme.h"
#include "../mastering/ProjectMastering.h"

namespace woodshed::app
{

/**
    The Mastering pane: loudness meters for the Stereo Out (EBU R128 momentary, short-term and
    integrated loudness, loudness range, true peak, correlation and a spectrum), and the
    mastering assistant.

    Analyse bounces the song (without the mastering processor), measures it, and puts the
    processor on the Stereo Out set to hit the chosen loudness target, with a matching EQ curve
    toward a balanced spectrum. Then the target, Character and amount controls re-master from
    that same bounce (no new bounce needed until the song changes), and Bypass compares (A/B).
*/
class MasteringPanel final : public juce::Component,
                             private juce::ChangeListener,
                             private juce::ValueTree::Listener,
                             private Studio::ProjectSwapListener,
                             private juce::Timer
{
public:
    explicit MasteringPanel (Studio&);
    ~MasteringPanel() override;

    /** Bounces the song and masters it (the Mastering... command). */
    void analyse();

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override        { refresh(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override  { refresh(); }
    void projectAboutToBeReplaced() override;
    void timerCallback() override;

    /** Re-masters the cached bounce with the controls' settings, on a background thread. */
    void remaster();
    void finished (const mastering::Result&, const mastering::Target&);
    mastering::Settings currentSettings() const;
    mastering::Target currentTarget() const;
    void refresh();
    void setKnob (int knob, float value, const juce::String& undoName);
    void drainMeters();

    void paintMeters (juce::Graphics&, juce::Rectangle<int>);
    void paintSpectrum (juce::Graphics&, juce::Rectangle<int>);
    void paintReport (juce::Graphics&, juce::Rectangle<int>);

    Studio& studio;
    juce::ValueTree watched, processor;

    // Header
    juce::ComboBox target;
    juce::Slider customLufs, customPeak;
    juce::OwnedArray<juce::TextButton> characters;
    juce::TextButton analyseButton { "Analyse" }, bypass { "Bypass" }, resetMeters { "Reset" };

    // Amounts, bound to the processor's knobs.
    struct Control { int knob; juce::String name; std::unique_ptr<juce::Slider> slider; std::unique_ptr<juce::Label> label; };
    std::vector<Control> controls;

    // Live meters (the engine's output tap, drained here).
    mastering::LoudnessMeter meter;
    mastering::CorrelationMeter correlation;
    mastering::SpectrumAnalyser spectrum { false };
    std::vector<float> scratchLeft, scratchRight;
    double meterRate = 0.0;
    bool wasPlaying = false;

    // The assistant: the last bounce, its measurement, and the job on its way.
    struct Job { std::atomic<bool> cancelled { false }; std::atomic<float> progress { 0.0f }; };
    std::shared_ptr<Job> job;
    std::shared_ptr<const juce::AudioBuffer<float>> mix;
    std::shared_ptr<const mastering::Measurement> mixMeasurement;
    double mixRate = 48000.0;
    juce::String status;
    bool bouncing = false;

    juce::Rectangle<int> metersArea, spectrumArea, reportArea;
    static constexpr int headerHeight = 40;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasteringPanel)
};

} // namespace woodshed::app
