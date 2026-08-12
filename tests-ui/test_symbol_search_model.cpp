// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_symbol_search_model.cpp — unit tests for SymbolSearchModel.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target.
 *
 * Requirement references: L-2 §3
 */

#include <catch2/catch_test_macros.hpp>

#include "SymbolSearchModel.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace hathor::ui;

// ===========================================================================
// With metadata
// ===========================================================================

TEST_CASE("SymbolSearchModel: with metadata finds functions", "[symbol_search]")
{
    hathor::language::LanguageMetadata metadata;
    metadata.schemaVersion = 1;
    metadata.hathorEngineCompat = "0.1.0";
    metadata.strudelMiniNotationCompat = "1.2.6";

    hathor::language::MiniNotationFunction fastFn;
    fastFn.name = "fast";
    fastFn.signature = "fast(multiplier: number)";
    fastFn.description = "Speed up pattern";
    fastFn.category = "pattern";
    fastFn.supported = true;
    metadata.functions.push_back(fastFn);

    hathor::language::MiniNotationFunction slowFn;
    slowFn.name = "slow";
    slowFn.signature = "slow(divisor: number)";
    slowFn.description = "Slow down pattern";
    slowFn.category = "pattern";
    slowFn.supported = true;
    metadata.functions.push_back(slowFn);

    SymbolSearchModel model(&metadata);
    model.searchMetadata("fast");

    const auto& results = model.results();
    REQUIRE_FALSE(results.empty());

    bool foundFast = false;
    bool foundSlow = false;
    for (const auto& r : results)
    {
        if (r.name == "fast")
        {
            foundFast = true;
            REQUIRE(r.kind == "function");
            REQUIRE(r.detail == "fast(multiplier: number)");
            REQUIRE(r.isBuiltin == true);
        }
        if (r.name == "slow")
            foundSlow = true;
    }
    REQUIRE(foundFast);
    REQUIRE_FALSE(foundSlow); // "slow" doesn't contain "fast"
}

TEST_CASE("SymbolSearchModel: with metadata finds samples", "[symbol_search]")
{
    hathor::language::LanguageMetadata metadata;
    metadata.functions.push_back({});

    hathor::language::SampleDefinition bd;
    bd.name = "bd";
    bd.description = "Bass drum";
    bd.category = "drum";
    metadata.samples.push_back(bd);

    hathor::language::SampleDefinition sn;
    sn.name = "sn";
    sn.description = "Snare";
    sn.category = "snare";
    metadata.samples.push_back(sn);

    SymbolSearchModel model(&metadata);
    model.searchMetadata("bd");

    const auto& results = model.results();
    REQUIRE_FALSE(results.empty());
    REQUIRE(results.front().name == "bd");
    REQUIRE(results.front().kind == "sample");
}

TEST_CASE("SymbolSearchModel: without metadata returns empty", "[symbol_search]")
{
    SymbolSearchModel model(nullptr);
    model.searchMetadata("anything");

    REQUIRE(model.results().empty());
}

TEST_CASE("SymbolSearchModel: empty query returns empty", "[symbol_search]")
{
    hathor::language::LanguageMetadata metadata;
    metadata.functions.push_back({});

    SymbolSearchModel model(&metadata);
    model.searchMetadata("");

    REQUIRE(model.results().empty());
}

TEST_CASE("SymbolSearchModel: clear empties results", "[symbol_search]")
{
    hathor::language::LanguageMetadata metadata;

    hathor::language::MiniNotationFunction fn;
    fn.name = "anything";
    metadata.functions.push_back(fn);

    SymbolSearchModel model(&metadata);
    model.searchMetadata("anything");
    REQUIRE_FALSE(model.results().empty());

    model.clear();
    REQUIRE(model.results().empty());
}

TEST_CASE("SymbolSearchModel: searchWorkspaceFiles finds function patterns", "[symbol_search]")
{
    auto tmpDir = std::filesystem::temp_directory_path() / "hathor-sym-test";
    std::filesystem::create_directories(tmpDir);

    {
        std::ofstream f(tmpDir / "test.hathor");
        f << "s(\"bd\")\nstack(s(\"sn\"), s(\"hh\"))\n";
    }

    {
        hathor::language::LanguageMetadata metadata;
        SymbolSearchModel model(&metadata);
        model.searchWorkspaceFiles(tmpDir, "stack");

        const auto& results = model.results();
        REQUIRE_FALSE(results.empty());
        REQUIRE(results.front().name == "stack");
        REQUIRE(results.front().kind == "function");
    }

    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
}
