#include "PitchEditor.h"
#include "ArrangeHeader.h"
#include "../flex/Edits.h"
#include "../flex/Renderer.h"

namespace woodshed::app
{

namespace ids = daw::ids;
namespace fids = woodshed::flex::ids;
using woodshed::flex::PitchNote;

namespace
{
const char* const noteNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

juce::String pitchName (int midi)
{
    return juce::String (noteNames[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
}

bool isBlackKey (int midi)
{
    const int k = ((midi % 12) + 12) % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}
}

PitchEditor::PitchEditor (Studio& s) : studio (s)
{
    setOpaque (true);
    title.setFont (theme::font (13.0f, theme::Weight::semibold));
    title.setColour (juce::Label::textColourId, theme::text);
    addAndMakeVisible (title);

    addAndMakeVisible (detectButton);
    detectButton.setTooltip ("Find the notes again (edits are discarded)");
    detectButton.onClick = [this] { detectNotes (true); };
    addAndMakeVisible (quantizeButton);
    quantizeButton.setTooltip ("Move the selected notes (or all of them) onto the key and scale");
    quantizeButton.onClick = [this] { quantizeToScale(); };

    for (int i = 0; i < 12; ++i)
        keyBox.addItem (noteNames[i], i + 1);
    keyBox.setSelectedId (1, juce::dontSendNotification);
    const auto scales = woodshed::flex::scaleNames();
    for (int i = 0; i < scales.size(); ++i)
        scaleBox.addItem (scales[i], i + 1);
    scaleBox.setSelectedId (2, juce::dontSendNotification);
    addAndMakeVisible (keyBox);
    addAndMakeVisible (scaleBox);

    auto setUp = [this] (Control& c, const juce::String& name, double lo, double hi, double step, const juce::String& suffix,
                         std::function<void (PitchNote&, float)> set)
    {
        c.label.setText (name, juce::dontSendNotification);
        c.label.setFont (theme::font (11.0f));
        c.label.setColour (juce::Label::textColourId, theme::dimText);
        c.label.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (c.label);
        c.slider.setSliderStyle (juce::Slider::LinearBar);
        c.slider.setRange (lo, hi, step);
        c.slider.setTextValueSuffix (suffix);
        c.slider.setDoubleClickReturnValue (true, name == "Drift" || name == "Vibrato" ? 100.0 : 0.0);
        c.slider.onValueChange = [this, &c, set, name]
        {
            if (updatingSliders)
                return;
            const auto v = (float) c.slider.getValue();
            applyToSelection ([set, v] (PitchNote& n) { set (n, v); }, "Pitch Edit: " + name);
        };
        addAndMakeVisible (c.slider);
    };
    setUp (fine, "Fine", -100.0, 100.0, 1.0, " ct", [] (PitchNote& n, float v)
    {
        n.shift = std::round (n.pitch + n.shift) - n.pitch + v / 100.0f;
    });
    setUp (drift, "Drift", 0.0, 200.0, 1.0, " %", [] (PitchNote& n, float v) { n.drift = v / 100.0f; });
    setUp (vibrato, "Vibrato", 0.0, 200.0, 1.0, " %", [] (PitchNote& n, float v) { n.vibrato = v / 100.0f; });
    setUp (gain, "Gain", -24.0, 12.0, 0.1, " dB", [] (PitchNote& n, float v) { n.gain = v; });
    setUp (formant, "Formant", -12.0, 12.0, 0.1, " st", [] (PitchNote& n, float v) { n.formant = v; });

    studio.addChangeListener (this);
    sync();
    startTimerHz (30);
}

PitchEditor::~PitchEditor()
{
    studio.removeChangeListener (this);
    if (watched.isValid())
        watched.removeListener (this);
}

//==============================================================================
void PitchEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    sync();
}

void PitchEditor::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (dragging)
        return;
    if (tree == region || tree == region.getParent() || property == fids::pitchNotes || property == fids::flexMarkers)
    {
        notes = woodshed::flex::notesOf (region);
        selected.erase (std::remove_if (selected.begin(), selected.end(), [this] (int i) { return i >= (int) notes.size(); }), selected.end());
        updateSliders();
        repaint();
    }
}

void PitchEditor::timerCallback()
{
    if (! isShowing() || ! region.isValid())
        return;
    const float x = xForTimeline ((double) studio.engine().getPosition());
    if (std::abs (x - lastPlayheadX) >= 1.0f)
    {
        const auto area = rollArea();
        repaint (juce::Rectangle<int> ((int) lastPlayheadX - 2, area.getY(), 5, area.getHeight()));
        repaint (juce::Rectangle<int> ((int) x - 2, area.getY(), 5, area.getHeight()));
        lastPlayheadX = x;
    }
}

void PitchEditor::sync()
{
    if (watched.isValid() && watched != studio.project().state)
    {
        watched.removeListener (this);
        watched = {};
    }
    if (! watched.isValid())
    {
        watched = studio.project().state;
        watched.addListener (this);
    }

    const auto r = studio.getPitchEditRegion();
    if (r != region)
    {
        region = r;
        selected.clear();
        track = nullptr;
        audio = nullptr;
        if (region.isValid())
        {
            auto& p = studio.project();
            const auto file = p.resolve (region[ids::file].toString());
            audio = studio.engine().getAudioPool().get (file);
            track = studio.engine().getFlexRenderer().analysis().pitch (file.getFullPathName(), audio, p.grid().sampleRate);
            if (! region.hasProperty (fids::pitchNotes))
                detectNotes (false);
        }
        notes = woodshed::flex::notesOf (region);
        title.setText (region.isValid() ? "Pitch Edit: " + region[ids::name].toString()
                                        : juce::String ("Pitch Edit: select an audio region of a voice or a solo instrument"),
                       juce::dontSendNotification);
        updateSliders();
    }

    // The pitch range: every note in view, with room above and below.
    if (! notes.empty())
    {
        lowPitch = 127.0f;
        highPitch = 0.0f;
        for (auto& n : notes)
        {
            lowPitch = juce::jmin (lowPitch, n.pitch + n.shift);
            highPitch = juce::jmax (highPitch, n.pitch + n.shift);
        }
        lowPitch = std::floor (lowPitch) - 3.0f;
        highPitch = std::ceil (highPitch) + 3.0f;
        if (highPitch - lowPitch < 14.0f)
        {
            const float mid = 0.5f * (highPitch + lowPitch);
            lowPitch = std::floor (mid - 7.0f);
            highPitch = lowPitch + 14.0f;
        }
    }
    detectButton.setEnabled (region.isValid());
    quantizeButton.setEnabled (! notes.empty());
    repaint();
}

void PitchEditor::detectNotes (bool undoable)
{
    if (! region.isValid() || track == nullptr)
        return;
    auto& p = studio.project();
    const auto warp = woodshed::flex::warpOf (region);
    const double from = warp.toSrc ((double) (juce::int64) region[ids::offset]);
    const double to = warp.toSrc ((double) ((juce::int64) region[ids::offset] + (juce::int64) region[ids::length]));
    std::vector<PitchNote> found;
    for (auto& n : woodshed::flex::detectNotes (*track, p.grid().sampleRate))
        if (n.end > from && n.start < to)
            found.push_back (n);
    if (undoable)
        p.undo.beginNewTransaction ("Detect Notes");
    // Without any notes the region still gets the (empty) property, so it isn't detected again.
    if (found.empty())
        region.setProperty (fids::pitchNotes, "", undoable ? &p.undo : nullptr);
    else
        woodshed::flex::setNotes (region, found, undoable ? &p.undo : nullptr);
    notes = found;
    selected.clear();
}

void PitchEditor::commit (const std::vector<PitchNote>& edited, const juce::String& undoName)
{
    if (! region.isValid())
        return;
    auto& p = studio.project();
    p.undo.beginNewTransaction (undoName);
    auto t = region.getParent();
    if (! (bool) t[fids::flexOn])
    {
        if (! t.hasProperty (fids::flexMode))
            t.setProperty (fids::flexMode, woodshed::flex::modeId (woodshed::flex::Mode::monophonic), &p.undo);
        woodshed::flex::setTrackFlex (p, t, true);
    }
    woodshed::flex::setNotes (region, edited, &p.undo);
    notes = edited;
}

void PitchEditor::applyToSelection (std::function<void (PitchNote&)> change, const juce::String& undoName)
{
    if (selected.empty())
        return;
    auto edited = notes;
    for (int i : selected)
        if (juce::isPositiveAndBelow (i, (int) edited.size()))
            change (edited[(size_t) i]);
    // One undo step per slider gesture: later changes join the transaction they started.
    auto& p = studio.project();
    if (p.undo.getCurrentTransactionName() == undoName)
    {
        woodshed::flex::setNotes (region, edited, &p.undo);
        notes = edited;
    }
    else
    {
        commit (edited, undoName);
    }
    repaint();
}

void PitchEditor::quantizeToScale()
{
    auto edited = notes;
    woodshed::flex::quantizeToScale (edited, keyBox.getSelectedId() - 1, (woodshed::flex::Scale) (scaleBox.getSelectedId() - 1), selected);
    commit (edited, "Quantize Pitch");
    updateSliders();
    repaint();
}

void PitchEditor::updateSliders()
{
    const juce::ScopedValueSetter<bool> busy (updatingSliders, true);
    const bool any = ! selected.empty() && juce::isPositiveAndBelow (selected.front(), (int) notes.size());
    for (auto* c : { &fine, &drift, &vibrato, &gain, &formant })
        c->slider.setEnabled (any);
    if (! any)
        return;
    const auto& n = notes[(size_t) selected.front()];
    const float target = n.pitch + n.shift;
    fine.slider.setValue (std::round ((target - std::round (target)) * 100.0f), juce::dontSendNotification);
    drift.slider.setValue (n.drift * 100.0f, juce::dontSendNotification);
    vibrato.slider.setValue (n.vibrato * 100.0f, juce::dontSendNotification);
    gain.slider.setValue (n.gain, juce::dontSendNotification);
    formant.slider.setValue (n.formant, juce::dontSendNotification);
}

//==============================================================================
juce::Rectangle<int> PitchEditor::rollArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight).withTrimmedLeft (keysWidth);
}

float PitchEditor::xForTimeline (double sample) const
{
    const auto area = rollArea();
    if (! region.isValid())
        return (float) area.getX();
    const auto start = (double) (juce::int64) region[ids::start];
    const auto length = juce::jmax (1.0, (double) (juce::int64) region[ids::length]);
    return (float) area.getX() + (float) ((sample - start) / length) * (float) area.getWidth();
}

float PitchEditor::yForPitch (float pitch) const
{
    const auto area = rollArea();
    return (float) area.getY() + (highPitch - pitch) / (highPitch - lowPitch) * (float) area.getHeight();
}

double PitchEditor::timelineFor (double src) const
{
    const auto origin = (double) ((juce::int64) region[ids::start] - (juce::int64) region[ids::offset]);
    return origin + woodshed::flex::warpOf (region).toDst (src);
}

juce::Rectangle<float> PitchEditor::noteRect (const PitchNote& n) const
{
    const float row = (float) rollArea().getHeight() / (highPitch - lowPitch);
    const float x1 = xForTimeline (timelineFor (n.start)), x2 = xForTimeline (timelineFor (n.end));
    const float y = yForPitch (n.pitch + n.shift);
    return { x1, y - row * 0.45f, juce::jmax (3.0f, x2 - x1), row * 0.9f };
}

int PitchEditor::noteAt (juce::Point<int> p) const
{
    for (int i = (int) notes.size(); --i >= 0;)
        if (noteRect (notes[(size_t) i]).expanded (0.0f, 2.0f).contains (p.toFloat()))
            return i;
    return -1;
}

//==============================================================================
void PitchEditor::resized()
{
    auto r = getLocalBounds().removeFromTop (headerHeight).reduced (10, 6);
    auto row1 = r.removeFromTop (26);
    quantizeButton.setBounds (row1.removeFromRight (120));
    row1.removeFromRight (6);
    scaleBox.setBounds (row1.removeFromRight (150));
    row1.removeFromRight (4);
    keyBox.setBounds (row1.removeFromRight (64));
    row1.removeFromRight (12);
    detectButton.setBounds (row1.removeFromRight (110));
    title.setBounds (row1);

    r.removeFromTop (6);
    const int w = r.getWidth() / 5;
    for (auto* c : { &fine, &drift, &vibrato, &gain, &formant })
    {
        auto cell = r.removeFromLeft (w).reduced (2, 0);
        c->label.setBounds (cell.removeFromLeft (56));
        c->slider.setBounds (cell.reduced (2, 1));
    }
}

void PitchEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::bar);
    g.fillRect (getLocalBounds().removeFromTop (headerHeight));
    g.setColour (theme::separator);
    g.fillRect (0, headerHeight - 1, getWidth(), 1);

    const auto area = rollArea();
    const auto keys = getLocalBounds().withTrimmedTop (headerHeight).withWidth (keysWidth);

    // Rows: one per semitone, darker for the black keys, names on the Cs and the keys column.
    g.saveState();
    g.reduceClipRegion (area.getUnion (keys));
    for (int pitch = (int) std::floor (lowPitch); pitch <= (int) std::ceil (highPitch); ++pitch)
    {
        const float y1 = yForPitch ((float) pitch + 0.5f), y2 = yForPitch ((float) pitch - 0.5f);
        g.setColour (isBlackKey (pitch) ? theme::lane : theme::laneAlt);
        g.fillRect (juce::Rectangle<float> ((float) area.getX(), y1, (float) area.getWidth(), y2 - y1));
        g.setColour (isBlackKey (pitch) ? juce::Colour (0xff111113) : juce::Colour (0xffd8d8dc));
        g.fillRect (juce::Rectangle<float> ((float) keys.getX(), y1, (float) keys.getWidth() - 1.0f, y2 - y1 - 1.0f));
        if (pitch % 12 == 0 && y2 - y1 > 7.0f)
        {
            g.setColour (theme::gridBar);
            g.fillRect (juce::Rectangle<float> ((float) area.getX(), y2 - 1.0f, (float) area.getWidth(), 1.0f));
            g.setColour (juce::Colour (0xff444448));
            g.setFont (theme::font (juce::jmin (11.0f, y2 - y1)));
            g.drawText (pitchName (pitch), juce::Rectangle<float> ((float) keys.getX() + 2.0f, y1, (float) keysWidth - 6.0f, y2 - y1),
                        juce::Justification::centredRight, false);
        }
    }
    g.restoreState();

    if (! region.isValid())
    {
        g.setColour (theme::dimText);
        g.setFont (theme::font (13.0f));
        g.drawText ("Double-click an audio region (a voice, a bass line, a solo) to edit its notes here.", area,
                    juce::Justification::centred, true);
        return;
    }

    g.saveState();
    g.reduceClipRegion (area);
    const auto colour = arrange::trackColour (region.getParent());

    // Bar lines.
    {
        const auto& grid = studio.project().grid();
        const auto start = (juce::int64) region[ids::start];
        const auto end = start + (juce::int64) region[ids::length];
        int bar = (int) std::floor (grid.barAt (grid.tickAt ((double) start)));
        for (auto s = grid.barStartSample (bar); s <= end; s = grid.barStartSample (++bar))
        {
            g.setColour (theme::gridBar.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (xForTimeline ((double) s), (float) area.getY(), 1.0f, (float) area.getHeight()));
        }
    }

    // The waveform, faint behind everything.
    if (audio != nullptr)
    {
        const auto warp = woodshed::flex::warpOf (region);
        const auto origin = (double) ((juce::int64) region[ids::start] - (juce::int64) region[ids::offset]);
        const float mid = (float) area.getCentreY(), half = (float) area.getHeight() * 0.45f;
        const float* x = audio->getReadPointer (0);
        const int n = audio->getNumSamples();
        g.setColour (colour.withAlpha (0.18f));
        for (int px = area.getX(); px < area.getRight(); ++px)
        {
            const auto t0 = (juce::int64) (region[ids::start]) + (juce::int64) ((double) (px - area.getX()) / area.getWidth() * (double) (juce::int64) region[ids::length]);
            const auto t1 = (juce::int64) (region[ids::start]) + (juce::int64) ((double) (px + 1 - area.getX()) / area.getWidth() * (double) (juce::int64) region[ids::length]);
            const auto s0 = (int) warp.toSrc ((double) t0 - origin), s1 = juce::jmax (s0 + 1, (int) warp.toSrc ((double) t1 - origin));
            float peak = 0.0f;
            for (int i = juce::jmax (0, s0); i < juce::jmin (n, s1); i += juce::jmax (1, (s1 - s0) / 48))
                peak = juce::jmax (peak, std::abs (x[i]));
            const float h = juce::jmin (1.0f, peak * 2.5f) * half;
            g.fillRect ((float) px, mid - h, 1.0f, 2.0f * h);
        }
    }

    // The notes, and the pitch line through each as it will play.
    const auto sr = studio.project().grid().sampleRate;
    for (int i = 0; i < (int) notes.size(); ++i)
    {
        const auto& note = notes[(size_t) i];
        const auto rect = noteRect (note);
        const bool isSelected = std::find (selected.begin(), selected.end(), i) != selected.end();
        g.setColour (colour.withAlpha (isSelected ? 0.85f : 0.5f));
        g.fillRoundedRectangle (rect, juce::jmin (6.0f, rect.getHeight() * 0.5f));
        g.setColour (isSelected ? juce::Colours::white : colour.brighter (0.4f));
        g.drawRoundedRectangle (rect, juce::jmin (6.0f, rect.getHeight() * 0.5f), isSelected ? 1.5f : 1.0f);

        // An edited note shows where it was sung, as an outline.
        if (std::abs (note.shift) > 0.01f)
        {
            auto original = note;
            original.shift = 0.0f;
            g.setColour (juce::Colours::white.withAlpha (0.25f));
            g.drawRoundedRectangle (noteRect (original), 4.0f, 1.0f);
        }

        if (track != nullptr)
        {
            juce::Path line;
            bool started = false;
            const auto stepSamples = juce::jmax (64.0, (note.end - note.start) / juce::jmax (8.0f, rect.getWidth() / 2.0f));
            for (double s = note.start; s < note.end; s += stepSamples)
            {
                const float sung = track->at (s);
                if (sung <= 0.0f) { started = false; continue; }
                const float played = sung + woodshed::flex::transposeAt (notes, *track, s, sr);
                const juce::Point<float> pt (xForTimeline (timelineFor (s)), yForPitch (played));
                if (! started) line.startNewSubPath (pt); else line.lineTo (pt);
                started = true;
            }
            g.setColour (juce::Colours::white.withAlpha (0.8f));
            g.strokePath (line, juce::PathStrokeType (1.3f));
        }

        if (rect.getWidth() > 34.0f && rect.getHeight() > 9.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.setFont (theme::font (juce::jmin (11.0f, rect.getHeight() - 1.0f), theme::Weight::medium));
            const float target = note.pitch + note.shift;
            const int cents = (int) std::lround ((target - std::round (target)) * 100.0f);
            g.drawText (pitchName ((int) std::round (target)) + (cents != 0 ? (cents > 0 ? " +" : " ") + juce::String (cents) : juce::String()),
                        rect.reduced (5.0f, 0.0f), juce::Justification::centredLeft, false);
        }
    }

    // The playhead.
    const float px = xForTimeline ((double) studio.engine().getPosition());
    g.setColour (theme::accent);
    g.fillRect (juce::Rectangle<float> (px - 0.5f, (float) area.getY(), 1.0f, (float) area.getHeight()));
    g.restoreState();
}

//==============================================================================
void PitchEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! rollArea().contains (e.getPosition()) || ! region.isValid())
        return;
    const int hit = noteAt (e.getPosition());
    if (hit < 0)
    {
        if (! e.mods.isShiftDown())
            selected.clear();
        // Click in the keys' row of the roll: move the playhead there.
        updateSliders();
        repaint();
        return;
    }

    const bool already = std::find (selected.begin(), selected.end(), hit) != selected.end();
    if (e.mods.isShiftDown())
    {
        if (already) selected.erase (std::remove (selected.begin(), selected.end(), hit), selected.end());
        else         selected.push_back (hit);
    }
    else if (! already)
    {
        selected = { hit };
    }
    updateSliders();

    dragging = true;
    dragStartY = (float) e.y;
    beforeDrag = notes;
    repaint();
}

void PitchEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;
    const float row = (float) rollArea().getHeight() / (highPitch - lowPitch);
    const float moved = ((float) dragStartY - (float) e.y) / row;
    const bool fineMove = e.mods.isCommandDown();
    notes = beforeDrag;
    for (int i : selected)
    {
        if (! juce::isPositiveAndBelow (i, (int) notes.size()))
            continue;
        auto& n = notes[(size_t) i];
        const float wanted = beforeDrag[(size_t) i].pitch + beforeDrag[(size_t) i].shift + moved;
        // Whole semitones (keeping the note's fine offset), unless Cmd is held.
        const float fineOffset = (beforeDrag[(size_t) i].pitch + beforeDrag[(size_t) i].shift) - std::round (beforeDrag[(size_t) i].pitch + beforeDrag[(size_t) i].shift);
        n.shift = (fineMove ? wanted : std::round (wanted - fineOffset) + fineOffset) - n.pitch;
    }
    repaint();
}

void PitchEditor::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;
    dragging = false;
    bool changed = false;
    for (size_t i = 0; i < notes.size() && i < beforeDrag.size(); ++i)
        changed = changed || std::abs (notes[i].shift - beforeDrag[i].shift) > 1.0e-4f;
    if (changed)
        commit (notes, "Move Notes");
    updateSliders();
    repaint();
}

void PitchEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-click a note: back as it was sung.
    if (const int hit = noteAt (e.getPosition()); hit >= 0)
    {
        auto edited = notes;
        auto& n = edited[(size_t) hit];
        n.shift = 0.0f; n.drift = 1.0f; n.vibrato = 1.0f; n.gain = 0.0f; n.formant = 0.0f;
        commit (edited, "Reset Note");
        updateSliders();
        repaint();
    }
}

} // namespace woodshed::app
