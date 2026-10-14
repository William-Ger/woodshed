#include "SettingsWindow.h"

namespace woodshed::app
{

namespace
{
/** Inputs, outputs, rate and buffer, plus what that adds up to in latency. */
class AudioPage final : public juce::Component,
                        private juce::ChangeListener
{
public:
    explicit AudioPage (Studio& s)
        : studio (s),
          selector (s.deviceManager, 1, 2, 1, 2, false, false, false, false)
    {
        addAndMakeVisible (selector);
        studio.deviceManager.addChangeListener (this);
        updateStatus();
    }

    ~AudioPage() override
    {
        studio.deviceManager.removeChangeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);

        auto r = getLocalBounds().reduced (24, 16);
        g.setColour (theme::text);
        g.setFont (theme::font (15.0f, true));
        g.drawText ("Audio interface", r.removeFromTop (22), juce::Justification::centredLeft);
        g.setColour (theme::dimText);
        g.setFont (theme::font (12.5f));
        g.drawText ("Pick where your guitar comes in and where you listen. Woodshed remembers this.",
                    r.removeFromTop (20), juce::Justification::centredLeft);

        auto footer = getLocalBounds().reduced (24, 12).removeFromBottom (22);
        g.setColour (theme::accent);
        g.setFont (theme::font (13.0f, true));
        g.drawText (status, footer, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 0);
        r.removeFromTop (64);
        r.removeFromBottom (40);
        selector.setBounds (r);
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        updateStatus();
        studio.saveAudioSettings();
    }

    void updateStatus()
    {
        if (auto* d = studio.deviceManager.getCurrentAudioDevice())
        {
            const double rate = d->getCurrentSampleRate();
            const int buffer = d->getCurrentBufferSizeSamples();
            const double ms = 1000.0 * (d->getInputLatencyInSamples() + d->getOutputLatencyInSamples() + buffer) / juce::jmax (1.0, rate);
            const auto dot = juce::String::fromUTF8 ("   \xc2\xb7   ");
            status = "Round trip " + juce::String (ms, 1) + " ms" + dot + juce::String (buffer) + " samples at "
                   + juce::String (rate / 1000.0, 1) + " kHz";
        }
        else
        {
            status = "No audio device is open";
        }
        repaint();
    }

    Studio& studio;
    juce::AudioDeviceSelectorComponent selector;
    juce::String status;
};

/** Where projects live, what opens at launch, and the click. */
class GeneralPage final : public juce::Component
{
public:
    explicit GeneralPage (Studio& s) : studio (s)
    {
        auto* props = studio.settings().getUserSettings();

        folderLabel.setColour (juce::Label::textColourId, theme::text);
        folderLabel.setColour (juce::Label::backgroundColourId, theme::lane);
        folderLabel.setColour (juce::Label::outlineColourId, theme::outline);
        folderLabel.setMinimumHorizontalScale (0.6f);
        addAndMakeVisible (folderLabel);

        chooseFolder.onClick = [this] { pickFolder(); };
        addAndMakeVisible (chooseFolder);
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
        clickLevel.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
        clickLevel.setRange (-36.0, 0.0, 0.5);
        clickLevel.setTextValueSuffix (" dB");
        clickLevel.setValue (juce::Decibels::gainToDecibels (studio.engine().getClickLevel(), -36.0f), juce::dontSendNotification);
        clickLevel.onValueChange = [this]
        {
            const float db = (float) clickLevel.getValue();
            studio.engine().setClickLevel (juce::Decibels::decibelsToGain (db, -36.0f));
            studio.settings().getUserSettings()->setValue ("clickDb", db);
        };
        addAndMakeVisible (clickLevel);

        refreshFolder();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);
        g.setColour (theme::text);
        g.setFont (theme::font (15.0f, true));
        g.drawText ("Projects", 24, 16, 300, 22, juce::Justification::centredLeft);
        g.drawText ("Metronome", 24, metronomeY, 300, 22, juce::Justification::centredLeft);

        g.setColour (theme::dimText);
        g.setFont (theme::font (12.5f));
        g.drawText ("New projects and recordings are saved here.", 24, 38, 500, 20, juce::Justification::centredLeft);
        g.drawText ("Click volume", 24, metronomeY + 30, 120, 24, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto row = juce::Rectangle<int> (24, 64, getWidth() - 48, 28);
        resetFolder.setBounds (row.removeFromRight (90));
        row.removeFromRight (8);
        chooseFolder.setBounds (row.removeFromRight (100));
        row.removeFromRight (8);
        folderLabel.setBounds (row);

        reopenLast.setBounds (24, 104, getWidth() - 48, 26);
        metronomeY = 160;
        clickLevel.setBounds (150, metronomeY + 30, juce::jmin (360, getWidth() - 174), 24);
    }

private:
    void refreshFolder()
    {
        folderLabel.setText (daw::Project::defaultProjectsFolder().getFullPathName(), juce::dontSendNotification);
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
    juce::TextButton chooseFolder { "Choose..." }, resetFolder { "Default" };
    juce::ToggleButton reopenLast { "Reopen the last project when Woodshed starts" };
    juce::Slider clickLevel;
    std::unique_ptr<juce::FileChooser> chooser;
    int metronomeY = 160;
};

class SettingsContent final : public juce::Component
{
public:
    explicit SettingsContent (Studio& s) : tabs (juce::TabbedButtonBar::TabsAtTop)
    {
        tabs.setTabBarDepth (34);
        tabs.setOutline (0);
        tabs.addTab ("Audio", theme::panel, new AudioPage (s), true);
        tabs.addTab ("General", theme::panel, new GeneralPage (s), true);
        addAndMakeVisible (tabs);
        setSize (640, 560);
    }

    void paint (juce::Graphics& g) override        { g.fillAll (theme::bar); }
    void resized() override                         { tabs.setBounds (getLocalBounds()); }

private:
    juce::TabbedComponent tabs;
};
}

//==============================================================================
SettingsWindow::SettingsWindow (Studio& s)
    : juce::DocumentWindow ("Settings", theme::panel, juce::DocumentWindow::closeButton), studio (s)
{
    // The studio's dark palette for the stock device selector, combo boxes and lists.
    auto scheme = juce::LookAndFeel_V4::getDarkColourScheme();
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::windowBackground, theme::panel);
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::widgetBackground, theme::panelLight);
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::menuBackground, theme::panelLight);
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::outline, theme::outline);
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::defaultText, theme::text);
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::defaultFill, theme::accent);
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::highlightedText, juce::Colours::black);
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::highlightedFill, theme::accent);
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::menuText, theme::text);
    lookAndFeel.setColourScheme (scheme);
    lookAndFeel.setColour (juce::TabbedButtonBar::tabTextColourId, theme::dimText);
    lookAndFeel.setColour (juce::TabbedButtonBar::frontTextColourId, theme::text);
    setLookAndFeel (&lookAndFeel);

    setUsingNativeTitleBar (true);
    setContentOwned (new SettingsContent (studio), true);
    setResizable (true, false);
    setResizeLimits (560, 480, 1200, 1000);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
}

SettingsWindow::~SettingsWindow()
{
    clearContentComponent();
    setLookAndFeel (nullptr);
}

void SettingsWindow::closeButtonPressed()
{
    studio.closeSettings();
}

} // namespace woodshed::app
