#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace woodshed::app
{
/** On the Mac, extends the window's content under a transparent title bar so the toolbar and the
    window buttons share one strip, like modern Mac apps. The content must keep
    theme::trafficLightInset clear at the top left and drag the window from empty toolbar space
    (see startWindowDrag). Does nothing elsewhere. */
void useUnifiedTitleBar (juce::Component& window);

/** True when useUnifiedTitleBar took effect (so the toolbar should reserve the inset). */
bool hasUnifiedTitleBar (const juce::Component& window);

/** Starts a native window drag from a mouse-down in empty toolbar space; on double-click, zooms. */
void startWindowDrag (juce::Component& source, const juce::MouseEvent&);
}
