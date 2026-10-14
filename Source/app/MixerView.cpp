#include "MixerView.h"

namespace woodshed::app
{

MixerView::MixerView (Studio& s) : studio (s)
{
    studio.addChangeListener (this);
}

MixerView::~MixerView()
{
    studio.removeChangeListener (this);
}

void MixerView::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::dimText);
    g.setFont (theme::font (14.0f));
    g.drawText ("MixerView", getLocalBounds(), juce::Justification::centred);
}

void MixerView::resized() {}

void MixerView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();
}

} // namespace woodshed::app
