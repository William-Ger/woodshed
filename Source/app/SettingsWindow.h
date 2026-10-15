#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

/**
    The app's Settings window (app menu > Settings..., Cmd+,): the audio interface's input and
    output devices, rate and buffer, and general preferences, as a Mac preferences window: a
    segmented control in the title bar, then right-aligned form rows. Changes apply and save at once.
*/
class SettingsWindow final : public juce::DocumentWindow
{
public:
    explicit SettingsWindow (Studio&);
    ~SettingsWindow() override;

    void closeButtonPressed() override;

private:
    Studio& studio;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsWindow)
};

} // namespace woodshed::app
