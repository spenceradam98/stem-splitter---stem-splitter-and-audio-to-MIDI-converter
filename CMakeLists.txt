cmake_minimum_required(VERSION 3.22)

# macOS 11 (Big Sur) on Intel is the minimum; build universal (x86_64 + arm64) on CI.
# No -march flags anywhere: the code is scalar C++ (Eigen picks SSE2 / NEON on its own),
# so it runs on any x86-64 without AVX2.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "macOS architectures")
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: no VC++ redist needed
endif()
project(StemScribe VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)

# ---- demucs.cpp (HTDemucs inference, MIT) + Eigen (MPL2), pinned ----
option(STEMSCRIBE_WITH_DEMUCS "Build the HTDemucs separation engine (demucs.cpp)" ON)
if(STEMSCRIBE_WITH_DEMUCS)
    set(DEMUCS_CPP_DIR "" CACHE PATH "Path to a local demucs.cpp checkout with vendor/eigen (optional)")
    if(NOT DEMUCS_CPP_DIR)
        FetchContent_Declare(demucs_cpp
            GIT_REPOSITORY https://github.com/sevagh/demucs.cpp.git
            GIT_TAG 4edae497a114de1ccb17ac9b4a2b8fc0144714d7
            GIT_SUBMODULES vendor/eigen
            SOURCE_SUBDIR do-not-add)   # we build the sources ourselves (their CMake adds -march=native and BLAS)
        FetchContent_MakeAvailable(demucs_cpp)
        set(DEMUCS_CPP_DIR ${demucs_cpp_SOURCE_DIR})
    endif()
    file(GLOB DEMUCS_SOURCES ${DEMUCS_CPP_DIR}/src/*.cpp)
    add_library(demucs_cpp STATIC ${DEMUCS_SOURCES})
    target_include_directories(demucs_cpp SYSTEM PUBLIC ${DEMUCS_CPP_DIR}/src ${DEMUCS_CPP_DIR}/vendor/eigen)
    target_compile_definitions(demucs_cpp PUBLIC EIGEN_MPL2_ONLY _USE_MATH_DEFINES)
    set_target_properties(demucs_cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(demucs_cpp PRIVATE /O2 /bigobj /W0 /fp:fast)
    else()
        # demucs.cpp's own release flags minus -march=native
        target_compile_options(demucs_cpp PRIVATE -O3 -w -fno-math-errno -freciprocal-math -fno-signed-zeros)
    endif()
endif()

# ---- Basic Pitch model (Apache 2.0) compiled into the engine ----
set(BP_MODEL ${CMAKE_CURRENT_SOURCE_DIR}/Source/engine/models/basic_pitch.bpg)
set(BP_CPP ${CMAKE_CURRENT_BINARY_DIR}/BasicPitchModelData.cpp)
if(NOT EXISTS ${BP_CPP} OR ${BP_MODEL} IS_NEWER_THAN ${BP_CPP})
    file(READ ${BP_MODEL} BP_HEX HEX)
    file(SIZE ${BP_MODEL} BP_SIZE)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BP_ARRAY "${BP_HEX}")
    string(REGEX REPLACE "((0x..,){32})" "\\1\n" BP_ARRAY "${BP_ARRAY}")
    file(WRITE ${BP_CPP} "// Generated from Source/engine/models/basic_pitch.bpg (Basic Pitch, Apache 2.0)\n"
        "extern const unsigned char kBasicPitchModel[];\nextern const unsigned long kBasicPitchModelSize;\n"
        "extern const unsigned char kBasicPitchModel[] = {\n${BP_ARRAY}\n};\n"
        "extern const unsigned long kBasicPitchModelSize = ${BP_SIZE}ul;\n")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BP_MODEL})

# ---- Core engine (no JUCE dependency, unit-testable) ----
add_library(stemscribe_engine STATIC
    Source/engine/Audio.cpp
    Source/engine/BasicPitch.cpp
    Source/engine/Dsp.cpp
    Source/engine/DrumTranscriber.cpp
    Source/engine/GraphRunner.cpp
    Source/engine/Harmony.cpp
    Source/engine/MidiFile.cpp
    Source/engine/Model.cpp
    Source/engine/MonoTranscriber.cpp
    Source/engine/Pipeline.cpp
    Source/engine/Separator.cpp
    ${BP_CPP})
target_include_directories(stemscribe_engine PUBLIC Source/engine)
target_compile_definitions(stemscribe_engine PUBLIC _USE_MATH_DEFINES)
set_target_properties(stemscribe_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(stemscribe_engine PUBLIC Threads::Threads)
if(STEMSCRIBE_WITH_DEMUCS)
    target_compile_definitions(stemscribe_engine PRIVATE STEMSCRIBE_WITH_DEMUCS=1)
    target_link_libraries(stemscribe_engine PRIVATE demucs_cpp)
endif()
if(MSVC)
    target_compile_options(stemscribe_engine PRIVATE /O2)
endif()

option(STEMSCRIBE_BUILD_TESTS "Build engine tests and the command-line tool" ON)
if(STEMSCRIBE_BUILD_TESTS)
    add_executable(EngineTests tests/EngineTests.cpp)
    target_link_libraries(EngineTests PRIVATE stemscribe_engine)
    enable_testing()
    add_test(NAME engine COMMAND EngineTests)
    # stemscribe_cli song.wav out_dir [--no-split] [--mode poly|mono|drums] [--model file] [--threads n]
    add_executable(stemscribe_cli tools/StemScribeCli.cpp)
    target_link_libraries(stemscribe_cli PRIVATE stemscribe_engine)
endif()

option(STEMSCRIBE_BUILD_PLUGIN "Build the JUCE plugin" ON)
if(NOT STEMSCRIBE_BUILD_PLUGIN)
    return()
endif()

# ---- JUCE ----
set(JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")
if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# An effect (so it can capture the track it sits on) that also runs as a standalone app.
set(STEMSCRIBE_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND STEMSCRIBE_FORMATS AU)
endif()

juce_add_plugin(StemScribe
    COMPANY_NAME "YourBrand"
    PLUGIN_MANUFACTURER_CODE Ybrd
    PLUGIN_CODE Ssc1
    FORMATS ${STEMSCRIBE_FORMATS}
    PRODUCT_NAME "stem-scribe"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Analyzer
    AU_MAIN_TYPE kAudioUnitType_Effect
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "stem-scribe can capture audio from an input to analyse it.")

set(STEMSCRIBE_PLUGIN_SOURCES
    Source/plugin/PluginProcessor.cpp
    Source/plugin/PluginEditor.cpp
    Source/plugin/RollView.cpp
    Source/plugin/StemList.cpp
    Source/plugin/Themes.cpp)
target_sources(StemScribe PRIVATE ${STEMSCRIBE_PLUGIN_SOURCES})

target_compile_definitions(StemScribe PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_MP3AUDIOFORMAT=1)

target_link_libraries(StemScribe
    PRIVATE
        stemscribe_engine
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---- Optional: render the editor in every theme to PNG (docs screenshots) ----
option(STEMSCRIBE_BUILD_SCREENSHOT "Build the screenshot tool" OFF)
if(STEMSCRIBE_BUILD_SCREENSHOT)
    juce_add_console_app(stemscribe_screenshot PRODUCT_NAME "stemscribe_screenshot")
    target_sources(stemscribe_screenshot PRIVATE tools/Screenshot.cpp ${STEMSCRIBE_PLUGIN_SOURCES})
    target_compile_definitions(stemscribe_screenshot PRIVATE
        JucePlugin_Name="stem-scribe"
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1)
    target_link_libraries(stemscribe_screenshot PRIVATE
        stemscribe_engine
        juce::juce_audio_utils
        juce::juce_recommended_config_flags)
endif()
