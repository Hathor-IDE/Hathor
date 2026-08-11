// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_ghost_state_isolation.cpp — AI-G6 state isolation tests.
 *
 * Verifies that ghost-text completion is purely temporary UI state that
 * never leaks into the document model, undo history, compilation input,
 * or diagnostics before explicit acceptance. Covers all 14 AI-G6
 * requirements at the JUCE-free logic layer.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
 */

#include <catch2/catch_test_macros.hpp>

#include "GhostCompletionLogic.hpp"
#include "GhostProtocol.hpp"
#include "CompletionCoordinator.hpp"

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
// Req 1: Ghost text is temporary UI state — GhostCompletionLogic never
// modifies the editor context. The stored context is a snapshot; responses
// only update ghost-specific state, never the document text.
// ===========================================================================

TEST_CASE("AI-G6.1: GhostCompletionLogic never modifies the editor document", "[ai-g6][isolation]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    GhostContext ctx = makeCtx("bd sn");
    logic.onEditorChanged(ctx, 0);

    // The stored context should match what we passed in
    const auto& stored = logic.currentContext();
    REQUIRE(stored.documentText == "bd sn");
    REQUIRE(stored.line == 0);
    REQUIRE(stored.character == 5);

    // Fire a request + response
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = logic.onGhostResponse(req.value().second, makeResp(req.value().second, " hh"), 0);
    REQUIRE(resp.has_value());

    // Context unchanged after response
    const auto& stored2 = logic.currentContext();
    REQUIRE(stored2.documentText == "bd sn");
    REQUIRE(stored2.line == 0);
    REQUIRE(stored2.character == 5);
}

// ===========================================================================
// Req 2: Accepting ghost text produces a single accept notification — the
// actual document insertion is a separate step performed by the UI layer.
// The logic layer only returns AcceptCompletionParams (the notification),
// never the document text itself.
// ===========================================================================

TEST_CASE("AI-G6.2: onAccept returns notification params, not document content", "[ai-g6][accept]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd");
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto ghost = logic.onGhostResponse(req.value().second, makeResp(req.value().second, " sn"), 0);
    REQUIRE(ghost.has_value());
    REQUIRE(logic.hasActiveGhost());

    // onAccept returns notification params only — GhostResult is not exposed
    auto params = logic.onAccept(10);
    REQUIRE(params.has_value());
    REQUIRE(params->requestId == req.value().second);
    REQUIRE(params->acceptedCompletion == 0);
    REQUIRE(params->shownCompletions.size() == 1);

    // Ghost state cleared — the UI layer is responsible for inserting
    // the text into the document (single undoable edit)
    REQUIRE_FALSE(logic.hasActiveGhost());
}

// ===========================================================================
// Req 3: Rejecting ghost text clears overlay state — no text is inserted,
// no undo entry is created, and the document remains untouched.
// ===========================================================================

TEST_CASE("AI-G6.3: onReject clears ghost state without document modification", "[ai-g6][reject]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd");
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto ghost = logic.onGhostResponse(req.value().second, makeResp(req.value().second, " sn"), 0);
    REQUIRE(ghost.has_value());
    REQUIRE(logic.hasActiveGhost());

    auto rejectParams = logic.onReject(10);
    REQUIRE(rejectParams.has_value());
    REQUIRE(rejectParams->requestId == req.value().second);

    REQUIRE_FALSE(logic.hasActiveGhost());

    // Verify no request is in flight (state fully cleared)
    REQUIRE_FALSE(logic.hasPendingRequest());
}

// ===========================================================================
// Req 4: Document change (edit) clears any active ghost — ghost text from
// the old document state must never persist into the new state.
// ===========================================================================

TEST_CASE("AI-G6.4: document change clears active ghost", "[ai-g6][doc-change]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    // Phase 1: generate ghost for "bd"
    auto ctx1 = makeCtx("bd");
    logic.onEditorChanged(ctx1, 0);
    auto req1 = logic.onTimerTick(0);
    REQUIRE(req1.has_value());

    auto ghost1 = logic.onGhostResponse(req1.value().second, makeResp(req1.value().second, " sn"), 0);
    REQUIRE(ghost1.has_value());
    REQUIRE(logic.hasActiveGhost());

    // Phase 2: document changes — ghost must be cleared
    auto ctx2 = makeCtx("bd sn hh");
    ctx2.character = 8;
    logic.onEditorChanged(ctx2, 100);

    REQUIRE_FALSE(logic.hasActiveGhost());
    REQUIRE_FALSE(logic.hasPendingRequest());
}

// ===========================================================================
// Req 5: Stale ghost responses (revision mismatch) are rejected — a late
// response for an old document state must not produce ghost text.
// ===========================================================================

TEST_CASE("AI-G6.5: stale response (revision mismatch) is rejected", "[ai-g6][stale]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    auto ctx = makeCtx("bd");
    logic.onEditorChanged(ctx, 0);
    auto req1 = logic.onTimerTick(0);
    REQUIRE(req1.has_value());

    // Document changes — revision increments, making the in-flight response stale.
    // GhostCompletionLogic::onEditorChanged increments revision_ and resets
    // activeGhost_, but does NOT cancel the pending request (that is the
    // coordinator's job via onDocumentChanged -> cancelPendingRequest).
    // The stale response is rejected later in onGhostResponse via revision check.
    ctx.documentText = "bd sn";
    ctx.character = 5;
    logic.onEditorChanged(ctx, 100);
    REQUIRE_FALSE(logic.hasActiveGhost());
    // Pending request still tracked (revision mismatch will reject it)
    REQUIRE(logic.hasPendingRequest());

    // Late response for the old request — stale, must be rejected
    auto result = logic.onGhostResponse(req1.value().second, makeResp(req1.value().second, " old"), 200);
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(logic.hasActiveGhost());
}

// ===========================================================================
// Req 6: Timeout clears the pending request — a hung provider does not leave
// orphaned state that could produce a stale ghost.
// ===========================================================================

TEST_CASE("AI-G6.6: timeout clears pending request", "[ai-g6][timeout]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(100);

    auto ctx = makeCtx("bd");
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());
    REQUIRE(logic.hasPendingRequest());

    // Tick past the timeout window
    auto r2 = logic.onTimerTick(200);
    REQUIRE_FALSE(r2.has_value());
    REQUIRE_FALSE(logic.hasPendingRequest());

    // No active ghost — provider timed out cleanly
    REQUIRE_FALSE(logic.hasActiveGhost());
}

// ===========================================================================
// Req 7: Cancel clears pending request — client-initiated cancellation
// (e.g., user typed while request was in flight) cleans up all state.
// ===========================================================================

TEST_CASE("AI-G6.7: cancelPendingRequest clears all pending state", "[ai-g6][cancel]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd");
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());
    REQUIRE(logic.hasPendingRequest());

    logic.cancelPendingRequest();

    REQUIRE_FALSE(logic.hasPendingRequest());
    REQUIRE_FALSE(logic.hasActiveGhost());
}

// ===========================================================================
// Req 8: Provider failure clears the active ghost — if the LLM errors out,
// any previously displayed ghost must be cleared so stale text is not shown.
// ===========================================================================

TEST_CASE("AI-G6.8: provider failure clears active ghost", "[ai-g6][provider-failure]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd");
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto ghost = logic.onGhostResponse(req.value().second, makeResp(req.value().second, " sn"), 0);
    REQUIRE(ghost.has_value());
    REQUIRE(logic.hasActiveGhost());

    // Provider fails — active ghost must be cleared
    auto cleared = logic.onProviderFailure();
    REQUIRE(cleared.has_value());
    REQUIRE_FALSE(logic.hasActiveGhost());
    REQUIRE_FALSE(logic.hasPendingRequest());
    // " sn" is whitespace-stripped to "sn" by FIM trimming
    REQUIRE(cleared->text == "sn");
}

// ===========================================================================
// Req 9: FIM suffix trimming — the document suffix is NEVER silently
// discarded. If the LLM output includes suffix text, it is trimmed, not
// inserted. The trimmed GhostResult.text must not contain suffix overlap.
// ===========================================================================

TEST_CASE("AI-G6.9: FIM suffix trimming removes suffix overlap from ghost text", "[ai-g6][fim-suffix]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    // Document: "foo " + cursor + "bar" → docPrefix="foo ", docSuffix="bar"
    auto ctx = makeCtx("foo bar", 0, 4);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    // LLM output includes the suffix "bar" at the end — must be trimmed
    GhostCompletionResponse resp;
    resp.request_id = req.value().second;
    resp.completions = {{.generatedText = "hello bar"}};

    auto result = logic.onGhostResponse(req.value().second, resp, 0);
    REQUIRE(result.has_value());
    // docSuffix="bar" is trimmed from the end → "hello"
    REQUIRE(result->text == "hello");
    REQUIRE(result->docSuffix == "bar");
}

// ===========================================================================
// Req 10: FIM prefix trimming — if the LLM repeats the document prefix,
// the overlap is trimmed so only the MIDDLE is inserted.
// ===========================================================================

TEST_CASE("AI-G6.10: FIM prefix trimming removes prefix overlap from ghost text", "[ai-g6][fim-prefix]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    // Document: "hello " + cursor → docPrefix="hello ", docSuffix=""
    auto ctx = makeCtx("hello ", 0, 6);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    // LLM repeats "hello " → trimmed → "world"
    GhostCompletionResponse resp;
    resp.request_id = req.value().second;
    resp.completions = {{.generatedText = "hello world"}};

    auto result = logic.onGhostResponse(req.value().second, resp, 0);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "world");
    REQUIRE(result->docPrefix == "hello ");
}

// ===========================================================================
// Bonus: Rapid consecutive edits via CompletionCoordinator — each document
// change increments the revision and cancels pending requests. Late responses
// for old revisions are rejected; only the latest revision's ghost is accepted.
// No race conditions.
// ===========================================================================

TEST_CASE("AI-G6.11: rapid edits ensure only latest revision's ghost is accepted", "[ai-g6][rapid-edits]")
{
    // Use CompletionCoordinator because it properly cancels pending requests
    // on document changes (via onDocumentChanged -> cancelPendingRequest).
    // GhostCompletionLogic alone only resets activeGhost_ on onEditorChanged,
    // leaving the pending request for stale-rejection in onGhostResponse.
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);
    coord.setGhostTimeoutMs(5000);

    // Revision 1: trigger + tick + response → GhostActive
    auto ctx1 = makeCtx("bd");
    coord.triggerGhostCompletion(ctx1, 0);
    auto req1 = coord.onGhostTick(0);
    REQUIRE(req1.has_value());

    auto ghost1 = coord.onGhostResponse(req1.value().second, makeResp(req1.value().second, " sn"), 0);
    REQUIRE(ghost1.has_value());
    REQUIRE(coord.isGhostActive());

    // Revision 2: document changes → ghost cleared, revision incremented
    coord.onDocumentChanged();
    REQUIRE_FALSE(coord.isGhostActive());
    REQUIRE(coord.documentRevision() == 1);

    // Revision 3: trigger + tick + response with latest revision
    auto ctx3 = makeCtx("bd sn hh", 0, 8);
    ctx3.revision = 1; // matches current docRevision
    coord.triggerGhostCompletion(ctx3, 0);
    auto req2 = coord.onGhostTick(0);
    REQUIRE(req2.has_value());

    // Late response for revision 1's request — stale (revision mismatch)
    auto staleResult = coord.onGhostResponse(req1.value().second, makeResp(req1.value().second, "old"), 40);
    REQUIRE_FALSE(staleResult.has_value());
    REQUIRE_FALSE(coord.isGhostActive());

    // Valid response for revision 3's request — accepted
    auto validResult = coord.onGhostResponse(req2.value().second, makeResp(req2.value().second, " cp"), 50);
    REQUIRE(validResult.has_value());
    REQUIRE(validResult->text == "cp");
    REQUIRE(coord.isGhostActive());
}

// ===========================================================================
// Req 12: Ghost disabled — no requests are issued, no ghost state is
// created. The editor context is untouched.
// ===========================================================================

TEST_CASE("AI-G6.12: ghost disabled produces no requests or ghost state", "[ai-g6][disabled]")
{
    GhostCompletionLogic logic;
    // Ghost disabled by default
    REQUIRE_FALSE(logic.isEnabled());

    auto ctx = makeCtx("bd");
    auto result = logic.onEditorChanged(ctx, 0);
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(logic.hasActiveGhost());
    REQUIRE_FALSE(logic.hasPendingRequest());

    // Even after ticks, nothing happens
    auto tickResult = logic.onTimerTick(1000);
    REQUIRE_FALSE(tickResult.has_value());
    REQUIRE_FALSE(logic.hasActiveGhost());
}

// ===========================================================================
// Req 13: CompletionCoordinator suppresses ghost response when LSP popup
// is visible — ghost text never appears behind the LSP completion popup.
// ===========================================================================

TEST_CASE("AI-G6.13: coordinator suppresses ghost response during LSP popup", "[ai-g6][coordinator-lsp]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Enter LSP popup mode
    coord.requestLspCompletion();
    REQUIRE(coord.isLspPopupActive());

    // A ghost response arriving during LSP popup should be suppressed
    // The coordinator returns nullopt — ghost text is never displayed
    auto result = coord.onGhostResponse("fake-id", makeResp("fake-id", "ghost"), 0);
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(coord.isGhostActive());
    REQUIRE(coord.isLspPopupActive());
}

// ===========================================================================
// Req 14: After accept/reject, the ghost state is fully cleared — no
// residual ghost text persists that could leak into the document on a
// subsequent operation. The coordinator returns to Idle mode.
// ===========================================================================

TEST_CASE("AI-G6.14: after accept, coordinator returns to Idle with no ghost state", "[ai-g6][coordinator-post-accept]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Trigger + tick + response
    auto ctx = makeCtx("bd");
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    auto ghost = coord.onGhostResponse(req.value().second, makeResp(req.value().second, " sn"), 0);
    REQUIRE(ghost.has_value());
    REQUIRE(coord.isGhostActive());

    // Accept
    auto params = coord.onGhostAccepted(10);
    REQUIRE(params.has_value());

    // State fully cleared
    REQUIRE(coord.mode() == CompletionCoordinator::Mode::Idle);
    REQUIRE_FALSE(coord.isGhostActive());
    REQUIRE_FALSE(coord.hasPendingGhostRequest());
}

// ===========================================================================
// Bonus: Coordinator — ghost rejected via onGhostRejected also fully clears
// ===========================================================================

TEST_CASE("AI-G6: after reject, coordinator returns to Idle with no ghost state", "[ai-g6][coordinator-post-reject]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    auto ctx = makeCtx("bd");
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    auto ghost = coord.onGhostResponse(req.value().second, makeResp(req.value().second, " sn"), 0);
    REQUIRE(ghost.has_value());
    REQUIRE(coord.isGhostActive());

    auto params = coord.onGhostRejected(10);
    REQUIRE(params.has_value());

    REQUIRE(coord.mode() == CompletionCoordinator::Mode::Idle);
    REQUIRE_FALSE(coord.isGhostActive());
    REQUIRE_FALSE(coord.hasPendingGhostRequest());
}

// ===========================================================================
// Bonus: Coordinator — onDocumentChanged fully resets state
// ===========================================================================

TEST_CASE("AI-G6: coordinator document change clears ghost and cancels pending", "[ai-g6][coordinator-doc]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Start a ghost cycle
    auto ctx = makeCtx("hello world");
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    // Get a response to enter GhostActive
    auto ghost = coord.onGhostResponse(req.value().second, makeResp(req.value().second, "ing"), 10);
    REQUIRE(ghost.has_value());
    REQUIRE(coord.isGhostActive());

    // Document changes
    coord.onDocumentChanged();

    REQUIRE_FALSE(coord.isGhostActive());
    REQUIRE(coord.mode() == CompletionCoordinator::Mode::Idle);
    REQUIRE(coord.documentRevision() == 1);
    REQUIRE_FALSE(coord.hasPendingGhostRequest());
}

// ===========================================================================
// Bonus: Empty response does not produce a ghost result (no empty ghost)
// ===========================================================================

TEST_CASE("AI-G6: empty response does not produce ghost result", "[ai-g6][empty]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd");
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    // LLM returns text that is entirely whitespace + suffix overlap → trimmed to empty
    GhostCompletionResponse resp;
    resp.request_id = req.value().second;
    resp.completions = {{.generatedText = "   "}};

    auto result = logic.onGhostResponse(req.value().second, resp, 0);
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(logic.hasActiveGhost());
}

// ===========================================================================
// Bonus: GhostResult.docPrefix/docSuffix survive after accept — verify the
// FIM context is preserved in the result for editor-side verification
// ===========================================================================

TEST_CASE("AI-G6: GhostResult preserves FIM context for editor verification", "[ai-g6][fim-context]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    // Cursor inside a string — configure policy to allow it (FIM test, not
    // trigger-policy test)
    GhostTriggerPolicyConfig cfg;
    cfg.allowInStrings = true;
    logic.setTriggerPolicyConfig(cfg);

    // Document with non-trivial prefix and suffix
    // "s(\"bd  " + cursor + "sd hh\")\n"
    auto ctx = makeCtx("s(\"bd  sd hh\")\n", 0, 7);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    GhostCompletionResponse resp;
    resp.request_id = req.value().second;
    resp.completions = {{.generatedText = "sn sd hh\")\n"}};

    auto result = logic.onGhostResponse(req.value().second, resp, 0);
    REQUIRE(result.has_value());

    // The result must carry the FIM context so the editor can verify
    // the ghost text still fits before insertion
    REQUIRE(result->docPrefix == "s(\"bd  ");
    REQUIRE(result->docSuffix == "sd hh\")\n");
    REQUIRE(result->text == "sn");  // trimmed against suffix

    // The GhostResult also carries cursor position for acceptance verification
    REQUIRE(result->cursorLine == 0);
    REQUIRE(result->character == 7);
}
