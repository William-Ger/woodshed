#include "WindowStyle.h"
#import <AppKit/AppKit.h>

namespace woodshed::app
{

static NSWindow* nsWindowFor (const juce::Component& c)
{
    if (auto* peer = c.getPeer())
        if (auto* view = (NSView*) peer->getNativeHandle())
            return [view window];
    return nil;
}

void useUnifiedTitleBar (juce::Component& window)
{
    if (NSWindow* w = nsWindowFor (window))
    {
        w.titlebarAppearsTransparent = YES;
        w.titleVisibility = NSWindowTitleHidden;
        w.styleMask |= NSWindowStyleMaskFullSizeContentView;
        w.movableByWindowBackground = NO;
    }
}

bool hasUnifiedTitleBar (const juce::Component& window)
{
    if (NSWindow* w = nsWindowFor (window))
        return (w.styleMask & NSWindowStyleMaskFullSizeContentView) != 0;
    return false;
}

void startWindowDrag (juce::Component& source, const juce::MouseEvent& e)
{
    NSWindow* w = nsWindowFor (source);
    if (w == nil)
        return;

    if (e.getNumberOfClicks() == 2)
    {
        // Follow the user's "double-click a window's title bar to" preference.
        NSString* action = [[NSUserDefaults standardUserDefaults] stringForKey: @"AppleActionOnDoubleClick"];
        if ([action isEqualToString: @"Minimize"]) [w performMiniaturize: nil];
        else if (! [action isEqualToString: @"None"]) [w performZoom: nil];
        return;
    }

    if (NSEvent* event = [NSApp currentEvent])
        if (event.type == NSEventTypeLeftMouseDown)
            [w performWindowDragWithEvent: event];
}

} // namespace woodshed::app
