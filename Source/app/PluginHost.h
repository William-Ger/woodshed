#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

/**
    Third-party plug-ins (Audio Units on the Mac, VST3 everywhere): the formats, the list of
    plug-ins found on this computer (kept in the app's settings), the Plug-in Manager window that
    scans for them, and the windows that show a plug-in's own editor.
*/
class PluginHost final : private Studio::ProjectSwapListener
{
public:
    class EditorWindow;
    explicit PluginHost (Studio&);
    ~PluginHost() override;

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;

    /** Makes a plug-in from its description XML (the engine's factory). Message thread. */
    std::unique_ptr<juce::AudioPluginInstance> create (const juce::String& descriptionXml, double rate, int block, juce::String& error);

    /** Every known instrument / effect, sorted by maker then name. */
    juce::Array<juce::PluginDescription> instruments() const;
    juce::Array<juce::PluginDescription> effects() const;

    /** The window that lists plug-ins and scans for new ones. */
    void showManager();

    /** Opens (or brings forward) the editor of the plug-in on a track's instrument (fxId -1) or
        one of its inserts (trackId -1 for the master). */
    void showEditor (int trackId, int fxId);
    void closeEditors();
    void closeEditor (EditorWindow*);

    /** A plug-in description as XML text (what INSTRUMENT / FX nodes store), and back. */
    static juce::String toXml (const juce::PluginDescription&);
    static bool fromXml (const juce::String&, juce::PluginDescription&);

private:
    void projectAboutToBeReplaced() override                { closeEditors(); }
    void saveList();

    class ManagerWindow;
    Studio& studio;
    std::unique_ptr<juce::ChangeListener> listSaver;
    std::vector<std::unique_ptr<EditorWindow>> editors;
    std::unique_ptr<ManagerWindow> manager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};

} // namespace woodshed::app
