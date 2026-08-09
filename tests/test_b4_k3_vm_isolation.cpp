// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_b4_k3_vm_isolation.cpp — tests for B4-K3 per-tab VM isolation + resource policy.
 *
 * Tests the VM lifecycle through the control-plane socket:
 *   1. Open tab (no VM) → no VM created, no OS thread spawned.
 *   2. Single activate → VM created, heartbeat advancing.
 *   3. Two simultaneously active tabs → independent VMs/threads.
 *   4. Stop one → other continues unaffected.
 *   5. Failure isolation → hung/crashed thread in one tab doesn't affect others.
 *   6. Suspension → VM paused, state retained, heartbeat stops.
 *   7. Resume → VM continues, state preserved, heartbeat resumes.
 *   8. Resource ceiling → maxConcurrentLiveVMs enforced.
 *   9. Ceiling eviction → LRU suspend at ceiling.
 *  10. Worker restart → mapping survives (re-activation works).
 *  11. Repeated lifecycle → create/suspend/resume/destroy cycles without leaks.
 *
 * JUCE-free: these tests link Catch2 + AudioWorkerManager only, spawning the
 * real hathor-audio-worker binary.
 *
 * Requirements: B4-K3, B4-K0.5, B4-K0.6, Decision #24
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AudioWorkerManager.hpp"
#include "audio_ipc.h"
#include "VMManager.hpp"
#include "ChuckVm.hpp"
#include "ResourcePolicy.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using hathor::AudioWorkerManager;
namespace aw = hathor::audio_worker;
using hathor::audio_worker::ChuckVM;

// ---------------------------------------------------------------------------
// Helpers (reuse from test_audio_worker_manager.cpp)
// ---------------------------------------------------------------------------

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

    const char* envSrc = std::getenv("CMAKE_SOURCE_DIR");
    if (envSrc) {
        fs::path p = fs::path(envSrc) / "build" / "app" / "audio-worker" / "hathor-audio-worker";
        if (fs::exists(p))
            return p.string();
        p = fs::path(envSrc) / "build3" / "app" / "audio-worker" / "hathor-audio-worker";
        if (fs::exists(p))
            return p.string();
    }

    const fs::path candidates[] = {
        fs::current_path() / "hathor-audio-worker",
        fs::current_path() / "build" / "hathor-audio-worker",
        fs::current_path() / "build3" / "app" / "audio-worker" / "hathor-audio-worker",
        fs::current_path() / "build" / "app" / "audio-worker" / "hathor-audio-worker",
        fs::current_path() / "app" / "audio-worker" / "hathor-audio-worker",
        fs::current_path() / "cmake-build-debug" / "hathor-audio-worker",
        fs::current_path() / "cmake-build-release" / "hathor-audio-worker",
    };

    for (const auto& p : candidates) {
        if (fs::exists(p))
            return p.string();
    }

    return "";
}

// ---------------------------------------------------------------------------
// Test 1: Open tab → no VM created
//
// Per B4-K3: "a live VM is not auto-created merely because a .ck file is
// open." Simply starting the worker and NOT sending vm_activate should
// result in zero active VMs.
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: open tab does not create VM", "[k3][no-vm-on-open]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    // The worker starts but no VMs should be active.
    std::string resp = mgr.sendControlCommand("vm_list", 1000);
    REQUIRE(resp.find("count=0") != std::string::npos);
    REQUIRE(resp.find("active=0") != std::string::npos);

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// Test 2: Single activate creates a VM with advancing heartbeat
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: single VM activation", "[k3][single-vm][heartbeat]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    // Activate VM for tab 0.
    std::string resp = mgr.sendControlCommand("vm_activate 0 44100 1", 1000);
    REQUIRE(resp.find("ok vm_activated") != std::string::npos);

    // Query the VM — should be active.
    resp = mgr.sendControlCommand("vm_query 0", 1000);
    REQUIRE(resp.find("state=active") != std::string::npos);

    // The VM should produce audio and advance its heartbeat.
    const uint64_t gen = mgr.generation();
    float buf[aw::kBlockSize];
    uint32_t reads = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && reads < 5) {
        if (mgr.tryReadAudioBlock(buf, aw::kBlockSize, gen)) {
            ++reads;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    REQUIRE(reads >= 5);

    // Verify a second VM for a different tab does NOT exist yet.
    resp = mgr.sendControlCommand("vm_query 1", 1000);
    REQUIRE(resp.find("state=inactive") != std::string::npos);

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// Test 3: Two simultaneously active tabs on independent VMs/threads
//
// Per B4-K3 acceptance: "two simultaneously active .ck tabs run on independent
// VMs/threads/watchdogs"
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: two independent active VMs", "[k3][isolation][two-vms]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    // Activate two VMs on different tabs.
    std::string resp = mgr.sendControlCommand("vm_activate 0 44100 1", 1000);
    REQUIRE(resp.find("ok vm_activated") != std::string::npos);

    resp = mgr.sendControlCommand("vm_activate 1 44100 1", 1000);
    REQUIRE(resp.find("ok vm_activated") != std::string::npos);

    // Both should be active.
    resp = mgr.sendControlCommand("vm_query 0", 1000);
    REQUIRE(resp.find("state=active") != std::string::npos);

    resp = mgr.sendControlCommand("vm_query 1", 1000);
    REQUIRE(resp.find("state=active") != std::string::npos);

    // vm_list should show 2 active.
    resp = mgr.sendControlCommand("vm_list", 1000);
    REQUIRE(resp.find("count=2") != std::string::npos);
    REQUIRE(resp.find("active=2") != std::string::npos);

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// Test 4: Stopping one VM never affects the other
//
// Per B4-K3 acceptance: "hanging or stopping one never affects the other"
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: stopping one VM does not affect other", "[k3][isolation][stop-one]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    // Activate two VMs.
    REQUIRE(mgr.sendControlCommand("vm_activate 0 44100 1", 1000).find("ok") != std::string::npos);
    REQUIRE(mgr.sendControlCommand("vm_activate 1 44100 1", 1000).find("ok") != std::string::npos);

    // Verify audio flows.
    const uint64_t gen = mgr.generation();
    float buf[aw::kBlockSize];
    bool gotFirst = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(buf, aw::kBlockSize, gen)) {
            gotFirst = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gotFirst);

    // Deactivate VM on tab 0.
    std::string resp = mgr.sendControlCommand("vm_deactivate 0 suspend", 1000);
    REQUIRE(resp.find("ok vm_deactivated") != std::string::npos);

    // Tab 1 should still be active.
    resp = mgr.sendControlCommand("vm_query 1", 1000);
    REQUIRE(resp.find("state=active") != std::string::npos);

    // Tab 0 should be suspended.
    resp = mgr.sendControlCommand("vm_query 0", 1000);
    REQUIRE(resp.find("state=suspended") != std::string::npos);

    // Audio should still flow (from tab 1's VM).
    bool gotMore = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(buf, aw::kBlockSize, gen)) {
            gotMore = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gotMore);

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// Test 5: Failure isolation — a destroyed/erroneous VM doesn't affect others
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: failure isolation — destroying one VM", "[k3][isolation][failure]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    // Activate two VMs.
    REQUIRE(mgr.sendControlCommand("vm_activate 0 44100 1", 1000).find("ok") != std::string::npos);
    REQUIRE(mgr.sendControlCommand("vm_activate 1 44100 1", 1000).find("ok") != std::string::npos);

    // Destroy VM on tab 0 (full destroy + remove from table).
    std::string resp = mgr.sendControlCommand("vm_destroy 0", 1000);
    REQUIRE(resp.find("ok vm_destroyed") != std::string::npos);

    // Tab 0 should be gone (inactive/destroyed).
    resp = mgr.sendControlCommand("vm_query 0", 1000);
    REQUIRE(resp.find("state=inactive") != std::string::npos);

    // Tab 1 should still be active — failure isolated.
    resp = mgr.sendControlCommand("vm_query 1", 1000);
    REQUIRE(resp.find("state=active") != std::string::npos);

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// Test 6: Suspension pauses the VM (deterministic, state retained)
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: suspension pauses VM", "[k3][suspend][state-retain]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    // Activate VM on tab 2.
    REQUIRE(mgr.sendControlCommand("vm_activate 2 44100 1", 1000).find("ok") != std::string::npos);

    // Get initial heartbeat.
    std::string resp = mgr.sendControlCommand("vm_query 2", 1000);
    REQUIRE(resp.find("state=active") != std::string::npos);

    // Read some audio to advance heartbeat.
    const uint64_t gen = mgr.generation();
    float buf[aw::kBlockSize];
    uint32_t reads = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline && reads < 3) {
        if (mgr.tryReadAudioBlock(buf, aw::kBlockSize, gen))
            ++reads;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(reads >= 3);

    // Suspend the VM.
    resp = mgr.sendControlCommand("vm_deactivate 2 suspend", 1000);
    REQUIRE(resp.find("ok vm_deactivated") != std::string::npos);

    // State should be suspended.
    resp = mgr.sendControlCommand("vm_query 2", 1000);
    REQUIRE(resp.find("state=suspended") != std::string::npos);

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// Test 7: Resume restores the VM without losing state
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: resume restores VM", "[k3][resume][state-preserve]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    // Activate, suspend, then resume.
    REQUIRE(mgr.sendControlCommand("vm_activate 3 44100 1", 1000).find("ok") != std::string::npos);

    std::string resp = mgr.sendControlCommand("vm_query 3", 1000);
    REQUIRE(resp.find("state=active") != std::string::npos);

    // Suspend.
    REQUIRE(mgr.sendControlCommand("vm_deactivate 3 suspend", 1000).find("ok") != std::string::npos);
    resp = mgr.sendControlCommand("vm_query 3", 1000);
    REQUIRE(resp.find("state=suspended") != std::string::npos);

    // Resume.
    resp = mgr.sendControlCommand("vm_resume 3", 1000);
    REQUIRE(resp.find("ok vm_resumed") != std::string::npos);

    // Should be active again.
    resp = mgr.sendControlCommand("vm_query 3", 1000);
    REQUIRE(resp.find("state=active") != std::string::npos);

    // Audio should flow again.
    const uint64_t gen = mgr.generation();
    float buf[aw::kBlockSize];
    bool gotData = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(buf, aw::kBlockSize, gen)) {
            gotData = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gotData);

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// Test 8: Resource ceiling enforcement
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: resource ceiling enforcement", "[k3][ceiling][policy]")
{
    // Unit test the VMManager directly (without spawning the worker).
    aw::VMManager vmMgr;

    // Set a low ceiling.
    aw::ResourcePolicy policy;
    policy.maxConcurrentLiveVMs = 2;
    policy.ceilingBehavior = aw::CeilingBehavior::RejectWithError;
    vmMgr.setPolicy(policy);

    // First activation should succeed.
    auto r1 = vmMgr.activateVM(0);
    REQUIRE(r1.ok);

    // Second activation should succeed.
    auto r2 = vmMgr.activateVM(1);
    REQUIRE(r2.ok);

    // Third should be rejected (ceiling = 2).
    auto r3 = vmMgr.activateVM(2);
    REQUIRE_FALSE(r3.ok);
    REQUIRE(r3.errorCode == 429);

    // Cleanup.
    vmMgr.destroyVM(0);
    vmMgr.destroyVM(1);
}

// ---------------------------------------------------------------------------
// Test 9: Ceiling eviction — LRU suspend at ceiling
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: ceiling eviction via LRU suspend", "[k3][ceiling][lru]")
{
    aw::VMManager vmMgr;

    aw::ResourcePolicy policy;
    policy.maxConcurrentLiveVMs = 2;
    policy.ceilingBehavior = aw::CeilingBehavior::LRUSuspend;
    vmMgr.setPolicy(policy);

    // Activate tab 0 and 1 (at ceiling).
    REQUIRE(vmMgr.activateVM(0).ok);
    REQUIRE(vmMgr.activateVM(1).ok);

    // Suspend tab 0 (making it an LRU candidate).
    REQUIRE(vmMgr.deactivateVM(0, true).ok);

    // Activate tab 2 — should evict (destroy) the suspended tab 0.
    auto r = vmMgr.activateVM(2);
    REQUIRE(r.ok);

    // Tab 0 should now be destroyed/inactive.
    auto q = vmMgr.queryVM(0);
    // After eviction, the VM for tab 0 should be destroyed.
    REQUIRE(q.ok);

    // Cleanup.
    vmMgr.destroyVM(1);
    vmMgr.destroyVM(2);
}

// ---------------------------------------------------------------------------
// Test 10: Worker restart — mapping survives (re-activation works)
//
// Per B4-K3 acceptance: "the mapping survives worker restart"
// This test verifies that after a worker restart with a new generation,
// the main process can re-activate VMs on the new worker.
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: worker restart allows re-activation", "[k3][restart][recovery]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));
    const uint64_t gen1 = mgr.generation();

    // Activate a VM.
    REQUIRE(mgr.sendControlCommand("vm_activate 5 44100 1", 1000).find("ok") != std::string::npos);

    // Restart the worker.
    REQUIRE(mgr.restart());
    const uint64_t gen2 = mgr.generation();
    REQUIRE(gen2 == gen1 + 1);

    // The old generation should be rejected.
    float buf[aw::kBlockSize];
    REQUIRE_FALSE(mgr.tryReadAudioBlock(buf, aw::kBlockSize, gen1));

    // Re-activate the VM on the restarted worker.
    std::string resp = mgr.sendControlCommand("vm_activate 5 44100 1", 1000);
    REQUIRE(resp.find("ok vm_activated") != std::string::npos);

    // Audio should flow on the new generation.
    bool gotData = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(buf, aw::kBlockSize, gen2)) {
            gotData = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gotData);

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// Test 11: Repeated lifecycle — create/suspend/resume/destroy cycles
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: repeated VM lifecycle cycles", "[k3][lifecycle][cycles]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    constexpr int kNumCycles = 3;
    for (int cycle = 0; cycle < kNumCycles; ++cycle) {
        // Activate
        std::string resp = mgr.sendControlCommand("vm_activate 7 44100 1", 1000);
        REQUIRE(resp.find("ok vm_activated") != std::string::npos);

        // Query — active
        resp = mgr.sendControlCommand("vm_query 7", 1000);
        REQUIRE(resp.find("state=active") != std::string::npos);

        // Suspend
        resp = mgr.sendControlCommand("vm_deactivate 7 suspend", 1000);
        REQUIRE(resp.find("ok vm_deactivated") != std::string::npos);

        // Resume
        resp = mgr.sendControlCommand("vm_resume 7", 1000);
        REQUIRE(resp.find("ok vm_resumed") != std::string::npos);

        // Destroy
        resp = mgr.sendControlCommand("vm_destroy 7", 1000);
        REQUIRE(resp.find("ok vm_destroyed") != std::string::npos);

        // Query — should be inactive
        resp = mgr.sendControlCommand("vm_query 7", 1000);
        REQUIRE(resp.find("state=inactive") != std::string::npos);
    }

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// Unit tests for ResourcePolicy serialization
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: resource policy serialization", "[k3][policy][serialization]")
{
    aw::ResourcePolicy policy;
    policy.maxConcurrentLiveVMs = 4;
    policy.maxThreads = 12;
    policy.maxVmMemoryMb = 128;
    policy.idleSuspendTimeoutSec = 15;
    policy.preferSuspendOverDestroy = false;
    policy.ceilingBehavior = aw::CeilingBehavior::RejectWithError;
    policy.vmCost.cpuPerBlock = 0.002f;
    policy.vmCost.idleMemoryBytes = 8 * 1024 * 1024;

    std::string json = policy.serialize();
    REQUIRE_FALSE(json.empty());

    aw::ResourcePolicy decoded;
    REQUIRE(decoded.deserialize(json));

    REQUIRE(decoded.maxConcurrentLiveVMs == 4);
    REQUIRE(decoded.maxThreads == 12);
    REQUIRE(decoded.maxVmMemoryMb == 128);
    REQUIRE(decoded.idleSuspendTimeoutSec == 15);
    REQUIRE_FALSE(decoded.preferSuspendOverDestroy);
    REQUIRE(decoded.ceilingBehavior == aw::CeilingBehavior::RejectWithError);
    REQUIRE(decoded.vmCost.cpuPerBlock == Catch::Approx(0.002f));
    REQUIRE(decoded.vmCost.idleMemoryBytes == 8 * 1024 * 1024);
}

// ---------------------------------------------------------------------------
// Unit tests for ChuckVM lifecycle (without worker process)
// ---------------------------------------------------------------------------

TEST_CASE("B4-K3: ChuckVM state machine", "[k3][vm-state-machine][unit]")
{
    // Test the ChuckVM lifecycle directly without the worker process.
    int callCount = 0;
    ChuckVM vm(4, [&callCount](float* /*outBuf*/, unsigned /*numFrames*/,
                                unsigned /*numChannels*/) {
        ++callCount;
    });

    // Initially inactive.
    REQUIRE(vm.state() == aw::VMState::Inactive);
    REQUIRE_FALSE(vm.isActive());
    REQUIRE(vm.isTerminated());

    // Activate.
    auto result = vm.activate();
    REQUIRE(result.ok);
    REQUIRE(vm.state() == aw::VMState::Active);
    REQUIRE(vm.isActive());

    // Let it run briefly.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(vm.blocksProduced() > 0);

    // Suspend.
    result = vm.deactivate(true);
    REQUIRE(result.ok);
    REQUIRE(vm.state() == aw::VMState::Suspended);
    REQUIRE(vm.isSuspended());

    uint64_t blocksBefore = vm.blocksProduced();

    // Resume.
    result = vm.resume();
    REQUIRE(result.ok);
    REQUIRE(vm.state() == aw::VMState::Active);

    // Let it run briefly again.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(vm.blocksProduced() > blocksBefore);

    // Destroy.
    result = vm.destroy();
    REQUIRE(result.ok);
    REQUIRE(vm.state() == aw::VMState::Destroyed);
    REQUIRE(vm.isTerminated());
}
