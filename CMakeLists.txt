# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------------------
# macOS universal binary support — Phase 6.1
# ---------------------------------------------------------------------------
# Default to a universal binary containing both `arm64` (Apple Silicon) and
# `x86_64` (Intel) architectures on macOS.  This is set as a cache variable
# *before* project() so that CMake's compiler ABI checks and every
# FetchContent dependency (JUCE, libwebp, libchuck) pick it up automatically.
#
# Override on the configure command line when a single-architecture build is
# needed, e.g.:
#   cmake -S . -B build -DCMAKE_OSX_ARCHITECTURES=x86_64
#
# When HATHOR_BUILD_APP=OFF (engine-only / CI), the architecture still applies
# to libchuck and other static libs so that test binaries can be sliced later.
if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING
        "macOS architectures to build (universal binary by default)")
endif()

# ---------------------------------------------------------------------------
# macOS deployment target — Phase 6.2
# ---------------------------------------------------------------------------
# Pin the minimum macOS version explicitly so that the build is reproducible
# across machines with different SDKs.  The binary and Info.plist must agree.
if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING
        "macOS minimum deployment target")
endif()
if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING
        "macOS minimum deployment target" FORCE)
endif()

project(Hathor VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

# Report the effective architecture list for build diagnostics.
if(APPLE)
    message(STATUS "Hathor macOS architectures: ${CMAKE_OSX_ARCHITECTURES}")
endif()

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

# libwebp — WebP decoding for the Petdex sprite pipeline (Phase G / D2).
# Verified decision (docs/design/petdex-d1-d4-decision.md): the live Petdex
# spritesheet assets are WebP and JUCE 8.0.4 ships no WebP decoder, so a
# small BSD-licensed decoder is required. libwebp builds a static `webp`
# library (decode + encode core); all optional tools/extras are disabled.
set(WEBP_BUILD_CWEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX    OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS     OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libwebp
    GIT_REPOSITORY https://github.com/webmproject/libwebp.git
    GIT_TAG        v1.4.0
    GIT_SHALLOW    TRUE
)

# Make Catch2 and nlohmann/json available now (lightweight, no audio deps)
FetchContent_MakeAvailable(Catch2 nlohmann_json libwebp)

# ---------------------------------------------------------------------------
# Universal build fix for libwebp (Phase 6.1)
# ---------------------------------------------------------------------------
# libwebp's cmake/cpu.cmake detects SIMD support via check_c_source_compiles.
# In a universal (arm64;x86_64) build that test compiles for BOTH
# architectures at once; arm64 has no __SSE2__, so the SSE2 test fails and
# libwebp appends -mno-sse2 to CMAKE_C_FLAGS for its scope.
#
# On x86_64, -mno-sse2 disables SSE2, which conflicts with the macOS 15 SDK's
# _Float16 math.h declarations and breaks compilation of libwebp's own
# SSE-dependent DSP sources (e.g. filter_enc.c).
#
# Fix: append -msse2 as a *target* compile option on every libwebp C/C++
# target.  Target COMPILE_OPTIONS are emitted after CMAKE_C_FLAGS in the
# compile command, so -msse2 overrides -mno-sse2 on the x86_64 slice.
# On arm64, -msse2 is an unrelated (harmless) unrecognized-argument warning.
# Only applied when the architecture list contains both arm64 and x86_64.
if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64"
         AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    foreach(_t sharpyuv webpencode webpdecode webpdsp webpdspdecode
              webputils webputilsdecode webp webpdemux libwebpmux
              cpufeatures-webp)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE -msse2)
        endif()
    endforeach()
    message(STATUS "Universal build: restored -msse2 for libwebp x86_64 slice")
endif()

# ---------------------------------------------------------------------------
# AI-4: Download the llm-ls binary for ghost text (only for app builds)
# ---------------------------------------------------------------------------
if(HATHOR_BUILD_APP)
    include(${CMAKE_CURRENT_SOURCE_DIR}/reference/llm-ls/download_llm_ls.cmake
        OPTIONAL RESULT_VARIABLE _llm_ls_downloaded)
endif()

# ---------------------------------------------------------------------------
# libchuck — vendored ChucK compiler/VM (B4-K4, AI-5)
#
# We download the ChucK source tree but do NOT run its CMake; instead we build
# a minimal "vanilla" static libchuck from a curated file list (matching the
# upstream vanilla makefile). This avoids pulling in ChucK's own build system
# and lets us control exactly which sources are compiled.
#
# Requires bison + flex to generate the parser/lexer. If unavailable, we
# gracefully skip libchuck and fall back to placeholder diagnostics.
# ---------------------------------------------------------------------------
find_package(BISON)
find_package(FLEX)

FetchContent_Declare(
    chuck_src
    GIT_REPOSITORY https://github.com/ccrma/chuck.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)

if(BISON_FOUND AND FLEX_FOUND)
    # Populate (download) the chuck source without configuring it
    FetchContent_Populate(chuck_src)
    set(CHUCK_AVAILABLE TRUE CACHE BOOL "libchuck is available" FORCE)
else()
    message(STATUS "Bison/Flex not found — libchuck unavailable. AI-5 tests will use fallback diagnostics.")
    FetchContent_Populate(chuck_src)
    set(CHUCK_AVAILABLE FALSE CACHE BOOL "libchuck is available" FORCE)
endif()

set(CHUCK_CORE_DIR "${chuck_src_SOURCE_DIR}/src/core" CACHE PATH "libchuck core source")

if(CHUCK_AVAILABLE)
    # Generate the parser and lexer (bison + flex)
    add_custom_command(
        OUTPUT ${CHUCK_CORE_DIR}/chuck.tab.c ${CHUCK_CORE_DIR}/chuck.tab.h
        COMMAND ${BISON_EXECUTABLE} -d -o ${CHUCK_CORE_DIR}/chuck.tab.c ${CHUCK_CORE_DIR}/chuck.y
        DEPENDS ${CHUCK_CORE_DIR}/chuck.y
        COMMENT "Generating Chuck parser with bison"
    )

    add_custom_command(
        OUTPUT ${CHUCK_CORE_DIR}/chuck.yy.c
        COMMAND ${FLEX_EXECUTABLE} --nounistd -o ${CHUCK_CORE_DIR}/chuck.yy.c ${CHUCK_CORE_DIR}/chuck.lex
        DEPENDS ${CHUCK_CORE_DIR}/chuck.lex ${CHUCK_CORE_DIR}/chuck.tab.h
        COMMENT "Generating Chuck lexer with flex"
    )

    file(GLOB CHUCK_LO_SRCS ${CHUCK_CORE_DIR}/lo/*.c)

    set(CHUCK_CXX_SRCS
        ${CHUCK_CORE_DIR}/chuck_absyn.cpp
        ${CHUCK_CORE_DIR}/chuck_parse.cpp
        ${CHUCK_CORE_DIR}/chuck_errmsg.cpp
        ${CHUCK_CORE_DIR}/chuck_frame.cpp
        ${CHUCK_CORE_DIR}/chuck_symbol.cpp
        ${CHUCK_CORE_DIR}/chuck_table.cpp
        ${CHUCK_CORE_DIR}/chuck_vm.cpp
        ${CHUCK_CORE_DIR}/chuck_instr.cpp
        ${CHUCK_CORE_DIR}/chuck_scan.cpp
        ${CHUCK_CORE_DIR}/chuck_type.cpp
        ${CHUCK_CORE_DIR}/chuck_emit.cpp
        ${CHUCK_CORE_DIR}/chuck_compile.cpp
        ${CHUCK_CORE_DIR}/chuck_dl.cpp
        ${CHUCK_CORE_DIR}/chuck_oo.cpp
        ${CHUCK_CORE_DIR}/chuck_lang.cpp
        ${CHUCK_CORE_DIR}/chuck_ugen.cpp
        ${CHUCK_CORE_DIR}/chuck_otf.cpp
        ${CHUCK_CORE_DIR}/chuck_stats.cpp
        ${CHUCK_CORE_DIR}/chuck_shell.cpp
        ${CHUCK_CORE_DIR}/chuck_io.cpp
        ${CHUCK_CORE_DIR}/chuck.cpp
        ${CHUCK_CORE_DIR}/chuck_carrier.cpp
        ${CHUCK_CORE_DIR}/chuck_globals.cpp
        ${CHUCK_CORE_DIR}/ugen_osc.cpp
        ${CHUCK_CORE_DIR}/ugen_filter.cpp
        ${CHUCK_CORE_DIR}/ugen_stk.cpp
        ${CHUCK_CORE_DIR}/ugen_xxx.cpp
        ${CHUCK_CORE_DIR}/ulib_ai.cpp
        ${CHUCK_CORE_DIR}/ulib_doc.cpp
        ${CHUCK_CORE_DIR}/ulib_machine.cpp
        ${CHUCK_CORE_DIR}/ulib_math.cpp
        ${CHUCK_CORE_DIR}/ulib_std.cpp
        ${CHUCK_CORE_DIR}/ulib_opsc.cpp
        ${CHUCK_CORE_DIR}/util_buffers.cpp
        ${CHUCK_CORE_DIR}/util_console.cpp
        ${CHUCK_CORE_DIR}/util_math.cpp
        ${CHUCK_CORE_DIR}/util_platforms.cpp
        ${CHUCK_CORE_DIR}/util_string.cpp
        ${CHUCK_CORE_DIR}/util_thread.cpp
        ${CHUCK_CORE_DIR}/util_opsc.cpp
        ${CHUCK_CORE_DIR}/util_serial.cpp
        # NOTE: util_hid.cpp excluded — requires SDL (hidio_sdl.h)
        ${CHUCK_CORE_DIR}/uana_xform.cpp
        ${CHUCK_CORE_DIR}/uana_extract.cpp
    )

    add_library(libchuck STATIC
        ${CHUCK_CORE_DIR}/chuck.tab.c
        ${CHUCK_CORE_DIR}/chuck.yy.c
        ${CHUCK_CORE_DIR}/util_network.c
        ${CHUCK_CORE_DIR}/util_raw.c
        ${CHUCK_CORE_DIR}/util_xforms.c
        ${CHUCK_CXX_SRCS}
        ${CHUCK_LO_SRCS}
    )

    target_include_directories(libchuck PUBLIC
        ${CHUCK_CORE_DIR}
    )
    # lo/ contains config.h which redefines macros that conflict with JUCE's
    # bundled libpng/jpegfl. Keep it PRIVATE so it does not leak into JUCE
    # module compilation (which happens via hathor-ui → hathor-control →
    # hathor-chuck-diagnostics → libchuck).
    target_include_directories(libchuck PRIVATE
        ${CHUCK_CORE_DIR}/lo
    )

    target_compile_features(libchuck PUBLIC cxx_std_14)
    target_compile_definitions(libchuck PUBLIC HAVE_CONFIG_H)

    # HID and MIDI support: libchuck references HID/MIDI APIs that require
    # SDL and RtMidi. We define __DISABLE_HID__ to skip HID class init,
    # __DISABLE_MIDI__ to skip MIDI, and __DISABLE_SERIAL__ to skip SerialIO.
    # __ALTER_HID__ is also needed to skip the HidInManager::cleanup() call in
    # chuck.cpp (chuck.cpp uses __ALTER_HID__, not __DISABLE_HID__, for that call).
    target_compile_definitions(libchuck PUBLIC
        __DISABLE_HID__
        __DISABLE_MIDI__
        __DISABLE_SHELL__
        __DISABLE_SERIAL__
        __ALTER_HID__
    )

    # libchuck has pre-existing warnings (deprecated-copy, etc.) from older C++
    # standards. Disable -Werror for libchuck sources and suppress those
    # warnings for any target that includes libchuck headers.
    if(MSVC)
        target_compile_options(libchuck PRIVATE /W4)
    else()
        target_compile_options(libchuck PRIVATE -Wall -Wextra -Wno-error)
        # Propagate warning suppression to consumers via INTERFACE so that
        # files including chuck.h don't trigger -Werror failures.
        target_compile_options(libchuck INTERFACE
            -Wno-deprecated-copy
            -Wno-deprecated-declarations
        )
    endif()

    if(APPLE)
        target_link_libraries(libchuck PUBLIC
            "-framework Foundation"
            "-framework CoreFoundation"
            "-framework CoreMIDI"
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework IOKit"
            "-framework ApplicationServices"
        )
        target_link_options(libchuck PUBLIC "-Wl,-undefined,dynamic_lookup")
    endif()

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SNDFILE IMPORTED_TARGET sndfile)
        if(SNDFILE_FOUND)
            target_link_libraries(libchuck PUBLIC PkgConfig::SNDFILE)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Subdirectories (build order: engine → control → app → tests)
# ---------------------------------------------------------------------------
add_subdirectory(engine)
add_subdirectory(control)
add_subdirectory(app)
add_subdirectory(ui)
add_subdirectory(tests)
add_subdirectory(tests-ui)
