#pragma once

#include "Studio.h"
#include "Theme.h"
#include "ArrangeWaveforms.h"

namespace woodshed::app
{

class MovieController;
class MovieStrip;

namespace arrange
{
    class TrackHeader;
    class Ruler;
    class Lanes;
}

/** Tracks down the left, the timeline to the right: ruler, regions with waveforms, playhead, loop range. Editing (select, move, trim, split, delete) goes through the project's undo manager. */
class ArrangeView final : public juce::Component,
                          private juce::ChangeListener,
                          private juce::ValueTree::Listener,
                          private juce::ScrollBar::Listener,
                          private juce::Timer
{
public:
    explicit ArrangeView (Studio&);
    ~ArrangeView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** Horizontal zoom, keeping the playhead where it is on screen. */
    void zoomBy (double factor);
    /** Fits the whole song (or 16 bars, if it's empty) in the window. */
    void zoomToFit();

    //==============================================================================
    // Shared by the ruler, the lanes and the track headers. X is in timeline coordinates
    // (0 = the left edge of the lanes), Y in lane coordinates (0 = top of the first lane on screen).
    Studio& studio;
    daw::Project& project() const                           { return studio.project(); }
    arrange::WaveformCache waveforms;

    static constexpr int headerWidth = 200, rulerHeight = 58, trackHeight = 64, scrollBarSize = 10;

    float xForSample (double sample) const                  { return (float) ((sample - viewStart) / samplesPerPixel); }
    double sampleAtX (double x) const                       { return viewStart + x * samplesPerPixel; }
    double getSamplesPerPixel() const                       { return samplesPerPixel; }
    double getViewStart() const                             { return viewStart; }
    double getViewLength() const;

    /** To the nearest beat, unless snapping is off (Cmd held). Never below zero. */
    juce::int64 snap (double sample, bool enabled = true) const;

    /** One line of the timeline grid: where it is and what it marks (bar and beat 1-based). */
    struct GridLine { double sample = 0.0; bool isBar = false, isBeat = false; int bar = 1, beat = 1; };
    /** The grid lines from one sample to another, every `stepBeats` (a fraction of a beat, or a
        whole number of bars' worth of beats), following tempo and meter changes. */
    std::vector<GridLine> gridLines (double fromSample, double toSample, double stepBeats) const;

    /** Grid spacing for the current zoom, in beats. */
    double gridStepBeats() const;
    double labelStepBeats() const;

    /** A track's top. Tracks inside a collapsed Track Stack aren't shown: they sit far above the
        view. An index past the last track gives the bottom of the shown ones. */
    int yForTrack (int index) const;
    /** Track index under a lane y, or -1 below the last track. */
    int trackIndexAtY (int y) const;
    /** How many tracks are shown (not inside a collapsed stack). */
    int visibleRows() const                                 { return (int) trackAtRow.size(); }

    void scrollPixels (double dx, double dy);
    void zoomAround (float x, double factor);
    /** Wheel and pinch behaviour shared by every part of the timeline. */
    void handleWheel (const juce::MouseEvent&, const juce::MouseWheelDetails&, float timelineX);

    /** While the user drags something, the playhead doesn't scroll the view. */
    void setUserGesture (bool active)                       { userGesture = active; }

    /** Header drag-to-reorder: `yInView` is the mouse in ArrangeView coordinates. */
    void dragTrackTo (const juce::ValueTree& track, int yInView);
    void endTrackDrag()                                     { reorderStarted = false; }

    void showTrackMenu (juce::ValueTree track);
    void addTrackAfterSelection();

    /** The movie's thumbnail strip under the ruler, shown while the project has a movie
        (nullptr takes it away; do that before the controller goes). */
    void attachMovie (MovieController*);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void timerCallback() override;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void valueTreeRedirected (juce::ValueTree&) override;

    void attachToProject();
    /** Which track shows on which row, after tracks or stacks change. */
    void updateRows();
    void rebuildHeaders();
    void layoutHeaders();
    void updateScrollBars();
    void contentChanged();
    void setViewStart (double);
    void setSamplesPerPixel (double);
    juce::int64 timelineLength() const;
    int lanesWidth() const;
    int lanesHeight() const;

    juce::ValueTree attached;
    daw::Project* attachedProject = nullptr;

    double viewStart = 0.0, samplesPerPixel = 1000.0;
    int scrollY = 0;
    std::vector<int> rowOfTrack, trackAtRow;        // -1: hidden in a collapsed stack
    bool fitPending = true, keepFitted = true, userGesture = false, reorderStarted = false, wasRecording = false;
    float lastPlayheadX = -1000.0f;
    juce::int64 lastRecordedLength = -1;

    std::unique_ptr<arrange::Ruler> ruler;
    std::unique_ptr<arrange::Lanes> lanes;
    std::unique_ptr<MovieStrip> movieStrip;
    int movieStripHeight() const;
    juce::Component headerArea;
    std::vector<std::unique_ptr<arrange::TrackHeader>> headers;
    /** The + in the header bar: a borderless icon button. */
    class AddButton final : public juce::Button
    {
    public:
        AddButton() : juce::Button ("New track") {}
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    };

    AddButton addButton;
    juce::ScrollBar hScroll { false }, vScroll { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrangeView)
};

} // namespace woodshed::app
