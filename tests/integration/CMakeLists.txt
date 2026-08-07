# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Integration tests for Hathor Phase 1
#
# These tests require the full "hathor" binary (JUCE + audio app) and are
# therefore registered only when HATHOR_BUILD_APP is ON.
#
# Each test script is registered as a CTest test.  CTest passes the path to
# the hathor binary via a generator expression so the tests always use the
# freshly built executable, regardless of the install prefix.
#
# Usage:
#   cmake --build build --target hathor
#   ctest --test-dir build -R integration -V

if(NOT HATHOR_BUILD_APP)
    message(STATUS "HATHOR_BUILD_APP=OFF — skipping integration tests (require hathor binary)")
    return()
endif()

# Ensure hathor is built before CTest tries to run any integration test.
# This is advisory; the user must still build the target before running ctest.
# We add a custom target that is a no-op but records the dependency.
if(TARGET hathor)
    # Nothing extra needed — $<TARGET_FILE:hathor> already creates the
    # build dependency at test invocation time via CTest properties.
endif()

# Samples directory (committed minimal bank: bd/0.wav, sn/0.wav)
set(HATHOR_SAMPLES_DIR "${CMAKE_SOURCE_DIR}/samples")

# Helper macro: register a shell-script integration test
# Usage: add_integration_test(<name> <script>)
macro(add_integration_test test_name script_name)
    add_test(
        NAME    "integration/${test_name}"
        COMMAND bash
                "${CMAKE_CURRENT_SOURCE_DIR}/${script_name}"
                "$<TARGET_FILE:hathor>"
                "${HATHOR_SAMPLES_DIR}"
    )
    # Give each test a generous timeout (seconds) to account for audio-device
    # initialisation latency in CI.
    set_tests_properties("integration/${test_name}" PROPERTIES
        TIMEOUT 60
        LABELS  "integration"
    )
endmacro()

# ---------------------------------------------------------------------------
# Individual test registrations (15.1, 15.2, 15.3, 20.1/20.2)
# ---------------------------------------------------------------------------
add_integration_test(latency  test_latency.sh)
add_integration_test(startup  test_startup.sh)
add_integration_test(protocol test_protocol.sh)

# Audio quality tests (Req 20.1 / 20.2) — use synthetic sample bank so they
# are self-contained and CI-safe.  Pass an empty SAMPLES_PATH so the script
# generates its own synthetic bank.
add_test(
    NAME    "integration/audio"
    COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/test_audio.sh"
            "$<TARGET_FILE:hathor>"
            # No samples arg — script generates synthetic sine-tone bank
)
set_tests_properties("integration/audio" PROPERTIES
    TIMEOUT 120
    LABELS  "integration"
)

# ---------------------------------------------------------------------------
# Convenience target: run all three via the run_all.sh wrapper
# ---------------------------------------------------------------------------
add_test(
    NAME    "integration/all"
    COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/run_all.sh"
            "$<TARGET_FILE:hathor>"
            "${HATHOR_SAMPLES_DIR}"
)
set_tests_properties("integration/all" PROPERTIES
    TIMEOUT 180
    LABELS  "integration"
)

message(STATUS "Integration tests registered (require hathor binary + audio device)")
