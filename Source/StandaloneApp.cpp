// Woodshed's standalone app: JUCE's stock standalone wrapper, but it goes
// straight to a Focusrite/Scarlett interface at a small buffer on first launch
// and doesn't mute the input (you're on headphones, not laptop speakers).

#include <juce_audio_utils/juce_audio_utils.h>

#if JucePlugin_Build_Standalone

#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace
{
constexpr int preferredBufferSize = 64;
constexpr double preferredSampleRate = 48000.0;   // GarageBand-class rate; 192k just burns CPU

juce::String findInterface()
{
    juce::AudioDeviceManager probe;
    juce::OwnedArray<juce::AudioIODeviceType> types;
    probe.createAudioDeviceTypes (types);

    for (auto* type : types)
    {
       #if JUCE_WINDOWS
        if (type->getTypeName() != "ASIO")
            continue;
       #endif

        type->scanForDevices();

        for (auto& name : type->getDeviceNames (true))
            if (name.containsIgnoreCase ("Scarlett") || name.containsIgnoreCase ("Focusrite"))
                return name;
    }

    return {};
}

class WoodshedApp final : public juce::JUCEApplication
{
public:
    WoodshedApp()
    {
        juce::PropertiesFile::Options options;
        options.applicationName     = JucePlugin_Name;
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
       #if JUCE_LINUX || JUCE_BSD
        options.folderName          = "~/.config";
       #endif
        appProperties.setStorageParameters (options);
    }

    const juce::String getApplicationName() override       { return JucePlugin_Name; }
    const juce::String getApplicationVersion() override    { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override             { return false; }

    void initialise (const juce::String&) override
    {
        auto* settings = appProperties.getUserSettings();
        const bool firstLaunch = ! settings->containsKey ("audioSetup");
        const auto interfaceName = findInterface();

        juce::AudioDeviceManager::AudioDeviceSetup preferred;
        preferred.inputDeviceName  = interfaceName;
        preferred.outputDeviceName = interfaceName;
        preferred.bufferSize       = preferredBufferSize;
        preferred.sampleRate       = preferredSampleRate;

        auto holder = std::make_unique<juce::StandalonePluginHolder> (settings, false, interfaceName,
                                                                      interfaceName.isNotEmpty() ? &preferred : nullptr);

        if (firstLaunch && interfaceName.isNotEmpty())
            holder->getMuteInputValue().setValue (false);

        mainWindow = std::make_unique<juce::StandaloneFilterWindow> (getApplicationName(), juce::Colour (0xff12110f),
                                                                     std::move (holder));
        mainWindow->setVisible (true);
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        appProperties.saveIfNeeded();
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            mainWindow->pluginHolder->savePluginState();

        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
        {
            juce::Timer::callAfterDelay (100, []
            {
                if (auto* app = juce::JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        }
        else
        {
            quit();
        }
    }

private:
    juce::ApplicationProperties appProperties;
    std::unique_ptr<juce::StandaloneFilterWindow> mainWindow;
};
} // namespace

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new WoodshedApp(); }

#endif
