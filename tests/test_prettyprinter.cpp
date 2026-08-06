// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_prettyprinter.cpp — unit tests for the PrettyPrinter and the mandatory
 * round-trip property (Property 2 / Req 19.2).
 *
 * Requirement references: 5.4, 5.5, 19.2
 */

#include <catch2/catch_test_macros.hpp>

#include "hathor/MiniParser.hpp"
#include "hathor/PrettyPrinter.hpp"
#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"
#include "hathor/Event.hpp"
#include "hathor/Pattern.hpp"

#include <array>
#include <span>
#include <string>
#include <variant>
#include <vector>

using namespace hathor;

// ---------------------------------------------------------------------------
// Helper: query a Pattern into a vector (allocation OK in tests).
// Event<std::string> has no default constructor, so fill with a dummy element.
// ---------------------------------------------------------------------------

static Event<std::string> blankEvent()
{
    Arc z{Rational{0}, Rational{0}};
    return Event<std::string>{z, z, {}};
}

static std::vector<Event<std::string>> queryToVec(const Pattern<std::string>& p, Arc arc)
{
    std::size_t cap = p.maxEventsPerCycle() * 8 + 8;
    std::vector<Event<std::string>> buf(cap, blankEvent());
    std::size_t n = p.query(arc, std::span<Event<std::string>>(buf));
    buf.erase(buf.begin() + static_cast<std::ptrdiff_t>(n), buf.end());
    return buf;
}

// ---------------------------------------------------------------------------
// Helper: parse successfully (fails the test if parsing fails).
// ---------------------------------------------------------------------------

static CompiledPattern parseOK(std::string_view s)
{
    auto result = parseMini(s);
    REQUIRE(std::holds_alternative<CompiledPattern>(result));
    return std::move(std::get<CompiledPattern>(result));
}

// ---------------------------------------------------------------------------
// Basic pretty-print tests
// ---------------------------------------------------------------------------

TEST_CASE("print atom", "[prettyprinter]")
{
    auto cp = parseOK("bd");
    CHECK(printMini(cp) == "bd");
}

TEST_CASE("print tilde", "[prettyprinter]")
{
    auto cp = parseOK("~");
    CHECK(printMini(cp) == "~");
}

TEST_CASE("print sequence", "[prettyprinter]")
{
    auto cp = parseOK("bd sn");
    CHECK(printMini(cp) == "bd sn");
}

TEST_CASE("print three-element sequence", "[prettyprinter]")
{
    auto cp = parseOK("bd sn cp");
    CHECK(printMini(cp) == "bd sn cp");
}

TEST_CASE("print bracket subsequence", "[prettyprinter]")
{
    auto cp = parseOK("[a b c]");
    CHECK(printMini(cp) == "[a b c]");
}

TEST_CASE("print angle bracket slow sequence", "[prettyprinter]")
{
    auto cp = parseOK("<a b>");
    CHECK(printMini(cp) == "<a b>");
}

TEST_CASE("print fast operator", "[prettyprinter]")
{
    auto cp = parseOK("bd*4");
    CHECK(printMini(cp) == "bd*4");
}

TEST_CASE("print slow operator", "[prettyprinter]")
{
    auto cp = parseOK("bd/2");
    CHECK(printMini(cp) == "bd/2");
}

TEST_CASE("print replicate operator", "[prettyprinter]")
{
    auto cp = parseOK("bd!3");
    CHECK(printMini(cp) == "bd!3");
}

// ---------------------------------------------------------------------------
// Property 2 — Mini-Notation Round-Trip (MANDATORY, Req 19.2)
//
// For each of 20 corpus strings:
//   1. Parse s → CompiledPattern A
//   2. Print A → s'
//   3. Parse s' → CompiledPattern B
//   4. For each arc in the 6-arc representative set, query both A and B and
//      assert the event lists are identical (count, whole arcs, active arcs,
//      values).
//
// This is the ONLY proof that the parser and pretty-printer are mutually
// consistent. Test does NOT compare Pattern objects directly.
// ---------------------------------------------------------------------------

TEST_CASE("round-trip property - all 20 corpus strings", "[prettyprinter][round-trip]")
{
    // 6-arc representative set.
    const std::array<Arc, 6> arcs = {{
        Arc{Rational{0},    Rational{1}},       // [0, 1)
        Arc{Rational{0},    Rational{1, 4}},    // [0, 1/4)
        Arc{Rational{1, 4}, Rational{1, 2}},    // [1/4, 1/2)
        Arc{Rational{1, 2}, Rational{3, 4}},    // [1/2, 3/4)
        Arc{Rational{3, 4}, Rational{1}},       // [3/4, 1)
        Arc{Rational{0},    Rational{2}},       // [0, 2)
    }};

    const std::array<std::string_view, 20> corpus = {{
        "bd",
        "sn",
        "~",
        "bd sn",
        "bd sn cp",
        "[hh hh]",
        "[a b c]",
        "<a b>",
        "<a b c>",
        "bd*2",
        "bd*4",
        "bd/2",
        "bd!2",
        "bd!3",
        "bd sn [hh hh] cp",
        "[a b] [c d]",
        "<a b> sn",
        "bd*2 sn",
        "[bd sn]*2",
        "<a b c d>",
    }};

    for (auto s : corpus) {
        INFO("corpus string: \"" << s << "\"");

        // Step 1: parse s → A
        auto rA = parseMini(s);
        REQUIRE(std::holds_alternative<CompiledPattern>(rA));
        auto& cpA = std::get<CompiledPattern>(rA);

        // Step 2: print A → s'
        std::string sp = printMini(cpA);
        INFO("printed: \"" << sp << "\"");
        REQUIRE(!sp.empty());

        // Step 3: parse s' → B
        auto rB = parseMini(sp);
        INFO("re-parse of printed string failed");
        REQUIRE(std::holds_alternative<CompiledPattern>(rB));
        auto& cpB = std::get<CompiledPattern>(rB);

        // Step 4: query both over each arc and compare.
        for (const auto& arc : arcs) {
            INFO("  arc: [" << arc.start.toDouble() << ", " << arc.end.toDouble() << ")");

            auto evA = queryToVec(cpA.pattern, arc);
            auto evB = queryToVec(cpB.pattern, arc);

            REQUIRE(evA.size() == evB.size());
            for (std::size_t i = 0; i < evA.size(); ++i) {
                INFO("    event " << i);
                CHECK(evA[i].whole.start  == evB[i].whole.start);
                CHECK(evA[i].whole.end    == evB[i].whole.end);
                CHECK(evA[i].active.start == evB[i].active.start);
                CHECK(evA[i].active.end   == evB[i].active.end);
                CHECK(evA[i].value        == evB[i].value);
            }
        }
    }
}
