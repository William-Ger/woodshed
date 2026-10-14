#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

/** The strip across the top: transport, the position readout, tempo and meter, loop/click/count-in, master level, and the pane switches. */
class ControlBar final : public juce::Component,
                         private juce::ChangeListener,
                         private juce::ValueTree::Listener,
                         private juce::Timer
{
public:
    explicit ControlBar (Studio&);
    ~ControlBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    //==============================================================================
    /** A square transport button with a drawn glyph. */
    class IconButton final : public juce::Button
    {
    public:
        enum class Glyph { toStart, barBack, barForward, play, stop, record, loop };

        IconButton (Glyph, juce::Colour litColour, const juce::String& tooltip);
        void setLit (bool);
        bool isLit() const                          { return lit; }

        void paintButton (juce::Graphics&, bool highlighted, bool down) override;

    private:
        Glyph glyph;
        juce::Colour litColour;
        bool lit = false;
    };

    /** A small labelled switch: the pane toggles, Audio, Tuner, click and count-in. */
    class PillButton final : public juce::Button
    {
    public:
        PillButton (const juce::String& text, bool withLed, const juce::String& tooltip);
        void setLit (bool);

        void paintButton (juce::Graphics&, bool highlighted, bool down) override;

    private:
        bool led, lit = false;
    };

    /** The dark inset with the position, tempo and time signature. */
    class Readout final : public juce::Component
    {
    public:
        explicit Readout (ControlBar&);

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        /** Called at 30 Hz; repaints the parts whose text changed. */
        void poll();
        void projectChanged();

        PillButton click { "CLICK", true, "Metronome (K)" },
                   countIn { "COUNT-IN", true, "One bar of click before recording" };

    private:
        void editTempo();
        void showSignatureMenu();

        ControlBar& owner;
        juce::Rectangle<int> positionArea, clockArea, tempoArea, signatureArea;
        juce::String positionText, clockText, tempoText, signatureText;
        bool countingIn = false;
        bool draggingTempo = false, tempoTransactionStarted = false;
        double dragStartTempo = 120.0;
        std::unique_ptr<juce::TextEditor> tempoEditor;
    };

    /** The two-channel master peak meter. */
    class Meter final : public juce::Component
    {
    public:
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

    juce::String projectTitle() const;
    juce::Rectangle<int> titleArea() const;

    Studio& studio;
    juce::ValueTree watched;

    IconButton toStart    { IconButton::Glyph::toStart, theme::text, "Go to start (Return)" },
               barBack    { IconButton::Glyph::barBack, theme::text, "Back one bar" },
               barForward { IconButton::Glyph::barForward, theme::text, "Forward one bar" },
               play       { IconButton::Glyph::play, theme::accent, "Play / stop (Space)" },
               stop       { IconButton::Glyph::stop, theme::text, "Stop (again: go to start)" },
               record     { IconButton::Glyph::record, theme::record, "Record (R)" },
               loop       { IconButton::Glyph::loop, theme::loop, "Loop (C)" };

    Readout readout { *this };

    PillButton audio { "Audio", false, "Audio interface settings" },
               rigPane { "Rig", false, "Show the selected track's rig (B)" },
               mixerPane { "Mixer", false, "Show the mixer (X)" },
               tuner { "Tuner", false, "Open the tuner" };

    struct KnobLook final : public juce::LookAndFeel_V4
    {
        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider&) override;
    } knobLook;

    juce::Slider master { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    Meter meter;

    juce::String shownTitle, shownFlash, statusText;
    juce::Rectangle<int> statusArea, masterLabelArea;
    int tick = 0;
    bool knobDragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlBar)
};

} // namespace woodshed::app
