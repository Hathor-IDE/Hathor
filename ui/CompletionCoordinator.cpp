// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
  * CompletionCoordinator.cpp — implementation of the coordinator state machine.
  *
  * Delegates core ghost-text lifecycle (debounce, timeout, stale rejection)
  * to GhostCompletionLogic. Adds LSP coexistence: suppress ghost when the
  * LSP popup is visible, cancel ghost on LSP request, resume ghost on
  * popup dismiss. Tracks document revision for staleness verification.
  *
  * Requirement references: AI-4, AI-G3
  */

#include "CompletionCoordinator.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

CompletionCoordinator::CompletionCoordinator()
    : ghostLogic_(std::make_unique<lsp::GhostCompletionLogic>())
{
}

// ---------------------------------------------------------------------------
// Configuration (delegated)
// ---------------------------------------------------------------------------

void CompletionCoordinator::setGhostEnabled(bool e) noexcept
{
    ghostLogic_->setEnabled(e);
}

void CompletionCoordinator::setGhostDebounceMs(int ms) noexcept
{
    ghostLogic_->setDebounceMs(ms);
}

void CompletionCoordinator::setGhostTimeoutMs(int ms) noexcept
{
    ghostLogic_->setTimeoutMs(ms);
}

void CompletionCoordinator::setGhostTriggerPolicyConfig(
    const lsp::GhostTriggerPolicyConfig& cfg) noexcept
{
    ghostLogic_->setTriggerPolicyConfig(cfg);
}

const lsp::GhostTriggerPolicyConfig&
CompletionCoordinator::ghostTriggerPolicyConfig() const noexcept
{
    return ghostLogic_->triggerPolicyConfig();
}

bool CompletionCoordinator::isGhostEnabled() const noexcept
{
    return ghostLogic_->isEnabled();
}

// -----------------------------------------------------------------------
// State queries
// -----------------------------------------------------------------------

bool CompletionCoordinator::hasPendingGhostRequest() const noexcept
{
    return ghostLogic_->hasPendingRequest();
}

// -----------------------------------------------------------------------
// Document lifecycle
// -----------------------------------------------------------------------

void CompletionCoordinator::onDocumentChanged()
{
    ++docRevision_;
    ghostLogic_->cancelPendingRequest();
    ghostLogic_->clearActiveGhost();
    mode_ = Mode::Idle;
}

// -----------------------------------------------------------------------
// Ghost completion
// -----------------------------------------------------------------------

    std::optional<std::pair<lsp::GhostCompletionRequest, std::string>>
    CompletionCoordinator::triggerGhostCompletion(const lsp::GhostContext& ctx, int64_t nowMs)
    {
        // Suppress if LSP popup is visible — ghost should not display behind popup.
        if (mode_ == Mode::LspPopupActive)
            return std::nullopt;

        // Defense-in-depth: sync the popup state to the ghost logic so its
        // internal trigger policy also knows the popup is invisible here.
        ghostLogic_->setDeterministicPopupActive(false);

        if (!ghostLogic_->isEnabled())
            return std::nullopt;

    // Stamp the coordinator's document revision onto the context so that
    // GhostCompletionLogic's internal revision-based staleness check uses
    // the same revision counter as the coordinator.
    lsp::GhostContext stamped = ctx;
    stamped.revision = docRevision_;

    return ghostLogic_->onEditorChanged(stamped, nowMs);
}

std::optional<std::pair<lsp::GhostCompletionRequest, std::string>>
CompletionCoordinator::onGhostTick(int64_t nowMs)
{
    // Suppress when LSP popup is visible — don't tick the ghost logic.
    if (mode_ == Mode::LspPopupActive)
        return std::nullopt;

    if (!ghostLogic_->isEnabled())
        return std::nullopt;

    auto opt = ghostLogic_->onTimerTick(nowMs);

    if (opt.has_value())
        markGhostRequestSent();

    return opt;
}

std::optional<lsp::GhostResult>
CompletionCoordinator::onGhostResponse(const std::string& requestId,
                                       const lsp::GhostCompletionResponse& response,
                                       int64_t nowMs)
{
    // Suppress if LSP popup is visible — don't render ghost behind popup.
    if (mode_ == Mode::LspPopupActive)
        return std::nullopt;

    // Late ghost response: if the document has changed since the request
    // was sent (revision mismatch), discard. This is an extra check beyond
    // GhostCompletionLogic's internal revision check.
    if (ghostRequestRevision_ != docRevision_)
        return std::nullopt;

    auto result = ghostLogic_->onGhostResponse(requestId, response, nowMs);

    if (result.has_value())
    {
        ghostRevision_ = docRevision_;
        mode_ = Mode::GhostActive;
    }

    return result;
}

std::optional<lsp::AcceptCompletionParams>
CompletionCoordinator::onGhostAccepted()
{
    auto params = ghostLogic_->onAccept();
    if (params.has_value())
        mode_ = Mode::Idle;
    return params;
}

std::optional<lsp::RejectCompletionParams>
CompletionCoordinator::onGhostRejected()
{
    auto params = ghostLogic_->onReject();
    if (params.has_value())
        mode_ = Mode::Idle;
    return params;
}

// ---------------------------------------------------------------------------
// J-3: Partial / multi-token acceptance
// ---------------------------------------------------------------------------

std::optional<lsp::PartialAcceptResult>
CompletionCoordinator::onGhostPartialAccepted(size_t acceptLen)
{
    auto result = ghostLogic_->onPartialAccept(acceptLen);
    // Mode stays GhostActive — the remaining suffix is still displayed.
    // No notification is sent to llm-ls (partial accept is UI-only state).
    // ghostRevision_ stays in sync with docRevision_ (it was set when the
    // ghost was generated; the document hasn't changed yet at this point).
    return result;
}

void CompletionCoordinator::onPartialAcceptDocumentChange() noexcept
{
    // Called AFTER the accepted prefix has been inserted into the CodeDocument.
    // We need to:
    //   1. Increment docRevision_ (the document changed)
    //   2. Sync ghostRevision_ with docRevision_ (the ghost is still valid)
    //   3. NOT clear the active ghost (unlike onDocumentChanged)
    //   4. NOT cancel pending requests (no new request needed just because
    //      a prefix was accepted — AI-G4/J-2 coexistence requirement)
    ++docRevision_;
    ghostRevision_ = docRevision_;
    // mode_ stays GhostActive
    // active ghost in GhostCompletionLogic is preserved (not cleared)
}

void CompletionCoordinator::clearActiveGhost()
{
    ghostLogic_->clearActiveGhost();
    mode_ = Mode::Idle;
}

void CompletionCoordinator::cancelPendingGhostRequest() noexcept
{
    ghostLogic_->cancelPendingRequest();
}

// ---------------------------------------------------------------------------
// J-2: Candidate cycling (delegated to GhostCompletionLogic)
// ---------------------------------------------------------------------------

size_t CompletionCoordinator::ghostCandidateCount() const noexcept
{
    return ghostLogic_->candidateCount();
}

size_t CompletionCoordinator::ghostSelectedCandidateIndex() const noexcept
{
    return ghostLogic_->selectedCandidateIndex();
}

bool CompletionCoordinator::selectNextGhostCandidate() noexcept
{
    return ghostLogic_->selectNextCandidate();
}

bool CompletionCoordinator::selectPreviousGhostCandidate() noexcept
{
    return ghostLogic_->selectPreviousCandidate();
}

std::optional<lsp::GhostResult> CompletionCoordinator::selectedGhostResult() const noexcept
{
    return ghostLogic_->selectedCandidate();
}

void CompletionCoordinator::markGhostRequestSent() noexcept
{
    ghostRequestRevision_ = docRevision_;
}

// -----------------------------------------------------------------------
// LSP completion control
// -----------------------------------------------------------------------

void CompletionCoordinator::setGhostDeterministicPopupActive(bool active) noexcept
{
    ghostLogic_->setDeterministicPopupActive(active);
}

void CompletionCoordinator::requestLspCompletion()
{
    // If ghost is currently displayed, send reject notification to llm-ls
    // (but don't re-query the ghost logic — it already processed the response).
    if (mode_ == Mode::GhostActive)
        ghostLogic_->onReject();

    // Cancel any pending ghost request — no point waiting if the user
    // explicitly invoked LSP completion.
    ghostLogic_->cancelPendingRequest();

    // Clear ghost display
    ghostLogic_->clearActiveGhost();

    // Suppress ghost completion while the popup is visible (AI-G5 precedence).
    ghostLogic_->setDeterministicPopupActive(true);
    mode_ = Mode::LspPopupActive;
}

void CompletionCoordinator::onLspPopupDismissed()
{
    mode_ = Mode::Idle;
    // Resume normal ghost completion now that the popup is gone.
    ghostLogic_->setDeterministicPopupActive(false);
}

} // namespace hathor::ui
