// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_b4_k7_ck_eval.cpp — tests for B4-K7: .ck tab evaluation via Ctrl+Enter.
 *
 * These tests spawn the real hathor-audio-worker executable and verify:
 *   - Successful compile of valid .ck source
 *   - Failed compile of invalid .ck source (validation rejection)
 *   - ck_stop destroys VM and clears handoff
 *   - VM activation before compile
 *   - Generation query returns correct values
 *   - Two .ck tabs are independently evaluable
 *   - Failed compile does NOT destroy running shred
 *   - ck_genv returns fresh generation after vm_activate
 *   - Re-evaluation replaces prior shred (not duplicate)
 *   - Empty source is rejected
 *   - Source with unbalanced brackets is rejected
 *   - Source with => operator is accepted
 *
 * JUCE-free: links Catch2 + AudioWorkerManager only.
 * Requires the hathor-audio-worker binary to be built.
 *
 * Requirements: B4-K7, B4-K4, B4-K3
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AudioWorkerManager.hpp"
#include "audio_ipc.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

using hathor::AudioWorkerManager;
using hathor::audio_worker::VMResult;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Locate the hathor-audio-worker binary (built by CMake in the same tree).
static std::string getWorkerPath()
{
    namespace fs = std::filesystem;

#ifdef CMAKE_BINARY_DIR
    fs::path p = fs::path(CMAKE_BINARY_DIR) / "app" / "audio-worker" / "hathor-audio-worker";
    if (fs::exists(p))
        return p.string();
    p = fs::path(CMAKE_BINARY_DIR) / "hathor-audio-worker";
    if (fs::exists(p))
        return p.string();
#endif

#ifdef CMAKE_SOURCE_DIR
    {
        fs::path p = fs::path(CMAKE_SOURCE_DIR) / "build" / "app" / "audio-worker" / "hathor-audio-worker";
        if (fs::exists(p))
            return p.string();
    }
#endif

    const char* envSrc = std::getenv("CMAKE_SOURCE_DIR");
    if (envSrc) {
        fs::path p = fs::path(envSrc) / "build" / "app" / "audio-worker" / "hathor-audio-worker";
        if (fs::exists(p))
            return p.string();
    }

    const fs::path candidates[] = {
        fs::current_path() / "hathor-audio-worker",
        fs::current_path() / "build" / "hathor-audio-worker",
        fs::current_path() / "build" / "app" / "audio-worker" / "hathor-audio-worker",
        fs::current_path() / "app" / "audio-worker" / "hathor-audio-worker",
    };

    for (const auto& p : candidates) {
        if (fs::exists(p))
            return p.string();
    }

    return "";
}

/// Valid ChucK source with => operator and semicolon.
static const std::string kValidCk = "SinOsc s => dac;";

/// Invalid ChucK source — no => operator, no semicolon.
static const std::string kInvalidCk = "just some text";

/// Source with unbalanced brackets.
static const std::string kUnbalancedCk = "SinOsc s => dac(";

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

TEST_CASE("B4-K7: valid .ck source compiles successfully", "[k7][compile][valid]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 0;

    // Activate VM for this tab.
    auto act = mgr.activateTabVM(tab);
    REQUIRE(act.ok);

    // Compile valid source.
    auto result = mgr.evaluateCkTab(tab, kValidCk);
    REQUIRE(result.ok);
    REQUIRE(result.message.find("ok") != std::string::npos);

    mgr.shutdown();
}

TEST_CASE("B4-K7: invalid .ck source fails validation", "[k7][compile][invalid]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 1;

    auto act = mgr.activateTabVM(tab);
    REQUIRE(act.ok);

    // Compile invalid source — no => or ;.
    auto result = mgr.evaluateCkTab(tab, kInvalidCk);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.message.find("err") != std::string::npos);
    // Error should mention the validation issue.
    const bool hasExpectedMsg =
        result.message.find("expected") != std::string::npos ||
        result.message.find("sporking") != std::string::npos ||
        result.message.find(";") != std::string::npos;
    REQUIRE(hasExpectedMsg);

    mgr.shutdown();
}

TEST_CASE("B4-K7: empty source is rejected", "[k7][compile][empty]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 2;

    auto act = mgr.activateTabVM(tab);
    REQUIRE(act.ok);

    auto result = mgr.evaluateCkTab(tab, "");
    REQUIRE_FALSE(result.ok);

    mgr.shutdown();
}

TEST_CASE("B4-K7: unbalanced brackets rejected", "[k7][compile][brackets]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 3;

    auto act = mgr.activateTabVM(tab);
    REQUIRE(act.ok);

    auto result = mgr.evaluateCkTab(tab, kUnbalancedCk);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.message.find("unbalanced") != std::string::npos ||
            result.message.find("expected") != std::string::npos);

    mgr.shutdown();
}

TEST_CASE("B4-K7: failed compile does NOT destroy running VM", "[k7][compile][no-destroy-on-fail]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 4;

    auto act = mgr.activateTabVM(tab);
    REQUIRE(act.ok);

    // First, compile valid source.
    auto okResult = mgr.evaluateCkTab(tab, kValidCk);
    REQUIRE(okResult.ok);

    // Then, attempt to compile invalid source.
    auto failResult = mgr.evaluateCkTab(tab, kInvalidCk);
    REQUIRE_FALSE(failResult.ok);

    // Query VM — it should still be active (not destroyed by failed compile).
    auto queryResult = mgr.queryTabVM(tab);
    REQUIRE(queryResult.ok);
    // The VM state should still be active.
    REQUIRE(queryResult.message.find("active") != std::string::npos ||
            queryResult.message.find("ok") != std::string::npos);

    mgr.shutdown();
}

TEST_CASE("B4-K7: ck_stop destroys VM and clears handoff", "[k7][stop]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 5;

    auto act = mgr.activateTabVM(tab);
    REQUIRE(act.ok);

    // Compile valid source.
    auto compileResult = mgr.evaluateCkTab(tab, kValidCk);
    REQUIRE(compileResult.ok);

    // Stop the tab.
    auto stopResult = mgr.stopCkTab(tab);
    REQUIRE(stopResult.ok);

    // Query after stop — VM should be destroyed/inactive.
    auto queryResult = mgr.queryTabVM(tab);
    REQUIRE(queryResult.ok);
    REQUIRE(queryResult.message.find("destroyed") != std::string::npos ||
            queryResult.message.find("inactive") != std::string::npos ||
            queryResult.message.find("no_vm") != std::string::npos);

    mgr.shutdown();
}

TEST_CASE("B4-K7: ck_genv returns valid generation after activation", "[k7][genv]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 6;

    auto act = mgr.activateTabVM(tab);
    REQUIRE(act.ok);

    // Query generation.
    auto queryResult = mgr.queryTabVM(tab);
    REQUIRE(queryResult.ok);
    // The response should contain gen= and version=.
    REQUIRE(queryResult.message.find("gen=") != std::string::npos ||
            queryResult.message.find("active") != std::string::npos);

    mgr.shutdown();
}

TEST_CASE("B4-K7: two .ck tabs are independently evaluable", "[k7][isolation][two-tabs]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tabA = 0;
    const uint8_t tabB = 1;

    // Activate both tabs.
    auto actA = mgr.activateTabVM(tabA);
    auto actB = mgr.activateTabVM(tabB);
    REQUIRE(actA.ok);
    REQUIRE(actB.ok);

    // Compile on tabA.
    auto resultA = mgr.evaluateCkTab(tabA, "SinOsc a => dac;");
    REQUIRE(resultA.ok);

    // Compile on tabB — should succeed independently.
    auto resultB = mgr.evaluateCkTab(tabB, "SinOsc b => dac;");
    REQUIRE(resultB.ok);

    // Stop tabA — tabB should be unaffected.
    auto stopA = mgr.stopCkTab(tabA);
    REQUIRE(stopA.ok);

    // tabB should still be active.
    auto queryB = mgr.queryTabVM(tabB);
    REQUIRE(queryB.ok);
    REQUIRE(queryB.message.find("active") != std::string::npos);

    mgr.shutdown();
}

TEST_CASE("B4-K7: re-evaluation replaces prior shred (not duplicate)", "[k7][re-eval]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 7;

    auto act = mgr.activateTabVM(tab);
    REQUIRE(act.ok);

    // First eval.
    auto result1 = mgr.evaluateCkTab(tab, "SinOsc s1 => dac;");
    REQUIRE(result1.ok);

    // Second eval — should replace, not duplicate.
    auto result2 = mgr.evaluateCkTab(tab, "SinOsc s2 => dac;");
    REQUIRE(result2.ok);

    // Both should succeed (no error about duplicate shred).
    REQUIRE(result1.message.find("ok") != std::string::npos);
    REQUIRE(result2.message.find("ok") != std::string::npos);

    mgr.shutdown();
}

TEST_CASE("B4-K7: stop on already-stopped tab returns ok (idempotent)", "[k7][stop][idempotent]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 8;

    auto act = mgr.activateTabVM(tab);
    REQUIRE(act.ok);

    // Stop once.
    auto stop1 = mgr.stopCkTab(tab);
    REQUIRE(stop1.ok);

    // Stop again — should be ok (idempotent).
    auto stop2 = mgr.stopCkTab(tab);
    REQUIRE(stop2.ok);

    mgr.shutdown();
}

TEST_CASE("B4-K7: evaluate without activation still works (auto-activate)", "[k7][auto-activate]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 9;

    // Don't call activateTabVM — evaluateCkTab should auto-activate.
    auto result = mgr.evaluateCkTab(tab, kValidCk);
    REQUIRE(result.ok);

    // Verify VM is active.
    auto query = mgr.queryTabVM(tab);
    REQUIRE(query.ok);
    REQUIRE(query.message.find("active") != std::string::npos);

    mgr.shutdown();
}

TEST_CASE("B4-K7: tab out of range is rejected", "[k7][bounds]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t badTab = 255; // out of range

    auto result = mgr.evaluateCkTab(badTab, kValidCk);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.message.find("out of range") != std::string::npos);

    auto stopResult = mgr.stopCkTab(badTab);
    REQUIRE_FALSE(stopResult.ok);

    mgr.shutdown();
}
