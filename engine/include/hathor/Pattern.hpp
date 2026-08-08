// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_PATTERN_HPP
#define HATHOR_PATTERN_HPP

#include <functional>
#include <span>
#include <cstddef>
#include <cmath>
#include "hathor/Event.hpp"
#include "hathor/Arc.hpp"

namespace hathor {

/**
 * Pattern<T> — a queryable stream of time-stamped events.
 *
 * A Pattern wraps a QueryFn that writes Event<T> values into a caller-supplied
 * span. The caller pre-sizes the buffer to at least maxEventsPerCycle() elements.
 *
 * Zero-allocation invariant for QueryFn:
 * ─────────────────────────────────────
 * The QueryFn stored in a Pattern MUST NOT allocate heap memory during
 * query execution. The caller supplies a pre-allocated span; the function
 * must only write into it. This invariant cannot be enforced at compile time
 * by the type system (std::function itself may allocate during construction
 * for large closures, which is acceptable — that happens on the worker thread
 * before any real-time constraint). At query time (audio callback), NO
 * allocation is permitted.
 *
 * Enforcement strategy:
 *  - Code review: all QueryFn implementations must capture only small,
 *    trivially-copyable values that fit within std::function's SBO buffer.
 *  - Test: test_pattern.cpp overrides operator new and asserts the counter
 *    does not increment during any call to Pattern::query().
 *
 * Requirement references: 1.1, 1.2, 1.3, 1.4, 1.5, 7.1, 7.2, 7.3
 */
template <typename T>
class Pattern {
public:
    /**
     * QueryFn signature:
     *   - arc:       the half-open rational time window to query
     *   - outBuffer: caller-owned span; write up to outBuffer.size() events
     *   - returns:   number of events actually written (≤ outBuffer.size())
     *
     * INVARIANT: implementations MUST NOT allocate on the heap.
     */
    using QueryFn = std::function<std::size_t(Arc, std::span<Event<T>>)>;

    /**
     * Construct a Pattern from a query function and a per-cycle event budget.
     *
     * @param fn               The query implementation. Must satisfy the
     *                         zero-allocation invariant at call time.
     * @param maxEventsPerCycle Upper bound on events produced per integer cycle.
     *                         Callers use this to size their output buffer.
     */
    Pattern(QueryFn fn, std::size_t maxEventsPerCycle)
        : fn_(std::move(fn))
        , maxEventsPerCycle_(maxEventsPerCycle)
    {}

    /**
     * Query events in the given arc.
     *
     * Delegates directly to the stored QueryFn. No heap allocation occurs
     * on this path (see zero-allocation invariant above).
     *
     * @param arc       Half-open time window [start, end) to query.
     * @param outBuffer Caller-supplied buffer; must have capacity ≥
     *                  maxEventsPerCycle() * ceil(arc.duration()).
     * @returns         Number of events written into outBuffer.
     */
    std::size_t query(Arc arc, std::span<Event<T>> outBuffer) const
    {
        return fn_(arc, outBuffer);
    }

    /**
     * Maximum number of events this pattern can produce in a single integer
     * cycle. Callers use this to size their pre-allocated output buffers.
     */
    std::size_t maxEventsPerCycle() const noexcept
    {
        return maxEventsPerCycle_;
    }

private:
    QueryFn     fn_;                ///< the query implementation
    std::size_t maxEventsPerCycle_; ///< per-cycle event budget
};

// ---------------------------------------------------------------------------
// Factory: pure
// ---------------------------------------------------------------------------

/**
 * pure(value, sourceOffset) — a Pattern that repeats a single value once per integer cycle.
 *
 * For each integer cycle c such that [c, c+1) overlaps the query arc:
 *  - whole  = Arc{Rational(c), Rational(c+1)}  (the full cycle arc)
 *  - active = whole.intersect(arc)              (clipped to the query window)
 *
 * The pattern produces exactly one event per overlapping cycle, so
 * maxEventsPerCycle == 1.
 *
 * Zero-allocation guarantee: the closure captures a single T by value.
 * For small T (e.g. std::string short-string-optimised, int, float) this fits
 * in std::function's SBO buffer and no heap allocation occurs at query time.
 *
 * @tparam T            Payload type.
 * @param value         The value emitted by every event.
 * @param sourceOffset  Byte offset of the originating source token (B2 metadata).
 *                      Callers that have no source position pass 0 (the default).
 * @returns             A Pattern<T> with maxEventsPerCycle == 1.
 *
 * Requirement references: 1.1, 1.3, 1.4, 7.1
 */
template <typename T>
Pattern<T> pure(T value, std::size_t sourceOffset = 0)
{
    // Capture value + sourceOffset by value — keep the closure small so
    // std::function can store it in its SBO buffer without heap allocation.
    auto fn = [v = std::move(value), src = sourceOffset]
              (Arc arc, std::span<Event<T>> outBuffer) -> std::size_t
    {
        if (outBuffer.empty()) return 0;

        // Find the first integer cycle that can possibly overlap [arc.start, arc.end).
        // Use floor of arc.start to handle fractional starts (e.g. 1/4 → cycle 0).
        // Note: std::floor on the double approximation is safe here because arc
        // boundaries are rational numbers with small denominators.
        auto cycleStart = static_cast<int64_t>(std::floor(arc.start.toDouble()));

        std::size_t count = 0;
        for (int64_t c = cycleStart; ; ++c) {
            Rational cStart{c};
            Rational cEnd{c + 1};

            // Stop as soon as the cycle starts at or after the query arc's end.
            if (!(cStart < arc.end)) break;

            // Stop writing if the output buffer is full.
            if (count >= outBuffer.size()) break;

            Arc whole{cStart, cEnd};
            Arc active = whole.intersect(arc);

            if (!active.isEmpty()) {
                outBuffer[count++] = Event<T>{whole, active, v, src, -1};
            }
        }

        return count;
    };

    // pure produces at most 1 event per integer cycle.
    return Pattern<T>{std::move(fn), /*maxEventsPerCycle=*/1};
}

} // namespace hathor

#endif // HATHOR_PATTERN_HPP
