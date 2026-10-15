#include "PluginEditor.h"
#include "Art.h"
// Colour constants only (header-only): the plug-in build has no studio fonts or drawing helpers,
// so nothing here may call theme::font() and friends. Studio mode uses these to match the app.
#include "app/Theme.h"

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
    const juce::Colour panel     { 0xff1a1816 };
    const juce::Colour text      { 0xffe9e2d4 };
    const juce::Colour dimText   { 0xff8d857a };
    const juce::Colour accent    { 0xffe8a33d };
    const juce::Colour ledOn     { 0xffff4a36 };
    const juce::Colour cable     { 0xff0a0a09 };
    const juce::Colour tolex     { 0xff1f1d1b };
    const juce::Colour piping    { 0xffc9a45c };
}

//==============================================================================
BoardLookAndFeel::BoardLookAndFeel()
{
    setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff24221f));
    setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff3a3631));
    setColour (juce::ComboBox::textColourId, palette::text);
    setColour (juce::ComboBox::arrowColourId, palette::accent);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff24221f));
    setColour (juce::PopupMenu::textColourId, palette::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, palette::accent.withAlpha (0.25f));
    setColour (juce::TextButton::buttonColourId, juce::Colour (0xff24221f));
    setColour (juce::TextButton::buttonOnColourId, palette::accent);
    setColour (juce::TextButton::textColourOffId, palette::text);
    setColour (juce::TextButton::textColourOnId, juce::Colour (0xff1a1410));
    setColour (juce::Label::textColourId, palette::text);
    setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff24221f));
    setColour (juce::TextEditor::textColourId, palette::text);
    setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff3a3631));
    setColour (juce::TextEditor::focusedOutlineColourId, palette::accent);
    setColour (juce::CaretComponent::caretColourId, palette::accent);
    setColour (juce::BubbleComponent::backgroundColourId, juce::Colour (0xf0121110));
    setColour (juce::BubbleComponent::outlineColourId, palette::accent);
    setColour (juce::TooltipWindow::textColourId, palette::text);
    setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff4a453f));
    setColour (juce::AlertWindow::backgroundColourId, juce::Colour (0xff1f1d1a));
    setColour (juce::AlertWindow::textColourId, palette::text);
}

void BoardLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                         float startAngle, float endAngle, juce::Slider& slider)
{
    auto& art = Art::get();
    const auto ink = slider.findColour (juce::Slider::rotarySliderOutlineColourId);
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (2.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);

    // Scale printed on the enclosure; stepped knobs get one tick per position.
    const auto interval = slider.getNormalisableRange().interval;
    const int ticks = interval > 0.0 ? juce::roundToInt (1.0 / interval) : 10;

    g.setColour (ink.withAlpha (0.6f));
    for (int i = 0; i <= ticks; ++i)
    {
        const float a = startAngle + (float) i / (float) ticks * (endAngle - startAngle);
        const bool major = interval > 0.0 || i % 5 == 0;
        g.drawLine ({ centre.getPointOnCircumference (radius * 0.88f, a),
                      centre.getPointOnCircumference (radius * (major ? 1.0f : 0.95f), a) }, major ? 1.5f : 1.0f);
    }

    // The rendered knob, sized so its skirt sits just inside the scale.
    const float skirt = radius * 0.9f;
    const float imageSize = skirt / art.knobSkirt;
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (art.knob, juce::Rectangle<float> (imageSize, imageSize).withCentre (centre));

    // Indicator line across the fluted body.
    const float body = imageSize * art.knobBody, cap = imageSize * art.knobCap;
    g.setColour (slider.isMouseOverOrDragging() ? palette::accent : juce::Colour (0xfff4efe4));
    g.drawLine ({ centre.getPointOnCircumference (cap * 0.35f, angle),
                  centre.getPointOnCircumference (body * 0.96f, angle) }, juce::jmax (1.6f, radius * 0.075f));
}

void BoardLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down)
{
    auto& art = Art::get();
    const auto b = button.getLocalBounds().toFloat();
    const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f * (down ? 0.97f : 1.0f);
    const float imageSize = r / art.footswitchRadius;

    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.setOpacity (1.0f);
    g.drawImage (art.footswitch, juce::Rectangle<float> (imageSize, imageSize).withCentre (b.getCentre().translated (0, down ? 1.0f : 0.0f)));

    if (down || highlighted)
    {
        g.setColour ((down ? juce::Colours::black : juce::Colours::white).withAlpha (down ? 0.18f : 0.06f));
        g.fillEllipse (juce::Rectangle<float> (r * 1.3f, r * 1.3f).withCentre (b.getCentre()));
    }
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const KnobSpec& s, juce::Colour ink)
    : spec (s), attachment (state, paramId, slider)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setPopupDisplayEnabled (true, true, nullptr, 900);
    slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
    slider.setMouseDragSensitivity (220);
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, ink);

    if (spec.isChoice())
        slider.setNormalisableRange ({ 0.0, 1.0, 1.0 / (double) juce::jmax (1, spec.choices.size() - 1) });

    // Show real units rather than the 0..1 the parameter stores.
    slider.textFromValueFunction = [this] (double v) { return spec.format (spec.toReal ((float) v)); };
    slider.setDoubleClickReturnValue (true, spec.defaultNorm());
    slider.onValueChange = [this] { updateValueText(); };
    addAndMakeVisible (slider);

    caption.setText (spec.label.toUpperCase(), juce::dontSendNotification);
    caption.setJustificationType (juce::Justification::centred);
    caption.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    caption.setColour (juce::Label::textColourId, ink);
    caption.setInterceptsMouseClicks (false, false);
    caption.setMinimumHorizontalScale (0.45f);
    caption.setBorderSize ({});
    addAndMakeVisible (caption);

    valueText.setJustificationType (juce::Justification::centred);
    valueText.setFont (juce::FontOptions (10.0f));
    valueText.setColour (juce::Label::textColourId, ink.withAlpha (0.75f));
    valueText.setInterceptsMouseClicks (false, false);
    valueText.setMinimumHorizontalScale (0.5f);
    valueText.setBorderSize ({});
    addChildComponent (valueText);
    valueText.setVisible (spec.isChoice());
    updateValueText();
}

void Knob::updateValueText()
{
    if (spec.isChoice())
        valueText.setText (spec.format (spec.toReal ((float) slider.getValue())), juce::dontSendNotification);
}

void Knob::resized()
{
    // Captions shrink with the knob so tightly packed pedals stay readable.
    const float fontSize = juce::jlimit (8.0f, 11.0f, (float) getWidth() * 0.16f);
    caption.setFont (juce::FontOptions (fontSize, juce::Font::bold));
    valueText.setFont (juce::FontOptions (fontSize - 0.5f));

    auto r = getLocalBounds();
    if (spec.isChoice())
        valueText.setBounds (r.removeFromBottom ((int) fontSize + 2));
    caption.setBounds (r.removeFromBottom ((int) fontSize + 4));

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

    if (slim)
    {
        const auto well = r.withSizeKeepingCentre (r.getWidth(), juce::jmin (4.0f, r.getHeight()));
        const float radius = well.getHeight() * 0.5f;
        g.setColour (juce::Colour (0xff111113));
        g.fillRoundedRectangle (well, radius);

        juce::ColourGradient ramp (theme::meterLow, well.getX(), 0.0f, theme::meterHigh, well.getRight(), 0.0f, false);
        ramp.addColour (0.72, theme::meterLow);
        ramp.addColour (0.86, theme::meterMid);
        g.setGradientFill (ramp);
        if (level > 0.01f)
            g.fillRoundedRectangle (well.withWidth (juce::jmax (well.getHeight(), well.getWidth() * level)), radius);
        if (hold > 0.02f)
            g.fillRect (well.getX() + (well.getWidth() - 2.0f) * hold, well.getY(), 2.0f, well.getHeight());
        return;
    }

    g.setColour (juce::Colour (0xff0b0a09));
    g.fillRoundedRectangle (r, 3.0f);

    const int segments = 20;
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
PedalView::PedalView (WoodshedProcessor& p, int s, const juce::String& t, bool canDrag)
    : processor (p), slot (s), type (Registry::get().find (t)), typeId (t), draggable (canDrag)
{
    removeButton.setTooltip ("Remove from board");
    removeButton.onClick = [this]
    {
        auto& proc = processor;
        const int position = slot;
        juce::MessageManager::callAsync ([&proc, position] { proc.removePedal (position); });
    };
    removeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2724));
    removeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.85f));
    addChildComponent (removeButton);
    removeButton.setVisible (draggable);

    if (draggable)
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);

    if (type == nullptr)
    {
        removeButton.setVisible (true);
        return;
    }

    for (size_t k = 0; k < type->info.knobs.size(); ++k)
        addAndMakeVisible (knobs.add (new Knob (processor.params, WoodshedProcessor::knobParamId (slot, (int) k),
                                                type->info.knobs[k], type->info.ink)));

    footswitchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.params, WoodshedProcessor::onParamId (slot), footswitch);
    footswitch.setTooltip (draggable ? "Click or press " + juce::String ((slot + 1) % 10) + " to stomp" : "Power");
    footswitch.onStateChange = [this] { repaint(); };
    addAndMakeVisible (footswitch);
}

juce::Rectangle<float> PedalView::bodyArea() const
{
    auto& art = Art::get();
    const auto all = getLocalBounds().toFloat();

    if (type != nullptr && type->info.category == "Amp")
        return Art::regionIn (all, art.ampRect);
    if (type != nullptr && type->info.category == "Cab")
        return Art::regionIn (all, art.cabRect);

    return Art::regionIn (all, art.enclosureBody);
}

juce::Rectangle<float> PedalView::controlArea() const
{
    auto& art = Art::get();
    const auto all = getLocalBounds().toFloat();

    if (type != nullptr && type->info.category == "Amp") return Art::regionIn (all, art.ampPanel).reduced (10.0f, 6.0f);
    if (type != nullptr && type->info.category == "Cab") return Art::regionIn (all, art.cabPanel).reduced (10.0f, 6.0f);

    auto body = bodyArea().reduced (bodyArea().getWidth() * 0.07f, bodyArea().getHeight() * 0.05f);
    return body.withHeight (body.getHeight() * 0.47f);
}

void PedalView::paint (juce::Graphics& g)
{
    auto& art = Art::get();
    const auto all = getLocalBounds().toFloat();
    const auto body = bodyArea();
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

    if (type == nullptr)
    {
        g.setColour (juce::Colour (0xff2a2826));
        g.fillRoundedRectangle (body, 10.0f);
        g.setColour (palette::dimText);
        g.setFont (juce::FontOptions (12.0f));
        g.drawFittedText ("Unknown pedal\n" + typeId, body.reduced (10).toNearestInt(), juce::Justification::centred, 3);
        return;
    }

    const auto& info = type->info;
    const bool on = footswitch.getToggleState();
    const bool isAmp = info.category == "Amp", isCab = info.category == "Cab";

    if (isAmp || isCab)
    {
        // Panel painted in the model's colour sits in the hole the body render leaves for it.
        g.drawImage (isAmp ? art.ampPanelIn (info.body) : art.cabPanelIn (info.body), all);
        g.drawImage (isAmp ? art.ampBody : art.cabBody, all);

        // Name badge on the grille cloth, like a script logo.
        const auto grille = Art::regionIn (all, isAmp ? art.ampGrille : art.cabGrille);
        const auto badge = grille.withSizeKeepingCentre (grille.getWidth() * 0.56f, juce::jmin (grille.getHeight(), 46.0f));
        const float byWidth = badge.getWidth() / ((float) juce::jmax (4, info.name.length()) * 0.82f);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 30.0f, juce::jmin (badge.getHeight() * 0.62f, byWidth)), juce::Font::bold).withKerningFactor (0.16f));
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawFittedText (info.name, badge.translated (1.0f, 2.0f).toNearestInt(), juce::Justification::centred, 1, 0.6f);
        g.setColour (palette::piping.brighter (0.25f));
        g.drawFittedText (info.name, badge.toNearestInt(), juce::Justification::centred, 1, 0.6f);

        // Pilot light.
        const auto pilot = juce::Rectangle<float> (18.0f, 18.0f).withCentre ({ grille.getX() + 22.0f, grille.getCentreY() });
        if (on)
        {
            g.setColour (palette::ledOn.withAlpha (0.3f));
            g.fillEllipse (pilot.expanded (8.0f));
        }
        g.drawImage (on ? art.ledOn : art.ledOff, pilot.expanded (pilot.getWidth() * 0.5f));
        return;
    }

    g.drawImage (art.enclosure (info.body), all);

    // LED in its chrome bezel.
    const float ledSize = juce::jlimit (8.0f, 22.0f, juce::jmin (body.getWidth() * 0.11f, body.getHeight() * 0.07f));
    const auto led = juce::Rectangle<float> (ledSize, ledSize).withCentre ({ body.getCentreX(), body.getY() + body.getHeight() * 0.53f });
    if (on)
    {
        juce::ColourGradient glow (palette::ledOn.withAlpha (0.45f), led.getCentreX(), led.getCentreY(),
                                   palette::ledOn.withAlpha (0.0f), led.getCentreX() + ledSize * 1.6f, led.getCentreY(), true);
        g.setGradientFill (glow);
        g.fillEllipse (led.expanded (ledSize * 1.6f));
    }
    g.drawImage (on ? art.ledOn : art.ledOff, led.expanded (ledSize * 0.5f));

    // Silkscreened name and subtitle.
    auto plate = body.withTrimmedTop (body.getHeight() * 0.585f).withHeight (body.getHeight() * 0.16f);
    const float nameSize = juce::jlimit (8.0f, 24.0f, body.getWidth() / (float) juce::jmax (4, info.name.length()) * 1.3f);
    g.setColour (info.ink);
    g.setFont (juce::FontOptions (nameSize, juce::Font::bold).withKerningFactor (0.1f));
    g.drawFittedText (info.name, plate.removeFromTop (plate.getHeight() * 0.64f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
    g.setColour (info.ink.withAlpha (0.7f));
    g.setFont (juce::FontOptions (juce::jlimit (6.0f, 10.0f, body.getWidth() * 0.055f)).withKerningFactor (0.14f));
    g.drawFittedText (info.subtitle.toUpperCase(), plate.toNearestInt(), juce::Justification::centred, 1, 0.7f);

    if (slot < 10)
    {
        g.setColour (info.ink.withAlpha (0.4f));
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText (juce::String ((slot + 1) % 10), body.withTrimmedBottom (body.getHeight() * 0.035f).removeFromBottom (12.0f), juce::Justification::centred);
    }
}

void PedalView::resized()
{
    const auto body = bodyArea().toNearestInt();
    // Hangs off the enclosure's top-right corner, clear of the knobs.
    removeButton.setBounds (body.getRight() - 9, body.getY() - 9, 18, 18);

    if (type == nullptr)
        return;

    const bool isAmp = type->info.category == "Amp", isCab = type->info.category == "Cab";
    auto knobArea = controlArea().toNearestInt();
    const int n = knobs.size();

    if (n > 0)
    {
        const int perRow = (isAmp || isCab) ? (n <= 4 ? n : (n + 1) / 2) : (n <= 2 ? n : n <= 4 ? 2 : n <= 6 ? 3 : 4);
        const int rows = (n + perRow - 1) / perRow;
        const int cellH = knobArea.getHeight() / rows;
        const int cellW = knobArea.getWidth() / perRow;

        for (int row = 0, i = 0; row < rows; ++row)
        {
            const int inRow = juce::jmin (perRow, n - i);
            auto line = knobArea.removeFromTop (cellH);
            line = line.withSizeKeepingCentre (cellW * inRow, cellH);

            for (int c = 0; c < inRow; ++c, ++i)
                knobs[i]->setBounds (line.removeFromLeft (cellW).reduced (2, 1));
        }
    }

    if (isAmp || isCab)
    {
        // A small power switch at the right end of the grille.
        const auto grille = Art::regionIn (getLocalBounds().toFloat(), isAmp ? Art::get().ampGrille : Art::get().cabGrille);
        const int s = juce::jlimit (22, 34, (int) (grille.getHeight() * 0.7f));
        footswitch.setBounds (juce::Rectangle<int> (s, s).withCentre ({ (int) grille.getRight() - s, (int) grille.getCentreY() }));
        return;
    }

    const int fs = juce::jlimit (18, 64, juce::jmin ((int) ((float) body.getWidth() * 0.36f), (int) ((float) body.getHeight() * 0.18f)));
    footswitch.setBounds (body.getCentreX() - fs / 2, body.getBottom() - fs - (int) ((float) body.getHeight() * 0.07f), fs, fs);
}

void PedalView::mouseDown (const juce::MouseEvent& e)
{
    if (draggable && e.eventComponent == this)
        dragger.startDraggingComponent (this, e);
}

void PedalView::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggable || e.eventComponent != this)
        return;

    if (! dragging && e.getDistanceFromDragStart() < 5)
        return;

    if (! dragging)
    {
        dragging = true;
        toFront (false);
    }

    dragger.dragComponent (this, e, nullptr);
    if (onDragMove) onDragMove (*this);
}

void PedalView::mouseUp (const juce::MouseEvent& e)
{
    if (e.eventComponent == this && dragging)
    {
        dragging = false;
        repaint();
        if (onDragEnd) onDragEnd (*this);
    }
}

//==============================================================================
BoardView::BoardView (WoodshedProcessor& p) : processor (p) {}

void BoardView::rebuild()
{
    pedals.clear();
    const auto types = processor.getBoard();
    spots = processor.getSpots();

    for (int slot = 0; slot < types.size(); ++slot)
    {
        if (spots[(size_t) slot] < 0)
            continue;

        auto* view = pedals.add (new PedalView (processor, slot, types[slot], true));

        view->onDragMove = [this] (PedalView& v)
        {
            const int target = spotAt (v.getBounds().getCentre());
            if (target != dragTarget) { dragTarget = target; repaint(); }
        };

        view->onDragEnd = [this] (PedalView& v)
        {
            const int target = dragTarget, from = v.getSlot();
            dragTarget = -1;

            if (target >= 0 && target != spots[(size_t) from])
            {
                auto& proc = processor;
                juce::MessageManager::callAsync ([&proc, from, target] { proc.movePedal (from, target); });
            }

            layoutPedals();   // snap into place now; the rebuild after the move confirms it
            repaint();
        };

        addAndMakeVisible (view);
    }

    layoutPedals();
    repaint();
}

juce::Array<PedalView*> BoardView::pedalsInChainOrder() const
{
    juce::Array<PedalView*> list;
    for (auto* p : pedals) list.add (p);
    std::sort (list.begin(), list.end(), [] (PedalView* a, PedalView* b) { return a->getSlot() < b->getSlot(); });
    return list;
}

juce::Rectangle<int> BoardView::spotBounds (int spot) const
{
    const int col = spot % WoodshedProcessor::boardColumns, row = spot / WoodshedProcessor::boardColumns;
    const auto rowArea = Art::regionIn (boardImage, Art::get().boardRows[(size_t) row]);
    const float colW = rowArea.getWidth() / (float) WoodshedProcessor::boardColumns;
    return rowArea.withX (rowArea.getX() + (float) col * colW).withWidth (colW).toNearestInt();
}

juce::Rectangle<float> BoardView::bodyBounds (int spot) const
{
    auto& art = Art::get();
    const auto cell = spotBounds (spot).toFloat();
    const float aspect = (art.enclosureBody.getWidth() * (float) art.enclosure (juce::Colours::white).getWidth())
                       / (art.enclosureBody.getHeight() * (float) art.enclosure (juce::Colours::white).getHeight());

    float h = cell.getHeight() * 1.04f, w = h * aspect;
    if (w > cell.getWidth() * 0.82f)
    {
        w = cell.getWidth() * 0.82f;
        h = w / aspect;
    }

    return juce::Rectangle<float> (w, h).withCentre (cell.getCentre());
}

juce::Rectangle<int> BoardView::pedalBounds (int spot) const
{
    return Art::imageBoundsFor (bodyBounds (spot), Art::get().enclosureBody).toNearestInt();
}

int BoardView::spotAt (juce::Point<int> pt) const
{
    for (int s = 0; s < WoodshedProcessor::boardSpots; ++s)
        if (spotBounds (s).expanded (4, 10).contains (pt))
            return s;

    return -1;
}

void BoardView::layoutPedals()
{
    for (auto* p : pedals)
        if (! p->isBeingDragged())
            p->setBounds (pedalBounds (spots[(size_t) p->getSlot()]));
}

void BoardView::resized()
{
    const auto& image = Art::get().board;
    boardImage = juce::RectanglePlacement (juce::RectanglePlacement::centred)
                     .appliedTo (image.getBounds().toFloat(), getLocalBounds().toFloat().reduced (2.0f));
    layoutPedals();
}

void BoardView::paint (juce::Graphics& g)
{
    auto& art = Art::get();
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (art.board, boardImage);

    std::array<bool, WoodshedProcessor::boardSpots> used {};
    for (auto s : spots)
        if (s >= 0) used[(size_t) s] = true;

    // Empty spots and the drop target.
    for (int s = 0; s < WoodshedProcessor::boardSpots; ++s)
    {
        const auto b = bodyBounds (s);

        if (s == dragTarget)
        {
            g.setColour (palette::accent.withAlpha (0.16f));
            g.fillRoundedRectangle (b, 10.0f);
            g.setColour (palette::accent);
            g.drawRoundedRectangle (b, 10.0f, 2.0f);
        }
        else if (! used[(size_t) s])
        {
            juce::Path outline, dashed;
            outline.addRoundedRectangle (b, 10.0f);
            const float dashes[] { 6.0f, 5.0f };
            juce::PathStrokeType (1.5f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (juce::Colours::white.withAlpha (s == hoverSpot ? 0.5f : 0.2f));
            g.fillPath (dashed);
            g.setFont (juce::FontOptions (s == hoverSpot ? 34.0f : 28.0f));
            g.drawText ("+", b, juce::Justification::centred);
        }
    }

    // Patch cables in chain order: guitar -> pedals -> amp, plugged into the side jacks.
    auto jack = [this, &art] (int spot, bool output)
    {
        const auto b = bodyBounds (spot);
        const float y = b.getY() + b.getHeight() * art.jackY;
        return output ? juce::Point<float> (b.getRight() + b.getWidth() * 0.075f, y)
                      : juce::Point<float> (b.getX() - b.getWidth() * 0.075f, y);
    };

    auto plug = [&g] (juce::Point<float> at, float direction)
    {
        // Right-angle plug: nickel barrel into the jack, black boot behind it.
        const auto barrel = juce::Rectangle<float> (9.0f, 9.0f).withCentre (at.translated (-direction * 3.0f, 0.0f));
        const auto boot = juce::Rectangle<float> (12.0f, 15.0f).withCentre (at.translated (direction * 6.0f, 0.0f));
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (boot.translated (2.0f, 3.0f), 3.5f);
        juce::ColourGradient metal (juce::Colour (0xffd6d2ca), barrel.getX(), barrel.getY(), juce::Colour (0xff6e6a64), barrel.getX(), barrel.getBottom(), false);
        g.setGradientFill (metal);
        g.fillRoundedRectangle (barrel, 2.0f);
        juce::ColourGradient rubber (juce::Colour (0xff3a3a3e), boot.getX(), boot.getY(), juce::Colour (0xff0c0c0d), boot.getX(), boot.getBottom(), false);
        g.setGradientFill (rubber);
        g.fillRoundedRectangle (boot, 3.5f);
    };

    auto cable = [&] (juce::Point<float> a, juce::Point<float> b, juce::Colour colour)
    {
        const auto a2 = a.translated (12.0f, 0.0f), b2 = b.translated (-12.0f, 0.0f);
        juce::Path p;
        p.startNewSubPath (a2);
        const float span = a2.getDistanceFrom (b2);
        const float pull = juce::jlimit (8.0f, 140.0f, std::abs (b2.x - a2.x) * 0.45f + 20.0f);
        const float sag = juce::jlimit (4.0f, 40.0f, span * 0.12f);
        p.cubicTo (a2.translated (pull, sag), b2.translated (-pull, sag), b2);

        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.strokePath (p, juce::PathStrokeType (8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), juce::AffineTransform::translation (3.0f, 5.0f));
        g.setColour (colour);
        g.strokePath (p, juce::PathStrokeType (6.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (colour.brighter (0.9f).withAlpha (0.35f));
        g.strokePath (p, juce::PathStrokeType (1.6f), juce::AffineTransform::translation (-0.8f, -1.6f));
        plug (a.translated (6.0f, 0.0f), 1.0f);
        plug (b.translated (-6.0f, 0.0f), -1.0f);
    };

    std::vector<int> chainSpots;
    for (auto* p : pedalsInChainOrder())
        chainSpots.push_back (spots[(size_t) p->getSlot()]);

    const auto frame = Art::regionIn (boardImage, art.boardFrame);
    const auto cableColour = juce::Colour (0xff222226);
    const auto guitarIn = juce::Point<float> (frame.getX() - 30.0f, chainSpots.empty() ? frame.getCentreY() : jack (chainSpots.front(), false).y);
    const auto toAmp = juce::Point<float> ((float) getWidth() + 30.0f, chainSpots.empty() ? frame.getCentreY() : jack (chainSpots.back(), true).y);

    if (chainSpots.empty())
    {
        cable (guitarIn, toAmp, cableColour);
    }
    else
    {
        cable (guitarIn, jack (chainSpots.front(), false), juce::Colour (0xff2c2c30));
        for (size_t i = 1; i < chainSpots.size(); ++i)
            cable (jack (chainSpots[i - 1], true), jack (chainSpots[i], false), cableColour);
        cable (jack (chainSpots.back(), true), toAmp, juce::Colour (0xff2c2c30));
    }
}

void BoardView::mouseMove (const juce::MouseEvent& e)
{
    const int s = e.eventComponent == this ? spotAt (e.getPosition()) : -1;
    const bool empty = s >= 0 && std::find (spots.begin(), spots.end(), s) == spots.end();
    const int hover = empty ? s : -1;

    if (hover != hoverSpot)
    {
        hoverSpot = hover;
        setMouseCursor (hover >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void BoardView::mouseExit (const juce::MouseEvent&)
{
    if (hoverSpot >= 0) { hoverSpot = -1; repaint(); }
}

void BoardView::mouseUp (const juce::MouseEvent& e)
{
    if (e.eventComponent != this || ! e.mouseWasClicked())
        return;

    const int s = spotAt (e.getPosition());
    if (s >= 0 && std::find (spots.begin(), spots.end(), s) == spots.end() && onEmptySpotClicked)
        onEmptySpotClicked (s);
}

//==============================================================================
RigStack::RigStack (WoodshedProcessor& p) : processor (p)
{
    chooseAmp.onClick = [this] { if (onChoose) onChoose ("Amp"); };
    chooseCab.onClick = [this] { if (onChoose) onChoose ("Cab"); };
    swapAmp.onClick = chooseAmp.onClick;
    swapCab.onClick = chooseCab.onClick;

    for (auto* b : { &swapAmp, &swapCab })
        b->setTooltip ("Pick a different model");

    addChildComponent (chooseAmp);
    addChildComponent (chooseCab);
    addChildComponent (swapAmp);
    addChildComponent (swapCab);
}

void RigStack::rebuild()
{
    amp.reset();
    cab.reset();

    const auto types = processor.getBoard();
    for (int slot = 0; slot < types.size(); ++slot)
    {
        const auto category = WoodshedProcessor::rigCategory (types[slot]);
        if (category == "Amp" && amp == nullptr) amp = std::make_unique<PedalView> (processor, slot, types[slot], false);
        if (category == "Cab" && cab == nullptr) cab = std::make_unique<PedalView> (processor, slot, types[slot], false);
    }

    // The renders have transparent margins; only the controls should take clicks.
    for (auto* v : { amp.get(), cab.get() })
    {
        if (v != nullptr)
        {
            v->setInterceptsMouseClicks (false, true);
            addAndMakeVisible (*v);
        }
    }

    if (amp != nullptr && cab != nullptr)
        amp->toFront (false);   // the head sits on top of the cab

    chooseAmp.setVisible (amp == nullptr);
    chooseCab.setVisible (cab == nullptr);
    swapAmp.setVisible (amp != nullptr);
    swapCab.setVisible (cab != nullptr);
    swapAmp.toFront (false);
    swapCab.toFront (false);

    resized();
    repaint();
}

void RigStack::resized()
{
    auto& art = Art::get();
    const auto area = getLocalBounds().toFloat();

    // Real proportions: the head sits directly on the cab, both as wide as the column allows.
    const float ampAspect = art.ampRect.getWidth() * (float) art.ampBody.getWidth() / (art.ampRect.getHeight() * (float) art.ampBody.getHeight());
    const float cabAspect = art.cabRect.getWidth() * (float) art.cabBody.getWidth() / (art.cabRect.getHeight() * (float) art.cabBody.getHeight());

    float width = area.getWidth() - 8.0f;
    if (width / ampAspect + width / cabAspect > area.getHeight() - 10.0f)
        width = (area.getHeight() - 10.0f) / (1.0f / ampAspect + 1.0f / cabAspect);

    const auto cabBody = juce::Rectangle<float> (width, width / cabAspect).withCentre ({ area.getCentreX(), 0.0f }).withBottomY (area.getBottom() - 6.0f);
    const auto ampBody = juce::Rectangle<float> (width * 0.96f, width * 0.96f / ampAspect).withCentre ({ area.getCentreX(), 0.0f }).withBottomY (cabBody.getY() + 2.0f);

    ampSlot = ampBody;
    cabSlot = cabBody;

    if (amp != nullptr) amp->setBounds (Art::imageBoundsFor (ampBody, art.ampRect).toNearestInt());
    if (cab != nullptr) cab->setBounds (Art::imageBoundsFor (cabBody, art.cabRect).toNearestInt());

    chooseAmp.setBounds (ampBody.toNearestInt().withSizeKeepingCentre (130, 32));
    chooseCab.setBounds (cabBody.toNearestInt().withSizeKeepingCentre (130, 32));

    // Small tabs sitting on the top edge of each box, out of the way of the controls and name.
    auto changeButton = [] (juce::Rectangle<float> box) { return juce::Rectangle<float> (60.0f, 20.0f).withCentre ({ box.getRight() - 58.0f, box.getY() - 2.0f }).toNearestInt(); };
    swapAmp.setBounds (changeButton (ampBody));
    const auto cabGrille = Art::regionIn (Art::imageBoundsFor (cabBody, art.cabRect), art.cabGrille);
    swapCab.setBounds (juce::Rectangle<float> (60.0f, 20.0f).withCentre ({ cabGrille.getRight() - 96.0f, cabGrille.getBottom() - 18.0f }).toNearestInt());
    swapAmp.toFront (false);
    swapCab.toFront (false);
}

void RigStack::paint (juce::Graphics& g)
{
    // Placeholders when no amp or cab is chosen.
    for (auto [slot, present] : { std::pair { ampSlot, amp != nullptr }, std::pair { cabSlot, cab != nullptr } })
    {
        if (present || slot.isEmpty())
            continue;

        juce::Path outline, dashed;
        outline.addRoundedRectangle (slot, 12.0f);
        const float dashes[] { 6.0f, 5.0f };
        juce::PathStrokeType (1.5f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.fillPath (dashed);
    }
}

//==============================================================================
class LibraryPanel::Card final : public juce::Component
{
public:
    Card (const PedalType& t, std::function<void (const juce::String&)> pick, bool studioLook)
        : type (t), onPick (std::move (pick)), embedded (studioLook)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
        if (embedded)
        {
            paintStudio (g);
            return;
        }

        auto r = getLocalBounds().toFloat().reduced (4.0f);
        g.setColour (isMouseOver() ? juce::Colour (0xff2e2b27) : juce::Colour (0xff24221f));
        g.fillRoundedRectangle (r, 10.0f);

        if (isMouseOver())
        {
            g.setColour (palette::accent);
            g.drawRoundedRectangle (r, 10.0f, 1.5f);
        }

        // Miniature enclosure.
        auto mini = r.removeFromLeft (54.0f).reduced (8.0f, 10.0f);
        g.setColour (type.info.body);
        g.fillRoundedRectangle (mini, 5.0f);
        g.setColour (type.info.ink.withAlpha (0.8f));
        for (int i = 0; i < juce::jmin (3, (int) type.info.knobs.size()); ++i)
            g.fillEllipse (mini.getX() + 6.0f + (float) i * 10.0f, mini.getY() + 8.0f, 7.0f, 7.0f);
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ mini.getCentreX(), mini.getBottom() - 12.0f }));

        r.removeFromLeft (4.0f);
        g.setColour (palette::text);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold).withKerningFactor (0.08f));
        g.drawText (type.info.name, r.removeFromTop (30.0f).withTrimmedTop (8.0f), juce::Justification::centredLeft);
        g.setColour (palette::accent);
        g.setFont (juce::FontOptions (11.0f).withKerningFactor (0.1f));
        g.drawText (type.info.subtitle.toUpperCase(), r.removeFromTop (16.0f), juce::Justification::centredLeft);
        g.setColour (palette::dimText);
        g.setFont (juce::FontOptions (11.5f));
        g.drawFittedText (type.info.description, r.reduced (0.0f, 2.0f).withTrimmedRight (8.0f).toNearestInt(),
                          juce::Justification::topLeft, 2);
    }

    void mouseEnter (const juce::MouseEvent&) override   { repaint(); }
    void mouseExit (const juce::MouseEvent&) override    { repaint(); }
    void mouseDown (const juce::MouseEvent&) override    { repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        repaint();
        if (e.mouseWasClicked())
            onPick (type.info.type);
    }

private:
    void paintStudio (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (4.0f);
        const bool over = isMouseOver(), down = over && isMouseButtonDown();
        g.setColour (down ? theme::controlDown : over ? theme::control : theme::panelLight);
        g.fillRoundedRectangle (r, theme::radiusPanel);
        g.setColour (theme::highlight);
        g.fillRect (r.reduced (theme::radiusPanel, 0.0f).withHeight (1.0f));
        g.setColour (over ? theme::accent.withAlpha (0.7f) : juce::Colours::white.withAlpha (0.05f));
        g.drawRoundedRectangle (r.reduced (0.5f), theme::radiusPanel, 1.0f);

        // Miniature enclosure: the pedal's own colours are the content here.
        auto mini = r.removeFromLeft (52.0f).reduced (10.0f, 12.0f);
        g.setColour (type.info.body);
        g.fillRoundedRectangle (mini, 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawRoundedRectangle (mini.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (type.info.ink.withAlpha (0.8f));
        for (int i = 0; i < juce::jmin (3, (int) type.info.knobs.size()); ++i)
            g.fillEllipse (mini.getX() + 5.0f + (float) i * 9.5f, mini.getY() + 7.0f, 6.0f, 6.0f);
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.fillEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ mini.getCentreX(), mini.getBottom() - 10.0f }));

        r.removeFromLeft (2.0f);
        r.removeFromRight (10.0f);
        auto top = r.removeFromTop (32.0f).withTrimmedTop (10.0f);
        g.setColour (theme::text);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        const auto name = type.info.name.substring (0, 1) + type.info.name.substring (1).toLowerCase();
        g.drawText (name, top, juce::Justification::centredLeft);

        g.setColour (theme::dimText);
        g.setFont (juce::FontOptions (11.5f).withStyle ("Medium"));
        g.drawText (type.info.subtitle, r.removeFromTop (16.0f), juce::Justification::centredLeft);
        g.setColour (theme::faintText);
        g.setFont (juce::FontOptions (11.0f));
        g.drawFittedText (type.info.description, r.withTrimmedTop (2.0f).withTrimmedBottom (6.0f).toNearestInt(),
                          juce::Justification::topLeft, 2, 0.95f);
    }

    const PedalType& type;
    std::function<void (const juce::String&)> onPick;
    const bool embedded;
};

LibraryPanel::LibraryPanel (std::function<void (const juce::String&)> pick) : onPick (std::move (pick))
{
    search.setTextToShowWhenEmpty ("Search pedals...", palette::dimText);
    search.setFont (juce::FontOptions (15.0f));
    search.onTextChange = [this] { refresh(); };
    search.onEscapeKey = [this] { if (onClose) onClose(); };
    addAndMakeVisible (search);

    juce::StringArray cats { "All" };
    cats.addArray (Registry::categories());

    for (auto& c : cats)
    {
        int count = 0;
        for (auto& t : Registry::get().all())
            count += (c == "All" || t.info.category == c) ? 1 : 0;

        if (count == 0)
            continue;

        auto* b = categoryButtons.add (new juce::TextButton (c + "  " + juce::String (count)));
        b->setClickingTogglesState (true);
        b->setRadioGroupId (77);
        b->onClick = [this, c] { category = c == "All" ? juce::String() : c; refresh(); };
        addAndMakeVisible (b);
    }

    if (! categoryButtons.isEmpty())
        categoryButtons[0]->setToggleState (true, juce::dontSendNotification);

    viewport.setViewedComponent (&grid, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    setWantsKeyboardFocus (true);
    refresh();
}

void LibraryPanel::showCategory (const juce::String& c)
{
    category = c == "All" ? juce::String() : c;

    for (auto* b : categoryButtons)
        b->setToggleState (b->getButtonText().upToFirstOccurrenceOf ("  ", false, false) == (category.isEmpty() ? "All" : category),
                           juce::dontSendNotification);
}

void LibraryPanel::refresh()
{
    cards.clear();
    const auto query = search.getText().trim().toLowerCase();

    for (auto& t : Registry::get().all())
    {
        if (category.isNotEmpty() && t.info.category != category)
            continue;

        const auto haystack = (t.info.name + " " + t.info.subtitle + " " + t.info.category + " " + t.info.description).toLowerCase();
        if (query.isNotEmpty() && ! haystack.contains (query))
            continue;

        grid.addAndMakeVisible (cards.add (new Card (t, onPick, embedded)));
    }

    resized();
}

void LibraryPanel::setEmbedded (bool shouldBeEmbedded)
{
    if (embedded == shouldBeEmbedded)
        return;

    embedded = shouldBeEmbedded;

    // The studio's default look (combo, field, segmented chips, overlay scroll bars).
    setLookAndFeel (embedded ? &juce::LookAndFeel::getDefaultLookAndFeel() : nullptr);
    search.setFont (juce::FontOptions (embedded ? 13.0f : 15.0f));
    search.setIndents (embedded ? 10 : 4, embedded ? 7 : 4);
    search.setTextToShowWhenEmpty (embedded ? "Search pedals" : "Search pedals...", embedded ? theme::faintText : palette::dimText);
    viewport.setScrollBarThickness (embedded ? 10 : 8);
    refresh();
}

void LibraryPanel::paint (juce::Graphics& g)
{
    if (embedded)
    {
        g.fillAll (juce::Colours::black.withAlpha (0.55f));
        const auto p = panelBounds.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (p.translated (0.0f, 6.0f).expanded (2.0f), theme::radiusPanel + 4.0f);
        g.setColour (theme::panel);
        g.fillRoundedRectangle (p, theme::radiusPanel + 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawRoundedRectangle (p.reduced (0.5f), theme::radiusPanel + 4.0f, 1.0f);

        auto head = panelBounds.reduced (20, 16).removeFromTop (28);
        g.setColour (theme::text);
        g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        g.drawText ("Pedal library", head, juce::Justification::centredLeft);
        g.setColour (theme::faintText);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (juce::String ((int) Registry::get().all().size())
                        + juce::String::fromUTF8 (" pedals  \u00b7  click one to add it  \u00b7  Esc to close"),
                    head, juce::Justification::centredRight);

        // Hairline under the search and category row.
        g.setColour (theme::separator);
        g.fillRect (viewport.getX(), viewport.getY() - 9, viewport.getWidth(), 1);
        return;
    }

    g.fillAll (juce::Colours::black.withAlpha (0.6f));

    g.setColour (palette::panel);
    g.fillRoundedRectangle (panelBounds.toFloat(), 14.0f);
    g.setColour (juce::Colour (0xff35312c));
    g.drawRoundedRectangle (panelBounds.toFloat(), 14.0f, 1.0f);

    g.setColour (palette::text);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold).withKerningFactor (0.15f));
    g.drawText ("PEDAL LIBRARY", panelBounds.reduced (24, 18).removeFromTop (30), juce::Justification::centredLeft);
    g.setColour (palette::dimText);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (juce::String ((int) Registry::get().all().size()) + " pedals  ·  click one to add it to the end of your board  ·  Esc to close",
                panelBounds.reduced (24, 18).removeFromTop (30), juce::Justification::centredRight);
}

void LibraryPanel::resized()
{
    if (embedded)
    {
        panelBounds = getLocalBounds().reduced (juce::jmax (24, getWidth() / 16), 12);
        auto r = panelBounds.reduced (20, 16);
        r.removeFromTop (28 + 12);

        auto row = r.removeFromTop (28);
        search.setBounds (row.removeFromLeft (juce::jlimit (180, 280, row.getWidth() / 4)));
        row.removeFromLeft (12);
        for (auto* b : categoryButtons)
        {
            const int w = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (13.0f).withStyle ("Medium"), b->getButtonText()) + 20;
            b->setBounds (row.removeFromLeft (w));
            row.removeFromLeft (4);
        }

        r.removeFromTop (18);
        viewport.setBounds (r);

        const int cardW = 240, cardH = 92;
        const int cols = juce::jmax (1, r.getWidth() / cardW);
        const int w = r.getWidth() / cols;
        for (int i = 0; i < cards.size(); ++i)
            cards[i]->setBounds ((i % cols) * w, (i / cols) * cardH, w, cardH);
        grid.setSize (r.getWidth(), ((cards.size() + cols - 1) / cols) * cardH);
        return;
    }

    // Short hosts (the studio's rig pane) get slimmer margins so more of the library shows.
    panelBounds = getLocalBounds().reduced (juce::jmax (30, getWidth() / 14), getHeight() < 560 ? 14 : 36);
    auto r = panelBounds.reduced (24, 18);
    r.removeFromTop (38);

    search.setBounds (r.removeFromTop (34));
    r.removeFromTop (12);

    auto row = r.removeFromTop (30);
    for (auto* b : categoryButtons)
    {
        const int w = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (14.0f), b->getButtonText()) + 26;
        b->setBounds (row.removeFromLeft (w));
        row.removeFromLeft (6);
    }

    r.removeFromTop (14);
    viewport.setBounds (r);

    const int cardW = 250, cardH = 96;
    const int cols = juce::jmax (1, (r.getWidth() - 12) / cardW);
    const int w = (r.getWidth() - 12) / cols;

    for (int i = 0; i < cards.size(); ++i)
        cards[i]->setBounds ((i % cols) * w, (i / cols) * cardH, w, cardH);

    grid.setSize (r.getWidth() - 12, ((cards.size() + cols - 1) / cols) * cardH);
}

void LibraryPanel::mouseDown (const juce::MouseEvent& e)
{
    if (! panelBounds.contains (e.getPosition()) && onClose)
        onClose();
}

bool LibraryPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && onClose)
    {
        onClose();
        return true;
    }

    return false;
}

void LibraryPanel::visibilityChanged()
{
    if (isVisible())
    {
        search.setText ({}, juce::dontSendNotification);
        refresh();
        search.grabKeyboardFocus();
    }
}

} // namespace woodshed

//==============================================================================
WoodshedEditor::WoodshedEditor (WoodshedProcessor& p)
    : AudioProcessorEditor (p), woodshedProcessor (p),
      inputAttachment (p.params, "input", inputChoice),
      inputGainAttachment (p.params, "input_gain", inputGain),
      masterAttachment (p.params, "master", master),
      board (p),
      rig (p),
      library ([this] (const juce::String& type) { pickFromLibrary (type); }),
      tuner (p)
{
    using namespace woodshed;
    setLookAndFeel (&lookAndFeel);

    inputChoice.addItemList (p.params.getParameter ("input")->getAllValueStrings(), 1);
    inputChoice.setSelectedItemIndex ((int) p.params.getRawParameterValue ("input")->load(), juce::dontSendNotification);
    inputChoice.setTooltip ("Scarlett Solo: Input 2 is the instrument (guitar) jack");
    addAndMakeVisible (inputChoice);

    for (auto* s : { &inputGain, &master })
    {
        s->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        s->setPopupDisplayEnabled (true, true, nullptr, 900);
        s->setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
        s->setColour (juce::Slider::rotarySliderOutlineColourId, palette::dimText);
        s->setDoubleClickReturnValue (true, 0.0);
        s->textFromValueFunction = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; };
        addAndMakeVisible (s);
    }

    addAndMakeVisible (inputMeter);
    addAndMakeVisible (outputMeter);

    latencyLabel.setFont (juce::FontOptions (12.0f));
    latencyLabel.setColour (juce::Label::textColourId, palette::dimText);
    latencyLabel.setMinimumHorizontalScale (0.7f);
    latencyLabel.setTooltip ("Round-trip latency  ·  buffer size @ sample rate");
    addAndMakeVisible (latencyLabel);

    statusLabel.setFont (juce::FontOptions (12.0f));
    statusLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setColour (juce::Label::textColourId, palette::accent);
    addAndMakeVisible (statusLabel);

    presetChoice.setTextWhenNothingSelected ("Presets");
    presetChoice.setTooltip ("Factory sounds by genre, then your saved boards");
    presetChoice.onChange = [this] { presetChosen (presetChoice.getSelectedId()); };
    addAndMakeVisible (presetChoice);

    prevPresetButton.setTooltip ("Previous preset (left arrow)");
    prevPresetButton.onClick = [this] { stepPreset (-1); };
    addAndMakeVisible (prevPresetButton);
    nextPresetButton.setTooltip ("Next preset (right arrow)");
    nextPresetButton.onClick = [this] { stepPreset (1); };
    addAndMakeVisible (nextPresetButton);

    savePresetButton.setTooltip ("Save this board as a preset");
    savePresetButton.onClick = [this] { savePresetAs(); };
    addAndMakeVisible (savePresetButton);

    libraryButton.setColour (juce::TextButton::buttonColourId, palette::accent);
    libraryButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff1a1410));
    libraryButton.setTooltip ("Browse every pedal and add one to the first free spot (A)");
    libraryButton.onClick = [this] { showLibrary(); };
    addAndMakeVisible (libraryButton);

    audioSetupButton.setTooltip ("Audio interface, buffer size and sample rate");
    addChildComponent (audioSetupButton);

   #if JucePlugin_Build_Standalone
    if (juce::JUCEApplicationBase::isStandaloneApp())
    {
        audioSetupButton.onClick = []
        {
            if (auto* holder = juce::StandalonePluginHolder::getInstance())
                holder->showAudioSettingsDialog();
        };
        audioSetupButton.setVisible (true);
    }
   #endif

    captureButton.setTooltip ("Records 10 seconds of dry input and processed output to a WAV file");
    captureButton.onClick = [this] { startCapture (10.0); };
    addAndMakeVisible (captureButton);

    board.onEmptySpotClicked = [this] (int spot) { showLibrary (spot); };
    board.rebuild();
    addAndMakeVisible (board);

    rig.onChoose = [this] (const juce::String& category) { showLibrary (-1, category); };
    rig.rebuild();
    addAndMakeVisible (rig);

    library.onClose = [this] { library.setVisible (false); grabKeyboardFocus(); };
    addChildComponent (library);

    tunerButton.setTooltip ("Chromatic tuner (T)");
    tunerButton.onClick = [this] { toggleTuner(); };
    addAndMakeVisible (tunerButton);
    tuner.onClose = [this] { toggleTuner(); };
    addChildComponent (tuner);

    captureDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               #if JUCE_MAC
                     .getChildFile ("Application Support")
               #endif
                     .getChildFile ("Woodshed").getChildFile ("captures");
    captureRequestFile = captureDir.getChildFile ("capture.request");

    refreshPresetList();
    p.addChangeListener (this);

    setWantsKeyboardFocus (true);
    setResizable (true, true);
    setResizeLimits (1000, 560, 8192, 8192);
    setSize (1320, 700);
    startTimerHz (30);
}

WoodshedEditor::~WoodshedEditor()
{
    woodshedProcessor.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void WoodshedEditor::paint (juce::Graphics& g)
{
    using namespace woodshed;
    g.fillAll (palette::board);

    // Stage floor under everything, darkened towards the edges like a pool of stage light.
    const int barH = topBarHeight();
    const auto stage = getLocalBounds().withTrimmedTop (barH).withTrimmedBottom (24).toFloat();
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (Art::get().floor, stage, juce::RectanglePlacement::fillDestination);
    juce::ColourGradient vignette (juce::Colours::transparentBlack, stage.getCentreX(), stage.getCentreY() - stage.getHeight() * 0.1f,
                                   juce::Colours::black.withAlpha (0.62f), stage.getX(), stage.getBottom(), true);
    g.setGradientFill (vignette);
    g.fillRect (stage);

    auto bar = getLocalBounds().removeFromTop (barH);
    g.setColour (palette::topBar);
    g.fillRect (bar);
    g.setColour (juce::Colour (0xff2c2925));
    g.fillRect (bar.removeFromBottom (1));

    if (embedded)
    {
        // The studio's pane header carries the name and the bar's controls; only the footer here.
        paintEmbeddedFooter (g);
        return;
    }

    g.setColour (palette::text);
    g.setFont (juce::FontOptions (25.0f, juce::Font::bold).withKerningFactor (0.22f));
    g.drawText ("WOODSHED", 24, 10, 220, 34, juce::Justification::centredLeft);

    g.setColour (palette::dimText);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold).withKerningFactor (0.15f));
    g.drawText ("IN", inputMeter.getBounds().withY (12).withHeight (12), juce::Justification::centredLeft);
    g.drawText ("OUT", outputMeter.getBounds().withY (12).withHeight (12), juce::Justification::centredLeft);
    g.drawText ("INPUT", inputGain.getBounds().withY (56).withHeight (12), juce::Justification::centred);
    g.drawText ("MASTER", master.getBounds().withY (56).withHeight (12), juce::Justification::centred);

    auto footer = getLocalBounds().removeFromBottom (24);
    g.setColour (palette::topBar);
    g.fillRect (footer);
    g.setColour (palette::dimText);
    g.setFont (juce::FontOptions (11.5f));
    int onBoard = 0;
    for (auto spot : woodshedProcessor.getSpots())
        onBoard += spot >= 0 ? 1 : 0;

    footer.reduce (16, 0);
    const auto hints = juce::String (onBoard) + " / " + juce::String (WoodshedProcessor::boardSpots)
                     + juce::String::fromUTF8 (" spots   \u00b7   1-0 stomp   \u00b7   \u2190 \u2192 presets   \u00b7   T tuner   \u00b7   F full screen   \u00b7   A add pedal   \u00b7   drag to rearrange");
    g.drawText (hints, footer.removeFromLeft (juce::jmin (footer.getWidth() / 2, 560)), juce::Justification::centredLeft);

    if (presetCaption.isNotEmpty() && statusLabel.getText().isEmpty())
    {
        g.setColour (palette::text.withAlpha (0.8f));
        g.drawFittedText (presetCaption, footer.withTrimmedLeft (16), juce::Justification::centredRight, 1, 0.85f);
    }
}

void WoodshedEditor::paintEmbeddedFooter (juce::Graphics& g)
{
    namespace theme = woodshed::theme;

    // A quiet status line in the studio's type: spots and shortcuts left, the preset right.
    auto footer = getLocalBounds().removeFromBottom (24);
    g.setColour (theme::bar);
    g.fillRect (footer);
    g.setColour (theme::separator);
    g.fillRect (footer.removeFromTop (1));

    int onBoard = 0;
    for (auto spot : woodshedProcessor.getSpots())
        onBoard += spot >= 0 ? 1 : 0;

    footer.reduce (16, 0);
    const auto dot = juce::String::fromUTF8 ("  \u00b7  ");
    const auto hints = juce::String (onBoard) + " of " + juce::String (WoodshedProcessor::boardSpots) + " spots" + dot
                     + "1-0 stomp" + dot + juce::String::fromUTF8 ("\u2190 \u2192 presets") + dot + "T tuner" + dot
                     + "A add pedal" + dot + "drag to rearrange";
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (theme::faintText);
    g.drawText (hints, footer.removeFromLeft (juce::jmin (footer.getWidth() / 2, 520)), juce::Justification::centredLeft);

    if (presetCaption.isNotEmpty() && statusLabel.getText().isEmpty())
    {
        g.setColour (theme::dimText);
        g.drawFittedText (presetCaption, footer.withTrimmedLeft (16), juce::Justification::centredRight, 1, 0.85f);
    }
}

void WoodshedEditor::resized()
{
    auto r = getLocalBounds();
    library.setBounds (r);
    tuner.setBounds (r);

    auto bar = r.removeFromTop (topBarHeight()).reduced (16, 0);

    if (! embedded)
    {
        auto brand = bar.removeFromLeft (230);
        latencyLabel.setBounds (brand.withTrimmedTop (42).withHeight (18).withTrimmedLeft (8));

        inputChoice.setBounds (bar.removeFromLeft (100).withSizeKeepingCentre (100, 28));
        bar.removeFromLeft (8);
        inputGain.setBounds (bar.removeFromLeft (52).withTrimmedTop (8).withHeight (48));
        bar.removeFromLeft (8);
        inputMeter.setBounds (bar.removeFromLeft (90).withTrimmedTop (28).withHeight (20));

        master.setBounds (bar.removeFromRight (52).withTrimmedTop (8).withHeight (48));
        bar.removeFromRight (8);
        outputMeter.setBounds (bar.removeFromRight (90).withTrimmedTop (28).withHeight (20));
        bar.removeFromRight (18);

        auto button = [&bar] (juce::Component& c, int w)
        {
            if (c.isVisible())
            {
                c.setBounds (bar.removeFromRight (w).withSizeKeepingCentre (w, 28));
                bar.removeFromRight (8);
            }
        };

        button (captureButton, 76);
        button (audioSetupButton, 64);
        button (libraryButton, 116);
        button (tunerButton, 64);
        bar.removeFromRight (8);
        button (savePresetButton, 56);
        nextPresetButton.setBounds (bar.removeFromRight (28).withSizeKeepingCentre (28, 28));
        bar.removeFromRight (4);
        // The preset box takes whatever the bar has left, within reason.
        const int presetWidth = juce::jlimit (130, 260, bar.getWidth() - 28 - 4 - 16);
        presetChoice.setBounds (bar.removeFromRight (presetWidth).withSizeKeepingCentre (presetWidth, 28));
        bar.removeFromRight (4);
        prevPresetButton.setBounds (bar.removeFromRight (28).withSizeKeepingCentre (28, 28));
        bar.removeFromRight (8);
    }

    // Status messages show in the footer, over the preset caption.
    statusLabel.setBounds (getLocalBounds().removeFromBottom (24).withTrimmedLeft (getWidth() / 2).withTrimmedRight (16));

    r.removeFromBottom (24);
    r.reduce (14, 14);

    const int rigWidth = juce::jlimit (300, 400, (int) ((float) r.getWidth() * 0.27f));

    if (embedded)
    {
        // A wide, short pane: keep the board and the stack together in the middle of the stage
        // instead of leaving the board stranded on the left and the amp on the far right.
        const auto& image = woodshed::Art::get().board;
        const int boardW = (int) std::ceil ((float) r.getHeight() * (float) image.getWidth() / (float) image.getHeight()) + 4;
        const int needed = boardW + 14 + rigWidth;
        if (r.getWidth() > needed)
            r = r.withSizeKeepingCentre (needed, r.getHeight());
    }

    rig.setBounds (r.removeFromRight (rigWidth));
    r.removeFromRight (14);
    board.setBounds (r);
}

void WoodshedEditor::setEmbedded (bool shouldBeEmbedded)
{
    if (embedded == shouldBeEmbedded)
        return;

    embedded = shouldBeEmbedded;

    for (juce::Component* c : { (juce::Component*) &inputChoice, (juce::Component*) &master, (juce::Component*) &outputMeter,
                                (juce::Component*) &captureButton, (juce::Component*) &latencyLabel })
        c->setVisible (! embedded);

    if (embedded)
    {
        audioSetupButton.setVisible (false);
        tunerButton.setVisible (false);                  // the pane header has its own
        setResizable (false, false);

        // The pane header adopts these; give them the studio's look and conventions.
        libraryButton.removeColour (juce::TextButton::buttonColourId);
        libraryButton.removeColour (juce::TextButton::textColourOffId);
        libraryButton.getProperties().set ("primary", true);
        inputGain.removeColour (juce::Slider::rotarySliderOutlineColourId);
        inputGain.setTooltip ("Input gain (double-click for 0 dB)");
        inputMeter.setSlim (true);
        prevPresetButton.setButtonText (juce::String::fromUTF8 ("\u2039"));
        nextPresetButton.setButtonText (juce::String::fromUTF8 ("\u203a"));
        statusLabel.setFont (juce::FontOptions (11.0f).withStyle ("Medium"));
        library.setEmbedded (true);
        tuner.setEmbedded (true);
        keepControlsFromTakingFocus (*this);
    }

    resized();
    repaint();
}

void WoodshedEditor::setTrackName (const juce::String& name)
{
    if (trackName == name)
        return;

    trackName = name;
    repaint (getLocalBounds().removeFromTop (topBarHeight()));
}

void WoodshedEditor::keepControlsFromTakingFocus (juce::Component& parent)
{
    // In the studio a clicked button or knob must not keep the keyboard focus, or space would
    // press it again instead of starting playback. Focus falls through to the editor instead.
    for (auto* child : parent.getChildren())
    {
        if (dynamic_cast<juce::TextEditor*> (child) != nullptr)
            continue;

        if (dynamic_cast<juce::Button*> (child) != nullptr || dynamic_cast<juce::ComboBox*> (child) != nullptr
            || dynamic_cast<juce::Slider*> (child) != nullptr)
        {
            child->setWantsKeyboardFocus (false);
            child->setMouseClickGrabsKeyboardFocus (false);
        }

        keepControlsFromTakingFocus (*child);
    }
}

bool WoodshedEditor::keyPressed (const juce::KeyPress& key)
{
    // In the studio, chords (Cmd+A, Cmd+T, Cmd+1...) belong to the studio's commands.
    if (embedded && (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown() || key.getModifiers().isAltDown()))
        return false;

    const auto c = key.getTextCharacter();

    if (c >= '0' && c <= '9')
    {
        const int index = c == '0' ? 9 : (int) (c - '1');
        const auto pedals = board.pedalsInChainOrder();
        if (juce::isPositiveAndBelow (index, pedals.size()))
            pedals[index]->toggle();
        return true;
    }

    if (c == 'a' || c == 'A')
    {
        showLibrary();
        return true;
    }

    if ((c == 'f' || c == 'F') && ! embedded)
    {
        if (auto* window = findParentComponentOfClass<juce::ResizableWindow>())
            window->setFullScreen (! window->isFullScreen());
        return true;
    }

    if (key.isKeyCode (juce::KeyPress::leftKey) || key.isKeyCode (juce::KeyPress::rightKey))
    {
        stepPreset (key.isKeyCode (juce::KeyPress::leftKey) ? -1 : 1);
        return true;
    }

    if (c == 't' || c == 'T')
    {
        toggleTuner();
        return true;
    }

    return false;
}

void WoodshedEditor::showLibrary (int targetSpot, const juce::String& category)
{
    libraryTargetSpot = targetSpot;
    library.showCategory (category.isEmpty() ? "All" : category);
    library.setVisible (true);
    library.toFront (true);
}

void WoodshedEditor::toggleTuner()
{
    const bool show = ! tuner.isVisible();
    library.setVisible (false);
    tuner.setVisible (show);
    tunerButton.setToggleState (show, juce::dontSendNotification);

    if (show)
        tuner.toFront (true);
    else
        grabKeyboardFocus();
}

void WoodshedEditor::pickFromLibrary (const juce::String& type)
{
    library.setVisible (false);
    grabKeyboardFocus();

    if (! woodshedProcessor.addPedal (type, WoodshedProcessor::isRigType (type) ? -1 : libraryTargetSpot))
        flash ("The board is full: remove a pedal to make room");
}

void WoodshedEditor::flash (const juce::String& message)
{
    statusLabel.setText (message, juce::dontSendNotification);
    repaint (getLocalBounds().removeFromBottom (24));
    statusFrames = 120;
}

void WoodshedEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    board.rebuild();
    rig.rebuild();

    if (embedded)
        keepControlsFromTakingFocus (*this);

    repaint();
}

void WoodshedEditor::refreshPresetList()
{
    presetFiles = WoodshedProcessor::presetFolder().findChildFiles (juce::File::findFiles, false, "*.woodshed");
    std::sort (presetFiles.begin(), presetFiles.end(),
               [] (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()) < 0; });

    const int selected = presetChoice.getSelectedId();
    presetChoice.clear (juce::dontSendNotification);
    auto* menu = presetChoice.getRootMenu();

    // Factory sounds, one submenu per genre.
    const auto& factory = WoodshedProcessor::factoryPresets();
    for (size_t i = 0; i < factory.size();)
    {
        const auto genre = factory[i].genre;
        juce::PopupMenu sub;
        for (; i < factory.size() && factory[i].genre == genre; ++i)
            sub.addItem (factoryIdBase + (int) i, factory[i].name);
        menu->addSubMenu (genre, sub);
    }

    menu->addSeparator();

    if (! presetFiles.isEmpty())
    {
        menu->addSectionHeader ("My presets");
        for (int i = 0; i < presetFiles.size(); ++i)
            menu->addItem (i + 1, presetFiles[i].getFileNameWithoutExtension());
        menu->addSeparator();
    }

    menu->addItem (defaultBoardId, "Default board");

    if (selected != 0)
        presetChoice.setSelectedId (selected, juce::dontSendNotification);
}

void WoodshedEditor::presetChosen (int id)
{
    const auto& factory = WoodshedProcessor::factoryPresets();

    if (id == defaultBoardId)
    {
        woodshedProcessor.loadDefaultBoard();
        presetCaption = {};
    }
    else if (juce::isPositiveAndBelow (id - factoryIdBase, (int) factory.size()))
    {
        const auto& p = factory[(size_t) (id - factoryIdBase)];
        woodshedProcessor.loadFactoryPreset (p);
        presetCaption = p.genre + juce::String::fromUTF8 ("  \u00b7  ") + p.name + "   " + p.description;
    }
    else if (juce::isPositiveAndBelow (id - 1, presetFiles.size()))
    {
        woodshedProcessor.loadPreset (presetFiles[id - 1]);
        presetCaption = juce::String::fromUTF8 ("My presets  \u00b7  ") + presetFiles[id - 1].getFileNameWithoutExtension();
    }

    repaint();
}

void WoodshedEditor::stepPreset (int delta)
{
    // Factory sounds in menu order, then the player's own.
    juce::Array<int> ids;
    for (int i = 0; i < (int) WoodshedProcessor::factoryPresets().size(); ++i) ids.add (factoryIdBase + i);
    for (int i = 0; i < presetFiles.size(); ++i) ids.add (i + 1);
    if (ids.isEmpty())
        return;

    const int at = ids.indexOf (presetChoice.getSelectedId());
    const int next = at < 0 ? (delta > 0 ? 0 : ids.size() - 1) : (at + delta + ids.size()) % ids.size();
    presetChoice.setSelectedId (ids[next], juce::sendNotificationSync);
}

void WoodshedEditor::savePresetAs()
{
    auto* window = new juce::AlertWindow ("Save preset", "Name this board:", juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor ("name", presetChoice.getSelectedId() > 0 && presetChoice.getSelectedId() != defaultBoardId ? presetChoice.getText() : juce::String());
    window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true, juce::ModalCallbackFunction::create ([this, window] (int result)
    {
        const auto name = juce::File::createLegalFileName (window->getTextEditorContents ("name").trim());

        if (result == 1 && name.isNotEmpty())
        {
            const auto file = WoodshedProcessor::presetFolder().getChildFile (name + ".woodshed");
            const bool ok = woodshedProcessor.savePreset (file);
            refreshPresetList();
            presetChoice.setSelectedId (presetFiles.indexOf (file) + 1, juce::dontSendNotification);
            statusLabel.setText (ok ? "Saved \"" + name + "\"" : "Couldn't save preset", juce::dontSendNotification);
            repaint (getLocalBounds().removeFromBottom (24));
            statusFrames = 90;
        }
    }), true);
}

void WoodshedEditor::startCapture (double seconds)
{
    woodshedProcessor.armCapture (seconds);
    pendingCaptureName = "capture-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".wav";
    statusLabel.setText ("Recording " + juce::String ((int) seconds) + "s... play something", juce::dontSendNotification);
    repaint (getLocalBounds().removeFromBottom (24));
    statusFrames = -1;
}

void WoodshedEditor::updateLatencyText()
{
    juce::String text = "Plugin mode: the host sets latency";

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
                text << "  ·  " << xruns << (xruns == 1 ? " dropout" : " dropouts");
        }
        else
        {
            text = "No audio device: click Audio";
        }
    }
   #endif

    latencyLabel.setText (text, juce::dontSendNotification);
}

void WoodshedEditor::timerCallback()
{
    inputMeter.setLevel (woodshedProcessor.inputPeak.exchange (0.0f));
    outputMeter.setLevel (woodshedProcessor.outputPeak.exchange (0.0f));

    if (++frame % 15 == 0 && ! embedded)
    {
        updateLatencyText();

        // Keep the stomp shortcuts working: if the window is active but nothing has focus, take it.
        // (Not in the studio: there the focus belongs to whatever the player last clicked.)
        if (auto* peer = getPeer(); peer != nullptr && peer->isFocused() && ! hasKeyboardFocus (true) && ! tuner.isVisible())
            grabKeyboardFocus();

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
        repaint (getLocalBounds().removeFromBottom (24));
        statusFrames = 150;
    }

    if (statusFrames > 0 && --statusFrames == 0)
    {
        statusLabel.setText ({}, juce::dontSendNotification);
        repaint (getLocalBounds().removeFromBottom (24));
    }
}
