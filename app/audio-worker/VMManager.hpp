// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * VMManager.hpp — per-tab ChucK VM registry inside the hathor-audio-worker process.
 *
 * Implements the TabId→VM→thread→watchdog mapping (B4-K3).  Each tab slot [0,15]
 * maps to zero or one ChuckVM instance.  The mapping is re-buildable after a
 * worker restart: only TabId and desired state need to be persisted by the
 * main process; the VM pointer itself is never sent across the process boundary.
 *
 * Resource-policy enforcement (Decision #24):
 *   - maxConcurrentLiveVMs ceiling
 *   - LRU suspend / reject / destroy at ceiling (configurable)
 *   - Idle suspension timeout (configurable)
 *
 * K0.5 conformance: compileCode() is serialized per VM — never called
 * concurrently with run() from a different thread.
 */

#include "ChuckVm.hpp"
#include "ResourcePolicy.hpp"
#include "audio_ipc.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hathor::audio_worker {

class VMManager {
public:
    VMManager();
    ~VMManager();

    VMManager(const VMManager&)            = delete;
    VMManager& operator=(const VMManager&) = delete;
    VMManager(VMManager&&)                 = delete;
    VMManager& operator=(VMManager&&)      = delete;

    // -----------------------------------------------------------------------
    // Lifecycle control (called from worker control thread)
    // -----------------------------------------------------------------------

    /// Activate a VM for the given tab (create or resume).
    /// Enforces the resource ceiling: if the ceiling is reached, applies
    /// the configured ceiling behavior (LRU suspend, reject, or LRU destroy).
    /// @return VMResult with ok=true on success.
    VMResult activateVM(TabId tabId, unsigned sampleRate = 44100, unsigned channels = 1);

    /// Deactivate a VM (suspend or destroy per policy).
    VMResult deactivateVM(TabId tabId, bool suspend = true);

    /// Resume a suspended VM.
    VMResult resumeVM(TabId tabId);

    /// Destroy a VM and release all resources.
    VMResult destroyVM(TabId tabId);

    /// Compile ChucK code for a tab's VM (K0.5 serialized).
    VMResult compileVM(TabId tabId, const std::string& code);

    // -----------------------------------------------------------------------
    // Status / introspection
    // -----------------------------------------------------------------------

    /// Query the state of a tab's VM.
    VMResult queryVM(TabId tabId) const;

    /// List all VMs and their states.
    std::string listVMs() const;

    /// Get the generation of a tab's VM (for watchdog stale-runtime protection).
    /// Returns 0 if no VM exists for the tab.
    uint64_t getVMGeneration(TabId tabId) const;

    /// Count VMs in a given state.
    int countState(VMState state) const;

    /// Count currently active (running) VMs.
    int countActive() const;

    /// Check if the resource ceiling would allow one more VM.
    bool canActivateMore() const;

    /// Get the current resource policy.
    ResourcePolicy getPolicy() const noexcept;

    /// Set the resource policy (runtime reconfigurable).
    void setPolicy(const ResourcePolicy& policy);

    // -----------------------------------------------------------------------
    // Render callback registration
    // -----------------------------------------------------------------------

    /// Set the render callback used by all new VMs.
    void setRenderCallback(ChuckVM::RenderCallback cb);

    /// B4-K7: Set the handoff loader callback used by all new VMs.
    /// The render thread calls this to consume compiled shreds from the
    /// atomic handoff slot (lock-free).
    void setHandoffLoader(ChuckVM::HandoffLoader loader) noexcept;

    // -----------------------------------------------------------------------
    // Watchdog integration (B4-K5)
    ///
    /// Called by the worker's watchdog checker.  Checks all active VMs'
    /// heartbeats and tears down any that have stalled beyond the timeout.
    /// @return Number of VMs that timed out and were restarted.
    int checkHeartbeats(std::chrono::milliseconds timeout);

    /// Look up a VM by tab ID (non-owning pointer).
    /// Used by the watchdog (VmWatchdog) to read heartbeat/state.
    /// Returns nullptr if no VM exists for the tab.
    ChuckVM* findVM(TabId tabId) const;

private:
    /// Count active VMs assuming mutex_ is already held.
    int countActiveLocked() const;

    /// Count VMs in a given state assuming mutex_ is already held.
    int countStateLocked(VMState state) const;

private:
    /// Look up or create a ChuckVM for the given tab.
    ChuckVM* getOrCreateVM(TabId tabId);

    /// Apply ceiling behavior when maxConcurrentLiveVMs is reached.
    /// @return true if ceiling was handled (a VM was suspended/destroyed),
    ///         false if the ceiling cannot be relieved.
    bool handleCeilingEviction();

    /// Check idle suspension timeout for all suspended VMs.
    void checkIdleSuspension();

    /// Update lastActiveTs timestamps for LRU tracking.
    mutable std::mutex mutex_;

    /// TabId → ChuckVM mapping.  Uses std::unique_ptr for clear ownership.
    std::unordered_map<TabId, std::unique_ptr<ChuckVM>> vms_;

    /// LRU tracking: list of TabIds ordered by least-recent use.
    std::vector<TabId> lruList_;

    /// Last active timestamps (monotonic, milliseconds since epoch).
    std::unordered_map<TabId, std::chrono::milliseconds> lastActiveTs_;

    /// Last pause timestamps (for idle suspend / destroy policy).
    std::unordered_map<TabId, std::chrono::milliseconds> lastPauseTs_;

    /// Current resource policy.
    ResourcePolicy policy_;

    /// Render callback for new VMs.
    ChuckVM::RenderCallback renderCallback_;

    /// Monotonic clock source for timestamps.
    static std::chrono::milliseconds nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch());
    }
};

} // namespace hathor::audio_worker
