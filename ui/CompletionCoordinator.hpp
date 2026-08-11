// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
  * CompletionCoordinator.hpp — JUCE-free state machine for LSP + ghost-text
  * completion coexistence.
  *
  * Orchestrates between GhostCompletionLogic (llm-ls ghost text) and the
  * LSP completion popup so that:
  *
  *   1. An LSP completion request (Ctrl+Space) cancels any active ghost
  *      and suppresses further ghost display while the popup is open.
  *   2. A ghost response that arrives while the LSP popup is visible is
  *      discarded (no ghost-behind-popup race).
  *   3. When the LSP popup is dismissed, the ghost cycle resumes normally.
  *   4. Document edits invalidate any in-flight or displayed ghost (revision
  *      tracking — a late ghost response for a stale document is rejected).
  *   5. Caret movement clears the active ghost but allows a new cycle to
  *      begin on the next idle tick.
  *
  * The coordinator owns a GhostCompletionLogic instance and delegates the
  * core debounce/timeout/stale-rejection lifecycle to it, adding only the
  * LSP coexistence and revision-tracking layer.
  *
  * Requirement references: AI-4, AI-G3 (deterministic LSP + ghost coexistence)
  */

#include "GhostCompletionLogic.hpp"
#include "GhostProtocol.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace hathor::ui {

class CompletionCoordinator
{
public:
    /**
      * Modal mode of the editor completion subsystem.
      */
    enum class Mode {
        Idle,           ///< no completion modal active
        GhostActive,    ///< ghost text is currently displayed
        LspPopupActive  ///< LSP completion popup is open
    };

    CompletionCoordinator();
    ~CompletionCoordinator() = default;

    // -----------------------------------------------------------------------
    // Configuration (delegated to GhostCompletionLogic)
    // -----------------------------------------------------------------------

    /** Enable/disable ghost-text completion. */
    void setGhostEnabled(bool e) noexcept;

    /** Milliseconds to wait after the last editor change before requesting. */
    void setGhostDebounceMs(int ms) noexcept;

    /** Maximum milliseconds to wait for a ghost response before timeout. */
    void setGhostTimeoutMs(int ms) noexcept;

    /**
       * Set the ghost trigger policy configuration (tunables for which
       * editor contexts should trigger ghost completion — J-1).
       * Delegates directly to GhostCompletionLogic.
       */
    void setGhostTriggerPolicyConfig(const lsp::GhostTriggerPolicyConfig& cfg) noexcept;

    /** Current trigger policy configuration. */
    const lsp::GhostTriggerPolicyConfig& ghostTriggerPolicyConfig() const noexcept;

    // -----------------------------------------------------------------------
    // State queries
    // -----------------------------------------------------------------------

    /** Current modal mode. */
    Mode mode() const noexcept { return mode_; }

    /** True when the LSP completion popup is active. */
    bool isLspPopupActive() const noexcept { return mode_ == Mode::LspPopupActive; }

    /** True when ghost text is currently displayed. */
    bool isGhostActive() const noexcept { return mode_ == Mode::GhostActive; }

    /** True if a ghost completion request is currently in-flight. */
    bool hasPendingGhostRequest() const noexcept;

    /** Current document revision (incremented on each edit). */
    int documentRevision() const noexcept { return docRevision_; }

    /** The document revision at which the active ghost was generated. */
    int ghostRevision() const noexcept { return ghostRevision_; }

    /* Whether ghost-text completion is enabled. */
    bool isGhostEnabled() const noexcept;

    /**
      * The document revision at which the last ghost request was sent.
      * Used to reject late responses for stale documents.
      */
    int ghostRequestRevision() const noexcept { return ghostRequestRevision_; }

    // -----------------------------------------------------------------------
    // Document lifecycle
    // -----------------------------------------------------------------------

    /**
      * Called from codeDocumentTextInserted / codeDocumentTextDeleted.
      * Increments docRevision_, cancels any pending ghost request, clears
      * any active ghost, and transitions to Idle.
      */
    void onDocumentChanged();

    // -----------------------------------------------------------------------
    // Ghost completion
    // -----------------------------------------------------------------------

    /**
      * Called on caret movement or forced ghost trigger (Ctrl+Shift+Space).
      *
      * Feeds the current editor context to GhostCompletionLogic (which handles
      * debouncing internally). Returns a request pair if one should be sent
      * immediately — normally this returns nullopt because the logic operates
      * in debounce mode; the actual request is produced by onGhostTick().
      *
      * Returns nullopt when ghost is disabled or the LSP popup is visible.
      */
    std::optional<std::pair<lsp::GhostCompletionRequest, std::string /*requestId*/>>
    triggerGhostCompletion(const lsp::GhostContext& ctx, int64_t nowMs);

    /**
      * Called on timer tick for debounce / timeout checking.
      * Returns a request pair if the debounce window has elapsed and a
      * request should be sent, nullopt otherwise.
      *
      * Suppressed when ghost is disabled or the LSP popup is visible.
      */
    std::optional<std::pair<lsp::GhostCompletionRequest, std::string /*requestId*/>>
    onGhostTick(int64_t nowMs);

    /**
      * Called when a ghost completion response arrives.
      *
      * Delegates to GhostCompletionLogic for stale-response rejection. If the
      * response is valid AND the LSP popup is not visible, transitions to
      * GhostActive and returns the GhostResult for display.
      *
      * Returns nullopt when the response is stale, timed out, or suppressed
      * because the LSP popup is currently visible.
      */
    std::optional<lsp::GhostResult>
    onGhostResponse(const std::string& requestId,
                    const lsp::GhostCompletionResponse& response,
                    int64_t nowMs);

    /**
      * Called when the user accepts the ghost text (Tab / Ctrl+.).
      * Returns AcceptCompletionParams for the notification to llm-ls.
      */
    std::optional<lsp::AcceptCompletionParams> onGhostAccepted();

    /**
      * Called when the user rejects the ghost text (Escape / any edit).
      * Returns RejectCompletionParams for the notification to llm-ls.
      */
    std::optional<lsp::RejectCompletionParams> onGhostRejected();

    /**
      * Clear the active ghost without sending accept/reject notifications.
      * Called when the cursor moves or the document changes.
      */
    void clearActiveGhost();

    /**
      * Cancel any in-flight ghost request (client-side cancellation — llm-ls
      * does not support $/cancelRequest).
      */
    void cancelPendingGhostRequest() noexcept;

    /**
      * Notify the coordinator that a deterministic completion popup has
      * become active or was dismissed. Delegates to GhostCompletionLogic
      * to suppress / resume ghost completion (AI-G5 precedence).
      */
    void setGhostDeterministicPopupActive(bool active) noexcept;

    // -----------------------------------------------------------------------
    // LSP completion control
    // -----------------------------------------------------------------------

    /**
      * Called when LSP completion is requested (Ctrl+Space).
      *
      * Cancels any active ghost (sends reject notification), cancels any
      * pending ghost request, and transitions to LspPopupActive.
      * The caller should proceed to show the LSP completion popup.
      */
    void requestLspCompletion();

    /**
      * Called when the LSP completion popup has been dismissed.
      *
      * Transitions to Idle, allowing ghost display to resume on the next
      * idle tick.
      */
    void onLspPopupDismissed();

    // -----------------------------------------------------------------------
    // Access to underlying ghost logic (for HathorTab to inspect state)
    // -----------------------------------------------------------------------

    /** Read-only access to the underlying ghost logic. */
    const lsp::GhostCompletionLogic& ghostLogic() const noexcept { return *ghostLogic_; }

private:
    /**
      * Set the ghost request revision to the current docRevision_.
      * Called when a request is dispatched.
      */
    void markGhostRequestSent() noexcept;

    Mode      mode_                   = Mode::Idle;
    int       docRevision_            = 0;
    int       ghostRevision_          = 0;
    int       ghostRequestRevision_   = 0;

    std::unique_ptr<lsp::GhostCompletionLogic> ghostLogic_;
};

} // namespace hathor::ui
