#include "WindowStyle.h"

#if ! JUCE_MAC
namespace woodshed::app
{
void useUnifiedTitleBar (juce::Component&) {}
bool hasUnifiedTitleBar (const juce::Component&)                 { return false; }
void startWindowDrag (juce::Component&, const juce::MouseEvent&) {}
}
#endif
