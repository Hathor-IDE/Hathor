// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
  * test_completion_coordinator.cpp — unit tests for CompletionCoordinator.
  *
  * Tests cover the LSP + ghost completion coexistence state machine:
  *   1. Initial state is Idle
  *   2. Document change clears ghost and increments revision
  *   3. LSP completion request cancels ghost and enters LspPopupActive
  *   4. Ghost response suppressed when LSP popup is visible
  *   5. Ghost response displayed when idle
  *   6. LSP popup dismissed resumes idle mode
  *   7. Ghost accepted clears ghost mode
  *   8. Ghost rejected clears ghost mode
  *   9. Late ghost response rejected on stale document (revision mismatch)
  *  10. Document change during in-flight request clears pending state
  *
  * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
  */

#include <catch2/catch_test_macros.hpp>

#include "CompletionCoordinator.hpp"

#include "GhostCompletionLogic.hpp"
#include "GhostProtocol.hpp"

#include <string>

using namespace hathor::ui;
using namespace hathor::lsp;

// ===========================================================================
// Helpers
// ===========================================================================

static GhostContext makeCtx(const std::string& text = "bd",
                            int line = 0, int character = -1)
{
    GhostContext ctx;
    ctx.documentText = text;
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = line;
    ctx.character = (character < 0) ? static_cast<int>(text.size()) : character;
    return ctx;
}

static GhostCompletionResponse makeResp(const std::string& id,
                                        const std::string& text)
{
    GhostCompletionResponse resp;
    resp.request_id = id;
    resp.completions = {{.generatedText = text}};
    return resp;
}

// ===========================================================================
// 1. Initial state is Idle
// ===========================================================================

TEST_CASE("CompletionCoordinator starts in Idle mode", "[coordinator][state]")
{
    CompletionCoordinator coord;

    REQUIRE(coord.mode() == CompletionCoordinator::Mode::Idle);
    REQUIRE_FALSE(coord.isLspPopupActive());
    REQUIRE_FALSE(coord.isGhostActive());
    REQUIRE_FALSE(coord.hasPendingGhostRequest());
    REQUIRE(coord.documentRevision() == 0);
    REQUIRE(coord.ghostRevision() == 0);
}

// ===========================================================================
// 2. Document change clears ghost and increments revision
// ===========================================================================

TEST_CASE("CompletionCoordinator onDocumentChanged clears ghost and bumps revision", "[coordinator][doc]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Start a ghost cycle: trigger + tick to send a request
    auto ctx = makeCtx("hello");
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    // Simulate a response to enter GhostActive
    std::string reqId = req->second;
    auto ghostResult = coord.onGhostResponse(reqId, makeResp(reqId, " world"), 10);
    REQUIRE(ghostResult.has_value());
    REQUIRE(coord.isGhostActive());

    // Now a document change should clear the ghost and increment revision
    coord.onDocumentChanged();

    REQUIRE_FALSE(coord.isGhostActive());
    REQUIRE(coord.mode() == CompletionCoordinator::Mode::Idle);
    REQUIRE(coord.documentRevision() == 1);
    REQUIRE_FALSE(coord.hasPendingGhostRequest());
}

// ===========================================================================
// 3. LSP completion request cancels ghost and enters LspPopupActive
// ===========================================================================

TEST_CASE("CompletionCoordinator requestLspCompletion cancels ghost and enters LspPopupActive", "[coordinator][lsp]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Trigger a ghost request and get a response
    auto ctx = makeCtx("test");
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    auto ghostResult = coord.onGhostResponse(req->second, makeResp(req->second, "ing"), 10);
    REQUIRE(ghostResult.has_value());
    REQUIRE(coord.isGhostActive());

    // Now request LSP completion — should cancel ghost and enter LspPopupActive
    coord.requestLspCompletion();

    REQUIRE(coord.mode() == CompletionCoordinator::Mode::LspPopupActive);
    REQUIRE(coord.isLspPopupActive());
    REQUIRE_FALSE(coord.isGhostActive());
    REQUIRE_FALSE(coord.hasPendingGhostRequest());
}

// ===========================================================================
// 4. Ghost response suppressed when LSP popup is visible
// ===========================================================================

TEST_CASE("CompletionCoordinator suppresses ghost response when LSP popup active", "[coordinator][suppress]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Enter LspPopupActive mode
    coord.requestLspCompletion();
    REQUIRE(coord.isLspPopupActive());

    // A ghost response arriving during LSP popup should be suppressed (nullopt)
    auto ctx = makeCtx("hello");
    auto resp = makeResp("some-id", "world");
    auto result = coord.onGhostResponse("some-id", resp, 0);
    REQUIRE_FALSE(result.has_value());

    REQUIRE(coord.isLspPopupActive());
    REQUIRE_FALSE(coord.isGhostActive());
}

// ===========================================================================
// 5. Ghost response displayed when idle
// ===========================================================================

TEST_CASE("CompletionCoordinator displays ghost response when idle", "[coordinator][response]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Trigger + tick to get a request
    auto ctx = makeCtx("bd sn");
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    // Valid response → ghost displayed
    auto result = coord.onGhostResponse(req->second, makeResp(req->second, " hh cp"), 10);
    REQUIRE(result.has_value());
    REQUIRE(coord.isGhostActive());
    REQUIRE(coord.ghostRevision() == coord.documentRevision());
}

// ===========================================================================
// 6. LSP popup dismissed resumes idle mode
// ===========================================================================

TEST_CASE("CompletionCoordinator onLspPopupDismissed resumes idle", "[coordinator][lsp]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);

    coord.requestLspCompletion();
    REQUIRE(coord.isLspPopupActive());

    coord.onLspPopupDismissed();

    REQUIRE(coord.mode() == CompletionCoordinator::Mode::Idle);
    REQUIRE_FALSE(coord.isLspPopupActive());
    REQUIRE_FALSE(coord.isGhostActive());
}

// ===========================================================================
// 7. Ghost accepted clears ghost mode
// ===========================================================================

TEST_CASE("CompletionCoordinator onGhostAccepted clears mode and returns params", "[coordinator][accept]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Trigger + tick + response to enter GhostActive
    auto ctx = makeCtx("bd");
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    auto result = coord.onGhostResponse(req->second, makeResp(req->second, " sn"), 10);
    REQUIRE(result.has_value());
    REQUIRE(coord.isGhostActive());

    // Accept
    auto params = coord.onGhostAccepted();
    REQUIRE(params.has_value());
    REQUIRE(params->requestId == req->second);

    REQUIRE(coord.mode() == CompletionCoordinator::Mode::Idle);
    REQUIRE_FALSE(coord.isGhostActive());
}

// ===========================================================================
// 8. Ghost rejected clears ghost mode
// ===========================================================================

TEST_CASE("CompletionCoordinator onGhostRejected clears mode and returns params", "[coordinator][reject]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Trigger + tick + response to enter GhostActive
    auto ctx = makeCtx("bd");
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    auto result = coord.onGhostResponse(req->second, makeResp(req->second, " sn"), 10);
    REQUIRE(result.has_value());
    REQUIRE(coord.isGhostActive());

    // Reject
    auto params = coord.onGhostRejected();
    REQUIRE(params.has_value());
    REQUIRE(params->requestId == req->second);

    REQUIRE(coord.mode() == CompletionCoordinator::Mode::Idle);
    REQUIRE_FALSE(coord.isGhostActive());
}

// ===========================================================================
// 9. Late ghost response rejected on stale document (revision mismatch)
// ===========================================================================

TEST_CASE("CompletionCoordinator rejects late ghost response on stale document", "[coordinator][stale]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Send a ghost request
    auto ctx = makeCtx("hello");
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    // Document changes while request is in flight — revision increments,
    // ghostRequestRevision is updated to match, but the pending request
    // in GhostCompletionLogic is cancelled.
    // Actually, onDocumentChanged cancels the pending request, so onGhostTick
    // won't produce another request. We need to test the revision mismatch
    // path differently.

    // Simulate: document changes, then a new ghost cycle starts, but an old
    // response arrives. The coordinator's onGhostResponse checks
    // ghostRequestRevision_ != docRevision_.

    // Start a new cycle after the document change
    coord.onDocumentChanged();  // revision 1, cancels pending, clears ghost
    REQUIRE(coord.documentRevision() == 1);

    // Start a new ghost request
    auto ctx2 = makeCtx("world");
    ctx2.revision = 1;
    coord.triggerGhostCompletion(ctx2, 0);
    auto req2 = coord.onGhostTick(0);
    REQUIRE(req2.has_value());
    // ghostRequestRevision_ == 1, docRevision_ == 1

    // Document changes again
    coord.onDocumentChanged();  // revision 2, ghostRequestRevision still 1
    REQUIRE(coord.documentRevision() == 2);
    REQUIRE(coord.ghostRequestRevision() == 1);

    // A late response from the previous request should be rejected
    auto result = coord.onGhostResponse(req2->second, makeResp(req2->second, " foo"), 10);
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(coord.isGhostActive());
}

// ===========================================================================
// 10. Document change during in-flight request clears pending state
// ===========================================================================

TEST_CASE("CompletionCoordinator documentChangeCancelsPendingGhostRequest", "[coordinator][doc]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(100);

    // Trigger a ghost cycle (debounce = 100ms)
    auto ctx = makeCtx("test text");
    coord.triggerGhostCompletion(ctx, 0);
    REQUIRE_FALSE(coord.hasPendingGhostRequest());

    // Tick before debounce expires — should not produce a request
    auto req1 = coord.onGhostTick(50);
    REQUIRE_FALSE(req1.has_value());
    REQUIRE_FALSE(coord.hasPendingGhostRequest());

    // Document changes while debounce is still pending
    coord.onDocumentChanged();
    REQUIRE(coord.documentRevision() == 1);
    REQUIRE_FALSE(coord.hasPendingGhostRequest());

    // Tick after debounce would have expired — should still not produce
    // a request because the document changed (context was invalidated)
    auto req2 = coord.onGhostTick(200);
    REQUIRE_FALSE(req2.has_value());
}

// ===========================================================================
// J-1: Trigger policy integration with coordinator
// ===========================================================================
// These tests verify that the trigger policy (which runs inside
// GhostCompletionLogic) is properly consulted by the coordinator's
// trigger + tick paths, and that the deterministic popup state is synced.

TEST_CASE("Coordinator syncs deterministic popup state to ghost logic", "[coordinator][trigger]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    GhostContext ctx = makeCtx("bd ");
    ctx.line = 0;
    ctx.character = 3;

    // When popup is active, triggerGhostCompletion returns nullopt
    coord.requestLspCompletion();
    REQUIRE(coord.isLspPopupActive());

    auto result = coord.triggerGhostCompletion(ctx, 0);
    REQUIRE_FALSE(result.has_value());

    // Verify ghost logic knows popup is active
    REQUIRE(coord.ghostLogic().isDeterministicPopupActive());

    // After dismiss, popup state is synced
    coord.onLspPopupDismissed();
    REQUIRE_FALSE(coord.isLspPopupActive());
    REQUIRE_FALSE(coord.ghostLogic().isDeterministicPopupActive());
}

TEST_CASE("Coordinator trigger policy suppresses ghost in string (AI-G5)", "[coordinator][trigger]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Cursor inside a string literal
    GhostContext ctx = makeCtx("bd \"sd", 0, 6);
    ctx.languageId = "hathor";

    coord.triggerGhostCompletion(ctx, 0);

    // Even with debounce=0, no request should fire because the trigger
    // policy suppresses at the onEditorChanged level.
    auto req = coord.onGhostTick(0);
    REQUIRE_FALSE(req.has_value());
}

TEST_CASE("Coordinator trigger policy suppresses mid-token (AI-G1)", "[coordinator][trigger]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Cursor mid-token (between two word chars)
    GhostContext ctx = makeCtx("bdsn", 0, 2);
    ctx.languageId = "hathor";

    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE_FALSE(req.has_value());
}

TEST_CASE("Coordinator trigger policy allows at boundary (J-1)", "[coordinator][trigger]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Cursor after a space following a word
    GhostContext ctx = makeCtx("bd ", 0, 3);
    ctx.languageId = "hathor";

    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());
}

TEST_CASE("Coordinator trigger policy respects configurable allowInStrings", "[coordinator][trigger]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Configure to allow in strings
    GhostTriggerPolicyConfig cfg;
    cfg.allowInStrings = true;
    coord.setGhostTriggerPolicyConfig(cfg);

    GhostContext ctx = makeCtx("bd \"", 0, 4);
    ctx.languageId = "hathor";

    // With allowInStrings=true, the in-string check is bypassed.
    // The cursor is at end of line (a meaningful boundary), so the policy
    // should allow the trigger.
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());
}

TEST_CASE("Coordinator: after LspPopupDismissed, ghost resumes", "[coordinator][trigger]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Show popup
    coord.requestLspCompletion();
    REQUIRE(coord.isLspPopupActive());

    // Try to trigger ghost — should be suppressed
    auto ctx = makeCtx("bd ");
    auto result = coord.triggerGhostCompletion(ctx, 0);
    REQUIRE_FALSE(result.has_value());

    auto req = coord.onGhostTick(0);
    REQUIRE_FALSE(req.has_value());

    // Dismiss popup
    coord.onLspPopupDismissed();

    // Now trigger should work
    coord.triggerGhostCompletion(ctx, 0);
    auto req2 = coord.onGhostTick(0);
    REQUIRE(req2.has_value());
}
