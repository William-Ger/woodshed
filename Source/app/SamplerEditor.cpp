#include "SamplerEditor.h"
#include "ArrangeHeader.h"

namespace woodshed::app
{

namespace sids = sampler::ids;
using sampler::Mode;
using sampler::LoopMode;

namespace
{
void makeQuiet (juce::Component& c)
{
    // Space must keep starting playback after a click in the pane, not press the control again.
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

juce::String noteName (int pitch)
{
    static const char* const names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[((pitch % 12) + 12) % 12]) + juce::String (pitch / 12 - 2);     // middle C (60) is C3
}

bool isBlack (int pitch)
{
    const int pc = ((pitch % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

std::vector<float> knobValuesOf (const juce::ValueTree& inst, const std::vector<KnobSpec>& specs)
{
    std::vector<float> values;
    for (auto& k : specs) values.push_back (k.def);
    const auto stored = daw::Project::decodeValues (inst[daw::ids::knobs].toString());
    for (size_t i = 0; i < stored.size() && i < values.size(); ++i) values[i] = stored[i];
    return values;
}

const Mode modes[] { Mode::classic, Mode::oneShot, Mode::slice, Mode::multi };
}

//==============================================================================
/** The selected zone's sample, with its markers. In Slice mode, every slice's start. */
class SamplerEditor::Waveform final : public juce::Component
{
public:
    explicit Waveform (SamplerEditor& e) : editor (e) {}

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        theme::drawInset (g, r, theme::radiusPanel);
        const auto z = editor.zone (editor.selectedZone);
        src = z.isValid() ? editor.sourceOf (z) : nullptr;
        if (src == nullptr)
        {
            g.setColour (theme::faintText);
            g.setFont (theme::font (12.5f));
            g.drawText (z.isValid() ? "Can't find " + z[sids::file].toString() : juce::String ("Drop an audio file here"),
                        getLocalBounds(), juce::Justification::centred);
            return;
        }
        makePeaks();
        findRange();

        const auto colour = arrange::trackColour (editor.track);
        const auto zone = sampler::Zone::fromTree (z);
        const auto end = zone.end < 0 ? src->length() : zone.end;
        const float mid = r.getCentreY(), half = r.getHeight() * 0.45f;

        // The waveform, dimmed outside the zone.
        for (int x = 0; x < getWidth(); ++x)
        {
            const auto a = frameAt ((float) x), b = frameAt ((float) x + 1.0f);
            const int pa = (int) juce::jlimit ((juce::int64) 0, (juce::int64) peaks.size() - 1, a / peakHop);
            const int pb = (int) juce::jlimit ((juce::int64) pa + 1, (juce::int64) peaks.size(), b / peakHop + 1);
            float lo = 0.0f, hi = 0.0f;
            for (int i = pa; i < pb; ++i) { lo = juce::jmin (lo, peaks[(size_t) i].first); hi = juce::jmax (hi, peaks[(size_t) i].second); }
            const bool inside = a >= zone.start && a < end;
            g.setColour (inside ? colour.withAlpha (0.9f) : colour.withAlpha (0.25f));
            g.fillRect ((float) x, mid - hi * half, 1.0f, juce::jmax (1.0f, (hi - lo) * half));
        }

        // The loop, shaded, with its markers.
        if (zone.loop != LoopMode::off && editor.modeNow() != Mode::slice && editor.modeNow() != Mode::oneShot)
        {
            const float x0 = xAt (zone.loopStart), x1 = xAt (zone.loopEnd);
            g.setColour (theme::loop.withAlpha (0.12f));
            g.fillRect (juce::Rectangle<float> (x0, r.getY(), x1 - x0, r.getHeight()));
            if (zone.crossfade > 0)
            {
                g.setColour (theme::loop.withAlpha (0.25f));
                const float xf = xAt (zone.loopEnd - zone.crossfade);
                juce::Path p;
                p.addTriangle (xf, r.getBottom(), x1, r.getY(), x1, r.getBottom());
                g.fillPath (p);
            }
            marker (g, zone.loopStart, theme::loop, "L");
            marker (g, zone.loopEnd, theme::loop, "L");
        }

        if (editor.modeNow() == Mode::slice)
        {
            for (int i = 0; i < editor.numZones(); ++i)
            {
                const auto zi = sampler::Zone::fromTree (editor.zone (i));
                if (zi.file != zone.file) continue;
                marker (g, zi.start, i == editor.selectedZone ? theme::accent : theme::text.withAlpha (0.6f), juce::String (i + 1));
            }
            marker (g, end, theme::record, "E");
        }
        else
        {
            marker (g, zone.start, theme::accent, "S");
            marker (g, end, theme::record, "E");
        }

        g.setColour (theme::faintText);
        g.setFont (theme::readout (10.0f, theme::Weight::medium));
        g.drawText (juce::String ((double) (end - zone.start) / src->rate, 3) + " s  " + juce::String (src->rate / 1000.0, 1) + " kHz  "
                        + (src->audio.getNumChannels() > 1 ? "stereo" : "mono"),
                    getLocalBounds().reduced (8, 6), juce::Justification::bottomRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        drag = hit (e.position.x);
        if (drag.kind != Kind::none)
        {
            editor.studio.project().undo.beginNewTransaction ("Move Sampler Marker");
            return;
        }
        // Slice mode: a click picks (and plays) the slice under it.
        if (src != nullptr && editor.modeNow() == Mode::slice)
        {
            const auto f = frameAt (e.position.x);
            for (int i = editor.numZones(); --i >= 0;)
            {
                const auto z = sampler::Zone::fromTree (editor.zone (i));
                if (f >= z.start)
                {
                    editor.selectedZone = i;
                    editor.studio.auditionNote (z.lo);
                    editor.refresh();
                    break;
                }
            }
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag.kind == Kind::none || src == nullptr)
            return;
        auto z = editor.zone (drag.zone);
        const auto zone = sampler::Zone::fromTree (z);
        const auto len = src->length();
        const auto end = zone.end < 0 ? len : zone.end;
        auto f = juce::jlimit ((juce::int64) 0, len, frameAt (e.position.x));
        auto* undo = &editor.studio.project().undo;
        constexpr juce::int64 minimum = 64;

        switch (drag.kind)
        {
            case Kind::start:     z.setProperty (sids::start, juce::jlimit ((juce::int64) 0, end - minimum, f), undo); break;
            case Kind::end:       z.setProperty (sids::end, juce::jlimit (zone.start + minimum, len, f), undo); break;
            case Kind::loopStart: z.setProperty (sids::loopStart, juce::jlimit (zone.start, zone.loopEnd - 32, f), undo); break;
            case Kind::loopEnd:   z.setProperty (sids::loopEnd, juce::jlimit (zone.loopStart + 32, end, f), undo); break;
            case Kind::slice:
            {
                // A slice line moves this slice's start and the previous one's end together.
                auto prev = editor.zone (drag.zone - 1);
                const auto lo = prev.isValid() ? (juce::int64) prev[sids::start] + minimum : (juce::int64) 0;
                f = juce::jlimit (lo, end - minimum, f);
                z.setProperty (sids::start, f, undo);
                if (prev.isValid())
                    prev.setProperty (sids::end, f, undo);
                break;
            }
            case Kind::none:
            default: break;
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (hit (e.position.x).kind != Kind::none ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseUp (const juce::MouseEvent&) override         { drag = {}; }

private:
    enum class Kind { none, start, end, loopStart, loopEnd, slice };
    struct Hit { Kind kind = Kind::none; int zone = 0; };

    SamplerEditor& editor;
    std::shared_ptr<const sampler::Source> src, peaksOf;
    std::vector<std::pair<float, float>> peaks;
    static constexpr int peakHop = 64;
    juce::int64 viewFrom = 0, viewTo = 1;
    Hit drag;

    void makePeaks()
    {
        if (peaksOf == src)
            return;
        peaksOf = src;
        peaks.assign ((size_t) (src->length() / peakHop + 1), { 0.0f, 0.0f });
        for (int c = 0; c < src->audio.getNumChannels(); ++c)
        {
            const auto* d = src->audio.getReadPointer (c);
            for (juce::int64 i = 0; i < src->length(); ++i)
            {
                auto& p = peaks[(size_t) (i / peakHop)];
                p.first = juce::jmin (p.first, d[i]);
                p.second = juce::jmax (p.second, d[i]);
            }
        }
    }

    /** The span of the file the zones use (all of it for one zone covering it), padded a little. */
    void findRange()
    {
        const auto file = editor.zone (editor.selectedZone)[sids::file].toString();
        juce::int64 lo = src->length(), hi = 0;
        for (int i = 0; i < editor.numZones(); ++i)
        {
            const auto z = sampler::Zone::fromTree (editor.zone (i));
            if (z.file != file) continue;
            lo = juce::jmin (lo, z.start);
            hi = juce::jmax (hi, z.end < 0 ? src->length() : z.end);
        }
        if (hi <= lo) { lo = 0; hi = src->length(); }
        const auto margin = (hi - lo) / 40;
        viewFrom = juce::jmax ((juce::int64) 0, lo - margin);
        viewTo = juce::jmin (src->length(), hi + margin);
        if (viewTo <= viewFrom) viewTo = viewFrom + 1;
    }

    float xAt (juce::int64 frame) const
    {
        return (float) ((double) (frame - viewFrom) / (double) (viewTo - viewFrom) * getWidth());
    }

    juce::int64 frameAt (float x) const
    {
        return viewFrom + (juce::int64) std::llround ((double) x / juce::jmax (1, getWidth()) * (double) (viewTo - viewFrom));
    }

    void marker (juce::Graphics& g, juce::int64 frame, juce::Colour c, const juce::String& label)
    {
        const float x = xAt (frame);
        g.setColour (c);
        g.fillRect (juce::Rectangle<float> (x - 0.75f, 0.0f, 1.5f, (float) getHeight()));
        const auto flag = juce::Rectangle<float> (x, 2.0f, juce::jmax (14.0f, 7.0f * (float) label.length() + 6.0f), 14.0f);
        g.fillRoundedRectangle (flag, 3.0f);
        g.setColour (theme::window);
        g.setFont (theme::font (10.0f, theme::Weight::bold));
        g.drawText (label, flag, juce::Justification::centred);
    }

    Hit hit (float x) const
    {
        if (src == nullptr)
            return {};
        const auto z = sampler::Zone::fromTree (editor.zone (editor.selectedZone));
        const auto end = z.end < 0 ? src->length() : z.end;
        auto near = [&] (juce::int64 f) { return std::abs (xAt (f) - x) < 6.0f; };
        const bool slice = editor.modeNow() == Mode::slice;
        if (slice)
        {
            for (int i = 0; i < editor.numZones(); ++i)
                if (near (sampler::Zone::fromTree (editor.zone (i)).start))
                    return { Kind::slice, i };
            const int last = editor.numZones() - 1;
            if (near (sampler::Zone::fromTree (editor.zone (last)).end))
                return { Kind::end, last };
            return {};
        }
        if (z.loop != LoopMode::off && editor.modeNow() != Mode::oneShot)
        {
            if (near (z.loopEnd))   return { Kind::loopEnd, editor.selectedZone };
            if (near (z.loopStart)) return { Kind::loopStart, editor.selectedZone };
        }
        if (near (end))     return { Kind::end, editor.selectedZone };
        if (near (z.start)) return { Kind::start, editor.selectedZone };
        return {};
    }
};

//==============================================================================
/** The keyboard with each zone's key and velocity range drawn above it; click a zone to pick it,
    a key to hear it. */
class SamplerEditor::KeyMap final : public juce::Component
{
public:
    explicit KeyMap (SamplerEditor& e) : editor (e) {}

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds();
        const auto zonesArea = r.withHeight (r.getHeight() - keysHeight);
        const auto keysArea = r.withTrimmedTop (r.getHeight() - keysHeight);
        theme::drawInset (g, zonesArea.toFloat(), theme::radiusChip);

        const auto colour = arrange::trackColour (editor.track);
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < editor.numZones(); ++i)
            {
                const bool selected = i == editor.selectedZone;
                if (selected != (pass == 1)) continue;
                const auto z = sampler::Zone::fromTree (editor.zone (i));
                const auto box = zoneBox (z, zonesArea);
                g.setColour (selected ? theme::accent.withAlpha (0.35f) : colour.withAlpha (0.28f));
                g.fillRect (box);
                g.setColour (selected ? theme::accent : colour.withAlpha (0.8f));
                g.drawRect (box, 1.0f);
            }

        for (int k = 0; k < 128; ++k)
        {
            const auto key = keyRect (k, keysArea);
            g.setColour (isBlack (k) ? juce::Colour (0xff2a2a2e) : juce::Colour (0xffd9d9de));
            g.fillRect (key.reduced (0.0f, 0.0f).withTrimmedRight (0.5f));
            if (k % 12 == 0)
            {
                g.setColour (theme::faintText);
                g.setFont (theme::font (9.0f, theme::Weight::medium));
                g.drawText (noteName (k), juce::Rectangle<float> (key.getX() + 1.0f, key.getBottom() - 11.0f, 24.0f, 10.0f),
                            juce::Justification::left);
            }
        }
        // Each zone's root key.
        for (int i = 0; i < editor.numZones(); ++i)
        {
            const auto z = sampler::Zone::fromTree (editor.zone (i));
            const auto key = keyRect (z.root, keysArea);
            g.setColour (i == editor.selectedZone ? theme::accent : colour);
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ key.getCentreX(), key.getY() + 6.0f }));
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto r = getLocalBounds();
        const int key = juce::jlimit (0, 127, (int) (e.position.x / (float) juce::jmax (1, r.getWidth()) * 128.0f));
        if (e.position.y >= (float) (r.getHeight() - keysHeight))
        {
            editor.studio.auditionNote (key);
            return;
        }
        // The zone under the click; clicking again cycles through overlapping ones.
        const auto zonesArea = r.withHeight (r.getHeight() - keysHeight);
        const int n = editor.numZones();
        for (int step = 1; step <= n; ++step)
        {
            const int i = (editor.selectedZone + step) % n;
            if (zoneBox (sampler::Zone::fromTree (editor.zone (i)), zonesArea).contains (e.position))
            {
                editor.selectedZone = i;
                editor.refresh();
                return;
            }
        }
    }

private:
    SamplerEditor& editor;
    static constexpr int keysHeight = 22;

    juce::Rectangle<float> keyRect (int k, juce::Rectangle<int> area) const
    {
        const float w = (float) area.getWidth() / 128.0f;
        return { (float) area.getX() + w * (float) k, (float) area.getY(), w, (float) area.getHeight() };
    }

    juce::Rectangle<float> zoneBox (const sampler::Zone& z, juce::Rectangle<int> area) const
    {
        const float w = (float) area.getWidth() / 128.0f;
        const float top = (float) area.getY() + 2.0f, h = (float) area.getHeight() - 4.0f;
        const float y0 = top + h * (1.0f - (float) z.velHi / 127.0f), y1 = top + h * (1.0f - (float) (z.velLo - 1) / 127.0f);
        return { (float) area.getX() + w * (float) z.lo, y0, w * (float) (z.hi - z.lo + 1), juce::jmax (2.0f, y1 - y0) };
    }
};

//==============================================================================
SamplerEditor::SamplerEditor (Studio& s) : studio (s)
{
    setOpaque (true);
    waveform = std::make_unique<Waveform> (*this);
    keyMap = std::make_unique<KeyMap> (*this);
    addAndMakeVisible (*waveform);
    addAndMakeVisible (*keyMap);

    for (int i = 0; i < (int) std::size (modes); ++i)
        mode.addItem (sampler::modeName (modes[i]), i + 1);
    mode.setTooltip ("Classic: one sample across the keyboard at its pitch. One Shot: plays the whole sample, ignoring note-off. "
                     "Slice: a slice per transient on keys from C1. Multi: a zone per file, mapped by pitch");
    mode.onChange = [this] { if (! updating) setMode (modes[(size_t) juce::jlimit (0, 3, mode.getSelectedId() - 1)]); };
    makeQuiet (mode);
    addAndMakeVisible (mode);

    load.setTooltip ("Load an audio file (it's copied into the project)");
    load.onClick = [this] { chooseFile(); };
    makeQuiet (load);
    addAndMakeVisible (load);

    sensitivity.setSliderStyle (juce::Slider::LinearBar);
    sensitivity.setRange (0.0, 1.0, 0.01);
    sensitivity.setTextValueSuffix (" sensitivity");
    sensitivity.textFromValueFunction = [] (double v) { return "Sensitivity " + juce::String (juce::roundToInt (v * 100.0)) + "%"; };
    sensitivity.setTooltip ("How many transients become slices");
    sensitivity.onDragEnd = [this]
    {
        auto node = samplerNode();
        const auto z = zone (0);
        auto src = z.isValid() ? sourceOf (z) : nullptr;
        if (! node.isValid() || src == nullptr)
            return;
        auto& undo = studio.project().undo;
        undo.beginNewTransaction ("Slice Sensitivity");
        node.setProperty (sids::sensitivity, sensitivity.getValue(), &undo);
        sampler::reslice (node, *src, &undo);
        selectedZone = 0;
    };
    makeQuiet (sensitivity);
    addAndMakeVisible (sensitivity);

    gate.setClickingTogglesState (true);
    gate.setTooltip ("Gate: a slice stops when its note does (off: every slice plays to its end)");
    gate.onClick = [this]
    {
        auto node = samplerNode();
        if (! node.isValid()) return;
        studio.project().undo.beginNewTransaction ("Slice Gate");
        node.setProperty (sids::gate, gate.getToggleState(), &studio.project().undo);
    };
    makeQuiet (gate);
    addAndMakeVisible (gate);

    loopMode.addItem ("Loop Off", 1);
    loopMode.addItem ("Loop Forward", 2);
    loopMode.addItem ("Loop Alternate", 3);
    loopMode.onChange = [this]
    {
        if (updating) return;
        auto z = zone (selectedZone);
        if (! z.isValid()) return;
        auto& undo = studio.project().undo;
        undo.beginNewTransaction ("Sampler Loop");
        const auto m = loopMode.getSelectedId() == 2 ? LoopMode::forward : loopMode.getSelectedId() == 3 ? LoopMode::alternate : LoopMode::off;
        z.setProperty (sids::loop, sampler::loopId (m), &undo);
        // A first loop covers the second half of the zone.
        const auto zz = sampler::Zone::fromTree (z);
        if (m != LoopMode::off && zz.loopEnd - zz.loopStart < 32)
            if (auto src = sourceOf (z))
            {
                const auto end = zz.end < 0 ? src->length() : zz.end;
                z.setProperty (sids::loopStart, zz.start + (end - zz.start) / 2, &undo);
                z.setProperty (sids::loopEnd, end, &undo);
                z.setProperty (sids::crossfade, (juce::int64) (src->rate * 0.02), &undo);
            }
    };
    makeQuiet (loopMode);
    addAndMakeVisible (loopMode);

    // The selected zone's settings.
    struct Spec { const char* label; const juce::Identifier* id; double lo, hi, step; int kind; };   // kind: 0 number, 1 note, 2 ms
    const Spec specs[] { { "Root", &sids::root, 0, 127, 1, 1 }, { "Low Key", &sids::lo, 0, 127, 1, 1 }, { "High Key", &sids::hi, 0, 127, 1, 1 },
                         { "Tune", &sids::tune, -48, 48, 1, 0 }, { "Fine", &sids::fine, -100, 100, 1, 0 }, { "Gain", &sids::gain, -36, 12, 0.1, 0 },
                         { "Pan", &sids::pan, -1, 1, 0.01, 0 }, { "Low Vel", &sids::velLo, 1, 127, 1, 0 }, { "High Vel", &sids::velHi, 1, 127, 1, 0 },
                         { "RR Group", &sids::group, 0, 63, 1, 0 }, { "X-Fade", &sids::crossfade, 0, 1000, 1, 2 } };
    for (auto& sp : specs)
    {
        auto f = std::make_unique<Field>();
        f->label = sp.label;
        f->property = *sp.id;
        auto& sl = f->slider;
        sl.setSliderStyle (juce::Slider::LinearBar);
        sl.setRange (sp.lo, sp.hi, sp.step);
        sl.setColour (juce::Slider::trackColourId, theme::accentDim);
        const auto unit = juce::String (sp.label) == "Tune" ? " st" : juce::String (sp.label) == "Fine" ? " ct"
                        : juce::String (sp.label) == "Gain" ? " dB" : sp.kind == 2 ? " ms" : "";
        if (sp.kind == 1)
            sl.textFromValueFunction = [] (double v) { return noteName (juce::roundToInt (v)) + "  (" + juce::String (juce::roundToInt (v)) + ")"; };
        else
            sl.textFromValueFunction = [unit, step = sp.step] (double v) { return juce::String (v, step < 1.0 ? (step < 0.1 ? 2 : 1) : 0) + unit; };
        sl.setDoubleClickReturnValue (true, sp.kind == 1 ? 60.0 : juce::String (sp.label) == "High Vel" ? 127.0 : juce::String (sp.label) == "Low Vel" ? 1.0 : 0.0);
        auto* field = f.get();
        sl.onDragStart = [this, field] { studio.project().undo.beginNewTransaction ("Sampler " + field->label); };
        sl.onValueChange = [this, field, kind = sp.kind]
        {
            if (updating) return;
            if (! field->slider.isMouseButtonDown())
                studio.project().undo.beginNewTransaction ("Sampler " + field->label);
            auto z = zone (selectedZone);
            if (! z.isValid()) return;
            auto v = field->slider.getValue();
            if (kind == 2)
            {
                auto src = sourceOf (z);
                z.setProperty (field->property, (juce::int64) std::llround (v * 0.001 * (src != nullptr ? src->rate : 48000.0)), &studio.project().undo);
                return;
            }
            // Key ranges keep low <= root's range <= high.
            if (field->property == sids::lo)    v = juce::jmin (v, (double) (int) z.getProperty (sids::hi, 127));
            if (field->property == sids::hi)    v = juce::jmax (v, (double) (int) z.getProperty (sids::lo, 0));
            if (field->property == sids::velLo) v = juce::jmin (v, (double) (int) z.getProperty (sids::velHi, 127));
            if (field->property == sids::velHi) v = juce::jmax (v, (double) (int) z.getProperty (sids::velLo, 1));
            const bool integer = kind == 1 || field->property == sids::velLo || field->property == sids::velHi || field->property == sids::group
                              || field->property == sids::tune;
            z.setProperty (field->property, integer ? juce::var (juce::roundToInt (v)) : juce::var (v), &studio.project().undo);
        };
        makeQuiet (sl);
        addAndMakeVisible (sl);
        fields.push_back (std::move (f));
    }

    knobs.onGestureStart = [this] { studio.project().undo.beginNewTransaction ("Sampler Knob"); };
    knobs.onChange = [this] (int index, float value)
    {
        auto inst = track.getChildWithName (daw::ids::INSTRUMENT);
        auto* type = InstrumentRegistry::get().find (sampler::typeId);
        if (! inst.isValid() || type == nullptr) return;
        auto values = knobValuesOf (inst, type->info.knobs);
        if (juce::isPositiveAndBelow (index, (int) values.size()))
            values[(size_t) index] = value;
        inst.setProperty (daw::ids::knobs, daw::Project::encodeValues (values), &studio.project().undo);
    };
    if (auto* type = InstrumentRegistry::get().find (sampler::typeId))
        knobs.setSpecs (type->info.knobs, type->info.colour);
    knobView.setViewedComponent (&knobs, false);
    knobView.setScrollBarsShown (true, false);
    knobView.setScrollBarThickness (8);
    addAndMakeVisible (knobView);

    studio.addChangeListener (this);
    studio.addProjectSwapListener (this);
    sync();
}

SamplerEditor::~SamplerEditor()
{
    watched.removeListener (this);
    studio.removeProjectSwapListener (this);
    studio.removeChangeListener (this);
}

//==============================================================================
void SamplerEditor::newSamplerTrack (Studio& studio)
{
    if (studio.addInstrumentTrack (sampler::typeId).isValid())
    {
        studio.project().undo.setCurrentTransactionName ("New Sampler Track");
        studio.setPane (Studio::Pane::sampler);
    }
}

bool SamplerEditor::canConvertSelection (Studio& studio)
{
    for (int id : studio.getSelectedRegions())
        if (sampler::canConvert (studio.findRegion (id)))
            return true;
    return false;
}

void SamplerEditor::convertSelectedRegion (Studio& studio, Mode m)
{
    for (int id : studio.getSelectedRegions())
    {
        auto region = studio.findRegion (id);
        if (! sampler::canConvert (region))
            continue;
        auto& p = studio.project();
        p.undo.beginNewTransaction (m == Mode::slice ? "Convert to Sampler Track (Slice)" : "Convert to Sampler Track");
        auto t = sampler::convertToSamplerTrack (p, region, m);
        if (! t.isValid())
        {
            studio.flash ("Couldn't read that region's audio");
            return;
        }
        studio.selectTrack ((int) t[daw::ids::id]);
        for (auto r : t)
            if (daw::Project::isMidiRegion (r))
                studio.setSelectedRegions ({ (int) r[daw::ids::id] });
        studio.setPane (Studio::Pane::sampler);
        return;
    }
}

//==============================================================================
void SamplerEditor::projectAboutToBeReplaced()
{
    watched.removeListener (this);
    watched = {};
    track = {};
}

void SamplerEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    sync();
}

bool SamplerEditor::belongsToTrack (const juce::ValueTree& tree) const
{
    return track.isValid() && (tree == track || tree.isAChildOf (track));
}

void SamplerEditor::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (belongsToTrack (tree))
        refresh();
}

void SamplerEditor::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (belongsToTrack (parent))
        refresh();
}

void SamplerEditor::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (child == track)
        sync();
    else if (belongsToTrack (parent))
        refresh();
}

void SamplerEditor::sync()
{
    if (watched != studio.project().state)
    {
        watched.removeListener (this);
        watched = studio.project().state;
        watched.addListener (this);
    }
    const auto next = studio.getSelectedTrack();
    if (next != track)
    {
        track = next;
        selectedZone = 0;
    }
    refresh();
}

juce::ValueTree SamplerEditor::samplerNode() const
{
    return track.getChildWithName (daw::ids::INSTRUMENT).getChildWithName (sids::SAMPLER);
}

int SamplerEditor::numZones() const
{
    int n = 0;
    for (auto c : samplerNode())
        if (c.hasType (sids::ZONE)) ++n;
    return n;
}

juce::ValueTree SamplerEditor::zone (int index) const
{
    int n = 0;
    for (auto c : samplerNode())
        if (c.hasType (sids::ZONE) && n++ == index)
            return c;
    return {};
}

Mode SamplerEditor::modeNow() const
{
    return sampler::modeFrom (samplerNode()[sids::mode].toString());
}

std::shared_ptr<const sampler::Source> SamplerEditor::sourceOf (const juce::ValueTree& z) const
{
    const auto file = z[sids::file].toString();
    if (file.isEmpty())
        return nullptr;
    return sampler::loadSource (juce::File::isAbsolutePath (file) ? juce::File (file) : studio.project().resolve (file));
}

void SamplerEditor::refresh()
{
    const juce::ScopedValueSetter<bool> guard (updating, true);
    const bool editing = isEditing();
    selectedZone = juce::jlimit (0, juce::jmax (0, numZones() - 1), selectedZone);
    const auto m = modeNow();
    const auto node = samplerNode();
    const auto z = zone (selectedZone);

    for (auto* c : std::initializer_list<juce::Component*> { waveform.get(), keyMap.get(), &mode, &knobView, &loopMode })
        c->setVisible (editing);
    sensitivity.setVisible (editing && m == Mode::slice);
    gate.setVisible (editing && m == Mode::slice);

    mode.setSelectedId ((int) (std::find (std::begin (modes), std::end (modes), m) - std::begin (modes)) + 1, juce::dontSendNotification);
    sensitivity.setValue ((double) node.getProperty (sids::sensitivity, 0.5), juce::dontSendNotification);
    gate.setToggleState ((bool) node.getProperty (sids::gate, false), juce::dontSendNotification);

    const auto zz = sampler::Zone::fromTree (z);
    loopMode.setSelectedId (zz.loop == LoopMode::forward ? 2 : zz.loop == LoopMode::alternate ? 3 : 1, juce::dontSendNotification);
    loopMode.setEnabled (z.isValid() && m != Mode::slice && m != Mode::oneShot);
    const auto src = z.isValid() ? sourceOf (z) : nullptr;
    for (auto& f : fields)
    {
        f->slider.setVisible (editing);
        f->slider.setEnabled (z.isValid());
        double v = 0.0;
        if (f->property == sids::crossfade)
            v = (double) zz.crossfade / (src != nullptr ? src->rate : 48000.0) * 1000.0;
        else if (z.isValid())
            v = (double) z.getProperty (f->property, f->property == sids::hi || f->property == sids::velHi ? 127
                                                      : f->property == sids::velLo ? 1 : f->property == sids::root ? 60 : 0);
        f->slider.setValue (v, juce::dontSendNotification);
    }

    if (auto* type = InstrumentRegistry::get().find (sampler::typeId); type != nullptr && editing)
        knobs.setValues (knobValuesOf (track.getChildWithName (daw::ids::INSTRUMENT), type->info.knobs));

    resized();
    repaint();
    waveform->repaint();
    keyMap->repaint();
}

//==============================================================================
void SamplerEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a sample", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   sampler::audioFileWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc) { loadFiles (fc.getResults()); });
}

bool SamplerEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (sampler::isAudioFile (juce::File (f)))
            return true;
    return false;
}

void SamplerEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dropping = false;
    juce::Array<juce::File> list;
    for (auto& f : files)
        if (sampler::isAudioFile (juce::File (f)))
            list.add (juce::File (f));
    loadFiles (list);
}

void SamplerEditor::loadFiles (const juce::Array<juce::File>& files)
{
    if (files.isEmpty())
        return;
    if (! isEditing())
    {
        newSamplerTrack (studio);
        sync();
        if (! isEditing())
            return;
    }

    auto& p = studio.project();
    p.undo.beginNewTransaction (files.size() > 1 ? "Load Samples" : "Load Sample");
    const auto current = modeNow();
    bool ok = false;
    for (int i = 0; i < files.size(); ++i)
    {
        // Several files make a multi-zone instrument (the first replacing what was there).
        const auto m = files.size() == 1 ? current : (i == 0 && current != Mode::multi ? Mode::classic : Mode::multi);
        ok = sampler::loadFileIntoTrack (p, track, files[i], m) || ok;
    }
    if (files.size() > 1)
        samplerNode().setProperty (sids::mode, sampler::modeId (Mode::multi), &p.undo);
    if (! ok)
        studio.flash ("Couldn't read that audio file");
    selectedZone = current == Mode::multi && files.size() == 1 ? numZones() - 1 : 0;
    refresh();
}

void SamplerEditor::setMode (Mode m)
{
    auto node = samplerNode();
    if (! isEditing())
        return;
    auto& undo = studio.project().undo;
    undo.beginNewTransaction ("Sampler Mode: " + sampler::modeName (m));
    if (! node.isValid())
        node = sampler::samplerNode (track.getChildWithName (daw::ids::INSTRUMENT), &undo);

    // The first zone's file again, over the span its zones cover, laid out for the new mode.
    const auto first = zone (0);
    auto src = first.isValid() ? sourceOf (first) : nullptr;
    if (src == nullptr)
    {
        node.setProperty (sids::mode, sampler::modeId (m), &undo);
        return;
    }
    const auto file = first[sids::file].toString();
    juce::int64 from = src->length(), to = 0;
    for (int i = 0; i < numZones(); ++i)
    {
        const auto z = sampler::Zone::fromTree (zone (i));
        if (z.file != file) continue;
        from = juce::jmin (from, z.start);
        to = juce::jmax (to, z.end < 0 ? src->length() : z.end);
    }
    if (m == Mode::multi)
    {
        node.setProperty (sids::mode, sampler::modeId (m), &undo);   // Multi keeps the zones; add more by dropping files
        return;
    }
    sampler::loadSample (node, m, file, *src, from, to, &undo);
    selectedZone = 0;
}

//==============================================================================
void SamplerEditor::resized()
{
    auto header = getLocalBounds().removeFromTop (headerHeight).reduced (12, 7);
    load.setBounds (header.removeFromRight (120));
    header.removeFromRight (8);
    mode.setBounds (header.removeFromRight (120));
    header.removeFromRight (8);
    gate.setBounds (header.removeFromRight (64));
    header.removeFromRight (8);
    sensitivity.setBounds (header.removeFromRight (170));

    auto body = getLocalBounds().withTrimmedTop (headerHeight).reduced (12, 10);
    auto bottom = body.removeFromBottom (juce::jlimit (110, 210, body.getHeight() * 45 / 100));
    body.removeFromBottom (8);
    keyMap->setBounds (body.removeFromBottom (juce::jmin (64, body.getHeight() / 3)));
    body.removeFromBottom (6);
    waveform->setBounds (body);

    // The zone's settings on the left, in rows of four, then the instrument's knobs.
    auto left = bottom.removeFromLeft (juce::jmin (440, bottom.getWidth() / 2));
    bottom.removeFromLeft (12);
    const int columns = 4, cellW = left.getWidth() / columns, cellH = 38;
    loopMode.setBounds (left.getX() + 3 * cellW, left.getY() + 2 * cellH + 13, cellW - 6, 22);
    for (size_t i = 0; i < fields.size(); ++i)
    {
        const int c = (int) i % columns, row = (int) i / columns;
        fields[i]->slider.setBounds (left.getX() + c * cellW, left.getY() + row * cellH + 13, cellW - 6, 22);
    }
    knobView.setBounds (bottom);
    const int w = juce::jmax (1, bottom.getWidth() - 10);
    knobs.setSize (w, knobs.heightFor (w));
}

void SamplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);

    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (theme::bar);
    g.fillRect (header);
    g.setColour (theme::highlight);
    g.fillRect (header.withHeight (1));
    g.setColour (theme::separator);
    g.fillRect (header.removeFromBottom (1));

    auto title = header.reduced (12, 0).removeFromLeft (juce::jmin (360, header.getWidth() / 3));
    g.setFont (theme::font (13.0f, theme::Weight::semibold));
    if (isEditing())
    {
        g.setColour (arrange::trackColour (track));
        g.fillEllipse (title.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat());
        title.removeFromLeft (8);
    }
    g.setColour (isEditing() ? theme::text : theme::dimText);
    const auto z = zone (selectedZone);
    g.drawFittedText (isEditing() ? track[daw::ids::name].toString() + "  -  Sampler"
                                        + (z.isValid() ? "  -  " + z[sids::name].toString() : juce::String())
                                  : juce::String ("Sampler"),
                      title, juce::Justification::centredLeft, 1, 0.85f);

    const auto body = getLocalBounds().withTrimmedTop (headerHeight).reduced (12, 10);
    if (! isEditing())
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (13.0f));
        g.drawFittedText ("Select a Sampler track, or drop an audio file here to make one.\n"
                          "(Track > New Sampler Track; or right-click an audio region > Convert to Sampler Track.)",
                          body, juce::Justification::centred, 3);
    }
    else
    {
        // Field names above their sliders.
        g.setColour (theme::dimText);
        g.setFont (theme::font (10.5f, theme::Weight::semibold));
        for (auto& f : fields)
            g.drawText (f->label.toUpperCase(), f->slider.getBounds().translated (0, -13).withHeight (12), juce::Justification::left);
        g.drawText ("LOOP", loopMode.getBounds().translated (0, -13).withHeight (12), juce::Justification::left);
        if (z.isValid())
        {
            const auto zz = sampler::Zone::fromTree (z);
            g.setColour (theme::faintText);
            g.setFont (theme::readout (10.0f, theme::Weight::medium));
            g.drawText ("Zone " + juce::String (selectedZone + 1) + " of " + juce::String (numZones()) + "   " + noteName (zz.lo) + ".."
                            + noteName (zz.hi),
                        loopMode.getBounds().translated (0, 30).withWidth (300).withX (fields.front()->slider.getX()),
                        juce::Justification::left);
        }
    }

    if (dropping)
    {
        g.setColour (theme::accent.withAlpha (0.12f));
        g.fillRect (getLocalBounds());
        g.setColour (theme::accent);
        g.drawRect (getLocalBounds(), 2);
    }
}

} // namespace woodshed::app
