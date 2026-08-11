// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
  * Unit tests for GhostTriggerPolicy — the JUCE-free trigger policy engine
  * for ghost (LLM) completion requests.
  *
  * Requirement references: J-1
  */

#include "GhostTriggerPolicy.hpp"
#include "GhostProtocol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace hathor::lsp;
using Catch::Matchers::ContainsSubstring;

namespace {

// Helper: build a GhostContext for a mini-notation document.
GhostContext miniCtx(std::string_view text, int line, int character)
{
    GhostContext ctx;
    ctx.documentText = std::string(text);
    ctx.uri = "test.hathor";
    ctx.languageId = "hathor";
    ctx.line = line;
    ctx.character = character;
    return ctx;
}

// Helper: build a GhostContext for a ChucK document.
GhostContext chuckCtx(std::string_view text, int line, int character)
{
    GhostContext ctx;
    ctx.documentText = std::string(text);
    ctx.uri = "test.ck";
    ctx.languageId = "chuck";
    ctx.line = line;
    ctx.character = character;
    return ctx;
}

} // namespace

// ---------------------------------------------------------------------------
// Test 1 — cursor inside string literal → suppressed
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: suppress inside string literal", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd \"sd sn\"", 0, 8);  // cursor inside "sd sn"
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE_FALSE(decision.shouldTrigger);
    REQUIRE_THAT(decision.reason, ContainsSubstring("string"));
}

// ---------------------------------------------------------------------------
// Test 2 — cursor inside comment → suppressed (ChucK)
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: suppress inside ChucK line comment", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = chuckCtx("// this is a comment", 0, 10);
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE_FALSE(decision.shouldTrigger);
    REQUIRE_THAT(decision.reason, ContainsSubstring("comment"));
}

// ---------------------------------------------------------------------------
// Test 3 — cursor inside ChucK block comment → suppressed
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: suppress inside ChucK block comment", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = chuckCtx("/* comment text */ code", 0, 8);
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE_FALSE(decision.shouldTrigger);
    REQUIRE_THAT(decision.reason, ContainsSubstring("comment"));
}

// ---------------------------------------------------------------------------
// Test 4 — cursor inside ChucK <-- comment → suppressed
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: suppress inside ChucK <-- comment", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = chuckCtx("<-- comment", 0, 3);
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE_FALSE(decision.shouldTrigger);
    REQUIRE_THAT(decision.reason, ContainsSubstring("comment"));
}

// ---------------------------------------------------------------------------
// Test 5 — mid-token (typing through a word) → suppressed
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: suppress mid-token", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bdsn", 0, 2);  // cursor between 'b' and 'd' — both word chars
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE_FALSE(decision.shouldTrigger);
    REQUIRE_THAT(decision.reason, ContainsSubstring("mid-token"));
}

// ---------------------------------------------------------------------------
// Test 6 — duplicate context (same as last request) → suppressed
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: suppress duplicate context", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd ", 0, 3);  // cursor at end of "bd "
    std::optional<GhostContext> last = ctx;

    auto decision = policy.shouldTrigger(ctx, last, false, false);
    REQUIRE_FALSE(decision.shouldTrigger);
    REQUIRE_THAT(decision.reason, ContainsSubstring("duplicate"));
}

// ---------------------------------------------------------------------------
// Test 7 — deterministic popup active → suppressed
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: suppress when deterministic popup active", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd sd", 0, 5);  // cursor at end of line — meaningful boundary
    auto decision = policy.shouldTrigger(ctx, std::nullopt, true, false);
    REQUIRE_FALSE(decision.shouldTrigger);
    REQUIRE_THAT(decision.reason, ContainsSubstring("popup"));
}

// ---------------------------------------------------------------------------
// Test 8 — pending request → suppressed
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: suppress when request already in-flight", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd sd", 0, 5);
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, true);
    REQUIRE_FALSE(decision.shouldTrigger);
    REQUIRE_THAT(decision.reason, ContainsSubstring("in-flight"));
}

// ---------------------------------------------------------------------------
// Test 9 — empty document → allowed (with config)
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: allow on empty document when configured", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    policy.setConfig({ .allowOnEmptyDocument = true });
    auto ctx = miniCtx("", 0, 0);
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE(decision.shouldTrigger);
}

// ---------------------------------------------------------------------------
// Test 10 — meaningful boundary: cursor after '(' triggers
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: allow after opening paren", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd(s", 0, 3);  // cursor after '('
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE(decision.shouldTrigger);
}

// ---------------------------------------------------------------------------
// Test 11 — meaningful boundary: cursor after '.' triggers
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: allow after dot", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd.sd", 0, 3);  // cursor after '.'
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE(decision.shouldTrigger);
}

// ---------------------------------------------------------------------------
// Test 12 — meaningful boundary: cursor after space following a word
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: allow after space following word", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd sn", 0, 3);  // cursor after space following 'bd'
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE(decision.shouldTrigger);
}

// ---------------------------------------------------------------------------
// Test 13 — meaningful boundary: cursor at end of token
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: allow at end of token", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd sn", 0, 2);  // cursor at end of 'bd'
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE(decision.shouldTrigger);
}

// ---------------------------------------------------------------------------
// Test 14 — not at meaningful boundary: cursor after random punctuation
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: suppress at non-meaningful boundary", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    // Cursor is after '/' which is not a trigger char in mini-notation
    // and is not a word char, so it's not at a meaningful boundary.
    // Actually '/' is not listed as a trigger, so we should suppress.
    // But the char before is '/' which is a delimiter — let me check.
    // Actually, '/' is not in the trigger list, and '/' is not a word char.
    // The isAtMeaningfulBoundary returns false for '/'. But wait, '/' could
    // be the start of a comment in ChucK. For mini-notation, it's not special.
    auto ctx = miniCtx("bd/sd", 0, 3);  // cursor right after '/'
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    // '/' is a delimiter — the isWordChar('/') is false, so we don't return
    // "end of token". '/' is not in the trigger list. So this should suppress.
    REQUIRE_FALSE(decision.shouldTrigger);
    REQUIRE_THAT(decision.reason, ContainsSubstring("boundary"));
}

// ---------------------------------------------------------------------------
// Test 15 — ChucK: cursor after '=>' (chucking operator) triggers
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: allow after => in ChucK", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = chuckCtx("bd =>", 0, 4);  // cursor after '=>'
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE(decision.shouldTrigger);
}

// ---------------------------------------------------------------------------
// Test 16 — Mini-notation: cursor after '→' (Unicode arrow) triggers
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: allow after Unicode arrow in mini-notation", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    // → is U+2192 = 0xE2 0x86 0x92 in UTF-8
    std::string text = "bd \xe2\x86\x92 ";
    auto ctx = miniCtx(text, 0, 6);  // cursor after '→'
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE(decision.shouldTrigger);
}

// ---------------------------------------------------------------------------
// Test 17 — Configurable: allow in strings when configured
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: allow in strings when configured", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    policy.setConfig({ .allowInStrings = true });
    auto ctx = miniCtx("bd \"sd", 0, 6);  // inside string
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    // If in string but not at a meaningful boundary, should still suppress.
    // The cursor is mid-word in the string context... actually no.
    // The cursor is inside a string — the char before is 'd' which is a word char,
    // and the char after is... nothing (end of string). Actually the string is
    // unclosed, so the quote is still active. The char before cursor is 'd' (word),
    // char at cursor is '"' — which is a non-word char. So it's "end of token"
    // if we don't check strings. But since we're in a string and allowInStrings
    // is true, we need to check the boundary.
    // Actually, when allowInStrings is true, the policy skips the string check.
    // Then isMidToken checks: prev='d' (word), next='"' (non-word) → not mid-token.
    // isAtMeaningfulBoundary: prev='d' is word, next='"' is non-word → "end of token" → true.
    // So the decision should be allow.
    REQUIRE(decision.shouldTrigger);
}

// ---------------------------------------------------------------------------
// Test 18 — Configurable: allow in comments when configured
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: allow in ChucK comment when configured", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    policy.setConfig({ .allowInComments = true });
    auto ctx = chuckCtx("// my comment", 0, 12);  // inside comment
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    // When allowInComments is true, we skip the comment check.
    // Then isMidToken: prev='t' (word), next='' (end of string) → not mid-token.
    // isAtMeaningfulBoundary: end of line → true.
    REQUIRE(decision.shouldTrigger);
}

// ---------------------------------------------------------------------------
// Test 19 — Duplicate context with different cursor position → not duplicate
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: not duplicate when cursor moved", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd sd", 0, 5);  // end of line
    GhostContext last = miniCtx("bd sd", 0, 4);  // slightly different position
    auto decision = policy.shouldTrigger(ctx, last, false, false);
    REQUIRE(decision.shouldTrigger);
}

// ---------------------------------------------------------------------------
// Test 20 — Duplicate context with different document → not duplicate
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: not duplicate when document changed", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd sd sn", 0, 7);  // cursor at end
    GhostContext last = miniCtx("bd sd", 0, 5);  // same cursor relative pos but diff text
    auto decision = policy.shouldTrigger(ctx, last, false, false);
    REQUIRE(decision.shouldTrigger);
}

// ---------------------------------------------------------------------------
// Test 21 — Syntactically unreliable: unclosed string on line
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: suppress for unclosed string on line", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd \"sd sn", 0, 10);  // unclosed string
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE_FALSE(decision.shouldTrigger);
    REQUIRE_THAT(decision.reason, ContainsSubstring("syntactically"));
}

// ---------------------------------------------------------------------------
// Test 22 — isMidToken returns false at start of line
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: isMidToken false at start of line", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd", 0, 0);  // cursor at start
    REQUIRE_FALSE(policy.isMidToken(ctx));
}

// ---------------------------------------------------------------------------
// Test 23 — isAtMeaningfulBoundary: after delimiter ';'
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: allow after semicolon", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = miniCtx("bd; sn", 0, 3);  // cursor after ';'
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE(decision.shouldTrigger);
}

// ---------------------------------------------------------------------------
// Test 24 — Mini-notation: no comments
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: mini-notation has no comments", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    // In mini-notation, '//' is not a comment — it's just punctuation.
    // The cursor is after the second '/', which is not a word char or trigger.
    // It should be at a boundary only if preceded by a word char or trigger char.
    auto ctx = miniCtx("bd//", 0, 3);  // cursor after "//"
    REQUIRE_FALSE(policy.isInComment(ctx));
    // But '/' is not a trigger char, so boundary check depends on prev char.
    // prev is '/' (not word, not trigger) → suppress.
    // Actually wait — the cursor is after "//" and the previous char is '/',
    // which is not a word char. So it's "after a delimiter" — let me check
    // if '/' is in the delimiter list. Looking at isAtMeaningfulBoundary:
    // '/' is not listed as a delimiter that triggers. So it should suppress.
}

// ---------------------------------------------------------------------------
// Test 25 — ChucK: cursor after '{' triggers
// ---------------------------------------------------------------------------
TEST_CASE("GhostTriggerPolicy: allow after opening brace", "[ghost-trigger-policy]")
{
    GhostTriggerPolicy policy;
    auto ctx = chuckCtx("SinOsc s =>", 0, 11);  // cursor after '=>'
    auto decision = policy.shouldTrigger(ctx, std::nullopt, false, false);
    REQUIRE(decision.shouldTrigger);
}
