// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ResourcePolicy.hpp — data-driven bounded resource policy for per-tab ChucK VMs (B4-K3).
 *
 * Implements Decision #24: resource policy is explicit configuration + measured
 * data, not hard-coded "one VM per file" or "no limit".
 *
 * Key concepts:
 *   - maxConcurrentLiveVMs: ceiling on simultaneously active VMs.
 *   - Per-VM estimated CPU cost (samples/ms) and RAM cost (bytes).
 *   - Idle suspension: inactive tabs are suspended (not destroyed) so they
 *     resume without losing state.
 *   - Ceiling behavior: configurable LRU-suspend or reject-with-message.
 *   - All policy is serializable to/from JSON for settings persistence.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace hathor::audio_worker {

/// TabId is defined in ChuckVM.hpp as a slot index [0,15].
using TabId = uint8_t;

/**
 * Per-VM resource cost estimates.
 *
 * These values are measured by the B4-K3 measurement harness
 * (see docs/b4-k3-measurements.md) and stored as configuration.
 * They inform the resource ceiling and ceiling behavior.
 */
struct VMResourceCost {
    /// Estimated CPU usage per render block (percentage of one core, 0.0–1.0).
    float cpuPerBlock = 0.001f;

    /// Estimated peak memory (bytes) for an idle ChucK VM instance.
    std::size_t idleMemoryBytes = 4 * 1024 * 1024; // ~4 MB baseline

    /// Estimated memory per active shred (bytes).
    std::size_t perShredBytes = 256 * 1024; // ~256 KB per shred
};

/**
 * Policy for what happens when the live-VM ceiling is reached.
 *
 * Per PROGRAM.md B4-K3: "Define what happens at the ceiling (LRU-suspend idle,
 * or reject with a message — configurable)."
 */
enum class CeilingBehavior : uint8_t {
    /// Suspend the least-recently-used inactive VM when ceiling is reached.
    /// Active tabs are preserved; idle-but-open tabs are candidates.
    LRUSuspend,

    /// Reject the activation request and return an error to the caller.
    RejectWithError,

    /// Hard-kill the least-recently-used inactive VM (destroy + recreate on re-activation).
    /// More aggressive than LRU suspend; used when memory is tight.
    LRUDestroy,
};

/**
 * ResourcePolicy — configures the bounded resource policy for per-tab VMs.
 *
 * This is data-driven (serialized via AudioWorkerManager to settings) so the
 * policy can change WITHOUT rewriting the per-tab isolation core (B4-K3).
 */
struct ResourcePolicy {
    /// Maximum concurrent live VM instances the worker may create.
    /// Default: 8 (measured safe ceiling at 44.1 kHz, 16-slot engine).
    int maxConcurrentLiveVMs = 8;

    /// Maximum total OS threads (audio + compile + watchdog) the worker may use.
    /// Each active VM consumes one thread (the ChucK thread).
    int maxThreads = 16;

    /// Maximum per-VM memory budget in megabytes.
    int maxVmMemoryMb = 256;

    /// Idle suspension timeout — tabs inactive for longer than this are
    /// automatically suspended (deterministic pause, state retained).
    /// 0 = no automatic suspension (manual policy only).
    int idleSuspendTimeoutSec = 30;

    /// Whether inactive tabs should be suspended (true) or destroyed (false)
    /// when deactivated.  Per PROGRAM.md: "prefer deterministic suspend."
    bool preferSuspendOverDestroy = true;

    /// What happens when maxConcurrentLiveVMs is reached.
    CeilingBehavior ceilingBehavior = CeilingBehavior::LRUSuspend;

    /// Per-VM resource cost estimates (measured data, Decision #24).
    VMResourceCost vmCost;

    // -----------------------------------------------------------------------
    // Serialization (for settings persistence via AudioWorkerManager)
    // -----------------------------------------------------------------------

    /// Serialize to a compact JSON string.
    std::string serialize() const;

    /// Deserialize from a JSON string.  Returns false on parse error.
    bool deserialize(const std::string& json);

    // -----------------------------------------------------------------------
    // Queries (used by the VM manager to enforce policy)
    // -----------------------------------------------------------------------

    /// Check if the policy would allow one more VM (given current active count).
    bool canActivate(int currentActiveVms) const noexcept {
        return currentActiveVms < maxConcurrentLiveVMs;
    }

    /// Check if the policy would allow activating the given number of VMs.
    bool canActivateN(int currentActiveVms, int n) const noexcept {
        return (currentActiveVms + n) <= maxConcurrentLiveVMs;
    }

    /// Estimated total CPU cost if all current VMs are at peak.
    float estimatedTotalCPU(int currentActiveVms) const noexcept {
        return static_cast<float>(currentActiveVms) * vmCost.cpuPerBlock;
    }

    /// Estimated total memory if all current VMs are active.
    std::size_t estimatedTotalMemory(int currentActiveVms) const noexcept {
        return static_cast<std::size_t>(currentActiveVms) * vmCost.idleMemoryBytes;
    }

    /// Returns true if the ceiling behavior suspends (preserves state).
    bool ceilingPreservesState() const noexcept {
        return ceilingBehavior == CeilingBehavior::LRUSuspend;
    }
};

/**
 * VMRecord — tracks a single VM's lifecycle metadata for policy decisions.
 *
 * The mapping is rebuildable after a worker restart: only TabId and state
 * are needed to reconstruct the intent.  The VM pointer itself is never
 * persisted across process boundaries.
 */
struct VMRecord {
    TabId   tabId;
    VMState state;          // from ChuckVM.hpp (included transitively)
    int64_t lastActiveTs;   // monotonic timestamp, for LRU eviction
    int64_t lastPauseTs;    // when suspended (for idle timeout)
    std::size_t memoryUsage; // last reported memory usage
    uint64_t  blocksProduced;
    uint64_t  heartbeat;
};

} // namespace hathor::audio_worker
