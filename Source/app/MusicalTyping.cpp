#include "MusicalTyping.h"

namespace woodshed::app
{

namespace
{
// The keys that play, from C up: white keys on the home row, black keys above them.
const char noteKeys[18] { 'a', 'w', 's', 'e', 'd', 'f', 't', 'g', 'y', 'h', 'u', 'j', 'k', 'o', 'l', 'p', ';', '\'' };

bool isBlack (int index)
{
    const int pc = index % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
}

MusicalTyping::MusicalTyping (Studio& s) : studio (s)
{
    setWantsKeyboardFocus (true);
    sounding.fill (-1);
}

MusicalTyping::~MusicalTyping()
{
    releaseAll();
}

void MusicalTyping::open()
{
    setVisible (true);
    toFront (true);
    grabKeyboardFocus();
    startTimerHz (30);
    repaint();
}

void MusicalTyping::close()
{
    releaseAll();
    stopTimer();
    setVisible (false);
    if (auto* parent = getParentComponent())
        parent->grabKeyboardFocus();
}

void MusicalTyping::send (const juce::MidiMessage& m)
{
    studio.engine().sendLiveMidi (m);
}

void MusicalTyping::releaseAll()
{
    for (size_t i = 0; i < held.size(); ++i)
    {
        if (sounding[i] >= 0)
            send (juce::MidiMessage::noteOff (1, sounding[i]));
        held[i] = false;
        sounding[i] = -1;
    }
    if (sustain)
        send (juce::MidiMessage::controllerEvent (1, 64, 0));
    sustain = false;
    repaint();
}

void MusicalTyping::focusLost (FocusChangeType)
{
    // Keys released while we can't see them would hang otherwise.
    releaseAll();
}

void MusicalTyping::timerCallback()
{
    // Catch releases that arrive without a key event (focus elsewhere, the window behind).
    keyStateChanged (false);
}

bool MusicalTyping::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown())
        return false;                               // shortcuts still work

    const auto c = (char) juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) key.getTextCharacter());
    if (key.getKeyCode() == juce::KeyPress::escapeKey) { close(); return true; }
    if (c == 'z') { releaseAll(); octave = juce::jmax (-2, octave - 1); repaint(); return true; }
    if (c == 'x') { releaseAll(); octave = juce::jmin (8, octave + 1); repaint(); return true; }
    if (c == 'c') { velocity = juce::jmax (1, velocity - 20); repaint(); return true; }
    if (c == 'v') { velocity = juce::jmin (127, velocity + 20); repaint(); return true; }

    for (char k : noteKeys)
        if (c == k)
            return true;                            // handled in keyStateChanged
    return key.getKeyCode() == juce::KeyPress::tabKey;
}

bool MusicalTyping::keyStateChanged (bool)
{
    bool changed = false;
    for (size_t i = 0; i < held.size(); ++i)
    {
        const bool down = hasKeyboardFocus (true) && juce::KeyPress::isKeyCurrentlyDown (noteKeys[i])
                       && ! juce::ModifierKeys::currentModifiers.isCommandDown();
        if (down == held[i])
            continue;
        held[i] = down;
        changed = true;
        if (down)
        {
            const int note = noteForKey ((int) i);
            if (note >= 0 && note <= 127)
            {
                sounding[i] = note;
                send (juce::MidiMessage::noteOn (1, note, (juce::uint8) velocity));
            }
        }
        else if (sounding[i] >= 0)
        {
            send (juce::MidiMessage::noteOff (1, sounding[i]));
            sounding[i] = -1;
        }
    }

    const bool tab = hasKeyboardFocus (true) && juce::KeyPress::isKeyCurrentlyDown (juce::KeyPress::tabKey);
    if (tab != sustain)
    {
        sustain = tab;
        send (juce::MidiMessage::controllerEvent (1, 64, tab ? 127 : 0));
        changed = true;
    }

    if (changed)
        repaint();
    return changed;
}

void MusicalTyping::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (e.y < 24 && e.x > getWidth() - 28)
        close();
}

void MusicalTyping::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    theme::drawRaised (g, r, theme::radiusPanel, theme::panelLight);

    auto top = getLocalBounds().removeFromTop (24).reduced (10, 0);
    g.setColour (theme::text);
    g.setFont (theme::font (12.0f, theme::Weight::semibold));
    const auto target = studio.getSelectedTrack();
    const bool playable = daw::Project::isInstrumentTrack (target);
    g.drawText ("Musical Typing" + (playable ? "  -  " + target[daw::ids::name].toString() : juce::String ("  -  select an instrument track")),
                top, juce::Justification::centredLeft, true);
    g.setColour (theme::dimText);
    g.drawText (juce::CharPointer_UTF8 ("\xc3\x97"), top.removeFromRight (18), juce::Justification::centred);

    auto info = getLocalBounds().removeFromBottom (22).reduced (10, 0);
    g.setColour (theme::dimText);
    g.setFont (theme::font (11.0f));
    g.drawText ("Octave C" + juce::String (octave) + "  (Z X)     Velocity " + juce::String (velocity) + "  (C V)     Sustain: Tab",
                info, juce::Justification::centredLeft, true);

    // The keys: 11 white across, black ones above.
    auto keys = getLocalBounds().withTrimmedTop (28).withTrimmedBottom (24).reduced (10, 0).toFloat();
    int whites = 0;
    for (int i = 0; i < 18; ++i)
        whites += isBlack (i) ? 0 : 1;
    const float w = keys.getWidth() / (float) whites;
    int whiteIndex = 0;
    g.setFont (theme::font (10.0f, theme::Weight::semibold));
    for (int i = 0; i < 18; ++i)
    {
        if (isBlack (i))
            continue;
        auto key = juce::Rectangle<float> (keys.getX() + w * (float) whiteIndex, keys.getY(), w - 2.0f, keys.getHeight());
        g.setColour (held[(size_t) i] ? theme::accent : juce::Colour (0xffd8d8dc));
        g.fillRoundedRectangle (key, 3.0f);
        g.setColour (juce::Colour (0xff3a3a40));
        g.drawText (juce::String::charToString ((juce::juce_wchar) noteKeys[i]).toUpperCase(), key.removeFromBottom (16.0f), juce::Justification::centred);
        ++whiteIndex;
    }
    whiteIndex = 0;
    for (int i = 0; i < 18; ++i)
    {
        if (! isBlack (i))
        {
            ++whiteIndex;
            continue;
        }
        auto key = juce::Rectangle<float> (keys.getX() + w * (float) whiteIndex - w * 0.32f, keys.getY(), w * 0.62f, keys.getHeight() * 0.58f);
        g.setColour (held[(size_t) i] ? theme::accent : juce::Colour (0xff202024));
        g.fillRoundedRectangle (key, 2.0f);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.drawText (juce::String::charToString ((juce::juce_wchar) noteKeys[i]).toUpperCase(), key.removeFromBottom (14.0f), juce::Justification::centred);
    }
}

} // namespace woodshed::app
