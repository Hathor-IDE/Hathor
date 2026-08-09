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
 * Requirements: B4-K0.6 (transport contract), B4-K2 (generation identity),
 *               B4-K8 (hard gate tests)
 */

#include <atomic>
#include <cstdint>
#include <cstring>

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
