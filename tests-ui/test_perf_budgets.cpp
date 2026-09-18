// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_perf_budgets.cpp — executable performance budgets (P7.3).
 *
 * These benchmarks pin the numbers recorded in docs/PERF-BUDGETS.md. They
 * run headless (no JUCE GUI): the tokenise benchmark drives the same
 * JUCE-free hathor::tokenise() the production MiniNotationTokeniser uses,
 * and the search benchmark drives the production WorkspaceSearchModel over
 * a generated fixture tree.
 *
 * Budgets asserted here (Debug build; Release is several times faster):
 *   - tokenise 10k-line doc: < 500 ms (measured ~17 ms)
 *   - workspace search, 200 files: < 2000 ms (measured ~40 ms)
 *
 * Run: ./hathor-ui-tests "[perf]"  (or with --benchmark-samples for tighter stats)
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>

#include "hathor/MiniTokeniser.hpp"
#include "WorkspaceSearchModel.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string makeTenKLineDoc()
{
    // Representative mini-notation: slots, euclidean rhythms, samples,
    // effects, comments, and blank separator lines.
    static const char* kLines[] = {
        "[hathor]", "slot = d0", "bpm = 120.0", "label = perf",
        "", "s \"bd sn hh cp\"",
        "d1 $ s \"bd*4\" # gain 0.9 # lpf 800",
        "d2 $ n \"0 2 4 7\" # s \"superpiano\" |*| gain \"0.8 0.6\"",
        "d3 $ s \"house:0 house:1 house:2 house:3\" # speed 1.5",
        "hush", "// a comment line for the highlighter",
        "p \"a b c\" # begin # end",
    };
    constexpr int kNumLines = sizeof(kLines) / sizeof(kLines[0]);
    std::string doc;
    doc.reserve(10 * 1024 * 64);
    for (int i = 0; i < 10000; ++i)
    {
        doc += kLines[i % kNumLines];
        doc += '\n';
    }
    return doc;
}

std::filesystem::path makeSearchFixture()
{
    const auto root = std::filesystem::temp_directory_path()
                      / "hathor-perf-search-fixture";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    for (int d = 0; d < 10; ++d)
    {
        const auto dir = root / ("folder" + std::to_string(d));
        std::filesystem::create_directories(dir, ec);
        for (int f = 0; f < 20; ++f)
        {
            std::ofstream out(dir / ("song" + std::to_string(f) + ".hathor"));
            out << "[hathor]\nslot = d" << (f % 8) << "\nlabel = bench\n\n";
            for (int l = 0; l < 50; ++l)
                out << "s \"bd sn hh cp\" # gain 0.9\n";
            out << "// needle_marker_xyz\n";
        }
    }
    return root;
}

} // namespace

using namespace hathor::ui;

TEST_CASE("perf: tokenise 10k-line mini-notation doc", "[perf]")
{
    const std::string doc = makeTenKLineDoc();
    REQUIRE(doc.size() > 100000);

    const auto t0 = std::chrono::steady_clock::now();
    auto tokens = hathor::tokenise(doc);
    const auto t1 = std::chrono::steady_clock::now();

    REQUIRE(!tokens.empty());
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    INFO("tokenise 10k lines: " << ms << " ms, " << tokens.size() << " tokens");
    REQUIRE(ms < 500);
}

TEST_CASE("perf: workspace search over 200-file fixture", "[perf]")
{
    const auto root = makeSearchFixture();
    WorkspaceSearchModel model(root);
    WorkspaceSearchFlags flags;

    const auto t0 = std::chrono::steady_clock::now();
    const int total = model.search("needle_marker_xyz", flags);
    const auto t1 = std::chrono::steady_clock::now();

    REQUIRE(total == 200);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    INFO("search 200 files: " << ms << " ms, " << total << " matches");
    REQUIRE(ms < 2000);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
