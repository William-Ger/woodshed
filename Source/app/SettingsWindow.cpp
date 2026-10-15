#include "SettingsWindow.h"
#include "WindowStyle.h"

namespace woodshed::app
{

namespace
{
// A Mac preferences form: right-aligned labels in one column, controls in the next, every row
// the same height, sections introduced by a semibold header and separated by hairlines.
constexpr int pageWidth = 600;
constexpr int margin = 28;
constexpr int rowHeight = 26;
constexpr int rowGap = 10;
constexpr int stripHeight = 34;          // the unified title bar, level with the window buttons

/** Where the label column ends and the control column starts, matching the stock device
    selector's own 35 / 60 split so every row on both pages lines up. */
struct Columns
{
    explicit Columns (int width)
    {
        const int formWidth = width - 2 * margin;
        controlX = margin + juce::roundToInt ((float) formWidth * 0.35f);
        controlRight = margin + juce::roundToInt ((float) formWidth * 0.95f);
        labelRight = controlX - 10;
    }

    juce::Rectangle<int> label (int y) const       { return { margin, y, labelRight - margin, rowHeight }; }
    juce::Rectangle<int> control (int y) const     { return { controlX, y, controlRight - controlX, rowHeight }; }

    int controlX = 0, controlRight = 0, labelRight = 0;
};

void drawSectionHeader (juce::Graphics& g, int y, const juce::String& title, const juce::String& detail, int width)
{
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, theme::Weight::semibold));
    g.drawText (title, margin, y, width - 2 * margin, 18, juce::Justification::centredLeft);

    if (detail.isNotEmpty())
    {
        g.setColour (theme::faintText);
        g.setFont (theme::font (12.0f));
        g.drawText (detail, margin, y + 20, width - 2 * margin, 16, juce::Justification::centredLeft);
    }
}

void drawFormLabel (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text)
{
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f));
    g.drawText (text, area, juce::Justification::centredRight);
}

void drawHairline (juce::Graphics& g, int y, int width)
{
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.fillRect (margin, y, width - 2 * margin, 1);
}

/** Gives the stock device selector's labels, combos and lists the form's type and colours. */
void styleSelector (juce::Component& parent)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && dynamic_cast<juce::ComboBox*> (&parent) == nullptr)
        {
            label->setFont (theme::font (13.0f));
            label->setColour (juce::Label::textColourId, theme::text);
            label->setJustificationType (juce::Justification::centredRight);
            label->setBorderSize ({ 0, 0, 0, 10 });
        }
        else if (auto* list = dynamic_cast<juce::ListBox*> (child))
        {
            list->setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff141416));
            list->setColour (juce::ListBox::outlineColourId, juce::Colours::white.withAlpha (0.06f));
            list->setOutlineThickness (1);
        }

        styleSelector (*child);
    }
}

//==============================================================================
/** Inputs, outputs, rate and buffer, plus what that adds up to in latency. */
class AudioPage final : public juce::Component,
                        private juce::ChangeListener
{
public:
    explicit AudioPage (Studio& s)
        : studio (s),
          selector (s.deviceManager, 1, 2, 1, 2, false, false, false, false)
    {
        selector.setItemHeight (rowHeight);
        addAndMakeVisible (selector);
        styleSelector (selector);
        studio.deviceManager.addChangeListener (this);
        updateStatus();
    }

    ~AudioPage() override
    {
        studio.deviceManager.removeChangeListener (this);
    }

    /** The height the page needs for the current device's controls. */
    int getIdealHeight() const                       { return latencyY() + rowHeight + margin; }

    void paint (juce::Graphics& g) override
    {
        drawSectionHeader (g, 22, "Audio interface", "Where your guitar comes in and where you listen. Woodshed remembers this.", getWidth());

        const Columns cols (getWidth());
        drawHairline (g, latencyY() - 14, getWidth());
        drawFormLabel (g, cols.label (latencyY()), "Round-trip latency:");

        auto value = cols.control (latencyY());
        g.setColour (theme::text);
        g.setFont (theme::readout (13.0f, theme::Weight::medium));
        const int msWidth = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), latency);
        g.drawText (latency, value, juce::Justification::centredLeft);
        g.setColour (theme::dimText);
        g.setFont (theme::font (12.0f));
        g.drawText (detail, value.withTrimmedLeft (msWidth + 10), juce::Justification::centredLeft);
    }

    void resized() override
    {
        // The selector lays itself out (35 % labels, 60 % controls) and sets its own height.
        selector.setBounds (margin, 62, getWidth() - 2 * margin, 400);   // its first row sits 15 px down
        styleSelector (selector);
    }

private:
    int latencyY() const                             { return selector.getBottom() - rowHeight + 12; }   // the selector ends in a spare row

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        updateStatus();
        studio.saveAudioSettings();

        // The selector rebuilds its controls on a device change; restyle once it has.
        juce::Component::SafePointer<AudioPage> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
            {
                styleSelector (safe->selector);
                safe->repaint();
                if (auto* parent = safe->getParentComponent())
                    parent->resized();
            }
        });
    }

    void updateStatus()
    {
        if (auto* d = studio.deviceManager.getCurrentAudioDevice())
        {
            const double rate = d->getCurrentSampleRate();
            const int buffer = d->getCurrentBufferSizeSamples();
            const double ms = 1000.0 * (d->getInputLatencyInSamples() + d->getOutputLatencyInSamples() + buffer) / juce::jmax (1.0, rate);
            latency = juce::String (ms, 1) + " ms";
            detail = juce::String (buffer) + " samples at " + juce::String (rate / 1000.0, 1) + " kHz";
        }
        else
        {
            latency = juce::String::fromUTF8 ("\xe2\x80\x94");
            detail = "No audio device is open";
        }
        repaint();
    }

    Studio& studio;
    juce::AudioDeviceSelectorComponent selector;
    juce::String latency, detail;
};

//==============================================================================
/** Where projects live, what opens at launch, and the click. */
class GeneralPage final : public juce::Component
{
public:
    explicit GeneralPage (Studio& s) : studio (s)
    {
        auto* props = studio.settings().getUserSettings();

        folderLabel.setFont (theme::font (12.0f));
        folderLabel.setColour (juce::Label::textColourId, theme::dimText);
        folderLabel.setMinimumHorizontalScale (0.6f);
        folderLabel.setBorderSize ({ 0, 10, 0, 10 });
        addAndMakeVisible (folderLabel);

        chooseFolder.onClick = [this] { pickFolder(); };
        addAndMakeVisible (chooseFolder);
        resetFolder.setTooltip ("Go back to the standard projects folder");
        resetFolder.onClick = [this]
        {
            studio.settings().getUserSettings()->removeValue ("projectsFolder");
            daw::Project::setDefaultProjectsFolder ({});
            refreshFolder();
        };
        addAndMakeVisible (resetFolder);

        reopenLast.setToggleState (props->getBoolValue ("reopenLast", true), juce::dontSendNotification);
        reopenLast.onClick = [this] { studio.settings().getUserSettings()->setValue ("reopenLast", reopenLast.getToggleState()); };
        addAndMakeVisible (reopenLast);

        clickLevel.setSliderStyle (juce::Slider::LinearHorizontal);
        clickLevel.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, rowHeight - 4);
        clickLevel.setRange (-36.0, 0.0, 0.5);
        clickLevel.setTextValueSuffix (" dB");
        clickLevel.setValue (juce::Decibels::gainToDecibels (studio.engine().getClickLevel(), -36.0f), juce::dontSendNotification);
        clickLevel.onValueChange = [this]
        {
            const float db = (float) clickLevel.getValue();
            studio.engine().setClickLevel (juce::Decibels::decibelsToGain (db, -36.0f));
            studio.settings().getUserSettings()->setValue ("clickDb", db);
        };
        clickLevel.setDoubleClickReturnValue (true, -12.0);
        addAndMakeVisible (clickLevel);

        refreshFolder();
    }

    int getIdealHeight() const                       { return metronomeY + 44 + rowHeight + margin; }

    void paint (juce::Graphics& g) override
    {
        const Columns cols (getWidth());
        drawSectionHeader (g, 22, "Projects", "New projects and their recordings are saved here.", getWidth());
        drawFormLabel (g, cols.label (folderY), "Projects folder:");
        theme::drawInset (g, folderLabel.getBounds().toFloat(), theme::radiusControl);
        drawFormLabel (g, cols.label (folderY + rowHeight + rowGap), "At launch:");

        drawHairline (g, metronomeY - 18, getWidth());
        drawSectionHeader (g, metronomeY, "Metronome", {}, getWidth());
        drawFormLabel (g, cols.label (metronomeY + 30), "Click volume:");
    }

    void resized() override
    {
        const Columns cols (getWidth());

        // The path gets its own full-width row so long folders stay readable.
        auto folderRow = cols.control (folderY);
        resetFolder.setBounds (folderRow.removeFromRight (66));
        folderRow.removeFromRight (6);
        chooseFolder.setBounds (folderRow.removeFromRight (78));
        folderRow.removeFromRight (8);
        folderLabel.setBounds (folderRow);

        reopenLast.setBounds (cols.control (folderY + rowHeight + rowGap).withX (cols.controlX - 2).withRight (getWidth() - margin));
        clickLevel.setBounds (cols.control (metronomeY + 30));

        // The value box reads like the studio's other readouts.
        for (auto* child : clickLevel.getChildren())
            if (auto* box = dynamic_cast<juce::Label*> (child))
                box->setFont (theme::readout (12.0f, theme::Weight::medium));
    }

private:
    static constexpr int folderY = 76;
    static constexpr int metronomeY = folderY + 2 * rowHeight + rowGap + 34;

    void refreshFolder()
    {
        const auto folder = daw::Project::defaultProjectsFolder();
        folderLabel.setText (folder.getFileName(), juce::dontSendNotification);
        folderLabel.setTooltip (folder.getFullPathName());
        folderLabel.setColour (juce::Label::textColourId, theme::text);
    }

    void pickFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose where to keep projects", daw::Project::defaultProjectsFolder());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  const auto folder = fc.getResult();
                                  if (folder == juce::File() || ! folder.isDirectory())
                                      return;
                                  studio.settings().getUserSettings()->setValue ("projectsFolder", folder.getFullPathName());
                                  daw::Project::setDefaultProjectsFolder (folder);
                                  refreshFolder();
                              });
    }

    Studio& studio;
    juce::Label folderLabel;
    juce::TextButton chooseFolder { juce::String::fromUTF8 ("Choose\u2026") }, resetFolder { "Default" };
    juce::ToggleButton reopenLast { "Reopen the last project" };
    juce::Slider clickLevel;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
/** A Mac segmented control: an inset track with the chosen segment raised inside it. */
class SegmentedControl final : public juce::Component
{
public:
    explicit SegmentedControl (juce::StringArray names) : segments (std::move (names))
    {
        setRepaintsOnMouseActivity (true);
    }

    int getIdealWidth() const
    {
        int w = 4;
        for (auto& s : segments)
            w += juce::GlyphArrangement::getStringWidthInt (theme::font (13.0f, theme::Weight::medium), s) + 32;
        return w;
    }

    void setSelected (int index)
    {
        if (index == selected || ! juce::isPositiveAndBelow (index, segments.size()))
            return;
        selected = index;
        repaint();
        if (onChange)
            onChange (selected);
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        theme::drawInset (g, r, theme::radiusControl + 1.0f, juce::Colour (0xff18181b));

        for (int i = 0; i < segments.size(); ++i)
        {
            const auto seg = segmentBounds (i);
            const bool on = i == selected;
            const bool over = isMouseOver() && seg.contains (getMouseXYRelative().toFloat());

            if (on)
                theme::drawRaised (g, seg.reduced (2.0f), theme::radiusControl, theme::controlHover);
            else if (over)
            {
                g.setColour (juce::Colours::white.withAlpha (isMouseButtonDown() ? 0.08f : 0.04f));
                g.fillRoundedRectangle (seg.reduced (2.0f), theme::radiusControl);
            }

            g.setColour (on ? theme::text : over ? theme::text.withAlpha (0.85f) : theme::dimText);
            g.setFont (theme::font (13.0f, theme::Weight::medium));
            g.drawText (segments[i], seg, juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < segments.size(); ++i)
            if (segmentBounds (i).contains (e.position))
                setSelected (i);
    }

    std::function<void (int)> onChange;

private:
    juce::Rectangle<float> segmentBounds (int i) const
    {
        const float w = (float) getWidth() / (float) segments.size();
        return { (float) i * w, 0.0f, w, (float) getHeight() };
    }

    juce::StringArray segments;
    int selected = 0;
};

//==============================================================================
class SettingsContent final : public juce::Component
{
public:
    explicit SettingsContent (Studio& s) : audio (s), general (s)
    {
        tabs.onChange = [this] (int index) { current = index; showPage(); };
        addAndMakeVisible (tabs);
        addChildComponent (audio);
        addChildComponent (general);
        audio.setSize (pageWidth, 600);
        general.setSize (pageWidth, 600);
        showPage();

    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);

        // The title bar strip the window buttons float over, with the page switch in its middle.
        auto strip = getLocalBounds().removeFromTop (stripHeight);
        g.setColour (theme::bar);
        g.fillRect (strip);
        g.setColour (theme::separator);
        g.fillRect (strip.removeFromBottom (1));
    }

    void resized() override
    {
        const int w = tabs.getIdealWidth();
        tabs.setBounds ((getWidth() - w) / 2, 4, w, 24);

        auto body = getLocalBounds().withTrimmedTop (stripHeight);
        audio.setBounds (body);
        general.setBounds (body);

        // Like a Mac preferences window, the window fits the page shown.
        const int wanted = stripHeight + (audio.isVisible() ? audio.getIdealHeight() : general.getIdealHeight());
        if (getHeight() != wanted)
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SettingsContent> (this), wanted]
            {
                if (safe != nullptr)
                    safe->setSize (pageWidth, wanted);
            });
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.y < stripHeight)
            startWindowDrag (*this, e);
    }

    void showTab (int index)                         { tabs.setSelected (index); }

private:
    void showPage()
    {
        const bool showAudio = current == 0;
        audio.setVisible (showAudio);
        general.setVisible (! showAudio);
        resized();
    }

    SegmentedControl tabs { { "Audio", "General" } };
    AudioPage audio;
    GeneralPage general;
    int current = 0;
};
}

//==============================================================================
SettingsWindow::SettingsWindow (Studio& s)
    : juce::DocumentWindow ("Settings", theme::panel, juce::DocumentWindow::closeButton), studio (s)
{
    // The studio's default look applies (no window-local LookAndFeel), so the stock device
    // selector gets the same combos, tick boxes and buttons as the rest of the app.
    setUsingNativeTitleBar (true);
    auto* content = new SettingsContent (studio);
    content->setSize (pageWidth, 420);
    setContentOwned (content, true);
    setResizable (false, false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
    useUnifiedTitleBar (*this);
    content->resized();
}

SettingsWindow::~SettingsWindow()
{
    clearContentComponent();
}

void SettingsWindow::closeButtonPressed()
{
    studio.closeSettings();
}

} // namespace woodshed::app
