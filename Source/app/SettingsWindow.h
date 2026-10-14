#pragma once

#include "Studio.h"
#include "Theme.h"

namespace woodshed::app
{

/**
    The app's Settings window (app menu > Settings..., Cmd+,): the audio interface's input and
    output devices, rate and buffer, and general preferences. Changes apply and save at once.
*/
class SettingsWindow final : public juce::DocumentWindow
{
public:
    explicit SettingsWindow (Studio&);
    ~SettingsWindow() override;

    void closeButtonPressed() override;

private:
    Studio& studio;
    juce::LookAndFeel_V4 lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsWindow)
};

} // namespace woodshed::app
