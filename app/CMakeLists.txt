# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# ---------------------------------------------------------------------------
# hathor-audio-worker — JUCE-free companion process (B4-K2)
#
# This is built unconditionally (independent of HATHOR_BUILD_APP) so the
# AudioWorkerManager can spawn it for testing and production use.
# It does NOT depend on JUCE.
# ---------------------------------------------------------------------------
add_subdirectory(audio-worker)

# Opt-out flag: controlled by the top-level CMakeLists.txt option HATHOR_BUILD_APP.
if(NOT HATHOR_BUILD_APP)
    message(STATUS "HATHOR_BUILD_APP=OFF — skipping app/ (hathor executable)")
    return()
endif()

# ---------------------------------------------------------------------------
# Fetch JUCE (audio framework — only needed for the app target)
# JUCE 8.0.0+ fixes macOS 15 / Xcode 16 compatibility issues present in 7.x
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# hathor — full audio application, links hathor-engine + JUCE + hathor-control
# ---------------------------------------------------------------------------
juce_add_console_app(hathor
    PRODUCT_NAME "Hathor"
    VERSION      "0.1.0"
)

target_sources(hathor
    PRIVATE
        AudioEngine.cpp
        SampleBank.cpp
        VoicePool.cpp
        Main.cpp
        $<TARGET_OBJECTS:hathor-control>
)

target_include_directories(hathor
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/engine/include
        ${CMAKE_SOURCE_DIR}/control
)

target_compile_features(hathor PRIVATE cxx_std_20)

target_link_libraries(hathor
    PRIVATE
        hathor-engine
        hathor-control
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_core
        nlohmann_json::nlohmann_json
)

# JUCE recommends these definitions for console apps
target_compile_definitions(hathor
    PRIVATE
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
)
