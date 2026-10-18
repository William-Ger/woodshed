#pragma once

#include "Studio.h"
#include "Theme.h"
#include "Icons.h"

namespace woodshed::app
{

/** The unified toolbar across the top of the window (it shares its strip with the Mac's window
    buttons): the project name and status on the left, the transport and position readout in the
    middle, and the pane switch, tuner, settings and master level on the right. Empty space drags
    the window. */
class ControlBar final : public juce::Component,
                         public juce::TooltipClient,
                         private juce::ChangeListener,
                         private juce::ValueTree::Listener,
                         private juce::Timer
{
public:
    explicit ControlBar (Studio&);
    ~ControlBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void parentHierarchyChanged() override;
    juce::String getTooltip() override;

    //==============================================================================
    /** A borderless toolbar button showing one icon: a soft fill on hover, an accent (or
        semantic) tint when lit. */
    class IconButton final : public juce::Button
    {
    public:
        IconButton (icons::Icon, juce::Colour litColour, const juce::String& tooltip, float iconSize = 16.0f);
        void setLit (bool);
        bool isLit() const                          { return lit; }
        /** How strongly the lit colour tints the button (quieter inside the readout). */
        void setLitFill (float alpha)               { litFill = alpha; }

        void paintButton (juce::Graphics&, bool highlighted, bool down) override;

    private:
        icons::Icon icon;
        float litFill = 0.18f;
        juce::Colour litColour;
        float iconSize;
        bool lit = false;
    };

    /** One segment of the Rig / Mixer switch. The parent draws the track behind them. */
    class Segment final : public juce::Button
    {
    public:
        Segment (const juce::String& text, icons::Icon, const juce::String& tooltip);
        void setSelected (bool);
        bool isSelected() const                     { return selected; }

        void paintButton (juce::Graphics&, bool highlighted, bool down) override;

    private:
        icons::Icon icon;
        bool selected = false;
    };

    /** The recessed readout: position and clock, tempo, time signature, click and count-in. */
    class Readout final : public juce::Component
    {
    public:
        explicit Readout (ControlBar&);

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        /** Called at 30 Hz; repaints the parts whose text changed. */
        void poll();
        void projectChanged();

        /** Narrowest width that still shows every field. */
        static constexpr int preferredWidth = 300, minimumWidth = 262;

        IconButton click   { icons::metronome(), theme::accent, "Metronome (K)", 15.0f },
                   countIn { icons::countIn(), theme::accent, "Count-in: one bar of click before recording", 15.0f };

    private:
        void editTempo();
        void showSignatureMenu();

        ControlBar& owner;
        juce::Rectangle<int> positionArea, clockArea, tempoArea, signatureArea;
        juce::String positionText, clockText, tempoText, signatureText;
        enum class Hover { none, tempo, signature } hover = Hover::none;
        bool countingIn = false;
        bool draggingTempo = false, tempoTransactionStarted = false;
        double dragStartTempo = 120.0;
        daw::TimeGrid dragStartGrid;        // the map before a tempo drag, to re-place everything once at the end
        std::unique_ptr<juce::TextEditor> tempoEditor;
    };

    /** The slim two-channel master peak meter, drawn as two horizontal bars. */
    class Meter final : public juce::Component
    {
    public:
        Meter()                                     { setInterceptsMouseClicks (false, false); }
        void setLevels (float left, float right);
        void paint (juce::Graphics&) override;

    private:
        float levels[2] {}, holds[2] {};
        int holdTicks[2] {};
    };

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeRedirected (juce::ValueTree&) override        { refreshFromProject(); }
    void timerCallback() override;

    void attachToProject();
    void refreshFromProject();
    void refreshTransport();
    void refreshStatus();
    void moveByBars (int bars);
    void invoke (juce::CommandID);
    int leftInset() const;

    juce::String projectTitle() const;
    juce::Rectangle<int> titleArea() const                      { return titleBounds; }

    Studio& studio;
    juce::ValueTree watched;

    IconButton toStart    { icons::toStart(), theme::text, "Go to start (Return)" },
               barBack    { icons::back(), theme::text, "Back one bar" },
               barForward { icons::forward(), theme::text, "Forward one bar" },
               play       { icons::play(), theme::accent, "Play / stop (Space)" },
               stop       { icons::stop(), theme::text, "Stop (again: go to start)", 14.0f },
               record     { icons::record(), theme::record, "Record (R)" },
               loop       { icons::loop(), theme::loop, "Loop (C)", 18.0f };

    Readout readout { *this };

    Segment rigPane   { "Rig", icons::rig(), "Show the selected track's rig (B)" },
            mixerPane { "Mixer", icons::mixer(), "Show the mixer (X)" },
            loopsPane { "Loops", icons::loopGrid(), "Show the Loop Grid (G)" };

    IconButton tuner    { icons::tuningFork(), theme::accent, "Tuner", 17.0f },
               settings { icons::gear(), theme::accent, "Settings (Cmd+,)", 17.0f };

    struct MasterLook final : public juce::LookAndFeel_V4
    {
        int getSliderThumbRadius (juce::Slider&) override;
        void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                               juce::Slider::SliderStyle, juce::Slider&) override;
    } masterLook;

    juce::Slider master { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    Meter meter;

    juce::String shownTitle, shownFlash, statusText, deviceDetail;
    juce::Rectangle<int> titleBounds, segmentTrack, speakerArea;
    int tick = 0;
    int layoutInset = -1;
    bool knobDragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlBar)
};

} // namespace woodshed::app
