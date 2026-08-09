// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * VmLifecycle.hpp — per-tab VM lifecycle management inside the worker.
 *
 * B4-K3: per-tab Chuck_VM isolation with own thread + lifecycle.
 *
 * This class manages the tab-to-VM mapping inside the worker process. It is
 * the single source of truth for VM state and is accessed by both the control
 * thread (commands: vm_create, vm_destroy) and the compile dispatcher thread
 * (handoff publication). The per-tab render threads read the handoff slot
 * lock-free.
 *
 * Thread safety:
 *   - vmCreate / vmDestroy / compileHandoff: guarded by vmTableMtx_ (control
 *     plane is not RT-sensitive).
 *   - handoffShred (inside ChuckVmEntry): read lock-free by the VM render
 *     thread via std::atomic_load_explicit(acquire); written by the compile
 *     dispatcher via std::atomic_store_explicit(release).
 *   - state / vmGeneration / currentRequestVersion: written under vmTableMtx_,
 *     read by the dispatcher under vmTableMtx_ (lookup). The render thread
 *     reads state via relaxed atomic for early-out decisions.
 *
 * Tab identity is a TabId (slot index). vmGeneration increments on every
 * create/replace/destroy so stale compile results are rejected.
 *
 * Requirements: B4-K3, B4-K4, K0.5
 */

#include "ChuckVm.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace hathor::audio_worker {

class VmLifecycle {
public:
    VmLifecycle() = default;
    ~VmLifecycle() = default;

    VmLifecycle(const VmLifecycle&) = delete;
    VmLifecycle& operator=(const VmLifecycle&) = delete;

    /// Create or replace the VM for a tab. Returns the new vmGeneration.
    /// The old VM (if any) is destroyed; its generation is no longer valid.
    uint64_t vmCreate(TabId tabId);

    /// Destroy the VM for a tab. The tab is marked Inactive; any pending
    /// handoff is invalidated (the VM render thread will discard it on the
    /// next loop iteration when it sees state == Destroyed).
    void vmDestroy(TabId tabId);

    /// Increment the per-tab request version. Called when a new compile
    /// request is issued, so stale results (with a lower version) are
    /// ignored by the VM render thread. Returns the new version.
    uint32_t bumpRequestVersion(TabId tabId);

    /// Resolve a TabId to its VM entry for the compile dispatcher.
    /// Returns nullptr if the VM is Inactive or Destroyed, or if the
    /// generation does not match.
    ChuckVmEntry* lookupForCompile(TabId tabId, uint64_t expectedGeneration);

    /// Load the current handoff shred for a tab (lock-free, called from
    /// the per-tab render thread). Returns nullptr if no new shred is
    /// available or the VM was destroyed/replaced.
    std::shared_ptr<CompiledShred> loadHandoff(TabId tabId) noexcept;

    /// Return the current VmState for a tab (relaxed read — for render
    /// thread early-outs).
    VmState stateOf(TabId tabId) const noexcept;

    /// Return the current vmGeneration for a tab.
    uint64_t generationOf(TabId tabId) const noexcept;

    /// Return the current request version for a tab.
    uint32_t currentVersionOf(TabId tabId) const noexcept;

    /// Check if a tab has an active VM.
    bool hasActiveVm(TabId tabId) const noexcept;

private:
    /// The VM table is a fixed array (kNumTabs) for O(1) lookup and to
    /// avoid heap allocation in the render path. Entries are only modified
    /// under vmTableMtx_.
    ChuckVmEntry vmTable_[kNumTabs];

    /// Guards structural changes to vmTable_ (create/destroy/version bump).
    mutable std::mutex vmTableMtx_;
};

} // namespace hathor::audio_worker
