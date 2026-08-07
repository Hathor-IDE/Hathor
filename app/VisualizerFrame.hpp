// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * VisualizerFrame.hpp — lock-free audio-to-UI data path.
 *
 * VisualizerFrame: one snapshot of playback state published by the audio
 * thread each callback via a seqlock discipline (Req 28.2, 28.3, 28.6).
 *
 * SpscRingBuffer<Capacity>: fixed-size, lock-free, single-producer /
 * single-consumer ring buffer of VisualizerFrame slots (Req 28.1, 28.4,
 * 28.7, 30.2).
 *
 * Requirements: 28.1–28.8, 30.2
 */

#include "hathor/Event.hpp"
#include "hathor/ParamMap.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace hathor {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Maximum events stored per VisualizerFrame (Req 28.2).
inline constexpr std::size_t kMaxFrameEvents = 64;

// ---------------------------------------------------------------------------
// VisualizerFrame — one callback's worth of visualizer data (Req 28.2, 28.6)
// ---------------------------------------------------------------------------
//
// Write protocol (audio thread — seqlock):
//   1. sequence.store(seq | 1, release)          // mark in-progress (odd)
//   2. copy cyclePos, eventCount, events[0..eventCount-1]  // partial copy
//   3. sequence.store(seq + 2, release)           // mark complete (even)
//
// Read protocol (UI timer):
//   uint32_t s0 = sequence.load(acquire);
//   if (s0 & 1) → discard (write in progress)
//   copy cyclePos, eventCount, events[0..eventCount-1]
//   uint32_t s1 = sequence.load(acquire);
//   if (s1 != s0) → discard (torn read)
//   else → valid frame
//
// NOTE: VisualizerFrame is not copyable (contains std::atomic).
// SpscRingBuffer uses an inline std::array and operates in-place.
// ---------------------------------------------------------------------------

struct VisualizerFrame {
    /// Seqlock counter. Even = valid/idle; Odd = write in progress.
    /// Increments by 2 per completed write. (Req 28.6)
    std::atomic<uint32_t> sequence{0};

    /// Current cycle position (Req 9.4 double-conversion formula).
    double cyclePos = 0.0;

    /// Number of valid entries in events[] (always ≤ kMaxFrameEvents).
    uint32_t eventCount = 0;

    /// Event payload — only entries [0, eventCount) are valid.
    std::array<Event<ParamMap>, kMaxFrameEvents> events{};

    // Non-copyable (atomic member); ring buffer manages slots in-place.
    VisualizerFrame()                                  = default;
    VisualizerFrame(const VisualizerFrame&)            = delete;
    VisualizerFrame& operator=(const VisualizerFrame&) = delete;
    VisualizerFrame(VisualizerFrame&&)                 = delete;
    VisualizerFrame& operator=(VisualizerFrame&&)      = delete;
};

// ---------------------------------------------------------------------------
// SpscRingBuffer<Capacity> — SPSC lock-free ring of VisualizerFrames
// ---------------------------------------------------------------------------
//
// Capacity must be a power of two (enforced by static_assert).
// All storage is inline in a std::array — no heap, no shared_ptr (Req 28.7,
// 30.2).
//
// write() — audio thread only. Never blocks; overwrites oldest slot when
//           the buffer is full (no back-pressure, Req 28.4).
// read()  — UI timer thread only. Returns false if no new frame available.
//
// writeIdx_ and readIdx_ are placed on separate cache lines to prevent
// false sharing between producer and consumer (Req 28.7).
// ---------------------------------------------------------------------------

template <std::size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "SpscRingBuffer: Capacity must be a power of two");
    static_assert(Capacity >= 2,
                  "SpscRingBuffer: Capacity must be at least 2");

public:
    SpscRingBuffer()  = default;
    ~SpscRingBuffer() = default;

    // Non-copyable (contains atomics and a large inline array).
    SpscRingBuffer(const SpscRingBuffer&)            = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&)                 = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&)      = delete;

    // -----------------------------------------------------------------------
    // write() — audio thread only (Req 28.3, 28.4, 28.8)
    //
    // Writes a frame using seqlock discipline:
    //   1. Store odd sequence (release) → signals write-in-progress.
    //   2. Copy cyclePos, eventCount, and only eventCount events (partial
    //      copy — NOT the full kMaxFrameEvents array).
    //   3. Store seq+2 (even, release) → signals write-complete.
    //
    // Overwrites the oldest slot when full — never blocks or allocates.
    // -----------------------------------------------------------------------
    void write(double cyclePos,
               uint32_t eventCount,
               const Event<ParamMap>* events) noexcept
    {
        // Clamp eventCount defensively.
        if (eventCount > static_cast<uint32_t>(kMaxFrameEvents))
            eventCount = static_cast<uint32_t>(kMaxFrameEvents);

        const uint32_t wIdx = writeIdx_.load(std::memory_order_relaxed);
        const uint32_t slot = wIdx & kMask;

        VisualizerFrame& frame = buf_[slot];

        // Seqlock: mark in-progress (odd sequence).
        const uint32_t seq = frame.sequence.load(std::memory_order_relaxed);
        frame.sequence.store(seq | 1u, std::memory_order_release);

        // Copy payload — partial copy only (Req 28.8).
        frame.cyclePos   = cyclePos;
        frame.eventCount = eventCount;
        for (uint32_t i = 0; i < eventCount; ++i)
            frame.events[i] = events[i];

        // Seqlock: mark complete (even, incremented by 2).
        frame.sequence.store(seq + 2u, std::memory_order_release);

        // Advance write index. If we're lapping the reader, advance readIdx_
        // too so the oldest slot is overwritten (back-pressure free).
        const uint32_t nextW = wIdx + 1u;
        writeIdx_.store(nextW, std::memory_order_release);

        // Lap detection: if nextW == readIdx_ (wrapped) the reader is stalled.
        // Advance readIdx_ by 1 to discard the oldest unread frame (Req 28.4).
        const uint32_t rIdx = readIdx_.load(std::memory_order_acquire);
        if ((nextW & kMask) == (rIdx & kMask) && nextW != rIdx) {
            // Full — silently discard oldest frame by advancing reader.
            readIdx_.store(rIdx + 1u, std::memory_order_release);
        }
    }

    // -----------------------------------------------------------------------
    // read() — UI timer thread only (Req 28.3, 28.5)
    //
    // Returns true and fills the output parameters if a new valid frame is
    // available; returns false if the buffer is empty.
    //
    // Seqlock validation: discards frames with odd or mismatched sequence
    // (torn read) rather than retrying — the UI timer will try again next
    // tick (Req 28.5).
    // -----------------------------------------------------------------------
    bool read(double& cyclePos,
              uint32_t& eventCount,
              Event<ParamMap>* eventsOut) noexcept
    {
        const uint32_t rIdx = readIdx_.load(std::memory_order_relaxed);
        const uint32_t wIdx = writeIdx_.load(std::memory_order_acquire);

        // Empty check: read index has caught up with write index.
        if (rIdx == wIdx)
            return false;

        const uint32_t slot = rIdx & kMask;
        const VisualizerFrame& frame = buf_[slot];

        // Seqlock: read sequence before copying.
        const uint32_t s0 = frame.sequence.load(std::memory_order_acquire);
        if (s0 & 1u)
            return false; // Write in progress — discard this tick.

        // Copy payload.
        const double   cp = frame.cyclePos;
        const uint32_t ec = frame.eventCount;
        const uint32_t safeEc = (ec <= static_cast<uint32_t>(kMaxFrameEvents))
                                    ? ec : static_cast<uint32_t>(kMaxFrameEvents);
        for (uint32_t i = 0; i < safeEc; ++i)
            eventsOut[i] = frame.events[i];

        // Seqlock: read sequence after copying.
        const uint32_t s1 = frame.sequence.load(std::memory_order_acquire);
        if (s1 != s0)
            return false; // Torn read — discard.

        // Valid frame — advance read index and return data.
        readIdx_.store(rIdx + 1u, std::memory_order_release);
        cyclePos   = cp;
        eventCount = safeEc;
        return true;
    }

private:
    static constexpr uint32_t kMask = static_cast<uint32_t>(Capacity) - 1u;

    // All VisualizerFrame storage is inline — no heap (Req 28.7, 30.2).
    std::array<VisualizerFrame, Capacity> buf_;

    // Producer and consumer indices on separate cache lines to prevent
    // false sharing (Req 28.7).
    alignas(64) std::atomic<uint32_t> writeIdx_{0};
    alignas(64) std::atomic<uint32_t> readIdx_{0};
};

} // namespace hathor
