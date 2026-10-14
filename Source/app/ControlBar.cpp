#include "ControlBar.h"

namespace woodshed::app
{

ControlBar::ControlBar (Studio& s) : studio (s)
{
    studio.addChangeListener (this);
}

ControlBar::~ControlBar()
{
    studio.removeChangeListener (this);
}

void ControlBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::dimText);
    g.setFont (theme::font (14.0f));
    g.drawText ("ControlBar", getLocalBounds(), juce::Justification::centred);
}

void ControlBar::resized() {}

void ControlBar::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();
}

} // namespace woodshed::app
