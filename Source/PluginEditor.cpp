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
    const auto ink = slider.findColour (juce::Slider::rotarySliderOutlineColourId);
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (3.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);

    // Tick marks printed on the enclosure; stepped knobs get one tick per position.
    const auto interval = slider.getNormalisableRange().interval;
    const int ticks = interval > 0.0 ? juce::roundToInt (1.0 / interval) : 10;

    g.setColour (ink.withAlpha (0.55f));
    for (int i = 0; i <= ticks; ++i)
    {
        const float a = startAngle + (float) i / (float) ticks * (endAngle - startAngle);
        const bool major = interval > 0.0 || i % 5 == 0;
        const auto p1 = centre.getPointOnCircumference (radius * 0.86f, a);
        const auto p2 = centre.getPointOnCircumference (radius * (major ? 1.0f : 0.94f), a);
        g.drawLine ({ p1, p2 }, major ? 1.6f : 1.0f);
    }

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

    g.setColour (slider.isMouseOverOrDragging() ? palette::accent : juce::Colour (0xfff2ece0));
    g.drawLine ({ centre.getPointOnCircumference (cap * 0.25f, angle),
                  centre.getPointOnCircumference (skirt * 0.95f, angle) }, juce::jmax (1.8f, radius * 0.08f));
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
    caption.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (caption);

    valueText.setJustificationType (juce::Justification::centred);
    valueText.setFont (juce::FontOptions (10.0f));
    valueText.setColour (juce::Label::textColourId, ink.withAlpha (0.75f));
    valueText.setInterceptsMouseClicks (false, false);
    valueText.setMinimumHorizontalScale (0.6f);
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
    auto r = getLocalBounds();
    if (spec.isChoice())
        valueText.setBounds (r.removeFromBottom (13));
    caption.setBounds (r.removeFromBottom (15));
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
PedalView::PedalView (WoodshedProcessor& p, int s, const juce::String& t)
    : processor (p), slot (s), type (Registry::get().find (t)), typeId (t)
{
    removeButton.setTooltip ("Remove from board");
    removeButton.onClick = [this]
    {
        auto& proc = processor;
        const int position = slot;
        juce::MessageManager::callAsync ([&proc, position] { proc.removePedal (position); });
    };
    addAndMakeVisible (removeButton);

    if (type == nullptr)
        return;

    for (size_t k = 0; k < type->info.knobs.size(); ++k)
        addAndMakeVisible (knobs.add (new Knob (processor.params, WoodshedProcessor::knobParamId (slot, (int) k),
                                                type->info.knobs[k], type->info.ink)));

    footswitchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.params, WoodshedProcessor::onParamId (slot), footswitch);
    footswitch.setTooltip ("Click or press " + juce::String (slot + 1) + " to stomp");
    footswitch.onStateChange = [this] { repaint(); };
    addAndMakeVisible (footswitch);
}

int PedalView::widthFor (const PedalType* t)
{
    if (t == nullptr)
        return 180;

    const int n = (int) t->info.knobs.size();
    int w = n <= 2 ? 190 : n == 3 ? 205 : n == 4 ? 225 : n <= 6 ? 285 : 345;

    if (t->info.category == "Amp")
        w = juce::jmax (w, 380);

    return w;
}

void PedalView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);

    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRoundedRectangle (r.translated (0, 6), 14.0f);

    if (type == nullptr)
    {
        g.setColour (juce::Colour (0xff2a2826));
        g.fillRoundedRectangle (r, 14.0f);
        g.setColour (palette::dimText);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText ("Unknown pedal\n" + typeId, r.reduced (12).toNearestInt(), juce::Justification::centred, 3);
        return;
    }

    const auto& info = type->info;
    const bool isAmp = info.category == "Amp";
    const bool isCab = info.category == "Cab";
    const auto knobArea = r.withHeight (r.getHeight() * 0.5f);

    if (isAmp)
    {
        // Tolex-covered head with a coloured control panel and gold piping.
        g.setColour (palette::tolex);
        g.fillRoundedRectangle (r, 12.0f);
        g.setColour (juce::Colours::white.withAlpha (0.025f));
        for (float y = r.getY(); y < r.getBottom(); y += 3.0f)
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());

        auto plate = knobArea.reduced (12.0f, 14.0f).withTrimmedTop (4.0f);
        juce::ColourGradient face (info.body.brighter (0.15f), plate.getX(), plate.getY(),
                                   info.body.darker (0.2f), plate.getX(), plate.getBottom(), false);
        g.setGradientFill (face);
        g.fillRoundedRectangle (plate, 6.0f);
        g.setColour (palette::piping);
        g.drawRoundedRectangle (plate.expanded (3.0f), 8.0f, 1.6f);
        g.drawRoundedRectangle (r.reduced (5.0f), 9.0f, 1.0f);
    }
    else if (isCab)
    {
        g.setColour (palette::tolex);
        g.fillRoundedRectangle (r, 12.0f);

        auto strip = knobArea.reduced (10.0f, 12.0f);
        g.setColour (info.body);
        g.fillRoundedRectangle (strip, 6.0f);

        // Grille cloth: a woven diagonal pattern under the controls.
        auto grille = r.withTrimmedTop (knobArea.getHeight() + 4.0f).reduced (14.0f, 0.0f).withTrimmedBottom (110.0f);
        g.setColour (juce::Colour (0xff3a342b));
        g.fillRoundedRectangle (grille, 6.0f);
        g.saveState();
        g.reduceClipRegion (grille.toNearestInt());
        g.setColour (juce::Colour (0xff58503f).withAlpha (0.55f));
        for (float d = -grille.getHeight(); d < grille.getWidth(); d += 6.0f)
        {
            g.drawLine (grille.getX() + d, grille.getBottom(), grille.getX() + d + grille.getHeight(), grille.getY(), 1.2f);
            g.drawLine (grille.getX() + d, grille.getY(), grille.getX() + d + grille.getHeight(), grille.getBottom(), 0.8f);
        }
        g.restoreState();
        g.setColour (palette::piping.withAlpha (0.7f));
        g.drawRoundedRectangle (grille, 6.0f, 1.2f);
    }
    else
    {
        juce::ColourGradient body (info.body.brighter (0.18f), r.getX(), r.getY(),
                                   info.body.darker (0.25f), r.getX(), r.getBottom(), false);
        g.setGradientFill (body);
        g.fillRoundedRectangle (r, 14.0f);
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.drawRoundedRectangle (r.reduced (1.0f), 13.0f, 1.2f);

        for (auto p : { r.getTopLeft() + juce::Point<float> (13, 13), r.getTopRight() + juce::Point<float> (-13, 13),
                        r.getBottomLeft() + juce::Point<float> (13, -13), r.getBottomRight() + juce::Point<float> (-13, -13) })
        {
            g.setColour (juce::Colour (0xff9a968f));
            g.fillEllipse (juce::Rectangle<float> (8, 8).withCentre (p));
            g.setColour (juce::Colour (0xff4a4743));
            g.drawLine ({ p.translated (-2.5f, -2.5f), p.translated (2.5f, 2.5f) }, 1.2f);
        }
    }

    const auto nameInk = (isAmp || isCab) ? palette::text : info.ink;

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
    const float nameSize = juce::jlimit (18.0f, 30.0f, r.getWidth() / (float) juce::jmax (4, info.name.length()) * 1.25f);
    g.setColour (nameInk);
    g.setFont (juce::FontOptions (nameSize, juce::Font::bold).withKerningFactor (0.12f));
    g.drawFittedText (info.name, plate.removeFromTop (34.0f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
    g.setColour (nameInk.withAlpha (0.7f));
    g.setFont (juce::FontOptions (11.0f).withKerningFactor (0.18f));
    g.drawText (info.subtitle.toUpperCase(), plate.removeFromTop (16.0f), juce::Justification::centred);

    if (slot < 9)
    {
        g.setColour (nameInk.withAlpha (0.45f));
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText (juce::String (slot + 1), r.withTrimmedBottom (8.0f).removeFromBottom (14.0f), juce::Justification::centred);
    }
}

void PedalView::resized()
{
    removeButton.setBounds (getWidth() - 30, 8, 22, 22);

    if (type == nullptr)
        return;

    auto r = getLocalBounds().reduced (18, 16);
    auto knobArea = r.removeFromTop ((int) ((float) getHeight() * 0.46f)).withTrimmedTop (type->info.category == "Amp" ? 8 : 4);
    const int n = knobs.size();

    if (n > 0)
    {
        const int perRow = n <= 2 ? n : n == 3 ? 2 : n == 4 ? 2 : n <= 6 ? 3 : 4;
        const int rows = (n + perRow - 1) / perRow;
        const int cellH = knobArea.getHeight() / rows;
        const int cellW = knobArea.getWidth() / perRow;

        for (int row = 0, i = 0; row < rows; ++row)
        {
            const int inRow = juce::jmin (perRow, n - i);
            auto line = knobArea.removeFromTop (cellH);
            line = line.withSizeKeepingCentre (cellW * inRow, cellH);

            for (int c = 0; c < inRow; ++c, ++i)
                knobs[i]->setBounds (line.removeFromLeft (cellW).reduced (3, 2));
        }
    }

    const int fs = 70;
    footswitch.setBounds (getLocalBounds().getCentreX() - fs / 2, getHeight() - fs - 26, fs, fs);
}

void PedalView::mouseDown (const juce::MouseEvent&)
{
    dragStarted = false;
}

void PedalView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStarted || e.getDistanceFromDragStart() < 8)
        return;

    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
    {
        dragStarted = true;
        auto image = createComponentSnapshot (getLocalBounds()).rescaled (getWidth() / 2, getHeight() / 2);
        container->startDragging ("pedal:" + juce::String (slot), this, juce::ScaledImage (image), true);
    }
}

//==============================================================================
BoardContent::BoardContent (WoodshedProcessor& p) : processor (p)
{
    addButton.setTooltip ("Add a pedal (A)");
    addButton.onClick = [this] { if (onAddClicked) onAddClicked(); };
    addAndMakeVisible (addButton);
}

void BoardContent::rebuild()
{
    pedals.clear();
    const auto types = processor.getBoard();

    for (int i = 0; i < types.size(); ++i)
        addAndMakeVisible (pedals.add (new PedalView (processor, i, types[i])));

    addButton.setVisible (types.size() < WoodshedProcessor::maxSlots);
    layoutForHeight (getHeight());
}

void BoardContent::layoutForHeight (int height)
{
    const auto types = processor.getBoard();
    const int top = 22, pedalH = juce::jmax (300, height - 44);
    int x = 26;

    for (auto* p : pedals)
    {
        const int w = PedalView::widthFor (Registry::get().find (types[p->getSlot()]));
        p->setBounds (x, top, w, pedalH);
        x += w + 24;
    }

    addButton.setBounds (x, top + pedalH / 2 - 70, 120, 140);
    x += addButton.isVisible() ? 120 + 26 : 0;

    setSize (x, height);
}

void BoardContent::paint (juce::Graphics& g)
{
    g.fillAll (palette::board);
    g.setColour (palette::boardRib);
    for (int y = 6; y < getHeight(); y += 14)
        g.fillRect (0, y, getWidth(), 6);

    // Patch cable running through the chain.
    const float y = (float) getHeight() * 0.62f;
    g.setColour (palette::cable);
    g.drawLine (0.0f, y, (float) getWidth(), y, 7.0f);
    g.setColour (juce::Colour (0xff2a2724));
    g.drawLine (0.0f, y - 2.0f, (float) getWidth(), y - 2.0f, 1.5f);
}

void BoardContent::paintOverChildren (juce::Graphics& g)
{
    if (dropIndex < 0)
        return;

    int x = 14;
    if (juce::isPositiveAndBelow (dropIndex, pedals.size()))
        x = pedals[dropIndex]->getX() - 13;
    else if (! pedals.isEmpty())
        x = pedals.getLast()->getRight() + 11;

    g.setColour (palette::accent);
    g.fillRoundedRectangle ((float) x - 2.0f, 16.0f, 4.0f, (float) getHeight() - 32.0f, 2.0f);
}

bool BoardContent::isInterestedInDragSource (const SourceDetails& d)
{
    return d.description.toString().startsWith ("pedal:");
}

int BoardContent::insertionIndexFor (int x) const
{
    for (int i = 0; i < pedals.size(); ++i)
        if (x < pedals[i]->getBounds().getCentreX())
            return i;

    return pedals.size();
}

void BoardContent::itemDragMove (const SourceDetails& d)
{
    const int index = insertionIndexFor (d.localPosition.x);
    if (index != dropIndex) { dropIndex = index; repaint(); }
}

void BoardContent::itemDragExit (const SourceDetails&)
{
    dropIndex = -1;
    repaint();
}

void BoardContent::itemDropped (const SourceDetails& d)
{
    const int from = d.description.toString().fromFirstOccurrenceOf ("pedal:", false, false).getIntValue();
    int to = insertionIndexFor (d.localPosition.x);
    if (to > from) --to;

    dropIndex = -1;
    repaint();

    auto& proc = processor;
    juce::MessageManager::callAsync ([&proc, from, to] { proc.movePedal (from, to); });
}

//==============================================================================
class LibraryPanel::Card final : public juce::Component
{
public:
    Card (const PedalType& t, std::function<void (const juce::String&)> pick) : type (t), onPick (std::move (pick))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
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
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked())
            onPick (type.info.type);
    }

private:
    const PedalType& type;
    std::function<void (const juce::String&)> onPick;
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

        grid.addAndMakeVisible (cards.add (new Card (t, onPick)));
    }

    resized();
}

void LibraryPanel::paint (juce::Graphics& g)
{
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
    panelBounds = getLocalBounds().reduced (juce::jmax (30, getWidth() / 14), 36);
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
        search.setText ({}, juce::sendNotification);
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
      library ([this] (const juce::String& type)
               {
                   library.setVisible (false);
                   woodshedProcessor.insertPedal (type, woodshedProcessor.getBoard().size());
                   juce::Timer::callAfterDelay (60, [safe = juce::Component::SafePointer<WoodshedEditor> (this)]
                   {
                       if (safe != nullptr)
                           safe->boardViewport.setViewPosition (safe->board.getWidth(), 0);
                   });
               })
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
    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setColour (juce::Label::textColourId, palette::accent);
    addAndMakeVisible (statusLabel);

    presetChoice.setTextWhenNothingSelected ("Presets");
    presetChoice.onChange = [this]
    {
        const int id = presetChoice.getSelectedId();
        if (id == 9999)
            woodshedProcessor.loadDefaultBoard();
        else if (juce::isPositiveAndBelow (id - 1, presetFiles.size()))
            woodshedProcessor.loadPreset (presetFiles[id - 1]);
    };
    addAndMakeVisible (presetChoice);

    savePresetButton.setTooltip ("Save this board as a preset");
    savePresetButton.onClick = [this] { savePresetAs(); };
    addAndMakeVisible (savePresetButton);

    libraryButton.setColour (juce::TextButton::buttonColourId, palette::accent);
    libraryButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff1a1410));
    libraryButton.setTooltip ("Browse every pedal (A)");
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

    board.onAddClicked = [this] { showLibrary(); };
    board.rebuild();
    boardViewport.setViewedComponent (&board, false);
    boardViewport.setScrollBarsShown (false, true);
    boardViewport.setScrollBarThickness (10);
    addAndMakeVisible (boardViewport);

    library.onClose = [this] { library.setVisible (false); grabKeyboardFocus(); };
    addChildComponent (library);

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
    setResizeLimits (1000, 560, 3000, 1400);
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

    auto bar = getLocalBounds().removeFromTop (72);
    g.setColour (palette::topBar);
    g.fillRect (bar);
    g.setColour (juce::Colour (0xff2c2925));
    g.fillRect (bar.removeFromBottom (1));

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
    g.drawText (juce::String (woodshedProcessor.getBoard().size()) + " / " + juce::String (WoodshedProcessor::maxSlots)
                    + " pedals   ·   1-9 stomp   ·   A add pedal   ·   drag a pedal to reorder   ·   double-click a knob to reset",
                footer.reduced (16, 0), juce::Justification::centredLeft);
}

void WoodshedEditor::resized()
{
    auto r = getLocalBounds();
    library.setBounds (r);

    auto bar = r.removeFromTop (72).reduced (16, 0);

    auto brand = bar.removeFromLeft (230);
    latencyLabel.setBounds (brand.withTrimmedTop (42).withHeight (18).withTrimmedLeft (8));

    inputChoice.setBounds (bar.removeFromLeft (100).withSizeKeepingCentre (100, 28));
    bar.removeFromLeft (8);
    inputGain.setBounds (bar.removeFromLeft (52).withTrimmedTop (8).withHeight (48));
    bar.removeFromLeft (8);
    inputMeter.setBounds (bar.removeFromLeft (120).withTrimmedTop (28).withHeight (20));

    master.setBounds (bar.removeFromRight (52).withTrimmedTop (8).withHeight (48));
    bar.removeFromRight (8);
    outputMeter.setBounds (bar.removeFromRight (120).withTrimmedTop (28).withHeight (20));
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
    bar.removeFromRight (8);
    button (savePresetButton, 56);
    button (presetChoice, 170);

    statusLabel.setBounds (bar.reduced (8, 22));

    r.removeFromBottom (24);
    boardViewport.setBounds (r);
    board.layoutForHeight (r.getHeight() - boardViewport.getScrollBarThickness());
}

bool WoodshedEditor::keyPressed (const juce::KeyPress& key)
{
    const auto c = key.getTextCharacter();

    if (c >= '1' && c <= '9')
    {
        const int index = (int) (c - '1');
        if (juce::isPositiveAndBelow (index, board.pedals.size()))
            board.pedals[index]->toggle();
        return true;
    }

    if (c == 'a' || c == 'A')
    {
        showLibrary();
        return true;
    }

    return false;
}

void WoodshedEditor::showLibrary()
{
    library.setVisible (true);
    library.toFront (true);
}

void WoodshedEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto scroll = boardViewport.getViewPosition();
    board.rebuild();
    board.layoutForHeight (boardViewport.getHeight() - boardViewport.getScrollBarThickness());
    boardViewport.setViewPosition (scroll);
    repaint();
}

void WoodshedEditor::refreshPresetList()
{
    presetFiles = WoodshedProcessor::presetFolder().findChildFiles (juce::File::findFiles, false, "*.woodshed");
    std::sort (presetFiles.begin(), presetFiles.end(),
               [] (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()) < 0; });

    presetChoice.clear (juce::dontSendNotification);
    for (int i = 0; i < presetFiles.size(); ++i)
        presetChoice.addItem (presetFiles[i].getFileNameWithoutExtension(), i + 1);

    if (! presetFiles.isEmpty())
        presetChoice.addSeparator();
    presetChoice.addItem ("Default board", 9999);
}

void WoodshedEditor::savePresetAs()
{
    auto* window = new juce::AlertWindow ("Save preset", "Name this board:", juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor ("name", presetChoice.getSelectedId() > 0 && presetChoice.getSelectedId() != 9999 ? presetChoice.getText() : juce::String());
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
            statusFrames = 90;
        }
    }), true);
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
                text << "  ·  " << xruns << " dropouts";
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

    if (++frame % 15 == 0)
    {
        updateLatencyText();

        // Keep the stomp shortcuts working: if the window is active but nothing has focus, take it.
        if (auto* peer = getPeer(); peer != nullptr && peer->isFocused() && ! hasKeyboardFocus (true))
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
        statusFrames = 150;
    }

    if (statusFrames > 0 && --statusFrames == 0)
        statusLabel.setText ({}, juce::dontSendNotification);
}
