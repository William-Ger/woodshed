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

# The DSP engine, every pedal and every software instrument; shared by the app and the offline bench.
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
    Source/pedals/Weird.cpp
    Source/instruments/InstrumentRegistry.cpp
    Source/instruments/Keyboards.cpp
    Source/instruments/Synths.cpp
    Source/instruments/Drums.cpp
    Source/instruments/Orchestral.cpp
    Source/instruments/Plucked.cpp
    # --- sampler ---
    Source/instruments/Sampler.cpp
    # --- end sampler ---
    )

# The multitrack engine (song model, playback, recording, bounce); no UI.
set(WOODSHED_DAW_SOURCES
    Source/daw/Project.cpp
    Source/daw/Engine.cpp
    Source/daw/MidiFiles.cpp
    Source/daw/Drummer.cpp
    Source/daw/StepPattern.cpp)

# --- mixing (track stacks, groups and VCAs, delay compensation, freeze, punch) ---
list(APPEND WOODSHED_DAW_SOURCES Source/daw/Stacks.cpp Source/daw/Groups.cpp Source/daw/Freeze.cpp)
# --- end mixing ---

# The Score editor's notation layout and drawing (no Component code); shared with the bench.
set(WOODSHED_SCORE_SOURCES
    Source/app/ScoreLayout.cpp
    Source/app/ScoreRender.cpp)

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
        COMPANY_NAME "willgerstung.com"
        COMPANY_WEBSITE "https://willgerstung.com"
        COMPANY_COPYRIGHT "Built by willgerstung.com. Free software under the GNU AGPLv3."
        BUNDLE_ID com.willgerstung.woodshed
        PRODUCT_NAME "Woodshed"
        VERSION 0.3.0
        ICON_BIG art/icon/woodshed-icon.png
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
        Source/app/ChannelStrip.cpp
        Source/app/RigPanel.cpp
        Source/app/SettingsWindow.cpp
        Source/app/Theme.cpp
        Source/app/Icons.cpp
        Source/app/InstrumentPanel.cpp
        Source/app/PianoRoll.cpp
        Source/app/ScoreView.cpp
        ${WOODSHED_SCORE_SOURCES}
        Source/app/DrummerEditor.cpp
        Source/app/StepSequencer.cpp
        Source/app/MusicalTyping.cpp
        Source/app/PluginHost.cpp
        Source/app/TakesEditor.cpp
        # --- sampler ---
        Source/app/SamplerEditor.cpp
        Source/instruments/SamplerTrack.cpp
        # --- end sampler ---
        Source/app/StudioLookAndFeel.cpp
        Source/app/WindowStyle_other.cpp
        ${WOODSHED_RIG_SOURCES}
        ${WOODSHED_ENGINE_SOURCES}
        ${WOODSHED_DAW_SOURCES})

    if(APPLE)
        target_sources(WoodshedStudio PRIVATE Source/app/WindowStyle_mac.mm)
    endif()

    # --- mixing ---
    target_sources(WoodshedStudio PRIVATE Source/app/MixCommands.cpp)
    # --- end mixing ---

    # The studio's typefaces: Inter and JetBrains Mono (both SIL Open Font License; see art/fonts).
    file(GLOB WOODSHED_FONTS CONFIGURE_DEPENDS art/fonts/*.ttf)
    juce_add_binary_data(WoodshedFonts HEADER_NAME FontData.h NAMESPACE FontData SOURCES ${WOODSHED_FONTS})

    target_compile_definitions(WoodshedStudio PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_AU=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_APPLICATION_NAME_STRING="Woodshed"
        JUCE_APPLICATION_VERSION_STRING="0.3.0")

    # macOS ties microphone permission to the code signature, and an ad-hoc signature changes
    # every build, so each rebuild would silently lose audio input. Sign with a stable local
    # identity instead when one is in the keychain (create it in Keychain Access → Certificate
    # Assistant → Create a Certificate, type Code Signing).
    set(WOODSHED_SIGN_IDENTITY "Woodshed Local Signing" CACHE STRING
        "Keychain code-signing identity for Woodshed.app; empty to sign ad hoc")
    if(APPLE AND WOODSHED_SIGN_IDENTITY)
        execute_process(COMMAND security find-identity -p codesigning
                        OUTPUT_VARIABLE WOODSHED_IDENTITIES ERROR_QUIET)
        string(FIND "${WOODSHED_IDENTITIES}" "${WOODSHED_SIGN_IDENTITY}" WOODSHED_IDENTITY_AT)
        if(WOODSHED_IDENTITY_AT GREATER -1)
            set_target_properties(WoodshedStudio PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${WOODSHED_SIGN_IDENTITY}"
                XCODE_ATTRIBUTE_CODE_SIGN_STYLE Manual)
            if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
                add_custom_command(TARGET WoodshedStudio POST_BUILD
                    COMMAND codesign --force --sign "${WOODSHED_SIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:WoodshedStudio>"
                    VERBATIM)
            endif()
        else()
            message(STATUS "Woodshed: no '${WOODSHED_SIGN_IDENTITY}' signing identity; microphone permission will reset on each rebuild")
        endif()
    endif()

    if(WIN32 AND ASIO_SDK_PATH)
        target_include_directories(WoodshedStudio PRIVATE "${ASIO_SDK_PATH}/common")
        target_compile_definitions(WoodshedStudio PRIVATE JUCE_ASIO=1)
    endif()

    target_link_libraries(WoodshedStudio
        PRIVATE
            WoodshedArt
            WoodshedPresets
            WoodshedFonts
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    #--------------------------------------------------------------------------
    # The rig alone as a plug-in.
    juce_add_plugin(WoodshedRig
        COMPANY_NAME "willgerstung.com"
        COMPANY_WEBSITE "https://willgerstung.com"
        COMPANY_COPYRIGHT "Built by willgerstung.com. Free software under the GNU AGPLv3."
        BUNDLE_ID com.willgerstung.woodshed.rig
        PRODUCT_NAME "Woodshed Rig"
        PLUGIN_MANUFACTURER_CODE Wger
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
target_sources(WoodshedBench PRIVATE tools/bench/Bench.cpp tools/bench/ProjectTest.cpp tools/bench/InstrumentTest.cpp tools/bench/ScoreTest.cpp tools/bench/DrummerTest.cpp
    ${WOODSHED_SCORE_SOURCES} ${WOODSHED_ENGINE_SOURCES} ${WOODSHED_DAW_SOURCES})
# --- sampler ---
target_sources(WoodshedBench PRIVATE tools/bench/SamplerTest.cpp Source/instruments/SamplerTrack.cpp)
# --- end sampler ---
# --- mixing ---
target_sources(WoodshedBench PRIVATE tools/bench/MixTest.cpp)
# --- end mixing ---
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
