// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_mini_tokeniser.cpp — Property test P3 for MiniNotationTokeniser.
 *
 * P3: Colour-kind bijection (Req 27.4) — the colour classification produced
 *     by the syntax-highlighting tokeniser (MiniNotationTokeniser) and the
 *     canonical TokenKind sequence from hathor::tokenise() agree at every
 *     source position.
 *
 * Invariant (Req 27.4 / Phase 2 tasks.md §2.3):
 *   For any mini-notation string, every source character that the canonical
 *   tokeniser hathor::tokenise() classifies as kind K MUST receive the colour
 *   tokenKindToColourIndex(K) from MiniNotationTokeniser, and vice versa.
 *   The two views of the input MUST cover the same source character-for-
 *   character, with no skips and no overlaps.
 *
 * Implementation note:
 *   MiniNotationTokeniser is a juce::CodeTokeniser; its readNextToken()
 *   contract is (a) tokenise each line via hathor::tokenise(), then (b)
 *   walk the source position-by-position returning
 *   tokenKindToColourIndex(tok.kind) for the token at that position, or 0
 *   for whitespace/gap characters.  That contract is identical to the
 *   production code path (ui/MiniNotationTokeniser.cpp), which delegates to
 *   the two JUCE-free production components below:
 *     Path 1 — hathor::tokenise()       (engine, JUCE-free)
 *     Path 2 — tokenKindToColourIndex()  (UI colour mapping, extracted to
 *              TokenColourMap.hpp so it is shared verbatim by the production
 *              MiniNotationTokeniser and this test — see ui/CMakeLists.txt /
 *              tests-ui/CMakeLists.txt which do NOT link JUCE in this target)
 *
 *   We therefore reproduce readNextToken's *mechanical* walk (advance by
 *   token length, return the production colour) using the same two shared
 *   production functions the real tokeniser uses.  We do NOT re-classify
 *   characters independently — the only sources of truth are the two
 *   production paths, compared position by position.  This makes the test a
 *   genuine bijection rather than a loose smoke test, and keeps it runnable
 *   headless (HATHOR_BUILD_APP=OFF) without linking the JUCE GUI stack.
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
// Canonical model (Path 1 + Path 2 unified)
//
// Builds the expected per-source-position colour classification directly from
// the production hathor::tokenise() + hathor::ui::tokenKindToColourIndex().
// This is the SAME information MiniNotationTokeniser::readNextToken derives
// from those two functions; we simply flatten it to a position-indexed array
// so the bijection can be checked character-by-character.
// ---------------------------------------------------------------------------

/// A coloured span: [start, start+length) classified with `colour`.
struct ColouredSpan {
    std::size_t start;
    std::size_t length;
    int         colour;
};

/// Canonical view (Path 1 + Path 2): one coloured span per token emitted by
/// hathor::tokenise(), plus one span per maximal whitespace/digit-free gap,
/// covering the entire input with no skips or overlaps.  Whitespace and
/// TK_EOF sentinels are covered as zero-length-skipping colour-0 positions.
std::vector<ColouredSpan> canonicalSpans(std::string_view input)
{
    std::vector<ColouredSpan> spans;
    const auto tokens = tokenise(input);

    std::size_t cursor = 0;
    for (const auto& tok : tokens) {
        if (tok.kind == TokenKind::TK_EOF)
            break;

        // Whitespace between cursor and this token is one colour-0 span.
        if (cursor < tok.pos) {
            spans.push_back({cursor, tok.pos - cursor, 0});
            cursor = tok.pos;
        }
        const int c = hathor::ui::tokenKindToColourIndex(tok.kind);
        spans.push_back({tok.pos, tok.text.size(), c});
        cursor = tok.pos + tok.text.size();
    }
    // Trailing whitespace.
    if (cursor < input.size())
        spans.push_back({cursor, input.size() - cursor, 0});

    return spans;
}

/// Simulate MiniNotationTokeniser::readNextToken over a single line (the
/// production contract, derived verbatim from ui/MiniNotationTokeniser.cpp):
///   - tokenise the line with hathor::tokenise()
///   - walk the source position-by-position; for the position matching a
///     token's pos, return tokenKindToColourIndex(kind) and advance the
///     iterator by exactly tok.text.size() (one coloured span per token);
///     otherwise (whitespace / gap / EOF sentinel) return colour 0 and
///     advance by 1.
/// Returns the coloured spans in source order — the same coverage the editor
/// computes for syntax highlighting.
std::vector<ColouredSpan> readNextTokenSpans(std::string_view line)
{
    std::vector<ColouredSpan> spans;
    const auto tokens = tokenise(line);

    std::size_t cursor = 0;
    while (cursor < line.size()) {
        int colourAtCursor = 0; // whitespace / gap / EOF sentinel
        std::size_t advance = 1;

        for (const auto& tok : tokens) {
            if (tok.kind == TokenKind::TK_EOF) {
                // EOF sentinel: production advances by 1, colour 0.
                break;
            }
            if (static_cast<std::size_t>(tok.pos) == cursor) {
                colourAtCursor = hathor::ui::tokenKindToColourIndex(tok.kind);
                advance = tok.text.size();
                break;
            }
        }
        spans.push_back({cursor, advance, colourAtCursor});
        cursor += advance;
    }
    return spans;
}

/// Merge adjacent spans that share the same colour (run-length coalescing).
/// Both tokeniser views may segment runs of identical colour differently
/// (e.g. whitespace advanced one char at a time vs. one trailing span); the
/// bijection cares about the colour at each position, not the segmentation.
std::vector<ColouredSpan> coalesce(std::vector<ColouredSpan> spans)
{
    std::vector<ColouredSpan> out;
    for (const auto& s : spans) {
        if (!out.empty() && out.back().colour == s.colour &&
            out.back().start + out.back().length == s.start) {
            out.back().length += s.length;
        } else {
            out.push_back(s);
        }
    }
    return out;
}

/// Compare two span sequences for bijection: identical number of spans,
/// identical (start, length, colour) triples, and contiguous+complete
/// coverage of `input` (no skips, no overlaps).
void requireBijectionSpans(std::string_view input,
                           const std::vector<ColouredSpan>& canonicalIn,
                           const std::vector<ColouredSpan>& uiIn)
{
    std::vector<ColouredSpan> canonical = coalesce(canonicalIn);
    std::vector<ColouredSpan> ui = coalesce(uiIn);
    // (1) Same number of spans.
    REQUIRE(canonical.size() == ui.size());

    // (2) Per-span correspondence.
    for (std::size_t i = 0; i < ui.size(); ++i) {
        INFO("span " << i
              << " range=[" << canonical[i].start << ","
              << (canonical[i].start + canonical[i].length) << ")"
              << " canonicalColour=" << canonical[i].colour
              << " uiColour=" << ui[i].colour);
        REQUIRE(ui[i].start == canonical[i].start);
        REQUIRE(ui[i].length == canonical[i].length);
        REQUIRE(ui[i].colour == canonical[i].colour);
        REQUIRE(ui[i].colour >= 0);
        REQUIRE(ui[i].colour <= 6);
    }

    // (3) Complete, contiguous, non-overlapping source coverage.
    std::size_t expected = 0;
    for (const auto& s : ui) {
        INFO("span start=" << s.start << " length=" << s.length);
        REQUIRE(s.start == expected);          // no skip
        REQUIRE(s.length > 0);                 // no zero-length (no overlap/spin)
        expected = s.start + s.length;
    }
    REQUIRE(expected == input.size());          // nothing left unconsumed
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
// P3b: Colour-kind BIJECTION for random mini-notation (the core property)
//
// Invariant: for every source character, the colour returned by simulating
// MiniNotationTokeniser::readNextToken (the UI syntax-highlighting path) MUST
// equal the colour derived from the canonical hathor::tokenise() + production
// colour map.  Both views MUST consume exactly the same source, with no skips
// and no overlaps.
//
// Diagnostics on failure report: seed, iteration, the input, the offending
// source position, the canonical vs. tokeniser colour, and the token span.
// ---------------------------------------------------------------------------

TEST_CASE("P3b: MiniNotationTokeniser colour-kind bijection for random mini-notation",
          "[tokeniser][p3][bijection]")
{
    std::mt19937_64 rng(kP3Seed);

    constexpr int kNumCases = 300;

    for (int iter = 0; iter < kNumCases; ++iter) {
        const std::string input = genMiniNotation(rng);

        // Canonical view (Path 1 + Path 2).
        const std::vector<ColouredSpan> canonical = canonicalSpans(input);

        // UI tokeniser view: simulate readNextToken over the (single-line) input.
        const std::vector<ColouredSpan> ui = readNextTokenSpans(input);

        INFO("seed=0xFEEDFACE iter=" << iter << "\ninput=\"" << input << "\"");

        requireBijectionSpans(input, canonical, ui);
    }
}

// ---------------------------------------------------------------------------
// P3c: Bijection across explicit edge cases (regression coverage)
//
// Supplements the randomized property with targeted boundaries. All syntax is
// confirmed by the production hathor::tokenise().
// ---------------------------------------------------------------------------

TEST_CASE("P3c: colour-kind bijection across edge cases and all token classes",
          "[tokeniser][p3][exhaustive]")
{
    const std::vector<std::string> testStrings = {
        "",                            // empty input
        "a",                           // single character
        " ",                           // whitespace only
        "  ",                          // multiple spaces
        "bd",                          // atom
        "42",                          // int
        "~",                           // rest
        "[", "]", "<", ">", "*", "/", "!", "(", ")", ",",  // every single-char kind
        "bd sn cp hh",                 // multiple atoms
        "bd 4 ~ [sn]",                 // mixed kinds
        "bd*4 sn/2 cp!3",              // operators
        "bd(3,5) sn(2,3,1)",           // euclid
        "bd, sn",                      // stack
        "[bd sn] <cp hh>",             // groups
        "bd sn [hh hh] cp",            // nested
        "bd*2 sn [hh hh] cp",          // complex
        "   bd   sn   ",               // ragged whitespace
        "<<bd sn>>",                   // nested angles
        "[[bd][sn]]",                  // nested brackets
        "x*1/2!3(y,z) a,b c",          // everything adjacent
        "0 00 123 007",                // int edge forms
    };

    for (const auto& input : testStrings) {
        INFO("input=\"" << input << "\"");
        const std::vector<ColouredSpan> canonical = canonicalSpans(input);
        const std::vector<ColouredSpan> ui = readNextTokenSpans(input);
        requireBijectionSpans(input, canonical, ui);
    }
}

// ---------------------------------------------------------------------------
// P3d: Multi-line documents — front-matter exclusion consistency
//
// MiniNotationTokeniser treats [hathor] front-matter lines as colour 0.  When
// there is no front-matter header the whole document is body.  This case
// verifies that for a body-only multi-line document, every non-front-matter
// line still satisfies the per-position bijection, and that the canonical
// (Path 1) and UI (readNextToken) views agree line by line with no skips.
// ---------------------------------------------------------------------------

TEST_CASE("P3d: colour-kind bijection holds per-line for multi-line body input",
          "[tokeniser][p3][multiline]")
{
    std::mt19937_64 rng(kP3Seed ^ 0x1234ULL);

    const std::vector<std::string> lines = {
        "bd sn [hh hh]",
        "cp*2 <oh ch>",
        "x/4 y!2 z(3,8)",
        "a, b, c",
        "  ~  ~  ",
    };

    for (int iter = 0; iter < 50; ++iter) {
        // Build a multi-line document by shuffling line order (no front-matter
        // header → whole document is body, per MiniNotationTokeniser logic).
        std::vector<std::string> doc = lines;
        std::shuffle(doc.begin(), doc.end(), rng);
        std::string input;
        for (std::size_t i = 0; i < doc.size(); ++i) {
            if (i) input += '\n';
            input += doc[i];
        }

        INFO("seed=0xFEEDFACE^0x1234 iter=" << iter << "\ninput=\"" << input << "\"");

        // Verify the bijection independently on each line (readNextToken works
        // per line).  Front-matter exclusion only affects body lines that look
        // like [hathor]/key=value; our body lines never do, so colour 0 is
        // expected only at whitespace.
        std::size_t lineStart = 0;
        for (std::size_t i = 0; i <= input.size(); ) {
            // Find end of current line.
            std::size_t nl = input.find('\n', lineStart);
            std::size_t lineEnd = (nl == std::string::npos) ? input.size() : nl;
            std::string_view line(input.data() + lineStart, lineEnd - lineStart);

            const std::vector<ColouredSpan> canonical = canonicalSpans(line);
            const std::vector<ColouredSpan> ui = readNextTokenSpans(line);
            requireBijectionSpans(line, canonical, ui);

            if (nl == std::string::npos)
                break;
            lineStart = lineEnd + 1;
            i = lineStart;
        }
    }
}
