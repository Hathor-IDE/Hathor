// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_strudel_differential.cpp — differential testing of the Hathor pattern
 * engine against Strudel golden fixtures.
 *
 * For every JSON file in reference/strudel-golden/:
 *   1. If `mini_notation` is non-null, parse it via parseMini() — a parse
 *      failure is a hard test failure (Strudel accepted it, Hathor must too).
 *   2. If `mini_notation` is null (direct-API fixtures), construct the
 *      equivalent pattern via the corresponding C++ combinator call.
 *   3. For each queries[] entry, query the pattern over the given arc and
 *      compare the resulting event list against the fixture's events[]:
 *      same count, and for each event, whole, active, and value must match
 *      exactly (Rational equality, exact string equality — no floating-point
 *      tolerance).
 *
 * Requirements: 3.1–3.11, 5.1–5.6 (verified against external ground truth
 * rather than self-consistency alone)
 *
 * Requirement references: 7.2
 */

#include <catch2/catch_test_macros.hpp>

#include "hathor/Combinators.hpp"
#include "hathor/Pattern.hpp"
#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"
#include "hathor/Event.hpp"
#include "hathor/MiniParser.hpp"

#include "FixtureLoader.hpp"

#include <cstddef>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace hathor;
using namespace hathor::test;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Directory containing the golden fixtures (relative to repo root).
// Set via GOLDEN_FIXTURE_DIR compile definition in CMakeLists.txt.
// ---------------------------------------------------------------------------
static const fs::path goldenDir()
{
    return fs::path(GOLDEN_FIXTURE_DIR);
}

// ---------------------------------------------------------------------------
// Helper: build a Hathor buffer of N blank events.
// Event<std::string> has no default constructor (Rational has no default ctor).
// ---------------------------------------------------------------------------
static Event<std::string> blankEvent()
{
    Arc z{Rational{0}, Rational{0}};
    return Event<std::string>{z, z, {}};
}

static std::vector<Event<std::string>> makeBuffer(std::size_t n)
{
    return std::vector<Event<std::string>>(n, blankEvent());
}

// ---------------------------------------------------------------------------
// Helper: construct a Pattern<std::string> from a direct-API fixture filename.
// These are fixtures where mini_notation is null and the pattern was built
// via core.* combinators.  We replicate the exact construction here.
// ---------------------------------------------------------------------------
static Pattern<std::string> buildDirectApiPattern(const std::string& filename)
{
    // Helper to build fast(pure("bd"), N) — i.e. "bd*N"
    auto fastPure = [](int N) {
        return fast(Rational{static_cast<int64_t>(N)}, pure(std::string{"bd"}));
    };

    // Helper to build fastcat of atoms
    auto fastcatAtoms = [](const std::vector<std::string>& atoms) {
        std::vector<Pattern<std::string>> pats;
        pats.reserve(atoms.size());
        for (const auto& a : atoms)
            pats.push_back(pure(a));
        return fastcat(std::move(pats));
    };

    if (filename == "stack-bd-sn-api") {
        // core.stack(pure("bd"), pure("sn"))
        return stack<std::string>({pure(std::string{"bd"}), pure(std::string{"sn"})});
    }
    if (filename == "rev-bd-sn-hh-cp") {
        // core.rev(m("bd sn hh cp"))
        return rev(fastcatAtoms({"bd", "sn", "hh", "cp"}));
    }
    if (filename == "every-3-rev-bd-sn") {
        // core.every(3, core.rev, m("bd sn"))
        auto revFn = [](Pattern<std::string> p) -> Pattern<std::string> {
            return rev(std::move(p));
        };
        return every<std::string>(3, revFn, fastcatAtoms({"bd", "sn"}));
    }
    if (filename == "iter-4-bd-sn-hh-cp") {
        // core.iter(4, m("bd sn hh cp"))
        return iter(4, fastcatAtoms({"bd", "sn", "hh", "cp"}));
    }
    if (filename == "degrade-by-0.0") {
        // core.degradeBy(0.0, m("bd*8"))
        return degradeBy(0.0, fastPure(8));
    }
    if (filename == "degrade-by-0.5-instance-a") {
        // core.degradeBy(0.5, m("bd*16"))
        return degradeBy(0.5, fastPure(16));
    }
    if (filename == "degrade-by-0.5-instance-b") {
        // core.degradeBy(0.5, m("bd*16"))  (distinct instance, default seed)
        return degradeBy(0.5, fastPure(16));
    }
    if (filename == "degrade-by-1.0") {
        // core.degradeBy(1.0, m("bd*8"))
        return degradeBy(1.0, fastPure(8));
    }

    // Should never reach here if expectedFixtureNames() is kept in sync.
    throw std::runtime_error("no direct-API builder for fixture: " + filename);
}

// ---------------------------------------------------------------------------
// Stream operator for Rational (for diagnostic output).
// Must be defined before any function that prints Rational values.
// ---------------------------------------------------------------------------
static std::ostream& operator<<(std::ostream& os, const Rational& r)
{
    if (r.den == 1)
        os << r.num;
    else
        os << r.num << "/" << r.den;
    return os;
}

// ---------------------------------------------------------------------------
// Helper: format an event for diagnostic output.
// ---------------------------------------------------------------------------
static std::string fmtEvent(const Event<std::string>& ev)
{
    std::ostringstream oss;
    oss << "{ whole:[" << ev.whole.start << ".." << ev.whole.end << "]"
        << " active:[" << ev.active.start << ".." << ev.active.end << "]"
        << " value:" << ev.value << " }";
    return oss.str();
}

static std::string fmtEvent(const FixtureEvent& ev)
{
    std::ostringstream oss;
    oss << "{ whole:[" << ev.whole.start << ".." << ev.whole.end << "]"
        << " active:[" << ev.active.start << ".." << ev.active.end << "]"
        << " value:" << ev.value << " }";
    return oss.str();
}

static std::string fmtArc(Arc a)
{
    std::ostringstream oss;
    oss << "[" << a.start << ".." << a.end << ")";
    return oss.str();
}

// ---------------------------------------------------------------------------
// TEST SUITE: Golden directory file-coverage check
// ---------------------------------------------------------------------------
// Ensures every .json file in the golden directory is accounted for in the
// expectedFixtureNames() list, so future fixture additions don't silently go
// unverified.
// ---------------------------------------------------------------------------

TEST_CASE("all golden fixture files are covered by expectedFixtureNames", "[strudel][fixture-coverage]")
{
    const fs::path dir = goldenDir();
    REQUIRE(fs::is_directory(dir));

    // Collect actual .json filenames (without extension).
    std::vector<std::string> actual;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
            actual.push_back(entry.path().stem().string());
    }
    std::sort(actual.begin(), actual.end());

    const auto expected = expectedFixtureNames();

    INFO("Mismatch between expected and actual fixture files:");
    for (const auto& name : actual) {
        bool found = std::find(expected.begin(), expected.end(), name) != expected.end();
        INFO("  actual file: " << name << (found ? " (covered)" : " (UNCOVERED — add to expectedFixtureNames())"));
        REQUIRE(found);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE: Strudel differential testing
// ---------------------------------------------------------------------------
// For each golden fixture:
//   - Parse or construct the pattern
//   - Query over each arc in queries[]
//   - Compare event lists exactly
// ---------------------------------------------------------------------------

TEST_CASE("Strudel golden fixtures — parse & coverage", "[strudel][differential]")
{
    const auto fixtures = loadAllFixtures(goldenDir().string());
    REQUIRE(!fixtures.empty());

    for (const auto& nf : fixtures) {
        SECTION("fixture: " + nf.filename)
        {
            // ---- Step 1/2: obtain a pattern ----
            Pattern<std::string> pat = [&]() -> Pattern<std::string> {
                if (nf.fixture.mini_notation) {
                    // Parse via parseMini() — a failure is a real bug.
                    auto result = parseMini(*nf.fixture.mini_notation);
                    if (std::holds_alternative<ParseError>(result)) {
                        const auto& err = std::get<ParseError>(result);
                        FAIL("parseMini failed for fixture '" + nf.filename + "' on mini-notation '"
                             + *nf.fixture.mini_notation + "': " + err.message
                             + " at position " + std::to_string(err.position));
                    }
                    auto& cp = std::get<CompiledPattern>(result);
                    return cp.pattern;
                } else {
                    // Direct API fixture — construct via C++ combinator call.
                    return buildDirectApiPattern(nf.filename);
                }
            }();

            // ---- Step 3: query and compare for each fixture query ----
            for (const auto& query : nf.fixture.queries) {
                // If Strudel recorded an error for this query, skip comparison
                // (the query threw in Strudel; there's nothing to compare).
                if (!query.error.empty())
                    continue;

                SECTION("arc " + fmtArc(query.arc))
                {
                    // Query the pattern.
                    // Size the buffer generously — euclid/degradeBy can have
                    // many onsets per cycle over multi-cycle arcs.
                    std::size_t cycles =
                        static_cast<std::size_t>(
                            static_cast<int64_t>(
                                std::ceil(query.arc.duration().toDouble())) + 2);
                    std::size_t bufSize = pat.maxEventsPerCycle() * cycles + 16;
                    auto buf = makeBuffer(bufSize);
                    std::size_t actualCount =
                        pat.query(query.arc, std::span<Event<std::string>>(buf));

                    // Compare event count.
                    if (actualCount != query.events.size()) {
                        // Build full diagnostic.
                        std::ostringstream diag;
                        diag << "event count mismatch in fixture '" << nf.filename << "'"
                             << "\n  arc: " << fmtArc(query.arc)
                             << "\n  strudel version: " << nf.fixture.strudel_version_or_commit
                             << "\n  expected " << query.events.size() << " events, got "
                             << actualCount;
                        diag << "\n  expected events:";
                        for (const auto& fe : query.events)
                            diag << "\n    " << fmtEvent(fe);
                        diag << "\n  actual events:";
                        for (std::size_t i = 0; i < actualCount; ++i)
                            diag << "\n    " << fmtEvent(buf[i]);
                        FAIL(diag.str());
                    }

                    // Compare each event exactly.
                    for (std::size_t i = 0; i < actualCount; ++i) {
                        const auto& actual = buf[i];
                        const auto& expected = query.events[i];

                        std::ostringstream diag;
                        diag << "event mismatch in fixture '" << nf.filename << "'"
                             << "\n  arc: " << fmtArc(query.arc)
                             << "\n  strudel version: " << nf.fixture.strudel_version_or_commit
                             << "\n  event index: " << i;

                        bool mismatch = false;

                        if (actual.whole.start != expected.whole.start) {
                            diag << "\n  whole.start: expected " << expected.whole.start
                                 << ", got " << actual.whole.start;
                            mismatch = true;
                        }
                        if (actual.whole.end != expected.whole.end) {
                            diag << "\n  whole.end: expected " << expected.whole.end
                                 << ", got " << actual.whole.end;
                            mismatch = true;
                        }
                        if (actual.active.start != expected.active.start) {
                            diag << "\n  active.start: expected " << expected.active.start
                                 << ", got " << actual.active.start;
                            mismatch = true;
                        }
                        if (actual.active.end != expected.active.end) {
                            diag << "\n  active.end: expected " << expected.active.end
                                 << ", got " << actual.active.end;
                            mismatch = true;
                        }
                        if (actual.value != expected.value) {
                            diag << "\n  value: expected '" << expected.value
                                 << "', got '" << actual.value << "'";
                            mismatch = true;
                        }

                        // Full event listing for context.
                        diag << "\n  expected:";
                        for (const auto& fe : query.events)
                            diag << "\n    " << fmtEvent(fe);
                        diag << "\n  actual:";
                        for (std::size_t j = 0; j < actualCount; ++j)
                            diag << "\n    " << fmtEvent(buf[j]);

                        if (mismatch)
                            FAIL(diag.str());
                    }

                    // Sanity: all events must have non-empty values.
                    for (std::size_t i = 0; i < actualCount; ++i) {
                        if (buf[i].value.empty()) {
                            FAIL("empty value event in fixture '" + nf.filename + "'");
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE: degradeBy correlation — instance-a must equal instance-b
// ---------------------------------------------------------------------------
// Per Task 5.4 revision, the two independent-instance degradeBy fixtures are
// expected to MATCH (correlated).  We assert equality here as an explicit
// check, not just a side effect of the per-fixture comparison.
// ---------------------------------------------------------------------------

TEST_CASE("degradeBy: instance-a and instance-b are correlated (identical)", "[strudel][degradeBy][correlation]")
{
    const auto fixtures = loadAllFixtures(goldenDir().string());

    const Fixture* fixtureA = nullptr;
    const Fixture* fixtureB = nullptr;
    for (const auto& nf : fixtures) {
        if (nf.filename == "degrade-by-0.5-instance-a")
            fixtureA = &nf.fixture;
        else if (nf.filename == "degrade-by-0.5-instance-b")
            fixtureB = &nf.fixture;
    }

    REQUIRE(fixtureA != nullptr);
    REQUIRE(fixtureB != nullptr);

    // Both fixtures should have exactly the same arcs and events.
    REQUIRE(fixtureA->queries.size() == fixtureB->queries.size());

    for (std::size_t qi = 0; qi < fixtureA->queries.size(); ++qi) {
        const auto& qa = fixtureA->queries[qi];
        const auto& qb = fixtureB->queries[qi];

        INFO("arc " << fmtArc(qa.arc));

        REQUIRE(qa.events.size() == qb.events.size());

        for (std::size_t ei = 0; ei < qa.events.size(); ++ei) {
            const auto& ea = qa.events[ei];
            const auto& eb = qb.events[ei];

            INFO("event " << ei);
            CHECK(ea.value == eb.value);
            CHECK(ea.whole.start == eb.whole.start);
            CHECK(ea.whole.end   == eb.whole.end);
            CHECK(ea.active.start == eb.active.start);
            CHECK(ea.active.end   == eb.active.end);
        }
    }
}
