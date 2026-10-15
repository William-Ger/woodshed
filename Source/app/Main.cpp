// Woodshed: a multitrack recording studio built around the guitar rig.

#include "MainComponent.h"
#include "StudioLookAndFeel.h"
#include "WindowStyle.h"

namespace woodshed::app
{

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& name, Studio& studio)
        : juce::DocumentWindow (name, theme::window, juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        auto* content = new MainComponent (studio);
        setContentOwned (content, false);

       #if JUCE_MAC
        // Settings... sits in the app menu, under About, like every Mac app.
        juce::PopupMenu appMenu;
        appMenu.addCommandItem (&studio.commands, cmd::audioSettings);
        juce::MenuBarModel::setMacMainMenu (content, &appMenu);
       #else
        setMenuBar (content);
       #endif

        setResizable (true, true);
        setResizeLimits (1100, 640, 8192, 8192);
        centreWithSize (1440, 900);
        setVisible (true);
        useUnifiedTitleBar (*this);
        content->resized();
        content->grabKeyboardFocus();
    }

    ~MainWindow() override
    {
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
       #else
        setMenuBar (nullptr);
       #endif
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class WoodshedStudioApp final : public juce::JUCEApplication
{
public:
    WoodshedStudioApp()
    {
        juce::PropertiesFile::Options options;
        options.applicationName     = "Woodshed";
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
       #if JUCE_LINUX || JUCE_BSD
        options.folderName          = "~/.config";
       #endif
        settings.setStorageParameters (options);
    }

    const juce::String getApplicationName() override       { return "Woodshed"; }
    const juce::String getApplicationVersion() override    { return "0.2.0"; }
    bool moreThanOneInstanceAllowed() override             { return false; }

    void initialise (const juce::String&) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        studio = std::make_unique<Studio> (settings);
        window = std::make_unique<MainWindow> ("Woodshed", *studio);
    }

    void shutdown() override
    {
        window = nullptr;
        studio = nullptr;      // saves the project and the audio setup
        settings.saveIfNeeded();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
            juce::Timer::callAfterDelay (100, [] { if (auto* app = getInstance()) app->systemRequestedQuit(); });
        else
            quit();
    }

private:
    juce::ApplicationProperties settings;
    StudioLookAndFeel lookAndFeel;
    std::unique_ptr<Studio> studio;
    std::unique_ptr<MainWindow> window;
};

} // namespace woodshed::app

START_JUCE_APPLICATION (woodshed::app::WoodshedStudioApp)
