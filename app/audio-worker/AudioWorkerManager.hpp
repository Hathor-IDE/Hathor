// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * AudioWorkerManager.hpp — out-of-process hathor-audio-worker lifecycle manager.
 *
 * Owns the companion process that runs ChucK VM(s).  Provides:
 *   - Worker start / stop / restart lifecycle
 *   - Generation / session identity tracking (distinguishes dead vs stale vs alive)
 *   - Control-plane IPC (Unix domain socket: ping, status, stop)
 *   - RT-safe audio transport consumption from validated shared-memory ring
 *   - Worker death detection (waitpid WNOHANG + heartbeat staleness + workerAlive flag)
 *   - Stale shared-memory invalidation (generation mismatch → silence)
 *   - Reinitialization of shared memory outside the real-time callback
 *   - Resource-policy hook for B4-K3 (configurable VM/thread budget)
 *
 * Thread model:
 *   - JUCE audio thread calls tryReadAudioBlock() — MUST be non-blocking,
 *     allocation-free, and never wait on worker lifecycle operations.
 *   - Main thread (or a dedicated worker thread) calls start(), restart(),
 *     shutdown(), and lifecycle management — these may perform blocking
 *     operations (posix_spawn, shm_open, waitpid).
 *   - An internal control-plane thread polls for process death and can be
 *     used for liveness checks, but death detection must also work without
 *     it (via shared-memory atomics checked by the audio thread).
 *
 * K0.5 conformance: This manager does NOT expose any concurrent compile/run
 * API.  Compilation of .ck code is serialized through the control plane
 * (future B4-K4), honoring the K0.5 NO-GO decision.
 *
 * Requirements: B4-K2, B4-K0.5, B4-K0.6, Decision #24
 */

#include "audio_ipc.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace hathor {

/**
 * AudioWorkerManager — manages the hathor-audio-worker companion process.
 *
 * Lifecycle:
 *   start()     → spawns worker, establishes generation, opens transport
 *   shutdown()  → clean stop via control plane, invalidates transport
 *   restart()   → stop + start with new generation
 *
 * Audio consumption (real-time safe):
 *   tryReadAudioBlock() — called from JUCE audio callback.
 *     Returns true if a valid block was consumed; false → caller emits silence.
 *     Never blocks, never allocates, never waits on worker lifecycle.
 *
 * Worker death detection:
 *   isWorkerAlive()    — checks process + heartbeat + workerAlive flag
 *   invalidateTransport() — called on detection; marks current generation invalid
 *
 * Resource policy (Decision #24):
 *   setResourceLimits() — configures max concurrent VMs / threads budget.
 *   getResourceLimits() — returns current policy (for K3 to enforce).
 */
class AudioWorkerManager {
public:
    // -----------------------------------------------------------------------
    // Resource policy (Decision #24 — configurable, not hard-coded)
    // -----------------------------------------------------------------------

    /// Per-worker resource budget.  This is the policy hook that B4-K3 will
    /// use to enforce the configurable VM/thread ceiling.  K2 does NOT auto-
    /// create a VM for every open .ck file — it only exposes this hook.
    struct ResourceLimits {
        /// Maximum concurrent live Chuck_VM instances the worker may create.
        int  maxVms         = 8;
        /// Maximum total OS threads (audio + compile + watchdog) the worker may use.
        int  maxThreads     = 16;
        /// Maximum per-VM memory budget in megabytes.
        int  maxVmMemoryMb  = 256;
        /// Heartbeat timeout in milliseconds — worker is considered dead if
        /// lastHeartbeat hasn't advanced in this long.
        int  heartbeatTimeoutMs = 500;
        /// Maximum restart attempts before giving up.
        int  maxRestarts    = 10;
    };

    // -----------------------------------------------------------------------
    // Worker status (liveness / generation)
    // -----------------------------------------------------------------------

    enum class WorkerStatus {
        Healthy,          ///< worker running and producing audio
        ShuttingDown,     ///< worker received stop command, in progress of exiting
        Dead,             ///< worker process detected as dead
        StaleGeneration,  ///< old generation detected (worker was replaced)
        NotStarted,       ///< no worker running
        StartError,       ///< worker failed to start
    };

    // -----------------------------------------------------------------------
    // Construction / destruction
    // -----------------------------------------------------------------------

    AudioWorkerManager();
    ~AudioWorkerManager();

    AudioWorkerManager(const AudioWorkerManager&)            = delete;
    AudioWorkerManager& operator=(const AudioWorkerManager&) = delete;
    AudioWorkerManager(AudioWorkerManager&&)                 = delete;
    AudioWorkerManager& operator=(AudioWorkerManager&&)      = delete;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Start the worker process and establish the shared-memory transport.
    /// Sets a new generation (max(current+1, 1)).
    /// @param workerPath  Path to the hathor-audio-worker executable.
    /// @return true on success; false + error message in getLastError().
    bool start(std::string workerPath);

    /// Clean shutdown: send "stop" via control plane, wait for exit,
    /// invalidate transport.  Safe to call if already stopped.
    void shutdown();

    /// Restart the worker: shutdown() + start() with a new generation.
    /// @return true on success.
    bool restart();

    // -----------------------------------------------------------------------
    // Status / liveness (non-RT-safe — may block briefly)
    // -----------------------------------------------------------------------

    /// Current worker status.
    WorkerStatus status() const noexcept;

    /// Current generation identity.  Changes on every restart.
    uint64_t generation() const noexcept;

    /// True if the worker process is alive and producing.
    bool isWorkerAlive() const noexcept;

    /// Last error message (cleared on next successful start).
    std::string getLastError() const;

    // -----------------------------------------------------------------------
    // Resource policy (Decision #24)
    // -----------------------------------------------------------------------

    /// Set the resource budget for the worker.  Sent to the worker via
    /// the control plane on start().  K3 will also be able to adjust this
    /// at runtime.
    void setResourceLimits(const ResourceLimits& limits) noexcept;

    /// Get the current resource limits.
    ResourceLimits getResourceLimits() const noexcept;

    // -----------------------------------------------------------------------
    // B4-K3: Per-tab VM control API (non-RT-safe)
    // -----------------------------------------------------------------------

    /// Activate a VM for the given tab slot.  Idempotent — if the VM is
    /// already active, returns success without effect.
    /// @param tabId      Slot index [0, 16).
    /// @param sampleRate Sample rate for the VM.
    /// @param channels   Channel count.
    /// @return VMResult with ok=true on success.
    audio_worker::VMResult activateTabVM(uint8_t tabId, unsigned sampleRate = 44100, unsigned channels = 1);

    /// Deactivate a VM: suspend (keep state) or destroy (full teardown).
    /// @param tabId   Slot index.
    /// @param suspend If true, pause the VM (resumable). If false, destroy it.
    /// @return VMResult with ok=true on success.
    audio_worker::VMResult deactivateTabVM(uint8_t tabId, bool suspend = true);

    /// Resume a previously suspended VM.
    /// @param tabId Slot index.
    /// @return VMResult with ok=true on success.
    audio_worker::VMResult resumeTabVM(uint8_t tabId);

    /// Destroy a VM and release all resources for the given tab.
    /// @param tabId Slot index.
    /// @return VMResult with ok=true on success.
    audio_worker::VMResult destroyTabVM(uint8_t tabId);

    /// Compile ChucK code for a tab's VM (K0.5 serialized path).
    /// @param tabId Slot index.
    /// @param code  ChucK source code.
    /// @return VMResult with ok=true on success.
    audio_worker::VMResult compileTabVM(uint8_t tabId, const std::string& code);

    /// B4-K7: Evaluate a .ck tab — full compile→load→execute path.
    /// Activates the VM for the tab (if not already active), queries its
    /// generation, bumps the request version, and sends ck_compile with the
    /// real generation.  The compile result is published via the atomic
    /// handoff slot and consumed by the VM's render thread.
    ///
    /// If the tab's VM was destroyed by a previous ck_stop, this reactivates it.
    /// On compile failure, the previously running shred is NOT replaced.
    ///
    /// @param tabId Slot index.
    /// @param code  ChucK source code.
    /// @return VMResult with ok=true on successful compile+publish.
    audio_worker::VMResult evaluateCkTab(uint8_t tabId, const std::string& code);

    /// B4-K7: Stop a .ck tab — destroy the VM and remove any pending handoff.
    /// @param tabId Slot index.
    /// @return VMResult with ok=true on success.
    audio_worker::VMResult stopCkTab(uint8_t tabId);

    /// Query the state of a tab's VM.
    /// @param tabId Slot index.
    /// @return VMResult with state info in the message field.
    audio_worker::VMResult queryTabVM(uint8_t tabId) const;

    /// List all VMs and their states.
    std::string listTabVMs() const;

    /// B4-K5: Query the worker's watchdog hang detection status for a specific
    /// tab, or all tabs.  Returns a JSON-like string with hang event info.
    /// @param tabId  Tab slot index, or -1 for all recent events.
    std::string queryHangStatus(int tabId) const;

    /// Set the resource ceiling policy at runtime.
    /// @param maxConcurrentLiveVMs Maximum number of active VMs.
    void setMaxConcurrentLiveVMs(int maxVms);

    // -----------------------------------------------------------------------
    // RT-safe audio consumption — called from the JUCE audio thread ONLY
    // -----------------------------------------------------------------------

    /**
     * Try to consume one audio block from the worker's shared-memory ring.
     *
     * MUST be called from the JUCE audio thread.  Guarantees:
     *   - Never blocks
     *   - Never allocates
     *   - Never waits on worker lifecycle (process spawn, restart, IPC)
     *   - Never acquires a lifecycle mutex
     *   - Returns false immediately if worker is dead, transport is stale,
     *     or no new data is available → caller emits silence.
     *
     * @param outBuf      Output buffer (kBlockSize floats).  Only written on success.
     * @param expectedGen The generation this block must belong to.  If the
     *                    transport's generation differs, the block is rejected
     *                    as stale.
     * @return true if a valid block was consumed; false → silence.
     */
    bool tryReadAudioBlock(float* outBuf, uint32_t blockSize, uint64_t expectedGen) noexcept;

    // -----------------------------------------------------------------------
    // Control-plane commands (non-RT-safe — may block briefly)
    // ------------------------------------------------------------------

    /// Send a command to the worker and wait for a response (bounded timeout).
    /// @return Response string, or empty string on timeout/disconnect.
    std::string sendControlCommand(std::string_view cmd, int timeoutMs = 500) const;

    // -----------------------------------------------------------------------
    // Transport invalidation (may be called from any thread)
    // -----------------------------------------------------------------------

    /// Mark the current transport/generation as invalid.  Called when worker
    /// death is detected.  The audio callback will then emit silence until
    /// a new worker is started with a fresh generation.
    void invalidateTransport() noexcept;

    /// Check if the current transport generation is still valid.
    bool isTransportValid(uint64_t expectedGen) const noexcept;

private:
    // -----------------------------------------------------------------------
    // Internal: process management
    // -----------------------------------------------------------------------

    /// Spawn the worker process with the given generation pre-set in shared memory.
    bool spawnWorker(const std::string& workerPath);

    /// Create or re-create the shared-memory segment and set the generation.
    /// Called outside the RT callback.
    bool initSharedMemory(uint64_t gen);

    /// Unmap and unlink shared memory.  Called outside the RT callback.
    void cleanupSharedMemory();

    /// Wait for the worker to report it is alive (polls workerAlive flag).
    bool waitForWorkerStart(int timeoutMs = 3000);

    // -----------------------------------------------------------------------
    // Internal: liveness detection
    // -----------------------------------------------------------------------

    /// Check if the worker process has exited (non-blocking waitpid).
    bool checkProcessExit();

    // -----------------------------------------------------------------------
    // Internal state
    // -----------------------------------------------------------------------

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hathor
