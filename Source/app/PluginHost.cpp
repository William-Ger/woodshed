#include "PluginHost.h"

namespace woodshed::app
{

//==============================================================================
/** A plug-in's own editor in a window of its own. It holds the plug-in's wrapper, so the plug-in
    lives at least as long as its editor even if the track drops it meanwhile. */
class PluginHost::EditorWindow final : public juce::DocumentWindow
{
public:
    EditorWindow (PluginHost& h, int track, int fx, std::shared_ptr<void> keepAlive, juce::AudioPluginInstance& plugin, const juce::String& title)
        : juce::DocumentWindow (title, theme::panel, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          host (h), trackId (track), fxId (fx), owner (std::move (keepAlive))
    {
        setUsingNativeTitleBar (true);
        juce::AudioProcessorEditor* editor = plugin.hasEditor() ? plugin.createEditorAndMakeActive() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (plugin);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~EditorWindow() override
    {
        clearContentComponent();        // the editor goes before the plug-in it edits
    }

    void closeButtonPressed() override
    {
        juce::MessageManager::callAsync ([&h = host, w = this] { h.closeEditor (w); });
    }

    PluginHost& host;
    const int trackId, fxId;

private:
    std::shared_ptr<void> owner;
};

/** The plug-in list with JUCE's scanner (Options > Scan for new or updated ...). */
class PluginHost::ManagerWindow final : public juce::DocumentWindow
{
public:
    ManagerWindow (PluginHost& h, juce::PropertiesFile* settings)
        : juce::DocumentWindow ("Plug-in Manager", theme::panel, juce::DocumentWindow::closeButton)
    {
        const auto pedal = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                               .getChildFile ("Woodshed").getChildFile ("plugin-scan-crash.txt");
        pedal.getParentDirectory().createDirectory();
        auto list = std::make_unique<juce::PluginListComponent> (h.formats, h.known, pedal, settings, true);
        list->setSize (720, 520);
        setContentOwned (list.release(), true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override                      { setVisible (false); }
};

//==============================================================================
namespace
{
struct ListSaver final : juce::ChangeListener
{
    explicit ListSaver (std::function<void()> f) : fn (std::move (f)) {}
    void changeListenerCallback (juce::ChangeBroadcaster*) override { fn(); }
    std::function<void()> fn;
};

bool sortByMaker (const juce::PluginDescription& a, const juce::PluginDescription& b)
{
    return a.manufacturerName != b.manufacturerName ? a.manufacturerName.compareIgnoreCase (b.manufacturerName) < 0
                                                    : a.name.compareIgnoreCase (b.name) < 0;
}
}

PluginHost::PluginHost (Studio& s) : studio (s)
{
    juce::addDefaultFormatsToManager (formats);

    if (auto xml = studio.settings().getUserSettings()->getXmlValue ("knownPlugins"))
        known.recreateFromXml (*xml);

    // Apple's own Audio Units are always there; list them the first time so there's something
    // to play before the user scans (a full scan of third-party plug-ins is in the manager).
    if (known.getNumTypes() == 0)
    {
       #if JUCE_PLUGINHOST_AU && JUCE_MAC
        for (auto* format : formats.getFormats())
            if (format->getName() == "AudioUnit")
                for (auto& id : format->searchPathsForPlugins ({}, false, false))
                    if (id.contains ("appl"))       // Apple's identifiers carry their maker code
                    {
                        juce::OwnedArray<juce::PluginDescription> found;
                        known.scanAndAddFile (id, true, found, *format);
                    }
       #endif
    }

    listSaver = std::make_unique<ListSaver> ([this] { saveList(); });
    known.addChangeListener (listSaver.get());
    studio.addProjectSwapListener (this);
    saveList();
}

PluginHost::~PluginHost()
{
    studio.removeProjectSwapListener (this);
    known.removeChangeListener (listSaver.get());
    closeEditors();
    manager.reset();
}

void PluginHost::saveList()
{
    if (auto xml = known.createXml())
        studio.settings().getUserSettings()->setValue ("knownPlugins", xml.get());
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::create (const juce::String& descriptionXml, double rate, int block, juce::String& error)
{
    juce::PluginDescription description;
    if (! fromXml (descriptionXml, description))
    {
        error = "Unreadable plug-in description";
        return nullptr;
    }
    return formats.createPluginInstance (description, rate, block, error);
}

juce::Array<juce::PluginDescription> PluginHost::instruments() const
{
    juce::Array<juce::PluginDescription> list;
    for (auto& d : known.getTypes())
        if (d.isInstrument)
            list.add (d);
    std::sort (list.begin(), list.end(), sortByMaker);
    return list;
}

juce::Array<juce::PluginDescription> PluginHost::effects() const
{
    juce::Array<juce::PluginDescription> list;
    for (auto& d : known.getTypes())
        if (! d.isInstrument && d.numInputChannels > 0
            && ! d.category.containsIgnoreCase ("Mixer") && ! d.category.containsIgnoreCase ("Panner")
            && ! d.category.containsIgnoreCase ("Generator"))
            list.add (d);
    std::sort (list.begin(), list.end(), sortByMaker);
    return list;
}

void PluginHost::showManager()
{
    if (manager == nullptr)
        manager = std::make_unique<ManagerWindow> (*this, studio.settings().getUserSettings());
    manager->setVisible (true);
    manager->toFront (true);
}

void PluginHost::showEditor (int trackId, int fxId)
{
    for (auto& w : editors)
        if (w->trackId == trackId && w->fxId == fxId)
        {
            w->toFront (true);
            return;
        }

    auto& engine = studio.engine();
    std::shared_ptr<void> keep;
    juce::AudioPluginInstance* plugin = nullptr;
    if (fxId < 0)
    {
        auto inst = engine.shareInstrument (trackId);
        if (auto* p = dynamic_cast<daw::PluginInstrument*> (inst.get()))
            plugin = &p->getPlugin();
        keep = inst;
    }
    else
    {
        auto pedal = engine.shareInsert (trackId, fxId);
        if (auto* p = dynamic_cast<daw::PluginEffect*> (pedal.get()))
            plugin = &p->getPlugin();
        keep = pedal;
    }

    if (plugin == nullptr)
    {
        studio.flash (engine.getPluginError().isNotEmpty() ? "Plug-in didn't load: " + engine.getPluginError() : juce::String ("The plug-in isn't loaded"));
        return;
    }

    const auto track = studio.project().trackById (trackId);
    const auto title = plugin->getName() + (track.isValid() ? "  -  " + track[daw::ids::name].toString() : juce::String ("  -  Stereo Out"));
    editors.push_back (std::make_unique<EditorWindow> (*this, trackId, fxId, keep, *plugin, title));
}

void PluginHost::closeEditors()
{
    editors.clear();
}

void PluginHost::closeEditor (EditorWindow* w)
{
    editors.erase (std::remove_if (editors.begin(), editors.end(), [w] (auto& e) { return e.get() == w; }), editors.end());
}

juce::String PluginHost::toXml (const juce::PluginDescription& d)
{
    if (auto xml = d.createXml())
        return xml->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());
    return {};
}

bool PluginHost::fromXml (const juce::String& text, juce::PluginDescription& d)
{
    if (auto xml = juce::parseXML (text))
        return d.loadFromXml (*xml);
    return false;
}

} // namespace woodshed::app
