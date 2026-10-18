#pragma once

#include "Studio.h"
#include "Theme.h"
#include "InstrumentPanel.h"
#include "../instruments/SamplerTrack.h"

namespace woodshed::app
{

/**
    The Sampler editor, in the bottom pane, for the selected Sampler track: the sample's waveform
    with draggable start, end and loop markers (and slice lines in Slice mode), the zones on a
    keyboard map, the selected zone's settings, the mode, and the instrument's envelope, filter
    and LFO knobs. Drop audio files anywhere on it to load them (onto a new Sampler track when the
    selected track isn't one). Every change is one undo step.
*/
class SamplerEditor final : public juce::Component,
                            public juce::FileDragAndDropTarget,
                            private juce::ChangeListener,
                            private juce::ValueTree::Listener,
                            private Studio::ProjectSwapListener
{
public:
    explicit SamplerEditor (Studio&);
    ~SamplerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override      { dropping = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override                 { dropping = false; repaint(); }
    void filesDropped (const juce::StringArray&, int, int) override;

    // The commands MainComponent routes here.
    /** A new Sampler track after the selected one, shown in this pane. */
    static void newSamplerTrack (Studio&);
    /** Convert to Sampler Track on the first selected audio region (Classic or Slice). */
    static void convertSelectedRegion (Studio&, sampler::Mode);
    static bool canConvertSelection (Studio&);

private:
    class Waveform;
    class KeyMap;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void projectAboutToBeReplaced() override;

    void sync();
    void refresh();
    bool belongsToTrack (const juce::ValueTree&) const;
    bool isEditing() const                      { return sampler::isSamplerTrack (track); }
    sampler::Mode modeNow() const;

    juce::ValueTree samplerNode() const;
    juce::ValueTree zone (int index) const;
    int numZones() const;
    std::shared_ptr<const sampler::Source> sourceOf (const juce::ValueTree& zone) const;

    void loadFiles (const juce::Array<juce::File>&);
    void chooseFile();
    void setMode (sampler::Mode);
    void setZoneProperty (const juce::Identifier&, const juce::var&, const juce::String& undoName);

    Studio& studio;
    juce::ValueTree watched, track;
    int selectedZone = 0;
    bool dropping = false, updating = false;

    std::unique_ptr<Waveform> waveform;
    std::unique_ptr<KeyMap> keyMap;
    juce::ComboBox mode, loopMode;
    juce::TextButton load { "Load Sample..." }, gate { "Gate" };
    juce::Slider sensitivity;

    struct Field
    {
        juce::String label;
        juce::Identifier property;
        juce::Slider slider;
    };
    std::vector<std::unique_ptr<Field>> fields;

    ParamGrid knobs;
    juce::Viewport knobView;
    std::unique_ptr<juce::FileChooser> chooser;

    static constexpr int headerHeight = 40;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerEditor)
};

} // namespace woodshed::app
