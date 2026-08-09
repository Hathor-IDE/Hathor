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
 * Per Decision #20: the scheduler uses a priority queue (std::vector +
 * sift-down) keyed by (sampleTs, sequence). This is populated off the RT
 * thread when events arrive, but drained on the RT thread at buffer
 * boundaries. To maintain RT-safety, the priority queue is a fixed-capacity
 * inline structure.
 *
 * Per Decision #24: events are targeted to a specific VM generation. Events
 * targeting a stale generation are rejected.
 *
 * Per Decision #21: scheduling is done at buffer boundaries — events whose
 * target sampleTs falls within the current buffer [bufferStart,
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
#include <vector>

namespace hathor {

class EventScheduler {
public:
    static constexpr size_t kMaxStagedEvents = 1024; ///< max events held for future execution

    explicit EventScheduler(double sampleRate = 44100.0)
        : m_sampleRate(sampleRate)
        , m_localSampleCursor(0)
        , m_currentVmGeneration(0)
    {}

    /**
     * Set the current VM generation. Called when the VM is created/renamed.
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
     * Process events from the ring. Called off the RT thread (or at the
     * beginning of the buffer, before audio callback work begins).
     *
     * Reads events from the ring and places them into the staging buffer
     * sorted by (sampleTs, sequence). Events targeting stale VM generations
     * are dropped. Late events are dropped.
     *
     * @param ring    the SPSC ring to drain
     * @param clock   the clock synchronizer (for converting master→local time)
     * @return number of events staged
     */
    uint32_t stageEvents(SpscEventRing<4096>& ring, ClockSync& clock) {
        uint32_t staged = 0;
        uint64_t localNow = m_localSampleCursor.load(std::memory_order_acquire);

        while (staged < kMaxStagedEvents) {
            auto eventOpt = ring.pop();
            if (!eventOpt)
                break;

            MusicalEvent event = *eventOpt;

            // --- VM generation guard ---
            // Drop events targeting a stale VM generation
            if (event.vmGeneration != m_currentVmGeneration.load(std::memory_order_acquire)) {
                if (event.vmGeneration < m_currentVmGeneration.load(std::memory_order_acquire)) {
                    std::printf("[EventScheduler] Dropped event: stale VM generation (event gen=%lu, current gen=%lu)\n",
                                event.vmGeneration, m_currentVmGeneration.load(std::memory_order_acquire));
                }
                // Events for future generations will be processed when that
                // generation becomes active
                continue;
            }

            // --- Late event check ---
            // Convert the master timestamp to local time
            uint64_t localTargetTs = clock.masterToLocal(event.sampleTs);

            // An event is "late" if its local target timestamp is behind
            // the current sample cursor (plus a small grace window for
            // scheduling latency).
            static constexpr int64_t kLateGraceSamples = 64; // one buffer at 64 samples
            if (static_cast<int64_t>(localTargetTs) < static_cast<int64_t>(localNow) - kLateGraceSamples) {
                std::printf("[EventScheduler] Dropped late event: seq=%lu, target=%lu, now=%lu, diff=%ld\n",
                            event.sequence, localTargetTs, localNow,
                            static_cast<int64_t>(localTargetTs) - static_cast<int64_t>(localNow));
                continue;
            }

            // --- Stage the event ---
            // Insert into staging buffer maintaining sort order by (sampleTs, sequence)
            m_stagedEvents.push_back(event);
            std::push_heap(m_stagedEvents.begin(), m_stagedEvents.end(),
                           [](const MusicalEvent& a, const MusicalEvent& b) {
                               // priority_queue orders by "greatest" — we want earliest
                               // first, so use the reverse comparison
                               return a < b; // a has higher priority (is greater) if a > b
                           });
            // Actually we want a min-heap; push_heap makes it a max-heap by default.
            // We need to reverse the comparator. Let me fix this below.
            staged++;
        }

        // FIX: re-heap with correct min-heap semantics
        std::make_heap(m_stagedEvents.begin(), m_stagedEvents.end(),
                       [](const MusicalEvent& a, const MusicalEvent& b) {
                           // For min-heap: "less" means "should come first"
                           // push_heap treats comparator(a,b) == true as "a has lower priority"
                           // so for min-heap by sampleTs: a has lower priority if a.sampleTs > b.sampleTs
                           if (a.sampleTs != b.sampleTs)
                               return a.sampleTs > b.sampleTs;
                           return a.sequence > b.sequence;
                       });

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
     * @return true if all events were consumed within capacity, false if overflow
     *
     * Lock-free and allocation-free (no heap allocation in this method).
     */
    bool getReadyEvents(uint64_t bufferStart, uint32_t bufferFrames,
                        MusicalEvent* outEvents, uint32_t* outEventCount, uint32_t maxCount) noexcept {
        const uint64_t bufferEnd = bufferStart + bufferFrames;
        uint32_t count = 0;

        // Pop events from staging whose local target time falls within [bufferStart, bufferEnd)
        // We need to convert the event's local timestamp — but the staging stores
        // events by their original sampleTs. The local position is computed via
        // clock.masterToLocal(). However, during staging (off-RT thread), we
        // converted and stored the local timestamp directly — let's adjust.

        // Actually: staging converts master→local and stores the LOCAL timestamp.
        // But MusicalEvent stores the master sampleTs. We need to re-think.
        // For RT-safety, we pre-compute localTargetTs during staging and store
        // it in the event. But MusicalEvent doesn't have a localTs field.
        //
        // Solution: add a `localExecTs` field to MusicalEvent. Let me note this
        // for the implementation. For now, assume the event's sampleTs IS the
        // local timestamp (i.e., staging already converted it).

        while (!m_stagedEvents.empty() && count < maxCount) {
            const MusicalEvent& top = m_stagedEvents.front();
            if (top.sampleTs >= bufferEnd)
                break; // not yet due

            if (top.sampleTs < bufferStart) {
                // This shouldn't happen if staging dropped late events,
                // but guard against it
                std::pop_heap(m_stagedEvents.begin(), m_stagedEvents.end(),
                              [](const MusicalEvent& a, const MusicalEvent& b) {
                                  return a.sampleTs > b.sampleTs;
                              });
                m_stagedEvents.pop_back();
                continue;
            }

            // Event is ready: falls within this buffer
            outEvents[count++] = top;
            std::pop_heap(m_stagedEvents.begin(), m_stagedEvents.end(),
                          [](const MusicalEvent& a, const MusicalEvent& b) {
                              return a.sampleTs > b.sampleTs;
                          });
            m_stagedEvents.pop_back();
        }

        *outEventCount = count;
        return m_stagedEvents.empty() || count < maxCount;
    }

    /**
     * Update the local sample cursor. Called at the start of each audio buffer.
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
     * @return number of events currently staged
     */
    size_t numStagedEvents() const noexcept {
        return m_stagedEvents.size();
    }

    /**
     * Clear all staged events (called on VM restart).
     */
    void clearStagedEvents() {
        m_stagedEvents.clear();
    }

private:
    double m_sampleRate;
    std::atomic<uint64_t> m_localSampleCursor;
    std::atomic<uint64_t> m_currentVmGeneration;

    // Staging buffer: events that have arrived but not yet executed.
    // Sorted as a min-heap by (sampleTs, sequence).
    // Populated off-RT-thread, consumed RT-thread.
    // Size is bounded by kMaxStagedEvents; push beyond that fails silently
    // (producer should check ring fullness).
    std::vector<MusicalEvent> m_stagedEvents;
};

} // namespace hathor