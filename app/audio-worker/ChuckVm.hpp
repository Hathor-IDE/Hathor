// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ChuckVm.hpp — per-tab ChucK VM lifecycle inside the hathor-audio-worker process.
 *
 * B4-K3 defines the per-tab isolated Chuck_VM lifecycle. This header provides
 * the worker-side representation: a tab identity (TabId) maps to a VM with
 * its own lifecycle state, generation counter, and atomic handoff slot for
 * compiled shreds.
 *
 * K0.5 constraint: compileCode() and run() must NOT be called concurrently on
 * the same ChucK instance. The serialized command path (ChuckCompiler) ensures
 * all ChucK operations on a given VM are serialized through a single dispatcher
 * thread. A VM's run() loop and the compile→handoff are therefore never on
 * the same call stack simultaneously.
 *
 * Tab identity:
 *   TabId = slot index [0, kNumTabs-1] from the editor/UI.
 *   Each TabId maps to at most one active VM. The VM carries a generation
 *   counter so stale compile results can be rejected.
 *
 * Compile handoff:
 *   Uses the same std::atomic_store_explicit / std::atomic_load_explicit
 *   discipline as AudioEngine::slots_ (app/AudioEngine.cpp). A shared_ptr
 *   to the compiled shred descriptor is published by the compile thread and
 *   consumed by the VM's loop on the next iteration.
 *
 * Requirements: B4-K3, B4-K4, K0.5 NO-GO decision
 */

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace hathor::audio_worker {

/// Maximum number of simultaneous .ck tabs (matches AudioEngine::kNumSlots).
static constexpr int kNumTabs = 16;

/// Tab identity — a small integer slot index assigned by the editor.
using TabId = int;

/// A compiled ChucK shred descriptor. This is the worker-side representation
/// suitable for handoff. It contains only data that owns its lifetime
/// explicitly — no raw VM pointers, no transient compiler state.
///
/// The compiled shred is represented by its source hash and a success flag.
/// The actual shred ID is assigned by the VM when it processes the handoff.
/// This decouples the compile result (which may outlive the originating VM)
/// from the VM's internal shred table.
struct CompiledShred {
    /// Source code hash — allows the VM to skip re-compilation if the
    /// incoming source is identical to what's already loaded (idempotent
    /// re-eval). The hash is computed on the compile thread.
    uint64_t sourceHash = 0;

    /// The source snippet (for error reporting and re-compilation if the VM
    /// was replaced). This is owned by the shared_ptr and lives as long as
    /// the handoff object.
    std::string sourceCode;

    /// Version tag — monotonically increasing per-tab counter. The VM only
    /// accepts the result if requestVersion == currentVersion. This handles
    /// the rapid-successive-evaluation case (A → B → C) so stale results
    /// cannot overwrite newer ones.
    uint32_t requestVersion = 0;

    /// Compile succeeded.
    bool ok = false;

    /// Error message (when ok == false). Populated on the compile thread;
    /// may allocate.
    std::string error;

    /// Error line/column (1-based) from the ChucK compiler, if available.
    int errorLine = 0;

    /// Error column (1-based), if available.
    int errorColumn = 0;
};

/// VM lifecycle state (worker-side, written by the control/serialized thread,
/// read by the audio-render loop via relaxed atomic).
enum class VmState : uint8_t {
    Inactive,   ///< no VM for this tab (never created or destroyed)
    Active,     ///< VM exists and is running
    Suspended,  ///< VM exists but suspended (not consuming CPU)
    Destroyed,  ///< VM was explicitly destroyed; tab identity is stale
};

/// Per-tab VM descriptor. Lives in the worker process. Each tab maps to at
/// most one of these. The descriptor is reference-counted via shared_ptr
/// so that in-flight compile results can hold a weak reference to verify
/// VM liveness at handoff time.
struct ChuckVmEntry {
    TabId         tabId = -1;
    uint64_t      vmGeneration = 0;   ///< increments on every create/replace
    VmState       state{VmState::Inactive};
    uint32_t      currentRequestVersion = 0;  ///< bump on each new compile request
    std::string   ckSource;  ///< last successfully compiled source (for idempotency)
    uint64_t      loadedSourceHash = 0;       ///< hash of currently-loaded shred
    int           loadedShredId = -1;         ///< shred ID assigned by the VM

    /// Atomic handoff slot — follows the AudioEngine::slots_ pattern exactly.
    /// The compile thread publishes a shared_ptr<CompiledShred> here via
    /// std::atomic_store_explicit(release). The VM's render loop loads it
    /// via std::atomic_load_explicit(acquire) and consumes on the next
    /// iteration.
    ///
    /// Apple-Clang compatibility: std::atomic<shared_ptr<T>> is not
    /// specialized in libc++, so we use the free-function API on a plain
    /// shared_ptr member instead (same approach as AudioEngine::slots_).
    std::shared_ptr<CompiledShred> handoffShred;
};

} // namespace hathor::audio_worker
