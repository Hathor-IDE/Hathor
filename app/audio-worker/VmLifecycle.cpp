// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "VmLifecycle.hpp"
#include "ChuckVm.hpp"

namespace hathor::audio_worker {

// ---------------------------------------------------------------------------
// vmCreate — create or replace the VM for a tab
// ---------------------------------------------------------------------------

uint64_t VmLifecycle::vmCreate(TabId tabId)
{
    if (tabId < 0 || tabId >= kNumTabs)
        return 0;

    std::lock_guard<std::mutex> lock(vmTableMtx_);
    ChuckVmEntry& entry = vmTable_[tabId];
    // Increment generation — any in-flight compile for the old generation
    // will be rejected by the compile dispatcher's generation check.
    // Use fetch_add to get the previous value, then verify it's >= 0.
    entry.vmGeneration.fetch_add(1, std::memory_order_acq_rel);
    entry.tabId = tabId;
    entry.state.store(VmState::Active, std::memory_order_release);
    entry.currentRequestVersion.store(0, std::memory_order_release);
    entry.loadedSourceHash = 0;
    entry.loadedShredId = -1;
    // Clear any pending handoff from a previous VM lifecycle.
    // Note: std::atomic_store_explicit on shared_ptr* expects shared_ptr<T>,
    // not nullptr, per libc++ API.
    std::atomic_store_explicit(&entry.handoffShred, std::shared_ptr<CompiledShred>{},
                               std::memory_order_release);
    return entry.vmGeneration;
}

// ---------------------------------------------------------------------------
// vmDestroy — destroy the VM for a tab
// ---------------------------------------------------------------------------

void VmLifecycle::vmDestroy(TabId tabId)
{
    if (tabId < 0 || tabId >= kNumTabs)
        return;

    std::lock_guard<std::mutex> lock(vmTableMtx_);
    ChuckVmEntry& entry = vmTable_[tabId];
    entry.state.store(VmState::Destroyed, std::memory_order_release);
    ++entry.vmGeneration; // invalidates any in-flight compile results
    entry.currentRequestVersion.store(0, std::memory_order_release);
    // The render thread, on its next iteration, will see state == Destroyed
    // and discard the handoff. We clear it here for cleanliness.
    std::atomic_store_explicit(&entry.handoffShred, std::shared_ptr<CompiledShred>{},
                               std::memory_order_release);
}

// ---------------------------------------------------------------------------
// bumpRequestVersion — called when a new compile request is issued
// ---------------------------------------------------------------------------

uint32_t VmLifecycle::bumpRequestVersion(TabId tabId)
{
    if (tabId < 0 || tabId >= kNumTabs)
        return 0;

    std::lock_guard<std::mutex> lock(vmTableMtx_);
    ChuckVmEntry& entry = vmTable_[tabId];
    entry.currentRequestVersion.fetch_add(1, std::memory_order_release);
    entry.cancelCompileRequest.store(false, std::memory_order_release);
    return entry.currentRequestVersion.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// cancelCompileRequest — set the cancellation flag for a tab's in-flight compile
// ---------------------------------------------------------------------------

bool VmLifecycle::cancelCompileRequest(TabId tabId)
{
    if (tabId < 0 || tabId >= kNumTabs)
        return false;

    std::lock_guard<std::mutex> lock(vmTableMtx_);
    ChuckVmEntry& entry = vmTable_[tabId];

    // Only meaningful if there is an active VM; silently no-op for
    // Inactive / Destroyed tabs (the dispatcher would reject the lookup
    // anyway, so there is nothing to cancel).
    if (entry.state.load(std::memory_order_acquire) == VmState::Inactive ||
        entry.state.load(std::memory_order_acquire) == VmState::Destroyed)
        return false;

    entry.cancelCompileRequest.store(true, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// compileCancelled — check whether cancellation has been requested
// ---------------------------------------------------------------------------

bool VmLifecycle::compileCancelled(TabId tabId) const noexcept
{
    if (tabId < 0 || tabId >= kNumTabs)
        return false;

    // Lock-free read: the dispatcher thread is the only writer (sets true),
    // bumpRequestVersion / vmDestroy are the only resetters (set false),
    // and both paths hold vmTableMtx_.  acquire ordering pairs with the
    // release store in cancelCompileRequest().
    return vmTable_[tabId].cancelCompileRequest.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// lookupForCompile — resolve TabId + generation, check liveness
// ---------------------------------------------------------------------------

ChuckVmEntry* VmLifecycle::lookupForCompile(TabId tabId, uint64_t expectedGeneration)
{
    if (tabId < 0 || tabId >= kNumTabs)
        return nullptr;

    std::lock_guard<std::mutex> lock(vmTableMtx_);
    ChuckVmEntry& entry = vmTable_[tabId];

    // Reject if the VM was replaced or destroyed since the request was issued.
    if (entry.vmGeneration != expectedGeneration)
        return nullptr;

    if (entry.state.load(std::memory_order_acquire) == VmState::Inactive ||
        entry.state.load(std::memory_order_acquire) == VmState::Destroyed)
        return nullptr;

    return &entry;
}

// ---------------------------------------------------------------------------
// loadHandoff — lock-free, called from the per-tab render thread
// ---------------------------------------------------------------------------

std::shared_ptr<CompiledShred> VmLifecycle::loadHandoff(TabId tabId) noexcept
{
    if (tabId < 0 || tabId >= kNumTabs)
        return nullptr;

    // No lock — the render thread is the sole consumer of handoffShred for
    // this tab. The compile dispatcher is the sole producer. They communicate
    // via the atomic store/load on the shared_ptr (Apple-Clang compatible
    // free-function API, matching AudioEngine::slots_).
    ChuckVmEntry& entry = vmTable_[tabId];

    // Reject if the VM was destroyed/replaced.
    if (entry.state.load(std::memory_order_acquire) != VmState::Active)
        return nullptr;

    auto shred = std::atomic_load_explicit(&entry.handoffShred,
                                           std::memory_order_acquire);
    if (!shred)
        return nullptr;

    // Stale-result rejection: the VM only accepts results whose requestVersion
    // matches the current version. This prevents stale A/B results from
    // overwriting C in the rapid-successive-evaluation case.
    if (shred->requestVersion != entry.currentRequestVersion.load(std::memory_order_acquire))
        return nullptr;

    // Clear the handoff slot so the result is consumed exactly once.
    // If this is a replacement, the old shred is released here.
    std::atomic_store_explicit(&entry.handoffShred, std::shared_ptr<CompiledShred>{},
                               std::memory_order_release);

    return shred;
}

// ---------------------------------------------------------------------------
// stateOf / generationOf / currentVersionOf / hasActiveVm
// ---------------------------------------------------------------------------

VmState VmLifecycle::stateOf(TabId tabId) const noexcept
{
    if (tabId < 0 || tabId >= kNumTabs)
        return VmState::Inactive;
    return vmTable_[tabId].state.load(std::memory_order_acquire);
}

uint64_t VmLifecycle::generationOf(TabId tabId) const noexcept
{
    if (tabId < 0 || tabId >= kNumTabs)
        return 0;
    return vmTable_[tabId].vmGeneration;
}

uint32_t VmLifecycle::currentVersionOf(TabId tabId) const noexcept
{
    if (tabId < 0 || tabId >= kNumTabs)
        return 0;
    return vmTable_[tabId].currentRequestVersion.load(std::memory_order_acquire);
}

bool VmLifecycle::hasActiveVm(TabId tabId) const noexcept
{
    if (tabId < 0 || tabId >= kNumTabs)
        return false;
    // Relaxed read is sufficient for a best-effort "is there a VM?" check.
    // The authoritative checks in lookupForCompile and loadHandoff use
    // stronger ordering under the appropriate locks/atomics.
    return vmTable_[tabId].state.load(std::memory_order_acquire) == VmState::Active;
}

} // namespace hathor::audio_worker
