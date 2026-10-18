#pragma once

#include "Studio.h"
#include "Theme.h"
#include "../flex/Analysis.h"

namespace woodshed::app
{

/**
    Pitch Edit: the notes of a sung or played (monophonic) audio region, as blobs on a piano
    roll laid over the audio, with the pitch line through them. Drag a note up or down to move
    it by semitones (Cmd for fine pitch); the sliders set the selected notes' fine pitch, how
    much of their drift and vibrato stays, their gain and their formants; Quantize Pitch puts
    them on the chosen key and scale. Edits play through a formant-preserving pitch shift (the
    track's Time Stretch turns on, Monophonic, the first time).
*/
class PitchEditor final : public juce::Component,
                          private juce::ChangeListener,
                          private juce::ValueTree::Listener,
                          private juce::Timer
{
public:
    explicit PitchEditor (Studio&);
    ~PitchEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeRedirected (juce::ValueTree&) override   { sync(); }
    void timerCallback() override;

    /** Picks up the region to edit, detecting its notes the first time. */
    void sync();
    void detectNotes (bool undoable);
    void updateSliders();
    void applyToSelection (std::function<void (woodshed::flex::PitchNote&)>, const juce::String& undoName);
    void commit (const std::vector<woodshed::flex::PitchNote>&, const juce::String& undoName);
    void quantizeToScale();

    juce::Rectangle<int> rollArea() const;
    float xForTimeline (double sample) const;
    float yForPitch (float pitch) const;
    double timelineFor (double src) const;
    int noteAt (juce::Point<int>) const;
    juce::Rectangle<float> noteRect (const woodshed::flex::PitchNote&) const;

    Studio& studio;
    juce::ValueTree region, watched;
    std::vector<woodshed::flex::PitchNote> notes;      // as stored, or as being dragged
    std::shared_ptr<const woodshed::flex::PitchTrack> track;
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    std::vector<int> selected;
    float lowPitch = 48.0f, highPitch = 72.0f;

    // A drag of the selected notes up or down.
    bool dragging = false;
    float dragStartY = 0.0f;
    std::vector<woodshed::flex::PitchNote> beforeDrag;

    juce::Label title;
    juce::TextButton detectButton { "Detect Notes" }, quantizeButton { "Quantize Pitch" };
    juce::ComboBox keyBox, scaleBox;
    struct Control { juce::Label label; juce::Slider slider; };
    Control fine, drift, vibrato, gain, formant;
    bool updatingSliders = false;
    float lastPlayheadX = -1.0f;

    static constexpr int headerHeight = 70, keysWidth = 44;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchEditor)
};

} // namespace woodshed::app
