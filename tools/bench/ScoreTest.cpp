// woodshed-bench score: checks the Score editor's notation layout (Source/app/ScoreLayout).
// Notes go in as song ticks (960 per quarter); each bar is compared as a compact string:
// w h q 8 16 32 values, "." dots, "t" triplets, "r" rests, "W" a whole-bar rest, "~" tied on.

#include "../../Source/app/ScoreRender.h"
#include <iostream>

using namespace woodshed::score;

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << "\n";
    failures += ok ? 0 : 1;
}

void expect (const juce::String& got, const juce::String& wanted, const juce::String& what)
{
    check (got == wanted, what + ": " + got + (got == wanted ? juce::String() : "   (wanted " + wanted + ")"));
}

Options plain (int beats = 4, int unit = 4)
{
    Options o;
    o.beatsPerBar = beats;
    o.beatUnit = unit;
    o.interpret = false;
    o.resolution = 240;
    return o;
}

std::vector<InputNote> notes (std::initializer_list<std::array<juce::int64, 3>> list)
{
    std::vector<InputNote> v;
    int id = 0;
    for (auto& n : list)
        v.push_back ({ (int) n[0], n[1], n[2], id++ });
    return v;
}

const Element* firstNote (const Layout& l, int staff, int bar, int index = 0)
{
    int i = 0;
    for (auto* e : l.on (staff, bar))
        if (! e->rest && i++ == index)
            return e;
    return nullptr;
}
} // namespace

int runScoreTest();
int runScoreTest()
{
    std::cout << "rhythm\n";
    {
        auto l = layout (notes ({ { 60, 0, 3840 } }), plain());
        expect (describe (l, 0, 0), "w", "a whole note in 4/4");
        check (l.bars.size() == 1 && l.staves.size() == 1 && l.staves[0] == Clef::treble, "one bar, one treble staff");
    }
    {
        auto o = plain();
        o.to = 3840 * 2;
        auto l = layout (notes ({ { 67, 2880, 1920 } }), o);
        expect (describe (l, 0, 0) + " | " + describe (l, 0, 1), "rh rq q~ | q rq rh", "a note across the bar line is tied");
        auto* a = firstNote (l, 0, 0);
        auto* b = firstNote (l, 0, 1);
        check (a != nullptr && b != nullptr && a->notes[0].tieToNext && b->notes[0].tiedFromPrevious && ! b->notes[0].tieToNext,
               "the tie runs from the first piece to the second");
    }
    {
        auto o = plain();
        o.to = 3840;
        expect (describe (layout (notes ({ { 60, 0, 960 } }), o), 0, 0), "q rq rh", "quarter then rests to the bar line");
        expect (describe (layout (notes ({ { 60, 480, 480 } }), o), 0, 0), "r8 8 rq rh", "an offbeat eighth");
        expect (describe (layout (notes ({ { 60, 240, 240 } }), o), 0, 0), "r16 16 r8 rq rh", "an offbeat sixteenth");
        expect (describe (layout (notes ({ { 60, 1920, 960 } }), o), 0, 0), "rh q rq", "a quarter on beat 3");
        expect (describe (layout (notes ({ { 60, 720, 240 } }), o), 0, 0), "r8. 16 rq rh", "a dotted-eighth rest before a sixteenth");
        expect (describe (layout (notes ({ { 60, 0, 1440 }, { 62, 1440, 480 } }), o), 0, 0), "q. 8 rh", "dotted quarter and eighth");
        expect (describe (layout (notes ({ { 60, 960, 1920 } }), o), 0, 0), "rq h rq", "a half note on beat 2");
        expect (describe (layout (notes ({ { 60, 960, 2880 } }), o), 0, 0), "rq h~ q", "a dotted half on beat 2 is a tied half and quarter");
        expect (describe (layout (notes ({ { 60, 480, 960 } }), o), 0, 0), "r8 8~ 8 r8 rh", "syncopation: an eighth tied across the beat");
    }
    {
        auto o = plain();
        o.to = 3840 * 2;
        expect (describe (layout (notes ({ { 60, 0, 960 } }), o), 0, 1), "W", "an empty bar is a whole-bar rest");
    }
    {
        auto l = layout (notes ({ { 60, 10, 470 } }), plain());
        expect (describe (l, 0, 0), "8 r8 rq rh", "display quantize snaps both ends");
        Options interpret = plain();
        interpret.interpret = true;
        auto i = layout (notes ({ { 60, 0, 240 }, { 62, 480, 240 }, { 64, 960, 400 } }), interpret);
        expect (describe (i, 0, 0), "8 8 q rh", "interpretation lengthens short notes within the beat");
    }

    std::cout << "beams\n";
    {
        std::vector<InputNote> eighths;
        for (int i = 0; i < 8; ++i)
            eighths.push_back ({ 64 + (i % 3), (juce::int64) i * 480, 480, i });
        auto l = layout (eighths, plain());
        expect (describe (l, 0, 0), "8 8 8 8 8 8 8 8", "eight eighths");
        bool pairs = l.beams.size() == 4;
        for (auto& b : l.beams)
            pairs = pairs && b.elements.size() == 2;
        check (pairs, "eighths beamed in pairs, one beam per beat (" + juce::String ((int) l.beams.size()) + " beams)");

        auto broken = layout (notes ({ { 64, 0, 480 }, { 64, 960, 480 }, { 65, 1440, 480 } }), plain());
        check (broken.beams.size() == 1 && broken.elements[(size_t) broken.beams[0].elements[0]].start == 960,
               "a lone eighth keeps its flag; the pair on beat 2 is beamed");

        auto sixteenths = layout (notes ({ { 72, 0, 240 }, { 74, 240, 240 }, { 76, 480, 480 } }), plain());
        check (sixteenths.beams.size() == 1 && sixteenths.beams[0].elements.size() == 3, "sixteenths and an eighth share a beam");
        check (! sixteenths.beams.empty() && ! sixteenths.beams[0].stemUp, "high notes: stems down");
    }

    std::cout << "meters\n";
    {
        auto o = plain (3, 4);
        o.to = 2880 * 3;
        auto l = layout (notes ({ { 60, 0, 960 }, { 62, 960, 960 }, { 64, 1920, 960 }, { 65, 2880, 2880 } }), o);
        expect (describe (l, 0, 0) + " | " + describe (l, 0, 1) + " | " + describe (l, 0, 2), "q q q | h. | W", "3/4: quarters, a dotted half, an empty bar");
        check (l.bars.size() == 3 && l.bars[1].start == 2880 && l.bars[1].number == 2, "3/4 bars are 2880 ticks");
        expect (describe (layout (notes ({ { 60, 960, 960 } }), o), 0, 0), "rq q rq", "3/4 rests");
    }
    {
        auto o = plain (6, 8);
        o.to = 2880 * 3;
        std::vector<InputNote> six;
        for (int i = 0; i < 6; ++i)
            six.push_back ({ 67, 2880 + (juce::int64) i * 480, 480, i });
        six.push_back ({ 60, 0, 1440, 6 });
        six.push_back ({ 62, 1440, 1440, 7 });
        six.push_back ({ 64, 5760, 480, 8 });
        auto l = layout (six, o);
        expect (describe (l, 0, 0), "q. q.", "6/8: two dotted quarters");
        expect (describe (l, 0, 1), "8 8 8 8 8 8", "6/8: six eighths");
        check (l.beams.size() == 2 && l.beams[0].elements.size() == 3 && l.beams[1].elements.size() == 3, "6/8 eighths beamed in threes");
        expect (describe (l, 0, 2), "8 r8 r8 rq.", "6/8 rests after an eighth");
    }

    std::cout << "pitch\n";
    {
        auto o = plain();
        o.to = 3840 * 2;
        auto l = layout (notes ({ { 61, 0, 960 }, { 61, 960, 960 }, { 60, 1920, 960 }, { 61, 2880, 960 }, { 61, 3840, 960 }, { 73, 4800, 960 } }), o);
        auto acc = [&] (int bar, int i) { auto* e = firstNote (l, 0, bar, i); return e != nullptr ? e->notes[0].accidental : Accidental::none; };
        check (acc (0, 0) == Accidental::sharp, "C# shows a sharp");
        check (acc (0, 1) == Accidental::none, "the sharp carries through the bar");
        check (acc (0, 2) == Accidental::natural, "C after C# in the bar shows a natural");
        check (acc (0, 3) == Accidental::sharp, "C# again after the natural shows a sharp");
        check (acc (1, 0) == Accidental::sharp, "the next bar starts fresh: sharp again");
        check (acc (1, 1) == Accidental::sharp, "carry is per octave: C#5 shows its own sharp");
    }
    {
        expect (stepName ({ 61, spell (61, 0).step, spell (61, 0).alter }), "C#4", "61 in C is C#");
        expect (stepName ({ 61, spell (61, -1).step, spell (61, -1).alter }), "Db4", "61 in F is Db");
        expect (stepName ({ 70, spell (70, -1).step, spell (70, -1).alter }), "Bb4", "70 in F is Bb");
        expect (stepName ({ 70, spell (70, 2).step, spell (70, 2).alter }), "A#4", "70 in D is A#");
        expect (stepName ({ 66, spell (66, -6).step, spell (66, -6).alter }), "Gb4", "66 in Gb is Gb");
        expect (stepName ({ 59, spell (59, -6).step, spell (59, -6).alter }), "Cb4", "59 in Gb is Cb (an octave-crossing spelling)");
        expect (stepName ({ 60, spell (60, 7).step, spell (60, 7).alter }), "B#3", "60 in C# is B#");

        auto o = plain();
        o.keyFifths = 2;    // D major: F# and C# are in the key signature
        auto d = layout (notes ({ { 61, 0, 960 }, { 66, 960, 960 }, { 65, 1920, 960 } }), o);
        check (firstNote (d, 0, 0, 0)->notes[0].accidental == Accidental::none, "C# in D major needs no accidental");
        check (firstNote (d, 0, 0, 2)->notes[0].accidental == Accidental::natural, "F in D major shows a natural");
        o.keyFifths = -4;   // Ab major
        auto ab = layout (notes ({ { 61, 0, 960 } }), o);
        check (firstNote (ab, 0, 0)->notes[0].accidental == Accidental::none && stepName (firstNote (ab, 0, 0)->notes[0]) == "Db4",
               "Db in Ab major: in the key signature");

        check (scaleStep (60, 0, 1) == 62 && scaleStep (64, 0, 1) == 65 && scaleStep (61, 0, 1) == 62 && scaleStep (60, 0, -1) == 59,
               "scale steps in C");
        check (scaleStep (64, 2, 1) == 66 && pitchForStep (31, 2) == 66 && pitchForStep (28, -2) == 60, "scale steps and staff steps in a key");
    }
    {
        auto l = layout (notes ({ { 60, 0, 960 }, { 64, 0, 960 }, { 67, 0, 960 } }), plain());
        expect (describe (l, 0, 0, true), "q[C4 E4 G4] rq rh", "a chord stacks on one stem");
        check (firstNote (l, 0, 0)->notes.size() == 3 && firstNote (l, 0, 0)->stemUp, "three noteheads, stem up below the middle line");

        auto stems = layout (notes ({ { 71, 0, 960 }, { 69, 960, 960 } }), plain());
        check (! firstNote (stems, 0, 0, 0)->stemUp && firstNote (stems, 0, 0, 1)->stemUp, "B4 stem down, A4 stem up");
    }

    std::cout << "staves\n";
    {
        auto grand = layout (notes ({ { 48, 0, 1920 }, { 72, 0, 960 }, { 60, 960, 960 } }), plain());
        check (grand.staves.size() == 2 && grand.staves[1] == Clef::bass, "a wide range gets a grand staff");
        expect (describe (grand, 0, 0, true) + " | " + describe (grand, 1, 0, true), "q[C5] q[C4] rh | h[C3] rh",
                "split at middle C");
        check (layout (notes ({ { 40, 0, 960 }, { 55, 960, 960 } }), plain()).staves[0] == Clef::bass, "low notes: bass clef");
        check (layout (notes ({ { 69, 0, 960 } }), plain()).staves.size() == 1, "high notes: one treble staff");

        Options drums = plain();
        drums.percussion = true;
        drums.interpret = true;
        auto kit = layout (notes ({ { 36, 0, 240 }, { 42, 0, 240 }, { 42, 480, 240 }, { 38, 960, 240 }, { 42, 960, 240 } }), drums);
        auto* first = firstNote (kit, 0, 0);
        check (kit.staves[0] == Clef::percussion && first != nullptr && first->notes.size() == 2
               && first->notes[0].step == 31 && ! first->notes[0].cross && first->notes[1].step == 39 && first->notes[1].cross,
               "drums: kick on F4, hi-hat as an x on G5");
        expect (describe (kit, 0, 0), "8 8 q rh", "drum hits read as eighths with interpretation");
    }

    std::cout << "triplets\n";
    {
        auto o = plain();
        o.resolution = 320;
        auto l = layout (notes ({ { 72, 0, 320 }, { 74, 320, 320 }, { 76, 640, 320 }, { 77, 960, 960 } }), o);
        expect (describe (l, 0, 0), "8t 8t 8t q rh", "three triplet eighths in a beat");
        check (l.beams.size() == 1 && l.beams[0].elements.size() == 3, "the triplet is beamed");
    }

    std::cout << (failures == 0 ? "all score checks passed\n" : juce::String (failures) + " score check(s) failed\n");
    return failures == 0 ? 0 : 1;
}

//==============================================================================
// woodshed-bench score-render <folder>: sample scores as PNGs, for looking at the notation.
namespace
{
void renderTo (const Layout& l, const juce::File& file, float staffSpace = 10.0f, double ppt = 0.11)
{
    Renderer r;
    r.setLayout (l, staffSpace, ppt);
    const int scale = 2;
    juce::Image image (juce::Image::ARGB, (int) r.getWidth() * scale, (int) r.getHeight() * scale, true);
    {
        juce::Graphics g (image);
        g.addTransform (juce::AffineTransform::scale ((float) scale));
        Style style;
        g.fillAll (style.paper);
        r.paint (g, style, [] (int id) { return id == 3; }, { 0.0f, r.getWidth() });
    }
    bool hitsMatch = true;
    for (auto& e : l.elements)
        for (auto& h : e.notes)
            hitsMatch = hitsMatch && r.noteAt (r.noteBounds (h.id).getCentre()) == h.id;
    check (hitsMatch, "every notehead hit-tests to its own note in " + file.getFileName());

    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat().writeImageToStream (image, out);
    std::cout << "wrote " << file.getFullPathName() << "\n";
}
} // namespace

int renderScoreSamples (const juce::File& folder);
int renderScoreSamples (const juce::File& folder)
{
    juce::ScopedJuceInitialiser_GUI init;
    folder.createDirectory();
    constexpr int q = quarter;

    // The demo song's parts.
    {
        std::vector<InputNote> chords;
        const std::vector<std::vector<int>> shapes { { 60, 64, 67 }, { 57, 60, 64 }, { 53, 57, 60 }, { 55, 59, 62 } };
        int id = 0;
        for (int b = 0; b < 4; ++b)
            for (int beat = 0; beat < 4; beat += 2)
                for (int p : shapes[(size_t) b])
                    chords.push_back ({ p, (juce::int64) (b * 4 + beat) * q, (juce::int64) q * 2 - 40, id++ });
        renderTo (layout (chords, Options()), folder.getChildFile ("chords.png"));
    }
    {
        std::vector<InputNote> bass;
        const int roots[] { 48, 45, 41, 43 };
        for (int b = 0; b < 4; ++b)
            for (int e = 0; e < 8; ++e)
                bass.push_back ({ roots[b] - 12 + (e == 6 ? 12 : 0), (juce::int64) (b * 8 + e) * q / 2, (juce::int64) q / 2 - 60, b * 8 + e });
        renderTo (layout (bass, Options()), folder.getChildFile ("bass.png"));
    }
    {
        std::vector<InputNote> drums;
        int id = 0;
        for (int b = 0; b < 2; ++b)
            for (int s = 0; s < 16; ++s)
            {
                const auto at = (juce::int64) (b * 16 + s) * q / 4;
                if (s == 0 || s == 8 || s == 10) drums.push_back ({ 36, at, q / 4, id++ });
                if (s == 4 || s == 12)           drums.push_back ({ 38, at, q / 4, id++ });
                if (s % 2 == 0)                  drums.push_back ({ 42, at, q / 4, id++ });
            }
        drums.push_back ({ 49, 0, q / 4, id++ });
        Options o;
        o.percussion = true;
        renderTo (layout (drums, o), folder.getChildFile ("drums.png"));
    }
    {
        // A key, accidentals, dots, ties, sixteenths, thirty-seconds, rests and ledger lines.
        std::vector<InputNote> mix {
            { 63, 0, 1440 }, { 65, 1440, 240 }, { 66, 1680, 240 }, { 68, 1920, 480 }, { 70, 2400, 240 }, { 71, 2640, 240 },
            { 75, 2880, 120 }, { 74, 3000, 120 }, { 72, 3120, 240 }, { 70, 3360, 960 },
            { 84, 4320, 480 }, { 86, 4800, 480 }, { 55, 5760, 1920 }, { 58, 5760, 1920 }, { 62, 5760, 1920 },
            { 61, 7680, 3840 }, { 64, 7680, 3840 }, { 65, 7680, 3840 }, { 72, 11520, 720 }, { 73, 12240, 240 }, { 60, 13440, 960 }
        };
        for (size_t i = 0; i < mix.size(); ++i)
            mix[i].id = (int) i;
        Options o;
        o.keyFifths = -3;
        o.resolution = 120;
        o.interpret = false;
        renderTo (layout (mix, o), folder.getChildFile ("mixed.png"));
        o.keyFifths = 4;
        o.staff = StaffMode::grand;
        renderTo (layout (mix, o), folder.getChildFile ("mixed-grand.png"));
    }
    {
        std::vector<InputNote> six;
        int id = 0;
        for (int i = 0; i < 6; ++i)
            six.push_back ({ 67 + (i % 3) * 2, (juce::int64) i * 480, 480, id++ });
        six.push_back ({ 72, 2880, 1440, id++ });
        six.push_back ({ 71, 4320 + 480, 960, id++ });
        six.push_back ({ 69, 5760, 2880, id++ });
        Options o;
        o.beatsPerBar = 6;
        o.beatUnit = 8;
        o.keyFifths = 1;
        o.interpret = false;
        renderTo (layout (six, o), folder.getChildFile ("six-eight.png"));

        std::vector<InputNote> trip;
        for (int i = 0; i < 6; ++i)
            trip.push_back ({ 72 + i, (juce::int64) i * 320, 320, i });
        trip.push_back ({ 79, 1920, 640, 6 });
        trip.push_back ({ 77, 2560, 640, 7 });
        trip.push_back ({ 76, 3200, 640, 8 });
        Options t;
        t.resolution = 320;
        t.interpret = false;
        renderTo (layout (trip, t), folder.getChildFile ("triplets.png"));
    }
    {
        // Big glyphs, to judge their shapes.
        std::vector<InputNote> n { { 77, 0, 3840, 0 }, { 79, 3840, 1920, 1 }, { 67, 5760, 960, 2 }, { 66, 6720, 480, 3 }, { 64, 7200, 240, 4 },
                                   { 63, 7440, 120, 5 }, { 62, 7560, 120, 6 } };
        Options o;
        o.resolution = 120;
        o.interpret = false;
        o.keyFifths = -2;
        o.to = 3840 * 3;
        renderTo (layout (n, o), folder.getChildFile ("glyphs.png"), 24.0f, 0.2);
    }
    {
        Renderer empty;
        empty.setLayout ({}, 9.0f, 0.1);
        check (empty.getWidth() <= 0.0f && empty.noteAt ({ 1.0f, 1.0f }) < 0, "an empty layout (no region) lays out as nothing");
    }
    return failures == 0 ? 0 : 1;
}
