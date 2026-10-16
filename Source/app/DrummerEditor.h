#pragma once

#include "Studio.h"
#include "Theme.h"
#include "../daw/Drummer.h"

namespace woodshed::app
{

/**
    The Drummer editor (Logic's Drummer Editor), in the bottom pane, for the selected Drummer
    region: pick a style, drag the puck between Simple and Complex, Soft and Loud, set swing,
    fills and feel, choose kit pieces and the timekeeper. Every change writes the region's notes
    again; a drag previews as it goes and is one undo step when it ends. Regenerate plays the
    same settings with a new seed.
*/
class DrummerEditor final : public juce::Component,
                            private juce::ChangeListener,
                            private juce::ValueTree::Listener,
                            private Studio::ProjectSwapListener,
                            private juce::Timer
{
public:
    explicit DrummerEditor (Studio&);
    ~DrummerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class StyleGrid;
    class XYPad;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void projectAboutToBeReplaced() override;
    void timerCallback() override;

    void sync();
    void showSettings();
    bool isEditing() const                  { return daw::drummer::isDrummerRegion (region); }

    /** A change from the controls: previewed while `dragging` (made one undo step by endGesture),
        else written at once as one undo step. */
    void change (const daw::drummer::Settings&, const juce::String& undoName, bool dragging);
    void endGesture();
    void commit (const juce::String& undoName);

    void makeDrummerRegion();

    Studio& studio;
    juce::ValueTree watched, region;
    daw::drummer::Settings settings;
    daw::RegionSnapshot snapshot;
    juce::String gestureName;
    bool previewPending = false, writing = false;

    std::unique_ptr<StyleGrid> styleGrid;
    std::unique_ptr<XYPad> pad;
    juce::Slider swing, fill, feel;
    juce::TextButton kickSnare { "Kick & Snare" }, hiHat { "Hi-Hat" }, cymbals { "Cymbals" }, toms { "Toms" }, percussion { "Percussion" };
    juce::ComboBox timekeeper, bars;
    juce::TextButton regenerate { "Regenerate" }, make;

    static constexpr int headerHeight = 40;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrummerEditor)
};

} // namespace woodshed::app
