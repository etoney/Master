cmake_minimum_required(VERSION 3.22)

# Must come BEFORE project(), or CMake ignores them: universal (Apple Silicon + Intel), macOS 11+
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
project(THEMASTER VERSION 1.13.8)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Same JUCE version as Dominatrix, Chop Shop, DREAD, Possessed and BuzzKill.
# (Pass -DJUCE_LOCAL_DIR=/path/to/JUCE to build against a local checkout.)
if(JUCE_LOCAL_DIR)
    add_subdirectory(${JUCE_LOCAL_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# The factory genre profiles, compiled in. Update Profiles/factory.json to ship new ones.
juce_add_binary_data(TheMasterData SOURCES Profiles/factory.json)

if(NOT THEMASTER_TESTS_ONLY)
juce_add_plugin(TheMaster
    COMPANY_NAME "Castle Danger"
    PRODUCT_NAME "The Master"
    BUNDLE_ID com.castledanger.themaster
    PLUGIN_MANUFACTURER_CODE Cdgr
    PLUGIN_CODE Mstr
    FORMATS VST3 AU
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Mastering Analyzer
    AU_MAIN_TYPE kAudioUnitType_Effect)

target_sources(TheMaster PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(TheMaster PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=1)

target_link_libraries(TheMaster
    PRIVATE
        TheMasterData
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
endif()

# Admin tool: builds genre profile packs from folders of reference songs (see ADMIN.txt)
juce_add_console_app(TheMasterProfiler PRODUCT_NAME "TheMasterProfiler")
target_sources(TheMasterProfiler PRIVATE Tools/Profiler.cpp)
target_compile_definitions(TheMasterProfiler PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=1)
target_link_libraries(TheMasterProfiler PRIVATE TheMasterData juce::juce_audio_formats juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)

# Headless checks: analyser accuracy, profiles, advice, processor, and renders the editor to PNGs
juce_add_console_app(TheMasterSnapshot PRODUCT_NAME "TheMasterSnapshot")
target_sources(TheMasterSnapshot PRIVATE test/snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_compile_definitions(TheMasterSnapshot PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=1
    "JucePlugin_VersionString=\"${PROJECT_VERSION}\"")
target_link_libraries(TheMasterSnapshot PRIVATE TheMasterData juce::juce_audio_processors juce::juce_audio_formats juce::juce_gui_basics juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
