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

    auto fn = [pats](Arc arc, std::span<Event<T>> out) -> std::size_t
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

    auto fn = [pats, rN](Arc arc, std::span<Event<T>> out) -> std::size_t
    {
        const std::size_t N2 = pats->size();
        std::size_t total = 0;

        // Find the range of integer cycles touched by the query arc.
        int64_t cycleStart = detail::floorRational(arc.start);
        int64_t cycleEnd   = detail::floorRational(arc.end);
        // Include the cycle that contains arc.end if it's not exactly on a boundary.
        if (Rational{cycleEnd} < arc.end)
            ++cycleEnd;

        // Iterate sub-pattern-first (matching Strudel's behaviour): for each
        // sub-pattern, query it across ALL matching cycles before moving to
        // the next sub-pattern. This produces events grouped by sub-pattern
        // rather than grouped by cycle.
        for (std::size_t i = 0; i < N2; ++i) {
            Rational sliceOffset{static_cast<int64_t>(i), static_cast<int64_t>(N2)};
            Rational sliceLen{1, static_cast<int64_t>(N2)};

            for (int64_t c = cycleStart; c < cycleEnd; ++c) {
                if (total >= out.size()) goto done;

                Rational cR{c};

                Rational sliceStart = cR + sliceOffset;
                Rational sliceEnd   = sliceStart + sliceLen;
                Arc sliceArc{sliceStart, sliceEnd};

                // Intersection of query arc with this slot.
                Arc queryInSlice = sliceArc.intersect(arc);
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

    // Scale maxEventsPerCycle: fast(f, p) over one outer cycle queries p over
    // f cycles, so the per-cycle event budget is ceil(f) * inner.maxEventsPerCycle().
    int64_t innerMax = static_cast<int64_t>(p.maxEventsPerCycle());
    int64_t ceilF = (factor.num + factor.den - 1) / factor.den;  // ceil(factor)
    std::size_t maxEvents = static_cast<std::size_t>(ceilF * innerMax);
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
        // Iterate over each integer cycle touched by the query arc.
        // On cycle c where c % n == 0: use the transformed pattern.
        // On other cycles: use the base pattern.
        int64_t cycleStart = detail::floorRational(arc.start);
        int64_t cycleEnd   = detail::floorRational(arc.end);
        if (Rational{cycleEnd} < arc.end)
            ++cycleEnd;

        std::size_t total = 0;

        for (int64_t c = cycleStart; c < cycleEnd; ++c) {
            if (total >= out.size()) break;

            Rational cR{c};
            Rational cNext{c + 1};
            Arc cycleArc{cR, cNext};
            Arc queryInCycle = cycleArc.intersect(arc);
            if (queryInCycle.isEmpty()) continue;

            int64_t mod = c % static_cast<int64_t>(period);
            if (mod < 0) mod += static_cast<int64_t>(period);

            auto sub = out.subspan(total);
            std::size_t got = (mod == 0)
                ? xform->query(queryInCycle, sub)
                : base->query(queryInCycle, sub);
            total += got;
        }
        return total;
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

    // iter(n, p) == slowcat of p.early(i/n) for i in 0..n-1.
    // On cycle c, shift = Rational(c % n, n); query p over (arc + shift),
    // then shift events back by -shift (matching Strudel's early()).
    std::size_t maxEvents = p.maxEventsPerCycle();
    auto inner = std::make_shared<Pattern<T>>(std::move(p));
    int  period = n;

    auto fn = [inner, period](Arc arc, std::span<Event<T>> out) -> std::size_t
    {
        // Iterate over each integer cycle touched by the query arc.
        int64_t cycleStart = detail::floorRational(arc.start);
        int64_t cycleEnd   = detail::floorRational(arc.end);
        if (Rational{cycleEnd} < arc.end)
            ++cycleEnd;

        std::size_t total = 0;

        for (int64_t c = cycleStart; c < cycleEnd; ++c) {
            if (total >= out.size()) break;

            Rational cR{c};
            Rational cNext{c + 1};
            Arc cycleArc{cR, cNext};
            Arc queryInCycle = cycleArc.intersect(arc);
            if (queryInCycle.isEmpty()) continue;

            // shift = (c % n) / n
            int64_t mod = c % static_cast<int64_t>(period);
            if (mod < 0) mod += static_cast<int64_t>(period);
            Rational shift{mod, static_cast<int64_t>(period)};

            // early(shift): query time += shift, event time -= shift
            Arc queryTime{queryInCycle.start + shift, queryInCycle.end + shift};
            auto sub = out.subspan(total);
            std::size_t got = inner->query(queryTime, sub);

            // Shift event arcs back by shift.
            for (std::size_t j = 0; j < got; ++j) {
                auto& ev = out[total + j];
                ev.whole.start  = ev.whole.start  - shift;
                ev.whole.end    = ev.whole.end    - shift;
                ev.active.start = ev.active.start - shift;
                ev.active.end   = ev.active.end   - shift;
            }
            total += got;
        }
        return total;
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

    auto fn = [inner, rhy, steps, off]
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

                // Apply offset rotation (Strudel uses rotate(b, -rotation),
                // which shifts onsets forward — so we subtract the offset).
                int rhythmIdx = ((i - off) % steps + steps) % steps;
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
 * degradeBy(prob, p, seed = 0) — stochastically remove each event of `p` with
 * probability `prob` (i.e. keep each event with probability 1 - prob).
 *
 * Correlated-by-default matches Strudel's reference semantics (see
 * reference/strudel-golden/degrade-by-*.json and
 * docs/pattern-semantics/degradeBy.md). A future decorrelating variant,
 * matching Strudel's `?` mini-notation operator (which applies a small
 * per-instance time offset via `rand.early(0.0003*seed)`), may be added later
 * if needed — do not add it speculatively now.
 *
 * The keep/drop decision replicates Strudel's *legacy* random signal exactly
 * (packages/core/signal.mjs, legacy mode, randSeed default 0 = rand). Strudel
 * samples `rand` at the event's begin time (whole.start) and keeps the event
 * when the sampled value is strictly `> prob`, i.e. filterValues(v => v > x).
 *
 * IMPORTANT — in legacy mode the seed is combined as a TIME OFFSET before
 * the hash (t_eff = t + seed), NOT as a separate hash lane. (Separate hash
 * lanes are the 'precise' RNG mode; degradeBy uses legacy mode so the decision
 * is a deterministic hash of the sample time `whole.start` plus the seed,
 * matching the golden fixtures which were generated with the default seed.)
 *
 * Legacy RNG (signal.mjs: __timeToRands / __intSeedToRand / __xorwise, n = 1):
 *
 *   t_eff = t + seed
 *   s0    = trunc( frac(t_eff / 300) * 2^29 )   // 0 <= s0 < 2^29
 *   x     = xorwise(s0)                        // 32-bit xorshift
 *   rand  = | (x % 2^29) / 2^29 |              // in [0, 1)
 *
 *   where xorwise (32-bit signed arithmetic; JS `>>` is ARITHMETIC and
 *   sign-propagating):
 *     a = (x << 13) ^ x ; b = (a >> 17) ^ a ; return (b << 5) ^ b
 *
 * Keep the event iff rand > prob (strict, matching Strudel). Because rand is
 * always in [0, 1), rand > 1.0 is never true, so degradeBy(1.0) is empty.
 *
 * There is NO special-case at prob == 0.0: Strudel is the golden standard and
 * Hathor matches it 1:1. Strudel's legacy RNG yields rand(0) == 0 at the single
 * measure-zero sample point t == 0, so at prob == 0.0 that event fails
 * `rand > 0` and is dropped — matching reference/strudel-golden/
 * degrade-by-0.0.json exactly. (The fixture's prose says "0% removal / all
 * survive", but its actual event list is authoritative and omits the t == 0
 * event; Hathor matches the event list, not the prose. See
 * docs/potential-improvements-over-strudel.md.)
 *
 * Requirement: 3.9 (deterministic per-(whole.start, seed) decision; a shared
 * default seed makes distinct instances correlate, matching Strudel). NOTE:
 * requirement 20.5's initial assumption that degradeBy(0.0) == identity was
 * superseded by the Strudel ground-truth fixtures — degradeBy(0.0) drops
 * only the t == 0 event (rand(0) == 0), matching Strudel exactly.
 */
template <typename T>
Pattern<T> degradeBy(double prob, Pattern<T> p, double seed = 0.0)
{
    std::size_t maxEvents = p.maxEventsPerCycle();
    auto inner = std::make_shared<Pattern<T>>(std::move(p));

    auto fn = [inner, prob, seed](Arc arc, std::span<Event<T>> out) -> std::size_t
    {
        // Query the inner pattern into the output span (in-place filter).
        std::size_t got = inner->query(arc, out);
        std::size_t total = 0;

        // Strudel legacy RNG constants (signal.mjs: __frac / __intSeedToRand).
        constexpr double kM  = static_cast<double>(0x20000000u);   // 2^29 = 536870912
        constexpr int64_t kMI = static_cast<int64_t>(0x20000000LL); // 2^29

        for (std::size_t i = 0; i < got; ++i) {
            const Event<T>& ev = out[i];

            // Sample Strudel's legacy `rand` signal at the event's whole.start.
            // In legacy mode the seed is combined as a TIME OFFSET (t + seed)
            // BEFORE the xorshift hash, NOT as a separate hash lane.
            //   t_eff = t + seed
            //   s0    = trunc( frac(t_eff / 300) * 2^29 )   // 0 <= s0 < 2^29
            //   x     = xorwise(s0)                        // 32-bit xorshift
            //   rand  = | (x % 2^29) / 2^29 |              // in [0, 1)
            const double t = ev.whole.start.toDouble();
            const double x = (t + seed) / 300.0;
            const double frac = x - std::trunc(x);              // __frac(x), in [0,1) for x >= 0
            const double scaled = frac * kM;                    // __frac(x) * 2^29
            const int64_t s0i = static_cast<int64_t>(std::trunc(scaled)); // trunc -> int
            const uint32_t s0 = static_cast<uint32_t>(s0i);     // Int32 coercion (mod 2^32)

            // __xorwise (32-bit signed arithmetic; JS `>>` is ARITHMETIC /
            // sign-propagating, so emulate it with a sign-bit fill since C++
            // `>>` on unsigned is logical):
            //   a = (x << 13) ^ x ; b = (a >> 17) ^ a ; return (b << 5) ^ b
            const uint32_t a = (s0 << 13) ^ s0;
            const uint32_t aShifted = (a >> 17) | ((a & 0x80000000u) ? 0xFFFF8000u : 0u);
            const uint32_t b = aShifted ^ a;
            const uint32_t res = (b << 5) ^ b;
            const int32_t resS = static_cast<int32_t>(res);

            // __intSeedToRand: (x % 2^29) / 2^29, then Math.abs -> [0, 1).
            // C++ `%` is truncated (sign of dividend), matching JS `%`.
            const int64_t rem = static_cast<int64_t>(resS) % kMI;
            const double randVal = std::fabs(static_cast<double>(rem) / kM);

            // Keep iff rand > prob, strict -- matching Strudel filterValues(v => v > x)
            // exactly, with NO special case at prob == 0.0 (Strudel is the golden
            // standard; match it 1:1). Strudel's legacy RNG yields rand(0) == 0 at
            // the measure-zero sample point t == 0, so at prob == 0.0 that event
            // fails `rand > 0` and is dropped -- matches reference/strudel-golden/
            // degrade-by-0.0.json. At prob == 1.0, randVal is always < 1 so
            // `randVal > 1.0` is never true -> empty (matches golden-1.0).
            const bool keep = (randVal > prob);

            if (keep) {
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
