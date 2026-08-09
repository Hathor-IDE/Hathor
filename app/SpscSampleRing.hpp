// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * SpscSampleRing.hpp — lock-free SPSC ring of raw float audio samples.
 *
 * Reuses the SpscRingBuffer template/pattern from VisualizerFrame.hpp:
 *   - power-of-two capacity (compile-time constant)
 *   - single-producer / single-consumer discipline
 *   - std::atomic<uint32_t> indices with memory_order_release/acquire
 *   - per-slot seqlock sequence counters (odd = in-progress, even = complete)
 *   - cache-line-aligned atomic indices (alignas(64))
 *   - overflow: drops oldest sample (no back-pressure, no block)
 *   - underrun: returns false (caller fills silence)
 *   - no heap allocation, no mutex, no blocking in steady state
 *
 * Ownership:
 *   Producer thread  — push() only.  Never touches readIdx_ except when
 *                      the ring is full (advances readIdx_ to discard oldest).
 *   Consumer thread  — pop() / popMany() only.  Never touches writeIdx_.
 *
 * Memory ordering:
 *   writeIdx_ — producer stores with release after writing the slot;
 *               consumer loads with acquire to observe completed writes.
 *   readIdx_  — consumer stores with release after reading the slot;
 *               producer loads with acquire only when detecting overflow.
 *   seq_[]    — per-slot seqlock: producer stores seq|1 (release) before
 *               writing, seq+2 (release) after; consumer loads with acquire
 *               before and after reading, discards if torn or in-progress.
 *
 * B4-K1 establishes this ring-family pattern.  B4-K6 reuses the same
 * pattern for timestamped musical events (separate ring type, same SPSC
 * discipline).  Cross-process/shared-memory transport is validated
 * separately by B4-K0.6.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hathor {

// ---------------------------------------------------------------------------
// SpscSampleRing<Capacity> — SPSC lock-free ring of float samples
// ---------------------------------------------------------------------------
//
// Capacity must be a power of two (enforced by static_assert).
// All storage is inline — no heap, no shared_ptr.
//
// push()  — producer thread only.  Never blocks; drops oldest sample when
//           the ring is full.
// pop()   — consumer thread only.  Returns false if no sample available.
// popMany — consumer thread only.  Reads up to maxOut samples; returns
//           the count actually read (caller zeros buffer for silence).
//
// writeIdx_ and readIdx_ are placed on separate cache lines to prevent
// false sharing between producer and consumer.
// ---------------------------------------------------------------------------

template <std::size_t Capacity>
class SpscSampleRing {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "SpscSampleRing: Capacity must be a power of two");
    static_assert(Capacity >= 2,
                  "SpscSampleRing: Capacity must be at least 2");
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "SpscSampleRing: std::atomic<uint32_t> must be lock-free");

public:
    SpscSampleRing()  = default;
    ~SpscSampleRing() = default;

    // Non-copyable / non-movable (contains atomics and inline array).
    SpscSampleRing(const SpscSampleRing&)            = delete;
    SpscSampleRing& operator=(const SpscSampleRing&) = delete;
    SpscSampleRing(SpscSampleRing&&)                 = delete;
    SpscSampleRing& operator=(SpscSampleRing&&)      = delete;

    // -----------------------------------------------------------------------
    // push() — producer thread only (B4-K1: no back-pressure)
    //
    // Writes a single float sample.  If the ring is full (producer has lapped
    // the consumer), drops the oldest unread sample by advancing readIdx_.
    // The per-slot seqlock ensures the consumer never observes a torn write.
    // -----------------------------------------------------------------------
    void push(float sample) noexcept
    {
        const uint32_t wIdx = writeIdx_.load(std::memory_order_relaxed);
        const uint32_t slot = wIdx & kMask;

        // Seqlock: mark in-progress (odd sequence).
        uint32_t seq = seq_[slot].load(std::memory_order_relaxed);
        seq_[slot].store(seq | 1u, std::memory_order_release);

        // Write payload.
        buf_[slot] = sample;

        // Seqlock: mark complete (even, incremented by 2).
        seq_[slot].store(seq + 2u, std::memory_order_release);

        // Advance write index.
        const uint32_t nextW = wIdx + 1u;
        writeIdx_.store(nextW, std::memory_order_release);

        // Lap detection: if producer has advanced more than Capacity items
        // past the consumer, the ring is full — silently discard oldest by
        // advancing readIdx_ (drop-oldest overflow policy).
        //
        // Using subtraction on uint32_t indices: nextW - rIdx is the number of
        // items in the ring after this push.  When it exceeds Capacity, the
        // oldest slot has been (or is about to be) overwritten.
        const uint32_t rIdx = readIdx_.load(std::memory_order_acquire);
        if (nextW - rIdx > Capacity) {
            readIdx_.store(rIdx + 1u, std::memory_order_release);
        }
    }

    // -----------------------------------------------------------------------
    // pop() — consumer thread only
    //
    // Returns true and sets @p sample if a new valid sample is available;
    // returns false if the ring is empty (caller should treat as silence).
    //
    // Seqlock validation: discards samples with odd or mismatched sequence
    // (torn read / in-progress write) rather than retrying — the consumer
    // will try again on the next callback.
    // -----------------------------------------------------------------------
    bool pop(float& sample) noexcept
    {
        const uint32_t rIdx = readIdx_.load(std::memory_order_relaxed);
        const uint32_t wIdx = writeIdx_.load(std::memory_order_acquire);

        // Empty check: read index has caught up with write index.
        if (rIdx == wIdx)
            return false;

        const uint32_t slot = rIdx & kMask;

        // Seqlock: read sequence before copying.
        const uint32_t s0 = seq_[slot].load(std::memory_order_acquire);
        if (s0 & 1u)
            return false; // Write in progress — discard.

        // Copy payload.
        sample = buf_[slot];

        // Seqlock: read sequence after copying.
        const uint32_t s1 = seq_[slot].load(std::memory_order_acquire);
        if (s1 != s0)
            return false; // Torn read — discard.

        // Valid sample — advance read index.
        readIdx_.store(rIdx + 1u, std::memory_order_release);
        return true;
    }

    // -----------------------------------------------------------------------
    // popMany() — consumer thread only
    //
    // Reads up to @p maxOut samples into @p out[].
    // Returns the number of samples actually read (0 .. maxOut).
    // Caller zeroes @p out[] first so missing samples are 0.0f (silence).
    // -----------------------------------------------------------------------
    std::size_t popMany(float* out, std::size_t maxOut) noexcept
    {
        std::size_t count = 0;
        while (count < maxOut) {
            if (!pop(out[count]))
                break;
            ++count;
        }
        return count;
    }

    // -----------------------------------------------------------------------
    // reset() — return ring to empty state (testing / reinit only)
    // -----------------------------------------------------------------------
    void reset() noexcept
    {
        writeIdx_.store(0, std::memory_order_relaxed);
        readIdx_.store(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < Capacity; ++i)
            seq_[i].store(0, std::memory_order_relaxed);
    }

private:
    static constexpr uint32_t kMask = static_cast<uint32_t>(Capacity) - 1u;

    // Inline storage — no heap (B4-K1: allocation-free).
    std::array<float, Capacity> buf_;

    // Per-slot seqlock sequence counters (reuses VisualizerFrame pattern).
    std::array<std::atomic<uint32_t>, Capacity> seq_;

    // Producer and consumer indices on separate cache lines to prevent
    // false sharing (reuses VisualizerFrame pattern).
    alignas(64) std::atomic<uint32_t> writeIdx_{0};
    alignas(64) std::atomic<uint32_t> readIdx_{0};
};

} // namespace hathor
