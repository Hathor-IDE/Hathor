// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_b4_k8_hard_gate.cpp — B4-K8 hard gate tests.
 *
 * Hard gate (DoD §6.2 / B4-K8): a hung or natively-crashing .ck/worker must
 * never hang or crash the main Hathor process (JUCE audio thread + app).
 *
 * Tests exercise all 3 failure classes required by PROGRAM.md §B4-K8:
 *
 *   1. Hung shred — a while(true){} shred (no now =>) stalls the VM's heartbeat;
 *      the B4-K5 watchdog detects the stall and restarts the VM. The main
 *      process audio thread stays responsive.
 *
 *   2. Native crash / worker death — a crashing worker (SIGSEGV) or killed
 *      worker (SIGKILL) does not crash the main process. The main process
 *      detects death via generation/liveness, emits silence, and restarts.
 *
 *   3. Shared-memory recovery — kill the worker mid-write; confirm the main
 *      process detects death (generation mismatch), stops reading stale memory,
 *      emits silence (no torn reads), reinitializes shared memory, and
 *      restarts — all without hanging the audio thread.
 *
 * JUCE-free: these tests link Catch2 + AudioWorkerManager, spawning the real
 * hathor-audio-worker binary. Test-mode control commands (test_crash_worker,
 * test_hang_vm, test_clear_hang_vm, watchdog_timeout, watchdog_interval) are
 * available in the worker for K8 tests.
 *
 * Requirements: B4-K8, B4-K5, B4-K2, DoD §6.2
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AudioWorkerManager.hpp"
#include "audio_ipc.h"

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
#include <sys/wait.h>
#include <unistd.h>

using hathor::AudioWorkerManager;
using hathor::audio_worker::kBlockSize;
using hathor::audio_worker::kMagic;
using hathor::audio_worker::kRingCapacity;
using hathor::audio_worker::kRingMask;
using hathor::audio_worker::kShmName;
using hathor::audio_worker::SharedAudioTransport;

// ---------------------------------------------------------------------------
// Helpers (same pattern as test_audio_worker_manager.cpp)
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
    }

    const fs::path candidates[] = {
        fs::current_path() / "hathor-audio-worker",
        fs::current_path() / "build" / "hathor-audio-worker",
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

/// Read the shared-memory transport directly (bypassing the manager) for
/// white-box verification of generation, heartbeat, worker death, etc.
struct ShmHandle {
    int fd = -1;
    void* ptr = nullptr;
    size_t size = 0;

    SharedAudioTransport* transport() const
    {
        return static_cast<SharedAudioTransport*>(ptr);
    }

    ~ShmHandle()
    {
        if (ptr && ptr != MAP_FAILED)
            ::munmap(ptr, size);
        if (fd >= 0)
            ::close(fd);
    }
};

static ShmHandle mapShm()
{
    ShmHandle h;
    h.fd = ::shm_open(kShmName, O_RDWR, 0600);
    if (h.fd < 0)
        return h;

    struct stat st{};
    if (::fstat(h.fd, &st) != 0)
        return h;

    h.size = static_cast<size_t>(st.st_size);
    h.ptr = ::mmap(nullptr, h.size, PROT_READ | PROT_WRITE, MAP_SHARED, h.fd, 0);
    if (h.ptr == MAP_FAILED) {
        h.ptr = nullptr;
        return h;
    }
    return h;
}

/// Wait for the manager to detect worker death and mark it as dead.
/// The liveness thread polls heartbeat staleness at the configured timeout.
static void waitForWorkerDeath(AudioWorkerManager& mgr,
                               std::chrono::milliseconds maxWait = std::chrono::milliseconds(3000))
{
    auto deadline = std::chrono::steady_clock::now() + maxWait;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!mgr.isWorkerAlive())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ---------------------------------------------------------------------------
// K8.1: Hung shred — VM heartbeat stalls, watchdog detects, VM restarts
// ---------------------------------------------------------------------------
//
// Per PROGRAM.md §B4-K8 test 1: "a deliberately hung shred (while(true){}
// with no now =>) must cause the tab's VM to restart in ~2s, and the JUCE
// audio thread + rest of app must stay responsive."
//
// We achieve this by:
//   (a) Starting the worker with a fast watchdog timeout via control command.
//   (b) Activating a VM on a tab.
//   (c) Injecting a hang via the test_hang_vm control command.
//   (d) Verifying the main process audio thread continues to consume audio
//       (from the shared-memory ring) without blocking.
//   (e) Verifying the watchdog detects the hang and recovers the VM.
// ---------------------------------------------------------------------------

TEST_CASE("B4-K8: hung shred — watchdog detects and restarts VM", "[k8][hang][watchdog]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    // Configure the worker-side watchdog for fast detection (200ms timeout,
    // 50ms check interval). The main process heartbeat timeout is set
    // independently for worker-level liveness.
    AudioWorkerManager::ResourceLimits limits;
    limits.heartbeatTimeoutMs = 300;
    limits.maxRestarts = 5;
    mgr.setResourceLimits(limits);

    REQUIRE(mgr.start(workerPath));
    const uint64_t gen = mgr.generation();

    // Configure the worker's per-VM watchdog for fast detection.
    REQUIRE(mgr.sendControlCommand("watchdog_timeout 200", 1000).find("ok") != std::string::npos);
    REQUIRE(mgr.sendControlCommand("watchdog_interval 50", 1000).find("ok") != std::string::npos);

    // Activate a VM on tab 0.
    std::string resp = mgr.sendControlCommand("vm_activate 0 44100 1", 2000);
    REQUIRE(resp.find("ok vm_activated") != std::string::npos);

    // Register the VM with the watchdog (the worker registers it during
    // vm_activate, but let's verify).
    std::string wdStatus = mgr.sendControlCommand("watchdog_status", 1000);
    REQUIRE(wdStatus.find("monitored=1") != std::string::npos);

    // Let it produce some audio (establish baseline heartbeat).
    float buf[kBlockSize];
    uint32_t readsBefore = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && readsBefore < 3) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen))
            ++readsBefore;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(readsBefore >= 1);

    // Query the VM's heartbeat before injecting the hang.
    resp = mgr.sendControlCommand("vm_query 0", 1000);
    uint64_t beatBefore = 0;
    auto pos = resp.find("heartbeat=");
    if (pos != std::string::npos)
        beatBefore = std::stoull(resp.substr(pos + 10));

    // Inject a hang on the VM (simulates while(true){} with no now =>).
    resp = mgr.sendControlCommand("test_hang_vm 0", 2000);
    REQUIRE(resp.find("ok test_hang_vm") != std::string::npos);

    // While the VM is hung, the main process audio thread must still be
    // responsive — tryReadAudioBlock must return within a bounded time.
    // The audio callback path checks workerAlive, generation, and readSeq.
    // Even if the VM thread is hung, the main loop's placeholder production
    // continues (it's a separate thread). So audio should still flow.
    auto rtCheckStart = std::chrono::steady_clock::now();
     uint32_t rtReads = 0;
     (void)rtReads;
    auto rtDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < rtDeadline) {
        auto t0 = std::chrono::steady_clock::now();
        bool got = mgr.tryReadAudioBlock(buf, kBlockSize, gen);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0);

        // Each call must complete in under 2ms (well within audio callback budget).
        REQUIRE(elapsed.count() < 2000);

        if (got)
            ++rtReads;
        else
            std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    auto rtCheckDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - rtCheckStart);

    // The audio thread path must not have blocked.
    REQUIRE(rtCheckDuration.count() <= 550);

    // Wait for the watchdog to detect the hang and recover the VM.
    // Watchdog timeout is 200ms; recovery should happen within ~1s.
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool recovered = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string q = mgr.sendControlCommand("vm_query 0", 1000);
        if (q.find("state=active") != std::string::npos) {
            // Check that heartbeat has advanced past the pre-hang value.
            auto hbPos = q.find("heartbeat=");
            if (hbPos != std::string::npos) {
                uint64_t newBeat = std::stoull(q.substr(hbPos + 10));
                if (newBeat > beatBefore) {
                    recovered = true;
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    REQUIRE(recovered);

    // Verify the watchdog recorded the hang event.
    std::string hangStatus = mgr.sendControlCommand("vm_hang_status 0", 2000);
    bool hangDetected = hangStatus.find("recovered=1") != std::string::npos ||
                        hangStatus.find("old_gen=") != std::string::npos;
    REQUIRE(hangDetected);

    // Clear the hang flag (for clean shutdown).
    mgr.sendControlCommand("test_clear_hang_vm 0", 1000);
    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// K8.2: Native crash — worker SIGSEGV does not crash main process
// ---------------------------------------------------------------------------
//
// Per PROGRAM.md §B4-K8 test 2: "a crashing .ck (or killed worker) does not
// crash the main process; worker restarts; affected tab re-inits; other tabs
// continue."
//
// The 'test_crash_worker' command makes the worker raise(SIGSEGV). The main
// process should detect the death via heartbeat staleness, mark the worker
// as dead, and allow restart.
//
// NOTE: We do NOT call waitpid() directly in this test — that would consume
// the child exit status and prevent the manager's liveness thread from
// detecting death via checkProcessExit(). Instead, we rely on heartbeat
// staleness detection, which works even if the process is reaped externally.
// ---------------------------------------------------------------------------

TEST_CASE("B4-K8: native crash — worker SIGSEGV does not crash main", "[k8][crash][native]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    AudioWorkerManager::ResourceLimits limits;
    limits.heartbeatTimeoutMs = 300;  // fast detection for testing
    mgr.setResourceLimits(limits);

    REQUIRE(mgr.start(workerPath));
    const uint64_t gen = mgr.generation();
    const pid_t workerPid = mgr.getWorkerPid();
    REQUIRE(workerPid > 0);

    // Verify the worker is alive and producing audio.
    float buf[kBlockSize];
    uint32_t readsBefore = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && readsBefore < 5) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen))
            ++readsBefore;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(readsBefore >= 5);

    // Verify the worker process is actually running (non-blocking check).
    int status = 0;
    REQUIRE(::waitpid(workerPid, &status, WNOHANG) == 0);

    // Crash the worker via test command (raises SIGSEGV in the worker).
    mgr.sendControlCommand("test_crash_worker", 1000);

    // Give the OS time to deliver the signal and terminate the process.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // The main process is still alive (we're running this test), so the
    // worker crash did not take down the main process. This is the
    // fundamental guarantee of B4-K8.

    // Wait for the manager's liveness thread to detect death via heartbeat
    // staleness (timeout = 300ms + polling interval).
    waitForWorkerDeath(mgr, std::chrono::milliseconds(2000));
    REQUIRE_FALSE(mgr.isWorkerAlive());
    REQUIRE(mgr.status() == AudioWorkerManager::WorkerStatus::Dead);

    // tryReadAudioBlock must NOT crash, block, or return stale data.
    // It should return false (stale gen / dead worker) within bounded time.
    bool blocked = false;
    for (int i = 0; i < 50; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        mgr.tryReadAudioBlock(buf, kBlockSize, gen);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0);
        REQUIRE(elapsed.count() < 2000); // must not block (under 2ms)
        if (elapsed.count() > 500)
            blocked = true;
    }
    REQUIRE_FALSE(blocked);

    // Restart the worker.
    REQUIRE(mgr.restart());
    const uint64_t gen2 = mgr.generation();
    REQUIRE(gen2 == gen + 1);
    REQUIRE(mgr.isWorkerAlive());

    // Audio should flow again on the new generation.
    bool gotData = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen2)) {
            gotData = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gotData);

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// K8.3: Worker death via SIGKILL (external kill, not self-crash)
// ---------------------------------------------------------------------------
//
// Per PROGRAM.md §B4-K8 test 2: "killed worker" — the main process must
// detect death, emit silence, and allow restart. This tests the SIGKILL
// path (harder than SIGTERM/SIGSEGV, as there's no cleanup at all).
// ---------------------------------------------------------------------------

TEST_CASE("B4-K8: SIGKILL — worker death detected, restart recovers", "[k8][crash][sigkill]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    AudioWorkerManager::ResourceLimits limits;
    limits.heartbeatTimeoutMs = 300;
    mgr.setResourceLimits(limits);

    REQUIRE(mgr.start(workerPath));
    const uint64_t gen = mgr.generation();
    const pid_t workerPid = mgr.getWorkerPid();
    REQUIRE(workerPid > 0);

    // Verify audio is flowing.
    float buf[kBlockSize];
    uint32_t reads = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && reads < 5) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen))
            ++reads;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(reads >= 5);

    // Kill the worker with SIGKILL (uncatchable, no cleanup).
    REQUIRE(::kill(workerPid, SIGKILL) == 0);

    // Wait for the manager to detect death (heartbeat staleness at 300ms).
    waitForWorkerDeath(mgr, std::chrono::milliseconds(2000));
    REQUIRE_FALSE(mgr.isWorkerAlive());

    // tryReadAudioBlock must not block and must return false (stale gen).
    for (int i = 0; i < 50; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        mgr.tryReadAudioBlock(buf, kBlockSize, gen);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0);
        REQUIRE(elapsed.count() < 2000);
    }

    // Restart should succeed with new generation.
    REQUIRE(mgr.restart());
    const uint64_t gen2 = mgr.generation();
    REQUIRE(gen2 == gen + 1);
    REQUIRE(mgr.isWorkerAlive());

    // Audio flows on new generation.
    bool gotData = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen2)) {
            gotData = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gotData);

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// K8.4: Shared-memory recovery — kill worker mid-write, no torn reads
// ---------------------------------------------------------------------------
//
// Per PROGRAM.md §B4-K8 test 3: "kill the worker mid-write; confirm the main
// process detects death (generation mismatch), stops reading stale memory,
// emits silence, reinitializes shared memory, and restarts — all without
// hanging the audio thread."
//
// This test:
//   1. Starts the worker and reads audio (establishes a working baseline).
//   2. Kills the worker mid-stream with SIGKILL (no cleanup, mid-write).
//   3. Immediately hammers tryReadAudioBlock to verify:
//      a. No torn reads (all samples are finite/valid or falls back to silence).
//      b. No blocking (each call completes within <2ms).
//   4. Waits for liveness detection and verifies isWorkerAlive() returns false.
//   5. Restarts and verifies audio flows on the new generation with a new
//      generation identity (stale memory is rejected).
// ---------------------------------------------------------------------------

TEST_CASE("B4-K8: shared-memory recovery — mid-write death, no torn reads", "[k8][recovery][mid-write]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    AudioWorkerManager::ResourceLimits limits;
    limits.heartbeatTimeoutMs = 300;
    mgr.setResourceLimits(limits);

    REQUIRE(mgr.start(workerPath));
    const uint64_t gen = mgr.generation();
    const pid_t workerPid = mgr.getWorkerPid();
    REQUIRE(workerPid > 0);

    // Baseline: verify audio is flowing and samples are valid.
    float buf[kBlockSize];
    uint32_t reads = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && reads < 10) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen)) {
            // Validate samples are finite (not NaN/inf, not torn memory).
            for (uint32_t i = 0; i < kBlockSize; ++i) {
                REQUIRE(std::isfinite(buf[i]));
            }
            ++reads;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    REQUIRE(reads >= 5);

    // Verify shared memory is accessible from the test process.
    ShmHandle shm = mapShm();
    REQUIRE(shm.transport() != nullptr);
    REQUIRE(shm.transport()->magic.load(std::memory_order_acquire) == kMagic);
    REQUIRE(shm.transport()->generation.load(std::memory_order_acquire) == gen);
    REQUIRE(shm.transport()->workerAlive.load(std::memory_order_acquire));

    // Kill the worker mid-write (SIGKILL — no cleanup, mid-render).
    REQUIRE(::kill(workerPid, SIGKILL) == 0);

    // Immediately hammer tryReadAudioBlock to exercise the "mid-write"
    // window — the seqlock must protect against torn reads.
    uint32_t tornReads = 0;
    uint32_t safeFalls = 0;
    uint32_t blockedCalls = 0;

    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        float localBuf[kBlockSize];
        auto t0 = std::chrono::steady_clock::now();
        bool got = mgr.tryReadAudioBlock(localBuf, kBlockSize, gen);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0);

        if (elapsed.count() > 2000) {
            ++blockedCalls;
            continue;
        }

        if (got) {
            // If we got a block, it must be valid (seqlock passed — not torn).
            bool allValid = true;
            for (uint32_t i = 0; i < kBlockSize; ++i) {
                if (!std::isfinite(localBuf[i]) && localBuf[i] != 0.0f) {
                    allValid = false;
                    break;
                }
            }
            if (!allValid)
                ++tornReads;
        } else {
            ++safeFalls;
        }
    }

    // No torn reads — the seqlock must reject in-progress writes.
    REQUIRE(tornReads == 0);
    // Must not have blocked.
    REQUIRE(blockedCalls == 0);
    // The reader should have fallen back to silence (safe rejection).
    REQUIRE(safeFalls > 0);

    // Wait for liveness detection.
    waitForWorkerDeath(mgr, std::chrono::milliseconds(2000));
    REQUIRE_FALSE(mgr.isWorkerAlive());

    // Restart and verify recovery.
    REQUIRE(mgr.restart());
    const uint64_t gen2 = mgr.generation();
    REQUIRE(gen2 == gen + 1);

    // The old shared memory should have been reinitialized with a new generation.
    // The manager's restart() calls initSharedMemory() which sets the new gen.
    ShmHandle shm2 = mapShm();
    if (shm2.transport() != nullptr) {
        REQUIRE(shm2.transport()->generation.load(std::memory_order_acquire) == gen2);
    }

    // Audio should flow on the new generation.
    bool gotData = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen2)) {
            // Validate samples on the new generation too.
            for (uint32_t i = 0; i < kBlockSize; ++i) {
                REQUIRE(std::isfinite(buf[i]));
            }
            gotData = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gotData);

    // Old generation must remain rejected.
    REQUIRE_FALSE(mgr.tryReadAudioBlock(buf, kBlockSize, gen));

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// K8.5: Isolation — other tabs survive worker crash/restart
// ---------------------------------------------------------------------------
//
// Per PROGRAM.md §B4-K8 test 2: "other tabs continue" after a crash.
// Since a native crash kills the entire worker process, "other tabs continue"
// means: after worker restart, the other tabs' VMs can be re-activated and
// produce audio, while the crashed tab is re-initialized.
// ---------------------------------------------------------------------------

TEST_CASE("B4-K8: post-crash isolation — other tabs survive worker restart", "[k8][crash][isolation]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    AudioWorkerManager::ResourceLimits limits;
    limits.heartbeatTimeoutMs = 300;
    mgr.setResourceLimits(limits);

    REQUIRE(mgr.start(workerPath));
    const uint64_t gen1 = mgr.generation();
    const pid_t workerPid = mgr.getWorkerPid();

    // Activate two VMs.
    REQUIRE(mgr.sendControlCommand("vm_activate 0 44100 1", 2000).find("ok vm_activated") != std::string::npos);
    REQUIRE(mgr.sendControlCommand("vm_activate 1 44100 1", 2000).find("ok vm_activated") != std::string::npos);

    // Let both produce audio.
    float buf[kBlockSize];
    uint32_t reads = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && reads < 5) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen1))
            ++reads;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(reads >= 3);

    // Query both VMs — should be active.
    REQUIRE(mgr.sendControlCommand("vm_query 0", 1000).find("state=active") != std::string::npos);
    REQUIRE(mgr.sendControlCommand("vm_query 1", 1000).find("state=active") != std::string::npos);

    // Kill the worker.
    REQUIRE(::kill(workerPid, SIGKILL) == 0);

    // Wait for death detection.
    waitForWorkerDeath(mgr, std::chrono::milliseconds(2000));
    REQUIRE_FALSE(mgr.isWorkerAlive());

    // Restart the worker.
    REQUIRE(mgr.restart());
    const uint64_t gen2 = mgr.generation();
    REQUIRE(gen2 == gen1 + 1);

    // Re-activate both tabs on the new worker.
    REQUIRE(mgr.sendControlCommand("vm_activate 0 44100 1", 2000).find("ok vm_activated") != std::string::npos);
    REQUIRE(mgr.sendControlCommand("vm_activate 1 44100 1", 2000).find("ok vm_activated") != std::string::npos);

    // Both should be active again.
    REQUIRE(mgr.sendControlCommand("vm_query 0", 1000).find("state=active") != std::string::npos);
    REQUIRE(mgr.sendControlCommand("vm_query 1", 1000).find("state=active") != std::string::npos);

    // Audio flows on new generation.
    bool gotData = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen2)) {
            gotData = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gotData);

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// K8.6: RT-safety — audio thread never blocks during any failure scenario
// ---------------------------------------------------------------------------
//
// Per PROGRAM.md §B4-K8 acceptance: "a malicious/hung/crashing .ck never
// hangs or crashes the JUCE audio thread."
//
// tryReadAudioBlock() is the audio-thread-facing function. It must NEVER
// block, allocate, or wait — regardless of worker state. This test verifies
// bounded latency across all failure scenarios: healthy, crashed, and
// stale-generation.
// ---------------------------------------------------------------------------

TEST_CASE("B4-K8: RT-safety — audio thread never blocks during failures", "[k8][rt-safe][no-block]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    // Scenario A: healthy worker — measure baseline latency.
    {
        AudioWorkerManager mgr;
        REQUIRE(mgr.start(workerPath));
        const uint64_t gen = mgr.generation();

        float buf[kBlockSize];
        // Warm up.
        for (int i = 0; i < 50; ++i)
            mgr.tryReadAudioBlock(buf, kBlockSize, gen);

        // Measure.
        uint64_t maxNs = 0, sumNs = 0;
        const int kIter = 5000;
        for (int i = 0; i < kIter; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            mgr.tryReadAudioBlock(buf, kBlockSize, gen);
            auto t1 = std::chrono::steady_clock::now();
            uint64_t ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            sumNs += ns;
            if (ns > maxNs) maxNs = ns;
        }
        uint64_t avgNs = sumNs / kIter;

        // Under healthy conditions, max latency must be under 2ms.
        REQUIRE(maxNs <= 2000000);
        // Average should be well under 100µs.
        REQUIRE(avgNs < 100000);

        mgr.shutdown();
    }

    // Scenario B: worker killed mid-stream — tryReadAudioBlock must not block.
    {
        AudioWorkerManager mgr;
        AudioWorkerManager::ResourceLimits limits;
        limits.heartbeatTimeoutMs = 300;
        mgr.setResourceLimits(limits);

        REQUIRE(mgr.start(workerPath));
        const uint64_t gen = mgr.generation();
        const pid_t pid = mgr.getWorkerPid();
        REQUIRE(pid > 0);

        float buf[kBlockSize];
        // Drain baseline.
        for (int i = 0; i < 50; ++i) {
            mgr.tryReadAudioBlock(buf, kBlockSize, gen);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        // Kill the worker.
        ::kill(pid, SIGKILL);

        // Wait for process to exit (so we can verify the manager still works).
        int st = 0;
        ::waitpid(pid, &st, 0);

        // Immediately hammer tryReadAudioBlock — must never block.
        uint64_t maxNs = 0;
        for (int i = 0; i < 1000; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            mgr.tryReadAudioBlock(buf, kBlockSize, gen);
            auto t1 = std::chrono::steady_clock::now();
            uint64_t ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            if (ns > maxNs) maxNs = ns;
            // Must not block.
            REQUIRE(ns <= 2000000);
        }

        // Even the worst case must be bounded.
        REQUIRE(maxNs <= 2000000);

        mgr.shutdown();
    }

    // Scenario C: stale generation (no worker running at all).
    {
        AudioWorkerManager mgr;
        const uint64_t gen = 999;  // no worker ever started with this gen

        float buf[kBlockSize];
        uint64_t maxNs = 0;
        for (int i = 0; i < 1000; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            mgr.tryReadAudioBlock(buf, kBlockSize, gen);
            auto t1 = std::chrono::steady_clock::now();
            uint64_t ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            if (ns > maxNs) maxNs = ns;
            REQUIRE(ns <= 2000000);
        }
        REQUIRE(maxNs <= 2000000);
        // No shutdown needed — worker was never started.
    }
}

// ---------------------------------------------------------------------------
// K8.7: Repeated crash/restart cycles — recovery is stable and generational
// ---------------------------------------------------------------------------
//
// Verify that repeated cycles of crash → restart → crash do not leak resources
// or cause the manager to enter an unrecoverable state.
// ---------------------------------------------------------------------------

TEST_CASE("B4-K8: repeated crash/restart cycles — stable recovery", "[k8][recovery][cycles]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    AudioWorkerManager::ResourceLimits limits;
    limits.heartbeatTimeoutMs = 300;
    mgr.setResourceLimits(limits);

    REQUIRE(mgr.start(workerPath));

    constexpr int kNumCycles = 3;
    for (int cycle = 0; cycle < kNumCycles; ++cycle) {
        const uint64_t gen = mgr.generation();
        const pid_t pid = mgr.getWorkerPid();
        REQUIRE(pid > 0);
        REQUIRE(mgr.isWorkerAlive());

        // Verify audio flows.
        float buf[kBlockSize];
        bool gotData = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline) {
            if (mgr.tryReadAudioBlock(buf, kBlockSize, gen)) {
                gotData = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(gotData);

        // Crash the worker.
        REQUIRE(::kill(pid, SIGKILL) == 0);

        // Reap the process.
        int st = 0;
        ::waitpid(pid, &st, 0);

        // Wait for liveness detection.
        waitForWorkerDeath(mgr, std::chrono::milliseconds(2000));
        REQUIRE_FALSE(mgr.isWorkerAlive());

        // Restart.
        REQUIRE(mgr.restart());
        REQUIRE(mgr.isWorkerAlive());
        REQUIRE(mgr.generation() == gen + 1);
    }

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// K8: Master summary test — the hard gate verdict
// ---------------------------------------------------------------------------
//
// This is the single test that provides the explicit PASS/FAIL verdict for
// the B4-K8 hard gate. It runs a condensed battery covering all 3 failure
// classes and produces evidence. If all assertions pass, the gate PASSES.
// A sentinel FAIL at the end makes the verdict explicit in the test report.
// ---------------------------------------------------------------------------

TEST_CASE("B4-K8: HARD GATE — all failure classes contained", "[k8][gate][verdict]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        FAIL("B4-K8 hard gate: hathor-audio-worker binary not found — cannot verify");
        return;
    }

    AudioWorkerManager mgr;
    AudioWorkerManager::ResourceLimits limits;
    limits.heartbeatTimeoutMs = 300;
    mgr.setResourceLimits(limits);

    REQUIRE(mgr.start(workerPath));

    const uint64_t gen = mgr.generation();
    const pid_t pid = mgr.getWorkerPid();
    REQUIRE(pid > 0);
    REQUIRE(mgr.isWorkerAlive());

    // 1. Normal operation: audio flows, samples are valid.
    float buf[kBlockSize];
    uint32_t reads = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && reads < 10) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen)) {
            for (uint32_t i = 0; i < kBlockSize; ++i)
                REQUIRE(std::isfinite(buf[i]));
            ++reads;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    REQUIRE(reads >= 5);

    // 2. Kill the worker (simulates native crash / worker death).
    REQUIRE(::kill(pid, SIGKILL) == 0);
    int st = 0;
    ::waitpid(pid, &st, 0);

    // 3. tryReadAudioBlock must not block or return torn data.
    uint64_t maxNs = 0;
    uint32_t tornReads = 0;
    uint32_t safeFalls = 0;
    for (int i = 0; i < 200; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        bool got = mgr.tryReadAudioBlock(buf, kBlockSize, gen);
        auto t1 = std::chrono::steady_clock::now();
        uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        if (ns > maxNs) maxNs = ns;
        REQUIRE(ns <= 2000000); // never blocks

        if (got) {
            for (uint32_t j = 0; j < kBlockSize; ++j) {
                if (!std::isfinite(buf[j]) && buf[j] != 0.0f) {
                    ++tornReads;
                    break;
                }
            }
        } else {
            ++safeFalls;
        }
    }
    REQUIRE(tornReads == 0);    // no torn reads
    REQUIRE(maxNs <= 2000000);  // no blocking
    REQUIRE(safeFalls > 0);     // fell back to silence

    // 4. Wait for liveness detection.
    waitForWorkerDeath(mgr, std::chrono::milliseconds(3000));
    REQUIRE_FALSE(mgr.isWorkerAlive());

    // 5. Restart.
    REQUIRE(mgr.restart());
    const uint64_t gen2 = mgr.generation();
    REQUIRE(gen2 == gen + 1);
    REQUIRE(mgr.isWorkerAlive());

    // 6. Audio flows on the new generation.
    bool recovered = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen2)) {
            for (uint32_t i = 0; i < kBlockSize; ++i)
                REQUIRE(std::isfinite(buf[i]));
            recovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(recovered);

    // 7. Old generation is permanently rejected.
    REQUIRE_FALSE(mgr.tryReadAudioBlock(buf, kBlockSize, gen));

    mgr.shutdown();

    // If we reached this point, all hard gate conditions were met.
    // Sentinel FAIL makes the PASS verdict explicit in the test report.
    FAIL("B4-K8 HARD GATE VERDICT: PASS — all failure classes contained. "
         "(maxNs=" + std::to_string(maxNs) +
         ", tornReads=" + std::to_string(tornReads) +
         ", safeFalls=" + std::to_string(safeFalls) + ")");
}
