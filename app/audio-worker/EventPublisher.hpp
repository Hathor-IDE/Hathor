// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * EventPublisher.hpp — Phase-1 pattern scheduler side of B4-K6.
 *
 * Generates timestamped MusicalEvents from the Hathor/Tidal master clock's
 * pattern evaluation and publishes them to the worker via the shared-memory
 * event transport.
 *
 * Per Decision #22: events carry an authoritative sample-frame timestamp
 * generated BEFORE IPC transport. The AudioEngine (master clock owner)
 * generates these timestamps from its sampleClock_ and the exact cycle
 * arithmetic already in audioDeviceIOCallbackWithContext().
 *
 * Per Decision #23: the event publisher writes to the shared-memory event
 * ring (data plane), separate from the control socket. No control-plane
 * round-trip is needed for event delivery.
 *
 * Per Decision #25: the AudioEngine audio callback is NOT the event producer.
 * The event publisher runs on a near-RT thread (the worker thread that
 * hot-swaps SlotState). The audio callback only reads the resulting audio.
 * This keeps the audio thread lock-free and allocation-free.
 *
 * Timestamp generation:
 *   When a pattern event fires (within bufferArc), the AudioEngine already
 *   computes:
 *     - clockNow (absolute sample position, from sampleClock_)
 *     - cycleStart (Rational, in Hathor cycles)
 *     - samplesPerCycle (double)
 *   The event's sampleTs is: clockNow + sampleOffset (where sampleOffset is
 *   the integer sample position within the buffer, already computed).
 *   The event's musicalTs is: cycleStart + Rational(sampleOffset, samplesPerCycle)
 *   — an exact Rational in cycle units.
 *
 * Requirement: B4-K6 EventPublisher
 */

#include "MusicalEvent.hpp"
#include "EventTransport.hpp"
#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"
#include "hathor/ParamMap.hpp"
#include "hathor/Event.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hathor {

class EventPublisher {
public:
    explicit EventPublisher(double sampleRate = 44100.0)
        : m_sampleRate(sampleRate)
        , m_sequenceCounter(0)
        , m_publishEnabled(false)
    {}

    /**
     * Set the shared-memory event transport. Called when the worker starts.
     */
    void setTransport(audio_worker::SharedEventTransport* transport) {
        m_transport = transport;
        m_publishEnabled.store(transport != nullptr, std::memory_order_release);
    }

    /**
     * Set the current worker generation. Events will only be published if
     * the transport's generation matches.
     */
    void setCurrentGeneration(uint64_t gen) {
        m_currentGeneration.store(gen, std::memory_order_release);
    }

    /**
     * Publish a timestamped musical event to the worker.
     *
     * This is called from the worker/hot-swap thread (NOT the audio callback)
     * when a pattern event fires. The event's timestamps are already set by
     * the caller (AudioEngine::audioDeviceIOCallbackWithContext).
     *
     * Uses the seqlock protocol to publish the event atomically into the
     * shared-memory ring. Returns false if the ring is full (backpressure).
     *
     * @param event  the event to publish (timestamps already set)
     * @return true if published, false if dropped (ring full or transport invalid)
     */
    bool publishEvent(const MusicalEvent& event) {
        if (!m_publishEnabled.load(std::memory_order_acquire))
            return false;

        audio_worker::SharedEventTransport* transport = m_transport;
        if (!transport)
            return false;

        // Validate magic
        const uint32_t magic = transport->magic.load(std::memory_order_acquire);
        if (magic != audio_worker::kEventMagic)
            return false;

        // Validate generation — don't publish to a stale transport
        const uint64_t transportGen = transport->generation.load(std::memory_order_acquire);
        const uint64_t currentGen = m_currentGeneration.load(std::memory_order_acquire);
        if (transportGen != currentGen) {
            // Transport doesn't match current generation — worker was restarted
            return false;
        }

        // Check if the ring is full (producer-side check).
        // We use the same drop-oldest policy as SpscSampleRing: if full,
        // we advance the read sequence to make room.
        const uint64_t wSeq = transport->writeSeq.load(std::memory_order_relaxed);
        const uint64_t rSeq = transport->readSeq.load(std::memory_order_acquire);

        if (wSeq - rSeq >= audio_worker::kEventRingCapacity) {
            // Ring is full — drop oldest event(s) to make room
            const uint64_t newRSeq = wSeq - audio_worker::kEventRingCapacity + 1u;
            transport->readSeq.store(newRSeq, std::memory_order_release);
            transport->droppedCount.fetch_add(1, std::memory_order_relaxed);
        }

        // Write the event into the slot using seqlock protocol
        const uint32_t slot = wSeq & audio_worker::kEventRingMask;
        auto& eventSlot = transport->slots[slot];

        // Seqlock: mark in-progress (odd sequence)
        eventSlot.seq.store(wSeq | 1u, std::memory_order_release);

        // Copy event into the slot
        eventSlot.event = event;

        // Seqlock: mark complete (even, incremented by 2)
        eventSlot.seq.store(wSeq + 2u, std::memory_order_release);

        // Advance write sequence (commit)
        transport->writeSeq.store(wSeq + 1u, std::memory_order_release);
        transport->eventCount.fetch_add(1, std::memory_order_relaxed);

        return true;
    }

    /**
     * Create a MusicalEvent from a pattern evaluation result.
     *
     * This is the bridge between the Phase-1 pattern scheduler's Event<ParamMap>
     * (from the engine's Arc/cycle model) and the B4-K6 MusicalEvent.
     *
     * @param hathorEvent   the event from pattern query (has Arc with cycle times)
     * @param cycleStart    the Rational cycle position at buffer start (from AudioEngine)
     * @param clockNow      the absolute sample position at buffer start
     * @param samplesPerCycle  double: samples per Hathor cycle (from BPM/sampleRate)
     * @param slotIdx       the slot index (maps to targetTabId)
     * @param vmGeneration  current VM generation for this tab
     * @return a MusicalEvent ready for publishing
     */
    MusicalEvent createEventFromPattern(const hathor::Event<ParamMap>& hathorEvent,
                                        const Rational& cycleStart,
                                        uint64_t clockNow,
                                        double samplesPerCycle,
                                        int8_t slotIdx,
                                        uint64_t vmGeneration) noexcept {
        // Musical timestamp: exact Rational in Hathor cycles
        // = cycleStart + (event.active.start - cycleStart)
        // But since the event's active.start is already relative to the query
        // window, we just use it directly as the musical timestamp.
        Rational musicalTs = hathorEvent.active.start;

        // Sample timestamp: clockNow + sampleOffset
        // sampleOffset = (event.active.start - cycleStart) * samplesPerCycle
        const double offsetD = (hathorEvent.active.start - cycleStart).toDouble() * samplesPerCycle;
        const int sampleOffset = static_cast<int>(offsetD);
        const uint64_t sampleTs = clockNow + static_cast<uint64_t>(sampleOffset);

        // Monotonic sequence number for deterministic ordering
        const uint64_t seq = m_sequenceCounter.fetch_add(1, std::memory_order_relaxed);

        return MusicalEvent(
            EventType::InstrumentTrigger,
            hathorEvent.value,
            musicalTs,
            sampleTs,
            0, // localExecTs will be set by EventScheduler::stageEvents
            seq,
            static_cast<uint8_t>(slotIdx),
            vmGeneration
        );
    }

    /**
     * @return the next sequence number (for diagnostics/testing)
     */
    uint64_t nextSequence() noexcept {
        return m_sequenceCounter.load(std::memory_order_relaxed);
    }

    /**
     * @return true if event publishing is enabled (transport is set)
     */
    bool isEnabled() const noexcept {
        return m_publishEnabled.load(std::memory_order_acquire);
    }

    /**
     * Disable event publishing (called when worker shuts down).
     */
    void disable() noexcept {
        m_publishEnabled.store(false, std::memory_order_release);
        m_transport = nullptr;
    }

    /**
     * @return current sample rate
     */
    double getSampleRate() const noexcept { return m_sampleRate; }

    /**
     * Set sample rate (called when audio device opens/closes).
     */
    void setSampleRate(double sr) noexcept { m_sampleRate = sr; }

private:
    double m_sampleRate;
    std::atomic<uint64_t> m_sequenceCounter{0};
    std::atomic<bool> m_publishEnabled{false};
    audio_worker::SharedEventTransport* m_transport{nullptr};
    std::atomic<uint64_t> m_currentGeneration{0};
};

} // namespace hathor
