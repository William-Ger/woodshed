#pragma once

#include "PluginProcessor.h"
#include "engine/PitchDetector.h"

/** Full-window chromatic tuner: big note, cents needle and a strobe band. */
class TunerPanel final : public juce::Component,
                         private juce::Timer
{
public:
    explicit TunerPanel (WoodshedProcessor&);
    ~TunerPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;

    std::function<void()> onClose;

private:
    void timerCallback() override;

    WoodshedProcessor& processor;
    woodshed::PitchDetector detector;
    std::vector<float> history = std::vector<float> ((size_t) woodshed::PitchDetector::windowSize, 0.0f);
    std::vector<float> incoming = std::vector<float> (1 << 15);

    juce::TextButton muteToggle { "Mute output while tuning" };
    juce::ComboBox reference;
    juce::TextButton closeButton { "Done" };

    // Display state.
    std::array<float, 5> recent {};
    int recentCount = 0, silentFrames = 100;
    float shownCents = 0.0f, shownHz = 0.0f, strobePhase = 0.0f;
    int shownNote = -1;
    juce::Rectangle<int> panelBounds;

    float referenceHz() const                       { return 430.0f + (float) reference.getSelectedId(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TunerPanel)
};
