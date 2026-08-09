// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * SpscEventRing.hpp — lock-free SPSC ring buffer for MusicalEvent objects.
 *
 * Reuses the SpscRingBuffer/SpscSampleRing design pattern established in B4-K1
 * (app/SpscSampleRing.hpp). Same SPSC discipline: single producer, single
 * consumer, lock-free, allocation-free at runtime (capacity fixed at compile
 * time). The ring stores MusicalEvent structs contiguously.
 *
 * Per Decision #19: use atomic operations with acquire/release for
 * cross-thread synchronization — never use fences or relaxed atomics for
 * state that must be visible to the consumer.
 *
 * Capacity is a compile-time constant. The ring stores exactly CAPACITY
 * events; if full, push returns false (producer handles backpressure).
 * This is allocation-free: no heap allocation after construction.
 *
 * Requirement: B4-K6 (event transport ring), reuses B4-K1 pattern
 */

#include "MusicalEvent.hpp"

#include <atomic>
#include <cstdint>
#include <optional>

namespace hathor {

template<size_t CAPACITY>
class SpscEventRing {
public:
    static_assert(CAPACITY > 0, "SpscEventRing capacity must be positive");
    static_assert(CAPACITY <= UINT32_MAX, "SpscEventRing capacity must fit in uint32_t");
    static constexpr uint32_t kCapacity = static_cast<uint32_t>(CAPACITY);

    SpscEventRing()
        : m_head(0)
        , m_tail(0)
    {}

    // Non-copyable, non-movable (ring contains atomics)
    SpscEventRing(const SpscEventRing&)            = delete;
    SpscEventRing& operator=(const SpscEventRing&) = delete;

    /**
     * Producer side: try to enqueue an event.
     * @return true if enqueued, false if the ring was full.
     *
     * Lock-free and allocation-free.
     */
    bool push(const MusicalEvent& event) {
        const uint32_t head = m_head.load(std::memory_order_relaxed);
        const uint32_t tail = m_tail.load(std::memory_order_acquire);
        const uint32_t nextHead = (head + 1) % kCapacity;
        if (nextHead == tail) {
            // Ring full — producer must handle backpressure
            return false;
        }
        m_buffer[head] = event;
        m_head.store(nextHead, std::memory_order_release);
        return true;
    }

    /**
     * Consumer side: try to dequeue an event.
     * @return the event if available, std::nullopt if empty.
     *
     * Lock-free and allocation-free.
     */
    std::optional<MusicalEvent> pop() {
        const uint32_t tail = m_tail.load(std::memory_order_relaxed);
        const uint32_t head = m_head.load(std::memory_order_acquire);
        if (head == tail) {
            // Ring empty
            return std::nullopt;
        }
        MusicalEvent event = m_buffer[tail];
        m_tail.store((tail + 1) % kCapacity, std::memory_order_release);
        return event;
    }

    /** @return true if the ring is empty (consumer-side check only). */
    bool empty() const {
        return m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_relaxed);
    }

    /** @return true if the ring is full (producer-side check only). */
    bool full() const {
        const uint32_t head = m_head.load(std::memory_order_relaxed);
        const uint32_t tail = m_tail.load(std::memory_order_acquire);
        return ((head + 1) % kCapacity) == tail;
    }

    /** @return compile-time capacity. */
    static constexpr uint32_t capacity() { return kCapacity; }

    /** @return current number of elements (approximate — for diagnostics, not synchronization). */
    uint32_t size() const {
        const uint32_t head = m_head.load(std::memory_order_acquire);
        const uint32_t tail = m_tail.load(std::memory_order_acquire);
        if (head >= tail)
            return head - tail;
        return (kCapacity - tail) + head;
    }

    /**
     * reset() — return ring to empty state (testing / reinit only)
     */
    void reset() noexcept
    {
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
    }

 private:
    alignas(64) MusicalEvent m_buffer[kCapacity]; ///< event storage; padded for cacheline isolation
    alignas(64) std::atomic<uint32_t> m_head{0};   ///< producer write index (only producer writes)
    alignas(64) std::atomic<uint32_t> m_tail{0};   ///< consumer read index (only consumer writes)
};

} // namespace hathor
