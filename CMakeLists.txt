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

option(WOODSHED_BENCH_ONLY "Only configure the offline pedal bench (fast builds for DSP work)" OFF)

# The DSP engine and every pedal; shared by the app and the offline bench.
set(WOODSHED_ENGINE_SOURCES
    Source/engine/Registry.cpp
    Source/engine/Chain.cpp
    Source/engine/Presets.cpp
    Source/pedals/Dynamics.cpp
    Source/pedals/Drive.cpp
    Source/pedals/AmpCab.cpp
    Source/pedals/Filter.cpp
    Source/pedals/Modulation.cpp
    Source/pedals/Pitch.cpp
    Source/pedals/Delay.cpp
    Source/pedals/Reverb.cpp
    Source/pedals/Glitch.cpp
    Source/pedals/Weird.cpp)

# The multitrack engine (song model, playback, recording, bounce); no UI.
set(WOODSHED_DAW_SOURCES
    Source/daw/Project.cpp
    Source/daw/Engine.cpp)

#==============================================================================
if(NOT WOODSHED_BENCH_ONLY)
    # Rendered UI art (see art/build_assets.py).
    file(GLOB WOODSHED_ART CONFIGURE_DEPENDS art/renders/*.png art/renders/*.jpg art/renders/*.json)
    juce_add_binary_data(WoodshedArt SOURCES ${WOODSHED_ART})

    # Factory presets (presets/*.json; check them with `woodshed-bench presets`).
    file(GLOB WOODSHED_PRESETS CONFIGURE_DEPENDS presets/*.json)
    juce_add_binary_data(WoodshedPresets HEADER_NAME PresetData.h NAMESPACE PresetData SOURCES ${WOODSHED_PRESETS})

    # The guitar rig: pedalboard, amp, cab, presets and tuner. One per track in the studio,
    # and on its own as an AU/VST3 for other hosts.
    set(WOODSHED_RIG_SOURCES
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/TunerPanel.cpp
        Source/Art.cpp)

    # Windows low-latency audio needs Steinberg's ASIO SDK, which JUCE does not ship.
    # Point ASIO_SDK_PATH at the extracted SDK (the folder containing common/iasiodrv.h).
    set(ASIO_SDK_PATH "" CACHE PATH "Path to the Steinberg ASIO SDK (Windows only)")

    #--------------------------------------------------------------------------
    # The studio app (Woodshed.app / Woodshed.exe).
    juce_add_gui_app(WoodshedStudio
        COMPANY_NAME "Gerstung Works"
        BUNDLE_ID com.gerstungworks.woodshed
        PRODUCT_NAME "Woodshed"
        VERSION 0.2.0
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "Woodshed needs audio input to hear and record your guitar through your interface.")

    target_sources(WoodshedStudio PRIVATE
        Source/app/Main.cpp
        Source/app/Studio.cpp
        Source/app/MainComponent.cpp
        Source/app/ControlBar.cpp
        Source/app/ArrangeView.cpp
        Source/app/ArrangeHeader.cpp
        Source/app/ArrangeRuler.cpp
        Source/app/ArrangeLanes.cpp
        Source/app/ArrangeWaveforms.cpp
        Source/app/MixerView.cpp
        Source/app/RigPanel.cpp
        ${WOODSHED_RIG_SOURCES}
        ${WOODSHED_ENGINE_SOURCES}
        ${WOODSHED_DAW_SOURCES})

    target_compile_definitions(WoodshedStudio PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="Woodshed"
        JUCE_APPLICATION_VERSION_STRING="0.2.0")

    if(WIN32 AND ASIO_SDK_PATH)
        target_include_directories(WoodshedStudio PRIVATE "${ASIO_SDK_PATH}/common")
        target_compile_definitions(WoodshedStudio PRIVATE JUCE_ASIO=1)
    endif()

    target_link_libraries(WoodshedStudio
        PRIVATE
            WoodshedArt
            WoodshedPresets
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    #--------------------------------------------------------------------------
    # The rig alone as a plug-in.
    juce_add_plugin(WoodshedRig
        COMPANY_NAME "Gerstung Works"
        BUNDLE_ID com.gerstungworks.woodshed.rig
        PRODUCT_NAME "Woodshed Rig"
        PLUGIN_MANUFACTURER_CODE Gwks
        PLUGIN_CODE Wdsh
        FORMATS AU VST3
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE)

    target_sources(WoodshedRig PRIVATE ${WOODSHED_RIG_SOURCES} ${WOODSHED_ENGINE_SOURCES})
    target_compile_definitions(WoodshedRig PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(WoodshedRig
        PRIVATE
            WoodshedArt
            WoodshedPresets
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    # On macOS, run tools/sign-mac.sh after building the app so microphone permission
    # survives rebuilds (ad-hoc signatures change every build and macOS re-prompts).
endif()

#==============================================================================
# Offline pedal bench: woodshed-bench list | run [type ...] [--render dir]
juce_add_console_app(WoodshedBench PRODUCT_NAME "woodshed-bench")
target_sources(WoodshedBench PRIVATE tools/bench/Bench.cpp tools/bench/ProjectTest.cpp ${WOODSHED_ENGINE_SOURCES} ${WOODSHED_DAW_SOURCES})
target_compile_definitions(WoodshedBench PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(WoodshedBench
    PRIVATE
        juce::juce_dsp
        juce::juce_audio_formats
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_graphics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
