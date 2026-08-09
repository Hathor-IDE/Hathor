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
# hathor-b7-k1-filter-tests — B7-K1 per-voice biquad low-pass filter
#
# Tests computeLpCoeffs, biquadProcessSample, and VoicePool end-to-end
# filtering.  JUCE-free: VoicePool.cpp and the header-only BiquadFilter.hpp
# and SampleBank.hpp have no JUCE dependency (SampleBank::find/addTestEntry
# are inline in the header; VoicePool.cpp uses only standard headers).
# ---------------------------------------------------------------------------
add_executable(hathor-b7-k1-filter-tests
    test_b7_k1_filter.cpp
    ${CMAKE_SOURCE_DIR}/app/VoicePool.cpp
)

target_compile_features(hathor-b7-k1-filter-tests PRIVATE cxx_std_20)
target_compile_options(hathor-b7-k1-filter-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-b7-k1-filter-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-b7-k1-filter-tests PRIVATE
    hathor-engine
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(hathor-b7-k1-filter-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# hathor-b7-k2-eq-tests — B7-K2 master-bus preset EQ
#
# Tests preset existence, coefficient generation, Flat neutrality,
# preset differentiation, atomic publication, and audio-thread safety.
# JUCE-free: MasterEq.hpp and BiquadFilter.hpp are header-only with no
# JUCE dependency.
# ---------------------------------------------------------------------------
add_executable(hathor-b7-k2-eq-tests
    test_b7_k2_eq.cpp
)

target_compile_features(hathor-b7-k2-eq-tests PRIVATE cxx_std_20)
target_compile_options(hathor-b7-k2-eq-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-b7-k2-eq-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-b7-k2-eq-tests PRIVATE
    hathor-engine
    Catch2::Catch2WithMain
)

# B7-K3 tests are in the same binary — they verify the preset identifier
# stability and persistence-key mapping that the Settings UI relies on.
# (The UI component itself is JUCE-dependent and tested via integration.)
include(Catch)
catch_discover_tests(hathor-b7-k2-eq-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# hathor-b4-k6-event-queue-tests — B4-K6 timestamped musical event queue
#
# Tests MusicalEvent, SpscEventRing, ClockSync, EventTransport, EventScheduler,
# and EventPublisher.  JUCE-free: all components are header-only or in-process.
# ---------------------------------------------------------------------------
add_executable(hathor-b4-k6-event-queue-tests
    test_b4_k6_timestamped_event_queue.cpp
)

target_compile_features(hathor-b4-k6-event-queue-tests PRIVATE cxx_std_20)
target_compile_options(hathor-b4-k6-event-queue-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-b4-k6-event-queue-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/app/audio-worker
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-b4-k6-event-queue-tests PRIVATE
    hathor-engine
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(hathor-b4-k6-event-queue-tests DISCOVERY_MODE PRE_TEST)

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
    hathor-audio-worker-lib
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
# hathor-b4-k3-vm-isolation-tests — B4-K3 per-tab VM isolation + resource policy
#
# Tests the VM lifecycle state machine, resource ceiling, suspension/resume,
# failure isolation, and worker restart recovery.  Links Catch2 +
# the audio-worker-lib (ChuckVM, VMManager, ResourcePolicy, ChuckCompiler,
# VmLifecycle) + AudioWorkerManager.
# ---------------------------------------------------------------------------
add_executable(hathor-b4-k3-vm-isolation-tests
    test_b4_k3_vm_isolation.cpp
)

target_compile_features(hathor-b4-k3-vm-isolation-tests PRIVATE cxx_std_20)
target_compile_options(hathor-b4-k3-vm-isolation-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-b4-k3-vm-isolation-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app/audio-worker
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-b4-k3-vm-isolation-tests PRIVATE
    hathor-audio-worker-lib
    Catch2::Catch2WithMain
)

target_compile_definitions(hathor-b4-k3-vm-isolation-tests PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
)

include(Catch)
catch_discover_tests(hathor-b4-k3-vm-isolation-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# hathor-b4-k5-watchdog-tests — B4-K5 per-VM hang detection / watchdog
#
# Tests VmWatchdog, ChuckVM heartbeat, VMManager forceDestroyVM, and the
# B4-K5 recovery flow.  Links Catch2 + hathor-audio-worker-lib.
# ---------------------------------------------------------------------------
add_executable(hathor-b4-k5-watchdog-tests
    test_b4_k5_watchdog.cpp
)

target_compile_features(hathor-b4-k5-watchdog-tests PRIVATE cxx_std_20)
target_compile_options(hathor-b4-k5-watchdog-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-b4-k5-watchdog-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app/audio-worker
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-b4-k5-watchdog-tests PRIVATE
    hathor-audio-worker-lib
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(hathor-b4-k5-watchdog-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# hathor-vm-lifecycle-tests — B4-K3/K4 VmLifecycle + ChuckCompiler unit tests
#
# Pure unit tests for the VM lifecycle manager, compile dispatcher serialization,
# and the atomic handoff pattern (no worker process spawn needed).
# Links hathor-audio-worker-lib + Catch2.
# ---------------------------------------------------------------------------
add_executable(hathor-vm-lifecycle-tests
    test_vm_lifecycle.cpp
)

target_compile_features(hathor-vm-lifecycle-tests PRIVATE cxx_std_20)
target_compile_options(hathor-vm-lifecycle-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-vm-lifecycle-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app/audio-worker
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-vm-lifecycle-tests PRIVATE
    hathor-audio-worker-lib
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(hathor-vm-lifecycle-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# hathor-b4-k7-ck-eval-tests — B4-K7 .ck tab evaluation (Ctrl+Enter path)
# ---------------------------------------------------------------------------
add_executable(hathor-b4-k7-ck-eval-tests
    test_b4_k7_ck_eval.cpp
)

target_compile_features(hathor-b4-k7-ck-eval-tests PRIVATE cxx_std_20)
target_compile_options(hathor-b4-k7-ck-eval-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-b4-k7-ck-eval-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app/audio-worker
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-b4-k7-ck-eval-tests PRIVATE
    hathor-audio-worker-lib
    Catch2::Catch2WithMain
)

target_compile_definitions(hathor-b4-k7-ck-eval-tests PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
)

include(Catch)
catch_discover_tests(hathor-b4-k7-ck-eval-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# hathor-b4-k8-hard-gate-tests — B4-K8 hard gate (DoD §6.2)
#
# Tests that a hung/crashing .ck file or killed worker never hangs or crashes
# the main process.  Covers all 3 failure classes: hung shred, native crash,
# shared-memory recovery mid-write.  Spawns the real hathor-audio-worker binary.
# ---------------------------------------------------------------------------
add_executable(hathor-b4-k8-hard-gate-tests
    test_b4_k8_hard_gate.cpp
)

target_compile_features(hathor-b4-k8-hard-gate-tests PRIVATE cxx_std_20)
target_compile_options(hathor-b4-k8-hard-gate-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-b4-k8-hard-gate-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app/audio-worker
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-b4-k8-hard-gate-tests PRIVATE
    hathor-audio-worker-lib
    Catch2::Catch2WithMain
)

target_compile_definitions(hathor-b4-k8-hard-gate-tests PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
)

include(Catch)
catch_discover_tests(hathor-b4-k8-hard-gate-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# hathor-b7-k4-verification — B7-K4 behavioral verification of per-sound
# filtering and master EQ hot-swap.
#
# This is a standalone verification program (not a Catch2 test) that:
#   1. Produces multiple simultaneous voices with different cutoff/resonance
#   2. Verifies per-voice filtering is independent (not global)
#   3. Hot-swaps all 4 EQ presets while audio plays
#   4. Checks for clicks/pops at every transition
#   5. Verifies allocation-free audio path
#   6. Verifies signal-chain ordering (per-voice → mix → ChucK → EQ → gain)
#   7. Writes WAV files for manual/ear verification
#
# JUCE-free: VoicePool.cpp, SampleBank.hpp, BiquadFilter.hpp, MasterEq.hpp
# have no JUCE dependency.
# ---------------------------------------------------------------------------
add_executable(hathor-b7-k4-verification
    test_b7_k4_verification.cpp
    ${CMAKE_SOURCE_DIR}/app/VoicePool.cpp
)

target_compile_features(hathor-b7-k4-verification PRIVATE cxx_std_20)
target_compile_options(hathor-b7-k4-verification PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-b7-k4-verification PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-b7-k4-verification PRIVATE
    hathor-engine
)

# ---------------------------------------------------------------------------
# hathor-b8-k1-asset-target-tests — B8-K1 Studio vs Live Jam asset target
#
# Tests target representation, path resolution (Studio permanent + LiveJam
# session temp), name sanitisation / path-traversal prevention, collision
# handling, Studio persistence across simulated reopen, and LiveJam session
# cleanup isolation (never removes Studio assets).
#
# JUCE-free: AssetTarget.hpp, AssetPathResolver.hpp, and
# LiveJamSessionManager.hpp/.cpp use std::filesystem only.
# ---------------------------------------------------------------------------
add_executable(hathor-b8-k1-asset-target-tests
    test_b8_k1_asset_target.cpp
    ${CMAKE_SOURCE_DIR}/app/LiveJamSessionManager.cpp
)

target_compile_features(hathor-b8-k1-asset-target-tests PRIVATE cxx_std_20)
target_compile_options(hathor-b8-k1-asset-target-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-b8-k1-asset-target-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-b8-k1-asset-target-tests PRIVATE
    hathor-engine
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(hathor-b8-k1-asset-target-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# hathor-b8-k2-render-writer-tests — B8-K2 background ChucK render → .wav
#
# Tests WAV file format correctness, duration/sample count, cancellation,
# state transitions, and error handling.  Uses a null worker (all-silence
# path) to verify the writer produces a valid file regardless.
#
# JUCE-free: ChuckRenderWriter.hpp uses only std::filesystem and standard
# headers.  Links hathor-engine + Catch2.
# ---------------------------------------------------------------------------
add_executable(hathor-b8-k2-render-writer-tests
    test_b8_k2_render_writer.cpp
    ${CMAKE_SOURCE_DIR}/app/ChuckRenderWriter.cpp
)

target_compile_features(hathor-b8-k2-render-writer-tests PRIVATE cxx_std_20)
target_compile_options(hathor-b8-k2-render-writer-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-b8-k2-render-writer-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/app/audio-worker
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-b8-k2-render-writer-tests PRIVATE
    hathor-audio-worker-lib
    hathor-engine
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(hathor-b8-k2-render-writer-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# hathor-b8-k3-vm-shutdown-tests — B8-K3 VM shutdown after successful bake
#
# Tests that ChuckRenderWriter destroys the tab's VM via destroyTabVM on the
# success path (and failure/cancel paths), per B8-K3.  Spawns the real
# hathor-audio-worker binary to verify end-to-end VM lifecycle:
# active → rendering → destroyed.
#
# JUCE-free: links hathor-audio-worker-lib + hathor-engine + Catch2.
# ---------------------------------------------------------------------------
add_executable(hathor-b8-k3-vm-shutdown-tests
    test_b8_k3_vm_shutdown.cpp
    ${CMAKE_SOURCE_DIR}/app/ChuckRenderWriter.cpp
)

target_compile_features(hathor-b8-k3-vm-shutdown-tests PRIVATE cxx_std_20)
target_compile_options(hathor-b8-k3-vm-shutdown-tests PRIVATE -Wall -Wextra -Werror)

target_include_directories(hathor-b8-k3-vm-shutdown-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/app/audio-worker
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(hathor-b8-k3-vm-shutdown-tests PRIVATE
    hathor-audio-worker-lib
    hathor-engine
    Catch2::Catch2WithMain
)

target_compile_definitions(hathor-b8-k3-vm-shutdown-tests PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
)

include(Catch)
catch_discover_tests(hathor-b8-k3-vm-shutdown-tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# Integration tests (require the full hathor binary; skipped if app is off)
# ---------------------------------------------------------------------------
add_subdirectory(integration)
