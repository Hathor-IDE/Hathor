// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * VmWatchdog.hpp — per-VM hang detection and recovery watchdog (B4-K5).
 *
 * Each active per-tab ChucK VM thread increments a std::atomic<uint64_t>
 * heartbeat once per rendered block.  The VmWatchdog is a low-frequency
 * checker thread that observes those heartbeats and, when a heartbeat stalls
 * for ~2 seconds while the VM is expected to be live/rendering, declares the
 * VM hung and triggers recovery: tear down that VM, create a fresh one on a
 * new thread, and re-establish the watchdog/heartbeat baseline.
 *
 * Architectural boundary (PROGRAM.md B4-K5 §ARCHITECTURAL BOUNDARY):
 *   - The watchdog does NOT run on the JUCE audio callback thread.
 *   - The watchdog does NOT block the JUCE audio callback.
 *   - The watchdog does NOT wait indefinitely for a ChucK thread.
 *   - The watchdog does NOT globally restart all ChucK VMs.
 *   - The watchdog does NOT globally mute unrelated .ck tabs.
 *   - The watchdog does NOT rely on UI polling.
 *
 * The watchdog lives in the worker process (hathor-audio-worker), not in the
 * main Hathor process.  It observes real per-tab execution progress, not
 * fabricated heartbeat updates.
 *
 * Recovery is intentionally blunt: the entire VM is torn down and recreated.
 * No surgical single-shred kill is attempted.
 *
 * Recovery is performed on the watchdog thread or a dedicated lifecycle thread,
 * never on the JUCE audio thread.  Teardown (thread join, VM destruction) may
 * block; the watchdog thread is separate from the audio path.
 *
 * Generation/race protection (PROGRAM.md B4-K5 §STALE RUNTIME PROTECTION):
 *   - Each VM has a generation counter that increments on create/replace/destroy.
 *   - The watchdog records the generation it is monitoring.
 *   - If a VM is already being recreated (state == Recreating) when the watchdog
 *     fires, it skips recovery — preventing duplicate recovery for one event.
 *
 * Restart loop protection (PROGRAM.md B4-K5 §RESTART_LOOP_PROTECTION):
 *   - Per-tab restart count and cooldown are tracked.
 *   - After kMaxRestartAttempts within a sliding window, the tab is marked
 *     as Failed (not Recreating) and no further automatic restarts occur.
 *   - Repeated failures are observable via the control plane.
 */

#pragma once

#include "audio_ipc.h"
#include "ChuckVm.hpp"
#include "VMManager.hpp"
#include "VmLifecycle.hpp"
#include "ResourcePolicy.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace hathor::audio_worker {

/**
 * Per-VM watchdog state tracked by VmWatchdog.
 *
 * Records the heartbeat value and timestamp of the last observed progress,
 * the VM generation being monitored (to detect stale recovery), and restart
 * tracking for loop protection.
 */
struct WatchdogEntry {
    TabId tabId = 0;

    /// Last heartbeat value observed by the watchdog.
    std::atomic<uint64_t> lastHeartbeat{0};

    /// Monotonic timestamp of when lastHeartbeat was last observed to change.
    /// Updated only by the watchdog thread (single writer).
    std::chrono::steady_clock::time_point lastProgress;

    /// The VM generation this watchdog entry is tracking.
    /// If the VM's generation changes (recreated), this entry is stale.
    std::atomic<uint64_t> trackedGeneration{0};

    /// Number of automatic restart attempts for this tab.
    /// Resets when the tab is explicitly stopped and later reactivated.
    std::atomic<int> restartCount{0};

    /// Timestamp of the last restart attempt (for cooldown enforcement).
    std::atomic<int64_t> lastRestartNs{0};

    WatchdogEntry() = default;

    explicit WatchdogEntry(TabId t)
        : tabId(t)
        , lastProgress(std::chrono::steady_clock::now())
    {}

    void reset() {
        lastHeartbeat.store(0, std::memory_order_release);
        lastProgress = std::chrono::steady_clock::now();
        restartCount.store(0, std::memory_order_release);
        lastRestartNs.store(0, std::memory_order_release);
    }
};

/**
 * VmWatchdog — low-frequency per-VM hang detection and recovery (B4-K5).
 *
 * Runs as a single worker-side thread that periodically checks all active VMs'
 * heartbeats.  When a VM's heartbeat has not advanced for the configured timeout
 * (~2 seconds by default) while the VM is in the Live/Active state, the watchdog
 * triggers recovery: tear down the VM, create a fresh one, and resume.
 *
 * Thread model:
 *   - The watchdog thread runs independently, polling at kDefaultWatchdogIntervalMs.
 *   - VM operations (activate/deactivate/destroy) are delegated to VMManager,
 *     which serializes them under its own mutex — the watchdog thread may
 *     briefly block here, but it is NEVER the JUCE audio thread.
 *   - Heartbeat reads are lock-free (std::atomic<uint64_t> relaxed load).
 *
 * Recovery flow (per PROGRAM.md B4-K5 §RECOVERY):
 *   detect → mark VM failed/hung → tear down → terminate thread →
 *   invalidate old generation → create fresh VM → create fresh thread →
 *   establish fresh watchdog/heartbeat → restart tab via normal B4-K4 path.
 */
class VmWatchdog {
public:
    /// Callback invoked when a VM hang is detected and recovery begins.
    /// Called from the watchdog thread.  The callback receives the tab ID,
    /// the old VM generation, and the heartbeat value at detection time.
    /// The main process uses this to emit a UI notification.
    using HangDetectedCallback = std::function<void(
        TabId tabId,
        uint64_t oldGeneration,
        uint64_t heartbeatValue,
        std::chrono::steady_clock::time_point detectionTime)>;

    /// Callback invoked after recovery completes (fresh VM created and running).
    using RecoveryCompleteCallback = std::function<void(
        TabId tabId,
        uint64_t newGeneration)>;

    explicit VmWatchdog(VMManager* vmManager,
                        VmLifecycle* vmLifecycle,
                        HangDetectedCallback onHangDetected = nullptr,
                        RecoveryCompleteCallback onRecoveryComplete = nullptr);
    ~VmWatchdog();

    VmWatchdog(const VmWatchdog&) = delete;
    VmWatchdog& operator=(const VmWatchdog&) = delete;
    VmWatchdog(VmWatchdog&&) = delete;
    VmWatchdog& operator=(VmWatchdog&&) = delete;

    /**
     * Start the watchdog thread.  Non-blocking — returns immediately.
     * Must be called from the worker control thread, before the main loop.
     */
    void start();

    /**
     * Signal the watchdog thread to stop and join it.
     * Called during worker shutdown.
     */
    void stop();

    /**
     * Register a VM for watchdog monitoring.
     * Called by VMManager when a VM is activated.
     * @param tabId   The tab slot index.
     * @param generation The VM generation being monitored.
     */
    void registerVM(TabId tabId, uint64_t generation);

    /**
     * Unregister a VM from watchdog monitoring.
     * Called by VMManager when a VM is deactivated/destroyed/suspended.
     */
    void unregisterVM(TabId tabId);

    /**
     * Reset the heartbeat baseline for a tab.
     * Called when a tab transitions from Suspended → Live so the watchdog
     * does not immediately classify the resumed VM as hung (its heartbeat
     * was unchanged while suspended).
     */
    void resetHeartbeat(TabId tabId);

    /**
     * Set the heartbeat timeout (default: kDefaultHeartbeatTimeoutMs = 2000ms).
     */
    void setTimeout(std::chrono::milliseconds timeout) noexcept;

    /**
     * Set the watchdog check interval (default: kDefaultWatchdogIntervalMs = 500ms).
     */
    void setInterval(std::chrono::milliseconds interval) noexcept;    /**
     * Get the number of VMs currently being monitored.
     */
    int monitoredCount() const noexcept;

    /**
     * Get the total number of hang detections across all tabs.
     */
    int totalHangDetections() const noexcept;

    /**
     * Get the number of restart attempts for a specific tab.
     * Returns 0 if the tab has no watchdog entry.
     */
    int restartCountFor(TabId tabId) const noexcept;

private:
    /// The watchdog thread's main loop.
    void watchdogLoop();

    /**
     * Check a single VM's heartbeat.  Called from the watchdog thread.
     * @return true if the VM was detected as hung (and recovery was triggered).
     */
    bool checkVM(TabId tabId, const std::chrono::steady_clock::time_point& now);

    /**
     * Perform recovery for a hung VM.
     * Called from the watchdog thread.
     * Tears down the old VM, creates a fresh one, and re-establishes the
     * watchdog baseline.
     * @return true if recovery succeeded.
     */
    bool recoverVM(TabId tabId);

    /**
     * Check if a restart is allowed for this tab (cooldown + attempt count).
     * Called from the watchdog thread.
     */
    bool canRestart(TabId tabId) const noexcept;

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    VMManager* vmManager_;
    VmLifecycle* vmLifecycle_;

    HangDetectedCallback onHangDetected_;
    RecoveryCompleteCallback onRecoveryComplete_;

    mutable std::mutex entriesMtx_;
    std::unordered_map<TabId, std::unique_ptr<WatchdogEntry>> entries_;

    std::atomic<bool> running_{false};
    std::thread watchdogThread_;

    std::chrono::milliseconds timeout_;
    std::atomic<int> intervalMs_;  // stored as int ms, not atomic<milliseconds>

    std::atomic<int> totalHangDetections_{0};
};

} // namespace hathor::audio_worker