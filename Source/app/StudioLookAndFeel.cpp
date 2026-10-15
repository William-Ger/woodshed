#include "StudioLookAndFeel.h"
#include "Icons.h"

namespace woodshed::app
{

using namespace woodshed;

StudioLookAndFeel::StudioLookAndFeel()
{
    auto scheme = getDarkColourScheme();
    scheme.setUIColour (ColourScheme::windowBackground, theme::window);
    scheme.setUIColour (ColourScheme::widgetBackground, theme::control);
    scheme.setUIColour (ColourScheme::menuBackground, juce::Colour (0xff26262b));
    scheme.setUIColour (ColourScheme::outline, theme::outline);
    scheme.setUIColour (ColourScheme::defaultText, theme::text);
    scheme.setUIColour (ColourScheme::defaultFill, theme::accent);
    scheme.setUIColour (ColourScheme::highlightedText, juce::Colour (0xff1a1206));
    scheme.setUIColour (ColourScheme::highlightedFill, theme::accent);
    scheme.setUIColour (ColourScheme::menuText, theme::text);
    setColourScheme (scheme);

    setColour (juce::ResizableWindow::backgroundColourId, theme::window);
    setColour (juce::DocumentWindow::backgroundColourId, theme::window);
    setColour (juce::TextButton::buttonColourId, theme::control);
    setColour (juce::TextButton::buttonOnColourId, theme::accentDim);
    setColour (juce::TextButton::textColourOffId, theme::text);
    setColour (juce::TextButton::textColourOnId, theme::accent);
    setColour (juce::ComboBox::backgroundColourId, theme::control);
    setColour (juce::ComboBox::outlineColourId, theme::separator);
    setColour (juce::ComboBox::textColourId, theme::text);
    setColour (juce::ComboBox::arrowColourId, theme::dimText);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xf026262b));
    setColour (juce::PopupMenu::textColourId, theme::text);
    setColour (juce::PopupMenu::headerTextColourId, theme::faintText);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::accent);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colour (0xff1a1206));
    setColour (juce::Label::textColourId, theme::text);
    setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111113));
    setColour (juce::TextEditor::textColourId, theme::text);
    setColour (juce::TextEditor::highlightColourId, theme::accent.withAlpha (0.35f));
    setColour (juce::TextEditor::highlightedTextColourId, theme::text);
    setColour (juce::TextEditor::outlineColourId, theme::separator);
    setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
    setColour (juce::CaretComponent::caretColourId, theme::accent);
    setColour (juce::ToggleButton::textColourId, theme::text);
    setColour (juce::ToggleButton::tickColourId, juce::Colour (0xff1a1206));
    setColour (juce::ToggleButton::tickDisabledColourId, theme::faintText);
    setColour (juce::Slider::thumbColourId, theme::text);
    setColour (juce::Slider::trackColourId, theme::accent);
    setColour (juce::Slider::backgroundColourId, theme::control);
    setColour (juce::Slider::rotarySliderFillColourId, theme::accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, theme::control);
    setColour (juce::Slider::textBoxTextColourId, theme::text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff111113));
    setColour (juce::Slider::textBoxOutlineColourId, theme::separator);
    setColour (juce::ScrollBar::thumbColourId, juce::Colours::white.withAlpha (0.28f));
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xf02c2c31));
    setColour (juce::TooltipWindow::textColourId, theme::text);
    setColour (juce::TooltipWindow::outlineColourId, theme::separator);
    setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff111113));
    setColour (juce::ListBox::outlineColourId, theme::separator);
    setColour (juce::TabbedComponent::backgroundColourId, theme::panel);
    setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TabbedButtonBar::tabTextColourId, theme::dimText);
    setColour (juce::TabbedButtonBar::frontTextColourId, theme::text);
    setColour (juce::AlertWindow::backgroundColourId, theme::panel);
    setColour (juce::AlertWindow::textColourId, theme::text);
    setColour (juce::AlertWindow::outlineColourId, theme::separator);

}

juce::Typeface::Ptr StudioLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    const auto name = f.getTypefaceName();

    if (name == juce::Font::getDefaultSansSerifFontName())
    {
        const auto style = f.getTypefaceStyle();
        if (f.isBold() || style.containsIgnoreCase ("bold"))
            return theme::typeface (style.containsIgnoreCase ("semi") ? theme::Weight::semibold : theme::Weight::semibold);
        if (style.containsIgnoreCase ("medium"))
            return theme::typeface (theme::Weight::medium);
        return theme::typeface (theme::Weight::regular);
    }

    if (name == juce::Font::getDefaultMonospacedFontName())
        return theme::monoTypeface (f.isBold() ? theme::Weight::semibold : theme::Weight::medium);

    return LookAndFeel_V4::getTypefaceForFont (f);
}

//==============================================================================
void StudioLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& base,
                                              bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool primary = (bool) b.getProperties().getWithDefault ("primary", false);
    const bool borderless = (bool) b.getProperties().getWithDefault ("borderless", false);
    const bool on = b.getToggleState();
    const float radius = juce::jmin (theme::radiusControl, r.getHeight() * 0.5f);
    const float alpha = b.isEnabled() ? 1.0f : 0.45f;

    if (primary)
    {
        auto fill = theme::accent;
        if (down) fill = fill.darker (0.2f); else if (highlighted) fill = fill.brighter (0.08f);
        g.setColour (fill.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (r, radius);
        return;
    }

    if (on)
    {
        g.setColour (theme::accent.withAlpha ((down ? 0.32f : highlighted ? 0.26f : 0.2f) * alpha));
        g.fillRoundedRectangle (r, radius);
        g.setColour (theme::accent.withAlpha (0.35f * alpha));
        g.drawRoundedRectangle (r, radius, 1.0f);
        return;
    }

    if (borderless)
    {
        if (highlighted || down)
        {
            g.setColour (juce::Colours::white.withAlpha (down ? 0.12f : 0.07f));
            g.fillRoundedRectangle (r, radius);
        }
        return;
    }

    auto fill = base == findColour (juce::TextButton::buttonColourId) ? theme::control : base;
    if (down) fill = theme::controlDown; else if (highlighted) fill = theme::controlHover;
    theme::drawRaised (g, r, radius, fill.withMultipliedAlpha (alpha));
}

void StudioLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    const bool primary = (bool) b.getProperties().getWithDefault ("primary", false);
    auto colour = primary ? juce::Colour (0xff1a1206)
                : b.getToggleState() ? theme::accent
                : b.findColour (juce::TextButton::textColourOffId);
    if (! b.isEnabled())
        colour = colour.withMultipliedAlpha (0.45f);

    g.setColour (colour);
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (8, 0), juce::Justification::centred, 1, 0.8f);
}

juce::Font StudioLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return theme::font (juce::jlimit (11.0f, 13.5f, (float) buttonHeight * 0.48f), theme::Weight::medium);
}

void StudioLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool down)
{
    const float box = juce::jmin (16.0f, (float) b.getHeight() - 4.0f);
    drawTickBox (g, b, 2.0f, ((float) b.getHeight() - box) * 0.5f, box, box,
                 b.getToggleState(), b.isEnabled(), highlighted, down);

    g.setColour (b.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
    g.setFont (theme::font (13.0f));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft ((int) box + 10).withTrimmedRight (2),
                      juce::Justification::centredLeft, 1);
}

void StudioLookAndFeel::drawTickBox (juce::Graphics& g, juce::Component&, float x, float y, float w, float h,
                                     bool ticked, bool enabled, bool highlighted, bool)
{
    const juce::Rectangle<float> r (x, y, w, h);
    if (ticked)
    {
        g.setColour (theme::accent.withMultipliedAlpha (enabled ? 1.0f : 0.5f));
        g.fillRoundedRectangle (r, theme::radiusChip);
        juce::Path tick;
        tick.startNewSubPath (r.getX() + w * 0.26f, r.getY() + h * 0.52f);
        tick.lineTo (r.getX() + w * 0.44f, r.getY() + h * 0.70f);
        tick.lineTo (r.getX() + w * 0.76f, r.getY() + h * 0.32f);
        g.setColour (juce::Colour (0xff1a1206));
        g.strokePath (tick, juce::PathStrokeType (juce::jmax (1.5f, w * 0.12f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else
    {
        theme::drawInset (g, r, theme::radiusChip, highlighted ? juce::Colour (0xff1b1b1f) : juce::Colour (0xff141416));
        g.setColour (theme::outline);
        g.drawRoundedRectangle (r.reduced (0.5f), theme::radiusChip, 1.0f);
    }
}

//==============================================================================
void StudioLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool down, int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    const bool over = box.isMouseOver (true);
    theme::drawRaised (g, r, juce::jmin (theme::radiusControl, r.getHeight() * 0.5f),
                       down ? theme::controlDown : over ? theme::controlHover : box.findColour (juce::ComboBox::backgroundColourId));

    const auto arrow = juce::Rectangle<float> ((float) width - 22.0f, 0.0f, 16.0f, (float) height);
    icons::draw (g, icons::chevronDown(), arrow.withSizeKeepingCentre (14.0f, 14.0f),
                 box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.4f), 2.0f);
}

juce::Font StudioLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return theme::font (juce::jlimit (11.0f, 13.5f, (float) box.getHeight() * 0.5f), theme::Weight::medium);
}

void StudioLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (4, 1, box.getWidth() - 26, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void StudioLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
    g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawRect (r, 1.0f);
}

void StudioLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                           bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                           const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        g.setColour (juce::Colours::white.withAlpha (0.09f));
        g.fillRect (area.reduced (10, 0).withHeight (1).withY (area.getCentreY()));
        return;
    }

    auto r = area.reduced (5, 1).toFloat();
    if (isHighlighted && isActive)
    {
        g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
        g.fillRoundedRectangle (r, 5.0f);
    }

    const auto ink = isHighlighted && isActive ? findColour (juce::PopupMenu::highlightedTextColourId)
                   : textColour != nullptr ? *textColour
                   : findColour (juce::PopupMenu::textColourId);
    const auto colour = isActive ? ink : ink.withMultipliedAlpha (0.4f);

    auto content = r.reduced (8.0f, 0.0f);
    auto lead = content.removeFromLeft (18.0f);

    if (isTicked)
    {
        juce::Path tick;
        const auto c = lead.withSizeKeepingCentre (11.0f, 11.0f);
        tick.startNewSubPath (c.getX(), c.getCentreY());
        tick.lineTo (c.getX() + c.getWidth() * 0.38f, c.getBottom() - 1.0f);
        tick.lineTo (c.getRight(), c.getY() + 1.0f);
        g.setColour (colour);
        g.strokePath (tick, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else if (icon != nullptr)
    {
        icon->drawWithin (g, lead.withSizeKeepingCentre (13.0f, 13.0f), juce::RectanglePlacement::centred, isActive ? 1.0f : 0.4f);
    }

    content.removeFromLeft (4.0f);
    g.setFont (getPopupMenuFont());
    g.setColour (colour);

    if (hasSubMenu)
    {
        juce::Path chevron;
        const auto c = content.removeFromRight (10.0f).withSizeKeepingCentre (6.0f, 10.0f);
        chevron.startNewSubPath (c.getX(), c.getY());
        chevron.lineTo (c.getRight(), c.getCentreY());
        chevron.lineTo (c.getX(), c.getBottom());
        g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour (colour.withMultipliedAlpha (isHighlighted ? 0.75f : 0.5f));
        g.drawText (shortcutKeyText, content, juce::Justification::centredRight);
        g.setColour (colour);
    }

    g.drawFittedText (text, content.toNearestInt(), juce::Justification::centredLeft, 1);
}

void StudioLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& text)
{
    g.setColour (findColour (juce::PopupMenu::headerTextColourId));
    g.setFont (theme::font (11.0f, theme::Weight::semibold));
    g.drawText (text.toUpperCase(), area.reduced (14, 0).withTrimmedTop (4), juce::Justification::bottomLeft);
}

juce::Font StudioLookAndFeel::getPopupMenuFont()
{
    return theme::font (13.0f);
}

void StudioLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardHeight, int& w, int& h)
{
    if (isSeparator)
    {
        w = 50;
        h = 9;
        return;
    }

    h = standardHeight > 0 ? standardHeight : 24;
    w = juce::GlyphArrangement::getStringWidthInt (getPopupMenuFont(), text) + 64;
}

//==============================================================================
void StudioLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool vertical,
                                       int thumbStart, int thumbSize, bool over, bool down)
{
    // Overlay style: no track, a thin pill that thickens under the mouse.
    juce::Rectangle<float> thumb;
    const float thickness = (over || down) ? 7.0f : 5.0f;
    if (vertical)
        thumb = { (float) x + (float) width - thickness - 2.0f, (float) (y + thumbStart) + 2.0f, thickness, (float) thumbSize - 4.0f };
    else
        thumb = { (float) (x + thumbStart) + 2.0f, (float) y + (float) height - thickness - 2.0f, (float) thumbSize - 4.0f, thickness };

    g.setColour (juce::Colours::white.withAlpha (down ? 0.45f : over ? 0.36f : 0.22f));
    g.fillRoundedRectangle (thumb, thickness * 0.5f);
}

void StudioLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                                          juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, minPos, maxPos, style, s);
        return;
    }

    const bool horizontal = style == juce::Slider::LinearHorizontal;
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat();
    const float track = 4.0f;
    const auto groove = horizontal ? bounds.withSizeKeepingCentre (bounds.getWidth(), track)
                                   : bounds.withSizeKeepingCentre (track, bounds.getHeight());
    theme::drawInset (g, groove, track * 0.5f);

    auto filled = groove;
    if (horizontal) filled.setRight (pos); else filled.setTop (pos);
    g.setColour (s.findColour (juce::Slider::trackColourId).withMultipliedAlpha (s.isEnabled() ? 1.0f : 0.4f));
    g.fillRoundedRectangle (filled, track * 0.5f);

    const float knob = 14.0f;
    const auto centre = horizontal ? juce::Point<float> (pos, bounds.getCentreY()) : juce::Point<float> (bounds.getCentreX(), pos);
    const auto thumb = juce::Rectangle<float> (knob, knob).withCentre (centre);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillEllipse (thumb.translated (0.0f, 1.0f));
    g.setColour (s.isMouseButtonDown() ? juce::Colour (0xffd8d8de) : juce::Colours::white);
    g.fillEllipse (thumb);
}

void StudioLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (2.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = start + pos * (end - start);

    juce::Path track, value;
    track.addCentredArc (centre.x, centre.y, radius - 2.0f, radius - 2.0f, 0.0f, start, end, true);
    value.addCentredArc (centre.x, centre.y, radius - 2.0f, radius - 2.0f, 0.0f, start, angle, true);
    g.setColour (theme::control);
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId));
    g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float capR = radius - 6.0f;
    juce::ColourGradient cap (juce::Colour (0xff4a4a51), centre.x, centre.y - capR, juce::Colour (0xff2c2c31), centre.x, centre.y + capR, false);
    g.setGradientFill (cap);
    g.fillEllipse (juce::Rectangle<float> (capR * 2.0f, capR * 2.0f).withCentre (centre));
    g.setColour (theme::text);
    g.drawLine ({ centre.getPointOnCircumference (capR * 0.35f, angle), centre.getPointOnCircumference (capR * 0.85f, angle) }, 2.0f);
}

void StudioLookAndFeel::drawTabButton (juce::TabBarButton& b, juce::Graphics& g, bool over, bool)
{
    // Segmented-control segments.
    const auto r = b.getActiveArea().toFloat().reduced (2.0f, 4.0f);
    const bool front = b.isFrontTab();
    if (front)
        theme::drawRaised (g, r, theme::radiusControl, theme::controlHover);
    else if (over)
    {
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRoundedRectangle (r, theme::radiusControl);
    }

    g.setColour (front ? theme::text : theme::dimText);
    g.setFont (theme::font (13.0f, theme::Weight::medium));
    g.drawText (b.getButtonText(), r, juce::Justification::centred);
}

void StudioLookAndFeel::drawTabAreaBehindFrontButton (juce::TabbedButtonBar& bar, juce::Graphics& g, int w, int h)
{
    g.setColour (theme::bar);
    g.fillRect (0, 0, w, h);
    g.setColour (theme::separator);
    g.fillRect (0, h - 1, w, 1);
    juce::ignoreUnused (bar);
}

int StudioLookAndFeel::getTabButtonBestWidth (juce::TabBarButton& b, int)
{
    return juce::GlyphArrangement::getStringWidthInt (theme::font (13.0f, theme::Weight::medium), b.getButtonText()) + 36;
}

juce::Rectangle<int> StudioLookAndFeel::getTooltipBounds (const juce::String& tip, juce::Point<int> pos, juce::Rectangle<int> parent)
{
    const int w = juce::jmin (360, juce::GlyphArrangement::getStringWidthInt (theme::font (12.0f), tip) + 20);
    const int h = 26;
    return juce::Rectangle<int> (pos.x > parent.getCentreX() ? pos.x - (w + 12) : pos.x + 14,
                                 pos.y > parent.getCentreY() ? pos.y - (h + 8) : pos.y + 18, w, h)
               .constrainedWithin (parent);
}

void StudioLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
    g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (juce::Colours::white.withAlpha (0.1f));
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    g.setColour (findColour (juce::TooltipWindow::textColourId));
    g.setFont (theme::font (12.0f));
    g.drawFittedText (text, r.reduced (10.0f, 2.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
}

void StudioLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& e)
{
    theme::drawInset (g, juce::Rectangle<int> (width, height).toFloat(), theme::radiusControl, e.findColour (juce::TextEditor::backgroundColourId));
}

void StudioLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& e)
{
    if (e.hasKeyboardFocus (true) && ! e.isReadOnly())
    {
        g.setColour (theme::accent.withAlpha (0.8f));
        g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (1.0f), theme::radiusControl, 2.0f);
    }
}

void StudioLookAndFeel::drawStretchableLayoutResizerBar (juce::Graphics& g, int w, int h, bool vertical, bool over, bool dragging)
{
    g.fillAll (theme::separator);
    if (over || dragging)
    {
        g.setColour (theme::accent.withAlpha (dragging ? 0.7f : 0.4f));
        if (vertical) g.fillRect (w / 2 - 1, 0, 2, h); else g.fillRect (0, h / 2 - 1, w, 2);
    }
}

void StudioLookAndFeel::drawCallOutBoxBackground (juce::CallOutBox& box, juce::Graphics& g, const juce::Path& path, juce::Image&)
{
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillPath (path, juce::AffineTransform::translation (0.0f, 2.0f));
    g.setColour (juce::Colour (0xf026262b)); juce::ignoreUnused (box);
    g.fillPath (path);
    g.setColour (juce::Colours::white.withAlpha (0.1f));
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

} // namespace woodshed::app
