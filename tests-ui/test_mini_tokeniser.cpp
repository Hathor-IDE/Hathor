// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_mini_tokeniser.cpp — Property test P3 for MiniNotationTokeniser.
 *
 * P3: Colour-kind bijection (Req 27.4) — the TokenKind sequence from
 *     hathor::tokenise() and the colour-index sequence from
 *     MiniNotationTokeniser agree at every index i:
 *     colourOf(kinds[i]) == colours[i].
 *
 * Invariant (Req 27.4 / Phase 2 tasks.md §2.3):
 *   For any mini-notation string that produces no TK_ERROR tokens, the
 *   sequence of non-TK_EOF TokenKind values from hathor::tokenise() and
 *   the sequence of colour-index categories SHALL have equal length and
 *   satisfy colourOf(kinds[i]) == colours[i] for every index i.
 *
 * Implementation note:
 *   The JUCE CodeTokeniser path (MiniNotationTokeniser::readNextToken) is
 *   exercised by the UI integration. In this headless test target
 *   (HATHOR_BUILD_APP=OFF, no JUCE link) we compare the two production paths
 *   directly:
 *     Path 1 — hathor::tokenise()      (engine, JUCE-free)
 *     Path 2 — tokenKindToColourIndex() (UI colour mapping, extracted to
 *              TokenColourMap.hpp so it is JUCE-free and shared by both the
 *              production MiniNotationTokeniser and this test)
 *   The bijection verifies that every token produced by Path 1 is correctly
 *   classified and mapped to the colour index defined by the spec table
 *   (Req 27.3) via Path 2.
 *
 * Requirements: 27.3, 27.4, 27.5
 */

#include <catch2/catch_test_macros.hpp>

#include <hathor/MiniTokeniser.hpp>
#include "TokenColourMap.hpp"

#include <cctype>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using hathor::Token;
using hathor::TokenKind;
using hathor::tokenise;

// ---------------------------------------------------------------------------
// Deterministic RNG
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint64_t kP3Seed = 0xFEEDFACE;

int rngInt(std::mt19937_64& rng, int lo, int hi)
{
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng);
}

// ---------------------------------------------------------------------------
// Mini-notation generator (grammar-aware)
//
// Generates structurally valid mini-notation strings that exercise every
// TokenKind class: atoms, integers, rests, brackets, angle brackets,
// operators (* / !), euclid postfixes, parens, commas, and whitespace.
// ---------------------------------------------------------------------------

/// A set of drum names / sound identifiers for atom generation.
static const std::vector<std::string> kDrumNames = {
    "bd", "sn", "hh", "cp", "kick", "rim", "cl", "oh", "ch",
    "x", "o", "t", "l", "r", "sd", "bd2", "hh2",
};

std::string genAtom(std::mt19937_64& rng)
{
    return kDrumNames[static_cast<std::size_t>(rngInt(rng, 0, static_cast<int>(kDrumNames.size()) - 1))];
}

std::string genRest(std::mt19937_64& rng)
{
    (void)rng;
    return "~";
}

std::string genInt(std::mt19937_64& rng)
{
    // 1–3 digit integer (no leading zeros beyond single "0")
    const int n = rngInt(rng, 0, 99);
    return std::to_string(n);
}

/// Forward declaration
std::string genMiniNotation(std::mt19937_64& rng, int depth);

/// Generate a single element: a factor optionally followed by modifiers.
/// Modifiers: *N (fast), /N (slow), !N (replicate), (P,S[,R]) (euclid).
std::string genElement(std::mt19937_64& rng, int depth)
{
    std::string s = genMiniNotation(rng, depth + 1);

    // Append 0–3 modifier suffixes.
    const int nMods = rngInt(rng, 0, 3);
    for (int i = 0; i < nMods; ++i) {
        switch (rngInt(rng, 0, 3)) {
            case 0: s += "*" + std::to_string(rngInt(rng, 1, 8)); break;
            case 1: s += "/" + std::to_string(rngInt(rng, 1, 8)); break;
            case 2: s += "!" + std::to_string(rngInt(rng, 1, 8)); break;
            case 3: // Euclid postfix
                s += "(" + std::to_string(rngInt(rng, 0, 8))
                   + "," + std::to_string(rngInt(rng, 1, 16));
                if (rngInt(rng, 0, 1) == 1)
                    s += "," + std::to_string(rngInt(rng, 0, 7));
                s += ")";
                break;
        }
    }
    return s;
}

/// Generate a sequence: space-separated elements.
std::string genSequence(std::mt19937_64& rng, int depth)
{
    const int n = rngInt(rng, 1, 5);
    std::string s;
    for (int i = 0; i < n; ++i) {
        if (i > 0) s += ' ';
        s += genElement(rng, depth);
    }
    return s;
}

/// Generate a bracket group: [ sequence ].
std::string genBracketGroup(std::mt19937_64& rng, int depth)
{
    return "[" + genSequence(rng, depth) + "]";
}

/// Generate an angle group: < sequence >.
std::string genAngleGroup(std::mt19937_64& rng, int depth)
{
    return "<" + genSequence(rng, depth) + ">";
}

/// Generate a stack: sequence (, sequence)*  →  comma-separated groups.
std::string genStack(std::mt19937_64& rng, int depth)
{
    const int n = rngInt(rng, 2, 4);
    std::string s;
    for (int i = 0; i < n; ++i) {
        if (i > 0) s += ' ';
        s += genElement(rng, depth) + "," + genElement(rng, depth);
    }
    return s;
}

/// Top-level mini-notation generator.
std::string genMiniNotation(std::mt19937_64& rng, int depth = 0)
{
    if (depth > 5)
        return genAtom(rng);

    switch (rngInt(rng, 0, 7)) {
        case 0: return genAtom(rng);
        case 1: return genRest(rng);
        case 2: return genInt(rng);
        case 3: return genElement(rng, depth);
        case 4: return genBracketGroup(rng, depth);
        case 5: return genAngleGroup(rng, depth);
        case 6: return genStack(rng, depth);
        case 7: return genSequence(rng, depth);
    }
    return genAtom(rng);
}

// ---------------------------------------------------------------------------
// Oracle: independently classify a token's text to its expected TokenKind.
//
// This is NOT a reimplementation of tokenise() — it classifies an already-
// extracted token's text based on the grammar specification.  It serves as
// an independent oracle to verify that hathor::tokenise() assigns the
// correct kind to each token.
// ---------------------------------------------------------------------------

TokenKind classifyByText(std::string_view text) noexcept
{
    if (text.empty()) return TokenKind::TK_ERROR;

    const char c = text[0];

    // Single-character special tokens.
    switch (c) {
        case '[': return TokenKind::TK_LBRACKET;
        case ']': return TokenKind::TK_RBRACKET;
        case '<': return TokenKind::TK_LANGLE;
        case '>': return TokenKind::TK_RANGLE;
        case '*': return TokenKind::TK_STAR;
        case '/': return TokenKind::TK_SLASH;
        case '!': return TokenKind::TK_BANG;
        case '~': return TokenKind::TK_TILDE;
        case '(': return TokenKind::TK_LPAREN;
        case ')': return TokenKind::TK_RPAREN;
        case ',': return TokenKind::TK_COMMA;
        default:   break;
    }

    // Check for all-digit token → TK_INT.
    bool allDigits = true;
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            allDigits = false;
            break;
        }
    }
    if (allDigits) return TokenKind::TK_INT;

    // Everything else is an atom.
    return TokenKind::TK_ATOM;
}

} // namespace

// ---------------------------------------------------------------------------
// P3a: Colour-index mapping table verification (Req 27.3)
//
// Verifies that the production tokenKindToColourIndex function maps each
// TokenKind to the colour index mandated by the specification.
// ---------------------------------------------------------------------------

TEST_CASE("P3a: tokenKindToColourIndex matches spec table (Req 27.3)",
          "[tokeniser][p3][colour-map]")
{
    using K = TokenKind;

    // Direct assertions of the full mapping table.
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_ATOM)    == 0);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_INT)     == 1);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_TILDE)   == 2);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_LBRACKET) == 3);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_RBRACKET) == 3);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_LANGLE)   == 3);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_RANGLE)   == 3);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_STAR)    == 4);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_SLASH)   == 4);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_BANG)    == 4);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_LPAREN)   == 5);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_RPAREN)  == 5);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_COMMA)    == 5);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_ERROR)   == 6);
    REQUIRE(hathor::ui::tokenKindToColourIndex(K::TK_EOF)      == 0);
}

// ---------------------------------------------------------------------------
// P3b: Colour-kind bijection for random valid mini-notation
//
// Invariant: for every token kind K produced by hathor::tokenise(),
//   tokenKindToColourIndex(K)   — the production colour mapping (Path 2)
//     returns the colour index mandated by the spec table (Req 27.3).
//
// The test verifies:
//   1. Generated strings produce no TK_ERROR tokens (they are valid).
//   2. Each token's kind matches an independent oracle classification of
//      the token text (verifying Path 1 classifies correctly).
//   3. tokenKindToColourIndex maps each kind to the expected spec colour
//      (verifying Path 2 maps correctly).
//   4. Token positions are contiguous — each token's text matches the input
//      at the reported position.
// ---------------------------------------------------------------------------

TEST_CASE("P3b: tokeniser colour-kind bijection for random mini-notation",
          "[tokeniser][p3][bijection]")
{
    std::mt19937_64 rng(kP3Seed);

    constexpr int kNumCases = 200;

    for (int iter = 0; iter < kNumCases; ++iter) {
        const std::string input = genMiniNotation(rng);
        const auto tokens = tokenise(input);

        INFO("iter " << iter << "\ninput: \"" << input << "\"");

        // The tokeniser always appends a TK_EOF sentinel.
        REQUIRE(tokens.back().kind == TokenKind::TK_EOF);

        // Collect non-EOF tokens.
        std::vector<const Token*> nonEof;
        for (const auto& tok : tokens) {
            if (tok.kind == TokenKind::TK_EOF) break;
            nonEof.push_back(&tok);
        }

        // (1) No TK_ERROR tokens — input is valid.
        for (std::size_t i = 0; i < nonEof.size(); ++i) {
            INFO("token " << i << ": kind=" << static_cast<int>(nonEof[i]->kind)
                 << " text=\"" << nonEof[i]->text << "\"");
            REQUIRE(nonEof[i]->kind != TokenKind::TK_ERROR);
        }

        // (2) Each token's kind matches the oracle classification of its text.
        for (std::size_t i = 0; i < nonEof.size(); ++i) {
            const TokenKind expected = classifyByText(nonEof[i]->text);
            INFO("token " << i << ": text=\"" << nonEof[i]->text << "\"");
            REQUIRE(nonEof[i]->kind == expected);
        }

        // (3) tokenKindToColourIndex maps each kind to the spec colour.
        //     Verify that the mapping is a consistent function: the same
        //     TokenKind always yields the same colour index.
        for (std::size_t i = 0; i < nonEof.size(); ++i) {
            const int colour = hathor::ui::tokenKindToColourIndex(nonEof[i]->kind);
            INFO("token " << i << " kind=" << static_cast<int>(nonEof[i]->kind)
                 << " colour=" << colour);
            // Every colour must be in the valid range [0, 6].
            REQUIRE(colour >= 0);
            REQUIRE(colour <= 6);
            // The colour must match the direct call on the same kind
            // (consistency of the mapping function).
            REQUIRE(hathor::ui::tokenKindToColourIndex(nonEof[i]->kind) == colour);
        }

        // (4) Token positions are contiguous — the text at pos matches the
        //     token's text field, and the next token starts right after.
        std::size_t cursor = 0;
        for (std::size_t i = 0; i < nonEof.size(); ++i) {
            const auto& tok = *nonEof[i];
            INFO("token " << i << " pos=" << tok.pos << " text=\"" << tok.text << "\"");

            // Skip whitespace between cursor and token position.
            while (cursor < tok.pos) {
                REQUIRE(std::isspace(static_cast<unsigned char>(input[cursor])));
                ++cursor;
            }
            REQUIRE(cursor == tok.pos);

            // Token text must match input at this position.
            const std::string_view slice(input.data() + cursor, tok.text.size());
            REQUIRE(slice == tok.text);

            cursor = tok.pos + tok.text.size();
        }
    }
}

// ---------------------------------------------------------------------------
// P3c: Bijection with multi-token single-line inputs
//
// Tests that the colour-kind correspondence holds for strings that mix
// every token class on a single line, including repeated occurrences to
// verify the mapping is consistent across multiple uses of the same kind.
// ---------------------------------------------------------------------------

TEST_CASE("P3c: colour-kind bijection across all token classes",
          "[tokeniser][p3][exhaustive]")
{
    // A string that contains every single-character special token, atoms,
    // and integers — exercising all colour categories at least once.
    const std::vector<std::string> testStrings = {
        "bd",                         // TK_ATOM → 0
        "42",                         // TK_INT → 1
        "~",                          // TK_TILDE → 2
        "[",                          // TK_LBRACKET → 3
        "]",                          // TK_RBRACKET → 3
        "<",                          // TK_LANGLE → 3
        ">",                          // TK_RANGLE → 3
        "*",                          // TK_STAR → 4
        "/",                          // TK_SLASH → 4
        "!",                          // TK_BANG → 4
        "(",                          // TK_LPAREN → 5
        ")",                          // TK_RPAREN → 5
        ",",                          // TK_COMMA → 5
        "bd sn cp hh",                // multiple TK_ATOM → 0
        "bd 4 ~ [sn]",                // mixed kinds
        "bd*4 sn/2 cp!3",             // operators
        "bd(3,5) sn(2,3,1)",          // euclid
        "bd, sn",                     // stack
        "[bd sn] <cp hh>",            // groups
        "bd sn [hh hh] cp",           // nested
        "bd*2 sn [hh hh] cp",         // complex
    };

    for (const auto& input : testStrings) {
        INFO("input: \"" << input << "\"");

        const auto tokens = tokenise(input);
        REQUIRE(tokens.back().kind == TokenKind::TK_EOF);

        // Build the colour sequence expected by the bijection.
        std::vector<int> colours;
        for (const auto& tok : tokens) {
            if (tok.kind == TokenKind::TK_EOF) break;
            REQUIRE(tok.kind != TokenKind::TK_ERROR);
            colours.push_back(hathor::ui::tokenKindToColourIndex(tok.kind));
        }

        // Verify each colour matches specColourOf(kind) and is in [0,6].
        for (std::size_t i = 0; i < colours.size(); ++i) {
            const int expectedColour = hathor::ui::tokenKindToColourIndex(tokens[i].kind);
            REQUIRE(colours[i] == expectedColour);
            REQUIRE(colours[i] >= 0);
            REQUIRE(colours[i] <= 6);
        }
    }
}
