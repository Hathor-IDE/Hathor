// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_hathor_file_parser.cpp — Property test P2 for HathorFileParser.
 *
 * P2: Round-trip consistency (Req 24.9) — serialise → parse → serialise
 *     produces a byte-identical string for any valid HathorFile.
 *
 * Invariant (Req 24.9 / Phase 2 tasks.md §2.2):
 *   For any valid HathorFile document, the round-trip property SHALL hold:
 *   parse the file into a (FrontMatter, body) pair, serialise back to a
 *   string (with bpm formatted as one decimal digit), re-parse — the slot,
 *   bpm, bank, label, color, and body fields SHALL be byte-for-byte identical
 *   to those of the original parse result.
 *
 * This test verifies the stronger string-level invariant:
 *   serialise(parse(serialise(hf))) == serialise(hf)
 * which implies all fields are preserved, because serialiseHathorFile always
 * emits a canonical form (fields in declaration order, bpm to 1 decimal).
 *
 * Requirements: 24.2, 24.3, 24.7, 24.8, 24.9
 */

#include <catch2/catch_test_macros.hpp>

#include "HathorFileParser.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <variant>

using hathor::ui::FrontMatter;
using hathor::ui::HathorFile;
using hathor::ui::ParseFileError;
using hathor::ui::parseHathorFile;
using hathor::ui::serialiseHathorFile;

// ---------------------------------------------------------------------------
// Deterministic RNG — seeded for reproducible property-based test cases.
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint64_t kP2Seed = 0xDEADBEEFCAFE;

// Generate a random integer in [lo, hi].
int rngInt(std::mt19937_64& rng, int lo, int hi)
{
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng);
}

// Generate a random string of n chars from the given charset.
std::string rngString(std::mt19937_64& rng, int n, std::string_view charset)
{
    if (charset.empty() || n <= 0) return std::string{};
    std::string s;
    s.reserve(static_cast<std::size_t>(n));
    const int maxIdx = static_cast<int>(charset.size()) - 1;
    for (int i = 0; i < n; ++i)
        s += charset[static_cast<std::size_t>(rngInt(rng, 0, maxIdx))];
    return s;
}

/// Trim leading and trailing ASCII whitespace (matching the parser's trim).
/// The parser strips whitespace from front-matter field values, so the
/// generator must produce values that are already trimmed to ensure
/// serialise→parse→serialise stability.
std::string trim(std::string s)
{
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && isSpace(s.front())) s.erase(s.begin());
    while (!s.empty() && isSpace(s.back()))  s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
// Generators for valid front-matter field values
// ---------------------------------------------------------------------------

/// Generate a slot identifier matching [a-z][0-9]{0,2} (e.g. "d1", "h12", "a").
std::string genSlot(std::mt19937_64& rng)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    char c = letters[static_cast<std::size_t>(rngInt(rng, 0, 25))];
    std::string s(1, c);
    const int ndigits = rngInt(rng, 0, 2);
    for (int i = 0; i < ndigits; ++i)
        s += static_cast<char>('0' + rngInt(rng, 0, 9));
    return s;
}

/// Generate a bpm value valid for the format: float in [20.0, 400.0].
/// The serialiser formats bpm to exactly one decimal place (e.g. 120.0, 93.5).
/// We generate values that are representable with one decimal digit so the
/// round-trip through parse→serialise is exact.
double genBpm(std::mt19937_64& rng)
{
    // Generate in tenths: 20.0 → 400.0 in 0.1 steps.
    const int tenths = rngInt(rng, 200, 4000);
    return static_cast<double>(tenths) / 10.0;
}

/// Generate a bank path (printable ASCII, no newlines).
std::string genBank(std::mt19937_64& rng)
{
    static constexpr std::string_view charset =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_-.";
    const int len = rngInt(rng, 1, 40);
    return trim(rngString(rng, len, charset));
}

/// Generate a label (≤ 64 chars, printable ASCII, no newlines).
/// Values are trimmed to match the parser's trimming behaviour (Req 24.3
/// rule 3: key=value values are trimmed).
std::string genLabel(std::mt19937_64& rng)
{
    static constexpr std::string_view charset =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_";
    const int len = rngInt(rng, 1, 64);
    std::string s = trim(rngString(rng, len, charset));
    // Guard against all-whitespace results (trim can make a string empty).
    if (s.empty()) s = "label";
    return s;
}

/// Generate a 7-character CSS hex colour (e.g. "#e05a5a").
std::string genColor(std::mt19937_64& rng)
{
    static const char hex[] = "0123456789abcdef";
    std::string s = "#";
    for (int i = 0; i < 6; ++i)
        s += hex[static_cast<std::size_t>(rngInt(rng, 0, 15))];
    return s;
}

/// Generate a random body string (mini-notation-like text).
/// May contain newlines to exercise multi-line body handling.
std::string genBody(std::mt19937_64& rng)
{
    static const std::string_view atoms =
        "bd sn hh cp kick rim cl oh ch x o t l r";
    static constexpr std::string_view specials = "[]<>*/!~(),.:";

    const int nTokens = rngInt(rng, 0, 24);
    std::string body;
    body.reserve(static_cast<std::size_t>(nTokens * 5));

    for (int i = 0; i < nTokens; ++i) {
        const int choice = rngInt(rng, 0, 99);
        if (choice < 35) {
            // Atom — pick a random drum name from the set.
            const int idx = rngInt(rng, 0, 18);
            const auto& a = std::string(atoms);
            body += a[static_cast<std::size_t>(idx * 2 + (idx > 0))];
            // Add second char for 2-letter names
            if (rngInt(rng, 0, 1) == 1 && idx < 19) {
                const char second = static_cast<char>(
                    "bd snbdfhhcpkic r o tx l r"[idx * 2]);
                body += second;
            }
        } else if (choice < 45) {
            // Single digit number
            body += static_cast<char>('0' + rngInt(rng, 0, 9));
        } else if (choice < 75) {
            // Special character (single-char token)
            body += specials[static_cast<std::size_t>(rngInt(rng, 0, 11))];
        } else if (choice < 90) {
            // Whitespace (space)
            body += ' ';
        } else {
            // Newline (exercises multi-line body)
            body += '\n';
        }
    }

    // Ensure non-empty body.
    if (body.empty())
        body = "bd sn";

    return body;
}

/// Generate a random FrontMatter.  Each field is independently present
/// with ~50% probability.  At least one field is guaranteed present so the
/// [hathor] block is emitted.
FrontMatter genFrontMatter(std::mt19937_64& rng)
{
    FrontMatter fm;
    bool any = false;

    if (rngInt(rng, 0, 1) == 1) { fm.slot = genSlot(rng); any = true; }
    if (rngInt(rng, 0, 1) == 1) { fm.bpm = genBpm(rng); any = true; }
    if (rngInt(rng, 0, 1) == 1) { fm.bank = genBank(rng); any = true; }
    if (rngInt(rng, 0, 1) == 1) { fm.label = genLabel(rng); any = true; }
    if (rngInt(rng, 0, 1) == 1) { fm.color = genColor(rng); any = true; }

    // Ensure at least one field is present.
    if (!any)
        fm.slot = genSlot(rng);

    return fm;
}

/// Generate a random HathorFile.  Randomly decides whether to include
/// front-matter (50% probability).  When no front-matter is present, the
/// body is ensured not to start with "[hathor]" to avoid ambiguity with
/// the parser's front-matter detection.
HathorFile genHathorFile(std::mt19937_64& rng)
{
    HathorFile hf;

    const bool hasFrontMatter = rngInt(rng, 0, 1) == 1;
    if (hasFrontMatter)
        hf.front = genFrontMatter(rng);

    hf.body = genBody(rng);

    // If no front matter, ensure body doesn't start with "[hathor]"
    // (the parser would interpret it as a front-matter header).
    if (!hasFrontMatter) {
        // Find first non-empty/non-whitespace character.
        std::string_view sv = hf.body;
        while (!sv.empty() && (sv[0] == ' ' || sv[0] == '\t' || sv[0] == '\n' || sv[0] == '\r'))
            sv.remove_prefix(1);
        if (sv.starts_with("[hathor]"))
            hf.body = "bd " + hf.body;  // prepend a safe atom
    }

    return hf;
}

} // namespace

// ---------------------------------------------------------------------------
// P2: serialise → parse → serialise round-trip
//
// Invariant:  serialise(parse(serialise(hf))) == serialise(hf)
//
// The serialiser always emits a canonical form (fields in declaration order,
// bpm to one decimal place, blank-line separator).  The parser preserves the
// body byte-for-byte.  Thus two serialisations of the same parsed result
// must be byte-identical.
// ---------------------------------------------------------------------------

TEST_CASE("P2: HathorFile serialise → parse → serialise round-trip",
          "[hathor-file][p2]")
{
    std::mt19937_64 rng(kP2Seed);

    constexpr int kNumCases = 200;

    for (int iter = 0; iter < kNumCases; ++iter) {
        const HathorFile hf = genHathorFile(rng);

        // Step 1: serialise the generated HathorFile.
        const std::string s1 = serialiseHathorFile(hf);

        // Step 2: parse the serialised string back.
        const auto parsed = parseHathorFile(s1);
        INFO("iter " << iter << "\ninput:\n" << s1);
        REQUIRE(std::holds_alternative<HathorFile>(parsed));

        const auto& hf2 = std::get<HathorFile>(parsed);

        // Step 3: re-serialise the parsed result.
        const std::string s2 = serialiseHathorFile(hf2);

        // Step 4: byte-for-byte equality of the two serialisations.
        INFO("iter " << iter << "\n"
             << "first serialise:\n" << s1 << "\n"
             << "second serialise:\n" << s2);
        REQUIRE(s1 == s2);
    }
}

// ---------------------------------------------------------------------------
// P2b: field-level equality after round-trip
//
// The round-trip property also guarantees that individual fields survive
// parse → serialise → parse.  This test checks field-by-field equality
// for richer diagnostics than string comparison alone.
// ---------------------------------------------------------------------------

TEST_CASE("P2b: HathorFile round-trip preserves all fields",
          "[hathor-file][p2]")
{
    std::mt19937_64 rng(kP2Seed + 1);

    constexpr int kNumCases = 200;

    for (int iter = 0; iter < kNumCases; ++iter) {
        const HathorFile hf = genHathorFile(rng);
        const std::string serialised = serialiseHathorFile(hf);

        INFO("iter " << iter << "\nserialised:\n" << serialised);

        const auto parsed = parseHathorFile(serialised);
        REQUIRE(std::holds_alternative<HathorFile>(parsed));
        const auto& hf2 = std::get<hathor::ui::HathorFile>(parsed);

        // slot
        if (hf.front.slot) {
            INFO("slot mismatch");
            REQUIRE(hf2.front.slot.has_value());
            REQUIRE(*hf2.front.slot == *hf.front.slot);
        } else {
            REQUIRE_FALSE(hf2.front.slot.has_value());
        }

        // bpm (within 1e-6 tolerance — serialiser formats to 1 decimal, parser
        // reads back exactly)
        if (hf.front.bpm) {
            INFO("bpm mismatch");
            REQUIRE(hf2.front.bpm.has_value());
            REQUIRE(std::abs(*hf2.front.bpm - *hf.front.bpm) < 1e-6);
        } else {
            REQUIRE_FALSE(hf2.front.bpm.has_value());
        }

        // bank
        if (hf.front.bank) {
            INFO("bank mismatch");
            REQUIRE(hf2.front.bank.has_value());
            REQUIRE(*hf2.front.bank == *hf.front.bank);
        } else {
            REQUIRE_FALSE(hf2.front.bank.has_value());
        }

        // label
        if (hf.front.label) {
            INFO("label mismatch");
            REQUIRE(hf2.front.label.has_value());
            REQUIRE(*hf2.front.label == *hf.front.label);
        } else {
            REQUIRE_FALSE(hf2.front.label.has_value());
        }

        // color
        if (hf.front.color) {
            INFO("color mismatch");
            REQUIRE(hf2.front.color.has_value());
            REQUIRE(*hf2.front.color == *hf.front.color);
        } else {
            REQUIRE_FALSE(hf2.front.color.has_value());
        }

        // body
        INFO("body mismatch");
        REQUIRE(hf2.body == hf.body);
    }
}

// ---------------------------------------------------------------------------
// P2c: edge cases — no front matter, empty body, all fields present
// ---------------------------------------------------------------------------

TEST_CASE("P2c: HathorFile edge cases round-trip",
          "[hathor-file][p2]")
{
    // No front matter — body only.
    {
        HathorFile hf;
        hf.body = "bd sn hh cp";
        const std::string s1 = serialiseHathorFile(hf);
        REQUIRE(s1 == "bd sn hh cp");

        const auto parsed = parseHathorFile(s1);
        REQUIRE(std::holds_alternative<HathorFile>(parsed));
        const auto& hf2 = std::get<HathorFile>(parsed);
        REQUIRE(serialiseHathorFile(hf2) == s1);
    }

    // Empty body with front matter.
    {
        HathorFile hf;
        hf.front.slot = "d1";
        hf.front.bpm = 120.0;
        hf.body = "";
        const std::string s1 = serialiseHathorFile(hf);
        const auto parsed = parseHathorFile(s1);
        REQUIRE(std::holds_alternative<HathorFile>(parsed));
        const auto& hf2 = std::get<HathorFile>(parsed);
        REQUIRE(serialiseHathorFile(hf2) == s1);
        REQUIRE(hf2.body == "");
    }

    // All fields present.
    {
        HathorFile hf;
        hf.front.slot = "a1";
        hf.front.bpm = 93.5;
        hf.front.bank = "samples/bd";
        hf.front.label = "My Groove";
        hf.front.color = "#e05a5a";
        hf.body = "bd sn [hh hh] cp";
        const std::string s1 = serialiseHathorFile(hf);
        const auto parsed = parseHathorFile(s1);
        REQUIRE(std::holds_alternative<HathorFile>(parsed));
        const auto& hf2 = std::get<HathorFile>(parsed);
        REQUIRE(serialiseHathorFile(hf2) == s1);
    }

    // BPM formatting: 120.0 and 93.5 format to one decimal.
    {
        HathorFile hf;
        hf.front.bpm = 120.0;
        hf.body = "bd";
        const std::string s1 = serialiseHathorFile(hf);
        REQUIRE(s1.find("bpm = 120.0") != std::string::npos);

        const auto parsed = parseHathorFile(s1);
        REQUIRE(std::holds_alternative<HathorFile>(parsed));
        const auto& hf2 = std::get<HathorFile>(parsed);
        REQUIRE(serialiseHathorFile(hf2) == s1);
    }
}
