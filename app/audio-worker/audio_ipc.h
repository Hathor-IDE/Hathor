// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * audio_ipc.h — cross-process audio transport contract (validated by B4-K0.6).
 *
 * This header defines the shared-memory layout and constants used by both
 * hathor-audio-worker (producer) and the main Hathor process (consumer).
 *
 * The transport contract is identical to the one validated by the B4-K0.6
 * spike in spikes/b4-k0-6/audio_ipc.h.  No new protocol is introduced here.
 *
 * Architecture:
 *   - Control plane: Unix domain socket (lifecycle, commands, status, liveness)
 *   - Audio plane:   POSIX shared memory (shm_open + mmap MAP_SHARED)
 *
 * The SharedAudioTransport struct lives in shared memory.  The main process
 * sets the generation before spawning the worker; the worker reads it at
 * startup and never increments it (the main process manages generation changes
 * on worker replacement).
 *
 * Seqlock discipline for AudioBlock:
 *   Writer:  sequence.store(seq | 1, release);  // odd = in-progress
 *            ... fill samples[] ...
 *            sequence.store(seq + 2, release);   // even = complete
 *
 *   Reader:  s0 = sequence.load(acquire);
 *            if (s0 & 1) -> discard (write in progress)
 *            copy samples[]
 *            s1 = sequence.load(acquire);
 *            if (s1 != s0) -> discard (torn read)
 *            else -> valid block
 *
 * Control-plane VM command protocol (B4-K3):
 *   Commands are newline-delimited ASCII strings sent over the Unix domain
 *   socket.  The worker responds with "ok" or "err" prefixed lines.
 *
 *   vm_activate <tabId> <sampleRate> <channels>
 *       -> "ok vm_activated tab=<tabId> state=active"
 *       -> "err vm_activate_failed tab=<tabId> <reason>"
 *
 *   vm_deactivate <tabId> [suspend|destroy]
 *       -> "ok vm_deactivated tab=<tabId> state=<suspended|destroyed>"
 *       -> "err vm_deactivate_failed tab=<tabId> <reason>"
 *
 *   vm_resume <tabId>
 *       -> "ok vm_resumed tab=<tabId>"
 *       -> "err vm_resume_failed tab=<tabId> <reason>"
 *
 *   vm_compile <tabId> <code-length>\n<code>
 *       -> "ok vm_compiled tab=<tabId> shreds=<n>"
 *       -> "err vm_compile_failed tab=<tabId> <error-text>"
 *
 *   vm_query <tabId>
 *       -> "ok vm_state tab=<tabId> state=<inactive|active| suspended|destroyed|error|failed|recreating>"
 *          + " blocks=<n> heartbeat=<n> memory=<bytes> gen=<g>"
 *       -> "err vm_query_failed tab=<tabId> <reason>"
 *
 *   vm_hang_status [all|<tabId>]
 *       -> "ok vm_hang_status events=<n> <event-list>"  (for "all")
 *       -> "ok vm_hang_status tab=<tabId> old_gen=<g> hb=<n> new_gen=<g> recovered=<0|1> restarts=<n>"
 *       -> "ok vm_hang_status tab=<tabId> no_hang_events"
 *       -> "err ..."
 *       (B4-K5: queried by the main process for UI notification)
 *
 *   watchdog_status
 *       -> "ok watchdog monitored=<n> total_detections=<n>"
 *       (B4-K5: general watchdog health)
 *
 *   vm_list
 *       -> "ok vm_list count=<n> active=<n> suspended=<n> destroyed=<n>"
 *
 * Requirements: B4-K0.6 (transport contract), B4-K2 (generation identity),
 *               B4-K3 (per-tab VM isolation), B4-K8 (hard gate tests)
 */

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace hathor::audio_worker {

// ---------------------------------------------------------------------------
// Constants (identical to B4-K0.6 spike)
// ---------------------------------------------------------------------------

/// Number of audio samples per transport block.
constexpr uint32_t kBlockSize     = 64;

/// Ring capacity in blocks (must be power-of-two for bitmask indexing).
constexpr uint32_t kRingCapacity  = 256;
constexpr uint32_t kRingMask      = kRingCapacity - 1;

/// Magic value written when shared memory is initialised.
constexpr uint32_t kMagic         = 0xB4A7D006;

/// Shared-memory object name (POSIX shm_open name, must start with '/').
inline constexpr const char* kShmName   = "/hathor-audio-worker";

/// Control-plane Unix domain socket path (filesystem socket).
inline constexpr const char* kControlName = "hathor-audio-worker-control";

// ---------------------------------------------------------------------------
// VM lifecycle constants (B4-K3)
// ---------------------------------------------------------------------------

/// Maximum number of tab slots (matches AudioEngine::kNumSlots).
constexpr int kMaxTabs = 16;

/// TabId is the stable tab identity: the slot index [0, kMaxSlots-1].
/// This matches the existing SlotState and HathorTab::slotIndex_ which
/// are in the range [0, AudioEngine::kNumSlots) = [0, 16).
using TabId = uint8_t;

/// Default heartbeat timeout for watchdog (milliseconds).
/// Per PROGRAM.md B4-K5: ~2s staleness = hung VM.
/// The actual timeout is configurable via ResourcePolicy.
constexpr int kDefaultHeartbeatTimeoutMs = 2000;

/// Watchdog check interval (milliseconds).
/// The watchdog thread polls VM heartbeats at this cadence.
constexpr int kDefaultWatchdogIntervalMs = 500;

/// Maximum restart attempts before marking a tab as permanently failed.
/// Per B4-K5 §RESTART_LOOP_PROTECTION.
constexpr int kMaxRestartAttempts = 5;

/// Cooldown between restart attempts for the same tab (milliseconds).
/// Prevents restart storms when a tab immediately hangs again.
constexpr int kRestartCooldownMs = 1000;

/// Alias used by VmLifecycle/ChuckCompiler (lowercase per existing codebase).
inline constexpr int kNumTabs = kMaxTabs;

// ---------------------------------------------------------------------------
// VM lifecycle state (B4-K3 — used by both worker and main process)
// ---------------------------------------------------------------------------

/// Per-VM lifecycle states (Decision #24: explicit, not implicit per-file).
/// Defined here in the IPC header so both the worker process and the main
/// process share the same enumeration without a JUCE dependency.
///
/// B4-K5 extends this with hang-detection states:
///   - Failed:    VM was detected as hung or hit an error; recovery pending.
///   - Recreating: VM is being torn down and a fresh one created.
enum class VMState : uint8_t {
    Inactive,    ///< No VM allocated (tab is open but not playing/eval'd).
    Active,      ///< VM exists and is running on its dedicated thread.
    Suspended,   ///< VM paused deterministically; Chuck instance alive, thread blocked.
    Destroyed,   ///< VM fully torn down; metadata retained for re-creation.
    Error,       ///< VM hit a fatal error; needs restart.
    Failed,      ///< VM was detected as hung (B4-K5); recovery pending.
    Recreating,  ///< VM is being torn down and a fresh one is being created (B4-K5).
};

/// Lowercase alias used throughout the existing worker code (VmLifecycle,
/// ChuckCompiler, hathor-audio-worker).  This avoids renaming all references.
using VmState = VMState;

/// Result of a VM control operation.
struct VMResult {
    bool     ok            = false;
    unsigned errorCode     = 0;
    std::string message;
};

// ---------------------------------------------------------------------------
// Compiled shred — result of ChucK compilation (B4-K4 handoff unit)
// ---------------------------------------------------------------------------

/**
 * CompiledShred — the atomic handoff unit from compile dispatcher to VM.
 *
 * Published via std::atomic_store_explicit(release) on ChuckVmEntry::handoffShred
 * and consumed via std::atomic_load_explicit(acquire) on the per-tab render thread.
 * This matches the AudioEngine::slots_ handoff pattern (atomic_store/load on
 * shared_ptr, Apple-Clang compatible free-function API).
 */
struct CompiledShred {
    bool     ok                = false;  ///< compilation succeeded
    uint32_t requestVersion    = 0;      ///< version tag for stale-result rejection
    uint64_t vmGeneration      = 0;      ///< the VM generation this result targets
    std::string sourceCode;              ///< original source (for hash / debugging)
    std::size_t sourceHash     = 0;      ///< FNV-1a hash of source
    int        loadedShredId   = -1;     ///< assigned by VM on consumption
    std::string error;                   ///< error text if ok==false
    int        errorLine      = 0;       ///< compiler error line if ok==false
    int        errorColumn    = 0;       ///< compiler error column if ok==false
};

/**
 * ChuckVmEntry — a single slot in the fixed-size VM table (VmLifecycle).
 *
 * The VM table is a fixed array of ChuckVmEntry (kNumTabs entries).
 * Structural modifications (create/destroy/version bump) are guarded by
 * vmTableMtx_.  The handoffShred field is read lock-free by the render
 * thread and written by the compile dispatcher via atomic store/load.
 */
struct ChuckVmEntry {
    /// Tab identity (slot index).  Immutable after construction.
    TabId tabId = 0;

    /// VM generation counter.  Increments on every create/replace/destroy.
    /// In-flight compile results for a stale generation are rejected.
    std::atomic<uint64_t> vmGeneration{0};

    /// Current lifecycle state (active/suspended/destroyed/error).
    std::atomic<VMState> state{VMState::Inactive};

    /// Per-tab request version (monotonic).  Bumped on each compile request.
    /// The VM render thread checks that a handoff result's requestVersion
    /// matches currentRequestVersion before accepting it.
    std::atomic<uint32_t> currentRequestVersion{0};

    /// Source hash of the currently loaded shred (0 if none).
    std::atomic<std::size_t> loadedSourceHash{0};

    /// Loaded shred ID (-1 if none loaded).
    std::atomic<int> loadedShredId{-1};

    /// Atomic handoff slot: compile dispatcher publishes here, render thread
    /// consumes here.  Uses std::atomic_store/load_explicit on shared_ptr
    /// (Apple-Clang compatible free-function API).
    std::shared_ptr<CompiledShred> handoffShred;

    ChuckVmEntry() = default;
    ChuckVmEntry(const ChuckVmEntry&) = delete;
    ChuckVmEntry& operator=(const ChuckVmEntry&) = delete;
};

// ---------------------------------------------------------------------------
// AudioBlock — one transport block (seqlock-protected)
// ---------------------------------------------------------------------------

struct AudioBlock {
    /// Aligned to cache line to avoid false sharing between producer and
    /// consumer.  64 bytes is standard on x86_64 and ARM64.
    alignas(64) float samples[kBlockSize];

    /// Seqlock counter.  Even = complete/idle; Odd = write in progress.
    /// Writer increments by 2 per completed block.
    std::atomic<uint32_t> sequence{0};
};

// ---------------------------------------------------------------------------
// SharedAudioTransport — the shared-memory segment header + ring
// ---------------------------------------------------------------------------

struct SharedAudioTransport {
    // -- Identification --
    std::atomic<uint32_t> magic{kMagic};    ///< initialization marker
    std::atomic<uint64_t> generation{0};    ///< worker session/generation identity

    // -- Producer/consumer coordination --
    std::atomic<uint32_t> writeSeq{0};      ///< producer publish counter
    std::atomic<uint32_t> readSeq{0};       ///< consumer consume counter

    // -- Audio format --
    std::atomic<uint32_t> sampleRate{44100};
    std::atomic<uint32_t> channels{1};

    // -- Liveness / heartbeat --
    std::atomic<bool>     workerAlive{false};  ///< set false on clean shutdown
    std::atomic<uint64_t> lastHeartbeat{0};    ///< incrementing counter; staleness = death

    // -- Writer-side RT instrumentation (best-effort, producer only) --
    std::atomic<uint64_t> wrCount{0};
    std::atomic<uint64_t> wrSumNs{0};
    std::atomic<uint64_t> wrMaxNs{0};
    std::atomic<uint64_t> wrMinNs{0};

    // -- The ring buffer of audio blocks --
    AudioBlock blocks[kRingCapacity];

    // -- Compile-time size info --
    static constexpr size_t blockSize()     { return kBlockSize; }
    static constexpr size_t ringCapacity()  { return kRingCapacity; }
};

// ---------------------------------------------------------------------------
// Post-definition constants (depend on the struct layout above)
// ---------------------------------------------------------------------------

/// Total size of the shared-memory segment.
inline constexpr size_t kShmSize = sizeof(SharedAudioTransport);

// Note: AudioBlock has alignas(64) on samples[64] (256 bytes) + atomic<uint32_t>,
// giving sizeof(AudioBlock) = 260, padded to 320 bytes by the 64-byte alignment.
// The original K0.6 spike did not enforce a static_assert on this; the layout
// is validated structurally by the shared-memory size check at init time.
static_assert((kRingCapacity & kRingMask) == 0,
              "kRingCapacity must be a power of two");

} // namespace hathor::audio_worker
