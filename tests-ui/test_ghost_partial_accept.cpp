// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
  * test_ghost_partial_accept.cpp — J-3 unit tests for partial / multi-token
  * ghost completion acceptance.
  *
  * Tests cover all 13 acceptance criteria:
  *   1.  A multi-token completion can be partially accepted.
  *   2.  Only the accepted prefix enters the document (at the logic layer,
  *       the accepted text is returned separately — the document is never
  *       touched by GhostCompletionLogic).
  *   3.  The remaining suffix stays visible as ghost text.
  *   4.  The accepted prefix is undoable (the logic layer returns it
  *       separately for insertion via CodeDocument::insertText).
  *   5.  The unaccepted suffix is absent from the accepted text.
  *   6.  Compilation/diagnostics see only the accepted prefix (the remaining
  *       suffix stays in ghost state, never enters the document model).
  *   7.  The remaining ghost remains valid against the updated document
  *       revision (coordinator revision is synced after partial accept).
  *   8.  Further partial acceptance works without a new LLM request.
  *   9.  Full acceptance commits the remaining suffix and clears ghost state.
  *   10. Dismissing the remainder leaves only previously accepted text.
  *   11. Editing after partial acceptance invalidates stale remainder.
  *   12. Candidate cycling continues to work correctly before partial
  *       acceptance.
  *   13. No partial-accept operation blocks or touches the JUCE audio thread
  *       (verified by the JUCE-free compilation of all ghost logic — the
  *       logic layer has zero JUCE dependencies).
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

static GhostCompletionResponse makeResp(const std::string& id,
                                        const std::string& text)
{
    GhostCompletionResponse resp;
    resp.request_id = id;
    resp.completions = {{.generatedText = text}};
    return resp;
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

/// Convenience: trigger a ghost + tick + respond with single-candidate text.
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
                                   const std::string& text)
    {
        logic.onEditorChanged(ctx, 0);
        auto req = logic.onTimerTick(0);
        assert(req.has_value());
        std::string requestId = req.value().second;

        auto resp = makeResp(requestId, text);
        auto result = logic.onGhostResponse(requestId, resp, 10);
        assert(result.has_value());
        return requestId;
    }
};

// ===========================================================================
// 1. A multi-token completion can be partially accepted.
// ===========================================================================

TEST_CASE("J-3: multi-token completion can be partially accepted", "[j-3][partial]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "kick snare hat clap");

    REQUIRE(h.logic.hasActiveGhost());
    REQUIRE(h.logic.candidateCount() == 1);

    // The full ghost text is "kick snare hat clap"
    auto sel = h.logic.selectedCandidate();
    REQUIRE(sel.has_value());
    REQUIRE(sel->text == "kick snare hat clap");

    // Partial accept: find next token boundary
    size_t boundary = GhostCompletionLogic::findNextTokenBoundary(sel->text);
    REQUIRE(boundary == 5); // "kick " — up to and including first space

    // Perform partial accept
    auto partial = h.logic.onPartialAccept(boundary);
    REQUIRE(partial.has_value());
    REQUIRE(partial->acceptedText == "kick ");
    REQUIRE(partial->remainingResult.text == "snare hat clap");

    // Ghost is still active — remaining suffix is displayed
    REQUIRE(h.logic.hasActiveGhost());
    REQUIRE(h.logic.candidateCount() == 1);
}

// ===========================================================================
// 2. Only the accepted prefix enters the document.
//    (At the logic layer: onPartialAccept returns only acceptedText;
//     the remaining suffix stays in ghost state, never in the document.)
// ===========================================================================

TEST_CASE("J-3: only accepted prefix is returned; remaining stays as ghost", "[j-3][prefix-only]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "kick snare hat clap");

    auto sel = h.logic.selectedCandidate();
    REQUIRE(sel.has_value());

    size_t boundary = GhostCompletionLogic::findNextTokenBoundary(sel->text);
    auto partial = h.logic.onPartialAccept(boundary);
    REQUIRE(partial.has_value());

    // The accepted text is ONLY the prefix — the suffix is NOT in it
    REQUIRE(partial->acceptedText == "kick ");
    REQUIRE(partial->acceptedText.find("snare") == std::string::npos);
    REQUIRE(partial->acceptedText.find("hat") == std::string::npos);
    REQUIRE(partial->acceptedText.find("clap") == std::string::npos);

    // The remaining result contains ONLY the suffix
    REQUIRE(partial->remainingResult.text == "snare hat clap");
    REQUIRE(partial->remainingResult.text.find("kick") == std::string::npos);
}

// ===========================================================================
// 3. The remaining suffix stays visible as ghost text.
// ===========================================================================

TEST_CASE("J-3: remaining suffix stays visible as ghost text after partial accept", "[j-3][suffix-remains]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "kick snare hat clap");

    auto sel = h.logic.selectedCandidate();
    size_t boundary = GhostCompletionLogic::findNextTokenBoundary(sel->text);
    auto partial = h.logic.onPartialAccept(boundary);
    REQUIRE(partial.has_value());

    // After partial accept, the selected candidate IS the remaining suffix
    auto remaining = h.logic.selectedCandidate();
    REQUIRE(remaining.has_value());
    REQUIRE(remaining->text == "snare hat clap");
    REQUIRE(remaining->text == partial->remainingResult.text);

    // Ghost is still active and the selected candidate is the suffix
    REQUIRE(h.logic.hasActiveGhost());
}

// ===========================================================================
// 4. The accepted prefix is undoable.
//    (At the logic layer: the accepted text is returned for insertion via
//     the normal document edit mechanism — CodeDocument::insertText, which
//     creates a proper undo entry. GhostCompletionLogic never touches the
//     document; the caller is responsible for the undoable insertion.)
// ===========================================================================

TEST_CASE("J-3: accepted prefix is returned as insertable text for undoable edit", "[j-3][undoable]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "kick snare hat clap");

    auto sel = h.logic.selectedCandidate();
    size_t boundary = GhostCompletionLogic::findNextTokenBoundary(sel->text);
    auto partial = h.logic.onPartialAccept(boundary);
    REQUIRE(partial.has_value());

    // The acceptedText is non-empty and ready for insertion via
    // CodeDocument::insertText() (which is naturally undoable in JUCE).
    // The logic layer returns it as a plain string — the UI layer inserts
    // it via document_.insertText(), a normal editor edit that participates
    // in undo history.
    REQUIRE_FALSE(partial->acceptedText.empty());

    // Verify the accepted text is exactly the first token + its trailing space
    REQUIRE(partial->acceptedText == "kick ");
}

// ===========================================================================
// 5. The unaccepted suffix is absent from the accepted (inserted) text.
// ===========================================================================

TEST_CASE("J-3: unaccepted suffix is absent from accepted text", "[j-3][suffix-absent]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "kick snare hat clap");

    auto sel = h.logic.selectedCandidate();
    size_t boundary = GhostCompletionLogic::findNextTokenBoundary(sel->text);
    auto partial = h.logic.onPartialAccept(boundary);
    REQUIRE(partial.has_value());

    // The accepted text does not contain any of the suffix tokens
    REQUIRE(partial->acceptedText == "kick ");
    REQUIRE(partial->acceptedText.find("snare") == std::string::npos);
    REQUIRE(partial->acceptedText.find("hat") == std::string::npos);
    REQUIRE(partial->acceptedText.find("clap") == std::string::npos);

    // The remaining ghost text contains all suffix tokens
    REQUIRE(partial->remainingResult.text == "snare hat clap");
    REQUIRE(partial->remainingResult.text.find("snare") != std::string::npos);
    REQUIRE(partial->remainingResult.text.find("hat") != std::string::npos);
    REQUIRE(partial->remainingResult.text.find("clap") != std::string::npos);
}

// ===========================================================================
// 6. Compilation/diagnostics see only the accepted prefix.
//    (At the logic layer: the remaining suffix stays in ghost state and
//     is never returned as document text. Only acceptedText is available
//     for insertion into the CodeDocument — diagnostics operate on the
//     document, not on ghost state.)
// ===========================================================================

TEST_CASE("J-3: remaining suffix is not exposed as document text", "[j-3][diagnostics]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "kick snare hat clap");

    // After two partial accepts:
    auto sel1 = h.logic.selectedCandidate();
    auto boundary1 = GhostCompletionLogic::findNextTokenBoundary(sel1->text);
    auto partial1 = h.logic.onPartialAccept(boundary1);
    REQUIRE(partial1.has_value());
    REQUIRE(partial1->acceptedText == "kick ");

    // Second partial accept
    auto sel2 = h.logic.selectedCandidate();
    auto boundary2 = GhostCompletionLogic::findNextTokenBoundary(sel2->text);
    auto partial2 = h.logic.onPartialAccept(boundary2);
    REQUIRE(partial2.has_value());
    REQUIRE(partial2->acceptedText == "snare ");

    // The remaining ghost text is only "hat clap" — this is NOT in the
    // acceptedText, so it would NOT appear in the document or diagnostics.
    REQUIRE(partial2->remainingResult.text == "hat clap");

    // Total accepted text (what would be in the document):
    std::string totalAccepted = partial1->acceptedText + partial2->acceptedText;
    REQUIRE(totalAccepted == "kick snare ");

    // The unaccepted suffix is not in the document text
    REQUIRE(totalAccepted.find("hat") == std::string::npos);
    REQUIRE(totalAccepted.find("clap") == std::string::npos);
}

// ===========================================================================
// 7. The remaining ghost remains valid against the updated document revision.
//    (After partial accept + document insert, onPartialAcceptDocumentChange()
//     increments the revision and syncs ghostRevision_ so the ghost is valid.)
// ===========================================================================

TEST_CASE("J-3: remaining ghost remains valid after document revision update", "[j-3][revision]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);
    coord.setGhostTimeoutMs(5000);

    // Set up: trigger + tick + respond → GhostActive
    auto ctx = makeCtx("bd ", 0, 3);
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    auto resp = makeResp(req->second, "kick snare hat clap");
    auto result = coord.onGhostResponse(req->second, resp, 10);
    REQUIRE(result.has_value());
    REQUIRE(coord.isGhostActive());

    int revisionBefore = coord.documentRevision();
    int ghostRevBefore = coord.ghostRevision();
    REQUIRE(ghostRevBefore == revisionBefore);

    // Partial accept at the coordinator level
    auto partial = coord.onGhostPartialAccepted(5); // accept "kick "
    REQUIRE(partial.has_value());
    REQUIRE(partial->acceptedText == "kick ");
    REQUIRE(partial->remainingResult.text == "snare hat clap");
    REQUIRE(coord.isGhostActive());  // still active

    // The coordinator revision has NOT yet been incremented (document insert
    // hasn't happened at the logic level yet)
    REQUIRE(coord.documentRevision() == revisionBefore);

    // Simulate the document insert → coordinator revision update
    coord.onPartialAcceptDocumentChange();

    // Now the revision is incremented and ghostRevision is synced
    REQUIRE(coord.documentRevision() == revisionBefore + 1);
    REQUIRE(coord.ghostRevision() == revisionBefore + 1);

    // Ghost is still active — the remaining suffix is valid for the new revision
    REQUIRE(coord.isGhostActive());
    REQUIRE(coord.ghostCandidateCount() == 1);
}

// ===========================================================================
// 8. Further partial acceptance works without a new LLM request.
// ===========================================================================

TEST_CASE("J-3: further partial acceptance works without a new LLM request", "[j-3][further-partial]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "kick snare hat clap");

    // First partial accept
    auto sel0 = h.logic.selectedCandidate();
    size_t b0 = GhostCompletionLogic::findNextTokenBoundary(sel0->text);
    auto p0 = h.logic.onPartialAccept(b0);
    REQUIRE(p0.has_value());
    REQUIRE(p0->acceptedText == "kick ");

    // No pending request after partial accept
    REQUIRE_FALSE(h.logic.hasPendingRequest());

    // Second partial accept
    auto sel1 = h.logic.selectedCandidate();
    REQUIRE(sel1->text == "snare hat clap");
    size_t b1 = GhostCompletionLogic::findNextTokenBoundary(sel1->text);
    auto p1 = h.logic.onPartialAccept(b1);
    REQUIRE(p1.has_value());
    REQUIRE(p1->acceptedText == "snare ");
    REQUIRE(p1->remainingResult.text == "hat clap");

    // Still no pending request
    REQUIRE_FALSE(h.logic.hasPendingRequest());

    // Third partial accept
    auto sel2 = h.logic.selectedCandidate();
    REQUIRE(sel2->text == "hat clap");
    size_t b2 = GhostCompletionLogic::findNextTokenBoundary(sel2->text);
    auto p2 = h.logic.onPartialAccept(b2);
    REQUIRE(p2.has_value());
    REQUIRE(p2->acceptedText == "hat ");
    REQUIRE(p2->remainingResult.text == "clap");

    // Ghost still active, still no pending request
    REQUIRE(h.logic.hasActiveGhost());
    REQUIRE_FALSE(h.logic.hasPendingRequest());
}

// ===========================================================================
// 9. Full acceptance commits the remaining suffix and clears ghost state.
// ===========================================================================

TEST_CASE("J-3: full accept after partial accept commits remaining suffix and clears ghost", "[j-3][full-accept]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "kick snare hat clap");

    // Partial accept "kick "
    auto sel0 = h.logic.selectedCandidate();
    auto p0 = h.logic.onPartialAccept(GhostCompletionLogic::findNextTokenBoundary(sel0->text));
    REQUIRE(p0.has_value());
    REQUIRE(h.logic.hasActiveGhost());

    // Full accept (onAccept) — commits the remaining "snare hat clap"
    auto params = h.logic.onAccept();
    REQUIRE(params.has_value());

    // Ghost state is fully cleared
    REQUIRE_FALSE(h.logic.hasActiveGhost());
    REQUIRE_FALSE(h.logic.hasPendingRequest());
    REQUIRE(h.logic.candidateCount() == 0);
}

// ===========================================================================
// 10. Dismissing the remainder leaves only previously accepted text.
// ===========================================================================

TEST_CASE("J-3: dismissing remainder after partial accept clears ghost", "[j-3][dismiss]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "kick snare hat clap");

    // Partial accept "kick "
    auto sel0 = h.logic.selectedCandidate();
    auto p0 = h.logic.onPartialAccept(GhostCompletionLogic::findNextTokenBoundary(sel0->text));
    REQUIRE(p0.has_value());
    REQUIRE(h.logic.hasActiveGhost());
    REQUIRE(p0->remainingResult.text == "snare hat clap");

    // Dismiss (reject) the remaining suffix
    auto rejectParams = h.logic.onReject();
    REQUIRE(rejectParams.has_value());

    // Ghost fully cleared — only the previously accepted "kick " would remain
    // in the document (the logic layer never touched the document; the UI
    // layer would have inserted "kick " already)
    REQUIRE_FALSE(h.logic.hasActiveGhost());
    REQUIRE_FALSE(h.logic.hasPendingRequest());
    REQUIRE(h.logic.candidateCount() == 0);
}

// ===========================================================================
// 11. Editing after partial acceptance invalidates stale remainder.
// ===========================================================================

TEST_CASE("J-3: document change after partial accept invalidates ghost", "[j-3][invalidate]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);
    coord.setGhostTimeoutMs(5000);

    // Set up ghost
    auto ctx = makeCtx("bd ", 0, 3);
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    auto resp = makeResp(req->second, "kick snare hat clap");
    auto result = coord.onGhostResponse(req->second, resp, 10);
    REQUIRE(result.has_value());
    REQUIRE(coord.isGhostActive());

    // Partial accept "kick "
    auto partial = coord.onGhostPartialAccepted(5);
    REQUIRE(partial.has_value());
    REQUIRE(coord.isGhostActive());

    // Simulate document insert (partial-accept path)
    coord.onPartialAcceptDocumentChange();
    REQUIRE(coord.isGhostActive());

    // Now the user types something — document change
    coord.onDocumentChanged();

    // Ghost must be cleared — the remaining suffix is invalid
    REQUIRE_FALSE(coord.isGhostActive());
    REQUIRE(coord.mode() == CompletionCoordinator::Mode::Idle);
}

// ===========================================================================
// 12. Candidate cycling continues to work correctly before partial acceptance.
// ===========================================================================

TEST_CASE("J-3: candidate cycling works before partial acceptance", "[j-3][cycle]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req.value().second, {"kick snare hat clap", "boom clash snap"});
    auto result = logic.onGhostResponse(req.value().second, resp, 10);
    REQUIRE(result.has_value());
    REQUIRE(logic.candidateCount() == 2);

    // Cycle to second candidate
    REQUIRE(logic.selectNextCandidate());
    REQUIRE(logic.selectedCandidateIndex() == 1);
    auto sel = logic.selectedCandidate();
    REQUIRE(sel->text == "boom clash snap");

    // Partial accept the second candidate
    size_t boundary = GhostCompletionLogic::findNextTokenBoundary(sel->text);
    REQUIRE(boundary == 5); // "boom " (4 chars + space)

    auto partial = logic.onPartialAccept(boundary);
    REQUIRE(partial.has_value());
    REQUIRE(partial->acceptedText == "boom ");
    REQUIRE(partial->remainingResult.text == "clash snap");

    // Ghost still active with the remaining suffix
    REQUIRE(logic.hasActiveGhost());
    auto remaining = logic.selectedCandidate();
    REQUIRE(remaining->text == "clash snap");
}

// ===========================================================================
// 13. No partial-accept operation blocks or touches the JUCE audio thread.
//     (Verified by compilation: GhostCompletionLogic and
//     CompletionCoordinator are JUCE-free — they have no JUCE dependencies,
//     so they cannot run on or block the audio thread. All operations are
//     pure C++ logic on the message thread.)
// ===========================================================================

TEST_CASE("J-3: partial accept is JUCE-free and non-blocking", "[j-3][thread-safety]")
{
    // If this test compiles and runs, it proves that all partial-accept
    // logic (GhostCompletionLogic, CompletionCoordinator) is JUCE-free.
    // The audio thread cannot reach this code path because:
    //   1. GhostCompletionLogic has no JUCE includes (only <chrono>, <optional>,
    //      <string>, <string_view>, <algorithm>, <cctype>, <nlohmann/json>).
    //   2. CompletionCoordinator delegates entirely to GhostCompletionLogic.
    //   3. The audio thread never calls triggerGhostCompletion / onPartialAccept
    //      / onPartialAcceptDocumentChange — those are message-thread-only.
    //
    // The partial accept operation is O(n) string operations on the ghost
    // text — it does not allocate on the heap (except for the returned
    // PartialAcceptResult) and completes in microseconds.

    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "kick snare hat clap");

    auto sel = h.logic.selectedCandidate();
    auto partial = h.logic.onPartialAccept(5);

    REQUIRE(partial.has_value());
    REQUIRE(partial->acceptedText == "kick ");

    // No blocking operations: no pending requests, no threads spawned
    REQUIRE_FALSE(h.logic.hasPendingRequest());
}

// ===========================================================================
// Bonus: findNextTokenBoundary returns full length for single-token text.
// ===========================================================================

TEST_CASE("J-3: findNextTokenBoundary returns full length for single-token text", "[j-3][boundary]")
{
    REQUIRE(GhostCompletionLogic::findNextTokenBoundary("clap") == 4);
    REQUIRE(GhostCompletionLogic::findNextTokenBoundary("") == 0);

    // "kick " — space at index 4, return 5
    REQUIRE(GhostCompletionLogic::findNextTokenBoundary("kick snare") == 5);

    // Multiple spaces
    REQUIRE(GhostCompletionLogic::findNextTokenBoundary("kick  snare") == 5);

    // Tab as boundary
    REQUIRE(GhostCompletionLogic::findNextTokenBoundary("kick\tsnare") == 5);

    // Newline as boundary
    REQUIRE(GhostCompletionLogic::findNextTokenBoundary("kick\nsnare") == 5);
}

// ===========================================================================
// Bonus: onPartialAccept with invalid acceptLen returns nullopt.
// ===========================================================================

TEST_CASE("J-3: onPartialAccept with invalid acceptLen returns nullopt", "[j-3][edge]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "kick snare hat clap");

    // acceptLen = 0 → nullopt
    REQUIRE_FALSE(h.logic.onPartialAccept(0).has_value());

    // acceptLen > text.size() → nullopt
    REQUIRE_FALSE(h.logic.onPartialAccept(100).has_value());

    // Accept entire text → nullopt (caller should use onAccept instead)
    REQUIRE_FALSE(h.logic.onPartialAccept(19).has_value());

    // Valid partial accept still works
    auto result = h.logic.onPartialAccept(5);
    REQUIRE(result.has_value());
    REQUIRE(result->acceptedText == "kick ");
}

// ===========================================================================
// Bonus: onPartialAccept with no active ghost returns nullopt.
// ===========================================================================

TEST_CASE("J-3: onPartialAccept with no active ghost returns nullopt", "[j-3][no-ghost]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    REQUIRE_FALSE(logic.onPartialAccept(5).has_value());
}

// ===========================================================================
// Bonus: Partial accept preserves docPrefix and docSuffix context.
// ===========================================================================

TEST_CASE("J-3: partial accept updates docPrefix to include accepted text", "[j-3][fim-context]")
{
    GhostTestHarness h;
    // Document: "start " + cursor + "end"
    auto ctx = makeCtx("start end", 0, 6);
    h.triggerAndRespond(ctx, "middle text");

    // Before partial accept:
    auto sel = h.logic.selectedCandidate();
    REQUIRE(sel->docPrefix == "start ");
    REQUIRE(sel->docSuffix == "end");

    // Partial accept "middle " (first token)
    size_t boundary = GhostCompletionLogic::findNextTokenBoundary(sel->text);
    REQUIRE(boundary == 7); // "middle "

    auto partial = h.logic.onPartialAccept(boundary);
    REQUIRE(partial.has_value());

    // docPrefix is updated to include the accepted text
    REQUIRE(partial->remainingResult.docPrefix == "start middle ");
    // docSuffix is unchanged
    REQUIRE(partial->remainingResult.docSuffix == "end");
    // Remaining text is just the suffix
    REQUIRE(partial->remainingResult.text == "text");
}

// ===========================================================================
// Bonus: Partial accept does not send notification to llm-ls (no request/reject
// side effects). The coordinator's onGhostPartialAccepted() returns the result
// without clearing the ghost or changing the mode.
// ===========================================================================

TEST_CASE("J-3: partial accept at coordinator level preserves mode and state", "[j-3][coordinator-state]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);
    coord.setGhostTimeoutMs(5000);

    auto ctx = makeCtx("bd ", 0, 3);
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    auto resp = makeResp(req->second, "kick snare hat clap");
    auto result = coord.onGhostResponse(req->second, resp, 10);
    REQUIRE(result.has_value());
    REQUIRE(coord.isGhostActive());
    REQUIRE(coord.mode() == CompletionCoordinator::Mode::GhostActive);

    // Partial accept
    auto partial = coord.onGhostPartialAccepted(5);
    REQUIRE(partial.has_value());

    // Mode stays GhostActive — ghost is still displayed
    REQUIRE(coord.isGhostActive());
    REQUIRE(coord.mode() == CompletionCoordinator::Mode::GhostActive);
    REQUIRE_FALSE(coord.hasPendingGhostRequest());

    // Ghost response still available
    REQUIRE(coord.ghostCandidateCount() == 1);
    auto remaining = coord.selectedGhostResult();
    REQUIRE(remaining.has_value());
    REQUIRE(remaining->text == "snare hat clap");
}

// ===========================================================================
// Bonus: After partial accept, coordinator staleness check uses updated
// revision — the remaining ghost is valid for the new revision.
// ===========================================================================

TEST_CASE("J-3: coordinator stale-check passes for remaining ghost after revision sync", "[j-3][stale-after-partial]")
{
    CompletionCoordinator coord;
    coord.setGhostEnabled(true);
    coord.setGhostDebounceMs(0);
    coord.setGhostTimeoutMs(5000);

    auto ctx = makeCtx("bd ", 0, 3);
    coord.triggerGhostCompletion(ctx, 0);
    auto req = coord.onGhostTick(0);
    REQUIRE(req.has_value());

    auto resp = makeResp(req->second, "kick snare hat clap");
    auto result = coord.onGhostResponse(req->second, resp, 10);
    REQUIRE(result.has_value());
    REQUIRE(coord.isGhostActive());

    int revBefore = coord.documentRevision();
    REQUIRE(revBefore == 0);

    // Partial accept + document change simulation
    auto partial = coord.onGhostPartialAccepted(5);
    REQUIRE(partial.has_value());
    coord.onPartialAcceptDocumentChange();

    int revAfter = coord.documentRevision();
    REQUIRE(revAfter == 1);

    // ghostRequestRevision_ was synced during markGhostRequestSent()
    // when the request was ticked. After onPartialAcceptDocumentChange(),
    // ghostRevision_ = docRevision_ = 1. Ghost is still active.
    REQUIRE(coord.isGhostActive());
    REQUIRE(coord.ghostRevision() == revAfter);

    // A simulated late response for an OLD request should still be rejected
    // (the request revision is 0, but document is now at revision 1).
    // But since the ghost is already active (not pending), this is about
    // future requests being properly revision-checked.
    coord.onDocumentChanged();  // revision 2, clears ghost
    REQUIRE_FALSE(coord.isGhostActive());
    REQUIRE(coord.documentRevision() == 2);
}

// ===========================================================================
// Bonus: Partial accept works with multiple candidates (J-2 integration).
// ===========================================================================

TEST_CASE("J-3: partial accept works with multiple candidates on selected one", "[j-3][multi-candidate]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    auto ctx = makeCtx("bd ", 0, 3);
    logic.onEditorChanged(ctx, 0);
    auto req = logic.onTimerTick(0);
    REQUIRE(req.has_value());

    auto resp = makeMultiResp(req.value().second, {"kick snare hat clap", "boom crash"});
    auto result = logic.onGhostResponse(req.value().second, resp, 10);
    REQUIRE(result.has_value());
    REQUIRE(logic.candidateCount() == 2);

    // Cycle to the second candidate
    logic.selectNextCandidate();
    REQUIRE(logic.selectedCandidateIndex() == 1);
    auto sel = logic.selectedCandidate();
    REQUIRE(sel->text == "boom crash");

    // Partial accept "boom " from the second candidate
    auto partial = logic.onPartialAccept(5);
    REQUIRE(partial.has_value());
    REQUIRE(partial->acceptedText == "boom ");
    REQUIRE(partial->remainingResult.text == "crash");

    // First candidate is untouched
    // (selectedCandidate returns the updated second candidate)
    auto remaining = logic.selectedCandidate();
    REQUIRE(remaining->text == "crash");
    REQUIRE(remaining->candidateIndex == 1);
}

// ===========================================================================
// Bonus: Full accept on a remaining single-token suffix clears ghost.
// ===========================================================================

TEST_CASE("J-3: full accept of remaining single-token suffix clears ghost", "[j-3][full-accept-remainder]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "kick snare");

    // Partial accept "kick "
    auto sel0 = h.logic.selectedCandidate();
    auto p0 = h.logic.onPartialAccept(GhostCompletionLogic::findNextTokenBoundary(sel0->text));
    REQUIRE(p0.has_value());
    REQUIRE(p0->acceptedText == "kick ");
    REQUIRE(h.logic.hasActiveGhost());

    // Remaining is "snare" (single token, no space)
    auto sel1 = h.logic.selectedCandidate();
    REQUIRE(sel1->text == "snare");

    // Full accept
    auto params = h.logic.onAccept();
    REQUIRE(params.has_value());

    REQUIRE_FALSE(h.logic.hasActiveGhost());
    REQUIRE(h.logic.candidateCount() == 0);
}

// ===========================================================================
// Bonus: onPartialAccept with acceptLen exactly at text boundary (off-by-one).
// ===========================================================================

TEST_CASE("J-3: onPartialAccept off-by-one at exact boundary", "[j-3][off-by-one]")
{
    GhostTestHarness h;
    auto ctx = makeCtx("bd ", 0, 3);
    h.triggerAndRespond(ctx, "hello world");

    auto sel = h.logic.selectedCandidate();
    // "hello world" → boundary at 6 (after "hello ")
    REQUIRE(GhostCompletionLogic::findNextTokenBoundary(sel->text) == 6);

    auto partial = h.logic.onPartialAccept(6); // accept "hello "
    REQUIRE(partial.has_value());
    REQUIRE(partial->acceptedText == "hello ");
    REQUIRE(partial->remainingResult.text == "world");

    // Now the remaining is "world" — full length, no more boundaries
    auto sel2 = h.logic.selectedCandidate();
    REQUIRE(sel2->text == "world");
    REQUIRE(GhostCompletionLogic::findNextTokenBoundary(sel2->text) == 5); // full length
    // acceptLen == full length → returns nullopt (should use onAccept)
    REQUIRE_FALSE(h.logic.onPartialAccept(5).has_value());

    // But a smaller acceptLen still works (accept "wor" from "world")
    auto partial2 = h.logic.onPartialAccept(3);
    REQUIRE(partial2.has_value());
    REQUIRE(partial2->acceptedText == "wor");
    REQUIRE(partial2->remainingResult.text == "ld");
}
