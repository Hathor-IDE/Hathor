// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// ---------------------------------------------------------------------------
// Zero-allocation operator new/delete override.
// Must be defined BEFORE any Catch2 or STL headers that might define their
// own operator new, so that the linker picks up this translation unit's
// definitions globally.
//
// We use a thread_local counting flag to avoid counting allocations that
// happen inside Catch2 itself (test framework infrastructure).
// ---------------------------------------------------------------------------
#include <cstdlib>    // std::malloc / std::free
#include <new>        // std::bad_alloc, std::size_t

static thread_local std::size_t g_alloc_count = 0;
static thread_local bool        g_counting    = false;

void* operator new(std::size_t size)
{
    if (g_counting) ++g_alloc_count;
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc{};
    return ptr;
}

void* operator new[](std::size_t size)
{
    if (g_counting) ++g_alloc_count;
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc{};
    return ptr;
}

void operator delete(void* ptr) noexcept  { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }

// ---------------------------------------------------------------------------
// Catch2 and hathor headers
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "hathor/Pattern.hpp"
#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"
#include "hathor/Event.hpp"

#include <vector>
#include <span>
#include <string>

using namespace hathor;

// Convenience: build a Rational from integer n/d
static Rational R(int64_t n, int64_t d = 1) { return Rational{n, d}; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Make a blank Event<std::string> with zero arcs for use as a vector fill.
static Event<std::string> blankEvent()
{
    Arc z{R(0), R(0)};
    return Event<std::string>{z, z, {}};
}

/// Query a pure<string> pattern over an arc, returning the resulting events.
static std::vector<Event<std::string>>
queryPureString(const Pattern<std::string>& pat, Arc arc)
{
    // Determine how many cycles the arc spans (conservatively +1).
    auto cycles = static_cast<std::size_t>(
        static_cast<int64_t>(std::ceil(arc.duration().toDouble())) + 2);
    std::size_t capacity = pat.maxEventsPerCycle() * cycles;

    // std::vector needs a default-constructible element type — supply a filler.
    std::vector<Event<std::string>> buf(capacity, blankEvent());
    std::span<Event<std::string>> sp(buf);
    std::size_t count = pat.query(arc, sp);
    buf.erase(buf.begin() + static_cast<std::ptrdiff_t>(count), buf.end());
    return buf;
}

// ---------------------------------------------------------------------------
// TEST SUITE: pure — arc coverage
// ---------------------------------------------------------------------------

TEST_CASE("pure: query over 0-1 produces one event", "[pattern][pure]")
{
    // Requirements: 1.1, 1.3
    auto pat = pure(std::string("bd"));
    Arc queryArc{R(0), R(1)};

    auto events = queryPureString(pat, queryArc);

    REQUIRE(events.size() == 1);

    // whole arc is exactly [0, 1)
    CHECK(events[0].whole.start == R(0));
    CHECK(events[0].whole.end   == R(1));

    // active == whole because query arc == whole cycle
    CHECK(events[0].active.start == R(0));
    CHECK(events[0].active.end   == R(1));

    CHECK(events[0].value == "bd");
}

TEST_CASE("pure: query over 0-2 produces two events", "[pattern][pure]")
{
    // Requirements: 1.1, 1.4
    auto pat = pure(std::string("bd"));
    Arc queryArc{R(0), R(2)};

    auto events = queryPureString(pat, queryArc);

    REQUIRE(events.size() == 2);

    // First event: cycle 0
    CHECK(events[0].whole.start == R(0));
    CHECK(events[0].whole.end   == R(1));
    CHECK(events[0].active.start == R(0));
    CHECK(events[0].active.end   == R(1));

    // Second event: cycle 1
    CHECK(events[1].whole.start == R(1));
    CHECK(events[1].whole.end   == R(2));
    CHECK(events[1].active.start == R(1));
    CHECK(events[1].active.end   == R(2));
}

TEST_CASE("pure: query over 1_4 to 3_4 produces one clipped event", "[pattern][pure]")
{
    // Requirements: 1.3, 7.1
    // Arc [1/4, 3/4) overlaps only cycle 0 ([0,1)).
    // whole = [0,1), active = [1/4, 3/4)
    auto pat = pure(std::string("bd"));
    Arc queryArc{R(1, 4), R(3, 4)};

    auto events = queryPureString(pat, queryArc);

    REQUIRE(events.size() == 1);

    // whole covers the full integer cycle
    CHECK(events[0].whole.start == R(0));
    CHECK(events[0].whole.end   == R(1));

    // active is the intersection with [1/4, 3/4)
    CHECK(events[0].active.start == R(1, 4));
    CHECK(events[0].active.end   == R(3, 4));

    CHECK(events[0].value == "bd");
}

TEST_CASE("pure: multi-cycle query 0-3 produces 3 events", "[pattern][pure]")
{
    // Requirements: 1.4
    auto pat = pure(std::string("sn"));
    Arc queryArc{R(0), R(3)};

    auto events = queryPureString(pat, queryArc);

    REQUIRE(events.size() == 3);

    for (int64_t c = 0; c < 3; ++c) {
        CHECK(events[static_cast<std::size_t>(c)].whole.start == R(c));
        CHECK(events[static_cast<std::size_t>(c)].whole.end   == R(c + 1));
        CHECK(events[static_cast<std::size_t>(c)].active.start == R(c));
        CHECK(events[static_cast<std::size_t>(c)].active.end   == R(c + 1));
        CHECK(events[static_cast<std::size_t>(c)].value == "sn");
    }
}

TEST_CASE("pure: query over fractional arc spanning two cycles", "[pattern][pure]")
{
    // [3/4, 5/4) overlaps cycle 0 ([0,1)) and cycle 1 ([1,2)).
    // Cycle 0 active = [3/4, 1), cycle 1 active = [1, 5/4)
    auto pat = pure(std::string("bd"));
    Arc queryArc{R(3, 4), R(5, 4)};

    auto events = queryPureString(pat, queryArc);

    REQUIRE(events.size() == 2);

    // Cycle 0
    CHECK(events[0].whole.start  == R(0));
    CHECK(events[0].whole.end    == R(1));
    CHECK(events[0].active.start == R(3, 4));
    CHECK(events[0].active.end   == R(1));

    // Cycle 1
    CHECK(events[1].whole.start  == R(1));
    CHECK(events[1].whole.end    == R(2));
    CHECK(events[1].active.start == R(1));
    CHECK(events[1].active.end   == R(5, 4));
}

// ---------------------------------------------------------------------------
// TEST SUITE: span-based API and buffer sizing
// ---------------------------------------------------------------------------

TEST_CASE("pure: span-based call with maxEventsPerCycle-sized buffer", "[pattern][pure][span]")
{
    // Requirements: 7.1, 7.2
    // Pre-size buffer to maxEventsPerCycle(), call query(), verify count.
    auto pat = pure(std::string("bd"));

    REQUIRE(pat.maxEventsPerCycle() == 1);

    std::vector<Event<std::string>> buf(pat.maxEventsPerCycle(), blankEvent());
    std::span<Event<std::string>> sp(buf);

    Arc arc{R(0), R(1)};
    std::size_t count = pat.query(arc, sp);

    REQUIRE(count == 1);
    CHECK(buf[0].whole.start  == R(0));
    CHECK(buf[0].whole.end    == R(1));
    CHECK(buf[0].active.start == R(0));
    CHECK(buf[0].active.end   == R(1));
    CHECK(buf[0].value == "bd");
}

TEST_CASE("pure: maxEventsPerCycle is 1", "[pattern][pure]")
{
    // Requirements: 7.3
    auto pat = pure(42);
    CHECK(pat.maxEventsPerCycle() == 1);
}

TEST_CASE("pure: empty arc produces no events", "[pattern][pure]")
{
    // An arc where start >= end should yield 0 events.
    auto pat = pure(std::string("bd"));
    Arc emptyArc{R(1), R(0)};  // start > end → empty

    auto events = queryPureString(pat, emptyArc);
    CHECK(events.empty());
}

TEST_CASE("pure: query with zero-size output buffer returns 0", "[pattern][pure][span]")
{
    auto pat = pure(std::string("bd"));
    std::span<Event<std::string>> emptySpan;
    std::size_t count = pat.query(Arc{R(0), R(1)}, emptySpan);
    CHECK(count == 0);
}

// ---------------------------------------------------------------------------
// MANDATORY: Zero-allocation test
// ---------------------------------------------------------------------------

TEST_CASE("pure query is allocation-free at call time", "[pattern][zero-alloc]")
{
    // Requirements: 7.1, 7.2
    //
    // Proof: override operator new (see top of file) with a counting version.
    // Construct the pattern and allocate the output buffer BEFORE enabling the
    // counter — std::function construction and vector allocation are fine on the
    // worker thread. Then enable the counter, call query(), and assert no
    // allocations occurred.

    using S = std::string;

    // Construct and warm up BEFORE counting starts.
    auto pat = pure(S("bd"));
    std::vector<Event<S>> buf(pat.maxEventsPerCycle(), blankEvent());
    std::span<Event<S>> sp(buf);

    // Reset and enable the counter.
    g_alloc_count = 0;
    g_counting    = true;

    std::size_t count = pat.query(Arc{R(0), R(1)}, sp);

    g_counting = false;

    // The query call must not have triggered any heap allocation.
    REQUIRE(g_alloc_count == 0);

    // Sanity: the query still produced the expected result.
    REQUIRE(count == 1);
    CHECK(buf[0].whole.start  == R(0));
    CHECK(buf[0].whole.end    == R(1));
    CHECK(buf[0].active.start == R(0));
    CHECK(buf[0].active.end   == R(1));
    CHECK(buf[0].value == S("bd"));
}

TEST_CASE("pure multi-cycle query is allocation-free at call time", "[pattern][zero-alloc]")
{
    // Same proof, but over a 3-cycle arc to exercise the loop.
    using S = std::string;

    auto pat = pure(S("sn"));
    // Buffer must hold maxEventsPerCycle * 3 cycles = 3
    std::vector<Event<S>> buf(pat.maxEventsPerCycle() * 3, blankEvent());
    std::span<Event<S>> sp(buf);

    g_alloc_count = 0;
    g_counting    = true;

    std::size_t count = pat.query(Arc{R(0), R(3)}, sp);

    g_counting = false;

    REQUIRE(g_alloc_count == 0);
    REQUIRE(count == 3);
}
