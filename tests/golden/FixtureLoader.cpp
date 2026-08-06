// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * FixtureLoader.cpp — implementation for Strudel golden-fixture JSON parsing.
 *
 * Requirement references: 7.1
 */

#include "FixtureLoader.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hathor::test {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Convert a [numerator, denominator] JSON array into a Rational.
/// The JSON value must be an array of exactly 2 integers.
static Rational jsonToRational(const json& j)
{
    if (!j.is_array() || j.size() != 2)
        throw std::runtime_error("expected [numerator, denominator] pair");

    int64_t num = j[0].get<int64_t>();
    int64_t den = j[1].get<int64_t>();
    return Rational{num, den};
}

/// Convert a [[n,d],[n,d]] JSON array into an Arc.
static Arc jsonToArc(const json& j)
{
    if (!j.is_array() || j.size() != 2)
        throw std::runtime_error("expected [[startN, startD], [endN, endD]] for arc");
    return Arc{jsonToRational(j[0]), jsonToRational(j[1])};
}

// ---------------------------------------------------------------------------
// loadFixture
// ---------------------------------------------------------------------------

Fixture loadFixture(const std::string& filePath)
{
    std::ifstream ifs(filePath);
    if (!ifs.is_open())
        throw std::runtime_error("cannot open fixture file: " + filePath);

    std::stringstream ss;
    ss << ifs.rdbuf();
    const std::string contents = ss.str();

    json doc;
    try {
        doc = json::parse(contents);
    } catch (const json::parse_error& e) {
        throw std::runtime_error("JSON parse error in " + filePath + ": " + e.what());
    }

    Fixture fixture;
    fixture.description = doc.value("description", std::string{});
    fixture.strudel_version_or_commit =
        doc.value("strudel_version_or_commit", std::string{});

    // mini_notation: null → nullopt, present → the string
    if (doc.contains("mini_notation") && !doc["mini_notation"].is_null())
        fixture.mini_notation = doc["mini_notation"].get<std::string>();
    // else: mini_notation is null or missing → nullopt

    // Parse queries[]
    const json& queries = doc.value("queries", json::array());
    fixture.queries.reserve(queries.size());

    for (const auto& q : queries) {
        FixtureQuery fq;

        // arc
        if (q.contains("arc"))
            fq.arc = jsonToArc(q["arc"]);

        // error (may be null)
        if (q.contains("error") && !q["error"].is_null())
            fq.error = q["error"].value("message", std::string{});
        // else: error is null → fq.error stays empty ("")

        // events[]
        const json& events = q.value("events", json::array());
        fq.events.reserve(events.size());

        for (const auto& e : events) {
            FixtureEvent fe;
            fe.whole  = jsonToArc(e["whole"]);
            fe.active = jsonToArc(e["active"]);
            fe.value  = e.value("value", std::string{});
            fq.events.push_back(std::move(fe));
        }

        fixture.queries.push_back(std::move(fq));
    }

    return fixture;
}

// ---------------------------------------------------------------------------
// loadAllFixtures
// ---------------------------------------------------------------------------

std::vector<NamedFixture> loadAllFixtures(const std::string& dirPath)
{
    // Collect all .json files (skip README.md).
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
            files.push_back(entry.path());
    }

    // Sort by filename for deterministic ordering.
    std::sort(files.begin(), files.end(),
              [](const fs::path& a, const fs::path& b) {
                  return a.filename().string() < b.filename().string();
              });

    std::vector<NamedFixture> result;
    result.reserve(files.size());

    for (const auto& fpath : files) {
        NamedFixture nf;
        // filename without extension
        nf.filename = fpath.stem().string();
        nf.fixture = loadFixture(fpath.string());
        result.push_back(std::move(nf));
    }

    return result;
}

// ---------------------------------------------------------------------------
// expectedFixtureNames
// ---------------------------------------------------------------------------

std::vector<std::string> expectedFixtureNames()
{
    // Master list of all fixture filenames (without .json extension).
    // Adding a new fixture file to reference/strudel-golden/ requires adding
    // its name here — the startup check in test_strudel_differential.cpp
    // verifies this list stays in sync with the actual directory contents.
    return {
        "degrade-by-0.0",
        "degrade-by-0.5-instance-a",
        "degrade-by-0.5-instance-b",
        "degrade-by-1.0",
        "euclid-3-8",
        "euclid-5-8",
        "euclid-7-16-2",
        "every-3-rev-bd-sn",
        "fast-bd-star-2",
        "fast-bd-star-4",
        "fast-seq-bd-sn-star-2",
        "fastcat-bd-sn",
        "fastcat-bd-sn-hh-cp",
        "iter-4-bd-sn-hh-cp",
        "nested-subsequence-angle",
        "nested-subsequence-bracket",
        "replicate-bd-3-sn",
        "rest-bd-sn",
        "rev-bd-sn-hh-cp",
        "slow-bd-div-2",
        "slow-seq-bd-sn-div-2",
        "slowcat-bd-sn-hh",
        "stack-bd-sn-api",
        "stack-bd-sn-mini",
    };
}

} // namespace hathor::test
