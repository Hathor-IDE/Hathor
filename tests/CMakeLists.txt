# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# hathor-engine-tests — unit tests for the pure engine, NO JUCE dependency
add_executable(hathor-engine-tests
    test_rational.cpp
    test_parammap.cpp
    test_pattern.cpp
    test_combinators.cpp
    test_miniparser.cpp
    test_prettyprinter.cpp
    test_strudel_differential.cpp
    test_event_metadata.cpp
    golden/FixtureLoader.cpp
)

target_link_libraries(hathor-engine-tests
    PRIVATE
        hathor-engine
        Catch2::Catch2WithMain
        nlohmann_json::nlohmann_json
)

target_compile_features(hathor-engine-tests PRIVATE cxx_std_20)

# Add golden/ as an include directory for FixtureLoader.hpp.
target_include_directories(hathor-engine-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/golden
)

# Pass the golden-fixture directory path as a compile definition so the test
# can locate reference/strudel-golden/ regardless of working-directory.
target_compile_definitions(hathor-engine-tests PRIVATE
    GOLDEN_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/reference/strudel-golden"
)

# Ensure -Wall -Wextra -Werror applies to the golden test sources too.
if(MSVC)
    target_compile_options(hathor-engine-tests PRIVATE /W4 /WX)
else()
    target_compile_options(hathor-engine-tests PRIVATE -Wall -Wextra -Werror)
endif()

# Register tests with CTest.
# DISCOVERY_MODE PRE_TEST avoids the bracket-escaping problem that occurs
# when test names contain "[" or "]" characters (e.g. "query over [0,1)").
# With POST_BUILD (the default) Catch2 escapes those names and CTest then
# fails to match them back. PRE_TEST discovers tests at test-run time,
# which never has this problem.
include(CTest)
include(Catch)
catch_discover_tests(hathor-engine-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# hathor-control-tests — control + MCP socket accept-loop tests (Phase 2.5 H0)
#
# Links hathor-control (ControlInterface + WorkerThread + SocketServer) and the
# engine.  JUCE-free: a fake AudioEngineFacade + an idle SampleBank stand in for
# the real app, so no audio device is required.
# ---------------------------------------------------------------------------
add_executable(hathor-control-tests
    test_mcp_socket.cpp
    test_control_slots.cpp
)

target_compile_features(hathor-control-tests PRIVATE cxx_std_20)
target_compile_options(hathor-control-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-control-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/control
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-control-tests PRIVATE
    hathor-control
    hathor-engine
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
)

include(Catch)
catch_discover_tests(hathor-control-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# hathor-audio-transport-tests — lock-free SPSC sample ring (B4-K1)
#
# Links no JUCE dependency — the ring is header-only and lives in app/.
# ---------------------------------------------------------------------------
add_executable(hathor-audio-transport-tests
    test_spsc_sample_ring.cpp
)

target_compile_features(hathor-audio-transport-tests PRIVATE cxx_std_20)
target_compile_options(hathor-audio-transport-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-audio-transport-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-audio-transport-tests PRIVATE
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(hathor-audio-transport-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# hathor-audio-worker-tests — B4-K2 worker lifecycle, generation, RT-safe audio
#
# These tests spawn the real hathor-audio-worker executable built by
# app/audio-worker/CMakeLists.txt.  JUCE-free: the manager is a pure POSIX
# process lifecycle wrapper.  Tests link Catch2 + AudioWorkerManager only.
# ---------------------------------------------------------------------------
add_executable(hathor-audio-worker-tests
    test_audio_worker_manager.cpp
)

target_compile_features(hathor-audio-worker-tests PRIVATE cxx_std_20)
target_compile_options(hathor-audio-worker-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-audio-worker-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app/audio-worker
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-audio-worker-tests PRIVATE
    Catch2::Catch2WithMain
)

# The test needs to locate the hathor-audio-worker binary at runtime.
# Pass the build directory as a compile definition.
target_compile_definitions(hathor-audio-worker-tests PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
)

include(Catch)
catch_discover_tests(hathor-audio-worker-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# Integration tests (require the full hathor binary; skipped if app is off)
# ---------------------------------------------------------------------------
add_subdirectory(integration)
