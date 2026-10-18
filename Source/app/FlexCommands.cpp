#include "FlexCommands.h"
#include "../flex/Edits.h"
#include "../flex/Renderer.h"

namespace woodshed::app::flexui
{

namespace ids = daw::ids;
namespace fids = woodshed::flex::ids;
using woodshed::flex::Mode;

namespace
{
juce::String importTempoSetting (Studio& s)
{
    return s.settings().getUserSettings()->getValue ("importTempo", "ask");
}

bool conformImports (Studio& s)
{
    return s.settings().getUserSettings()->getBoolValue ("conformImports", true);
}

juce::ValueTree selectedAudioTrack (Studio& s)
{
    auto t = s.getSelectedTrack();
    return t.isValid() && ! daw::Project::isInstrumentTrack (t) && ! daw::Project::isAuxTrack (t) ? t : juce::ValueTree();
}

bool isAudioRegion (const juce::ValueTree& r)
{
    return r.isValid() && ! daw::Project::isMidiRegion (r) && ! daw::Project::isTakeFolder (r) && r.hasProperty (ids::file);
}

void setMode (Studio& s, Mode m)
{
    if (auto t = selectedAudioTrack (s); t.isValid())
    {
        auto& p = s.project();
        p.undo.beginNewTransaction ("Time Stretch Mode");
        woodshed::flex::setTrackMode (p, t, m);
    }
}

/** A short description of an estimate, for the status line. */
juce::String describe (const woodshed::flex::TempoEstimate& t)
{
    return juce::String (t.bpm, 1) + " bpm" + (t.variable ? " (varying)" : "");
}
}

juce::Array<juce::ValueTree> selectedAudioRegions (Studio& s)
{
    juce::Array<juce::ValueTree> list;
    for (int id : s.getSelectedRegions())
        if (auto r = s.findRegion (id); isAudioRegion (r))
            list.add (r);
    return list;
}

std::shared_ptr<const woodshed::flex::TempoEstimate> tempoOf (Studio& s, const juce::ValueTree& region)
{
    auto& p = s.project();
    const auto file = p.resolve (region[ids::file].toString());
    auto audio = s.engine().getAudioPool().get (file);
    return s.engine().getFlexRenderer().analysis().tempo (file.getFullPathName(), audio, p.grid().sampleRate, p.grid().beatsPerBar);
}

//==============================================================================
void getAllCommands (juce::Array<juce::CommandID>& c)
{
    c.addArray ({ cmd::flexTrack, cmd::flexMono, cmd::flexPoly, cmd::flexRhythmic, cmd::flexSpeed, cmd::quantizeAudio,
                  cmd::followTempo, cmd::resetTiming, cmd::detectTempo, cmd::conformTempo, cmd::showPitchEdit,
                  cmd::importTempoAsk, cmd::importTempoAlways, cmd::importTempoNever, cmd::conformImports });
}

bool getCommandInfo (Studio& s, juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    const auto track = selectedAudioTrack (s);
    const bool flexOn = (bool) track[fids::flexOn];
    const auto mode = woodshed::flex::modeOf (track);
    const auto regions = selectedAudioRegions (s);
    const auto cmdKey = juce::ModifierKeys::commandModifier;

    switch (id)
    {
        case cmd::flexTrack:     info.setInfo ("Time Stretch", "Time Stretch on the selected track", "Time Stretch", 0);
                                 info.addDefaultKeypress ('f', cmdKey); info.setActive (track.isValid()); info.setTicked (flexOn); return true;
        case cmd::flexMono:      info.setInfo ("Monophonic", {}, "Time Stretch", 0); info.setActive (track.isValid());
                                 info.setTicked (flexOn && mode == Mode::monophonic); return true;
        case cmd::flexPoly:      info.setInfo ("Polyphonic", {}, "Time Stretch", 0); info.setActive (track.isValid());
                                 info.setTicked (flexOn && mode == Mode::polyphonic); return true;
        case cmd::flexRhythmic:  info.setInfo ("Rhythmic (Slicing)", {}, "Time Stretch", 0); info.setActive (track.isValid());
                                 info.setTicked (flexOn && mode == Mode::rhythmic); return true;
        case cmd::flexSpeed:     info.setInfo ("Speed (Varispeed)", {}, "Time Stretch", 0); info.setActive (track.isValid());
                                 info.setTicked (flexOn && mode == Mode::speed); return true;
        case cmd::quantizeAudio: info.setInfo ("Quantize Audio", "Moves the selected audio regions' transients onto the grid", "Time Stretch", 0);
                                 info.addDefaultKeypress ('q', juce::ModifierKeys::ctrlModifier); info.setActive (! regions.isEmpty()); return true;
        case cmd::followTempo:
        {
            bool all = ! regions.isEmpty();
            for (auto& r : regions) all = all && (bool) r[fids::followTempo];
            info.setInfo ("Follow Tempo", "The selected audio regions stretch when the tempo changes", "Time Stretch", 0);
            info.setActive (! regions.isEmpty()); info.setTicked (all); return true;
        }
        case cmd::resetTiming:   info.setInfo ("Reset Time Stretch", "The selected audio regions play as recorded again", "Time Stretch", 0);
                                 info.setActive (! regions.isEmpty()); return true;
        case cmd::detectTempo:   info.setInfo ("Detect Tempo", "The song takes the selected audio region's tempo", "Time Stretch", 0);
                                 info.setActive (regions.size() == 1); return true;
        case cmd::conformTempo:  info.setInfo ("Conform to Song Tempo", "The selected audio regions play at the song's tempo", "Time Stretch", 0);
                                 info.setActive (! regions.isEmpty()); return true;
        case cmd::showPitchEdit: info.setInfo ("Pitch Edit", "Edit the pitch of a sung or played audio region", "View", 0);
                                 info.setTicked (s.getPane() == Studio::Pane::pitchEdit); return true;
        case cmd::importTempoAsk:    info.setInfo ("Ask", {}, "Time Stretch", 0); info.setTicked (importTempoSetting (s) == "ask"); return true;
        case cmd::importTempoAlways: info.setInfo ("Always", {}, "Time Stretch", 0); info.setTicked (importTempoSetting (s) == "always"); return true;
        case cmd::importTempoNever:  info.setInfo ("Never", {}, "Time Stretch", 0); info.setTicked (importTempoSetting (s) == "never"); return true;
        case cmd::conformImports:    info.setInfo ("Conform Imported Audio to Song Tempo", {}, "Time Stretch", 0);
                                     info.setTicked (conformImports (s)); return true;
        default: return false;
    }
}

bool perform (Studio& s, juce::CommandID id)
{
    auto& p = s.project();
    auto* prefs = s.settings().getUserSettings();

    switch (id)
    {
        case cmd::flexTrack:
            if (auto t = selectedAudioTrack (s); t.isValid())
            {
                p.undo.beginNewTransaction ("Time Stretch");
                woodshed::flex::setTrackFlex (p, t, ! (bool) t[fids::flexOn]);
            }
            return true;
        case cmd::flexMono:     setMode (s, Mode::monophonic); return true;
        case cmd::flexPoly:     setMode (s, Mode::polyphonic); return true;
        case cmd::flexRhythmic: setMode (s, Mode::rhythmic); return true;
        case cmd::flexSpeed:    setMode (s, Mode::speed); return true;

        case cmd::quantizeAudio:
        {
            p.undo.beginNewTransaction ("Quantize Audio");
            for (auto& r : selectedAudioRegions (s))
            {
                const auto file = p.resolve (r[ids::file].toString());
                auto transients = s.engine().getFlexRenderer().analysis().transients (file.getFullPathName(),
                                                                                      s.engine().getAudioPool().get (file), p.grid().sampleRate);
                woodshed::flex::quantizeAudio (p, r, *transients, s.getQuantizeTicks());
            }
            s.flash ("Quantized to the grid");
            return true;
        }
        case cmd::followTempo:
        {
            const auto regions = selectedAudioRegions (s);
            bool all = true;
            for (auto& r : regions) all = all && (bool) r[fids::followTempo];
            p.undo.beginNewTransaction ("Follow Tempo");
            for (auto r : regions)
                woodshed::flex::setFollowTempo (p, r, ! all);
            return true;
        }
        case cmd::resetTiming:
            p.undo.beginNewTransaction ("Reset Time Stretch");
            for (auto r : selectedAudioRegions (s))
                woodshed::flex::resetTiming (p, r);
            return true;
        case cmd::detectTempo:
        {
            const auto regions = selectedAudioRegions (s);
            if (regions.size() != 1)
                return true;
            const auto t = tempoOf (s, regions[0]);
            if (t->bpm <= 0.0)
            {
                s.flash ("No steady beat found in " + regions[0][ids::name].toString());
                return true;
            }
            p.undo.beginNewTransaction ("Detect Tempo");
            woodshed::flex::adoptTempo (p, regions[0], *t);
            s.flash ("Tempo " + describe (*t) + " from " + regions[0][ids::name].toString());
            return true;
        }
        case cmd::conformTempo:
        {
            p.undo.beginNewTransaction ("Conform to Song Tempo");
            int done = 0;
            for (auto r : selectedAudioRegions (s))
                if (const auto t = tempoOf (s, r); t->bpm > 0.0)
                {
                    woodshed::flex::conformToTempo (p, r, *t);
                    ++done;
                }
            s.flash (done > 0 ? "Conformed to " + juce::String (p.grid().tempo, 1) + " bpm" : juce::String ("No steady beat found"));
            return true;
        }
        case cmd::showPitchEdit:
            if (s.getPane() == Studio::Pane::pitchEdit)
                s.setPane (Studio::Pane::none);
            else if (const auto regions = selectedAudioRegions (s); ! regions.isEmpty())
                s.openInPitchEdit ((int) regions[0][ids::id]);
            else
                s.setPane (Studio::Pane::pitchEdit);
            return true;
        case cmd::importTempoAsk:    prefs->setValue ("importTempo", "ask"); return true;
        case cmd::importTempoAlways: prefs->setValue ("importTempo", "always"); return true;
        case cmd::importTempoNever:  prefs->setValue ("importTempo", "never"); return true;
        case cmd::conformImports:    prefs->setValue ("conformImports", ! conformImports (s)); return true;
        default: return false;
    }
}

void addMenuItems (juce::PopupMenu& m, juce::ApplicationCommandManager* cm)
{
    juce::PopupMenu stretch;
    stretch.addCommandItem (cm, cmd::flexTrack);
    stretch.addSeparator();
    stretch.addCommandItem (cm, cmd::flexMono);
    stretch.addCommandItem (cm, cmd::flexPoly);
    stretch.addCommandItem (cm, cmd::flexRhythmic);
    stretch.addCommandItem (cm, cmd::flexSpeed);
    stretch.addSeparator();
    stretch.addCommandItem (cm, cmd::quantizeAudio);
    stretch.addCommandItem (cm, cmd::followTempo);
    stretch.addCommandItem (cm, cmd::resetTiming);
    stretch.addSeparator();
    stretch.addCommandItem (cm, cmd::detectTempo);
    stretch.addCommandItem (cm, cmd::conformTempo);
    stretch.addSeparator();
    juce::PopupMenu adopt;
    adopt.addCommandItem (cm, cmd::importTempoAsk);
    adopt.addCommandItem (cm, cmd::importTempoAlways);
    adopt.addCommandItem (cm, cmd::importTempoNever);
    stretch.addSubMenu ("Empty Song Takes Imported Audio's Tempo", adopt);
    stretch.addCommandItem (cm, cmd::conformImports);
    m.addSubMenu ("Time Stretch and Tempo", stretch);
}

//==============================================================================
void audioImported (Studio& s, const juce::Array<int>& regionIds, bool songWasEmpty)
{
    if (regionIds.isEmpty())
        return;
    auto region = s.findRegion (regionIds[0]);
    if (! isAudioRegion (region))
        return;
    const auto t = tempoOf (s, region);
    if (t->bpm <= 0.0 || t->confidence < 0.3)
        return;     // no clear beat: leave it as it is

    auto& p = s.project();
    const auto setting = importTempoSetting (s);
    if (songWasEmpty && setting != "never")
    {
        auto adopt = [&s, regionId = regionIds[0], t]
        {
            auto& project = s.project();
            if (auto r = s.findRegion (regionId); r.isValid())
            {
                project.undo.beginNewTransaction ("Adopt Tempo");
                woodshed::flex::adoptTempo (project, r, *t);
                s.flash ("The song is now " + describe (*t));
            }
        };
        if (setting == "always")
        {
            adopt();
            return;
        }
        const auto name = region[ids::name].toString();
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("Use the file's tempo?")
                                          .withMessage (name + " plays at about " + describe (*t) + ". Set the song to that tempo, "
                                                        "so the bars and the click follow the recording?")
                                          .withButton ("Use " + juce::String (t->bpm, 1) + " bpm")
                                          .withButton ("Keep " + juce::String (p.grid().tempo, 1) + " bpm"),
                                      [adopt] (int button) { if (button == 1) adopt(); });
        return;
    }

    if (! songWasEmpty && conformImports (s) && std::abs (t->bpm - p.grid().tempoAt ((double) (juce::int64) region[ids::start])) > 0.5)
    {
        p.undo.beginNewTransaction ("Conform to Song Tempo");
        for (int id : regionIds)
            if (auto r = s.findRegion (id); isAudioRegion (r))
                if (const auto rt = tempoOf (s, r); rt->bpm > 0.0 && rt->confidence >= 0.3)
                    woodshed::flex::conformToTempo (p, r, *rt);
        s.flash ("Conformed from " + describe (*t) + " to the song's " + juce::String (p.grid().tempo, 1) + " bpm (Undo to keep it as recorded)");
    }
}

} // namespace woodshed::app::flexui
