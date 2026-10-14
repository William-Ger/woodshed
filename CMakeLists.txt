cmake_minimum_required(VERSION 3.22)

project(Woodshed VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Windows low-latency audio needs Steinberg's ASIO SDK, which JUCE does not ship.
# Point ASIO_SDK_PATH at the extracted SDK (the folder containing common/iasiodrv.h).
set(ASIO_SDK_PATH "" CACHE PATH "Path to the Steinberg ASIO SDK (Windows only)")

juce_add_plugin(Woodshed
    COMPANY_NAME "Gerstung Works"
    BUNDLE_ID com.gerstungworks.woodshed
    PRODUCT_NAME "Woodshed"
    PLUGIN_MANUFACTURER_CODE Gwks
    PLUGIN_CODE Wdsh
    FORMATS Standalone AU VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Woodshed needs audio input to hear your guitar through your interface."
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(Woodshed PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/StandaloneApp.cpp)

target_compile_definitions(Woodshed PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)

if(WIN32 AND ASIO_SDK_PATH)
    target_include_directories(Woodshed PRIVATE "${ASIO_SDK_PATH}/common")
    target_compile_definitions(Woodshed PUBLIC JUCE_ASIO=1)
endif()

target_link_libraries(Woodshed
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
