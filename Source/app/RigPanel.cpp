#include "RigPanel.h"

namespace woodshed::app
{

RigPanel::RigPanel (Studio& s) : studio (s)
{
    studio.addChangeListener (this);
}

RigPanel::~RigPanel()
{
    studio.removeChangeListener (this);
}

void RigPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::dimText);
    g.setFont (theme::font (14.0f));
    g.drawText ("RigPanel", getLocalBounds(), juce::Justification::centred);
}

void RigPanel::resized() {}

void RigPanel::showTuner() {}

void RigPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();
}

} // namespace woodshed::app
