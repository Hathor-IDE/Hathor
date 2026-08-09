// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * EventScheduler.hpp — worker-side timestamped event scheduler for B4-K6.
 *
 * This is the consumer-side logic that receives MusicalEvents from the
 * shared-memory event transport and dispatches them to the appropriate ChucK
 * VM at the correct sample-frame boundary.
 *
 * Per Decision #13: the worker maintains a staging buffer of events that have
 * arrived but not yet executed. Events are moved from staging into the "ready
 * to execute" set when the audio callback reaches the event's target sample
 * timestamp.
 *
 * Per Decision #14: events arriving after their target timestamp are dropped
 * (late events are a transport failure, not musical time). Events arriving
 * before their target are held until the correct sample position.
 *
 * Per Decision #20: the scheduler uses a fixed-capacity inline min-heap
 * (std::array + std::push_heap/pop_heap) keyed by (localExecTs, sequence).
 * This is populated off the RT thread when events arrive, and drained on
 * the RT thread at buffer boundaries. To maintain RT-safety, the heap is
 * allocation-free — no heap allocation in the RT path.
 *
 * Per Decision #24: events are targeted to a specific VM generation. Events
 * targeting a stale generation are rejected.
 *
 * Per Decision #21: scheduling is done at buffer boundaries — events whose
 * target localExecTs falls within the current buffer [bufferStart,
 * bufferEnd) are queued for execution at the appropriate position within
 * the buffer.
 *
 * Per Decision #25: the audio thread only drains from the pre-populated
 * staging heap via getReadyEvents(). No atomic operations, no syscalls,
 * no allocation in the RT path. The staging heap population (stageEvents)
 * runs off the RT thread (called by the per-tab render thread, not the
 * JUCE audio callback).
 *
 * Requirement: B4-K6 EventScheduler
 */

#include "MusicalEvent.hpp"
#include "SpscEventRing.hpp"
#include "ClockSync.hpp"
#include "EventTransport.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <array>

namespace hathor {

class EventScheduler {
public:
    static constexpr size_t kMaxStagedEvents = 1024; ///< max events held for future execution
    static constexpr int64_t kLateGraceSamples = 64; ///< grace window before declaring an event late (one 64-sample buffer)

    explicit EventScheduler(double sampleRate = 44100.0)
        : m_sampleRate(sampleRate)
        , m_localSampleCursor(0)
        , m_currentVmGeneration(0)
        , m_stagedCount(0)
    {}

    /**
     * Set the current VM generation. Called when the VM is created/renamed.
     * This is called off the RT thread.
     */
    void setCurrentVmGeneration(uint64_t gen) {
        m_currentVmGeneration.store(gen, std::memory_order_release);
    }

    /**
     * Get the current VM generation (for staleness checks).
     */
    uint64_t getCurrentVmGeneration() const {
        return m_currentVmGeneration.load(std::memory_order_acquire);
    }

    /**
     * Process events from the in-process SPSC ring (for testing).
     * Called off the RT thread.
     *
     * Reads events from the ring and places them into the staging buffer
     * (min-heap) sorted by (localExecTs, sequence). Events targeting stale
     * VM generations are dropped. Late events are dropped.
     */
    template<size_t RingCapacity>
    uint32_t stageEvents(SpscEventRing<RingCapacity>& ring,
                         ClockSync& clock) {
        uint32_t staged = 0;
        uint32_t heapSize = m_stagedCount; // local copy; only off-RT thread writes
        uint64_t localNow = m_localSampleCursor.load(std::memory_order_acquire);
        uint64_t currentGen = m_currentVmGeneration.load(std::memory_order_acquire);

        while (heapSize < kMaxStagedEvents) {
            auto eventOpt = ring.pop();
            if (!eventOpt)
                break;

            MusicalEvent event = *eventOpt;

            // --- VM generation guard ---
            // Drop events targeting a stale VM generation
            if (event.vmGeneration < currentGen) {
                std::printf("[EventScheduler] Dropped: stale VM gen (event=%llu, current=%llu, seq=%llu)\n",
                            static_cast<unsigned long long>(event.vmGeneration),
                            static_cast<unsigned long long>(currentGen),
                            static_cast<unsigned long long>(event.sequence));
                continue;
            }
            // Events for future generations: skip (producer should not send
            // ahead of generation activation)
            if (event.vmGeneration > currentGen) {
                continue;
            }

            // --- Convert master timestamp to local execution timestamp ---
            event.localExecTs = clock.masterToLocal(event.sampleTs);

            // --- Late event check ---
            if (static_cast<int64_t>(event.localExecTs) <
                static_cast<int64_t>(localNow) - kLateGraceSamples) {
                std::printf("[EventScheduler] Dropped late: seq=%llu, target=%llu, now=%llu, diff=%lld\n",
                            static_cast<unsigned long long>(event.sequence),
                            static_cast<unsigned long long>(event.localExecTs),
                            static_cast<unsigned long long>(localNow),
                            static_cast<long long>(static_cast<int64_t>(event.localExecTs) - static_cast<int64_t>(localNow)));
                continue;
            }

            // --- Stage the event ---
            m_stagedEvents[heapSize] = event;
            ++heapSize;
            std::push_heap(&m_stagedEvents[0], &m_stagedEvents[heapSize],
                           EventCompare{});
            ++staged;
        }

        m_stagedCount = heapSize;
        return staged;
    }

    /**
     * Process events from the shared-memory event transport (production path).
     * Called off the RT thread by the per-tab render thread.
     *
     * Uses the same logic as the in-process version but reads from the
     * seqlock-protected shared-memory ring.
     */
    uint32_t stageEvents(audio_worker::SharedEventTransport* transport,
                         ClockSync& clock) {
        if (!transport)
            return 0;

        // Validate magic and generation
        const uint32_t magic = transport->magic.load(std::memory_order_acquire);
        if (magic != audio_worker::kEventMagic)
            return 0;

        const uint64_t gen = transport->generation.load(std::memory_order_acquire);
        const uint64_t currentGen = m_currentVmGeneration.load(std::memory_order_acquire);
        if (gen != currentGen)
            return 0; // Transport generation mismatch — skip

        uint32_t staged = 0;
        uint32_t heapSize = m_stagedCount;
        uint64_t localNow = m_localSampleCursor.load(std::memory_order_acquire);

        // Drain the shared-memory event ring using seqlock protocol
        while (heapSize < kMaxStagedEvents) {
            const uint64_t rSeq = transport->readSeq.load(std::memory_order_relaxed);
            const uint64_t wSeq = transport->writeSeq.load(std::memory_order_acquire);

            if (rSeq >= wSeq)
                break; // Ring empty

            const uint32_t slot = rSeq & audio_worker::kEventRingMask;
            const auto& sl = transport->slots[slot];

            // Seqlock: read sequence before copying
            const uint64_t s0 = sl.seq.load(std::memory_order_acquire);
            if (s0 & 1u)
                break; // Write in progress — stop draining for now

            MusicalEvent event = sl.event;

            // Seqlock: read sequence after copying
            const uint64_t s1 = sl.seq.load(std::memory_order_acquire);
            if (s1 != s0)
                continue; // Torn read — skip this slot, move on

            // Advance read sequence (commit the read)
            transport->readSeq.store(rSeq + 1u, std::memory_order_release);

            // --- VM generation guard (re-check per-event) ---
            if (event.vmGeneration < currentGen) {
                continue;
            }
            if (event.vmGeneration > currentGen) {
                continue;
            }

            // --- Convert master timestamp to local execution timestamp ---
            event.localExecTs = clock.masterToLocal(event.sampleTs);

            // --- Late event check ---
            if (static_cast<int64_t>(event.localExecTs) <
                static_cast<int64_t>(localNow) - kLateGraceSamples) {
                std::printf("[EventScheduler] Dropped late (shm): seq=%llu, target=%llu, now=%llu\n",
                            static_cast<unsigned long long>(event.sequence),
                            static_cast<unsigned long long>(event.localExecTs),
                            static_cast<unsigned long long>(localNow));
                continue;
            }

            // --- Stage the event ---
            m_stagedEvents[heapSize] = event;
            ++heapSize;
            std::push_heap(&m_stagedEvents[0], &m_stagedEvents[heapSize],
                           EventCompare{});
            ++staged;
        }

        m_stagedCount = heapSize;
        return staged;
    }

    /**
     * Get events ready to fire within a given buffer range.
     * Called from the audio callback (RT thread).
     *
     * @param bufferStart  local sample position of buffer start
     * @param bufferFrames number of frames in the current buffer
     * @param[out] outEvents     array to write ready events into (caller-allocated)
     * @param[out] outEventCount number of ready events written
     * @param maxCount          capacity of outEvents array
     * @return true if all ready events were consumed within capacity, false if overflow
     *
     * RT-safe: lock-free and allocation-free (no heap allocation, no syscalls,
     * no blocking). Only reads/writes to the pre-allocated m_stagedEvents array
     * and the output buffer.
     */
    bool getReadyEvents(uint64_t bufferStart, uint32_t bufferFrames,
                        MusicalEvent* outEvents, uint32_t* outEventCount, uint32_t maxCount) noexcept {
        const uint64_t bufferEnd = bufferStart + bufferFrames;
        uint32_t count = 0;
        uint32_t heapSize = m_stagedCount;

        while (heapSize > 0 && count < maxCount) {
            const MusicalEvent& top = m_stagedEvents[0];
            if (top.localExecTs >= bufferEnd)
                break; // not yet due

            if (top.localExecTs < bufferStart) {
                // Should not happen if staging caught late events, but guard
                std::pop_heap(&m_stagedEvents[0], &m_stagedEvents[heapSize], EventCompare{});
                --heapSize;
                continue;
            }

            // Event is ready: falls within this buffer
            outEvents[count++] = top;
            std::pop_heap(&m_stagedEvents[0], &m_stagedEvents[heapSize], EventCompare{});
            --heapSize;
        }

        m_stagedCount = heapSize;
        *outEventCount = count;
        return (count < maxCount) || (heapSize == 0);
    }

    /**
     * Update the local sample cursor. Called at the start of each audio buffer
     * (off the RT thread, before staging events).
     */
    void setSampleCursor(uint64_t cursor) noexcept {
        m_localSampleCursor.store(cursor, std::memory_order_release);
    }

    /**
     * @return current local sample cursor
     */
    uint64_t getSampleCursor() const noexcept {
        return m_localSampleCursor.load(std::memory_order_acquire);
    }

    /**
     * @return number of events currently staged in the scheduling heap
     */
    uint32_t numStagedEvents() const noexcept {
        return m_stagedCount;
    }

    /**
     * Clear all staged events (called on VM restart or shutdown).
     */
    void clearStagedEvents() noexcept {
        m_stagedCount = 0;
    }

private:
    struct EventCompare {
        // For std::push_heap/pop_heap: returns true if a has LOWER priority
        // (i.e., a should come AFTER b in the heap).
        // For a min-heap (earliest localExecTs first), "a has lower priority"
        // means a should come later, i.e., a > b.
        bool operator()(const MusicalEvent& a, const MusicalEvent& b) const noexcept {
            if (a.localExecTs != b.localExecTs)
                return a.localExecTs > b.localExecTs;
            return a.sequence > b.sequence;
        }
    };

    double m_sampleRate;
    std::atomic<uint64_t> m_localSampleCursor;
    std::atomic<uint64_t> m_currentVmGeneration;

    // Staging buffer: fixed-capacity min-heap of events.
    // std::array provides allocation-free storage (no heap growth).
    // Populated off-RT-thread, consumed RT-thread.
    std::array<MusicalEvent, kMaxStagedEvents> m_stagedEvents;
    uint32_t m_stagedCount;
};

} // namespace hathor
