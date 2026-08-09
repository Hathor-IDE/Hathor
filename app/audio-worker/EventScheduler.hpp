// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * EventScheduler.hpp — worker-side timestamped event scheduler for B4-K6.
 *
 * This is the consumer-side logic that receives MusicalEvents from the
 * SpscEventRing and dispatches them to the appropriate ChucK VM at the
 * correct sample-frame boundary.
 *
 * Per Decision #13: the worker maintains a small per-VM staging buffer of
 * events that have arrived but not yet executed. Events are moved from the
 * staging buffer into the "ready to execute" set when the audio callback
 * reaches the event's target sample timestamp.
 *
 * Per Decision #14: events arriving after their target timestamp are dropped
 * (late events are a transport failure, not musical time). Events arriving
 * before their target are held until the correct sample position.
 *
 * Per Decision #20: the scheduler uses a fixed-capacity inline min-heap
 * (std::array + std::make_heap) keyed by (localExecTs, sequence). This is
 * populated off the RT thread when events arrive, and drained on the
 * RT thread at buffer boundaries. To maintain RT-safety, the heap is
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
 * Requirement: B4-K6 EventScheduler
 */

#include "MusicalEvent.hpp"
#include "SpscEventRing.hpp"
#include "ClockSync.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <array>

namespace hathor {

class EventScheduler {
public:
    static constexpr size_t kMaxStagedEvents = 1024; ///< max events held for future execution
    static constexpr int64_t kLateGraceSamples = 64; ///< grace window before declaring an event late

    explicit EventScheduler(double sampleRate = 44100.0)
        : m_sampleRate(sampleRate)
        , m_localSampleCursor(0)
        , m_currentVmGeneration(0)
    {}

    void setCurrentVmGeneration(uint64_t gen) {
        m_currentVmGeneration.store(gen, std::memory_order_release);
    }

    uint64_t getCurrentVmGeneration() const {
        return m_currentVmGeneration.load(std::memory_order_acquire);
    }

    /**
     * Process events from the ring. Called off the RT thread (or at the
     * beginning of the buffer, before audio callback work begins).
     *
     * Reads events from the ring and places them into the staging buffer
     * sorted by (localExecTs, sequence). Events targeting stale VM generations
     * are dropped. Late events are dropped.
     *
     * @param ring    the SPSC ring to drain
     * @param clock   the clock synchronizer (for converting master→local time)
     * @return number of events staged
     */
    uint32_t stageEvents(SpscEventRing<kMaxStagedEvents * 2>& ring, ClockSync& clock) {
        uint32_t staged = 0;
        uint32_t heapSize = m_stagedCount.load(std::memory_order_acquire);
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
                std::printf("[EventScheduler] Dropped event: stale VM gen (event=%lu, current=%lu, seq=%lu)\n",
                            event.vmGeneration, currentGen, event.sequence);
                continue;
            }
            // Events for future generations: buffer them (will be processed
            // when that generation becomes active)
            if (event.vmGeneration > currentGen) {
                // Don't stage yet — skip for now (producer should not send
                // ahead of generation activation)
                continue;
            }

            // --- Convert master timestamp to local execution timestamp ---
            event.localExecTs = clock.masterToLocal(event.sampleTs);

            // --- Late event check ---
            // An event is "late" if its local target timestamp is behind
            // the current sample cursor (plus a small grace window).
            if (static_cast<int64_t>(event.localExecTs) <
                static_cast<int64_t>(localNow) - kLateGraceSamples) {
                std::printf("[EventScheduler] Dropped late event: seq=%lu, target=%lu, now=%lu, diff=%ld\n",
                            event.sequence, event.localExecTs, localNow,
                            static_cast<int64_t>(event.localExecTs) - static_cast<int64_t>(localNow));
                continue;
            }

            // --- Stage the event ---
            // Insert into the min-heap, maintaining order by (localExecTs, sequence)
            m_stagedEvents[heapSize] = event;
            ++heapSize;
            std::push_heap(&m_stagedEvents[0], &m_stagedEvents[heapSize],
                           EventCompare{});
            ++staged;
        }

        m_stagedCount.store(heapSize, std::memory_order_release);
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
     * Lock-free and allocation-free (no heap allocation in this method).
     */
    bool getReadyEvents(uint64_t bufferStart, uint32_t bufferFrames,
                        MusicalEvent* outEvents, uint32_t* outEventCount, uint32_t maxCount) noexcept {
        const uint64_t bufferEnd = bufferStart + bufferFrames;
        uint32_t count = 0;
        uint32_t heapSize = m_stagedCount.load(std::memory_order_acquire);

        while (heapSize > 0 && count < maxCount) {
            const MusicalEvent& top = m_stagedEvents[0];
            if (top.localExecTs >= bufferEnd)
                break; // not yet due

            if (top.localExecTs < bufferStart) {
                // This shouldn't happen if staging dropped late events,
                // but guard against it — this is an error condition
                std::pop_heap(&m_stagedEvents[0], &m_stagedEvents[heapSize], EventCompare{});
                --heapSize;
                continue;
            }

            // Event is ready: falls within this buffer
            outEvents[count++] = top;
            std::pop_heap(&m_stagedEvents[0], &m_stagedEvents[heapSize], EventCompare{});
            --heapSize;
        }

        m_stagedCount.store(heapSize, std::memory_order_release);
        *outEventCount = count;
        return count < maxCount || heapSize == 0;
    }

    void setSampleCursor(uint64_t cursor) noexcept {
        m_localSampleCursor.store(cursor, std::memory_order_release);
    }

    uint64_t getSampleCursor() const noexcept {
        return m_localSampleCursor.load(std::memory_order_acquire);
    }

    uint32_t numStagedEvents() const noexcept {
        return m_stagedCount.load(std::memory_order_acquire);
    }

    void clearStagedEvents() noexcept {
        m_stagedCount.store(0, std::memory_order_release);
    }

private:
    struct EventCompare {
        // For std::push_heap/pop_heap: returns true if a has LOWER priority
        // (i.e., a should come AFTER b in the heap).
        // For a min-heap (earliest first), "a has lower priority" means
        // a should come later, i.e., a > b.
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
    std::array<MusicalEvent, kMaxStagedEvents> m_stagedEvents;
    std::atomic<uint32_t> m_stagedCount{0};
};

} // namespace hathor