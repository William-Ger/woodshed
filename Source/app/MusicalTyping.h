#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

/**
    Logic's Musical Typing: play the selected instrument track from the computer keyboard.

        A W S E D F T G Y H U J K O L P ; '   the notes from C up to F an octave later
        Z / X                                 octave down / up
        C / V                                 velocity down / up
        Tab                                   sustain while held

    A small floating keyboard shows what's held. While it's open it has the keyboard focus, so
    those keys play instead of running commands; Space and the Cmd shortcuts still work.
*/
class MusicalTyping final : public juce::Component,
                            private juce::Timer
{
public:
    explicit MusicalTyping (Studio&);
    ~MusicalTyping() override;

    void open();
    void close();
    bool isOpen() const                                     { return isVisible(); }

    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;
    void focusLost (FocusChangeType) override;
    void mouseDown (const juce::MouseEvent&) override;

    static constexpr int preferredWidth = 520, preferredHeight = 118;

private:
    void timerCallback() override;
    void releaseAll();
    void send (const juce::MidiMessage&);
    int noteForKey (int index) const                        { return 12 * (octave + 2) + index; }

    Studio& studio;
    int octave = 3;                 // C3 (MIDI 60) on A, as in Logic
    int velocity = 100;
    std::array<bool, 18> held {};
    std::array<int, 18> sounding {};
    bool sustain = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MusicalTyping)
};

} // namespace woodshed::app
