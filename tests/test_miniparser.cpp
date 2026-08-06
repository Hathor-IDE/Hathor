// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_miniparser.cpp — unit tests for the mini-notation parser.
 *
 * Requirement references: 5.1, 5.2, 5.3, 5.6, 19.2
 */

#include <catch2/catch_test_macros.hpp>

#include "hathor/MiniParser.hpp"
#include "hathor/PrettyPrinter.hpp"
#include "hathor/Combinators.hpp"
#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"
#include "hathor/Event.hpp"
#include "hathor/Pattern.hpp"

#include <span>
#include <string>
#include <variant>
#include <vector>

using namespace hathor;

// ---------------------------------------------------------------------------
// Helper: query a pattern and return events as a vector (allocates in test code).
// Event<std::string> has no default constructor (Rational lacks one), so we
// fill the vector with a dummy element.
// ---------------------------------------------------------------------------

static Event<std::string> blankEvent()
{
    Arc z{Rational{0}, Rational{0}};
    return Event<std::string>{z, z, {}};
}

static std::vector<Event<std::string>> queryToVec(const Pattern<std::string>& p, Arc arc)
{
    std::size_t bufSize = p.maxEventsPerCycle() * 8 + 8; // generous headroom
    std::vector<Event<std::string>> buf(bufSize, blankEvent());
    std::size_t n = p.query(arc, std::span<Event<std::string>>(buf));
    buf.erase(buf.begin() + static_cast<std::ptrdiff_t>(n), buf.end());
    return buf;
}

static CompiledPattern& asCP(std::variant<CompiledPattern, ParseError>& v)
{
    REQUIRE(std::holds_alternative<CompiledPattern>(v));
    return std::get<CompiledPattern>(v);
}

// ---------------------------------------------------------------------------
// Basic atom
// ---------------------------------------------------------------------------

TEST_CASE("parse simple atom", "[miniparser]")
{
    auto result = parseMini("bd");
    auto& cp = asCP(result);

    Arc arc{Rational{0}, Rational{1}};
    auto events = queryToVec(cp.pattern, arc);

    REQUIRE(events.size() == 1);
    CHECK(events[0].value == "bd");
    CHECK(events[0].whole.start == Rational{0});
    CHECK(events[0].whole.end   == Rational{1});
}

// ---------------------------------------------------------------------------
// Space-separated sequence
// ---------------------------------------------------------------------------

TEST_CASE("parse space-separated sequence", "[miniparser]")
{
    auto result = parseMini("bd sn");
    auto& cp = asCP(result);

    Arc arc{Rational{0}, Rational{1}};
    auto events = queryToVec(cp.pattern, arc);

    REQUIRE(events.size() == 2);
    CHECK(events[0].value == "bd");
    CHECK(events[1].value == "sn");

    // Each occupies half a cycle.
    CHECK(events[0].whole.start == Rational{0});
    CHECK(events[0].whole.end   == Rational{1, 2});
    CHECK(events[1].whole.start == Rational{1, 2});
    CHECK(events[1].whole.end   == Rational{1});
}

// ---------------------------------------------------------------------------
// Bracket subsequence [a b c]
// ---------------------------------------------------------------------------

TEST_CASE("parse bracket subsequence", "[miniparser]")
{
    auto result = parseMini("[a b c]");
    auto& cp = asCP(result);

    Arc arc{Rational{0}, Rational{1}};
    auto events = queryToVec(cp.pattern, arc);

    REQUIRE(events.size() == 3);
    CHECK(events[0].value == "a");
    CHECK(events[1].value == "b");
    CHECK(events[2].value == "c");
}

// ---------------------------------------------------------------------------
// Angle-bracket slow sequence <a b>
// ---------------------------------------------------------------------------

TEST_CASE("parse angle bracket slow sequence", "[miniparser]")
{
    auto result = parseMini("<a b>");
    auto& cp = asCP(result);

    // Cycle 0 → "a"
    Arc arc0{Rational{0}, Rational{1}};
    auto ev0 = queryToVec(cp.pattern, arc0);
    REQUIRE(ev0.size() == 1);
    CHECK(ev0[0].value == "a");

    // Cycle 1 → "b"
    Arc arc1{Rational{1}, Rational{2}};
    auto ev1 = queryToVec(cp.pattern, arc1);
    REQUIRE(ev1.size() == 1);
    CHECK(ev1[0].value == "b");
}

// ---------------------------------------------------------------------------
// Fast operator bd*4
// ---------------------------------------------------------------------------

TEST_CASE("parse fast operator bd*4", "[miniparser]")
{
    auto result = parseMini("bd*4");
    auto& cp = asCP(result);

    Arc arc{Rational{0}, Rational{1}};
    auto events = queryToVec(cp.pattern, arc);

    REQUIRE(events.size() == 4);
    for (auto& ev : events)
        CHECK(ev.value == "bd");
}

// ---------------------------------------------------------------------------
// Slow operator bd/2
// ---------------------------------------------------------------------------

TEST_CASE("parse slow operator bd/2", "[miniparser]")
{
    auto result = parseMini("bd/2");
    auto& cp = asCP(result);

    // Over [0,1) the pattern is slowed to 2 cycles — only half the event fires.
    Arc arc{Rational{0}, Rational{1}};
    auto ev1 = queryToVec(cp.pattern, arc);
    // A slowed-by-2 pure("bd") produces 1 event over [0,2).
    // Over [0,1) the whole arc is [0,2), active is [0,1) — still 1 partial event.
    REQUIRE(ev1.size() == 1);
    CHECK(ev1[0].value == "bd");
    // whole arc spans two cycles
    CHECK(ev1[0].whole.start == Rational{0});
    CHECK(ev1[0].whole.end   == Rational{2});

    // Over [0,2) → exactly 1 event.
    Arc arc2{Rational{0}, Rational{2}};
    auto ev2 = queryToVec(cp.pattern, arc2);
    REQUIRE(ev2.size() == 1);
    CHECK(ev2[0].value == "bd");
}

// ---------------------------------------------------------------------------
// Replicate operator bd!3
// ---------------------------------------------------------------------------

TEST_CASE("parse replicate bd!3", "[miniparser]")
{
    auto result = parseMini("bd!3");
    auto& cp = asCP(result);

    Arc arc{Rational{0}, Rational{1}};
    auto events = queryToVec(cp.pattern, arc);

    // bd!3 → fastcat of 3 copies of pure("bd") → 3 events per cycle.
    REQUIRE(events.size() == 3);
    for (auto& ev : events)
        CHECK(ev.value == "bd");
}

// ---------------------------------------------------------------------------
// Tilde silence token
// ---------------------------------------------------------------------------

TEST_CASE("parse tilde silence", "[miniparser]")
{
    // Ground truth: reference/strudel-golden/rest-bd-sn.json — "bd ~ sn ~"
    // queried over [1/4, 1/2) (the first ~ slot) returns ZERO events.
    // The mini-notation-grammar.md confirms: "~" maps to silence (no event
    // emitted), not to an event carrying a "~" value. The parser is correct;
    // this test was originally wrong to expect 1 event.
    auto result = parseMini("~");
    auto& cp = asCP(result);

    Arc arc{Rational{0}, Rational{1}};
    auto events = queryToVec(cp.pattern, arc);

    REQUIRE(events.size() == 0);
}

// ---------------------------------------------------------------------------
// Error: unclosed bracket
// ---------------------------------------------------------------------------

TEST_CASE("parse error unclosed bracket", "[miniparser]")
{
    auto result = parseMini("[a b");
    REQUIRE(std::holds_alternative<ParseError>(result));
    const auto& err = std::get<ParseError>(result);
    // Position should point to the opening bracket or beyond.
    CHECK(err.position < 4); // somewhere in the input
    CHECK(!err.message.empty());
}

// ---------------------------------------------------------------------------
// Error: invalid modifier (star with no integer)
// ---------------------------------------------------------------------------

TEST_CASE("parse error invalid modifier", "[miniparser]")
{
    auto result = parseMini("bd*");
    REQUIRE(std::holds_alternative<ParseError>(result));
}

// ---------------------------------------------------------------------------
// Error: empty input
// ---------------------------------------------------------------------------

TEST_CASE("parse error empty input", "[miniparser]")
{
    auto result = parseMini("");
    REQUIRE(std::holds_alternative<ParseError>(result));
}

// ---------------------------------------------------------------------------
// Combination: "bd sn [hh hh] cp"
// ---------------------------------------------------------------------------

TEST_CASE("parse combination bd sn hh-sub cp", "[miniparser]")
{
    auto result = parseMini("bd sn [hh hh] cp");
    auto& cp = asCP(result);

    Arc arc{Rational{0}, Rational{1}};
    auto events = queryToVec(cp.pattern, arc);

    // 4 top-level slots → fastcat divides cycle into 4 slices.
    // Slots: bd, sn, [hh hh], cp.
    // [hh hh] is fastcat(hh, hh) in slot 3 → produces 2 events in that slot.
    // Total: 1 + 1 + 2 + 1 = 5 events.
    REQUIRE(events.size() == 5);
    CHECK(events[0].value == "bd");
    CHECK(events[1].value == "sn");
    CHECK(events[2].value == "hh");
    CHECK(events[3].value == "hh");
    CHECK(events[4].value == "cp");
}

// ---------------------------------------------------------------------------
// Nested brackets with modifier: [bd sn]*2
// ---------------------------------------------------------------------------

TEST_CASE("parse nested bracket with fast modifier", "[miniparser]")
{
    auto result = parseMini("[bd sn]*2");
    auto& cp = asCP(result);

    Arc arc{Rational{0}, Rational{1}};
    auto events = queryToVec(cp.pattern, arc);

    // [bd sn]*2 = fast(2, fastcat(bd, sn)) → 4 events per cycle.
    // Ground truth: reference/strudel-golden/fast-seq-bd-sn-star-2.json
    // Strudel returns events in stack/sub-pattern order (not time order):
    //   bd at [0, 1/4), bd at [1/2, 3/4),   -- bd sub-pattern events first
    //   sn at [1/4, 1/2), sn at [3/4, 1)    -- sn sub-pattern events second
    REQUIRE(events.size() == 4);
    CHECK(events[0].value == "bd");
    CHECK(events[0].whole.start == Rational{0});
    CHECK(events[0].whole.end   == Rational{1, 4});
    CHECK(events[1].value == "bd");
    CHECK(events[1].whole.start == Rational{1, 2});
    CHECK(events[1].whole.end   == Rational{3, 4});
    CHECK(events[2].value == "sn");
    CHECK(events[2].whole.start == Rational{1, 4});
    CHECK(events[2].whole.end   == Rational{1, 2});
    CHECK(events[3].value == "sn");
    CHECK(events[3].whole.start == Rational{3, 4});
    CHECK(events[3].whole.end   == Rational{1});
}

// ---------------------------------------------------------------------------
// Multiple cycles query
// ---------------------------------------------------------------------------

TEST_CASE("parse multi-cycle query", "[miniparser]")
{
    auto result = parseMini("bd");
    auto& cp = asCP(result);

    Arc arc{Rational{0}, Rational{3}};
    auto events = queryToVec(cp.pattern, arc);

    REQUIRE(events.size() == 3);
    for (auto& ev : events)
        CHECK(ev.value == "bd");
}
