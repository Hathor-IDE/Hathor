// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "hathor/Combinators.hpp"
#include "hathor/Pattern.hpp"
#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"
#include "hathor/Event.hpp"

#include <vector>
#include <string>
#include <span>

using namespace hathor;

// ---------------------------------------------------------------------------
// Helper: make a buffer of N events.
// Event<T> does not have a default constructor (Rational has no default ctor),
// so we build the vector using an explicit dummy event.
// ---------------------------------------------------------------------------

template <typename T>
static std::vector<Event<T>> makeBuffer(std::size_t n)
{
    Arc dummy{Rational{0}, Rational{0}};
    Event<T> e{dummy, dummy, T{}};
    return std::vector<Event<T>>(n, e);
}

// ---------------------------------------------------------------------------
// Test: stack
// ---------------------------------------------------------------------------

TEST_CASE("stack: merge two patterns", "[combinators][stack]")
{
    auto p1 = pure<std::string>("a");
    auto p2 = pure<std::string>("b");
    auto stacked = stack<std::string>({p1, p2});

    auto buffer = makeBuffer<std::string>(10);
    Arc arc{Rational{0}, Rational{1}};
    std::size_t count = stacked.query(arc, buffer);

    REQUIRE(count == 2);
    REQUIRE(buffer[0].value == "a");
    REQUIRE(buffer[1].value == "b");
}

TEST_CASE("stack: empty patterns list produces no events", "[combinators][stack]")
{
    auto stacked = stack<std::string>({});
    auto buffer = makeBuffer<std::string>(10);
    Arc arc{Rational{0}, Rational{1}};
    std::size_t count = stacked.query(arc, buffer);
    REQUIRE(count == 0);
}

TEST_CASE("stack: maxEventsPerCycle is sum of children", "[combinators][stack]")
{
    auto p1 = pure<int>(1);
    auto p2 = pure<int>(2);
    auto p3 = pure<int>(3);
    auto stacked = stack<int>({p1, p2, p3});
    REQUIRE(stacked.maxEventsPerCycle() == 3);
}

// ---------------------------------------------------------------------------
// Test: fastcat
// ---------------------------------------------------------------------------

TEST_CASE("fastcat: sequence two patterns in one cycle", "[combinators][fastcat]")
{
    auto p1 = pure<std::string>("a");
    auto p2 = pure<std::string>("b");
    auto cat = fastcat<std::string>({p1, p2});

    auto buffer = makeBuffer<std::string>(10);
    Arc arc{Rational{0}, Rational{1}};
    std::size_t count = cat.query(arc, buffer);

    REQUIRE(count == 2);
    REQUIRE(buffer[0].value == "a");
    REQUIRE(buffer[0].whole.start == Rational{0});
    REQUIRE(buffer[0].whole.end == Rational{1, 2});

    REQUIRE(buffer[1].value == "b");
    REQUIRE(buffer[1].whole.start == Rational{1, 2});
    REQUIRE(buffer[1].whole.end == Rational{1});
}

TEST_CASE("fastcat: empty list throws", "[combinators][fastcat]")
{
    REQUIRE_THROWS_AS(fastcat<std::string>({}), std::invalid_argument);
}

TEST_CASE("fastcat: event count over [0,1) equals sum of sub-patterns", "[combinators][fastcat]")
{
    auto p1 = pure<int>(1);
    auto p2 = pure<int>(2);
    auto p3 = pure<int>(3);
    auto cat = fastcat<int>({p1, p2, p3});

    auto buffer = makeBuffer<int>(10);
    Arc arc{Rational{0}, Rational{1}};
    std::size_t count = cat.query(arc, buffer);
    REQUIRE(count == 3);
}

// ---------------------------------------------------------------------------
// Test: slowcat
// ---------------------------------------------------------------------------

TEST_CASE("slowcat: each pattern occupies one full cycle", "[combinators][slowcat]")
{
    auto p1 = pure<std::string>("a");
    auto p2 = pure<std::string>("b");
    auto cat = slowcat<std::string>({p1, p2});

    auto buffer = makeBuffer<std::string>(10);

    // Cycle 0 → "a"
    std::size_t count0 = cat.query(Arc{Rational{0}, Rational{1}}, buffer);
    REQUIRE(count0 == 1);
    REQUIRE(buffer[0].value == "a");

    // Cycle 1 → "b"
    std::size_t count1 = cat.query(Arc{Rational{1}, Rational{2}}, buffer);
    REQUIRE(count1 == 1);
    REQUIRE(buffer[0].value == "b");

    // Cycle 2 → "a" again (rotation)
    std::size_t count2 = cat.query(Arc{Rational{2}, Rational{3}}, buffer);
    REQUIRE(count2 == 1);
    REQUIRE(buffer[0].value == "a");
}

TEST_CASE("slowcat: empty list produces no events", "[combinators][slowcat]")
{
    auto cat = slowcat<std::string>({});
    auto buffer = makeBuffer<std::string>(10);
    std::size_t count = cat.query(Arc{Rational{0}, Rational{1}}, buffer);
    REQUIRE(count == 0);
}

// ---------------------------------------------------------------------------
// Test: fast / slow
// ---------------------------------------------------------------------------

TEST_CASE("fast: compress time by factor 2", "[combinators][fast]")
{
    auto p = pure<std::string>("x");
    auto f = fast(Rational{2}, p);

    auto buffer = makeBuffer<std::string>(10);
    Arc arc{Rational{0}, Rational{1}};
    std::size_t count = f.query(arc, buffer);

    // fast(2, pure) over [0,1) → 2 events: [0,1/2) and [1/2,1)
    REQUIRE(count == 2);
    REQUIRE(buffer[0].whole.start == Rational{0});
    REQUIRE(buffer[0].whole.end == Rational{1, 2});
    REQUIRE(buffer[1].whole.start == Rational{1, 2});
    REQUIRE(buffer[1].whole.end == Rational{1});
}

TEST_CASE("fast: three inputs scale arc positions correctly", "[combinators][fast]")
{
    auto p = pure<std::string>("x");
    auto f = fast(Rational{3}, p);

    auto buffer = makeBuffer<std::string>(10);
    Arc arc{Rational{0}, Rational{1}};
    std::size_t count = f.query(arc, buffer);

    REQUIRE(count == 3);
    REQUIRE(buffer[0].whole.start == Rational{0});
    REQUIRE(buffer[0].whole.end == Rational{1, 3});
    REQUIRE(buffer[1].whole.start == Rational{1, 3});
    REQUIRE(buffer[1].whole.end == Rational{2, 3});
    REQUIRE(buffer[2].whole.start == Rational{2, 3});
    REQUIRE(buffer[2].whole.end == Rational{1});
}

TEST_CASE("fast: throws if factor <= 0", "[combinators][fast]")
{
    auto p = pure<int>(1);
    REQUIRE_THROWS_AS(fast(Rational{0}, p), std::invalid_argument);
    REQUIRE_THROWS_AS(fast(Rational{-1}, p), std::invalid_argument);
}

TEST_CASE("slow: dilate time by factor 2", "[combinators][slow]")
{
    auto p = pure<std::string>("x");
    auto s = slow(Rational{2}, p);

    auto buffer = makeBuffer<std::string>(10);
    Arc arc{Rational{0}, Rational{2}};
    std::size_t count = s.query(arc, buffer);

    // slow(2, pure) over [0,2) → 1 event occupying [0,2)
    REQUIRE(count == 1);
    REQUIRE(buffer[0].whole.start == Rational{0});
    REQUIRE(buffer[0].whole.end == Rational{2});
}

TEST_CASE("slow: throws if factor <= 0", "[combinators][slow]")
{
    auto p = pure<int>(1);
    REQUIRE_THROWS_AS(slow(Rational{0}, p), std::invalid_argument);
    REQUIRE_THROWS_AS(slow(Rational{-1}, p), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Test: rev
// ---------------------------------------------------------------------------

TEST_CASE("rev: reverses event positions within cycle", "[combinators][rev]")
{
    // fastcat("a","b") → a at [0,1/2), b at [1/2,1)
    // rev reflects each event's arc: [s,e) in cycle c → [(c+1)-e, (c+1)-s)
    // So: a [0,1/2) → [1/2, 1), b [1/2,1) → [0, 1/2)
    // Events come out in the same production order as the inner pattern,
    // but with reflected arcs.
    auto cat = fastcat<std::string>({pure<std::string>("a"), pure<std::string>("b")});
    auto reversed = rev(cat);

    auto buffer = makeBuffer<std::string>(10);
    Arc arc{Rational{0}, Rational{1}};
    std::size_t count = reversed.query(arc, buffer);

    REQUIRE(count == 2);
    // Inner pattern produces "a" first, then "b" — reflected arcs:
    REQUIRE(buffer[0].value == "a");
    REQUIRE(buffer[0].whole.start == Rational{1, 2});
    REQUIRE(buffer[0].whole.end == Rational{1});

    REQUIRE(buffer[1].value == "b");
    REQUIRE(buffer[1].whole.start == Rational{0});
    REQUIRE(buffer[1].whole.end == Rational{1, 2});
}

// ---------------------------------------------------------------------------
// Test: every
// ---------------------------------------------------------------------------

TEST_CASE("every: applies f on cycle 0, N, 2N; not on others", "[combinators][every]")
{
    auto base = pure<std::string>("base");
    auto xform = [](Pattern<std::string>) -> Pattern<std::string> {
        return pure<std::string>("transformed");
    };
    auto pat = every<std::string>(3, xform, base);

    auto buffer = makeBuffer<std::string>(10);

    // Cycle 0: transformed (0 % 3 == 0)
    REQUIRE(pat.query(Arc{Rational{0}, Rational{1}}, buffer) == 1);
    REQUIRE(buffer[0].value == "transformed");

    // Cycle 1: base
    REQUIRE(pat.query(Arc{Rational{1}, Rational{2}}, buffer) == 1);
    REQUIRE(buffer[0].value == "base");

    // Cycle 2: base
    REQUIRE(pat.query(Arc{Rational{2}, Rational{3}}, buffer) == 1);
    REQUIRE(buffer[0].value == "base");

    // Cycle 3: transformed (3 % 3 == 0)
    REQUIRE(pat.query(Arc{Rational{3}, Rational{4}}, buffer) == 1);
    REQUIRE(buffer[0].value == "transformed");

    // Cycle 6: transformed (6 % 3 == 0)
    REQUIRE(pat.query(Arc{Rational{6}, Rational{7}}, buffer) == 1);
    REQUIRE(buffer[0].value == "transformed");
}

TEST_CASE("every: throws if n <= 0", "[combinators][every]")
{
    auto p = pure<int>(1);
    auto identity = [](Pattern<int> x) { return x; };
    REQUIRE_THROWS_AS(every<int>(0, identity, p), std::invalid_argument);
    REQUIRE_THROWS_AS(every<int>(-1, identity, p), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Test: iter
// ---------------------------------------------------------------------------

TEST_CASE("iter: arc rotates by 1/N each cycle", "[combinators][iter]")
{
    // fastcat("a","b") → a at [0,1/2), b at [1/2,1) per cycle
    auto cat = fastcat<std::string>({pure<std::string>("a"), pure<std::string>("b")});
    auto iterated = iter(2, cat);

    auto buffer = makeBuffer<std::string>(10);

    // Cycle 0: shift = 0/2 = 0 → a at [0,1/2), b at [1/2,1)
    std::size_t c0 = iterated.query(Arc{Rational{0}, Rational{1}}, buffer);
    REQUIRE(c0 == 2);
    REQUIRE(buffer[0].value == "a");
    REQUIRE(buffer[0].whole.start == Rational{0});
    REQUIRE(buffer[1].value == "b");
    REQUIRE(buffer[1].whole.start == Rational{1, 2});

    // Cycle 1: shift = 1/2 → query shifted arc [1/2, 3/2) into fastcat
    //   inner [1/2, 1) → b, inner [1, 3/2) → a of cycle 2
    // After shifting results back by +1/2:
    //   b moves from inner [1/2,1) → outer [1, 3/2) but wait, we add shift to results:
    //   b: inner whole [1/2,1) + 1/2 = [1, 3/2) which is in cycle 1's context [1,2)
    // Actually querying [1,2) with iter(2) should give us shifted events in that outer arc.
    std::size_t c1 = iterated.query(Arc{Rational{1}, Rational{2}}, buffer);
    REQUIRE(c1 == 2);
    // b at [1, 3/2)
    REQUIRE(buffer[0].value == "b");
    REQUIRE(buffer[0].whole.start == Rational{1});
    // a at [3/2, 2)
    REQUIRE(buffer[1].value == "a");
    REQUIRE(buffer[1].whole.start == Rational{3, 2});
}

TEST_CASE("iter: throws if n <= 0", "[combinators][iter]")
{
    auto p = pure<int>(1);
    REQUIRE_THROWS_AS(iter(0, p), std::invalid_argument);
    REQUIRE_THROWS_AS(iter(-1, p), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Test: euclid
// ---------------------------------------------------------------------------

TEST_CASE("euclid: (3,8,0) onset positions", "[combinators][euclid]")
{
    auto p = pure<std::string>("x");
    auto rhythm = euclid(3, 8, 0, p);

    auto buffer = makeBuffer<std::string>(10);
    std::size_t count = rhythm.query(Arc{Rational{0}, Rational{1}}, buffer);

    REQUIRE(count == 3);
    // Bjorklund(3,8) → onsets at steps 0, 3, 6 (evenly spaced)
    REQUIRE(buffer[0].whole.start == Rational{0, 8});
    REQUIRE(buffer[1].whole.start == Rational{3, 8});
    REQUIRE(buffer[2].whole.start == Rational{6, 8});
}

TEST_CASE("euclid: (5,8,0) produces 5 onsets", "[combinators][euclid]")
{
    auto p = pure<std::string>("x");
    auto rhythm = euclid(5, 8, 0, p);

    auto buffer = makeBuffer<std::string>(10);
    std::size_t count = rhythm.query(Arc{Rational{0}, Rational{1}}, buffer);
    REQUIRE(count == 5);
}

TEST_CASE("euclid: (7,16,2) produces 7 onsets", "[combinators][euclid]")
{
    auto p = pure<std::string>("x");
    auto rhythm = euclid(7, 16, 2, p);

    auto buffer = makeBuffer<std::string>(20);
    std::size_t count = rhythm.query(Arc{Rational{0}, Rational{1}}, buffer);
    REQUIRE(count == 7);
}

TEST_CASE("euclid: offset rotates rhythm", "[combinators][euclid]")
{
    auto p = pure<std::string>("x");
    auto rhythm = euclid(3, 8, 2, p);

    auto buffer = makeBuffer<std::string>(10);
    std::size_t count = rhythm.query(Arc{Rational{0}, Rational{1}}, buffer);

    REQUIRE(count == 3);
    // Bjorklund(3,8) onsets at steps {0,3,6}.
    // offset=2: step i fires if rhythm[(i+2)%8] is set.
    // rhythm[2]=false, rhythm[3]=true → step 1 fires (1/8)
    // rhythm[5]=false, rhythm[6]=true → step 4 fires (4/8 = 1/2)
    // rhythm[8%8=0]=true → step 6 fires (6/8 = 3/4)
    REQUIRE(buffer[0].whole.start == Rational{1, 8});
    REQUIRE(buffer[1].whole.start == Rational{1, 2});
    REQUIRE(buffer[2].whole.start == Rational{3, 4});
}

TEST_CASE("euclid: throws on invalid arguments", "[combinators][euclid]")
{
    auto p = pure<int>(1);
    REQUIRE_THROWS_AS(euclid(-1,  8, 0, p), std::invalid_argument);  // k < 0
    REQUIRE_THROWS_AS(euclid( 9,  8, 0, p), std::invalid_argument);  // k > n
    REQUIRE_THROWS_AS(euclid( 1,  0, 0, p), std::invalid_argument);  // n == 0
    REQUIRE_THROWS_AS(euclid( 1, -1, 0, p), std::invalid_argument);  // n < 0
}

// ---------------------------------------------------------------------------
// Test: degradeBy
// ---------------------------------------------------------------------------

TEST_CASE("degradeBy 0.0 = original events (all kept)", "[combinators][degradeBy]")
{
    // Query over [0,10) → 10 events from pure
    auto p = pure<std::string>("x");
    auto degraded = degradeBy(0.0, p);

    auto buffer = makeBuffer<std::string>(20);
    Arc arc{Rational{0}, Rational{10}};
    std::size_t count = degraded.query(arc, buffer);

    // 0% degradation: all 10 events kept
    REQUIRE(count == 10);
}

TEST_CASE("degradeBy 1.0 = empty (all removed)", "[combinators][degradeBy]")
{
    auto p = pure<std::string>("x");
    auto degraded = degradeBy(1.0, p);

    auto buffer = makeBuffer<std::string>(20);
    Arc arc{Rational{0}, Rational{10}};
    std::size_t count = degraded.query(arc, buffer);

    // 100% degradation: all events removed
    REQUIRE(count == 0);
}

TEST_CASE("degradeBy: deterministic — repeated queries produce identical results", "[combinators][degradeBy]")
{
    auto p = pure<std::string>("x");
    auto degraded = degradeBy(0.5, p);

    auto buffer1 = makeBuffer<std::string>(50);
    auto buffer2 = makeBuffer<std::string>(50);

    Arc arc{Rational{0}, Rational{50}};
    std::size_t count1 = degraded.query(arc, buffer1);
    std::size_t count2 = degraded.query(arc, buffer2);

    // Same instance, same arc → identical results
    REQUIRE(count1 == count2);
}

TEST_CASE("degradeBy: two default-seed instances are correlated (identical events)", "[combinators][degradeBy]")
{
    // Per Requirement 20.5 / degradeBy.md: degradeBy with the default seed (0,
    // matching Strudel's randSeed default) is a deterministic function of
    // (whole.start, seed). Two independently-constructed instances sharing the
    // default seed MUST therefore produce identical event lists at the same
    // positions (see degrade-by-0.5-instance-a.json == degrade-by-0.5-instance-b.json
    // in reference/strudel-golden). This also confirms the deprecated
    // per-instance auto-incrementing salt counter has been removed.
    auto p = pure<std::string>("x");
    auto degraded1 = degradeBy(0.5, p);
    auto degraded2 = degradeBy(0.5, p);  // distinct instance, default seed = 0

    auto buffer1 = makeBuffer<std::string>(100);
    auto buffer2 = makeBuffer<std::string>(100);

    Arc arc{Rational{0}, Rational{50}};  // inner: 50 events at whole.start = 0..49
    std::size_t count1 = degraded1.query(arc, buffer1);
    std::size_t count2 = degraded2.query(arc, buffer2);

    // Correlation: identical counts ...
    REQUIRE(count1 == count2);
    // ... and identical event positions, in production order.
    for (std::size_t i = 0; i < count1; ++i) {
        REQUIRE(buffer1[i].whole.start == buffer2[i].whole.start);
    }

    // Sanity: 0.5 degradation of 50 events yields a real (non-trivial) subset --
    // not identity (all kept) and not empty (all dropped).
    REQUIRE(count1 > 0);
    REQUIRE(count1 < 50);
}
