// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
  * GhostCompletionLogic.hpp — JUCE-free ghost-text lifecycle logic.
  *
  * Manages the lifecycle of ghost-text (inline completion) requests to llm-ls:
  *
  *   1. Debouncing — suppress requests while the user is still typing.
  *   2. Revision tracking — each editor change increments a revision counter.
  *      When a response arrives, we compare its revision against the current
  *      revision. If they differ, the response is stale and discarded.
  *   3. Timeout — llm-ls does not support `$/cancelRequest`. We enforce a
  *      latency timeout; if the server doesn't respond within the window,
  *      we treat the response as failed and surface a fallback.
  *   4. FIM prompt building — constructs the prefix/suffix/middle context
  *      from the document text and cursor position (AI-G2). The document
  *      prefix and suffix are explicitly computed and stored in the request
  *      so that the generated MIDDLE can be trimmed against the suffix.
  *   5. GhostResult assembly — extracts the completion text from the llm-ls
  *      response, trims prefix/suffix overlap, and produces a GhostResult
  *      carrying the explicit docPrefix/docSuffix for editor verification.
  *
  * AI-G2 (Fill-in-the-Middle as a First-Class Requirement):
  *   - The document prefix (text before cursor) and suffix (text after cursor)
  *     are explicitly computed and preserved in every request.
  *   - The response is trimmed against the suffix so only the MIDDLE is inserted.
  *   - The AI-8 authoring context is included as additional FIM context.
  *
  * This class is JUCE-free and fully unit-testable in hathor-ui-tests.
  *
  * Requirement references: AI-2, AI-3, AI-4, AI-8 §4, §7, AI-G1, AI-G2
  */

#include "GhostProtocol.hpp"
#include "GhostProviderConfig.hpp"
#include "GhostTriggerPolicy.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// GhostContext — defined in GhostProtocol.hpp (JUCE-free, shared with
//                 GhostTriggerPolicy — J-1)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// FIM context builder
// ---------------------------------------------------------------------------

/**
  * Built FIM context from a document + cursor position.
  *
  * AI-G2 (Fill-in-the-Middle as a First-Class Requirement):
  *   - `docPrefix`: the actual document text before the cursor (from the
  *     start of the document to the cursor position). Used for client-side
  *     prefix-overlap trimming of the generated MIDDLE.
  *   - `docSuffix`: the actual document text after the cursor (from the
  *     cursor position to the end of the document). Used for client-side
  *     suffix-overlap trimming of the generated MIDDLE.
  *   - `middle`: empty (llm-ls fills from tokenizer).
  *
  * llm-ls extracts prefix/suffix from the document it receives via
  * didOpen/didChange notifications. The docPrefix/docSuffix fields here
  * are NOT sent to llm-ls directly — they are used for client-side
  * response trimming and result verification. The AI-8 authoring context
  * is injected separately by the caller into fim.prefix.
  */
struct FimContext {
    std::string docPrefix;          ///< document text before cursor (for trimming)
    std::string docSuffix;          ///< document text after cursor (for trimming)
    std::string middle;             ///< empty (llm-ls fills from tokenizer)
};

/**
  * Build FIM context (prefix/suffix/middle) from the document and cursor.
  *
  * Computes the actual document prefix (text before the cursor) and suffix
  * (text after the cursor) from the document text and cursor position.
  * These are used for client-side response trimming: the generated MIDDLE
  * will be trimmed against the suffix to ensure it fits between prefix and
  * suffix without overlap.
  *
  * @param documentText  Full document text.
  * @param line          0-based cursor line.
  * @param character     0-based cursor character offset on the line.
  * @return FimContext with computed docPrefix/docSuffix.
  */
FimContext buildFimContext(std::string_view documentText, int line, int character);

// ---------------------------------------------------------------------------
// GhostCompletionLogic — main lifecycle controller
// ---------------------------------------------------------------------------

/**
 * GhostCompletionLogic
 *
 * Controls the request/response lifecycle for ghost-text completions.
 * This is a pure logic class — no I/O, no JUCE dependencies.
 *
 * Lifecycle:
 *   1. Editor changes → call onEditorChanged() with new context.
 *   2. Logic debounces: if still typing, suppress the request.
 *   3. When debounce expires → build FIM request → return it for the
 *      client to send.
 *   4. Response arrives → call onGhostResponse() → if revision matches,
 *      process the completions; if not, discard as stale.
 *   5. Timeout → if no response within the latency window, report failure.
 *
 * Usage:
 *   GhostCompletionLogic logic;
 *   logic.setEnabled(true);
 *   logic.setDebounceMs(300);
 *   logic.setTimeoutMs(5000);
 *
 *   // On editor change:
 *   auto opt = logic.onEditorChanged(ctx);
 *   if (opt) {
 *       // client sends the request
 *   }
 *
 *   // When response arrives:
 *   auto result = logic.onGhostResponse(requestId, response, timestamp);
 *   if (result) {
 *       // display ghost text
 *   }
 */
class GhostCompletionLogic
{
public:
    GhostCompletionLogic() = default;
    ~GhostCompletionLogic() = default;

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    /** Enable/disable ghost-text completion. */
    void setEnabled(bool e) noexcept { enabled_ = e; }

    /** True if ghost-text completion is enabled. */
    bool isEnabled() const noexcept { return enabled_; }

    /** Milliseconds to wait after the last editor change before requesting. */
    void setDebounceMs(int ms) noexcept { debounceMs_ = ms; }

    /** Maximum milliseconds to wait for a response before timeout. */
    void setTimeoutMs(int ms) noexcept { timeoutMs_ = ms; }

    /**
      * Set the trigger policy configuration (tunables for the ghost completion
      * triggering rules — J-1). Allows callers to configure which contexts
      * should trigger ghost completion without restructuring the lifecycle.
      */
    void setTriggerPolicyConfig(const GhostTriggerPolicyConfig& cfg) noexcept
    {
        policy_.setConfig(cfg);
    }

    /** Read the current trigger policy configuration. */
    const GhostTriggerPolicyConfig& triggerPolicyConfig() const noexcept
    {
        return policy_.config();
    }

    /**
      * Notify the ghost logic that a deterministic (LSP/metadata) completion
      * popup is currently visible or has been dismissed.
      *
      * When a deterministic popup is active, ghost completion is suppressed
      * (AI-G5 precedence) and any in-flight request / active ghost is cancelled.
      */
    void setDeterministicPopupActive(bool active) noexcept;

    /** True if a deterministic completion popup is currently active. */
    bool isDeterministicPopupActive() const noexcept { return deterministicPopupActive_; }

    // -----------------------------------------------------------------------
    // State queries
    // -----------------------------------------------------------------------

    /** True if a ghost completion request is currently in-flight. */
    bool hasPendingRequest() const noexcept { return pendingRequest_.has_value(); }

    /** The current editor revision. */
    int currentRevision() const noexcept { return revision_; }

    /** True if a ghost result is currently being displayed. */
    bool hasActiveGhost() const noexcept { return activeGhost_.has_value(); }

    // -----------------------------------------------------------------------
    // Lifecycle methods
    // -----------------------------------------------------------------------

    /**
     * Called when the editor changes (text, cursor, or focus).
     *
     * @param ctx   Current editor snapshot.
      * @param nowMs  Current time in milliseconds (steady clock epoch).
      * @return Optional GhostCompletionRequest — present if a request should
      *         be sent now (debounce expired, no in-flight request, provider valid).
      *         Absent if the request is suppressed (still debouncing, request
      *         already in flight, or ghost disabled / no valid config).
      */
     std::optional<std::pair<GhostCompletionRequest, std::string /*requestId*/>>
     onEditorChanged(const GhostContext& ctx, int64_t nowMs);

    /**
     * Called when a timer tick fires (for debounce + timeout checking).
     *
     * @param nowMs  Current time in milliseconds (steady clock).
     * @return Optional request — same semantics as onEditorChanged.
     */
    std::optional<std::pair<GhostCompletionRequest, std::string /*requestId*/>>
    onTimerTick(int64_t nowMs);

    /**
     * Called when a ghost completion response arrives.
     *
     * @param requestId   The request ID (must match pendingRequestId).
      * @param response    The parsed llm-ls response.
      * @param nowMs  Current time in milliseconds (steady clock epoch).
      * @return Optional GhostResult — present if the response is valid and
      *         not stale. Absent if the response is stale, timed out, or
      *         does not match the pending request.
      */
     std::optional<GhostResult> onGhostResponse(
         const std::string& requestId,
         const GhostCompletionResponse& response,
         int64_t nowMs);

    /**
     * Called when the ghost provider fails (error, timeout, or connection lost).
     *
     * @return The stale ghost result to clear, if any.
     */
    std::optional<GhostResult> onProviderFailure();

    /**
      * Called when the user accepts the ghost text.
      * Sends an accept notification (caller is responsible for sending it
      * via the LSP client) and clears the active ghost.
      *
      * Returns AcceptCompletionParams with acceptedCompletion set to the
      * currently selected candidate index and shownCompletions listing all
      * candidate indices that were presented (for llm-ls feedback).
      */
    std::optional<AcceptCompletionParams> onAccept();

    /**
      * Called when the user dismisses or modifies the ghost text.
      * Clears the active ghost and sends a reject notification.
      *
      * Returns RejectCompletionParams with shownCompletions listing all
      * candidate indices that were presented.
      */
    std::optional<RejectCompletionParams> onReject();

    /**
     * Clear the active ghost without sending accept/reject notifications.
     * Called when the cursor moves or the document changes.
     */
    void clearActiveGhost() noexcept;

    /**
      * Cancel any in-flight request (client-side cancellation — llm-ls
      * does not support `$/cancelRequest`).
      */
    void cancelPendingRequest() noexcept;

    // -----------------------------------------------------------------------
    // J-2: Candidate cycling API
    // -----------------------------------------------------------------------
    // When llm-ls returns multiple completions, they are cached in the
    // active ghost state. Cycling operates entirely on the cached set —
    // no new LLM request is issued. The selected candidate index wraps
    // around (wrapping is enabled) so cycling from the last candidate
    // returns to the first and vice-versa.
    // -----------------------------------------------------------------------

    /** Maximum number of candidates to request from the LLM. */
    static constexpr uint32_t kDefaultMaxCandidates = 4;

    /** Configure the bounded candidate count for new requests. */
    void setMaxCandidates(uint32_t n) noexcept { maxCandidates_ = n; }

    /** Current max candidates configured (0 = server default). */
    uint32_t maxCandidates() const noexcept { return maxCandidates_; }

    /** Number of cached candidates in the active ghost, or 0 if none. */
    size_t candidateCount() const noexcept;

    /** Index of the currently selected candidate (0-based). */
    size_t selectedCandidateIndex() const noexcept;

    /**
      * Get the currently selected candidate result.
      * Returns nullopt if no ghost is active.
      */
    std::optional<const GhostResult&> selectedCandidate() const noexcept;

    /**
      * Select the next candidate (wraps to first after the last).
      * Does NOT issue an LLM request — operates on cached candidates.
      * Returns true if the selection changed (i.e. there is an active ghost
      * with >1 candidate).
      */
    bool selectNextCandidate() noexcept;

    /**
      * Select the previous candidate (wraps to last before the first).
      * Does NOT issue an LLM request — operates on cached candidates.
      * Returns true if the selection changed.
      */
    bool selectPreviousCandidate() noexcept;

    // -----------------------------------------------------------------------
    // Helper: build a GhostCompletionRequest from context + provider config
    // -----------------------------------------------------------------------

    /**
      * Build the actual llm-ls request from the current context.
      * Populates FIM fields and backend config.
      */
    static GhostCompletionRequest buildRequest(
        const GhostContext& ctx,
        const GhostProviderConfig& config);

    /**
     * Get the current editor context (for building requests).
     * Used by onTimeout/onProviderFailure to know what was being requested.
     */
    const GhostContext& currentContext() const noexcept { return currentCtx_; }

private:
    // -----------------------------------------------------------------------
    // Config
    // -----------------------------------------------------------------------
    bool  enabled_     = false;
    int   debounceMs_  = 300;
    int   timeoutMs_   = 5000;
    uint32_t maxCandidates_ = kDefaultMaxCandidates;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    GhostContext currentCtx_;
    int         revision_         = 0;
    int64_t     lastChangeTimeMs_ = 0;  ///< ms since epoch of last editor change
    bool        debouncePending_  = false;

    /// The trigger policy (J-1) — decides whether to start the debounce cycle.
    GhostTriggerPolicy policy_;

    /// True when a deterministic (LSP/metadata) completion popup is visible.
    /// When true, ghost requests are suppressed (AI-G5 precedence).
    bool deterministicPopupActive_ = false;

    /// The context from the last request that was actually sent (in onTimerTick).
    /// Used by the trigger policy to suppress duplicate requests for unchanged
    /// editor state (J-1 test #9).
    std::optional<GhostContext> lastRequestedCtx_;

    /// The current in-flight request (requestId + revision at time of request).
    // AI-G2: also carries docPrefix/docSuffix for response trimming.
    struct PendingRequest {
        std::string requestId;
        int         revision;
        int64_t     sentAtMs;
        std::string docPrefix;  ///< snapshot of document prefix at request time
        std::string docSuffix;  ///< snapshot of document suffix at request time
    };
    std::optional<PendingRequest> pendingRequest_;

    /// The currently displayed ghost candidates (cleared on cursor move, accept,
    /// reject, or timeout).  J-2: caches all candidates returned by the LLM
    /// so the user can cycle through them locally without re-requesting.
    // AI-G2: each GhostResult carries docPrefix/docSuffix for editor-side verification.
    struct ActiveGhost {
        std::vector<GhostResult> candidates;   ///< all cached, trimmed candidates
        std::string              requestId;     ///< for accept/reject notifications
        int                      revision;      ///< document revision at generation time
        size_t                   selectedIndex = 0;  ///< currently displayed candidate
    };
    std::optional<ActiveGhost> activeGhost_;
};

} // namespace hathor::lsp
