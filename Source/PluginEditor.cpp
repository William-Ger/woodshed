#include "PluginEditor.h"

#if JucePlugin_Build_Standalone
 #include <juce_audio_utils/juce_audio_utils.h>
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace woodshed
{

namespace palette
{
    const juce::Colour board     { 0xff1c1a17 };
    const juce::Colour boardRib  { 0xff23211d };
    const juce::Colour topBar    { 0xff12110f };
    const juce::Colour text      { 0xffe9e2d4 };
    const juce::Colour dimText   { 0xff8d857a };
    const juce::Colour accent    { 0xffe8a33d };
    const juce::Colour ledOn     { 0xffff4a36 };
    const juce::Colour cable     { 0xff0a0a09 };
}

//==============================================================================
BoardLookAndFeel::BoardLookAndFeel()
{
    setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff24221f));
    setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff3a3631));
    setColour (juce::ComboBox::textColourId, palette::text);
    setColour (juce::ComboBox::arrowColourId, palette::accent);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff24221f));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, palette::accent.withAlpha (0.25f));
    setColour (juce::TextButton::buttonColourId, juce::Colour (0xff24221f));
    setColour (juce::TextButton::textColourOffId, palette::text);
    setColour (juce::Label::textColourId, palette::text);
    setColour (juce::BubbleComponent::backgroundColourId, juce::Colour (0xf0121110));
    setColour (juce::BubbleComponent::outlineColourId, palette::accent);
    setColour (juce::TooltipWindow::textColourId, palette::text);
}

void BoardLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                         float startAngle, float endAngle, juce::Slider& slider)
{
    const auto ink = slider.findColour (juce::Slider::rotarySliderOutlineColourId);
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (4.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);

    // Tick marks printed on the enclosure.
    g.setColour (ink.withAlpha (0.55f));
    for (int i = 0; i <= 10; ++i)
    {
        const float a = startAngle + (float) i / 10.0f * (endAngle - startAngle);
        const auto p1 = centre.getPointOnCircumference (radius * 0.86f, a);
        const auto p2 = centre.getPointOnCircumference (radius * (i % 5 == 0 ? 1.0f : 0.94f), a);
        g.drawLine ({ p1, p2 }, i % 5 == 0 ? 1.6f : 1.0f);
    }

    // Skirt + cap.
    const float skirt = radius * 0.76f, cap = radius * 0.56f;
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillEllipse (juce::Rectangle<float> (skirt * 2, skirt * 2).withCentre (centre.translated (0, 2.5f)));

    juce::ColourGradient skirtGrad (juce::Colour (0xff2b2926), centre.x, centre.y - skirt,
                                    juce::Colour (0xff0e0d0c), centre.x, centre.y + skirt, false);
    g.setGradientFill (skirtGrad);
    g.fillEllipse (juce::Rectangle<float> (skirt * 2, skirt * 2).withCentre (centre));

    juce::ColourGradient capGrad (juce::Colour (0xff3d3a36), centre.x - cap, centre.y - cap,
                                  juce::Colour (0xff151413), centre.x + cap, centre.y + cap, false);
    g.setGradientFill (capGrad);
    g.fillEllipse (juce::Rectangle<float> (cap * 2, cap * 2).withCentre (centre));

    // Pointer.
    g.setColour (slider.isMouseOverOrDragging() ? palette::accent : juce::Colour (0xfff2ece0));
    g.drawLine ({ centre.getPointOnCircumference (cap * 0.25f, angle),
                  centre.getPointOnCircumference (skirt * 0.95f, angle) }, 2.6f);
}

void BoardLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down)
{
    // A stomp footswitch: knurled ring, domed metal cap.
    const auto b = button.getLocalBounds().toFloat();
    const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f - 2.0f;
    const auto c = b.getCentre().translated (0, down ? 1.5f : 0.0f);

    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (b.getCentre().translated (0, 3)));

    juce::ColourGradient ring (juce::Colour (0xffbdbab4), c.x, c.y - r, juce::Colour (0xff55524d), c.x, c.y + r, false);
    g.setGradientFill (ring);
    g.fillEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (c));

    g.setColour (juce::Colour (0xff3d3b38));
    for (int i = 0; i < 36; ++i)
    {
        const float a = (float) i / 36.0f * juce::MathConstants<float>::twoPi;
        g.drawLine ({ c.getPointOnCircumference (r * 0.82f, a), c.getPointOnCircumference (r * 0.97f, a) }, 1.0f);
    }

    const float dome = r * 0.66f;
    juce::ColourGradient domeGrad (highlighted ? juce::Colour (0xfff5f3ee) : juce::Colour (0xffe3e0da),
                                   c.x - dome * 0.4f, c.y - dome * 0.6f,
                                   juce::Colour (0xff7a7670), c.x + dome * 0.5f, c.y + dome, true);
    g.setGradientFill (domeGrad);
    g.fillEllipse (juce::Rectangle<float> (dome * 2, dome * 2).withCentre (c));
}

juce::Label* BoardLookAndFeel::createSliderTextBox (juce::Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setColour (juce::Label::textColourId, palette::text);
    return l;
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& label)
    : attachment (state, paramId, slider)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setPopupDisplayEnabled (true, true, nullptr, 900);
    slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
    slider.setDoubleClickReturnValue (true, state.getParameter (paramId)->convertFrom0to1 (state.getParameter (paramId)->getDefaultValue()));
    slider.setMouseDragSensitivity (220);
    addAndMakeVisible (slider);

    caption.setText (label.toUpperCase(), juce::dontSendNotification);
    caption.setJustificationType (juce::Justification::centred);
    caption.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    caption.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (caption);
}

void Knob::resized()
{
    auto r = getLocalBounds();
    caption.setBounds (r.removeFromBottom (16));
    slider.setBounds (r);
}

//==============================================================================
void Meter::setLevel (float linearPeak)
{
    const float db = juce::Decibels::gainToDecibels (linearPeak, -60.0f);
    const float target = juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f);
    level = target > level ? target : level * 0.86f;

    if (target >= hold) { hold = target; holdFrames = 45; }
    else if (--holdFrames <= 0) hold = juce::jmax (0.0f, hold - 0.02f);

    repaint();
}

void Meter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff0b0a09));
    g.fillRoundedRectangle (r, 3.0f);

    const int segments = 24;
    const float gap = 2.0f;
    const float segW = (r.getWidth() - gap * (float) (segments + 1)) / (float) segments;

    for (int i = 0; i < segments; ++i)
    {
        const float t = (float) i / (float) (segments - 1);
        const auto colour = t > 0.92f ? juce::Colour (0xffff4a36) : t > 0.75f ? palette::accent : juce::Colour (0xff7fbf5a);
        const bool lit = t <= level || std::abs (t - hold) < 0.5f / (float) segments;
        g.setColour (lit ? colour : colour.withAlpha (0.12f));
        g.fillRect (r.getX() + gap + (float) i * (segW + gap), r.getY() + 3.0f, segW, r.getHeight() - 6.0f);
    }
}

//==============================================================================
Pedal::Pedal (juce::AudioProcessorValueTreeState& state, const juce::String& pedalId, PedalStyle s,
              std::vector<std::pair<juce::String, juce::String>> knobSpecs, int key)
    : style (std::move (s)), hotkey (key),
      footswitchAttachment (state, pedalId + "_on", footswitch)
{
    for (auto& [paramId, label] : knobSpecs)
    {
        auto* k = knobs.add (new Knob (state, paramId, label));
        k->slider.setColour (juce::Slider::rotarySliderOutlineColourId, style.ink);
        k->setColour (juce::Label::textColourId, style.ink);
        addAndMakeVisible (k);
    }

    for (auto* k : knobs)
        for (auto* child : k->getChildren())
            if (auto* l = dynamic_cast<juce::Label*> (child))
                l->setColour (juce::Label::textColourId, style.ink);

    footswitch.setTooltip ("Click or press " + juce::String (hotkey) + " to stomp");
    footswitch.onStateChange = [this] { repaint(); };
    addAndMakeVisible (footswitch);
}

void Pedal::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);

    // Drop shadow on the board.
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRoundedRectangle (r.translated (0, 6), 14.0f);

    juce::ColourGradient body (style.body.brighter (0.18f), r.getX(), r.getY(),
                               style.body.darker (0.25f), r.getX(), r.getBottom(), false);
    g.setGradientFill (body);
    g.fillRoundedRectangle (r, 14.0f);

    g.setColour (juce::Colours::white.withAlpha (0.14f));
    g.drawRoundedRectangle (r.reduced (1.0f), 13.0f, 1.2f);

    // Screws.
    for (auto p : { r.getTopLeft() + juce::Point<float> (13, 13), r.getTopRight() + juce::Point<float> (-13, 13),
                    r.getBottomLeft() + juce::Point<float> (13, -13), r.getBottomRight() + juce::Point<float> (-13, -13) })
    {
        g.setColour (juce::Colour (0xff9a968f));
        g.fillEllipse (juce::Rectangle<float> (8, 8).withCentre (p));
        g.setColour (juce::Colour (0xff4a4743));
        g.drawLine ({ p.translated (-2.5f, -2.5f), p.translated (2.5f, 2.5f) }, 1.2f);
    }

    // LED.
    const bool on = footswitch.getToggleState();
    const auto led = juce::Rectangle<float> (12, 12).withCentre ({ r.getCentreX(), r.getY() + r.getHeight() * 0.555f });
    if (on)
    {
        g.setColour (palette::ledOn.withAlpha (0.35f));
        g.fillEllipse (led.expanded (7.0f));
    }
    g.setColour (on ? palette::ledOn : juce::Colour (0xff3a1612));
    g.fillEllipse (led);
    g.setColour (juce::Colours::white.withAlpha (on ? 0.7f : 0.15f));
    g.fillEllipse (led.reduced (3.5f).translated (-1.2f, -1.2f));

    // Name plate.
    auto plate = r.withTrimmedTop (r.getHeight() * 0.6f).withHeight (58.0f);
    g.setColour (style.ink);
    g.setFont (juce::FontOptions (30.0f, juce::Font::bold).withKerningFactor (0.12f));
    g.drawText (style.name, plate.removeFromTop (34.0f), juce::Justification::centred);
    g.setColour (style.ink.withAlpha (0.7f));
    g.setFont (juce::FontOptions (11.0f).withKerningFactor (0.18f));
    g.drawText (style.subtitle.toUpperCase(), plate.removeFromTop (16.0f), juce::Justification::centred);

    g.setColour (style.ink.withAlpha (0.45f));
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.drawText (juce::String (hotkey), r.withTrimmedBottom (8.0f).removeFromBottom (14.0f), juce::Justification::centred);
}

void Pedal::resized()
{
    auto r = getLocalBounds().reduced (16);
    auto knobArea = r.removeFromTop ((int) ((float) getHeight() * 0.44f));

    if (knobs.size() == 3)
    {
        // Two up top, one centred below: the classic three-knob layout.
        auto top = knobArea.removeFromTop (knobArea.getHeight() / 2);
        knobs[0]->setBounds (top.removeFromLeft (top.getWidth() / 2).reduced (6, 0));
        knobs[1]->setBounds (top.reduced (6, 0));
        knobs[2]->setBounds (knobArea.withSizeKeepingCentre (top.getWidth(), knobArea.getHeight()).reduced (6, 0));
    }
    else
    {
        const int w = knobArea.getWidth() / juce::jmax (1, knobs.size());
        for (auto* k : knobs)
            k->setBounds (knobArea.removeFromLeft (w).withSizeKeepingCentre (w, juce::jmin (knobArea.getHeight(), w + 20)).reduced (4, 0));
    }

    const int fs = 74;
    footswitch.setBounds (getLocalBounds().getCentreX() - fs / 2, getHeight() - fs - 26, fs, fs);
}

} // namespace woodshed

//==============================================================================
WoodshedEditor::WoodshedEditor (WoodshedProcessor& p)
    : AudioProcessorEditor (p), woodshedProcessor (p),
      inputAttachment (p.params, "input", inputChoice),
      inputGain (p.params, "input_gain", "Input"),
      master (p.params, "master", "Master")
{
    using namespace woodshed;
    setLookAndFeel (&lookAndFeel);

    inputChoice.addItemList (p.params.getParameter ("input")->getAllValueStrings(), 1);
    inputChoice.setSelectedItemIndex ((int) p.params.getRawParameterValue ("input")->load(), juce::dontSendNotification);
    inputChoice.setTooltip ("Scarlett Solo: Input 2 is the instrument (guitar) jack");
    addAndMakeVisible (inputChoice);

    for (auto* k : { &inputGain, &master })
    {
        k->slider.setColour (juce::Slider::rotarySliderOutlineColourId, palette::dimText);
        addAndMakeVisible (k);
    }

    addAndMakeVisible (inputMeter);
    addAndMakeVisible (outputMeter);

    latencyLabel.setFont (juce::FontOptions (13.0f));
    latencyLabel.setMinimumHorizontalScale (0.7f);
    latencyLabel.setTooltip ("Round-trip latency  ·  buffer size @ sample rate");
    latencyLabel.setJustificationType (juce::Justification::centredRight);
    latencyLabel.setColour (juce::Label::textColourId, palette::dimText);
    addAndMakeVisible (latencyLabel);

    statusLabel.setFont (juce::FontOptions (12.0f));
    statusLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setColour (juce::Label::textColourId, palette::accent);
    addAndMakeVisible (statusLabel);

   #if JucePlugin_Build_Standalone
    if (juce::JUCEApplicationBase::isStandaloneApp())
    {
        audioSetupButton.onClick = []
        {
            if (auto* holder = juce::StandalonePluginHolder::getInstance())
                holder->showAudioSettingsDialog();
        };
        addAndMakeVisible (audioSetupButton);
    }
   #endif

    captureButton.setTooltip ("Records 10 seconds of dry input and processed output to a WAV file");
    captureButton.onClick = [this] { startCapture (10.0); };
    addAndMakeVisible (captureButton);

    pedals.add (new Pedal (p.params, "gate",  { "HUSH", "noise gate",   juce::Colour (0xff3e4c55), juce::Colour (0xffe8eee9) },
                           { { "gate_threshold", "Threshold" }, { "gate_release", "Release" } }, 1));
    pedals.add (new Pedal (p.params, "drive", { "GRIT", "overdrive",    juce::Colour (0xffc4582b), juce::Colour (0xff1d1310) },
                           { { "drive_gain", "Drive" }, { "drive_level", "Level" }, { "drive_tone", "Tone" } }, 2));
    pedals.add (new Pedal (p.params, "delay", { "ECHO", "analog delay", juce::Colour (0xff27506b), juce::Colour (0xfff1e9d8) },
                           { { "delay_time", "Time" }, { "delay_feedback", "Repeats" }, { "delay_mix", "Mix" } }, 3));
    pedals.add (new Pedal (p.params, "room",  { "ROOM", "reverb",       juce::Colour (0xffd8cdb4), juce::Colour (0xff2a2520) },
                           { { "room_size", "Size" }, { "room_damp", "Damp" }, { "room_mix", "Mix" } }, 4));

    for (auto* pedal : pedals)
        addAndMakeVisible (pedal);

    captureDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                    #if JUCE_MAC
                     .getChildFile ("Application Support")
               #endif
                     .getChildFile ("Woodshed").getChildFile ("captures");
    captureRequestFile = captureDir.getChildFile ("capture.request");

    setWantsKeyboardFocus (true);
    setSize (1120, 640);
    startTimerHz (30);
}

WoodshedEditor::~WoodshedEditor()
{
    setLookAndFeel (nullptr);
}

void WoodshedEditor::paint (juce::Graphics& g)
{
    using namespace woodshed;
    auto r = getLocalBounds();

    g.fillAll (palette::board);

    // Rubber board ribs.
    g.setColour (palette::boardRib);
    for (int y = 80; y < getHeight(); y += 14)
        g.fillRect (0, y, getWidth(), 6);

    auto bar = r.removeFromTop (72);
    g.setColour (palette::topBar);
    g.fillRect (bar);
    g.setColour (juce::Colour (0xff2c2925));
    g.fillRect (bar.removeFromBottom (1));

    g.setColour (palette::text);
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold).withKerningFactor (0.22f));
    g.drawText ("WOODSHED", 24, 0, 220, 72, juce::Justification::centredLeft);

    g.setColour (palette::dimText);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold).withKerningFactor (0.15f));
    g.drawText ("IN", inputMeter.getBounds().withY (8).withHeight (14), juce::Justification::centredLeft);
    g.drawText ("OUT", outputMeter.getBounds().withY (8).withHeight (14), juce::Justification::centredLeft);

    // Patch cable running through the chain, guitar in on the left, amp out on the right.
    if (! pedals.isEmpty())
    {
        const float y = (float) pedals[0]->getBounds().getCentreY() + 40.0f;
        g.setColour (palette::cable);
        g.drawLine (8.0f, y, (float) getWidth() - 8.0f, y, 7.0f);
        g.setColour (juce::Colour (0xff2a2724));
        g.drawLine (8.0f, y - 2.0f, (float) getWidth() - 8.0f, y - 2.0f, 1.5f);

        g.setColour (palette::dimText);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold).withKerningFactor (0.2f));
        g.drawText ("IN", 8, (int) y + 8, 30, 14, juce::Justification::centredLeft);
        g.drawText ("OUT", getWidth() - 38, (int) y + 8, 30, 14, juce::Justification::centredRight);
    }
}

void WoodshedEditor::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop (72).reduced (16, 0);

    bar.removeFromLeft (220);
    inputChoice.setBounds (bar.removeFromLeft (110).withSizeKeepingCentre (110, 28));
    bar.removeFromLeft (10);
    inputGain.setBounds (bar.removeFromLeft (60).withSizeKeepingCentre (60, 64));
    bar.removeFromLeft (6);
    inputMeter.setBounds (bar.removeFromLeft (150).withTrimmedTop (24).withHeight (22));

    master.setBounds (bar.removeFromRight (60).withSizeKeepingCentre (60, 64));
    bar.removeFromRight (6);
    outputMeter.setBounds (bar.removeFromRight (150).withTrimmedTop (24).withHeight (22));
    bar.removeFromRight (14);

    captureButton.setBounds (bar.removeFromRight (96).withSizeKeepingCentre (96, 28));
    bar.removeFromRight (8);
    if (audioSetupButton.isVisible())
    {
        audioSetupButton.setBounds (bar.removeFromRight (104).withSizeKeepingCentre (104, 28));
        bar.removeFromRight (8);
    }

    latencyLabel.setBounds (bar.withTrimmedBottom (30).withTrimmedTop (12));
    statusLabel.setBounds (bar.withTrimmedTop (40).withHeight (20));

    auto board = r.reduced (40, 36);
    const int gap = 28;
    const int pedalW = (board.getWidth() - gap * (pedals.size() - 1)) / pedals.size();

    for (auto* pedal : pedals)
    {
        pedal->setBounds (board.removeFromLeft (pedalW));
        board.removeFromLeft (gap);
    }
}

bool WoodshedEditor::keyPressed (const juce::KeyPress& key)
{
    const int index = key.getKeyCode() - '1';

    if (juce::isPositiveAndBelow (index, pedals.size()))
    {
        pedals[index]->toggle();
        return true;
    }

    return false;
}

void WoodshedEditor::startCapture (double seconds)
{
    woodshedProcessor.armCapture (seconds);
    pendingCaptureName = "capture-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".wav";
    statusLabel.setText ("Recording " + juce::String ((int) seconds) + "s... play something", juce::dontSendNotification);
    statusFrames = -1;
}

void WoodshedEditor::updateLatencyText()
{
    juce::String text = "Running inside a host: the host sets latency";

   #if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
    {
        if (auto* device = holder->deviceManager.getCurrentAudioDevice())
        {
            const double rate = device->getCurrentSampleRate();
            const int total = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples()
                            + woodshedProcessor.getLatencySamples();
            text = juce::String (1000.0 * total / rate, 1) + " ms  ·  "
                 + juce::String (device->getCurrentBufferSizeSamples()) + " @ "
                 + juce::String (rate / 1000.0, 1) + "k";

            if (const int xruns = device->getXRunCount(); xruns > 0)
                text << "  ·  " << xruns << " dropouts";
        }
        else
        {
            text = "No audio device: open Audio Setup";
        }
    }
   #endif

    latencyLabel.setText (text, juce::dontSendNotification);
}

void WoodshedEditor::timerCallback()
{
    inputMeter.setLevel (woodshedProcessor.inputPeak.exchange (0.0f));
    outputMeter.setLevel (woodshedProcessor.outputPeak.exchange (0.0f));

    static int frame = 0;
    if (++frame % 15 == 0)
    {
        updateLatencyText();

        // A dropped-in request file arms a capture (lets a script trigger one while you play).
        if (captureRequestFile.existsAsFile() && ! woodshedProcessor.captureRunning())
        {
            const double requested = captureRequestFile.loadFileAsString().trim().getDoubleValue();
            captureRequestFile.deleteFile();
            startCapture (requested > 0.0 ? juce::jmin (requested, 30.0) : 10.0);
        }
    }

    if (woodshedProcessor.captureFinished())
    {
        const auto file = captureDir.getChildFile (pendingCaptureName.isEmpty() ? "capture.wav" : pendingCaptureName);
        const bool ok = woodshedProcessor.writeCapture (file);
        statusLabel.setText (ok ? "Saved " + file.getFileName() : "Capture failed", juce::dontSendNotification);
        statusFrames = 150;
    }

    if (statusFrames > 0 && --statusFrames == 0)
        statusLabel.setText ({}, juce::dontSendNotification);
}
