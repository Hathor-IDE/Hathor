// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
  * test_ghost_candidates.cpp — J-2 unit tests for multiple-candidate cycling.
  *
  * Tests cover:
  *   1.  Multiple candidates returned from one request can be cycled.
  *   2.  Alt+→ selects the next candidate.
  *   3.  Alt+← selects the previous candidate.
  *   4.  Cycling does not issue another LLM request.
  *   5.  Tab accepts exactly the selected candidate.
  *   6.  Esc dismisses the candidates without changing the document.
  *   7.  Editing the document invalidates the candidate set.
  *   8.  A deterministic completion popup suppresses ghost candidates.
  *   9.  Accepting one candidate discards the remaining candidates.
  *   10. Stale candidates cannot be applied to a newer document revision.
  *
  * Wrapping behavior (defined): cycling wraps from the last candidate to the
  * first and vice-versa.
  *
  * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
  */

#include <catch2/catch_test_macros.hpp>

#include "CompletionCoordinator.hpp"
#include "GhostCompletionLogic.hpp"
#include "GhostProtocol.hpp"

#include <string>
#include <vector>

using namespace hathor::ui;
using namespace hathor::lsp;

// ===========================================================================
// Helpers
// ===========================================================================

static GhostContext makeCtx(const std::string& text = "bd ",
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

static GhostCompletionResponse makeMultiResp(const std::string& id,
                                              const std::vector<std::string>& texts)
{
    GhostCompletionResponse resp;
    resp.request_id = id;
    for (const auto& t : texts)
        resp.completions.push_back({.generatedText = t});
    return resp;
}

/// Convenience: trigger a ghost + tick + respond with multi-candidate text.
struct GhostTestHarness
{
    GhostCompletionLogic logic;

    GhostTestHarness()
    {
        logic.setEnabled(true);
        logic.setDebounceMs(0);
        logic.setTimeoutMs(5000);
    }

    std::string triggerAndRespond(const GhostContext& ctx,
                                   const std::vector<std::string>& texts)
    {
        logic.onEditorChanged(ctx, 0);
        auto req = logic.onTimerTick(0);
        assert(req.has_value());
        std::string requestId = req.value().second;

        auto resp = makeMultiResp(requestId, texts);
        auto result = logic.onGhostResponse(requestId, resp, 10);
        assert(result.has_value());
        return requestId;
    }
};

// ===========================================================================
// 1. Multiple candidates returned from one request can be cycled.
// ===========================================================================

TEST_CASE("J-2: multiple candidates from one request can be cycled", "[j-2][cycle]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, {"sn cp hh", "sd hh cp", "hh sd cp"});

    REQUIRE(h.logic.candidateCount() == 3);
    REQUIRE(h.logic.selectedCandidateIndex() == 0);

    // First candidate
    auto sel0 = h.logic.selectedCandidate();
    REQUIRE(sel0.has_value());
    REQUIRE(sel0->text == "sn cp hh");

    // Next → second candidate
    h.logic.selectNextCandidate();
    REQUIRE(h.logic.selectedCandidateIndex() == 1);
    auto sel1 = h.logic.selectedCandidate();
    REQUIRE(sel1.has_value());
    REQUIRE(sel1->text == "sd hh cp");

    // Next → third candidate
    h.logic.selectNextCandidate();
    REQUIRE(h.logic.selectedCandidateIndex() == 2);
    auto sel2 = h.logic.selectedCandidate();
    REQUIRE(sel2.has_value());
    REQUIRE(sel2->text == "hh sd cp");
}

// ===========================================================================
// 2. Alt+→ selects the next candidate.
//    (Tested at the logic layer — Coordinator::selectNextGhostCandidate
//     delegates to GhostCompletionLogic::selectNextCandidate.)
// ===========================================================================

TEST_CASE("J-2: Alt+→ selects the next candidate", "[j-2][cycle]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req.value().second, {"first", "second", "third"});
    auto result = logic.onGhostResponse(req.value().second, resp, 10);
    REQUIRE(result.has_value());
    REQUIRE(logic.candidateCount() == 3);
    REQUIRE(logic.selectedCandidateIndex() == 0);

    // Alt+→ → next
    REQUIRE(logic.selectNextCandidate());
    REQUIRE(logic.selectedCandidateIndex() == 1);
    auto sel = logic.selectedCandidate();
    REQUIRE(sel.has_value());
    REQUIRE(sel->text == "second");
}

// ===========================================================================
// 3. Alt+← selects the previous candidate.
// ===========================================================================

TEST_CASE("J-2: Alt+← selects the previous candidate", "[j-2][cycle]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req.value().second, {"first", "second", "third"});
    logic.onGhostResponse(req.value().second, resp, 10);
    REQUIRE(logic.candidateCount() == 3);

    // Move to index 2 (third candidate) first
    logic.selectNextCandidate();
    logic.selectNextCandidate();
    REQUIRE(logic.selectedCandidateIndex() == 2);

    // Alt+← → previous
    REQUIRE(logic.selectPreviousCandidate());
    REQUIRE(logic.selectedCandidateIndex() == 1);
    auto sel = logic.selectedCandidate();
    REQUIRE(sel.has_value());
    REQUIRE(sel->text == "second");

    // Alt+← again → first
    REQUIRE(logic.selectPreviousCandidate());
    REQUIRE(logic.selectedCandidateIndex() == 0);
}

// ===========================================================================
// 4. Cycling does not issue another LLM request.
// ===========================================================================

TEST_CASE("J-2: cycling does not issue another LLM request", "[j-2][no-request]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req.value().second, {"a", "b", "c"});
    logic.onGhostResponse(req.value().second, resp, 10);
    REQUIRE(logic.hasActiveGhost());

    // No pending request after response
    REQUIRE_FALSE(logic.hasPendingRequest());

    // Cycle
    logic.selectNextCandidate();
    logic.selectNextCandidate();
    logic.selectPreviousCandidate();

    // Still no pending request — cycling did not trigger a new request
    REQUIRE_FALSE(logic.hasPendingRequest());
    REQUIRE(logic.hasActiveGhost());
    REQUIRE(logic.candidateCount() == 3);
}

// ===========================================================================
// 5. Tab accepts exactly the selected candidate.
// ===========================================================================

TEST_CASE("J-2: Tab accepts exactly the selected candidate", "[j-2][accept]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req.value().second, {"first", "second", "third"});
    logic.onGhostResponse(req.value().second, resp, 10);

    // Cycle to the second candidate
    logic.selectNextCandidate();
    REQUIRE(logic.selectedCandidateIndex() == 1);

    // Accept
    auto params = logic.onAccept(10);
    REQUIRE(params.has_value());

    // The accepted completion index should be 1 (the second candidate)
    REQUIRE(params->acceptedCompletion == 1);

    // shownCompletions should list all candidates that were shown (0, 1, 2)
    REQUIRE(params->shownCompletions.size() == 3);
    REQUIRE(params->shownCompletions[0] == 0);
    REQUIRE(params->shownCompletions[1] == 1);
    REQUIRE(params->shownCompletions[2] == 2);

    // Ghost cleared
    REQUIRE_FALSE(logic.hasActiveGhost());
    REQUIRE_FALSE(logic.hasPendingRequest());
}

// ===========================================================================
// 6. Esc dismisses the candidates without changing the document.
//    (At the logic layer: onReject clears ghost state without side effects
//     on the document. The document is never touched by GhostCompletionLogic.)
// ===========================================================================

TEST_CASE("J-2: Esc dismisses candidates without changing document state", "[j-2][reject]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req.value().second, {"first", "second", "third"});
    logic.onGhostResponse(req.value().second, resp, 10);
    REQUIRE(logic.hasActiveGhost());

    // The editor context (documentText) is immutable from the logic layer
    const auto& beforeCtx = logic.currentContext();
    std::string beforeDoc = beforeCtx.documentText;

    // Dismiss (Esc)
    auto params = logic.onReject(10);
    REQUIRE(params.has_value());

    // Context unchanged
    const auto& afterCtx = logic.currentContext();
    REQUIRE(afterCtx.documentText == beforeDoc);

    // Ghost cleared
    REQUIRE_FALSE(logic.hasActiveGhost());
    REQUIRE_FALSE(logic.hasPendingRequest());

    // shownCompletions should list all candidate indices
    REQUIRE(params->shownCompletions.size() == 3);
}

// ===========================================================================
// 7. Editing the document invalidates the candidate set.
// ===========================================================================

TEST_CASE("J-2: editing the document invalidates the candidate set", "[j-2][invalidate]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    // Revision 0: trigger + respond → GhostActive with 3 candidates
    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req.value().second, {"a", "b", "c"});
    logic.onGhostResponse(req.value().second, resp, 10);
    REQUIRE(logic.hasActiveGhost());
    REQUIRE(logic.candidateCount() == 3);

    // Edit the document — onEditorChanged increments revision and clears activeGhost
    ctx.documentText = "bd sn ";
    ctx.character = 6;
    logic.onEditorChanged(ctx, 100);

    // Active ghost is cleared — candidate set is invalidated
    REQUIRE_FALSE(logic.hasActiveGhost());
    REQUIRE_FALSE(logic.hasPendingRequest());

    // The old activeGhost_ is gone — no candidates accessible
    REQUIRE(logic.candidateCount() == 0);

    // Verify that the document revision has changed
    REQUIRE(logic.currentRevision() == 2);  // 0 (initial) → 1 (first edit) → 2 (this edit)
}

// ===========================================================================
// 8. A deterministic completion popup suppresses ghost candidates.
// ===========================================================================

TEST_CASE("J-2: deterministic completion popup suppresses ghost candidates", "[j-2][suppress]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    // Trigger + tick + respond → GhostActive with multiple candidates
    auto ctx = makeCtx("bd ", 0, 3);
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req->second, {"a", "b", "c"});
    auto result = coord.onGhostResponse(req->second, resp, 10);
    REQUIRE(result.has_value());
    REQUIRE(coord.isGhostActive());
    REQUIRE(coord.ghostCandidateCount() == 3);

    // Now show a deterministic popup — ghost candidates must be hidden/suppressed
    coord.requestLspCompletion();

    REQUIRE(coord.isLspPopupActive());
    REQUIRE_FALSE(coord.isGhostActive());
    REQUIRE(coord.ghostCandidateCount() == 0);

    // Cycling should have no effect while popup is active
    REQUIRE_FALSE(coord.selectNextGhostCandidate());
    REQUIRE_FALSE(coord.selectPreviousGhostCandidate());

    // Dismiss popup — ghost resumes (but current ghost was cleared by popup)
    coord.onLspPopupDismissed();
    REQUIRE_FALSE(coord.isLspPopupActive());
    REQUIRE_FALSE(coord.isGhostActive());
}

// ===========================================================================
// 9. Accepting one candidate discards the remaining candidates.
// ===========================================================================

TEST_CASE("J-2: accepting one candidate discards the remaining candidates", "[j-2][accept-discards]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req.value().second, {"a", "b", "c", "d"});
    logic.onGhostResponse(req.value().second, resp, 10);
    REQUIRE(logic.hasActiveGhost());
    REQUIRE(logic.candidateCount() == 4);

    // Cycle to the third candidate
    logic.selectNextCandidate();
    logic.selectNextCandidate();
    REQUIRE(logic.selectedCandidateIndex() == 2);

    // Accept the selected candidate
    auto params = logic.onAccept(10);
    REQUIRE(params.has_value());
    REQUIRE(params->acceptedCompletion == 2);

    // All candidates are discarded — ghost state fully cleared
    REQUIRE_FALSE(logic.hasActiveGhost());
    REQUIRE_FALSE(logic.hasPendingRequest());
    REQUIRE(logic.candidateCount() == 0);
}

// ===========================================================================
// 10. Stale candidates cannot be applied to a newer document revision.
// ===========================================================================

TEST_CASE("J-2: stale candidates cannot be applied to newer document revision", "[j-2][stale]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    // Revision 0: trigger + send request
    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());
    std::string requestId = req.value().second;

    // Document changes — revision increments, making the pending request stale
    // Also clear the pending request (as the coordinator would via onDocumentChanged)
    logic.cancelPendingRequest();
    ctx.documentText = "bd sn ";
    ctx.character = 6;
    logic.onEditorChanged(ctx, 100);

    // Late response arrives for the old request — revision mismatch → rejected
    auto resp = makeMultiResp(requestId, {"old-a", "old-b", "old-c"});
    auto result = logic.onGhostResponse(requestId, resp, 200);
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(logic.hasActiveGhost());
    REQUIRE(logic.candidateCount() == 0);

    // The coordinator-level staleness check (revision-based)
    {
        CompletionCoordinator coord;
        coord.setGhostEnabled(true);
        coord.setGhostDebounceMs(0);
        coord.setGhostTimeoutMs(5000);

        auto ctx1 = makeCtx("bd ", 0, 3);
        coord.triggerGhostCompletion(ctx1, 0);
        auto req1 = coord.onGhostTick(0);
        REQUIRE(req1.has_value());

        // Document changes
        coord.onDocumentChanged();
        REQUIRE(coord.documentRevision() == 1);

        // Late response from the old request — rejected by coordinator's
        // revision check (ghostRequestRevision_ != docRevision_)
        auto resp2 = makeMultiResp(req1->second, {"stale-a", "stale-b"});
        auto result2 = coord.onGhostResponse(req1->second, resp2, 100);
        REQUIRE_FALSE(result2.has_value());
        REQUIRE_FALSE(coord.isGhostActive());
        REQUIRE(coord.ghostCandidateCount() == 0);
    }
}

// ===========================================================================
// Bonus: Wrapping behavior — cycling from the last candidate wraps to first.
// ===========================================================================

TEST_CASE("J-2: cycling wraps from last candidate to first", "[j-2][cycle][wrap]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req.value().second, {"a", "b", "c"});
    logic.onGhostResponse(req.value().second, resp, 10);
    REQUIRE(logic.candidateCount() == 3);

    // Navigate to the last candidate
    logic.selectNextCandidate();  // → 1
    logic.selectNextCandidate();  // → 2 (last)
    REQUIRE(logic.selectedCandidateIndex() == 2);

    // Next → wraps to 0 (first)
    REQUIRE(logic.selectNextCandidate());
    REQUIRE(logic.selectedCandidateIndex() == 0);
    auto sel = logic.selectedCandidate();
    REQUIRE(sel.has_value());
    REQUIRE(sel->text == "a");

    // Previous from first → wraps to last
    REQUIRE(logic.selectPreviousCandidate());
    REQUIRE(logic.selectedCandidateIndex() == 2);
    sel = logic.selectedCandidate();
    REQUIRE(sel.has_value());
    REQUIRE(sel->text == "c");
}

// ===========================================================================
// Bonus: Cycling with a single candidate is a no-op.
// ===========================================================================

TEST_CASE("J-2: cycling with single candidate is a no-op", "[j-2][cycle][single]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req.value().second, {"only"});
    logic.onGhostResponse(req.value().second, resp, 10);
    REQUIRE(logic.candidateCount() == 1);

    REQUIRE_FALSE(logic.selectNextCandidate());
    REQUIRE_FALSE(logic.selectPreviousCandidate());
    REQUIRE(logic.selectedCandidateIndex() == 0);
}

// ===========================================================================
// Bonus: Cycling with no active ghost is a no-op.
// ===========================================================================

TEST_CASE("J-2: cycling with no active ghost is a no-op", "[j-2][cycle][empty]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    REQUIRE_FALSE(logic.selectNextCandidate());
    REQUIRE_FALSE(logic.selectPreviousCandidate());
    REQUIRE(logic.candidateCount() == 0);
}

// ===========================================================================
// Bonus: Coordinator-level cycling delegates correctly.
// ===========================================================================

TEST_CASE("J-2: coordinator cycling delegates to ghost logic", "[j-2][coordinator][cycle]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);

    auto ctx = makeCtx("bd ", 0, 3);
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req->second, {"first", "second", "third"});
    auto result = coord.onGhostResponse(req->second, resp, 10);
    REQUIRE(result.has_value());
    REQUIRE(coord.ghostCandidateCount() == 3);
    REQUIRE(coord.ghostSelectedCandidateIndex() == 0);

    // Coordinator delegates to ghost logic
    REQUIRE(coord.selectNextGhostCandidate());
    REQUIRE(coord.ghostSelectedCandidateIndex() == 1);

    auto sel = coord.selectedGhostResult();
    REQUIRE(sel.has_value());
    REQUIRE(sel->text == "second");

    REQUIRE(coord.selectPreviousGhostCandidate());
    REQUIRE(coord.ghostSelectedCandidateIndex() == 0);

    sel = coord.selectedGhostResult();
    REQUIRE(sel.has_value());
    REQUIRE(sel->text == "first");
}

// ===========================================================================
// Bonus: Empty completions after trimming are discarded (no empty candidates).
// ===========================================================================

TEST_CASE("J-2: empty candidates after trimming are discarded", "[j-2][trim]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    auto ctx = makeCtx("hello world", 0, 6);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());
    std::string requestId = req.value().second;

    // docPrefix = "hello ", docSuffix = "world"
    // "world" → trimmed against suffix → empty → discarded
    // "goodbye" → no overlap → kept
    // "world" → trimmed against suffix → empty → discarded
    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {
        {.generatedText = "world"},
        {.generatedText = "goodbye"},
        {.generatedText = "world"}
    };

    auto result = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "goodbye");
    REQUIRE(logic.candidateCount() == 1);
}

// ===========================================================================
// Bonus: maxCandidates caps the number of cached candidates.
// ===========================================================================

TEST_CASE("J-2: maxCandidates caps the number of cached candidates", "[j-2][max-candidates]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setMaxCandidates(2);

    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());
    REQUIRE(req.value().first.maxCandidates == 2);

    // Server returns 5 completions but only 2 should be cached
    auto resp = makeMultiResp(req.value().second, {"a", "b", "c", "d", "e"});
    auto result = logic.onGhostResponse(req.value().second, resp, 10);
    REQUIRE(result.has_value());
    REQUIRE(logic.candidateCount() == 2);
}

// ===========================================================================
// Bonus: After accept, a new request cycle is fresh (no stale candidates).
// ===========================================================================

TEST_CASE("J-2: after accept, cycling has no candidates", "[j-2][post-accept]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req.value().second, {"a", "b", "c"});
    logic.onGhostResponse(req.value().second, resp, 10);
    REQUIRE(logic.candidateCount() == 3);

    logic.onAccept(10);

    REQUIRE_FALSE(logic.hasActiveGhost());
    REQUIRE(logic.candidateCount() == 0);
    REQUIRE_FALSE(logic.selectNextCandidate());
    REQUIRE_FALSE(logic.selectPreviousCandidate());

    // A new request cycle should start fresh
    ctx.documentText = "bd sn ";
    ctx.character = 6;
    logic.onEditorChanged(ctx, 100);
    REQUIRE_FALSE(logic.hasActiveGhost());
}

// ===========================================================================
// Bonus: GhostResult preserves candidateIndex and FIM context for each
// candidate.
// ===========================================================================

TEST_CASE("J-2: each candidate preserves candidateIndex and FIM context", "[j-2][context]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    GhostContext ctx = makeCtx("hello world", 0, 6);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());
    std::string requestId = req.value().second;

    auto resp = makeMultiResp(requestId, {"first", "second", "third"});
    auto result = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(result.has_value());

    // All candidates share the same FIM context
    REQUIRE(result->docPrefix == "hello ");
    REQUIRE(result->docSuffix == "world");
    REQUIRE(result->cursorLine == 0);
    REQUIRE(result->character == 6);
    REQUIRE(result->requestId == requestId);

    // Each candidate should have a unique candidateIndex
    logic.selectNextCandidate();
    auto sel1 = logic.selectedCandidate();
    REQUIRE(sel1.has_value());
    REQUIRE(sel1->candidateIndex == 1);
    REQUIRE(sel1->docPrefix == "hello ");
    REQUIRE(sel1->docSuffix == "world");

    logic.selectNextCandidate();
    auto sel2 = logic.selectedCandidate();
    REQUIRE(sel2.has_value());
    REQUIRE(sel2->candidateIndex == 2);
    REQUIRE(sel2->docPrefix == "hello ");
    REQUIRE(sel2->docSuffix == "world");
    REQUIRE(sel2->cursorLine == 0);
    REQUIRE(sel2->character == 6);
}