#pragma once

#include "Studio.h"
#include "Theme.h"
#include "ArrangeWaveforms.h"
#include "../loops/LoopGrid.h"

namespace woodshed::app
{

/**
    The Loop Grid pane: a row per track, a column per scene, a cell where they meet. Click a
    cell to launch it on the next launch point (click it again to stop); the square at the end
    of a track's name stops the track, the buttons along the bottom launch whole scenes. On an
    armed track, click an empty cell to record into it (click again to finish on the bar).
    Cells show their audio or notes, and a ring that runs round while they play. Drag a cell to
    another cell (Alt copies) or onto the arrange; drag regions from the arrange onto a cell.
    The record button in the header writes everything the grid plays into the arrange.
*/
class LoopGridView final : public juce::Component,
                           private juce::ChangeListener,
                           private juce::ValueTree::Listener,
                           private Studio::ProjectSwapListener,
                           private juce::Timer
{
public:
    explicit LoopGridView (Studio&);
    ~LoopGridView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** Regions dragged off the arrange and let go at `screenPos`: copied into the cell there.
        False if that isn't over a cell. */
    bool dropRegions (juce::Point<int> screenPos, const juce::Array<int>& regionIds);

    static constexpr int headerHeight = 36, footerHeight = 34, nameWidth = 176, rowHeight = 56;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { repaint(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override             { repaint(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override      { repaint(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override              { repaint(); }
    void valueTreeRedirected (juce::ValueTree&) override                               { repaint(); }
    void projectAboutToBeReplaced() override;
    void timerCallback() override;
    void attach();
    bool engineActive() const;

    daw::Project& project() const                           { return studio.project(); }
    loops::LoopGrid& grid() const                           { return studio.loopGrid(); }

    // Geometry ------------------------------------------------------------------------
    juce::Rectangle<int> bodyArea() const;          // rows and cells, between header and footer
    juce::Rectangle<int> cellsArea() const;         // right of the names
    int cellWidth() const;
    juce::Rectangle<int> cellRect (int row, int column) const;
    juce::Rectangle<int> nameRect (int row) const;
    juce::Rectangle<int> stopRect (int row) const;
    juce::Rectangle<int> sceneRect (int column) const;
    struct Hit { enum Part { none, cell, stop, name, scene } part = none; int row = -1, column = -1; };
    Hit hitAt (juce::Point<int>) const;
    void clampScroll();

    // Drawing ---------------------------------------------------------------------------
    void drawHeader (juce::Graphics&);
    void drawRow (juce::Graphics&, int row);
    void drawCell (juce::Graphics&, const juce::ValueTree& track, int row, int column);
    void drawPreview (juce::Graphics&, const juce::ValueTree& cell, juce::Rectangle<float>, juce::Colour);
    void drawFooter (juce::Graphics&);
    static void drawRing (juce::Graphics&, juce::Rectangle<float>, float progress, juce::Colour, bool pulse);

    // Actions -----------------------------------------------------------------------------
    void clickCell (const juce::ValueTree& track, int column);
    void showCellMenu (const juce::ValueTree& track, int column);
    void showSceneMenu (int column);
    void showQuantizeMenu();
    void renameScene (int column);
    void dropCellOnArrange (const juce::ValueTree& cell, juce::Point<int> screenPos);

    Studio& studio;
    juce::ValueTree watched;
    arrange::WaveformCache waveforms;

    juce::Rectangle<int> quantizeButton, performButton, stopAllButton, addSceneButton;
    int scrollX = 0, scrollY = 0;
    Hit hover;
    int selectedColumn = -1;

    // A cell being dragged.
    juce::ValueTree dragCell;
    bool dragging = false;
    juce::Point<int> dragAt;

    int tick = 0;
    bool wasActive = false;
    std::unique_ptr<juce::TextEditor> nameEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopGridView)
};

} // namespace woodshed::app
