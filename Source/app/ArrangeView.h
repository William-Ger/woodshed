#pragma once

#include "Studio.h"
#include "Theme.h"
#include "ArrangeWaveforms.h"

namespace woodshed::app
{

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

    /** Grid spacing for the current zoom, in beats. */
    double gridStepBeats() const;
    double labelStepBeats() const;

    int yForTrack (int index) const                         { return index * trackHeight - scrollY; }
    /** Track index under a lane y, or -1 below the last track. */
    int trackIndexAtY (int y) const;

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
    bool fitPending = true, keepFitted = true, userGesture = false, reorderStarted = false, wasRecording = false;
    float lastPlayheadX = -1000.0f;
    juce::int64 lastRecordedLength = -1;

    std::unique_ptr<arrange::Ruler> ruler;
    std::unique_ptr<arrange::Lanes> lanes;
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
