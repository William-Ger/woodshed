#pragma once

#include "Studio.h"
#include "Theme.h"
#include "../video/MovieMedia.h"
#include "../video/MovieSync.h"

namespace woodshed::app
{

class ArrangeView;

/**
    The project's movie: open, remove and import its sound, the floating movie window, and
    keeping the picture on the playhead. The movie is stored in the project (PROJECT movie and
    movieOffset), so it comes back with the song. Playback uses the system's movie player
    (macOS and Windows); on Linux the window says video isn't supported there yet.
*/
class MovieController final : public juce::ChangeBroadcaster,
                              private juce::ChangeListener,
                              private juce::ValueTree::Listener,
                              private juce::Timer
{
public:
    explicit MovieController (Studio&);
    ~MovieController() override;

    void openDialog();
    void remove();
    /** The movie's sound on a new audio track, starting where the movie does. */
    void importAudio();

    bool hasMovie() const                                   { return movie != juce::File(); }
    const juce::File& getMovieFile() const                  { return movie; }
    const video::MovieInfo& getInfo() const                 { return info; }
    /** Timeline sample where the movie's first frame sits. */
    juce::int64 getOffset() const;
    void setOffset (juce::int64, bool newUndoStep = true);
    /** The movie's own time (seconds) at a timeline sample. */
    double movieTimeAt (juce::int64 timelineSample) const;
    /** "HH:MM:SS:FF" at a timeline sample, at the movie's frame rate. */
    juce::String timecodeAt (juce::int64 timelineSample) const;

    void showWindow();
    void toggleWindow();
    bool isWindowShowing() const;

    static constexpr const char* supportedPatterns = "*.mov;*.mp4;*.m4v;*.avi;*.wmv;*.mpg;*.mkv";

private:
    class Window;
    friend class Window;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeRedirected (juce::ValueTree&) override        { reload(); }
    void timerCallback() override;
    void attach();
    void reload();

    Studio& studio;
    juce::ValueTree attached;
    juce::File movie;
    video::MovieInfo info;
    video::MovieSync sync;
    double lastSpeed = 1.0;
    std::unique_ptr<Window> window;
    std::unique_ptr<juce::FileChooser> chooser;
    bool importing = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE (MovieController)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MovieController)
};

//==============================================================================
/**
    The movie's lane at the top of the arrange view: thumbnails along the timeline (where the
    system can make them), the movie's name and the timecode at the playhead in the header.
    Drag it to move the movie's start; double-click for the movie window; right-click for the rest.
*/
class MovieStrip final : public juce::Component,
                         private juce::ChangeListener,
                         private juce::AsyncUpdater,
                         private juce::Timer
{
public:
    MovieStrip (ArrangeView&, MovieController&);
    ~MovieStrip() override;

    static constexpr int height = 44;
    bool isWanted() const                                   { return movie.hasMovie(); }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    class Thumbnailer;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void handleAsyncUpdate() override                       { repaint(); }
    void timerCallback() override;
    void restartThumbnails();

    ArrangeView& view;
    MovieController& movie;
    std::unique_ptr<Thumbnailer> thumbnailer;
    juce::CriticalSection lock;
    std::vector<juce::Image> thumbnails;        // evenly over the movie, filled in as they come
    juce::File thumbnailsFor;
    juce::int64 dragStartOffset = 0;
    bool dragging = false;
    double lastViewStart = -1.0, lastSpp = -1.0;
    juce::int64 lastPlayhead = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MovieStrip)
};

} // namespace woodshed::app
