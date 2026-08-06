// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * FixtureLoader.hpp — loader for Strudel golden-fixture JSON files.
 *
 * Parses the JSON schema used by the golden fixture files (.json):
 *
 *   {
 *     "description": "...",
 *     "mini_notation": "bd(3,8)" | null,
 *     "strudel_version_or_commit": "1.2.6",
 *     "arcs_used": [[[n,d],[n,d]], ...],   // (informational, not parsed here)
 *     "queries": [
 *       {
 *         "arc": [[n,d],[n,d]],
 *         "events": [
 *           {
 *             "whole":  [[n,d],[n,d]],
 *             "active": [[n,d],[n,d]],
 *             "value":  "bd"
 *           }, ...
 *         ],
 *         "error": null
 *       }
 *     ],
 *     "precision_note": "..."
 *   }
 *
 * `whole`/`active`/`arc` values are [numerator, denominator] pairs — loaded
 * directly into Rational with no float conversion.
 *
 * Uses nlohmann/json (available as a CMake dependency).  No JUCE dependency.
 *
 * Requirement references: 7.1
 */

#ifndef HATHOR_TEST_FIXTURE_LOADER_HPP
#define HATHOR_TEST_FIXTURE_LOADER_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <utility>

#include "hathor/Rational.hpp"
#include "hathor/Arc.hpp"

namespace hathor::test {

// ---------------------------------------------------------------------------
// Data structures mirroring the JSON schema
// ---------------------------------------------------------------------------

/// A single event as recorded in the fixture.
struct FixtureEvent {
    Arc        whole    = {Rational{0}, Rational{0}};  ///< full logical arc of the event
    Arc        active   = {Rational{0}, Rational{0}};  ///< arc clipped to the query window
    std::string value   = "";                         ///< event payload (e.g. "bd", "~")
};

/// One query entry: an arc to query and the expected events.
struct FixtureQuery {
    Arc                          arc    = {Rational{0}, Rational{0}};  ///< [start, end) to query
    std::vector<FixtureEvent>    events;                                ///< expected events
    std::string                  error;                                  ///< empty if no error, otherwise the error message
};

/// A complete fixture loaded from a single JSON file.
struct Fixture {
    std::string                 description;
    std::optional<std::string>  mini_notation;  ///< nullopt when mini_notation was null in JSON
    std::string                 strudel_version_or_commit;
    std::vector<FixtureQuery>   queries;
};

/// A fixture paired with its filename (without extension) for identification.
struct NamedFixture {
    std::string filename;  ///< e.g. "degrade-by-0.0"
    Fixture     fixture;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Load and parse a single fixture JSON file.
 *
 * @param filePath  Path to the .json file.
 * @returns         The parsed fixture.
 * @throws std::runtime_error if the file cannot be opened or parsed.
 *
 * Requirement: 7.1
 */
Fixture loadFixture(const std::string& filePath);

/**
 * Load all *.json fixture files from a directory, sorted by filename.
 *
 * @param dirPath  Path to the golden-fixture directory.
 * @returns        Vector of named fixtures in sorted filename order.
 * @throws std::runtime_error if the directory cannot be opened.
 *
 * Requirement: 7.1
 */
std::vector<NamedFixture> loadAllFixtures(const std::string& dirPath);

/**
 * Convenience: return the list of expected fixture filenames (without .json)
 * that must be present in the golden directory.  Used by the startup check
 * in test_strudel_differential.cpp to ensure every fixture file is covered
 * by a test case.
 *
 * Requirement: 7.2
 */
std::vector<std::string> expectedFixtureNames();

} // namespace hathor::test

#endif // HATHOR_TEST_FIXTURE_LOADER_HPP
