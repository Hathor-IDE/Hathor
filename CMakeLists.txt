# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.24)
project(Hathor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable CTest support
enable_testing()

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------
# Set HATHOR_BUILD_APP=OFF to build only the engine and tests (no JUCE needed).
# Useful for CI environments without audio hardware or a full macOS SDK.
option(HATHOR_BUILD_APP "Build the full JUCE audio application (hathor)" ON)

# ---------------------------------------------------------------------------
# FetchContent dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

# NOTE: JUCE is declared here for documentation purposes but fetched inside
# app/CMakeLists.txt so that builds targeting only hathor-engine-tests do not
# need to compile JUCE at all. This keeps CI fast on environments without
# audio hardware or a full macOS SDK.

# Catch2 — unit-test framework (used by tests/ only, no JUCE)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
    GIT_SHALLOW    TRUE
)

# nlohmann/json — JSON library (used by control/ and app/)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# Make Catch2 and nlohmann/json available now (lightweight, no audio deps)
FetchContent_MakeAvailable(Catch2 nlohmann_json)

# ---------------------------------------------------------------------------
# Subdirectories (build order: engine → control → app → tests)
# ---------------------------------------------------------------------------
add_subdirectory(engine)
add_subdirectory(control)
add_subdirectory(app)
add_subdirectory(tests)
