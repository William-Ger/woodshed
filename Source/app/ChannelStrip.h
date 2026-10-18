#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

namespace mixer
{
    /** Stored for a fader pulled all the way down; the engine treats anything at or below -96 dB as silence. */
    constexpr double silenceDb = -100.0;

    /** Fader travel (0 = bottom, 1 = top) <-> dB, on a console-style taper with 0 dB at about three quarters. */
    float faderPos (double db);
    double faderDb (float pos);

    /** Meter height (0..1) <-> dBFS, -60..+6. */
    float meterPos (float db);
    float meterDb (float pos);

    /** "+2.5", "0.0", "-12.0", "-inf". */
    juce::String dbText (double db);

    //==============================================================================
    /** A control bound to one property of a ValueTree. It paints straight from the tree, so undo and
        edits made elsewhere show up on the next repaint; its own edits go through the project's undo. */
    class Bound : public juce::Component,
                  public juce::SettableTooltipClient
    {
    public:
        Bound (Studio&, juce::ValueTree, const juce::Identifier&, const juce::String& undoName);

    protected:
        juce::var get() const                               { return tree[prop]; }
        void beginGesture();
        void set (const juce::var&);

        Studio& studio;
        juce::ValueTree tree;
        juce::Identifier prop;
        juce::String undoName;
        juce::uint32 lastWheel = 0;

        /** Wheel turns that follow each other closely are one undo step. */
        void beginWheelGesture();
    };

    /** The volume fader: thin groove, flat cap, dB scale. Drag (Shift for fine), wheel, double-click = 0 dB. */
    class Fader final : public Bound
    {
    public:
        Fader (Studio&, juce::ValueTree, const juce::Identifier&, juce::Colour tint);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void mouseUp (const juce::MouseEvent&) override     { repaint(); }
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }

        void setTint (juce::Colour c)                       { tint = c; repaint(); }
        /** Vertical span the cap's centre travels; the meter beside it lines up with this. */
        juce::Range<float> travel() const;

        static constexpr int scaleWidth = 22, slotWidth = 22;

    private:
        double value() const;
        float capHeight() const;
        void setPos (float pos, bool detent);

        juce::Colour tint;
        float dragStartPos = 0.0f;
        int lastDragY = 0;
        bool fine = false;
    };

    /** Pan, -1..1. Drag up/right, wheel, double-click centres. */
    class PanKnob final : public Bound
    {
    public:
        PanKnob (Studio&, juce::ValueTree);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void mouseUp (const juce::MouseEvent&) override     { repaint(); }
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }

    private:
        float value() const;
        void setValue (float);
        float dragStart = 0.0f;
    };

    /** A small latching rounded button for a bool property. */
    class Toggle final : public Bound
    {
    public:
        enum class Glyph { text, dot, headphones, led };

        Toggle (Studio&, juce::ValueTree, const juce::Identifier&, const juce::String& undoName,
                const juce::String& label, juce::Colour onColour, Glyph);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override   { repaint(); }
        void mouseUp (const juce::MouseEvent&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }

        /** Shown faintly when the state is implied rather than set (a track silenced by another's solo). */
        std::function<bool()> implied;

    private:
        juce::String label;
        juce::Colour onColour;
        Glyph glyph;
    };

    /** Which interface input the track records and monitors. */
    class InputSelector final : public Bound
    {
    public:
        InputSelector (Studio&, juce::ValueTree);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }

    private:
        int numInputs() const;
    };

    /** A chip showing a line of text from a callback (the instrument, the effects); click for an action. */
    class SlotButton final : public juce::Component,
                             public juce::SettableTooltipClient
    {
    public:
        SlotButton() = default;
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override   { repaint(); }
        void mouseUp (const juce::MouseEvent&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }

        std::function<juce::String()> text;
        std::function<bool()> lit;              // shown in the accent colour
        std::function<void()> onClick;
    };

    /** One of a strip's sends (Logic's send slots): the aux it goes to and its level. Drag to set
        the level, double-click for 0 dB, click for pre/post, another aux or remove. With no send
        at its position it offers "+ Send" (the first empty slot only). */
    class SendSlot final : public juce::Component,
                           public juce::SettableTooltipClient
    {
    public:
        SendSlot (Studio&, juce::ValueTree track, int index);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }

    private:
        juce::ValueTree send() const;
        void showAddMenu();
        void showSendMenu();

        Studio& studio;
        juce::ValueTree track;
        int index;
        float dragStartPos = 0.0f;
        bool dragged = false;
    };

    /** A small momentary button with a drawn icon. */
    class IconButton final : public juce::Component,
                             public juce::SettableTooltipClient
    {
    public:
        enum class Icon { rig };
        explicit IconButton (Icon i) : icon (i) {}

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override   { repaint(); }
        void mouseUp (const juce::MouseEvent&) override;
        void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { repaint(); }

        std::function<void()> onClick;

    private:
        Icon icon;
    };

    /** Stereo peak meter: falling bars, peak hold, a clip light that stays on until clicked. */
    class Meter final : public juce::Component,
                        public juce::SettableTooltipClient
    {
    public:
        Meter();

        /** Linear peaks since the last call; dt in seconds. */
        void push (float left, float right, float dt);
        void resetPeaks();

        float getPeakHoldDb() const                         { return juce::jmax (hold[0], hold[1]); }
        bool isClipped() const                              { return clipped; }

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

        std::function<void()> onReset;

    private:
        float level[2] { -100.0f, -100.0f }, hold[2] { -100.0f, -100.0f }, holdAge[2] {};
        bool clipped = false;
    };
}

//==============================================================================
/** One console strip: a track's (or, with no track, the master's) controls, fader and meter. */
class ChannelStrip final : public juce::Component
{
public:
    /** A track strip, or the master strip when `track` is the project state itself. */
    ChannelStrip (Studio&, juce::ValueTree track, bool isMaster);
    ~ChannelStrip() override;

    static constexpr int trackWidth = 88, masterWidth = 100;

    const juce::ValueTree& getTree() const                  { return tree; }
    int getTrackId() const                                  { return master ? -1 : (int) tree[daw::ids::id]; }
    bool isMaster() const                                   { return master; }

    void setSelected (bool);
    void setAnySolo (bool);
    /** Something in the tree changed: repaint from it. */
    void refresh();

    /** Takes the engine's peaks since the last call and moves the meter; dt in seconds. */
    void pollMeter (float dt);
    /** Throws away the engine's accumulated peaks (after the mixer was hidden). */
    void discardMeter();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Colour trackColour() const;
    void select();

    Studio& studio;
    juce::ValueTree tree;
    const bool master;
    bool selected = false, anySolo = false;

    mixer::Fader fader;
    mixer::Meter meter;
    std::unique_ptr<mixer::InputSelector> input;
    std::unique_ptr<mixer::Toggle> rig, mute, solo, arm, monitor;
    std::unique_ptr<mixer::IconButton> openRig;
    std::unique_ptr<mixer::SlotButton> instrumentSlot, fxSlot;
    std::unique_ptr<mixer::SendSlot> sendSlots[2];
    std::unique_ptr<mixer::SlotButton> groupSlot;           // the track's group and VCA, in the name plate
    bool scalerOnly = false;                                // a VCA or folder stack main: a fader, no audio
    bool instrumentTrack = false, auxTrack = false;
    void showInserts();
    std::unique_ptr<mixer::PanKnob> pan;

    juce::Rectangle<int> nameArea, topArea, faderReadout, peakReadout;
    juce::String shownPeak;
    bool shownClip = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};

} // namespace woodshed::app
