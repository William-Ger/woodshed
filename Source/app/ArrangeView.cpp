#include "ArrangeView.h"

namespace woodshed::app
{

ArrangeView::ArrangeView (Studio& s) : studio (s)
{
    studio.addChangeListener (this);
}

ArrangeView::~ArrangeView()
{
    studio.removeChangeListener (this);
}

void ArrangeView::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::dimText);
    g.setFont (theme::font (14.0f));
    g.drawText ("ArrangeView", getLocalBounds(), juce::Justification::centred);
}

void ArrangeView::resized() {}

void ArrangeView::zoomBy (double) {}
void ArrangeView::zoomToFit() {}

void ArrangeView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();
}

} // namespace woodshed::app
