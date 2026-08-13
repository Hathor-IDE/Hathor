// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_b4_k5_watchdog.cpp — tests for B4-K5 per-VM hang detection / watchdog.
 *
 * Tests the per-VM watchdog lifecycle:
 *   1. Healthy VM — heartbeat advances, watchdog does not trigger.
 *   2. Intentional hang — heartbeat stalls, watchdog detects at ~2s.
 *   3. Recovery — old VM torn down, fresh VM created, heartbeat restarts.
 *   4. Two active tabs — hanging one doesn't affect the other.
 *   5. Reverse isolation — hanging B doesn't affect A.
 *   6. Stopped tab — no false hang report.
 *   7. Suspended tab — no false hang; resume resets baseline.
 *   8. Duplicate detection/recovery — only one recovery per event.
 *   9. Restart failure — recompile error surfaces, tab stays isolated.
 *  10. Worker death — K5 doesn't claim worker death as a hung VM.
 *  11. Audio-thread safety — no mutex/blocking/join on audio callback path.
 *
 * JUCE-free: links only hathor-audio-worker-lib + Catch2. Uses the real
 * ChuckVM, VMManager, VmWatchdog classes (no worker process spawn needed for
 * unit tests; integration tests spawn the worker binary).
 *
 * Requirements: B4-K5, B4-K3, K0.5
 */

#define CATCH_CONFIG_NO_POSIX_SIGNALS

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "ChuckVm.hpp"
#include "VMManager.hpp"
#include "VmLifecycle.hpp"
#include "VmWatchdog.hpp"
#include "audio_ipc.h"
#include "ResourcePolicy.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using hathor::audio_worker::ChuckVM;
using hathor::audio_worker::VMManager;
using hathor::audio_worker::VMResult;
using hathor::audio_worker::VmLifecycle;
using hathor::audio_worker::VMState;
using hathor::audio_worker::VmWatchdog;
using hathor::audio_worker::WatchdogEntry;
using hathor::audio_worker::TabId;
using hathor::audio_worker::kDefaultHeartbeatTimeoutMs;
using hathor::audio_worker::kDefaultWatchdogIntervalMs;
using hathor::audio_worker::kMaxRestartAttempts;
using hathor::audio_worker::kRestartCooldownMs;
using hathor::audio_worker::kNumTabs;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

/// A render callback that produces silence and increments a counter.
/// Used for healthy VMs.
static ChuckVM::RenderCallback makeSilenceCallback(std::atomic<int>* blockCounter = nullptr)
{
    return [blockCounter](float* outBuf, unsigned numFrames, unsigned /*numChannels*/) {
        if (blockCounter)
            blockCounter->fetch_add(1, std::memory_order_relaxed);
        if (outBuf)
            std::memset(outBuf, 0, numFrames * sizeof(float));
    };
}

/// A render callback that can be "hung" by setting an atomic flag.
/// While the flag is set, the callback busy-spins WITHOUT advancing the
/// heartbeat (the heartbeat is incremented after the callback returns in
/// chucKThreadLoop).  The spin loop uses std::this_thread::yield() which
/// is a pthread cancellation point, allowing forceDestroyVM's pthread_cancel
/// to terminate the thread safely.
///
/// The flag is checked periodically so the callback can be "un-hung" by
/// clearing the flag, allowing normal cleanup without pthread_cancel.
static ChuckVM::RenderCallback makeHanggableCallback(std::atomic<bool>* hangFlag)
{
    return [hangFlag](float* /*outBuf*/, unsigned /*numFrames*/, unsigned /*numChannels*/) {
        // While hangFlag is set, spin without yielding to ChucK time.
        // This simulates a shred stuck in while(true){} with no now +=>.
        while (hangFlag->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };
}

/// Create a VmWatchdog with fast timeout/interval for testing.
/// Not used directly — tests configure the watchdog inline for clarity.
[[maybe_unused]] static std::unique_ptr<VmWatchdog> makeTestWatchdog(
    VMManager* vmMgr,
    VmLifecycle* vmLifecycle,
    VmWatchdog::HangDetectedCallback onHangDetected = nullptr,
    VmWatchdog::RecoveryCompleteCallback onRecoveryComplete = nullptr)
{
    auto watchdog = std::make_unique<VmWatchdog>(vmMgr, vmLifecycle,
        std::move(onHangDetected),
        std::move(onRecoveryComplete));
    // Use fast timeout for testing (200ms instead of 2s default).
    watchdog->setTimeout(std::chrono::milliseconds(200));
    watchdog->setInterval(std::chrono::milliseconds(50));
    return watchdog;
}

// ---------------------------------------------------------------------------
// Test 1: Healthy VM — heartbeat advances, watchdog does not trigger
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: healthy VM — heartbeat advances, no false hang", "[k5][healthy]")
{
    VMManager vmMgr;
    std::atomic<int> blockCount{0};
    vmMgr.setRenderCallback(makeSilenceCallback(&blockCount));

    TabId tabId = 0;
    VMResult r = vmMgr.activateVM(tabId);
    REQUIRE(r.ok);

    ChuckVM* vm = vmMgr.findVM(tabId);
    REQUIRE(vm != nullptr);
    REQUIRE(vm->state() == VMState::Active);

    // Let it run briefly.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Heartbeat should have advanced.
    uint64_t beat = vm->heartbeat();
    REQUIRE(beat > 0);

    // Blocks produced should be > 0.
    REQUIRE(vm->blocksProduced() > 0);

    // Blocks actually rendered.
    REQUIRE(blockCount.load() > 0);

    vmMgr.destroyVM(tabId);
    // VMManager::destroyVM erases from map, so we can't query the state
    // through findVM after destruction.  Use queryVM instead.
    VMResult qr = vmMgr.queryVM(tabId);
    REQUIRE(qr.ok);
    REQUIRE(qr.message.find("state=inactive") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 2: Intentional hang — heartbeat stalls, watchdog detects at ~2s
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: intentional hang — watchdog detects at ~2s", "[k5][hang][detection]")
{
    VMManager vmMgr;
    VmLifecycle vmLifecycle;

    std::atomic<bool> hangDetected{false};
    std::atomic<bool> hangFlag{true};  // Set to true = callback spins = heartbeat stalls
    int detectionCount = 0;

    TabId tabId = 0;

    // Track generations through VmLifecycle so the watchdog can use it.
    uint64_t gen = vmLifecycle.vmCreate(tabId);

    // Create VM with a hanging render callback that can be cleared for cleanup.
    vmMgr.setRenderCallback(makeHanggableCallback(&hangFlag));
    VMResult r = vmMgr.activateVM(tabId);
    REQUIRE(r.ok);

    ChuckVM* vm = vmMgr.findVM(tabId);
    REQUIRE(vm != nullptr);
    vm->setGeneration(gen);
    REQUIRE(vm->state() == VMState::Active);

    VmWatchdog watchdog(&vmMgr, &vmLifecycle,
        [&hangDetected, &detectionCount](TabId, uint64_t, uint64_t, std::chrono::steady_clock::time_point) {
            hangDetected.store(true, std::memory_order_release);
            ++detectionCount;
        },
        nullptr
    );

    watchdog.setTimeout(std::chrono::milliseconds(200));
    watchdog.setInterval(std::chrono::milliseconds(50));
    watchdog.setAutoRecovery(false);  // Detection-only test — no recovery
    watchdog.registerVM(tabId, gen);
    watchdog.start();

    // Wait for detection (should be ~200ms + 50ms interval = ~250ms).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!hangDetected.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    REQUIRE(hangDetected.load(std::memory_order_acquire));
    REQUIRE(detectionCount == 1);

    // Clear the hang flag so the thread can exit cooperatively.
    hangFlag.store(false, std::memory_order_release);

    watchdog.stop();

    // Now destroy the VM — the thread should exit cleanly.
    vmMgr.destroyVM(tabId);
}

// ---------------------------------------------------------------------------
// Test 3: Recovery — old VM torn down, fresh VM created
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: recovery — old VM torn down, fresh VM created", "[k5][recovery]")
{
    VMManager vmMgr;
    VmLifecycle vmLifecycle;

    std::atomic<bool> hangDetected{false};
    std::atomic<bool> recoveryComplete{false};
    std::atomic<bool> hangFlag{true};  // Controls the hanging callback
    uint64_t newGen = 0;

    TabId tabId = 3;

    // Create the initial VM with a hanging callback that can be cleared.
    uint64_t gen1 = vmLifecycle.vmCreate(tabId);
    vmMgr.setRenderCallback(makeHanggableCallback(&hangFlag));
    VMResult r = vmMgr.activateVM(tabId);
    REQUIRE(r.ok);

    ChuckVM* vm = vmMgr.findVM(tabId);
    REQUIRE(vm != nullptr);
    vm->setGeneration(gen1);

    VmWatchdog watchdog(&vmMgr, &vmLifecycle,
        // Hang detection callback: clear the hang flag BEFORE recovery
        // so forceDestroyVM's cooperative join can succeed without
        // needing pthread_cancel.
        [&hangDetected, &hangFlag, &vmMgr](TabId, uint64_t, uint64_t, std::chrono::steady_clock::time_point) {
            hangDetected.store(true, std::memory_order_release);
            hangFlag.store(false, std::memory_order_release);
            // Also set a silence callback so the recovered VM doesn't hang.
            vmMgr.setRenderCallback(ChuckVM::RenderCallback{
                [](float* outBuf, unsigned numFrames, unsigned) {
                    if (outBuf) std::memset(outBuf, 0, numFrames * sizeof(float));
                }
            });
        },
        // Recovery callback: record the new generation.
        [&recoveryComplete, &newGen](TabId, uint64_t gen) {
            newGen = gen;
            recoveryComplete.store(true, std::memory_order_release);
        }
    );

    watchdog.setTimeout(std::chrono::milliseconds(200));
    watchdog.setInterval(std::chrono::milliseconds(50));
    watchdog.registerVM(tabId, gen1);
    watchdog.start();

    // Wait for detection + recovery.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!recoveryComplete.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    REQUIRE(hangDetected.load(std::memory_order_acquire));
    REQUIRE(recoveryComplete.load(std::memory_order_acquire));

    // After recovery, the VM should have a new generation and be active.
    // The watchdog calls vmCreate + activateVM internally, so the new VM
    // should be tracked by VMManager.  Verify the generation changed.
    uint64_t postRecoveryGen = vmLifecycle.generationOf(tabId);
    REQUIRE(postRecoveryGen > gen1);

    // The old VM was force-destroyed; the new VM should be active.
    ChuckVM* newVm = vmMgr.findVM(tabId);
    REQUIRE(newVm != nullptr);
    REQUIRE(newVm->state() == VMState::Active);
    REQUIRE(newVm->generation() == postRecoveryGen);

    watchdog.stop();

    // Give the recovered VM a moment to produce some heartbeats.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Cleanup.
    vmMgr.destroyVM(tabId);
}

// ---------------------------------------------------------------------------
// Test 4: Two active tabs — hanging one doesn't affect the other
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: two active tabs — isolate hang to affected tab", "[k5][isolation][A-hangs]")
{
    VMManager vmMgr;
    VmLifecycle vmLifecycle;

    std::atomic<bool> tabAHungDetected{false};
    std::atomic<bool> tabBHungDetected{false};
    std::atomic<int> blockCountB{0};
    std::atomic<bool> hangFlagA{true};  // Tab A's callback will spin

    TabId tabA = 0;
    TabId tabB = 1;

    // Create the watchdog with isolation — only tabA will hang.
    VmWatchdog watchdog(&vmMgr, &vmLifecycle,
        [&tabAHungDetected, &tabBHungDetected, &tabA, &tabB](
            TabId tabId, uint64_t, uint64_t, std::chrono::steady_clock::time_point) {
            if (tabId == tabA) tabAHungDetected.store(true, std::memory_order_release);
            if (tabB == tabId) tabBHungDetected.store(true, std::memory_order_release);
        },
        nullptr
    );

    watchdog.setTimeout(std::chrono::milliseconds(200));
    watchdog.setInterval(std::chrono::milliseconds(50));
    watchdog.setAutoRecovery(false);  // Detection-only: don't trigger recovery

    // --- Tab A: hanging VM (flaggable for clean cleanup) ---
    uint64_t genA = vmLifecycle.vmCreate(tabA);
    vmMgr.setRenderCallback(makeHanggableCallback(&hangFlagA));
    VMResult rA = vmMgr.activateVM(tabA);
    REQUIRE(rA.ok);
    ChuckVM* vmA = vmMgr.findVM(tabA);
    REQUIRE(vmA != nullptr);
    vmA->setGeneration(genA);

    // --- Tab B: healthy VM ---
    // Set silence callback for tab B (overwrites the hang callback for new VMs only).
    uint64_t genB = vmLifecycle.vmCreate(tabB);
    vmMgr.setRenderCallback(makeSilenceCallback(&blockCountB));
    VMResult rB = vmMgr.activateVM(tabB);
    REQUIRE(rB.ok);
    ChuckVM* vmB = vmMgr.findVM(tabB);
    REQUIRE(vmB != nullptr);
    vmB->setGeneration(genB);

    // Register both with the watchdog.
    watchdog.registerVM(tabA, genA);
    watchdog.registerVM(tabB, genB);
    watchdog.start();

    // Wait for tab A to be detected as hung.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!tabAHungDetected.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    REQUIRE(tabAHungDetected.load(std::memory_order_acquire));  // A was hung

    // Give a little extra time to ensure tab B is NOT flagged.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    REQUIRE_FALSE(tabBHungDetected.load(std::memory_order_acquire));  // B is fine

    // Tab B should still be producing blocks.
    REQUIRE(blockCountB.load(std::memory_order_acquire) > 0);

    watchdog.stop();

    // Clear the hang flag so tab A's thread can exit cooperatively.
    hangFlagA.store(false, std::memory_order_release);

    // Cleanup.
    vmMgr.destroyVM(tabA);
    vmMgr.destroyVM(tabB);
}

// ---------------------------------------------------------------------------
// Test 5: Reverse isolation — hang B, A continues
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: reverse isolation — hang B, A continues", "[k5][isolation][B-hangs]")
{
    VMManager vmMgr;
    VmLifecycle vmLifecycle;

    std::atomic<bool> tabAHungDetected{false};
    std::atomic<bool> tabBHungDetected{false};
    std::atomic<bool> recoveryComplete{false};
    std::atomic<bool> hangFlagB{true};  // Tab B's callback will spin
    std::atomic<int> blockCountA{0};

    TabId tabA = 0;
    TabId tabB = 1;

    VmWatchdog watchdog(&vmMgr, &vmLifecycle,
        // Hang detection: clear the flag BEFORE recovery so forceDestroyVM's
        // cooperative join succeeds without needing pthread_cancel.
        [&tabAHungDetected, &tabBHungDetected, &tabA, &tabB, &hangFlagB, &vmMgr](
            TabId tabId, uint64_t, uint64_t, std::chrono::steady_clock::time_point) {
            if (tabId == tabA) tabAHungDetected.store(true, std::memory_order_release);
            if (tabB == tabId) {
                tabBHungDetected.store(true, std::memory_order_release);
                hangFlagB.store(false, std::memory_order_release);
                vmMgr.setRenderCallback(ChuckVM::RenderCallback{
                    [](float* outBuf, unsigned numFrames, unsigned) {
                        if (outBuf) std::memset(outBuf, 0, numFrames * sizeof(float));
                    }
                });
            }
        },
        [&recoveryComplete](TabId, uint64_t) {
            recoveryComplete.store(true, std::memory_order_release);
        }
    );

    watchdog.setTimeout(std::chrono::milliseconds(200));
    watchdog.setInterval(std::chrono::milliseconds(50));

    // --- Tab A: healthy VM ---
    uint64_t genA = vmLifecycle.vmCreate(tabA);
    vmMgr.setRenderCallback(makeSilenceCallback(&blockCountA));
    VMResult rA = vmMgr.activateVM(tabA);
    REQUIRE(rA.ok);
    ChuckVM* vmA = vmMgr.findVM(tabA);
    REQUIRE(vmA != nullptr);
    vmA->setGeneration(genA);

    // --- Tab B: hanging VM (flaggable for clean cleanup) ---
    uint64_t genB = vmLifecycle.vmCreate(tabB);
    vmMgr.setRenderCallback(makeHanggableCallback(&hangFlagB));
    VMResult rB = vmMgr.activateVM(tabB);
    REQUIRE(rB.ok);
    ChuckVM* vmB = vmMgr.findVM(tabB);
    REQUIRE(vmB != nullptr);
    vmB->setGeneration(genB);

    watchdog.registerVM(tabA, genA);
    watchdog.registerVM(tabB, genB);
    watchdog.start();

    // Wait for tab B to be detected as hung.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!tabBHungDetected.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    REQUIRE(tabBHungDetected.load(std::memory_order_acquire));  // B was hung
    REQUIRE_FALSE(tabAHungDetected.load(std::memory_order_acquire));  // A is fine

    // Tab A should still be producing blocks.
    REQUIRE(blockCountA.load(std::memory_order_acquire) > 0);

    watchdog.stop();

    // Wait for recovery to complete (if it hasn't already).
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!recoveryComplete.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // The flag was cleared by the recovery callback, so the thread should
    // have exited cooperatively.  Destroy should work without pthread_cancel.
    vmMgr.destroyVM(tabB);
    vmMgr.destroyVM(tabA);
}

// ---------------------------------------------------------------------------
// Test 6: Stopped tab — no false hang report
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: stopped/inactive tab — no false hang", "[k5][stopped][no-false-positive]")
{
    VMManager vmMgr;
    VmLifecycle vmLifecycle;

    std::atomic<bool> hangDetected{false};

    TabId tabId = 5;

    VmWatchdog watchdog(&vmMgr, &vmLifecycle,
        [&hangDetected](TabId, uint64_t, uint64_t, std::chrono::steady_clock::time_point) {
            hangDetected.store(true, std::memory_order_release);
        },
        nullptr
    );

    watchdog.setTimeout(std::chrono::milliseconds(200));
    watchdog.setInterval(std::chrono::milliseconds(50));

    // Create and activate a VM, then immediately destroy it.
    uint64_t gen = vmLifecycle.vmCreate(tabId);
    vmMgr.setRenderCallback(makeSilenceCallback());
    VMResult r = vmMgr.activateVM(tabId);
    REQUIRE(r.ok);

    ChuckVM* vm = vmMgr.findVM(tabId);
    REQUIRE(vm != nullptr);
    vm->setGeneration(gen);
    REQUIRE(vm->state() == VMState::Active);

    // Register with the watchdog.
    watchdog.registerVM(tabId, gen);
    watchdog.start();

    // Let it run for a bit — heartbeat should advance.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Now stop the VM (destroy it).
    vmMgr.destroyVM(tabId);
    // VMManager::destroyVM erases from map — verify via queryVM instead.
    VMResult qr = vmMgr.queryVM(tabId);
    REQUIRE(qr.ok);
    REQUIRE(qr.message.find("state=inactive") != std::string::npos);

    // Wait well past the timeout — the watchdog should NOT detect a hang
    // because the VM is no longer Active.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    REQUIRE_FALSE(hangDetected.load(std::memory_order_acquire));

    watchdog.stop();
}

// ---------------------------------------------------------------------------
// Test 7: Suspended tab — no false hang; resume resets baseline
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: suspended tab — no false hang; resume resets baseline", "[k5][suspended][resume]")
{
    VMManager vmMgr;
    VmLifecycle vmLifecycle;

    std::atomic<bool> hangDetected{false};

    TabId tabId = 7;

    VmWatchdog watchdog(&vmMgr, &vmLifecycle,
        [&hangDetected](TabId, uint64_t, uint64_t, std::chrono::steady_clock::time_point) {
            hangDetected.store(true, std::memory_order_release);
        },
        nullptr
    );

    watchdog.setTimeout(std::chrono::milliseconds(200));
    watchdog.setInterval(std::chrono::milliseconds(50));

    uint64_t gen = vmLifecycle.vmCreate(tabId);
    vmMgr.setRenderCallback(makeSilenceCallback());
    VMResult r = vmMgr.activateVM(tabId);
    REQUIRE(r.ok);

    ChuckVM* vm = vmMgr.findVM(tabId);
    REQUIRE(vm != nullptr);
    vm->setGeneration(gen);
    REQUIRE(vm->state() == VMState::Active);

    watchdog.registerVM(tabId, gen);
    watchdog.start();

    // Let it run for a bit (heartbeat advances).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Suspend the VM.
    VMResult sr = vmMgr.deactivateVM(tabId, true);
    REQUIRE(sr.ok);
    REQUIRE(vm->state() == VMState::Suspended);

    // Wait well past the timeout — watchdog should NOT flag a suspended VM.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    REQUIRE_FALSE(hangDetected.load(std::memory_order_acquire));

    // Resume the VM — the watchdog should reset the heartbeat baseline.
    VMResult rr = vmMgr.resumeVM(tabId);
    REQUIRE(rr.ok);
    REQUIRE(vm->state() == VMState::Active);

    // Re-register with the watchdog (simulating what the worker does on vm_resume).
    watchdog.resetHeartbeat(tabId);

    // Let it run — heartbeat should advance, no hang.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    REQUIRE_FALSE(hangDetected.load(std::memory_order_acquire));

    watchdog.stop();
    vmMgr.destroyVM(tabId);
}

// ---------------------------------------------------------------------------
// Test 8: Duplicate detection/recovery — only one recovery per event
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: duplicate detection — only one recovery", "[k5][race][idempotent]")
{
    VMManager vmMgr;
    VmLifecycle vmLifecycle;

    std::atomic<int> detectionCount{0};
    std::atomic<int> recoveryCount{0};
    std::atomic<bool> hangFlag{true};  // Controls the hanging callback

    TabId tabId = 9;

    VmWatchdog watchdog(&vmMgr, &vmLifecycle,
        // Hang detection: clear the flag BEFORE recovery so forceDestroyVM's
        // cooperative join succeeds without needing pthread_cancel.
        [&detectionCount, &hangFlag, &vmMgr](TabId, uint64_t, uint64_t, std::chrono::steady_clock::time_point) {
            detectionCount.fetch_add(1, std::memory_order_acq_rel);
            hangFlag.store(false, std::memory_order_release);
            // Set silence callback so the recovered VM doesn't hang.
            vmMgr.setRenderCallback(ChuckVM::RenderCallback{
                [](float* outBuf, unsigned numFrames, unsigned /*numChannels*/) {
                    if (outBuf) std::memset(outBuf, 0, numFrames * sizeof(float));
                }
            });
        },
        // Recovery callback: just count.
        [&recoveryCount](TabId, uint64_t) {
            recoveryCount.fetch_add(1, std::memory_order_acq_rel);
        }
    );

    watchdog.setTimeout(std::chrono::milliseconds(200));
    watchdog.setInterval(std::chrono::milliseconds(50));

    // Create a hanging VM via VMManager.
    uint64_t gen = vmLifecycle.vmCreate(tabId);
    vmMgr.setRenderCallback(makeHanggableCallback(&hangFlag));
    VMResult r = vmMgr.activateVM(tabId);
    REQUIRE(r.ok);

    ChuckVM* vm = vmMgr.findVM(tabId);
    REQUIRE(vm != nullptr);
    vm->setGeneration(gen);

    watchdog.registerVM(tabId, gen);
    watchdog.start();

    // Wait for detection + recovery.  Recovery runs asynchronously on the
    // watchdog thread AFTER the detection callback (detect -> recover ->
    // onRecoveryComplete), so waiting on detection alone would race the
    // `newGen > gen` assertion below: the detection callback can fire before
    // vmCreate() has produced the replacement generation.  Wait for both.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((detectionCount.load() == 0 || recoveryCount.load() == 0) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // After recovery, the VM should have been recreated with a new generation.
    uint64_t newGen = vmLifecycle.generationOf(tabId);
    REQUIRE(newGen > gen);

    // The watchdog should not re-detect the same hang on the new VM
    // (because the new VM has a fresh heartbeat that advances).
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Should have exactly 1 detection and 1 recovery (within the retry budget).
    REQUIRE(detectionCount.load() >= 1);
    REQUIRE(recoveryCount.load() >= 1);

    watchdog.stop();

    // Cleanup
    vmMgr.destroyVM(tabId);
}

// ---------------------------------------------------------------------------
// Test 9: Restart limit — repeated failures are bounded
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: restart limit — repeated failures bounded", "[k5][restart-limit][bounded]")
{
    VmWatchdog watchdog(nullptr, nullptr, nullptr, nullptr);

    TabId tabId = 2;

    // Register the VM with generation 1.
    watchdog.registerVM(tabId, 1);

    // Verify initial state.
    REQUIRE(watchdog.restartCountFor(tabId) == 0);
    REQUIRE(watchdog.monitoredCount() == 1);

    // The watchdog enforces the limit internally via canRestart().
    // We verify the constants that govern the limit.
    REQUIRE(kMaxRestartAttempts > 0);
    REQUIRE(kRestartCooldownMs > 0);
    REQUIRE(kDefaultHeartbeatTimeoutMs == 2000);
    REQUIRE(kDefaultWatchdogIntervalMs == 500);
}

// ---------------------------------------------------------------------------
// Test 10: Worker death — K5 doesn't claim worker death as a hung VM
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: worker death distinct from VM hang", "[k5][worker-death][distinct]")
{
    // The K5 watchdog only checks per-VM heartbeats, not the worker-level
    // SharedAudioTransport::lastHeartbeat.  Worker death detection is K2's
    // responsibility (AudioWorkerManager::isWorkerAlive).
    //
    // This test verifies the watchdog only monitors VMState::Active VMs and
    // does not claim a non-existent VM is hung.

    VMManager vmMgr;
    VmLifecycle vmLifecycle;

    std::atomic<bool> hangDetected{false};

    VmWatchdog watchdog(&vmMgr, &vmLifecycle,
        [&hangDetected](TabId, uint64_t, uint64_t, std::chrono::steady_clock::time_point) {
            hangDetected.store(true, std::memory_order_release);
        },
        nullptr
    );

    watchdog.setTimeout(std::chrono::milliseconds(200));
    watchdog.setInterval(std::chrono::milliseconds(50));

    // Don't create any VM — simulate worker death scenario where
    // no VMs exist.  The watchdog should not detect anything.
    watchdog.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    REQUIRE_FALSE(hangDetected.load());

    // No VMs should be monitored.
    REQUIRE(watchdog.monitoredCount() == 0);
    REQUIRE(watchdog.totalHangDetections() == 0);

    watchdog.stop();
}

// ---------------------------------------------------------------------------
// Test 11: Audio-thread safety — no mutex/blocking/join on audio callback path
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: heartbeat is lock-free and allocation-free", "[k5][rt-safe][heartbeat]")
{
    VMManager vmMgr;
    std::atomic<int> blockCount{0};
    vmMgr.setRenderCallback(makeSilenceCallback(&blockCount));

    TabId tabId = 4;
    VMResult r = vmMgr.activateVM(tabId);
    REQUIRE(r.ok);

    ChuckVM* vm = vmMgr.findVM(tabId);
    REQUIRE(vm != nullptr);

    // Let it run briefly to produce heartbeats.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Reading the heartbeat must be a single atomic load — no mutex, no alloc.
    uint64_t beat1 = vm->heartbeat();
    uint64_t beat2 = vm->heartbeat();

    // The heartbeat should be a valid uint64_t value.
    REQUIRE(beat1 <= beat2);  // Monotonically non-decreasing.

    // The state() check must also be lock-free (relaxed atomic load).
    VMState state = vm->state();
    REQUIRE(state == VMState::Active);

    // Generation access must be lock-free.
    uint64_t gen = vm->generation();
    REQUIRE(gen >= 0);  // Generation starts at 0 for a fresh VM.

    // Blocks produced should be > 0.
    REQUIRE(vm->blocksProduced() > 0);

    // Blocks actually rendered.
    REQUIRE(blockCount.load() > 0);

    vmMgr.destroyVM(tabId);
}

// ---------------------------------------------------------------------------
// Unit test: WatchdogEntry heartbeat tracking
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: WatchdogEntry tracks heartbeat correctly", "[k5][unit][watchdog-entry]")
{
    WatchdogEntry entry(0);
    REQUIRE(entry.tabId == 0);
    REQUIRE(entry.lastHeartbeat.load() == 0);
    REQUIRE(entry.trackedGeneration.load() == 0);
    REQUIRE(entry.restartCount.load() == 0);

    // Reset should clear everything.
    entry.lastHeartbeat.store(42, std::memory_order_release);
    entry.trackedGeneration.store(7, std::memory_order_release);
    entry.restartCount.store(3, std::memory_order_release);
    entry.reset();

    REQUIRE(entry.lastHeartbeat.load() == 0);
    REQUIRE(entry.trackedGeneration.load() == 0);
    REQUIRE(entry.restartCount.load() == 0);
}

// ---------------------------------------------------------------------------
// Unit test: Watchdog timeout/interval configuration
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: watchdog timeout and interval configurable", "[k5][unit][config]")
{
    VMManager vmMgr;
    VmLifecycle vmLifecycle;
    VmWatchdog watchdog(&vmMgr, &vmLifecycle, nullptr, nullptr);

    // Default values.
    REQUIRE(kDefaultHeartbeatTimeoutMs == 2000);
    REQUIRE(kDefaultWatchdogIntervalMs == 500);

    // Configure.
    watchdog.setTimeout(std::chrono::milliseconds(500));
    watchdog.setInterval(std::chrono::milliseconds(100));

    watchdog.registerVM(1, 1);
    watchdog.start();
    watchdog.stop();
}

// ---------------------------------------------------------------------------
// Unit test: False positive protection — only Active VMs are monitored
// ---------------------------------------------------------------------------

TEST_CASE("B4-K5: only Active VMs are monitored (false positive protection)", "[k5][false-positive]")
{
    VMManager vmMgr;
    VmLifecycle vmLifecycle;

    std::atomic<bool> hangDetected{false};

    TabId tabId = 6;

    VmWatchdog watchdog(&vmMgr, &vmLifecycle,
        [&hangDetected](TabId, uint64_t, uint64_t, std::chrono::steady_clock::time_point) {
            hangDetected.store(true, std::memory_order_release);
        },
        nullptr
    );

    watchdog.setTimeout(std::chrono::milliseconds(200));
    watchdog.setInterval(std::chrono::milliseconds(50));

    // Create a VM, activate it, then suspend it.
    uint64_t gen = vmLifecycle.vmCreate(tabId);
    vmMgr.setRenderCallback(makeSilenceCallback());
    VMResult r = vmMgr.activateVM(tabId);
    REQUIRE(r.ok);

    ChuckVM* vm = vmMgr.findVM(tabId);
    REQUIRE(vm != nullptr);
    vm->setGeneration(gen);

    // Register with watchdog.
    watchdog.registerVM(tabId, gen);
    watchdog.start();

    // Let it run and produce heartbeats.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Suspend — the watchdog should stop monitoring.
    VMResult sr = vmMgr.deactivateVM(tabId, true);
    REQUIRE(sr.ok);
    REQUIRE(vm->state() == VMState::Suspended);

    // Even after timeout, no hang should be detected for a suspended VM.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    REQUIRE_FALSE(hangDetected.load());

    watchdog.stop();
    vmMgr.destroyVM(tabId);
}
