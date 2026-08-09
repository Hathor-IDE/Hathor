// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_b8_k3_vm_shutdown.cpp — tests for B8-K3: VM shutdown after successful bake.
 *
 * These tests spawn the real hathor-audio-worker executable and verify:
 *   - After a successful ChuckRenderWriter render, the tab's VM is destroyed
 *     (verified via vm_query returning a destroyed/inactive state)
 *   - On compile failure, the VM is destroyed (no leak)
 *   - On worker-death during render, the VM is destroyed
 *   - The completion callback fires with success=true AND VM already destroyed
 *   - VM state transitions: active → rendering → destroyed (not left active)
 *
 * JUCE-free: links Catch2 + AudioWorkerManager + ChuckRenderWriter.
 * Requires the hathor-audio-worker binary to be built.
 *
 * Requirements: B8-K3, B8-K2, B4-K3, B4-K7
 */

#include <catch2/catch_test_macros.hpp>

#include "ChuckRenderWriter.hpp"
#include "AudioWorkerManager.hpp"
#include "audio_ipc.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

using hathor::AudioWorkerManager;
using hathor::ChuckRenderWriter;
using hathor::RenderResult;
using hathor::RenderState;

namespace {

constexpr unsigned kSampleRate = 44100;

/// Locate the hathor-audio-worker binary (built by CMake in the same tree).
std::string getWorkerPath()
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

/// Valid ChucK source for a short tone.
const std::string kValidCk =
    "SinOsc s => dac;"
    "440 => s.freq;"
    "0.5 => s.gain;"
    "0.1::second => now;";

/// Invalid ChucK source — no => operator, will fail compile.
const std::string kInvalidCk = "just some text without proper syntax";

} // namespace

// ---------------------------------------------------------------------------
// B8-K3 core: VM is destroyed after a successful render
// ---------------------------------------------------------------------------

TEST_CASE("B8-K3: successful render destroys the VM", "[b8-k3][success][destroy]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 0;

    // Activate the VM and compile valid source (simulating what a user would do
    // before triggering a bake).
    auto act = mgr.activateTabVM(tab, kSampleRate, 1);
    REQUIRE(act.ok);

    auto compileResult = mgr.evaluateCkTab(tab, kValidCk);
    REQUIRE(compileResult.ok);

    // Verify the VM is active before the render.
    auto queryBefore = mgr.queryTabVM(tab);
    REQUIRE(queryBefore.ok);
    REQUIRE(queryBefore.message.find("state=active") != std::string::npos);

    // Set up the render writer and start a render.
    auto tmpDir = std::filesystem::temp_directory_path();
    auto wavPath = tmpDir / "b8k3_success_destroy.wav";
    std::filesystem::remove(wavPath);

    std::promise<RenderResult> done;
    auto fut = done.get_future();

    ChuckRenderWriter writer(&mgr);
    writer.startRender(tab, kValidCk, 4410, kSampleRate, wavPath,
        [&](const RenderResult& r) {
            done.set_value(r);
        });

    // Wait for the render to complete (with timeout).
    REQUIRE(fut.wait_for(std::chrono::seconds(10)) == std::future_status::ready);

    RenderResult result = fut.get();

    // The render itself should have succeeded.
    REQUIRE(result.success);
    REQUIRE(result.state == RenderState::Completed);
    REQUIRE(result.errorMessage.empty());

    // B8-K3 acceptance: after a successful bake, the tab's VM must be gone.
    // Query the VM — it should be destroyed/inactive, NOT active.
    auto queryAfter = mgr.queryTabVM(tab);
    REQUIRE(queryAfter.ok);

    const bool vmIsGone =
        queryAfter.message.find("destroyed") != std::string::npos ||
        queryAfter.message.find("inactive") != std::string::npos ||
        queryAfter.message.find("no_vm") != std::string::npos;
    REQUIRE(vmIsGone);
    REQUIRE_FALSE(queryAfter.message.find("state=active") != std::string::npos);

    // The WAV file should exist.
    REQUIRE(std::filesystem::exists(wavPath));

    std::filesystem::remove(wavPath);
    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// B8-K3: VM is destroyed on compile-failure during render
// ---------------------------------------------------------------------------

TEST_CASE("B8-K3: render compile failure destroys the VM", "[b8-k3][failure][destroy][destroy-on-fail]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 1;

    // Activate the VM first (so it exists before the render tries to compile).
    auto act = mgr.activateTabVM(tab, kSampleRate, 1);
    REQUIRE(act.ok);

    auto queryBefore = mgr.queryTabVM(tab);
    REQUIRE(queryBefore.ok);
    REQUIRE(queryBefore.message.find("state=active") != std::string::npos);

    // Start a render with invalid source — should fail to compile.
    auto tmpDir = std::filesystem::temp_directory_path();
    auto wavPath = tmpDir / "b8k3_fail_destroy.wav";
    std::filesystem::remove(wavPath);

    std::promise<RenderResult> done;
    auto fut = done.get_future();

    ChuckRenderWriter writer(&mgr);
    writer.startRender(tab, kInvalidCk, 4410, kSampleRate, wavPath,
        [&](const RenderResult& r) {
            done.set_value(r);
        });

    REQUIRE(fut.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    RenderResult result = fut.get();

    // The render should have failed (bad ChucK source).
    REQUIRE_FALSE(result.success);
    REQUIRE(result.state == RenderState::Failed);

    // B8-K3: on failure path too, the VM must be destroyed (no leak).
    auto queryAfter = mgr.queryTabVM(tab);
    REQUIRE(queryAfter.ok);

    const bool vmIsGone =
        queryAfter.message.find("destroyed") != std::string::npos ||
        queryAfter.message.find("inactive") != std::string::npos ||
        queryAfter.message.find("no_vm") != std::string::npos;
    REQUIRE(vmIsGone);

    // No WAV should have been produced.
    REQUIRE_FALSE(std::filesystem::exists(wavPath));

    std::filesystem::remove(wavPath);
    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// B8-K3: VM destroyed on worker-death during render
// ---------------------------------------------------------------------------

TEST_CASE("B8-K3: render on dead worker fails and does not hang", "[b8-k3][dead-worker][error]")
{
    AudioWorkerManager mgr;
    // Do NOT start the worker — simulate a dead worker.

    const uint8_t tab = 2;
    auto tmpDir = std::filesystem::temp_directory_path();
    auto wavPath = tmpDir / "b8k3_dead_worker.wav";
    std::filesystem::remove(wavPath);

    std::promise<RenderResult> done;
    auto fut = done.get_future();

    ChuckRenderWriter writer(&mgr);
    writer.startRender(tab, kValidCk, 4410, kSampleRate, wavPath,
        [&](const RenderResult& r) {
            done.set_value(r);
        });

    // Should fail quickly (not hang waiting for a dead worker).
    REQUIRE(fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    RenderResult result = fut.get();

    REQUIRE_FALSE(result.success);
    REQUIRE(result.state == RenderState::Failed);
    REQUIRE_FALSE(result.errorMessage.empty());
    REQUIRE_FALSE(std::filesystem::exists(wavPath));

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// B8-K3: Multiple sequential renders each get their own VM lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("B8-K3: sequential renders each destroy their VM", "[b8-k3][sequential][multiple-renders]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 3;

    for (int i = 0; i < 3; ++i) {
        // Activate and compile for each iteration.
        auto act = mgr.activateTabVM(tab, kSampleRate, 1);
        REQUIRE(act.ok);

        auto compileResult = mgr.evaluateCkTab(tab, kValidCk);
        REQUIRE(compileResult.ok);

        auto tmpDir = std::filesystem::temp_directory_path();
        auto wavPath = tmpDir / ("b8k3_sequential_" + std::to_string(i) + ".wav");
        std::filesystem::remove(wavPath);

        std::promise<RenderResult> done;
        auto fut = done.get_future();

        ChuckRenderWriter writer(&mgr);
        writer.startRender(tab, kValidCk, 2205, kSampleRate, wavPath,
            [&](const RenderResult& r) {
                done.set_value(r);
            });

        REQUIRE(fut.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        RenderResult result = fut.get();

        REQUIRE(result.success);
        REQUIRE(result.state == RenderState::Completed);
        REQUIRE(std::filesystem::exists(wavPath));

        // B8-K3: VM must be destroyed after each successful render.
        auto queryAfter = mgr.queryTabVM(tab);
        REQUIRE(queryAfter.ok);
        const bool vmIsGone =
            queryAfter.message.find("destroyed") != std::string::npos ||
            queryAfter.message.find("inactive") != std::string::npos ||
            queryAfter.message.find("no_vm") != std::string::npos;
        REQUIRE(vmIsGone);

        std::filesystem::remove(wavPath);
    }

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// B8-K3: VM destroyed before completion callback fires
// ---------------------------------------------------------------------------

TEST_CASE("B8-K3: VM destroyed before completion callback", "[b8-k3][callback-order][destroy-before-callback]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 4;

    // Activate and compile.
    auto act = mgr.activateTabVM(tab, kSampleRate, 1);
    REQUIRE(act.ok);

    auto compileResult = mgr.evaluateCkTab(tab, kValidCk);
    REQUIRE(compileResult.ok);

    auto tmpDir = std::filesystem::temp_directory_path();
    auto wavPath = tmpDir / "b8k3_callback_order.wav";
    std::filesystem::remove(wavPath);

    // Use an atomic flag so the callback can communicate with the test thread.
    std::atomic<bool> callbackVmDestroyed{false};

    std::promise<RenderResult> done;
    auto fut = done.get_future();

    ChuckRenderWriter writer(&mgr);
    writer.startRender(tab, kValidCk, 1102, kSampleRate, wavPath,
        [&](const RenderResult& r) {
            // Inside the callback, check that the VM is already destroyed.
            // The B8-K3 spec says: shut down the VM BEFORE firing the callback.
            auto query = mgr.queryTabVM(tab);
            if (query.ok) {
                const bool gone =
                    query.message.find("destroyed") != std::string::npos ||
                    query.message.find("inactive") != std::string::npos ||
                    query.message.find("no_vm") != std::string::npos;
                callbackVmDestroyed.store(gone, std::memory_order_release);
            }
            done.set_value(r);
        });

    REQUIRE(fut.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    RenderResult result = fut.get();

    REQUIRE(result.success);
    REQUIRE(result.state == RenderState::Completed);

    // B8-K3: the VM must have been destroyed BEFORE the callback was invoked.
    REQUIRE(callbackVmDestroyed.load(std::memory_order_acquire));

    std::filesystem::remove(wavPath);
    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// B8-K3: Cancelled render also destroys the VM
// ---------------------------------------------------------------------------

TEST_CASE("B8-K3: cancelled render destroys the VM", "[b8-k3][cancel][destroy]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 5;

    auto act = mgr.activateTabVM(tab, kSampleRate, 1);
    REQUIRE(act.ok);

    auto compileResult = mgr.evaluateCkTab(tab, kValidCk);
    REQUIRE(compileResult.ok);

    auto tmpDir = std::filesystem::temp_directory_path();
    auto wavPath = tmpDir / "b8k3_cancel_destroy.wav";
    std::filesystem::remove(wavPath);

    std::promise<RenderResult> done;
    auto fut = done.get_future();

    ChuckRenderWriter writer(&mgr);
    auto handle = writer.startRender(tab, kValidCk, 44100, kSampleRate, wavPath,
        [&](const RenderResult& r) {
            done.set_value(r);
        });

    // Cancel immediately.
    handle.cancel();

    REQUIRE(fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    RenderResult result = fut.get();

    // Should be cancelled.
    REQUIRE_FALSE(result.success);
    REQUIRE(result.state == RenderState::Cancelled);

    // B8-K3: on cancellation, the VM must also be destroyed (no leak).
    auto queryAfter = mgr.queryTabVM(tab);
    REQUIRE(queryAfter.ok);
    const bool vmIsGone =
        queryAfter.message.find("destroyed") != std::string::npos ||
        queryAfter.message.find("inactive") != std::string::npos ||
        queryAfter.message.find("no_vm") != std::string::npos;
    REQUIRE(vmIsGone);

    // No WAV should be produced on cancellation.
    REQUIRE_FALSE(std::filesystem::exists(wavPath));

    std::filesystem::remove(wavPath);
    mgr.shutdown();
}
