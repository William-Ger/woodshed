// woodshed-bench drummer: checks the Drummer pattern generator and the step sequencer's patterns.
// woodshed-bench demo-drummer <folder>: a project with a few styles in a row on a drum kit, to
// render with render-project and listen to.

#include "../../Source/daw/Drummer.h"
#include "../../Source/daw/StepPattern.h"
#include "../../Source/instruments/Instrument.h"
#include <iostream>
#include <map>

using namespace woodshed::daw;

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << "\n";
    failures += ok ? 0 : 1;
}

double meanVelocity (const std::vector<Note>& notes)
{
    double sum = 0.0;
    for (auto& n : notes) sum += n.velocity;
    return notes.empty() ? 0.0 : sum / (double) notes.size();
}

bool isTom (int pitch)      { return pitch == 41 || pitch == 43 || pitch == 45 || pitch == 47 || pitch == 48 || pitch == 50; }
bool isCrash (int pitch)    { return pitch == 49 || pitch == 57; }
}

/** A text picture of a groove: one line per drum, one column per 16th (or triplet 8th). */
static void printGroove (const juce::StringArray& args)
{
    using namespace woodshed::daw::drummer;
    Settings s = withStyle (Settings(), styleFromName (args[0].replace ("_", " ")));
    Meter m;
    for (auto& a : args)
    {
        if (a.contains ("/")) m = { a.upToFirstOccurrenceOf ("/", false, false).getIntValue(), a.fromFirstOccurrenceOf ("/", false, false).getIntValue() };
        if (a.startsWith ("c=")) s.complexity = a.substring (2).getFloatValue();
        if (a.startsWith ("l=")) s.loudness = a.substring (2).getFloatValue();
        if (a.startsWith ("f=")) s.fill = a.substring (2).getFloatValue();
        if (a.startsWith ("seed=")) s.seed = (juce::uint32) a.substring (5).getIntValue();
        if (a == "perc") s.percussion = true;
    }
    s.bars = 4;
    s.humanize = 0.0f;
    s.swing = 0.0f;
    const auto notes = generate (s, m);
    const int cell = s.style == Style::jazz || s.style == Style::shuffle || (m.beatUnit == 8 && m.beatsPerBar % 3 == 0) ? 80 : 240;
    const int cols = (int) (m.barTicks() * s.bars / cell);
    std::map<int, std::string> lines;
    for (auto& n : notes)
    {
        auto& line = lines[n.pitch];
        if (line.empty()) line.assign ((size_t) cols, '.');
        const auto col = (size_t) juce::jlimit (0, cols - 1, (int) ((n.start + cell / 2) / cell));
        line[col] = n.velocity >= 100 ? 'X' : n.velocity >= 55 ? 'x' : 'g';
    }
    std::cout << styleName (s.style) << " " << m.beatsPerBar << "/" << m.beatUnit << "  " << notes.size() << " notes\n";
    for (auto it = lines.rbegin(); it != lines.rend(); ++it)
    {
        std::string bars;
        const int perBar = (int) (m.barTicks() / cell);
        for (int i = 0; i < cols; ++i)
            bars += (i > 0 && i % perBar == 0 ? "|" : "") + std::string (1, it->second[(size_t) i]);
        std::cout << juce::String (StepPattern::rowName (it->first, true)).paddedRight (' ', 12) << bars << "\n";
    }
}

int runDrummerTest (const juce::StringArray& args);
int runDrummerTest (const juce::StringArray& args)
{
    juce::ScopedJuceInitialiser_GUI init;
    using namespace woodshed::daw::drummer;

    if (args.size() > 1 && args[0] == "print")
    {
        printGroove (juce::StringArray (args.begin() + 1, args.size() - 1));
        return 0;
    }

    const Meter meters[] { { 4, 4 }, { 3, 4 }, { 6, 8 }, { 5, 4 }, { 7, 8 }, { 12, 8 } };

    // Every style, every bar count and meter: notes inside the bars, General MIDI drums, a groove.
    {
        int problems = 0, empty = 0, outside = 0, badPitch = 0, unsorted = 0;
        for (int style = 0; style < numStyles; ++style)
            for (auto& m : meters)
                for (int bars = 1; bars <= 16; ++bars)
                    for (float c : { 0.0f, 0.5f, 1.0f })
                    {
                        Settings s;
                        s = withStyle (s, (Style) style);
                        s.bars = bars;
                        s.complexity = c;
                        s.fill = 0.7f;
                        s.percussion = true;
                        s.seed = (juce::uint32) (style * 31 + bars);
                        const auto notes = generate (s, m);
                        const auto end = m.barTicks() * bars;
                        if (notes.empty()) ++empty;
                        for (size_t i = 0; i < notes.size(); ++i)
                        {
                            const auto& n = notes[i];
                            if (n.start < 0 || n.start + n.length > end || n.length < 1) ++outside;
                            if (! isGeneralMidiDrum (n.pitch) || n.velocity < 1 || n.velocity > 127) ++badPitch;
                            if (i > 0 && notes[i - 1].start > n.start) ++unsorted;
                        }

                        // A groove has a kick and a backbeat (or the jazz feathered kick and ride) every bar.
                        for (int b = 0; b < bars; ++b)
                        {
                            bool kick = false, time = false;
                            for (auto& n : notes)
                                if (n.start >= m.barTicks() * b - 40 && n.start < m.barTicks() * (b + 1) - 40)    // humanizing can lead the barline
                                {
                                    kick = kick || n.pitch == 36;
                                    time = time || n.pitch == 42 || n.pitch == 46 || n.pitch == 51 || n.pitch == 38 || n.pitch == 37 || n.pitch == 39;
                                }
                            if (! (kick && time) && ! isFillBar (s, b))
                            {
                                if (problems < 12)
                                    std::cout << "        " << styleName (s.style) << " " << m.beatsPerBar << "/" << m.beatUnit
                                              << " complexity " << c << " bar " << b + 1 << (kick ? "" : " no kick") << (time ? "" : " no time") << "\n";
                                ++problems;
                            }
                        }
                    }
        check (empty == 0, "every style writes notes for 1..16 bars in 4/4, 3/4, 6/8, 5/4, 7/8, 12/8 (" + juce::String (empty) + " empty)");
        check (outside == 0, "every note is inside the bars (" + juce::String (outside) + " outside)");
        check (badPitch == 0, "every note is a General MIDI drum with a valid velocity (" + juce::String (badPitch) + " bad)");
        check (unsorted == 0, "notes come in time order");
        check (problems == 0, "every bar outside a fill has a kick and a timekeeper or backbeat (" + juce::String (problems) + " without)");
    }

    // Complexity adds notes; loudness raises velocity.
    {
        int busier = 0, louder = 0, total = 0;
        for (int style = 0; style < numStyles; ++style)
            for (juce::uint32 seed : { 1u, 7u, 99u })
            {
                Settings s = withStyle (Settings(), (Style) style);
                s.bars = 8;
                s.fill = 0.0f;
                s.seed = seed;
                s.percussion = true;
                auto simple = s, complex = s;
                simple.complexity = 0.0f;
                complex.complexity = 1.0f;
                busier += generate (complex, {}).size() > generate (simple, {}).size() ? 1 : 0;

                auto soft = s, loud = s;
                soft.loudness = 0.05f;
                loud.loudness = 0.95f;
                louder += meanVelocity (generate (loud, {})) > meanVelocity (generate (soft, {})) + 15.0 ? 1 : 0;
                ++total;
            }
        check (busier == total, "complex grooves have more notes than simple ones (" + juce::String (busier) + "/" + juce::String (total) + ")");
        check (louder == total, "loud grooves have a higher mean velocity than soft ones (" + juce::String (louder) + "/" + juce::String (total) + ")");
    }

    // Seeds.
    {
        Settings s = withStyle (Settings(), Style::funk);
        s.complexity = 0.8f;
        s.bars = 8;
        s.seed = 1234;
        auto a = generate (s, {}), b = generate (s, {});
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i)
            same = a[i].pitch == b[i].pitch && a[i].start == b[i].start && a[i].velocity == b[i].velocity;
        check (same, "the same seed gives the same notes");
        s.seed = 1235;
        auto c = generate (s, {});
        bool differs = c.size() != a.size();
        for (size_t i = 0; ! differs && i < a.size(); ++i)
            differs = a[i].start != c[i].start || a[i].velocity != c[i].velocity || a[i].pitch != c[i].pitch;
        check (differs, "another seed gives another performance");
    }

    // Fills: toms only in the last bar of a phrase, crashes only on the downbeat after one.
    {
        int tomsOutside = 0, crashesOutside = 0, fills = 0, crashes = 0;
        for (int style = 0; style < numStyles; ++style)
            for (float f : { 0.2f, 0.5f, 1.0f })
                for (auto& m : meters)
                {
                    Settings s = withStyle (Settings(), (Style) style);
                    s.bars = 16;
                    s.fill = f;
                    s.complexity = 0.7f;
                    s.percussion = true;
                    const auto bar = m.barTicks();
                    for (auto& n : generate (s, m))
                    {
                        const int b = (int) (n.start / bar);
                        if (isTom (n.pitch))
                        {
                            ++fills;
                            if (! isFillBar (s, b)) ++tomsOutside;
                        }
                        if (isCrash (n.pitch))
                        {
                            ++crashes;
                            // Humanizing may nudge the crash a few ticks either side of the barline.
                            const auto nearest = (n.start + bar / 2) / bar;
                            if (std::abs (n.start - nearest * bar) > 40 || nearest == 0 || ! isFillBar (s, (int) nearest - 1))
                                ++crashesOutside;
                        }
                    }
                }
        check (fills > 0 && tomsOutside == 0, "toms play only in phrase-end fill bars (" + juce::String (fills) + " tom hits, "
                                                  + juce::String (tomsOutside) + " outside)");
        check (crashes > 0 && crashesOutside == 0, "crashes land only on the downbeat after a fill (" + juce::String (crashes) + ", "
                                                       + juce::String (crashesOutside) + " elsewhere)");

        Settings none;
        none.fill = 0.0f;
        none.bars = 16;
        bool noToms = true;
        for (auto& n : generate (none, {}))
            noToms = noToms && ! isTom (n.pitch) && ! isCrash (n.pitch);
        check (noToms, "no fills, no toms or crashes");
        check (phraseBars (none) == 0 && phraseBars ([] { Settings s; s.fill = 0.3f; return s; }()) == 8
                   && phraseBars ([] { Settings s; s.fill = 0.8f; return s; }()) == 4, "fill amount sets the phrase: none, 8 bars, 4 bars");
    }

    // Kit pieces.
    {
        Settings s = withStyle (Settings(), Style::rock);
        s.hiHat = false;
        s.cymbals = false;
        s.bars = 8;
        bool ok = true;
        for (auto& n : generate (s, {}))
            ok = ok && n.pitch != 42 && n.pitch != 46 && n.pitch != 44 && ! isCrash (n.pitch) && n.pitch != 51;
        check (ok, "hats and cymbals off: none play");

        s = withStyle (Settings(), Style::rock);
        s.timekeeper = Timekeeper::ride;
        int rides = 0, hats = 0;
        for (auto& n : generate (s, {}))
        {
            rides += n.pitch == 51 ? 1 : 0;
            hats += n.pitch == 42 ? 1 : 0;
        }
        check (rides > 0 && hats == 0, "ride as the timekeeper replaces the hi-hat");

        s = withStyle (Settings(), Style::funk);
        s.complexity = 1.0f;
        s.bars = 4;
        int ghosts = 0;
        for (auto& n : generate (s, {}))
            ghosts += n.pitch == 38 && n.velocity < 50 ? 1 : 0;
        check (ghosts >= 8, "complex funk has ghost notes on the snare (" + juce::String (ghosts) + ")");

        s.swing = 1.0f;
        s.humanize = 0.0f;
        s.fill = 0.0f;
        bool swung = false;
        for (auto& n : generate (s, {}))
            swung = swung || (n.pitch == 42 && n.start % ticksPerQuarter == 320);       // 16th swing: the e lands at 320
        check (swung, "full swing moves the off-16ths to the triplet");
    }

    // Settings round-trip through JSON.
    {
        Settings s = withStyle (Settings(), Style::bossaNova);
        s.complexity = 0.3f; s.loudness = 0.2f; s.swing = 0.1f; s.fill = 0.9f; s.toms = false; s.timekeeper = Timekeeper::ride;
        s.bars = 12; s.seed = 4000000000u;
        check (Settings::fromJson (s.toJson()) == s, "drummer settings survive JSON");
    }

    // Applying to a region: one undo step, the region's length follows the bars.
    {
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("woodshed-drummer-test");
        folder.deleteRecursively();
        auto project = Project::createIn (folder, 48000.0);
        auto track = project->addInstrumentTrack ("Drums", "drums.kit", {}, {});
        const auto bar = (juce::int64) std::llround (project->grid().samplesPerBar());
        auto region = project->addMidiRegion (track, bar, bar, "Drummer");
        project->undo.beginNewTransaction ("before");
        project->undo.beginNewTransaction ("Drummer");
        Settings s;
        s.bars = 4;
        apply (*project, region, s, &project->undo);
        check ((juce::int64) region[ids::length] == bar * 4 && region.getNumChildren() > 20 && isDrummerRegion (region),
               "apply writes 4 bars of notes and the settings");
        project->undo.undo();
        check ((juce::int64) region[ids::length] == bar && region.getNumChildren() == 0 && ! isDrummerRegion (region),
               "one undo takes it all back");

        // A drag: previews without undo, then one undoable write.
        project->undo.beginNewTransaction ("Drummer");
        apply (*project, region, s, &project->undo);
        const auto before = regionNotes (region).size();
        RegionSnapshot snap;
        snap.take (region);
        for (float c = 0.0f; c <= 1.0f; c += 0.1f)
        {
            auto t = s;
            t.complexity = c;
            apply (*project, region, t, nullptr);
        }
        snap.restore();
        check (regionNotes (region).size() == before && settingsOf (region) == s, "a snapshot puts the region back after previews");
        project->undo.beginNewTransaction ("Drummer Complexity");
        auto finalSettings = s;
        finalSettings.complexity = 1.0f;
        apply (*project, region, finalSettings, &project->undo);
        project->undo.undo();
        check (regionNotes (region).size() == before && settingsOf (region) == s, "the whole drag is one undo step");
        folder.deleteRecursively();
    }

    // Step sequencer patterns.
    {
        StepPattern p;
        p.rows.clear();
        for (int pitch : StepPattern::drumRows())
            p.rows.push_back ({ pitch, false, false, std::vector<int> (16, 0) });
        p.randomize (42, true, 0.6f);
        check (! p.isEmpty() && p.isOn (0, 0) && p.isOn (1, 4) && p.isOn (1, 12), "randomized drums: a kick on 1 and the backbeat");

        const auto notes = p.toNotes (p.lengthTicks());
        const auto back = StepPattern::fromNotes (notes, StepPattern::drumRows(), p.stepTicks, p.numSteps);
        check (back == p, "pattern -> notes -> pattern round-trips");

        const auto twice = p.toNotes (p.lengthTicks() * 2 + 100);
        check (twice.size() == notes.size() * 2 + (size_t) std::count_if (notes.begin(), notes.end(), [] (const Note& n) { return n.start == 0; }),
               "the pattern repeats to fill the region");

        StepPattern json;
        check (StepPattern::fromJson (p.toJson(), json) && json == p, "patterns survive JSON");

        auto muted = p;
        muted.rows[0].mute = true;
        bool noKick = true;
        for (auto& n : muted.toNotes (muted.lengthTicks()))
            noKick = noKick && n.pitch != muted.rows[0].pitch;
        check (noKick, "a muted row plays nothing");
        auto soloed = p;
        soloed.rows[1].solo = true;
        bool onlySnare = true;
        for (auto& n : soloed.toNotes (soloed.lengthTicks()))
            onlySnare = onlySnare && n.pitch == soloed.rows[1].pitch;
        check (onlySnare, "a soloed row plays alone");

        StepPattern triplets;
        triplets.stepTicks = 160;
        triplets.setNumSteps (12);
        triplets.rows = { { 60, false, false, {} } };
        triplets.setNumSteps (12);
        for (int i = 0; i < 12; i += 2) triplets.set (0, i, 90);
        check (StepPattern::fromNotes (triplets.toNotes (triplets.lengthTicks()), { 60 }, 160, 12) == triplets,
               "1/16 triplet patterns round-trip");

        StepPattern melodic;
        melodic.setRows (StepPattern::scaleRows (StepPattern::Scale::minor, 48, 15));
        melodic.setNumSteps (16);
        melodic.randomize (7, false, 0.5f);
        int perStep = 0;
        bool inScale = true;
        for (int s = 0; s < melodic.numSteps; ++s)
        {
            int on = 0;
            for (int r = 0; r < (int) melodic.rows.size(); ++r)
                on += melodic.isOn (r, s) ? 1 : 0;
            perStep = juce::jmax (perStep, on);
        }
        for (auto& r : melodic.rows)
            inScale = inScale && juce::Array<int> { 0, 2, 3, 5, 7, 8, 10 }.contains (r.pitch % 12);
        check (! melodic.isEmpty() && perStep == 1 && inScale, "randomized melody: one note at a time, in the scale");

        auto kept = p;
        kept.setRows ({ 38, 36 });
        check (kept.rows.size() == 2 && kept.rows[1].velocity == p.rows[0].velocity, "changing rows keeps each pitch's steps");
    }

    std::cout << (failures == 0 ? "all drummer checks passed\n" : juce::String (failures) + " drummer checks failed\n");
    return failures == 0 ? 0 : 1;
}

//==============================================================================
int writeDrummerDemoProject (const juce::File& folder);
int writeDrummerDemoProject (const juce::File& folder)
{
    juce::ScopedJuceInitialiser_GUI init;
    using namespace woodshed::daw::drummer;
    folder.deleteRecursively();
    auto project = Project::createIn (folder, 48000.0);
    project->state.setProperty (ids::name, "Drummer Demo", nullptr);
    project->state.setProperty (ids::tempo, 104.0, nullptr);
    const auto bar = (juce::int64) std::llround (project->grid().samplesPerBar());

    auto* kit = woodshed::InstrumentRegistry::get().find ("drums.kit");
    if (kit == nullptr)
    {
        std::cerr << "no drum kit\n";
        return 1;
    }
    auto track = project->addInstrumentTrack ("Drummer", "drums.kit", "Studio Kit", woodshed::patchValues (kit->info, nullptr));

    // Styles one after another on one kit, four bars each, so levels can be compared.
    const std::pair<Style, float> parts[] { { Style::rock, 0.6f }, { Style::funk, 0.6f }, { Style::hipHop, 0.5f },
                                            { Style::house, 0.6f }, { Style::jazz, 0.4f }, { Style::shuffle, 0.6f },
                                            { Style::bossaNova, 0.4f }, { Style::metal, 1.0f }, { Style::lofi, 0.4f } };
    juce::int64 at = 0;
    juce::uint32 seed = 1;
    for (auto& [style, loudness] : parts)
    {
        Settings s = withStyle (Settings(), style);
        s.loudness = loudness;
        s.complexity = 0.55f;
        s.fill = 0.6f;
        s.bars = 4;
        s.seed = seed++;
        auto region = project->addMidiRegion (track, at, bar * 4, styleName (style));
        apply (*project, region, s, nullptr);
        at += bar * 4;
    }

    // A step-sequencer pattern on a second kit, for the Step Sequencer pane.
    auto steps = project->addInstrumentTrack ("Steps", "drums.kit", "Studio Kit", woodshed::patchValues (kit->info, nullptr));
    StepPattern pattern;
    for (int pitch : StepPattern::drumRows())
        pattern.rows.push_back ({ pitch, false, false, std::vector<int> (16, 0) });
    pattern.randomize (5, true, 0.6f);
    pattern.applyTo (*project, project->addMidiRegion (steps, 0, bar * 4, "Pattern"), nullptr);

    if (! project->save())
        return 1;
    std::cout << "wrote " << project->projectFile().getFullPathName() << "\n";
    return 0;
}
