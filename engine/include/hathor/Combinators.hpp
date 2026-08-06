// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_COMBINATORS_HPP
#define HATHOR_COMBINATORS_HPP

/**
 * Combinators.hpp — must-have TidalCycles-style combinators for Pattern<T>.
 *
 * All combinators are pure functions that return a new Pattern<T>.
 * - Construction is O(n) in the number of sub-patterns, but allocation-free at
 *   query time.
 * - Out-of-range arguments throw std::invalid_argument at construction time,
 *   never at query time.
 * - The QueryFn closures capture shared_ptr<> to a heap-allocated capture block
 *   so that pattern objects can be cheaply copied; the capture block itself is
 *   constructed once on the worker thread.
 *
 * Requirement references: 3.1–3.11
 */

#include <vector>
#include <functional>
#include <memory>
#include <stdexcept>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <algorithm>

#include "hathor/Pattern.hpp"
#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"
#include "hathor/Event.hpp"

namespace hathor {

// ---------------------------------------------------------------------------
// Internal helper — integer floor of a Rational
// ---------------------------------------------------------------------------
namespace detail {

inline int64_t floorRational(Rational r) noexcept
{
    // r.den > 0 always (Rational invariant).
    // floor(n/d) = (n - ((n % d + d) % d)) / d   (works for negative n too)
    int64_t q = r.num / r.den;
    int64_t rem = r.num % r.den;
    if (rem < 0) q -= 1;
    return q;
}

} // namespace detail

// ---------------------------------------------------------------------------
// stack
// ---------------------------------------------------------------------------

/**
 * stack(patterns) — merge multiple patterns by returning the union of their
 * events for any queried arc.
 *
 * maxEventsPerCycle = sum of all children's maxEventsPerCycle.
 *
 * Requirement: 3.1
 */
template <typename T>
Pattern<T> stack(std::vector<Pattern<T>> patterns)
{
    if (patterns.empty()) {
        // An empty stack produces no events — valid but trivially empty.
        return Pattern<T>{
            [](Arc, std::span<Event<T>>) -> std::size_t { return 0; },
            0
        };
    }

    std::size_t maxEvents = 0;
    for (const auto& p : patterns)
        maxEvents += p.maxEventsPerCycle();

    // Share ownership of the patterns vector via shared_ptr so the closure
    // can be cheaply copied without re-allocating.
    auto pats = std::make_shared<std::vector<Pattern<T>>>(std::move(patterns));

    auto fn = [pats, maxEvents](Arc arc, std::span<Event<T>> out) -> std::size_t
    {
        std::size_t total = 0;
        for (const auto& p : *pats) {
            if (total >= out.size()) break;
            auto sub = out.subspan(total);
            total += p.query(arc, sub);
        }
        return total;
    };

    return Pattern<T>{std::move(fn), maxEvents};
}

// ---------------------------------------------------------------------------
// fastcat
// ---------------------------------------------------------------------------

/**
 * fastcat(patterns) — sequence N patterns by dividing each cycle evenly.
 *
 * Pattern i occupies the slice [i/N, (i+1)/N) of each cycle.
 * Time is scaled so each sub-pattern sees a full [0,1) window.
 *
 * Throws std::invalid_argument if patterns is empty.
 * Requirement: 3.2
 */
template <typename T>
Pattern<T> fastcat(std::vector<Pattern<T>> patterns)
{
    if (patterns.empty())
        throw std::invalid_argument("fastcat: patterns list must not be empty");

    const std::size_t N = patterns.size();

    // maxEventsPerCycle: each sub-pattern contributes its max (they share cycle time).
    std::size_t maxEvents = 0;
    for (const auto& p : patterns)
        maxEvents += p.maxEventsPerCycle();

    auto pats = std::make_shared<std::vector<Pattern<T>>>(std::move(patterns));
    auto rN = std::make_shared<Rational>(static_cast<int64_t>(N));

    auto fn = [pats, rN, maxEvents](Arc arc, std::span<Event<T>> out) -> std::size_t
    {
        const std::size_t N2 = pats->size();
        std::size_t total = 0;

        // Find the range of integer cycles touched by the query arc.
        int64_t cycleStart = detail::floorRational(arc.start);
        int64_t cycleEnd   = detail::floorRational(arc.end);
        // Include the cycle that contains arc.end if it's not exactly on a boundary.
        if (Rational{cycleEnd} < arc.end)
            ++cycleEnd;

        for (int64_t c = cycleStart; c < cycleEnd; ++c) {
            Rational cR{c};
            Rational cNext{c + 1};

            // Arc of this full cycle, clipped to the query arc.
            Arc cycleArc{cR, cNext};
            Arc queryInCycle = cycleArc.intersect(arc);
            if (queryInCycle.isEmpty()) continue;

            // Iterate over each sub-pattern slot [i/N, (i+1)/N) within this cycle.
            for (std::size_t i = 0; i < N2; ++i) {
                if (total >= out.size()) goto done;

                Rational sliceStart = cR + Rational{static_cast<int64_t>(i),   static_cast<int64_t>(N2)};
                Rational sliceEnd   = cR + Rational{static_cast<int64_t>(i+1), static_cast<int64_t>(N2)};
                Arc sliceArc{sliceStart, sliceEnd};

                // Intersection of query arc with this slot.
                Arc queryInSlice = sliceArc.intersect(queryInCycle);
                if (queryInSlice.isEmpty()) continue;

                // Scale the intersected arc into the sub-pattern's own [0,1) time:
                //   innerArc = (queryInSlice - sliceStart) * N
                Rational innerStart = (queryInSlice.start - sliceStart) * (*rN);
                Rational innerEnd   = (queryInSlice.end   - sliceStart) * (*rN);
                Arc innerArc{innerStart, innerEnd};

                // Query the sub-pattern with a temporary buffer.
                // We write directly into the remaining output span.
                auto sub = out.subspan(total);
                std::size_t got = (*pats)[i].query(innerArc, sub);

                // Un-scale the event arcs back to outer time.
                for (std::size_t j = 0; j < got; ++j) {
                    auto& ev = out[total + j];
                    // whole and active arcs: divide by N, then add sliceStart.
                    ev.whole.start  = ev.whole.start  / (*rN) + sliceStart;
                    ev.whole.end    = ev.whole.end    / (*rN) + sliceStart;
                    ev.active.start = ev.active.start / (*rN) + sliceStart;
                    ev.active.end   = ev.active.end   / (*rN) + sliceStart;
                }
                total += got;
            }
        }
        done:
        return total;
    };

    return Pattern<T>{std::move(fn), maxEvents};
}

// ---------------------------------------------------------------------------
// slowcat
// ---------------------------------------------------------------------------

/**
 * slowcat(patterns) — each pattern occupies one full cycle in rotation.
 *
 * On cycle c, the pattern at index (c mod N) is queried over [c, c+1).
 * Requirement: 3.3
 */
template <typename T>
Pattern<T> slowcat(std::vector<Pattern<T>> patterns)
{
    if (patterns.empty()) {
        return Pattern<T>{
            [](Arc, std::span<Event<T>>) -> std::size_t { return 0; },
            0
        };
    }

    // maxEventsPerCycle = max of all children (only one plays per cycle).
    std::size_t maxEvents = 0;
    for (const auto& p : patterns)
        if (p.maxEventsPerCycle() > maxEvents)
            maxEvents = p.maxEventsPerCycle();

    auto pats = std::make_shared<std::vector<Pattern<T>>>(std::move(patterns));

    auto fn = [pats](Arc arc, std::span<Event<T>> out) -> std::size_t
    {
        const std::size_t N = pats->size();
        std::size_t total = 0;

        int64_t cycleStart = detail::floorRational(arc.start);
        int64_t cycleEnd   = detail::floorRational(arc.end);
        if (Rational{cycleEnd} < arc.end)
            ++cycleEnd;

        for (int64_t c = cycleStart; c < cycleEnd; ++c) {
            if (total >= out.size()) break;

            Rational cR{c};
            Rational cNext{c + 1};
            Arc cycleArc{cR, cNext};

            Arc queryInCycle = cycleArc.intersect(arc);
            if (queryInCycle.isEmpty()) continue;

            // Which sub-pattern plays on cycle c?
            // Handle negative cycle numbers with a proper modulo.
            std::size_t idx;
            if (c >= 0) {
                idx = static_cast<std::size_t>(c) % N;
            } else {
                // e.g. c = -1, N = 3 → (-1 mod 3 + 3) mod 3 = 2
                int64_t m = (c % static_cast<int64_t>(N) + static_cast<int64_t>(N))
                            % static_cast<int64_t>(N);
                idx = static_cast<std::size_t>(m);
            }

            auto sub = out.subspan(total);
            total += (*pats)[idx].query(queryInCycle, sub);
        }
        return total;
    };

    return Pattern<T>{std::move(fn), maxEvents};
}

// ---------------------------------------------------------------------------
// fast / slow
// ---------------------------------------------------------------------------

/**
 * fast(factor, p) — compress pattern time by factor.
 *
 * Equivalent to querying p at (arc * factor) and dividing result arcs by factor.
 * Throws std::invalid_argument if factor <= 0.
 * Requirement: 3.4
 */
template <typename T>
Pattern<T> fast(Rational factor, Pattern<T> p)
{
    if (factor.num <= 0)
        throw std::invalid_argument("fast: factor must be positive");

    std::size_t maxEvents = p.maxEventsPerCycle();
    auto inner = std::make_shared<Pattern<T>>(std::move(p));
    auto fac   = std::make_shared<Rational>(factor);

    auto fn = [inner, fac](Arc arc, std::span<Event<T>> out) -> std::size_t
    {
        // Scale arc into inner time.
        Arc innerArc{arc.start * (*fac), arc.end * (*fac)};
        std::size_t got = inner->query(innerArc, out);

        // Un-scale event arcs back to outer time.
        for (std::size_t i = 0; i < got; ++i) {
            auto& ev = out[i];
            ev.whole.start  = ev.whole.start  / (*fac);
            ev.whole.end    = ev.whole.end    / (*fac);
            ev.active.start = ev.active.start / (*fac);
            ev.active.end   = ev.active.end   / (*fac);
        }
        return got;
    };

    return Pattern<T>{std::move(fn), maxEvents};
}

/**
 * slow(factor, p) — dilate pattern time by factor (inverse of fast).
 *
 * Throws std::invalid_argument if factor <= 0.
 * Requirement: 3.5
 */
template <typename T>
Pattern<T> slow(Rational factor, Pattern<T> p)
{
    if (factor.num <= 0)
        throw std::invalid_argument("slow: factor must be positive");

    // slow(f, p) == fast(1/f, p)
    Rational inv{factor.den, factor.num};  // 1/factor; den>0 guaranteed by Rational invariant
    return fast(inv, std::move(p));
}

// ---------------------------------------------------------------------------
// rev
// ---------------------------------------------------------------------------

/**
 * rev(p) — reverse events within each cycle.
 *
 * For an event in cycle c with arc [s, e):
 *   reflected start = (c+1) - e
 *   reflected end   = (c+1) - s
 *
 * Requirement: 3.7
 */
template <typename T>
Pattern<T> rev(Pattern<T> p)
{
    std::size_t maxEvents = p.maxEventsPerCycle();
    auto inner = std::make_shared<Pattern<T>>(std::move(p));

    auto fn = [inner](Arc arc, std::span<Event<T>> out) -> std::size_t
    {
        // To get the reversed version, we need to query the inner pattern over
        // the "mirror" of the query arc within each cycle, then reflect the
        // returned events.

        // For an arc [s,e) within cycle c: reflected = [(c+1)-e, (c+1)-s).
        // We iterate over each integer cycle covered by the query arc.
        int64_t cycleStart = detail::floorRational(arc.start);
        int64_t cycleEnd   = detail::floorRational(arc.end);
        if (Rational{cycleEnd} < arc.end)
            ++cycleEnd;

        std::size_t total = 0;

        for (int64_t c = cycleStart; c < cycleEnd; ++c) {
            if (total >= out.size()) break;

            Rational cR{c};
            Rational cEnd{c + 1};

            // Mirror the query arc within this cycle.
            Arc cycleArc{cR, cEnd};
            Arc queryInCycle = cycleArc.intersect(arc);
            if (queryInCycle.isEmpty()) continue;

            // Reflect: [s,e) in cycle c → [(c+1)-e, (c+1)-s)
            Rational mirStart = cEnd - queryInCycle.end;
            Rational mirEnd   = cEnd - queryInCycle.start;
            Arc mirArc{mirStart, mirEnd};

            auto sub = out.subspan(total);
            std::size_t got = inner->query(mirArc, sub);

            // Reflect the returned event arcs.
            for (std::size_t i = 0; i < got; ++i) {
                auto& ev = out[total + i];
                // Reflect whole arc.
                Rational ws = cEnd - ev.whole.end;
                Rational we = cEnd - ev.whole.start;
                ev.whole = Arc{ws, we};
                // Reflect active arc.
                Rational as = cEnd - ev.active.end;
                Rational ae = cEnd - ev.active.start;
                ev.active = Arc{as, ae};
            }
            total += got;
        }
        return total;
    };

    return Pattern<T>{std::move(fn), maxEvents};
}

// ---------------------------------------------------------------------------
// every
// ---------------------------------------------------------------------------

/**
 * every(n, f, p) — apply function f to p on cycles that are multiples of n.
 *
 * On cycle c where c % n == 0: return f(p).query(arc).
 * On other cycles:             return p.query(arc).
 *
 * Throws std::invalid_argument if n <= 0.
 * Requirement: 3.6
 */
template <typename T>
Pattern<T> every(int n, std::function<Pattern<T>(Pattern<T>)> f, Pattern<T> p)
{
    if (n <= 0)
        throw std::invalid_argument("every: n must be positive");

    // Pre-compute both the base and transformed patterns.
    Pattern<T> transformed = f(p);  // f applied to p

    std::size_t maxEvents = std::max(p.maxEventsPerCycle(), transformed.maxEventsPerCycle());

    auto base = std::make_shared<Pattern<T>>(std::move(p));
    auto xform = std::make_shared<Pattern<T>>(std::move(transformed));
    int  period = n;

    auto fn = [base, xform, period](Arc arc, std::span<Event<T>> out) -> std::size_t
    {
        int64_t cycle = detail::floorRational(arc.start);
        // Determine which pattern to use based on the cycle.
        // Use proper modulo for potentially negative cycles.
        int64_t mod = cycle % static_cast<int64_t>(period);
        if (mod < 0) mod += static_cast<int64_t>(period);

        if (mod == 0) {
            return xform->query(arc, out);
        } else {
            return base->query(arc, out);
        }
    };

    return Pattern<T>{std::move(fn), maxEvents};
}

// ---------------------------------------------------------------------------
// iter
// ---------------------------------------------------------------------------

/**
 * iter(n, p) — rotate pattern forward by 1/n of a cycle each cycle.
 *
 * On cycle c, shift = Rational(c % n, n).
 * The arc is shifted by -shift before querying p, then shifted back.
 *
 * Throws std::invalid_argument if n <= 0.
 * Requirement: 3.10
 */
template <typename T>
Pattern<T> iter(int n, Pattern<T> p)
{
    if (n <= 0)
        throw std::invalid_argument("iter: n must be positive");

    std::size_t maxEvents = p.maxEventsPerCycle();
    auto inner = std::make_shared<Pattern<T>>(std::move(p));
    int  period = n;

    auto fn = [inner, period](Arc arc, std::span<Event<T>> out) -> std::size_t
    {
        int64_t cycle = detail::floorRational(arc.start);

        // shift = (cycle % n) / n, with proper wrap for negative cycles.
        int64_t mod = cycle % static_cast<int64_t>(period);
        if (mod < 0) mod += static_cast<int64_t>(period);
        Rational shift{mod, static_cast<int64_t>(period)};

        // Shift the query arc backwards by shift, query p, shift results forwards.
        Arc shiftedArc{arc.start - shift, arc.end - shift};
        std::size_t got = inner->query(shiftedArc, out);

        // Shift event arcs back by adding shift.
        for (std::size_t i = 0; i < got; ++i) {
            auto& ev = out[i];
            ev.whole.start  = ev.whole.start  + shift;
            ev.whole.end    = ev.whole.end    + shift;
            ev.active.start = ev.active.start + shift;
            ev.active.end   = ev.active.end   + shift;
        }
        return got;
    };

    return Pattern<T>{std::move(fn), maxEvents};
}

// ---------------------------------------------------------------------------
// euclid
// ---------------------------------------------------------------------------

/**
 * euclid(k, n, offset, p) — Euclidean (Bjorklund) rhythm.
 *
 * Distributes k onsets across n steps as evenly as possible (Bjorklund algorithm).
 * The rhythm array is computed at construction time.
 * An onset at step i produces an event at [c + i/n, c + (i+1)/n) per cycle c.
 * The offset rotates the rhythm by (offset mod n) steps.
 *
 * Throws std::invalid_argument if k < 0, k > n, or n <= 0.
 * Requirement: 3.8
 */
template <typename T>
Pattern<T> euclid(int k, int n, int offset, Pattern<T> p)
{
    if (n <= 0)
        throw std::invalid_argument("euclid: n must be positive");
    if (k < 0)
        throw std::invalid_argument("euclid: k must be >= 0");
    if (k > n)
        throw std::invalid_argument("euclid: k must be <= n");

    // Bjorklund algorithm: compute bool[n] rhythm.
    // We use the standard "remainder distribution" approach.
    std::vector<bool> rhythm(static_cast<std::size_t>(n), false);
    {
        // Build sequences using Euclidean algorithm on two groups.
        std::vector<std::vector<bool>> ones(static_cast<std::size_t>(k), std::vector<bool>{true});
        std::vector<std::vector<bool>> zeros(static_cast<std::size_t>(n - k), std::vector<bool>{false});

        while (zeros.size() > 1 && ones.size() > 0) {
            std::size_t minLen = std::min(ones.size(), zeros.size());
            std::vector<std::vector<bool>> next;
            next.reserve(minLen);
            for (std::size_t i = 0; i < minLen; ++i) {
                std::vector<bool> merged = ones[i];
                merged.insert(merged.end(), zeros[i].begin(), zeros[i].end());
                next.push_back(std::move(merged));
            }
            // The remainder goes to whichever group is larger.
            if (ones.size() > zeros.size()) {
                // ones had more: remainder is ones[minLen..end]
                std::vector<std::vector<bool>> rem(ones.begin() + static_cast<ptrdiff_t>(minLen), ones.end());
                ones = std::move(next);
                zeros = std::move(rem);
            } else {
                // zeros had more or equal: remainder is zeros[minLen..end]
                std::vector<std::vector<bool>> rem(zeros.begin() + static_cast<ptrdiff_t>(minLen), zeros.end());
                ones = std::move(next);
                zeros = std::move(rem);
            }
        }
        // Flatten into rhythm array.
        std::size_t pos = 0;
        for (auto& seq : ones)
            for (bool b : seq) { if (pos < static_cast<std::size_t>(n)) rhythm[pos++] = b; }
        for (auto& seq : zeros)
            for (bool b : seq) { if (pos < static_cast<std::size_t>(n)) rhythm[pos++] = b; }
    }

    // Normalise offset.
    int normOffset = (n > 0) ? ((offset % n + n) % n) : 0;

    std::size_t maxEvents = static_cast<std::size_t>(k);  // at most k onsets per cycle

    auto inner = std::make_shared<Pattern<T>>(std::move(p));
    auto rhy = std::make_shared<std::vector<bool>>(std::move(rhythm));
    int  steps = n;
    int  off   = normOffset;

    auto fn = [inner, rhy, steps, off, maxEvents]
              (Arc arc, std::span<Event<T>> out) -> std::size_t
    {
        int64_t cycleStart = detail::floorRational(arc.start);
        int64_t cycleEnd   = detail::floorRational(arc.end);
        if (Rational{cycleEnd} < arc.end)
            ++cycleEnd;

        std::size_t total = 0;

        for (int64_t c = cycleStart; c < cycleEnd; ++c) {
            Rational cR{c};

            for (int i = 0; i < steps; ++i) {
                if (total >= out.size()) goto euclid_done;

                // Apply offset rotation.
                int rhythmIdx = (i + off) % steps;
                if (!(*rhy)[static_cast<std::size_t>(rhythmIdx)]) continue;

                // Step i occupies [c + i/n, c + (i+1)/n).
                Rational stepStart = cR + Rational{static_cast<int64_t>(i),     static_cast<int64_t>(steps)};
                Rational stepEnd   = cR + Rational{static_cast<int64_t>(i + 1), static_cast<int64_t>(steps)};
                Arc stepArc{stepStart, stepEnd};
                Arc active = stepArc.intersect(arc);
                if (active.isEmpty()) continue;

                // Query the inner pattern for this step's arc to get the value.
                // We use a local 1-element temp span trick: write into out[total].
                auto sub = out.subspan(total, 1);
                std::size_t got = inner->query(stepArc, sub);
                if (got == 0) continue;

                // Fix arcs: whole is the step arc, active is the intersection.
                out[total].whole  = stepArc;
                out[total].active = active;
                ++total;
            }
        }
        euclid_done:
        return total;
    };

    return Pattern<T>{std::move(fn), maxEvents};
}

// ---------------------------------------------------------------------------
// degradeBy
// ---------------------------------------------------------------------------

/**
 * degradeBy(prob, p) — stochastically remove events with probability `prob`.
 *
 * Uses a deterministic hash of (whole.start.num, whole.start.den, salt) so
 * that identical queries to the same instance produce identical results, but
 * two separate degradeBy instances are uncorrelated (different salts).
 *
 * Keep event if: hash(num, den, salt) > UINT64_MAX * prob
 *
 * The salt is assigned from a static atomic counter at construction time,
 * giving each degradeBy node a unique identity.
 *
 * Requirement: 3.9
 */
template <typename T>
Pattern<T> degradeBy(double prob, Pattern<T> p)
{
    // Assign a unique salt for this combinator instance.
    static std::atomic<uint64_t> saltCounter{1};
    uint64_t salt = saltCounter.fetch_add(1, std::memory_order_relaxed);

    std::size_t maxEvents = p.maxEventsPerCycle();
    auto inner = std::make_shared<Pattern<T>>(std::move(p));
    double keepProb = 1.0 - prob;  // probability to KEEP an event

    auto fn = [inner, salt, keepProb](Arc arc, std::span<Event<T>> out) -> std::size_t
    {
        // Query the inner pattern into a local scratch region of the output span.
        // We do an in-place filter.
        std::size_t got = inner->query(arc, out);
        std::size_t total = 0;

        for (std::size_t i = 0; i < got; ++i) {
            const auto& ev = out[i];

            // Hash: xxhash64-style mixing of (num, den, salt).
            uint64_t h = salt;
            // Mix in whole.start.num
            h ^= static_cast<uint64_t>(ev.whole.start.num) * UINT64_C(11400714785074694791);
            h = (h << 31) | (h >> 33);
            h *= UINT64_C(14029467366897019727);
            // Mix in whole.start.den
            h ^= static_cast<uint64_t>(ev.whole.start.den) * UINT64_C(11400714785074694791);
            h = (h << 27) | (h >> 37);
            h *= UINT64_C(14029467366897019727);
            // Final avalanche
            h ^= h >> 33;
            h *= UINT64_C(0xff51afd7ed558ccd);
            h ^= h >> 33;
            h *= UINT64_C(0xc4ceb9fe1a85ec53);
            h ^= h >> 33;

            // Keep if h / UINT64_MAX <= keepProb
            // Equivalently: keep if h <= UINT64_MAX * keepProb
            // Use double comparison to avoid integer overflow.
            constexpr double kMaxU64 = static_cast<double>(UINT64_MAX);
            double normalised = static_cast<double>(h) / kMaxU64;
            if (normalised <= keepProb) {
                if (total != i)
                    out[total] = std::move(out[i]);
                ++total;
            }
        }
        return total;
    };

    return Pattern<T>{std::move(fn), maxEvents};
}

} // namespace hathor

#endif // HATHOR_COMBINATORS_HPP
