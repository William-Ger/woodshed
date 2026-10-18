#include "Movie.h"
#include "ArrangeView.h"

#if JUCE_MAC || JUCE_WINDOWS
 #include <juce_video/juce_video.h>
 #define WOODSHED_VIDEO 1
#else
 #define WOODSHED_VIDEO 0
#endif

namespace woodshed::app
{

namespace ids = daw::ids;

//==============================================================================
/** The floating movie window: the picture, and below it the timecode at the playhead and where
    the movie starts. */
class MovieController::Window final : public juce::DocumentWindow
{
public:
    explicit Window (MovieController& c)
        : juce::DocumentWindow ("Movie", theme::window, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          content (c)
    {
        setUsingNativeTitleBar (true);
        setContentNonOwned (&content, false);
        setResizable (true, false);
        setResizeLimits (240, 180, 4000, 3000);
        centreWithSize (640, 400);
        setVisible (true);
        content.load();
    }

    ~Window() override
    {
        clearContentComponent();
    }

    void closeButtonPressed() override      { setVisible (false); }

    struct Content final : public juce::Component
    {
        explicit Content (MovieController& c) : controller (c)
        {
           #if WOODSHED_VIDEO
            addAndMakeVisible (video);
            video.onErrorOccurred = [this] (const juce::String& e) { error = e; repaint(); };
           #endif
            startAtPlayhead.setTooltip ("Line the movie's first frame up with the playhead");
            startAtPlayhead.onClick = [this]
            {
                controller.setOffset (controller.studio.engine().getPosition());
            };
            addAndMakeVisible (startAtPlayhead);
        }

        void load()
        {
            error.clear();
           #if WOODSHED_VIDEO
            video.closeVideo();
            if (controller.hasMovie())
            {
                const auto result = video.load (controller.getMovieFile());
                if (result.failed())
                    error = result.getErrorMessage().isNotEmpty() ? result.getErrorMessage() : "Couldn't open the movie";
                video.setAudioVolume (0.0f);    // the sound plays from the song (Import Audio from Movie)
            }
           #endif
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::black);
            auto footer = getLocalBounds().removeFromBottom (footerHeight);
            g.setColour (theme::bar);
            g.fillRect (footer);
            g.setColour (theme::separator);
            g.fillRect (footer.removeFromTop (1));

            auto& engine = controller.studio.engine();
            footer = footer.reduced (12, 0).withTrimmedRight (startAtPlayhead.getWidth() + 12);
            g.setColour (theme::accent);
            g.setFont (theme::readout (16.0f));
            g.drawText (controller.timecodeAt (engine.getPosition()), footer.removeFromLeft (130), juce::Justification::centredLeft, false);
            g.setColour (theme::dimText);
            g.setFont (theme::font (11.0f, theme::Weight::medium));
            const auto& grid = controller.studio.project().grid();
            g.drawText ("Starts at " + grid.toBarsBeats (controller.getOffset()) + "  (" + grid.toClock (controller.getOffset()) + ")",
                        footer, juce::Justification::centredLeft, true);

            auto picture = getLocalBounds().withTrimmedBottom (footerHeight);
            juce::String message = error;
           #if ! WOODSHED_VIDEO
            message = "Video isn't supported on Linux yet";
           #endif
            if (! controller.hasMovie())
                message = "No movie (File > Open Movie...)";
            if (message.isNotEmpty())
            {
                g.setColour (theme::dimText);
                g.setFont (theme::font (14.0f, theme::Weight::medium));
                g.drawFittedText (message, picture.reduced (20), juce::Justification::centred, 3);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds();
            auto footer = r.removeFromBottom (footerHeight);
            startAtPlayhead.setBounds (footer.removeFromRight (150).reduced (8, 7));
           #if WOODSHED_VIDEO
            video.setBounds (r);
           #endif
        }

        void repaintFooter()                { repaint (getLocalBounds().removeFromBottom (footerHeight)); }

        static constexpr int footerHeight = 40;
        MovieController& controller;
       #if WOODSHED_VIDEO
        juce::VideoComponent video { false };
       #endif
        juce::TextButton startAtPlayhead { "Start at Playhead" };
        juce::String error;
    };

    Content content;
};

//==============================================================================
MovieController::MovieController (Studio& s) : studio (s)
{
    studio.addChangeListener (this);
    attach();
    startTimerHz (30);
}

MovieController::~MovieController()
{
    stopTimer();
    studio.removeChangeListener (this);
    attached.removeListener (this);
    window.reset();
}

void MovieController::attach()
{
    if (attached == studio.project().state)
        return;
    attached.removeListener (this);
    attached = studio.project().state;
    attached.addListener (this);
    reload();
}

void MovieController::reload()
{
    const auto path = attached[ids::movie].toString();
    const auto next = path.isNotEmpty() ? studio.project().resolve (path) : juce::File();
    if (next == movie)
        return;

    movie = next;
    info = movie != juce::File() ? video::probe (movie) : video::MovieInfo();
    if (window != nullptr)
    {
        window->content.load();
        if (movie == juce::File())
            window->setVisible (false);
    }
    sendChangeMessage();
}

void MovieController::changeListenerCallback (juce::ChangeBroadcaster*)
{
    attach();       // the project may have been replaced
}

void MovieController::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree != attached)
        return;
    if (property == ids::movie)
        reload();
    else if (property == ids::movieOffset)
        sendChangeMessage();
}

juce::int64 MovieController::getOffset() const
{
    return (juce::int64) attached.getProperty (ids::movieOffset, 0);
}

void MovieController::setOffset (juce::int64 offset, bool newUndoStep)
{
    auto& p = studio.project();
    if (newUndoStep)
        p.undo.beginNewTransaction ("Move Movie");
    attached.setProperty (ids::movieOffset, juce::jmax ((juce::int64) 0, offset), &p.undo);
}

double MovieController::movieTimeAt (juce::int64 timelineSample) const
{
    const double rate = studio.engine().getSampleRate();
    return (double) (timelineSample - getOffset()) / (rate > 0.0 ? rate : 48000.0);
}

juce::String MovieController::timecodeAt (juce::int64 timelineSample) const
{
    return video::MovieSync::timecode (movieTimeAt (timelineSample), info.fps > 0.0 ? info.fps : 30.0);
}

void MovieController::openDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Open a movie", juce::File::getSpecialLocation (juce::File::userMoviesDirectory),
                                                   supportedPatterns);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (! file.existsAsFile())
                                  return;
                              auto& p = studio.project();
                              // Inside the project folder it's stored relative, so the folder can move.
                              const auto path = file.isAChildOf (p.folder) ? file.getRelativePathFrom (p.folder).replaceCharacter ('\\', '/')
                                                                           : file.getFullPathName();
                              p.undo.beginNewTransaction ("Open Movie");
                              attached.setProperty (ids::movie, path, &p.undo);
                              if (! attached.hasProperty (ids::movieOffset))
                                  attached.setProperty (ids::movieOffset, 0, &p.undo);
                              showWindow();
                              studio.flash ("Opened " + file.getFileName()
                                            + (info.hasAudio ? juce::String ("; Import Audio from Movie brings in its sound") : juce::String()));
                          });
}

void MovieController::remove()
{
    if (! hasMovie())
        return;
    auto& p = studio.project();
    p.undo.beginNewTransaction ("Remove Movie");
    attached.removeProperty (ids::movie, &p.undo);
    attached.removeProperty (ids::movieOffset, &p.undo);
}

void MovieController::importAudio()
{
    if (! hasMovie() || importing)
        return;

    auto& p = studio.project();
    p.audioFolder().createDirectory();
    const auto wav = p.audioFolder().getNonexistentChildFile (juce::File::createLegalFileName (movie.getFileNameWithoutExtension()) + " audio", ".wav", true);
    const auto source = movie;
    const double rate = p.grid().sampleRate;
    auto* projectWas = &p;
    importing = true;
    studio.flash ("Importing the movie's sound...");

    juce::WeakReference<MovieController> weak (this);
    juce::Thread::launch ([weak, source, wav, rate, projectWas]
    {
        juce::String error;
        const bool ok = video::extractAudio (source, wav, rate, error);
        juce::MessageManager::callAsync ([weak, ok, error, wav, projectWas]
        {
            if (weak == nullptr)
                return;
            auto& self = *weak;
            self.importing = false;
            if (! ok || projectWas != &self.studio.project())
            {
                self.studio.flash (ok ? juce::String ("The project changed before the movie's sound was in") : error);
                return;
            }

            auto& project = self.studio.project();
            std::unique_ptr<juce::AudioFormatReader> reader (self.studio.engine().getAudioPool().formats.createReaderFor (wav));
            if (reader == nullptr || reader->lengthInSamples <= 0)
            {
                self.studio.flash ("Couldn't read the movie's sound");
                return;
            }
            const auto length = reader->lengthInSamples;
            reader.reset();

            auto track = self.studio.addTrack (false, "Movie Audio");       // one undo step with the region
            project.addRegion (track, wav.getRelativePathFrom (project.folder).replaceCharacter ('\\', '/'), self.getOffset(), 0, length,
                               self.movie.getFileNameWithoutExtension());
            self.studio.flash ("Imported the movie's sound");
        });
    });
}

void MovieController::showWindow()
{
    if (window == nullptr)
        window = std::make_unique<Window> (*this);
    window->setVisible (true);
    window->toFront (true);
}

void MovieController::toggleWindow()
{
    if (isWindowShowing())
        window->setVisible (false);
    else
        showWindow();
}

bool MovieController::isWindowShowing() const
{
    return window != nullptr && window->isVisible();
}

void MovieController::timerCallback()
{
    if (window == nullptr || ! window->isVisible())
        return;
    window->content.repaintFooter();

   #if WOODSHED_VIDEO
    // Keep the picture on what you hear: the playhead less the output latency.
    auto& video = window->content.video;
    if (! video.isVideoOpen())
        return;
    auto& engine = studio.engine();
    juce::int64 heard = engine.getPosition();
    if (engine.isPlaying())
        if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
            heard -= device->getOutputLatencyInSamples();

    video::MovieSync::Player player;
    player.open = true;
    player.playing = video.isPlaying();
    player.position = video.getPlayPosition();
    player.duration = video.getVideoDuration();
    player.speed = lastSpeed;
    const auto action = sync.update (engine.isPlaying(), movieTimeAt (heard), player);
    if (action.seek)
        video.setPlayPosition (action.seekTo);
    if (action.play)
        video.play();
    if (action.stop)
        video.stop();
    if (! juce::approximatelyEqual (action.speed, lastSpeed))
    {
        video.setPlaySpeed (action.speed);
        lastSpeed = action.speed;
    }
   #endif
}

//==============================================================================
/** Makes the strip's thumbnails in the background, evenly over the movie. */
class MovieStrip::Thumbnailer final : public juce::Thread
{
public:
    Thumbnailer (MovieStrip& s, juce::File f, double d, int n)
        : juce::Thread ("Woodshed thumbnails"), strip (s), file (std::move (f)), duration (d), count (n) {}

    ~Thumbnailer() override                 { stopThread (4000); }

    void run() override
    {
        // Coarse first (every 8th), then fill in, so the whole strip shows something early.
        for (int step : { 8, 4, 2, 1 })
            for (int i = 0; i < count && ! threadShouldExit(); i += step)
            {
                {
                    const juce::ScopedLock sl (strip.lock);
                    if (strip.thumbnails[(size_t) i].isValid())
                        continue;
                }
                auto image = video::frameAt (file, (i + 0.5) * duration / count, height - 8);
                if (! image.isValid())
                    continue;
                {
                    const juce::ScopedLock sl (strip.lock);
                    strip.thumbnails[(size_t) i] = image;
                }
                strip.triggerAsyncUpdate();
            }
    }

private:
    MovieStrip& strip;
    const juce::File file;
    const double duration;
    const int count;
};

MovieStrip::MovieStrip (ArrangeView& v, MovieController& m) : view (v), movie (m)
{
    movie.addChangeListener (this);
    restartThumbnails();
    startTimerHz (30);
}

MovieStrip::~MovieStrip()
{
    movie.removeChangeListener (this);
    thumbnailer.reset();
    cancelPendingUpdate();
}

void MovieStrip::restartThumbnails()
{
    thumbnailer.reset();
    thumbnailsFor = movie.getMovieFile();
    {
        const juce::ScopedLock sl (lock);
        thumbnails.clear();
    }
    const auto& info = movie.getInfo();
    if (! movie.hasMovie() || ! video::canMakeThumbnails() || info.duration <= 0.0)
        return;

    constexpr int count = 160;
    {
        const juce::ScopedLock sl (lock);
        thumbnails.resize ((size_t) count);
    }
    thumbnailer = std::make_unique<Thumbnailer> (*this, movie.getMovieFile(), info.duration, count);
    thumbnailer->startThread (juce::Thread::Priority::background);
}

void MovieStrip::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // A new movie (or none): new thumbnails, and the arrange makes room for the strip or takes it back.
    if (movie.getMovieFile() != thumbnailsFor)
        restartThumbnails();
    if (auto* parent = getParentComponent())
        parent->resized();
    repaint();
}

void MovieStrip::timerCallback()
{
    // Follow the arrange view's scroll and zoom, and the playhead for the timecode.
    const auto playhead = view.studio.engine().getPosition();
    if (! juce::approximatelyEqual (lastViewStart, view.getViewStart()) || ! juce::approximatelyEqual (lastSpp, view.getSamplesPerPixel()))
    {
        lastViewStart = view.getViewStart();
        lastSpp = view.getSamplesPerPixel();
        repaint();
    }
    else if (playhead != lastPlayhead)
        repaint();      // the timecode and the playhead line
    lastPlayhead = playhead;
}

void MovieStrip::paint (juce::Graphics& g)
{
    const auto headerW = ArrangeView::headerWidth;
    auto header = getLocalBounds().removeFromLeft (headerW);
    auto lane = getLocalBounds().withTrimmedLeft (headerW);

    g.setColour (theme::panel);
    g.fillRect (header);
    g.setColour (theme::lane);
    g.fillRect (lane);

    // Header: the movie's name and the timecode under the playhead.
    g.setColour (theme::text);
    g.setFont (theme::font (12.0f, theme::Weight::semibold));
    auto text = header.reduced (14, 5);
    g.drawText (movie.getMovieFile().getFileNameWithoutExtension(), text.removeFromTop (17), juce::Justification::centredLeft, true);
    g.setColour (theme::accent);
    g.setFont (theme::readout (11.0f, theme::Weight::medium));
    g.drawText (movie.timecodeAt (view.studio.engine().getPosition()), text, juce::Justification::centredLeft, false);

    // The movie's extent on the timeline.
    const auto& info = movie.getInfo();
    const double rate = view.studio.engine().getSampleRate() > 0.0 ? view.studio.engine().getSampleRate() : 48000.0;
    const auto start = (double) movie.getOffset();
    const auto end = start + juce::jmax (1.0, info.duration) * rate;
    const float x0 = (float) headerW + view.xForSample (start), x1 = (float) headerW + view.xForSample (end);
    auto body = juce::Rectangle<float> (x0, 4.0f, x1 - x0, (float) getHeight() - 8.0f).getIntersection (lane.toFloat());
    if (! body.isEmpty())
    {
        g.saveState();
        g.reduceClipRegion (body.toNearestInt());
        g.setColour (juce::Colour (0xff101012));
        g.fillRect (body);

        juce::ScopedLock sl (lock);
        if (! thumbnails.empty())
        {
            // Tiles a thumbnail wide, each showing the frame nearest its middle.
            const float tileH = body.getHeight();
            const float aspect = info.width > 0 && info.height > 0 ? (float) info.width / (float) info.height : 16.0f / 9.0f;
            const float tileW = juce::jmax (8.0f, tileH * aspect);
            const float first = x0 + std::floor ((body.getX() - x0) / tileW) * tileW;
            for (float x = first; x < body.getRight(); x += tileW)
            {
                const double sample = view.sampleAtX ((double) (x + tileW * 0.5f - (float) headerW));
                const double t = (sample - start) / rate;
                const int index = juce::jlimit (0, (int) thumbnails.size() - 1, (int) (t / info.duration * (double) thumbnails.size()));
                const auto& image = thumbnails[(size_t) index];
                if (image.isValid())
                    g.drawImage (image, juce::Rectangle<float> (x, body.getY(), tileW - 1.0f, tileH), juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
            }
        }
        else
        {
            // No pictures on this system: second ticks and the name.
            g.setColour (theme::gridBar);
            const double secondPx = rate / view.getSamplesPerPixel();
            const double every = secondPx > 40.0 ? 1.0 : secondPx > 8.0 ? 5.0 : 30.0;
            for (double s = 0.0; s < info.duration; s += every)
            {
                const float x = (float) headerW + view.xForSample (start + s * rate);
                if (x >= body.getX() && x <= body.getRight())
                    g.fillRect (x, body.getBottom() - 6.0f, 1.0f, 6.0f);
            }
            g.setColour (theme::dimText);
            g.setFont (theme::font (11.0f, theme::Weight::medium));
            g.drawText (movie.getMovieFile().getFileName(), body.reduced (8.0f, 0.0f).withX (juce::jmax (body.getX(), x0) + 8.0f),
                        juce::Justification::centredLeft, true);
        }
        g.restoreState();
        g.setColour (theme::outline);
        g.drawRect (juce::Rectangle<float> (x0, 4.0f, x1 - x0, (float) getHeight() - 8.0f), 1.0f);
    }

    // The playhead runs through the strip too.
    const float px = (float) headerW + view.xForSample ((double) view.studio.engine().getPosition());
    if (px >= (float) headerW)
    {
        g.setColour (theme::accent);
        g.fillRect (px, 0.0f, 1.0f, (float) getHeight());
    }

    g.setColour (theme::separator);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
    g.fillRect (headerW - 1, 0, 1, getHeight());
}

void MovieStrip::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addItem (movie.isWindowShowing() ? "Hide Movie Window" : "Show Movie Window", [this] { movie.toggleWindow(); });
        m.addItem ("Start Movie at Playhead", [this] { movie.setOffset (view.studio.engine().getPosition()); });
        m.addItem ("Import Audio from Movie", [this] { movie.importAudio(); });
        m.addSeparator();
        m.addItem ("Remove Movie", [this] { movie.remove(); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
        return;
    }
    dragging = e.x > ArrangeView::headerWidth;
    dragStartOffset = movie.getOffset();
    if (dragging)
    {
        view.studio.project().undo.beginNewTransaction ("Move Movie");
        view.setUserGesture (true);
    }
}

void MovieStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;
    // Free by default (a movie's cut rarely sits on a beat); with Shift, to the beat grid.
    auto to = (double) dragStartOffset + e.getDistanceFromDragStartX() * view.getSamplesPerPixel();
    const auto at = e.mods.isShiftDown() ? view.snap (to) : (juce::int64) std::llround (juce::jmax (0.0, to));
    movie.setOffset (at, false);
}

void MovieStrip::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
        view.setUserGesture (false);
    dragging = false;
}

void MovieStrip::mouseDoubleClick (const juce::MouseEvent&)
{
    movie.toggleWindow();
}

void MovieStrip::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    view.handleWheel (e, w, (float) (e.x - ArrangeView::headerWidth));
}

} // namespace woodshed::app
