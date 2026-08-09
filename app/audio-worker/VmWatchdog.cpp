// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * VmWatchdog.cpp — per-VM hang detection and recovery implementation (B4-K5).
 *
 * See VmWatchdog.hpp for the full architectural documentation.
 */

#include "VmWatchdog.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace hathor::audio_worker {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

VmWatchdog::VmWatchdog(VMManager* vmManager,
                        VmLifecycle* vmLifecycle,
                        HangDetectedCallback onHangDetected,
                        RecoveryCompleteCallback onRecoveryComplete)
    : vmManager_(vmManager)
    , vmLifecycle_(vmLifecycle)
    , onHangDetected_(std::move(onHangDetected))
    , onRecoveryComplete_(std::move(onRecoveryComplete))
    , timeout_(kDefaultHeartbeatTimeoutMs)
    , interval_(kDefaultWatchdogIntervalMs)
{
}

VmWatchdog::~VmWatchdog()
{
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void VmWatchdog::start()
{
    running_.store(true, std::memory_order_release);
    watchdogThread_ = std::thread(&VmWatchdog::watchdogLoop, this);
}

void VmWatchdog::stop()
{
    running_.store(false, std::memory_order_release);
    if (watchdogThread_.joinable())
        watchdogThread_.join();
}

// ---------------------------------------------------------------------------
// VM registration / unregistration
// ---------------------------------------------------------------------------

void VmWatchdog::registerVM(TabId tabId, uint64_t generation)
{
    std::lock_guard<std::mutex> lock(entriesMtx_);
    auto& entry = entries_[tabId];
    if (!entry) {
        entry = std::make_unique<WatchdogEntry>(tabId);
    }
    entry->trackedGeneration.store(generation, std::memory_order_release);
    entry->lastHeartbeat.store(0, std::memory_order_release);
    entry->lastProgress = std::chrono::steady_clock::now();
    entry->restartCount.store(0, std::memory_order_release);
    entry->lastRestartNs.store(0, std::memory_order_release);
}

void VmWatchdog::unregisterVM(TabId tabId)
{
    std::lock_guard<std::mutex> lock(entriesMtx_);
    entries_.erase(tabId);
}

void VmWatchdog::resetHeartbeat(TabId tabId)
{
    std::lock_guard<std::mutex> lock(entriesMtx_);
    auto it = entries_.find(tabId);
    if (it != entries_.end() && it->second) {
        it->second->lastProgress = std::chrono::steady_clock::now();
        it->second->lastHeartbeat.store(0, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void VmWatchdog::setTimeout(std::chrono::milliseconds timeout) noexcept
{
    timeout_ = timeout;
}

void VmWatchdog::setInterval(std::chrono::milliseconds interval) noexcept
{
    interval_ = interval;
}

int VmWatchdog::monitoredCount() const noexcept
{
    std::lock_guard<std::mutex> lock(entriesMtx_);
    return static_cast<int>(entries_.size());
}

int VmWatchdog::totalHangDetections() const noexcept
{
    return totalHangDetections_.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Watchdog thread main loop
// ---------------------------------------------------------------------------

void VmWatchdog::watchdogLoop()
{
    while (running_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();

        // Snapshot the list of monitored tabs (under lock), then check each
        // outside the lock to avoid holding the mutex during VM operations.
        std::vector<TabId> tabsToCheck;
        {
            std::lock_guard<std::mutex> lock(entriesMtx_);
            tabsToCheck.reserve(entries_.size());
            for (const auto& [tabId, entry] : entries_) {
                if (entry)
                    tabsToCheck.push_back(tabId);
            }
        }

        for (TabId tabId : tabsToCheck) {
            if (!running_.load(std::memory_order_acquire))
                break;

            checkVM(tabId, now);
        }

        // Low-frequency polling: sleep for the check interval.
        std::this_thread::sleep_for(interval_.load(std::memory_order_acquire));
    }
}

// ---------------------------------------------------------------------------
// Per-VM heartbeat check
// ---------------------------------------------------------------------------

bool VmWatchdog::checkVM(TabId tabId, const std::chrono::steady_clock::time_point& now)
{
    std::unique_ptr<WatchdogEntry> entry;
    uint64_t currentHeartbeat = 0;
    uint64_t trackedGen = 0;

    {
        std::lock_guard<std::mutex> lock(entriesMtx_);
        auto it = entries_.find(tabId);
        if (it == entries_.end() || !it->second)
            return false;  // VM not monitored (may have been unregistered).

        entry = it->second->tabId ? std::make_unique<WatchdogEntry>(tabId) : nullptr;
        // We don't need to copy the entry — we can read it in-place under the lock.
        std::unique_ptr<WatchdogEntry>& e = it->second;
        trackedGen = e->trackedGeneration.load(std::memory_order_acquire);
    }

    // -----------------------------------------------------------------------
    // FALSE POSITIVE PROTECTION (PROGRAM.md B4-K5 §FALSE POSITIVE PROTECTION):
    //
    // Only monitor VMs that are expected to be making progress.
    // Do NOT report a hang when the VM is:
    //   - stopped (Inactive)
    //   - suspended (Suspended)
    //   - destroying (Destroyed)
    //   - being recreated (Recreating)
    //   - failed (Failed)
    //   - error state (Error)
    // -----------------------------------------------------------------------

    ChuckVM* vm = vmManager_->findVM(tabId);
    if (!vm) {
        // VM doesn't exist — not a hang.
        return false;
    }

    VMState state = vm->state();

    // Only monitor VMs in the Active (Live) state.
    // In all other states, heartbeat progress is NOT expected.
    if (state != VMState::Active) {
        return false;  // Not expected to be live — no hang.
    }

    // Check generation: if the VM was replaced, this entry is stale.
    if (vm->generation() != trackedGen) {
        // The VM was recreated — unregister and re-register silently.
        unregisterVM(tabId);
        registerVM(tabId, vm->generation());
        return false;  // Not a hang — VM was legitimately recreated.
    }

    // Read the current heartbeat (lock-free, RT-safe).
    currentHeartbeat = vm->heartbeat();

    // Read the last observed heartbeat and progress timestamp.
    std::lock_guard<std::mutex> lock(entriesMtx_);
    auto& e = entries_[tabId];
    if (!e)
        return false;

    uint64_t lastHearbeat = e->lastHeartbeat.load(std::memory_order_acquire);
    auto lastProgress = e->lastProgress;

    // -----------------------------------------------------------------------
    // HEARTBEAT STALL DETECTION (PROGRAM.md B4-K5 §HANG DETECTION):
    //
    // If the heartbeat has not advanced for approximately `timeout_` while
    // the VM is expected to be live/rendering, declare it hung.
    // -----------------------------------------------------------------------

    if (currentHeartbeat > lastHearbeat) {
        // Heartbeat advanced — update baseline.
        e->lastHeartbeat.store(currentHeartbeat, std::memory_order_release);
        e->lastProgress = now;
        return false;  // Not a hang.
    }

    // Heartbeat hasn't advanced — check elapsed time.
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgress);
    if (elapsed < timeout_) {
        return false;  // Still within tolerance.
    }

    // -----------------------------------------------------------------------
    // HANG DETECTED! Record details and trigger recovery.
    // (PROGRAM.md B4-K5 §HANG DETECTION)
    // -----------------------------------------------------------------------

    const uint64_t detectionHeartbeat = currentHeartbeat;
    const auto detectionTime = std::chrono::steady_clock::now();
    const uint64_t detectionGen = trackedGen;

    std::fprintf(stderr,
                 "[watchdog] HANG DETECTED: tab=%u gen=%llu heartbeat=%llu "
                 "elapsed_ms=%lld timeout_ms=%lld\n",
                 static_cast<unsigned>(tabId),
                 static_cast<unsigned long long>(detectionGen),
                 static_cast<unsigned long long>(detectionHeartbeat),
                 static_cast<long long>(elapsed.count()),
                 static_cast<long long>(timeout_.count()));

    totalHangDetections_.fetch_add(1, std::memory_order_acq_rel);

    // Mark the VM as Failed before attempting recovery.
    // This prevents the watchdog from re-triggering recovery concurrently
    // (RESTART LOOP PROTECTION: idempotency).
    vm->setState(VMState::Failed, std::memory_order_release);

    // Notify the callback (UI notification) — called outside any lock.
    if (onHangDetected_) {
        onHangDetected_(tabId, detectionGen, detectionHeartbeat, detectionTime);
    }

    // Attempt recovery.
    recoverVM(tabId);

    return true;
}

// ---------------------------------------------------------------------------
// Restart loop protection
// ---------------------------------------------------------------------------

bool VmWatchdog::canRestart(TabId tabId) const noexcept
{
    std::lock_guard<std::mutex> lock(entriesMtx_);
    auto it = entries_.find(tabId);
    if (it == entries_.end() || !it->second)
        return false;

    const auto& e = it->second;

    // Check restart attempt limit.
    int restarts = e->restartCount.load(std::memory_order_acquire);
    if (restarts >= kMaxRestartAttempts) {
        std::fprintf(stderr,
                     "[watchdog] tab=%u exceeded max restart attempts (%d/%d); "
                     "marking as permanently failed\n",
                     static_cast<unsigned>(tabId),
                     restarts, kMaxRestartAttempts);
        return false;
    }

    // Check cooldown.
    auto nowNs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t lastRestart = e->lastRestartNs.load(std::memory_order_acquire);
    if (lastRestart > 0) {
        int64_t elapsedSinceRestart = nowNs - lastRestart;
        if (elapsedSinceRestart < kRestartCooldownMs) {
            return false;  // Still in cooldown.
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Recovery (PROGRAM.md B4-K5 §RECOVERY)
// ---------------------------------------------------------------------------

bool VmWatchdog::recoverVM(TabId tabId)
{
    // -----------------------------------------------------------------------
    // RESTART LOOP PROTECTION: prevent duplicate/rapid recovery.
    // (PROGRAM.md B4-K5 §RESTART_LOOP_PROTECTION)
    // -----------------------------------------------------------------------
    if (!canRestart(tabId)) {
        std::fprintf(stderr,
                     "[watchdog] recovery skipped for tab=%u (restart loop protection)\n",
                     static_cast<unsigned>(tabId));
        return false;
    }

    // Record the restart attempt.
    {
        std::lock_guard<std::mutex> lock(entriesMtx_);
        auto it = entries_.find(tabId);
        if (it != entries_.end() && it->second) {
            int newCount = it->second->restartCount.fetch_add(1, std::memory_order_acq_rel) + 1;
            auto nowNs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            it->second->lastRestartNs.store(nowNs, std::memory_order_release);

            std::fprintf(stderr,
                         "[watchdog] tab=%u restart attempt %d/%d\n",
                         static_cast<unsigned>(tabId), newCount, kMaxRestartAttempts);
        }
    }

    // -----------------------------------------------------------------------
    // RECOVERY FLOW (PROGRAM.md B4-K5 §RECOVERY):
    //   detect → mark VM failed/hung → tear down → terminate thread →
    //   invalidate old generation → create fresh VM → create fresh thread →
    //   establish fresh watchdog/heartbeat → restart tab via normal B4-K4 path.
    //
    // The VM is already marked Failed above.  Now tear down and recreate.
    // -----------------------------------------------------------------------

    // Step 1: Tear down the old VM (this joins the ChucK thread — may block,
    //         but we're on the watchdog thread, NOT the JUCE audio thread).
    vmManager_->destroyVM(tabId);

    // Step 2: Invalidate the old generation — the VmLifecycle entry is
    //         already marked Destroyed by vmDestroy, which increments the
    //         generation.  Any in-flight compile results targeting the old
    //         generation will be rejected by lookupForCompile().
    //         (STALE RUNTIME PROTECTION)

    // Step 3: Create a fresh VM with VmLifecycle (new generation).
    uint64_t newGen = vmLifecycle_->vmCreate(tabId);

    // Step 4: Activate the fresh VM via VMManager (creates a fresh thread).
    //         The render callback is already registered from the original activation.
    VMResult result = vmManager_->activateVM(tabId);
    if (!result.ok) {
        std::fprintf(stderr,
                     "[watchdog] FAILED to reactivate VM for tab=%u: %s\n",
                     static_cast<unsigned>(tabId), result.message.c_str());

        // Mark as Error so the main process can surface the failure.
        ChuckVM* vm = vmManager_->findVM(tabId);
        if (vm)
            vm->setState(VMState::Error, std::memory_order_release);

        return false;
    }

    // Step 5: Re-register with the watchdog (fresh heartbeat baseline).
    //         This resets the heartbeat tracking and restart cooldown.
    registerVM(tabId, newGen);
    resetHeartbeat(tabId);

    // Step 6: Notify that recovery completed.
    if (onRecoveryComplete_) {
        onRecoveryComplete_(tabId, newGen);
    }

    std::fprintf(stderr,
                 "[watchdog] RECOVERY COMPLETE: tab=%u new_gen=%llu\n",
                 static_cast<unsigned>(tabId),
                 static_cast<unsigned long long>(newGen));

    return true;
}

} // namespace hathor::audio_worker