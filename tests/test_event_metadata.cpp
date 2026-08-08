// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_event_metadata.cpp — B2 tests for sourceOffset and slotId propagation.
 *
 * Verifies that:
 * - lowerToParamMap preserves sourceOffset from Event<std::string> → Event<ParamMap>
 * - Combinators preserve sourceOffset (events are copied through the pipeline)
 * - Event copying preserves metadata fields
 * - No pointer/string_view lifetime dependency exists
 */

#include <catch2/catch_test_macros.hpp>

#include "hathor/MiniParser.hpp"
#include "hathor/Pattern.hpp"
#include "hathor/Event.hpp"
#include "hathor/ParamMap.hpp"
#include "hathor/Combinators.hpp"
#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"
#include "hathor/PatternCompiler.hpp"

#include <span>
#include <string>
#include <vector>

using namespace hathor;

// ---------------------------------------------------------------------------
// Helper: query a Pattern<string> and return events as a vector
// ---------------------------------------------------------------------------

static Event<std::string> blankStringEvent()
{
    Arc z{Rational{0}, Rational{0}};
    return Event<std::string>{z, z, {}};
}

static std::vector<Event<std::string>> queryStringVec(const Pattern<std::string>& p, Arc arc)
{
    std::size_t bufSize = p.maxEventsPerCycle() * 8 + 8;
    std::vector<Event<std::string>> buf(bufSize, blankStringEvent());
    std::size_t n = p.query(arc, std::span<Event<std::string>>(buf));
    buf.erase(buf.begin() + static_cast<std::ptrdiff_t>(n), buf.end());
    return buf;
}

// ---------------------------------------------------------------------------
// Helper: query a Pattern<ParamMap> and return events as a vector
// ---------------------------------------------------------------------------

static Event<ParamMap> blankParamEvent()
{
    Arc z{Rational{0}, Rational{0}};
    return Event<ParamMap>{z, z, {}};
}

static std::vector<Event<ParamMap>> queryParamsVec(const Pattern<ParamMap>& p, Arc arc)
{
    std::size_t bufSize = p.maxEventsPerCycle() * 8 + 8;
    std::vector<Event<ParamMap>> buf(bufSize, blankParamEvent());
    std::size_t n = p.query(arc, std::span<Event<ParamMap>>(buf));
    buf.erase(buf.begin() + static_cast<std::ptrdiff_t>(n), buf.end());
    return buf;
}

// ---------------------------------------------------------------------------
// lowerToParamMap preserves sourceOffset
// ---------------------------------------------------------------------------

TEST_CASE("lowerToParamMap preserves sourceOffset", "[b2][lowerToParamMap]")
{
    auto result = parseMini("bd sn");
    REQUIRE(std::holds_alternative<CompiledPattern>(result));
    auto& cp = std::get<CompiledPattern>(result);

    // Source pattern events should carry their source offsets.
    Arc arc{Rational{0}, Rational{1}};
    auto srcEvents = queryStringVec(cp.pattern, arc);
    REQUIRE(srcEvents.size() == 2);
    CHECK(srcEvents[0].value == "bd");
    CHECK(srcEvents[0].sourceOffset == 0);
    CHECK(srcEvents[1].value == "sn");
    CHECK(srcEvents[1].sourceOffset == 3);

    // After lowering to ParamMap, the sourceOffset must survive.
    auto paramPattern = lowerToParamMap(cp.pattern);
    auto paramEvents = queryParamsVec(paramPattern, arc);

    REQUIRE(paramEvents.size() == 2);
    CHECK(paramEvents[0].sourceOffset == 0);
    CHECK(paramEvents[1].sourceOffset == 3);

    // Verify the sample name survived too.
    const Value* s0 = paramEvents[0].value.get(keys::kS);
    REQUIRE(s0 != nullptr);
    CHECK(std::get<std::string>(*s0) == "bd");
}

// ---------------------------------------------------------------------------
// lowerToParamMap with offset in non-zero position
// ---------------------------------------------------------------------------

TEST_CASE("lowerToParamMap preserves sourceOffset at non-zero position", "[b2][lowerToParamMap]")
{
    auto result = parseMini("  bd*2  sn");
    REQUIRE(std::holds_alternative<CompiledPattern>(result));
    auto& cp = std::get<CompiledPattern>(result);

    Arc arc{Rational{0}, Rational{1}};
    auto paramPattern = lowerToParamMap(cp.pattern);
    auto paramEvents = queryParamsVec(paramPattern, arc);

    REQUIRE(paramEvents.size() == 3);
    // "bd" appears at offset 2, "sn" at offset 8
    CHECK(paramEvents[0].sourceOffset == 2);
    CHECK(paramEvents[1].sourceOffset == 2);  // repeated from *2
    CHECK(paramEvents[2].sourceOffset == 8);
}

// ---------------------------------------------------------------------------
// Combinators preserve sourceOffset
// ---------------------------------------------------------------------------

TEST_CASE("fast combinator preserves sourceOffset", "[b2][combinator]")
{
    // "bd*2" — lowerNode produces fast(2, pure("bd", offset=0))
    auto result = parseMini("bd*2");
    REQUIRE(std::holds_alternative<CompiledPattern>(result));
    auto& cp = std::get<CompiledPattern>(result);

    Arc arc{Rational{0}, Rational{1}};
    auto events = queryStringVec(cp.pattern, arc);

    REQUIRE(events.size() == 2);
    CHECK(events[0].sourceOffset == 0);
    CHECK(events[1].sourceOffset == 0);
}

TEST_CASE("slow combinator preserves sourceOffset", "[b2][combinator]")
{
    // "bd/2" — lowerNode produces slow(2, pure("bd", offset=0))
    auto result = parseMini("bd/2");
    REQUIRE(std::holds_alternative<CompiledPattern>(result));
    auto& cp = std::get<CompiledPattern>(result);

    Arc arc{Rational{0}, Rational{2}};
    auto events = queryStringVec(cp.pattern, arc);

    REQUIRE(events.size() == 1);
    CHECK(events[0].value == "bd");
    CHECK(events[0].sourceOffset == 0);
}

TEST_CASE("stack combinator preserves sourceOffset for each branch", "[b2][combinator]")
{
    // "bd,sn" — stack of two atoms with different offsets
    auto result = parseMini("bd,sn");
    REQUIRE(std::holds_alternative<CompiledPattern>(result));
    auto& cp = std::get<CompiledPattern>(result);

    Arc arc{Rational{0}, Rational{1}};
    auto events = queryStringVec(cp.pattern, arc);

    REQUIRE(events.size() == 2);
    // Both events fire at cycle 0 (stack plays concurrently)
    CHECK(events[0].sourceOffset == 0);   // "bd" at offset 0
    CHECK(events[1].sourceOffset == 3);   // "sn" at offset 3
}

TEST_CASE("stepcat combinator preserves sourceOffset", "[b2][combinator]")
{
    // "bd sn" — stepcat of two atoms with different offsets
    auto result = parseMini("bd sn");
    REQUIRE(std::holds_alternative<CompiledPattern>(result));
    auto& cp = std::get<CompiledPattern>(result);

    Arc arc{Rational{0}, Rational{1}};
    auto events = queryStringVec(cp.pattern, arc);

    REQUIRE(events.size() == 2);
    CHECK(events[0].sourceOffset == 0);
    CHECK(events[1].sourceOffset == 3);
}

TEST_CASE("fastcat combinator preserves sourceOffset", "[b2][combinator]")
{
    // "bd sn" — stepcat of two atoms with different offsets, queried over 2 cycles
    auto result = parseMini("bd sn");
    REQUIRE(std::holds_alternative<CompiledPattern>(result));
    auto& cp = std::get<CompiledPattern>(result);

    // Query multiple cycles to see offset preserved across cycles
    Arc arc{Rational{0}, Rational{2}};
    auto events = queryStringVec(cp.pattern, arc);

    // 2 events per cycle * 2 cycles = 4 events.
    // stepcat/stack groups by sub-pattern, so ordering is:
    //   bd(cycle 0), bd(cycle 1), sn(cycle 0), sn(cycle 1)
    REQUIRE(events.size() == 4);
    // All "bd" events should have sourceOffset 0
    REQUIRE(events[0].sourceOffset == 0);
    REQUIRE(events[1].sourceOffset == 0);
    // All "sn" events should have sourceOffset 3
    REQUIRE(events[2].sourceOffset == 3);
    REQUIRE(events[3].sourceOffset == 3);
}

// ---------------------------------------------------------------------------
// Event copy preserves metadata
// ---------------------------------------------------------------------------

TEST_CASE("Event copy constructor preserves metadata", "[b2][event]")
{
    Arc a{Rational{0}, Rational{1}};
    Event<std::string> ev{a, a, "bd"};
    ev.sourceOffset = 42;
    ev.slotId = 5;

    Event<std::string> copy = ev;
    CHECK(copy.sourceOffset == 42);
    CHECK(copy.slotId == 5);
    CHECK(copy.value == "bd");
}

TEST_CASE("Event copy assignment preserves metadata", "[b2][event]")
{
    Arc a{Rational{0}, Rational{1}};
    Event<std::string> src{a, a, "sn"};
    src.sourceOffset = 99;
    src.slotId = -1;

    Event<std::string> dst{a, a, "bd"};
    dst.sourceOffset = 0;
    dst.slotId = 0;

    dst = src;
    CHECK(dst.sourceOffset == 99);
    CHECK(dst.slotId == -1);
    CHECK(dst.value == "sn");
}

// ---------------------------------------------------------------------------
// No pointer/string_view lifetime dependency
// ---------------------------------------------------------------------------

TEST_CASE("sourceOffset is stable after input string is destroyed", "[b2][lifetime]")
{
    std::size_t capturedOffset = 0;
    {
        std::string input = "bd sn";
        auto result = parseMini(input);
        REQUIRE(std::holds_alternative<CompiledPattern>(result));
        auto& cp = std::get<CompiledPattern>(result);

        Arc arc{Rational{0}, Rational{1}};
        auto events = queryStringVec(cp.pattern, arc);

        REQUIRE(events.size() == 2);
        capturedOffset = events[0].sourceOffset;
        CHECK(events[0].sourceOffset == 0);
        CHECK(events[1].sourceOffset == 3);
    }
    // input is now out of scope — sourceOffset must remain valid
    CHECK(capturedOffset == 0);
}

// ---------------------------------------------------------------------------
// Default values
// ---------------------------------------------------------------------------

TEST_CASE("Event default-constructed metadata has sensible defaults", "[b2][event]")
{
    // Event<T> with default member initializers for sourceOffset and slotId
    Arc a{Rational{0}, Rational{0}};
    Event<std::string> ev{a, a, "x"};

    CHECK(ev.sourceOffset == 0);
    CHECK(ev.slotId == -1);
}
