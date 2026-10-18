#include "MasteringPanel.h"

namespace woodshed::app
{

namespace ids = daw::ids;
using namespace mastering;

namespace
{
constexpr int customTarget = 100;           // the target menu's Custom item id
constexpr float spectrumFloor = -90.0f, eqRange = 12.0f;

juce::String lufsText (float v, int places = 1)
{
    return std::isfinite (v) && v > -70.0f ? juce::String (v, places) : juce::String ("-inf");
}

float xForHz (float hz, juce::Rectangle<float> r)
{
    return r.getX() + r.getWidth() * std::log (hz / 20.0f) / std::log (1000.0f);
}
}

MasteringPanel::MasteringPanel (Studio& s) : studio (s)
{
    setOpaque (true);

    // Target: the presets, then Custom.
    for (size_t i = 0; i < targetPresets().size(); ++i)
        target.addItem (targetPresets()[i].name, (int) i + 1);
    target.addItem ("Custom", customTarget);
    target.setSelectedId (1, juce::dontSendNotification);
    target.setTooltip ("The loudness (integrated LUFS) and true-peak ceiling to master to");
    target.onChange = [this]
    {
        resized();
        if (mix != nullptr)
            remaster();
        else if (processor.isValid())
            setKnob (knobs::ceiling, currentTarget().truePeak, "Mastering Ceiling");
    };
    addAndMakeVisible (target);

    for (auto* slider : { &customLufs, &customPeak })
    {
        slider->setSliderStyle (juce::Slider::LinearBar);
        slider->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 0, 0);
        slider->onDragEnd = [this] { if (mix != nullptr) remaster(); };
        addChildComponent (slider);
    }
    customLufs.setRange (-24.0, -6.0, 0.5);
    customLufs.setValue (-14.0, juce::dontSendNotification);
    customLufs.setTextValueSuffix (" LUFS");
    customPeak.setRange (-3.0, 0.0, 0.1);
    customPeak.setValue (-1.0, juce::dontSendNotification);
    customPeak.setTextValueSuffix (" dBTP");

    for (int c = 0; c < characterNames().size(); ++c)
    {
        auto* b = characters.add (new juce::TextButton (characterNames()[c]));
        b->setClickingTogglesState (true);
        b->setRadioGroupId (0x3400);
        b->setConnectedEdges ((c > 0 ? juce::Button::ConnectedOnLeft : 0) | (c < characterNames().size() - 1 ? juce::Button::ConnectedOnRight : 0));
        b->setWantsKeyboardFocus (false);
        b->onClick = [this, c]
        {
            if (! characters[c]->getToggleState())
                return;
            const auto values = daw::Project::decodeValues (processor[ids::knobs].toString());
            if (processor.isValid() && (values.empty() || juce::roundToInt (values[knobs::character]) != c))
                setKnob (knobs::character, (float) c, "Mastering Character");
            if (mix != nullptr)
                remaster();
        };
        addAndMakeVisible (b);
    }
    characters[0]->setToggleState (true, juce::dontSendNotification);

    analyseButton.setTooltip ("Bounce the song, measure it and master it to the target (Mix > Mastering...)");
    analyseButton.onClick = [this] { analyse(); };
    bypass.setTooltip ("Compare with and without mastering (A/B)");
    bypass.setClickingTogglesState (true);
    bypass.onClick = [this]
    {
        if (processor.isValid())
            processor.setProperty (ids::on, ! bypass.getToggleState(), nullptr);    // a comparison, not an edit
    };
    resetMeters.setTooltip ("Start the integrated loudness, range and peak over");
    resetMeters.onClick = [this] { meter.reset(); correlation.reset(); repaint(); };
    for (auto* b : { &analyseButton, &bypass, &resetMeters })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    // The amounts, bound to the processor's knobs.
    const auto* type = Registry::get().find (processorType);
    for (int k : { (int) knobs::gain, (int) knobs::eq, (int) knobs::compress, (int) knobs::width, (int) knobs::monoBass, (int) knobs::ceiling })
    {
        Control c;
        c.knob = k;
        const auto spec = type != nullptr ? type->info.knobs[(size_t) k] : KnobSpec();
        c.name = spec.label;
        c.slider = std::make_unique<juce::Slider> (juce::Slider::LinearBar, juce::Slider::TextBoxLeft);
        c.slider->setRange (spec.min, spec.max, 0.0);
        c.slider->setNormalisableRange ({ spec.min, spec.max, 0.0f, spec.skew });
        c.slider->setDoubleClickReturnValue (true, spec.def);
        c.slider->textFromValueFunction = [spec] (double v) { return spec.format ((float) v); };
        c.slider->setWantsKeyboardFocus (false);
        auto* slider = c.slider.get();
        const auto name = c.name;
        c.slider->onDragStart = [this, name] { studio.project().undo.beginNewTransaction ("Mastering " + name); };
        c.slider->onValueChange = [this, k, slider, name]
        {
            if (! slider->isMouseButtonDown())
                studio.project().undo.beginNewTransaction ("Mastering " + name);
            setKnob (k, (float) slider->getValue(), {});
        };
        // The gain follows the target: after a change that moves the loudness, re-master.
        if (k == knobs::eq || k == knobs::compress || k == knobs::width || k == knobs::monoBass)
            c.slider->onDragEnd = [this] { if (mix != nullptr) remaster(); };
        c.label = std::make_unique<juce::Label> (juce::String(), c.name);
        c.label->setFont (theme::font (12.0f));
        c.label->setColour (juce::Label::textColourId, theme::dimText);
        addAndMakeVisible (*c.slider);
        addAndMakeVisible (*c.label);
        controls.push_back (std::move (c));
    }

    scratchLeft.resize (daw::OutputTap::capacity);
    scratchRight.resize (daw::OutputTap::capacity);

    studio.addChangeListener (this);
    studio.addProjectSwapListener (this);
    refresh();
    startTimerHz (30);
}

MasteringPanel::~MasteringPanel()
{
    if (job != nullptr)
        job->cancelled = true;
    studio.engine().getOutputTap().enabled = false;
    watched.removeListener (this);
    studio.removeProjectSwapListener (this);
    studio.removeChangeListener (this);
}

void MasteringPanel::projectAboutToBeReplaced()
{
    if (job != nullptr)
        job->cancelled = true;
    job.reset();
    mix.reset();
    mixMeasurement.reset();
    status = {};
    watched.removeListener (this);
    watched = {};
    processor = {};
    meter.reset();
}

void MasteringPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void MasteringPanel::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (tree == processor || tree == watched || tree.hasType (ids::MASTER))
        refresh();
}

void MasteringPanel::visibilityChanged()
{
    studio.engine().getOutputTap().enabled = isShowing();
}

//==============================================================================
void MasteringPanel::refresh()
{
    if (watched != studio.project().state)
    {
        watched.removeListener (this);
        watched = studio.project().state;
        watched.addListener (this);
    }
    processor = findProcessor (studio.project());

    const auto values = processor.isValid() ? daw::Project::decodeValues (processor[ids::knobs].toString()) : std::vector<float>();
    for (auto& c : controls)
    {
        c.slider->setEnabled (processor.isValid());
        if (juce::isPositiveAndBelow (c.knob, (int) values.size()) && ! c.slider->isMouseButtonDown())
            c.slider->setValue (values[(size_t) c.knob], juce::dontSendNotification);
    }
    if (processor.isValid() && ! values.empty())
        if (auto* b = characters[juce::jlimit (0, characters.size() - 1, juce::roundToInt (values[0]))])
            b->setToggleState (true, juce::dontSendNotification);

    // The target the processor was mastered to.
    if (processor.isValid() && job == nullptr)
    {
        const auto report = juce::JSON::parse (processor[reportId].toString());
        const auto name = report["target"].toString();
        for (size_t i = 0; i < targetPresets().size(); ++i)
            if (targetPresets()[i].name == name)
                target.setSelectedId ((int) i + 1, juce::dontSendNotification);
        if (name == "Custom")
        {
            target.setSelectedId (customTarget, juce::dontSendNotification);
            customLufs.setValue ((double) report.getProperty ("targetLufs", -14.0), juce::dontSendNotification);
            customPeak.setValue ((double) report.getProperty ("targetPeak", -1.0), juce::dontSendNotification);
        }
    }

    bypass.setEnabled (processor.isValid());
    bypass.setToggleState (processor.isValid() && ! (bool) processor.getProperty (ids::on, true), juce::dontSendNotification);
    analyseButton.setEnabled (! bouncing && job == nullptr);
    resized();
    repaint();
}

Target MasteringPanel::currentTarget() const
{
    if (target.getSelectedId() == customTarget)
        return { "Custom", (float) customLufs.getValue(), (float) customPeak.getValue() };
    return targetPresets()[(size_t) juce::jlimit (0, (int) targetPresets().size() - 1, target.getSelectedId() - 1)];
}

Settings MasteringPanel::currentSettings() const
{
    Settings s;
    s.target = currentTarget();
    for (int c = 0; c < characters.size(); ++c)
        if (characters[c]->getToggleState())
            s.character = (Character) c;

    // The amounts as they stand (the defaults before there's a processor).
    if (processor.isValid())
    {
        const auto values = daw::Project::decodeValues (processor[ids::knobs].toString());
        if (values.size() >= (size_t) knobs::numKnobs)
        {
            s.eqAmount = values[knobs::eq];
            s.compress = values[knobs::compress];
            s.width = values[knobs::width];
            s.monoHz = values[knobs::monoBass];
        }
    }
    return s;
}

void MasteringPanel::setKnob (int knob, float value, const juce::String& undoName)
{
    if (! processor.isValid())
        return;
    auto& p = studio.project();
    if (undoName.isNotEmpty())
        p.undo.beginNewTransaction (undoName);
    auto values = daw::Project::decodeValues (processor[ids::knobs].toString());
    if (! juce::isPositiveAndBelow (knob, (int) values.size()))
        return;
    values[(size_t) knob] = value;
    processor.setProperty (ids::knobs, daw::Project::encodeValues (values), &p.undo);
}

//==============================================================================
void MasteringPanel::analyse()
{
    auto& engine = studio.engine();
    auto& p = studio.project();
    if (bouncing || job != nullptr || engine.isBouncing())
        return;

    const auto end = p.contentEnd();
    if (end <= 0)
    {
        status = "Nothing to master yet: record or write something first";
        repaint();
        return;
    }

    // Bounce the mix without the mastering processor (any other Stereo Out effects stay). The
    // bounce takes its snapshot straight away, so the processor can come back on at once.
    auto fx = findProcessor (p);
    const bool wasOn = fx.isValid() && (bool) fx.getProperty (ids::on, true);
    if (wasOn)
        fx.setProperty (ids::on, false, nullptr);

    const auto masterGain = juce::Decibels::decibelsToGain ((float) (double) p.state.getProperty (ids::masterVolume, 0.0), -100.0f);
    const auto file = juce::File::createTempFile (".wav");
    daw::BounceOptions options;
    options.format = "wav";
    options.bits = 32;
    bouncing = true;
    status = "Bouncing the song...";
    mix.reset();
    mixMeasurement.reset();

    juce::Component::SafePointer<MasteringPanel> safe (this);
    engine.startBounce (file, 0, end, options, [safe, file, masterGain] (bool ok)
    {
        auto buffer = std::make_shared<juce::AudioBuffer<float>>();
        double rate = 48000.0;
        if (ok)
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) })
            {
                rate = reader->sampleRate;
                buffer->setSize (2, (int) reader->lengthInSamples);
                ok = reader->read (buffer.get(), 0, buffer->getNumSamples(), 0, true, true);
                // The Stereo Out fader comes after the processor, so master what feeds it.
                if (masterGain > 0.0f)
                    buffer->applyGain (1.0f / masterGain);
            }
            else
                ok = false;
        }
        file.deleteFile();

        if (safe == nullptr)
            return;
        safe->bouncing = false;
        if (! ok || buffer->getNumSamples() == 0)
        {
            safe->status = "The bounce failed";
            safe->refresh();
            return;
        }
        safe->mix = std::move (buffer);
        safe->mixRate = rate;
        safe->remaster();
    });

    if (wasOn)
        fx.setProperty (ids::on, true, nullptr);
    refresh();
}

void MasteringPanel::remaster()
{
    if (mix == nullptr)
        return;
    if (job != nullptr)
        job->cancelled = true;

    job = std::make_shared<Job>();
    status = "Mastering...";
    auto j = job;
    auto audio = mix;
    auto before = mixMeasurement;
    const auto settings = currentSettings();
    const auto rate = mixRate;
    juce::Component::SafePointer<MasteringPanel> safe (this);

    juce::Thread::launch ([j, audio, before, settings, rate, safe]
    {
        auto mastered = master (*audio, rate, settings, before.get(),
                              [j] (float progress) { j->progress = progress; return ! j->cancelled.load(); });
        if (j->cancelled)
            return;
        juce::MessageManager::callAsync ([safe, j, result = std::move (mastered), t = settings.target]
        {
            if (safe != nullptr && safe->job == j)
            {
                safe->job.reset();
                safe->finished (result, t);
            }
        });
    });
    refresh();
}

void MasteringPanel::finished (const Result& result, const Target& t)
{
    if (mixMeasurement == nullptr && std::isfinite (result.before.integrated))
        mixMeasurement = std::make_shared<const Measurement> (result.before);

    if (! result.ok)
    {
        status = result.message.isNotEmpty() ? result.message : juce::String ("Mastering failed");
        refresh();
        return;
    }

    auto& p = studio.project();
    p.undo.beginNewTransaction ("Mastering");
    applyResult (p, result, t);
    status = result.message.isNotEmpty() ? result.message
                                         : "Mastered to " + t.name + ": " + lufsText (result.after.integrated) + " LUFS, "
                                               + juce::String (result.after.truePeak, 1) + " dBTP";
    refresh();
}

//==============================================================================
void MasteringPanel::drainMeters()
{
    auto& engine = studio.engine();
    if (! juce::approximatelyEqual (meterRate, engine.getSampleRate()))
    {
        meterRate = engine.getSampleRate();
        meter.prepare (meterRate);
        correlation.prepare (meterRate);
        spectrum.prepare (meterRate);
    }

    // A fresh measurement each time playback starts.
    const bool playing = engine.isPlaying();
    if (playing && ! wasPlaying)
    {
        meter.reset();
        correlation.reset();
    }
    wasPlaying = playing;

    auto& tap = engine.getOutputTap();
    for (int n; (n = tap.pull (scratchLeft.data(), scratchRight.data(), (int) scratchLeft.size())) > 0;)
    {
        meter.process (scratchLeft.data(), scratchRight.data(), n);
        correlation.process (scratchLeft.data(), scratchRight.data(), n);
        spectrum.process (scratchLeft.data(), scratchRight.data(), n);
    }
}

void MasteringPanel::timerCallback()
{
    studio.engine().getOutputTap().enabled = isShowing();
    if (! isShowing())
        return;
    drainMeters();
    repaint();
}

//==============================================================================
void MasteringPanel::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (headerHeight).reduced (12, 6);
    header.removeFromLeft (92);         // the title
    target.setBounds (header.removeFromLeft (180));
    header.removeFromLeft (6);
    const bool custom = target.getSelectedId() == customTarget;
    customLufs.setVisible (custom);
    customPeak.setVisible (custom);
    if (custom)
    {
        customLufs.setBounds (header.removeFromLeft (96));
        header.removeFromLeft (4);
        customPeak.setBounds (header.removeFromLeft (86));
        header.removeFromLeft (6);
    }
    header.removeFromLeft (8);
    for (auto* b : characters)
        b->setBounds (header.removeFromLeft (juce::jmax (52, b->getBestWidthForHeight (header.getHeight()) + 8)));
    bypass.setBounds (header.removeFromRight (72));
    header.removeFromRight (6);
    analyseButton.setBounds (header.removeFromRight (84));

    r.reduce (12, 8);
    metersArea = r.removeFromLeft (juce::jmin (230, r.getWidth() / 4));
    resetMeters.setBounds (metersArea.getRight() - 56, metersArea.getY(), 56, 22);
    r.removeFromLeft (12);
    reportArea = r.removeFromRight (juce::jmin (290, r.getWidth() / 3));
    r.removeFromRight (12);
    spectrumArea = r;

    // The amounts sit under the report.
    auto controlsArea = reportArea.withTrimmedTop (juce::jmin (118, reportArea.getHeight() / 2));
    const int rowHeight = juce::jlimit (16, 24, controlsArea.getHeight() / juce::jmax (1, (int) controls.size()));
    for (auto& c : controls)
    {
        auto row = controlsArea.removeFromTop (rowHeight).reduced (0, 2);
        c.label->setBounds (row.removeFromLeft (74));
        c.slider->setBounds (row);
    }
}

void MasteringPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::window);
    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (theme::bar);
    g.fillRect (header);
    g.setColour (theme::highlight);
    g.fillRect (header.withHeight (1));
    g.setColour (theme::separator);
    g.fillRect (header.removeFromBottom (1));
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, theme::Weight::semibold));
    g.drawText ("Mastering", header.reduced (12, 0), juce::Justification::centredLeft);

    paintMeters (g, metersArea);
    paintSpectrum (g, spectrumArea);
    paintReport (g, reportArea);
}

void MasteringPanel::paintMeters (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto r = area;
    g.setColour (theme::dimText);
    g.setFont (theme::font (11.0f, theme::Weight::semibold));
    g.drawText ("LOUDNESS  (EBU R128)", r.removeFromTop (22), juce::Justification::centredLeft);

    // Short-term, big, with a bar against the target.
    const auto t = currentTarget();
    const float shortTerm = meter.shortTerm(), momentary = meter.momentary();
    g.setColour (theme::text);
    g.setFont (theme::readout (26.0f));
    auto big = r.removeFromTop (34);
    g.drawText (lufsText (shortTerm), big, juce::Justification::centredLeft);
    g.setColour (theme::faintText);
    g.setFont (theme::font (11.0f));
    g.drawText ("LUFS short-term", big, juce::Justification::bottomRight);

    auto bar = r.removeFromTop (10).toFloat();
    theme::drawInset (g, bar, 3.0f);
    auto toX = [bar] (float lufs) { return bar.getX() + bar.getWidth() * juce::jlimit (0.0f, 1.0f, (lufs + 40.0f) / 40.0f); };
    if (std::isfinite (momentary))
    {
        g.setColour (momentary > t.lufs + 1.0f ? theme::meterMid : theme::meterLow);
        g.fillRoundedRectangle (bar.withRight (toX (momentary)).reduced (1.0f), 2.0f);
    }
    g.setColour (theme::accent);
    g.fillRect (juce::Rectangle<float> (toX (t.lufs) - 1.0f, bar.getY() - 3.0f, 2.0f, bar.getHeight() + 6.0f));
    r.removeFromTop (8);

    auto row = [&] (const juce::String& name, const juce::String& value, juce::Colour colour)
    {
        auto line = r.removeFromTop (19);
        g.setColour (theme::dimText);
        g.setFont (theme::font (12.0f));
        g.drawText (name, line, juce::Justification::centredLeft);
        g.setColour (colour);
        g.setFont (theme::readout (13.0f));
        g.drawText (value, line, juce::Justification::centredRight);
    };
    row ("Momentary", lufsText (momentary) + " LUFS", theme::text);
    row ("Integrated", lufsText (meter.integrated()) + " LUFS", std::isfinite (meter.integrated()) && std::abs (meter.integrated() - t.lufs) <= 1.0f ? theme::loop : theme::text);
    row ("Range", juce::String (meter.loudnessRange(), 1) + " LU", theme::text);
    const float tp = meter.truePeakDb();
    row ("True peak", (tp > -100.0f ? juce::String (tp, 1) : juce::String ("-inf")) + " dBTP", tp > t.truePeak + 0.05f ? theme::meterHigh : theme::text);

    // Correlation: -1 .. +1.
    r.removeFromTop (6);
    auto corr = r.removeFromTop (19);
    g.setColour (theme::dimText);
    g.setFont (theme::font (12.0f));
    g.drawText ("Correlation", corr.removeFromLeft (80), juce::Justification::centredLeft);
    auto well = corr.reduced (0, 5).toFloat();
    theme::drawInset (g, well, 3.0f);
    const float c = correlation.value();
    const float mid = well.getCentreX(), at = well.getX() + well.getWidth() * (c + 1.0f) * 0.5f;
    g.setColour (c < 0.0f ? theme::meterHigh : theme::meterLow);
    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (mid, at), well.getY() + 1.0f, juce::jmax (mid, at), well.getBottom() - 1.0f));
    g.setColour (theme::faintText);
    g.fillRect (juce::Rectangle<float> (mid - 0.5f, well.getY(), 1.0f, well.getHeight()));

    // What the processor is doing right now.
    if (processor.isValid())
        if (auto pedal = studio.engine().shareInsert (-1, (int) processor[ids::id]))
            if (auto* m = dynamic_cast<MasteringProcessor*> (pedal.get()))
            {
                r.removeFromTop (4);
                row ("Limiter", "-" + juce::String (m->getLimiterReduction(), 1) + " dB", theme::text);
                row ("Compressor", "-" + juce::String (m->getCompressorReduction(), 1) + " dB", theme::text);
            }
}

void MasteringPanel::paintSpectrum (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (area.getWidth() < 60 || area.getHeight() < 40)
        return;
    auto r = area.toFloat();
    theme::drawInset (g, r, theme::radiusPanel);
    auto plot = r.reduced (8.0f, 10.0f).withTrimmedBottom (12.0f);

    // Grid: octaves-ish and 0 dB of EQ.
    g.setFont (theme::font (10.0f));
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = xForHz (hz, plot);
        g.setColour (theme::gridBar);
        g.fillRect (juce::Rectangle<float> (x, plot.getY(), 1.0f, plot.getHeight()));
        g.setColour (theme::faintText);
        g.drawText (hz >= 1000.0f ? juce::String (hz / 1000.0f, 0) + "k" : juce::String (hz, 0), juce::Rectangle<float> (x - 20.0f, plot.getBottom(), 40.0f, 12.0f),
                    juce::Justification::centred);
    }
    const float zeroY = plot.getCentreY();
    g.setColour (theme::gridBar);
    g.fillRect (juce::Rectangle<float> (plot.getX(), zeroY, plot.getWidth(), 1.0f));

    auto spectrumY = [plot] (float db) { return plot.getY() + plot.getHeight() * juce::jlimit (0.0f, 1.0f, db / spectrumFloor); };
    auto eqY = [plot, zeroY] (float db) { return zeroY - plot.getHeight() * 0.5f * juce::jlimit (-1.0f, 1.0f, db / eqRange); };

    // The bounce's spectrum before and after (from the processor's report), faint.
    const auto report = processor.isValid() ? juce::JSON::parse (processor[reportId].toString()) : juce::var();
    auto drawMeasured = [&] (const std::vector<float>& levels, juce::Colour colour)
    {
        if (levels.size() != spectrumFrequencies().size())
            return;
        juce::Path p;
        for (size_t i = 0; i < levels.size(); ++i)
        {
            const juce::Point<float> pt (xForHz (spectrumFrequencies()[i], plot), spectrumY (levels[i]));
            i == 0 ? p.startNewSubPath (pt) : p.lineTo (pt);
        }
        g.setColour (colour);
        g.strokePath (p, juce::PathStrokeType (1.2f));
    };
    drawMeasured (measurementFrom (report["before"]).spectrum, theme::dimText.withAlpha (0.45f));
    drawMeasured (measurementFrom (report["after"]).spectrum, theme::mute.withAlpha (0.55f));

    // Live.
    if (spectrum.hasData())
    {
        juce::Path live;
        const int steps = juce::jmax (32, (int) plot.getWidth() / 3);
        for (int i = 0; i <= steps; ++i)
        {
            const float hz = 20.0f * std::pow (1000.0f, (float) i / (float) steps);
            const juce::Point<float> pt (plot.getX() + plot.getWidth() * (float) i / (float) steps, spectrumY (spectrum.levelAt (hz)));
            i == 0 ? live.startNewSubPath (pt) : live.lineTo (pt);
        }
        g.setColour (theme::text.withAlpha (0.8f));
        g.strokePath (live, juce::PathStrokeType (1.4f));
    }

    // The matching EQ as it's set (curve times amount), on its own +/-12 dB scale.
    if (processor.isValid())
    {
        const auto values = daw::Project::decodeValues (processor[ids::knobs].toString());
        const float amount = values.size() > (size_t) knobs::eq ? values[knobs::eq] : 1.0f;
        auto gains = decodeCurve (processor[ids::state].toString());
        for (auto& v : gains)
            v *= amount;
        juce::Path eq;
        const double rate = studio.engine().getSampleRate();
        const int steps = juce::jmax (32, (int) plot.getWidth() / 3);
        for (int i = 0; i <= steps; ++i)
        {
            const float hz = 20.0f * std::pow (1000.0f, (float) i / (float) steps);
            const juce::Point<float> pt (plot.getX() + plot.getWidth() * (float) i / (float) steps, eqY (eqResponseDb (gains, hz, rate)));
            i == 0 ? eq.startNewSubPath (pt) : eq.lineTo (pt);
        }
        g.setColour (theme::accent);
        g.strokePath (eq, juce::PathStrokeType (2.0f));
    }

    // Legend.
    auto legend = plot.withHeight (14.0f).reduced (4.0f, 0.0f);
    g.setFont (theme::font (10.5f));
    auto key = [&] (const juce::String& name, juce::Colour colour)
    {
        auto item = legend.removeFromLeft (juce::GlyphArrangement::getStringWidth (theme::font (10.5f), name) + 22.0f);
        g.setColour (colour);
        g.fillRect (item.removeFromLeft (12.0f).withSizeKeepingCentre (12.0f, 2.0f));
        g.setColour (theme::dimText);
        g.drawText (name, item.withTrimmedLeft (4.0f), juce::Justification::centredLeft);
    };
    key ("Output", theme::text);
    key ("Mix", theme::dimText);
    key ("Mastered", theme::mute);
    key ("Matching EQ", theme::accent);
}

void MasteringPanel::paintReport (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto r = area.withHeight (juce::jmin (118, area.getHeight() / 2));
    const auto report = processor.isValid() ? juce::JSON::parse (processor[reportId].toString()) : juce::var();
    const auto before = measurementFrom (report["before"]), after = measurementFrom (report["after"]);

    g.setColour (theme::dimText);
    g.setFont (theme::font (11.0f, theme::Weight::semibold));
    auto title = r.removeFromTop (22);
    g.drawText ("ASSISTANT", title, juce::Justification::centredLeft);

    // Progress or the last message.
    juce::String line = status;
    if (bouncing)
        line = "Bouncing the song... " + juce::String (juce::roundToInt (studio.engine().getBounceProgress() * 100.0f)) + "%";
    else if (job != nullptr)
        line = "Mastering... " + juce::String (juce::roundToInt (job->progress.load() * 100.0f)) + "%";
    else if (line.isEmpty())
        line = processor.isValid() ? "Mastered to " + report["target"].toString() : juce::String ("Analyse to master the song");
    g.setColour (theme::text);
    g.setFont (theme::font (12.0f));
    g.drawFittedText (line, r.removeFromTop (18), juce::Justification::centredLeft, 1);

    if (! processor.isValid() || report.isVoid())
        return;

    auto cells = [&] (const juce::String& a, const juce::String& b, const juce::String& c, juce::Colour colour, bool mono)
    {
        auto row = r.removeFromTop (18);
        g.setColour (theme::dimText);
        g.setFont (theme::font (12.0f));
        g.drawText (a, row.removeFromLeft (row.getWidth() / 3), juce::Justification::centredLeft);
        g.setColour (colour);
        g.setFont (mono ? theme::readout (12.5f) : theme::font (11.0f));
        g.drawText (b, row.removeFromLeft (row.getWidth() / 2), juce::Justification::centredRight);
        g.drawText (c, row, juce::Justification::centredRight);
    };
    r.removeFromTop (4);
    cells ({}, "Before", "After", theme::faintText, false);
    cells ("Loudness", lufsText (before.integrated), lufsText (after.integrated) + " LUFS", theme::text, true);
    cells ("True peak", juce::String (before.truePeak, 1), juce::String (after.truePeak, 1) + " dBTP", theme::text, true);
    cells ("Range", juce::String (before.range, 1), juce::String (after.range, 1) + " LU", theme::text, true);
}

} // namespace woodshed::app
