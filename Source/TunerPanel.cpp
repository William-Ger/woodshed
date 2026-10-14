#include "TunerPanel.h"

namespace
{
const juce::Colour inTune     { 0xff6fdc8c };
const juce::Colour nearly     { 0xffe8a33d };
const juce::Colour off        { 0xffff5a48 };
const juce::Colour text       { 0xffe9e2d4 };
const juce::Colour dimText    { 0xff8d857a };
const juce::Colour panel      { 0xff161513 };

const char* const noteNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

juce::Colour colourForCents (float cents)
{
    const float a = std::abs (cents);
    return a <= 3.0f ? inTune : a <= 15.0f ? nearly : off;
}
}

TunerPanel::TunerPanel (WoodshedProcessor& p) : processor (p)
{
    muteToggle.setClickingTogglesState (true);
    muteToggle.setToggleState (processor.tunerMutes.load(), juce::dontSendNotification);
    muteToggle.setColour (juce::TextButton::buttonOnColourId, inTune.darker (0.35f));
    muteToggle.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    muteToggle.onClick = [this] { processor.tunerMutes = muteToggle.getToggleState(); };
    addAndMakeVisible (muteToggle);

    for (int hz = 432; hz <= 446; ++hz)
        reference.addItem ("A = " + juce::String (hz) + " Hz", hz - 430);
    reference.setSelectedId (10, juce::dontSendNotification);
    reference.setTooltip ("Reference pitch");
    addAndMakeVisible (reference);

    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeButton);

    setWantsKeyboardFocus (true);
}

TunerPanel::~TunerPanel()
{
    processor.tunerOpen = false;
}

void TunerPanel::visibilityChanged()
{
    processor.tunerOpen = isVisible();

    if (isVisible())
    {
        std::fill (history.begin(), history.end(), 0.0f);
        silentFrames = 100;
        recentCount = 0;
        startTimerHz (30);
        grabKeyboardFocus();
    }
    else
    {
        stopTimer();
    }
}

void TunerPanel::timerCallback()
{
    // Slide the newest input into the analysis window.
    const int got = processor.readTunerInput (incoming.data(), (int) incoming.size());
    const int window = (int) history.size();

    if (got >= window)
    {
        std::copy (incoming.begin() + (got - window), incoming.begin() + got, history.begin());
    }
    else if (got > 0)
    {
        std::move (history.begin() + got, history.end(), history.begin());
        std::copy (incoming.begin(), incoming.begin() + got, history.end() - got);
    }

    const auto r = detector.analyse (history.data(), processor.getCurrentSampleRate());
    const bool confident = r.frequency > 0.0f && r.clarity > 0.88f && juce::Decibels::gainToDecibels (r.level) > -58.0f;

    if (confident)
    {
        // Median of the last few readings throws away the odd octave slip on the pick attack.
        recent[(size_t) (recentCount++ % (int) recent.size())] = r.frequency;
        std::array<float, 5> sorted = recent;
        const int n = juce::jmin (recentCount, (int) recent.size());
        std::sort (sorted.begin(), sorted.begin() + n);
        const float hz = sorted[(size_t) (n / 2)];

        const float midi = 69.0f + 12.0f * std::log2 (hz / referenceHz());
        const int note = juce::roundToInt (midi);
        const float cents = (midi - (float) note) * 100.0f;

        if (note != shownNote || silentFrames > 6)
            shownCents = cents;                                  // jump straight to a new note
        else
            shownCents += 0.35f * (cents - shownCents);

        shownNote = note;
        shownHz = hz;
        silentFrames = 0;
    }
    else
    {
        ++silentFrames;
        if (silentFrames > 3) recentCount = 0;
    }

    strobePhase += shownCents * 0.12f;
    repaint();
}

void TunerPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.72f));

    auto p = panelBounds.toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRoundedRectangle (p.translated (0, 10), 22.0f);
    juce::ColourGradient bg (juce::Colour (0xff1e1c19), p.getX(), p.getY(), panel, p.getX(), p.getBottom(), false);
    g.setGradientFill (bg);
    g.fillRoundedRectangle (p, 22.0f);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.drawRoundedRectangle (p.reduced (1.0f), 21.0f, 1.5f);

    auto area = p.reduced (40.0f, 28.0f);
    g.setColour (dimText);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold).withKerningFactor (0.3f));
    g.drawText ("CHROMATIC TUNER", area.removeFromTop (20.0f), juce::Justification::centredLeft);

    const bool hasNote = shownNote >= 0 && silentFrames < 45;
    const float fade = hasNote ? 1.0f - juce::jlimit (0.0f, 1.0f, (float) silentFrames / 45.0f) * 0.6f : 0.25f;
    const auto colour = hasNote ? colourForCents (shownCents) : dimText;

    // Gauge.
    auto gaugeArea = area.removeFromTop (area.getHeight() * 0.42f);
    const auto centre = juce::Point<float> (gaugeArea.getCentreX(), gaugeArea.getBottom() + gaugeArea.getHeight() * 0.25f);
    const float radius = juce::jmin (gaugeArea.getWidth() * 0.6f, gaugeArea.getHeight() * 1.15f);
    g.saveState();
    g.reduceClipRegion (gaugeArea.expanded (0.0f, 4.0f).toNearestInt());
    const float span = juce::degreesToRadians (55.0f);

    for (int c = -50; c <= 50; c += 5)
    {
        const float a = (float) c / 50.0f * span;
        const bool major = c % 25 == 0;
        const bool centreTick = c == 0;
        g.setColour (centreTick ? inTune.withAlpha (0.9f) : juce::Colours::white.withAlpha (major ? 0.55f : 0.25f));
        g.drawLine ({ centre.getPointOnCircumference (radius * (major ? 0.86f : 0.9f), a),
                      centre.getPointOnCircumference (radius, a) }, centreTick ? 3.0f : major ? 2.0f : 1.2f);
    }

    // The in-tune window.
    juce::Path zone;
    zone.addCentredArc (centre.x, centre.y, radius * 1.04f, radius * 1.04f, 0.0f, -3.0f / 50.0f * span, 3.0f / 50.0f * span, true);
    g.setColour (inTune.withAlpha (hasNote && std::abs (shownCents) <= 3.0f ? 0.9f : 0.3f));
    g.strokePath (zone, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (dimText);
    g.setFont (juce::FontOptions (12.0f));
    for (int c : { -50, -25, 25, 50 })
    {
        const auto at = centre.getPointOnCircumference (radius * 0.76f, (float) c / 50.0f * span);
        g.drawText ((c > 0 ? "+" : "") + juce::String (c), juce::Rectangle<float> (40.0f, 16.0f).withCentre (at), juce::Justification::centred);
    }

    // Needle.
    const float needleAngle = juce::jlimit (-1.0f, 1.0f, shownCents / 50.0f) * span;
    g.setColour (colour.withAlpha (fade));
    g.drawLine ({ centre, centre.getPointOnCircumference (radius * 0.97f, needleAngle) }, 4.0f);
    g.restoreState();

    // Note name under the gauge.
    const auto noteRow = area.removeFromTop (area.getHeight() * 0.5f);
    const auto noteBox = noteRow.withSizeKeepingCentre (noteRow.getWidth() * 0.5f, noteRow.getHeight());
    if (hasNote)
    {
        const int pc = ((shownNote % 12) + 12) % 12;
        const juce::String name (noteNames[pc]);
        const int octave = shownNote / 12 - 1;

        g.setColour (colour.withAlpha (fade));
        g.setFont (juce::FontOptions (noteBox.getHeight() * 0.95f, juce::Font::bold));
        g.drawText (name, noteBox, juce::Justification::centred);

        g.setColour (text.withAlpha (0.55f * fade));
        g.setFont (juce::FontOptions (noteBox.getHeight() * 0.28f, juce::Font::bold));
        g.drawText (juce::String (octave), noteBox.withTrimmedLeft (noteBox.getWidth() * (name.length() > 1 ? 0.78f : 0.66f)).withTrimmedTop (noteBox.getHeight() * 0.55f),
                    juce::Justification::centredLeft);
    }
    else
    {
        g.setColour (dimText.withAlpha (0.5f));
        g.setFont (juce::FontOptions (noteBox.getHeight() * 0.5f, juce::Font::bold));
        g.drawText ("--", noteBox, juce::Justification::centred);
    }

    // Readouts.
    auto readout = area.removeFromTop (34.0f);
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.setColour (hasNote ? colour : dimText);
    g.drawText (hasNote ? (shownCents >= 0 ? "+" : "") + juce::String (shownCents, 1) + " cents" : "play a single note",
                readout, juce::Justification::centred);
    g.setColour (dimText);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (hasNote ? juce::String (shownHz, 1) + " Hz" : juce::String(), area.removeFromTop (18.0f), juce::Justification::centred);

    // Strobe band: stripes drift left when flat, right when sharp, and stand still in tune.
    area.removeFromTop (12.0f);
    auto strobe = area.removeFromTop (26.0f).reduced (area.getWidth() * 0.12f, 0.0f);
    g.setColour (juce::Colour (0xff0d0c0b));
    g.fillRoundedRectangle (strobe, 6.0f);
    g.saveState();
    g.reduceClipRegion (strobe.toNearestInt());
    const float stripe = 22.0f;
    const float offset = std::fmod (strobePhase, stripe * 2.0f);
    g.setColour (colour.withAlpha (hasNote ? 0.8f : 0.15f));
    for (float x = strobe.getX() - stripe * 2.0f + offset; x < strobe.getRight(); x += stripe * 2.0f)
        g.fillRect (x, strobe.getY(), stripe, strobe.getHeight());
    g.restoreState();
}

void TunerPanel::resized()
{
    // Short hosts (the studio's rig pane) get slimmer margins so the gauge keeps its size.
    const int margin = getHeight() < 560 ? 20 : 60;
    panelBounds = getLocalBounds().withSizeKeepingCentre (juce::jmin (760, getWidth() - 80), juce::jmin (540, getHeight() - margin));

    auto bottom = panelBounds.reduced (40, 24).removeFromBottom (32);
    closeButton.setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (12);
    reference.setBounds (bottom.removeFromRight (140));
    muteToggle.setBounds (bottom.removeFromLeft (240));
}

bool TunerPanel::keyPressed (const juce::KeyPress& key)
{
    if ((key == juce::KeyPress::escapeKey || key.getTextCharacter() == 't' || key.getTextCharacter() == 'T') && onClose)
    {
        onClose();
        return true;
    }

    return false;
}

void TunerPanel::mouseDown (const juce::MouseEvent& e)
{
    if (! panelBounds.contains (e.getPosition()) && onClose)
        onClose();
}
