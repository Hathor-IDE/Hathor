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
 *      from the document text and cursor position, matching llm-ls's expected
 *      reversed-line prefix and forward-line suffix format.
 *   5. GhostResult assembly — extracts the completion text from the llm-ls
 *      response, trims to the current prefix, and produces a GhostResult.
 *
 * This class is JUCE-free and fully unit-testable in hathor-ui-tests.
 *
 * Requirement references: AI-4, AI-8 §4, §7
 */

#include "GhostProtocol.hpp"
#include "GhostProviderConfig.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// GhostContext — current editor state snapshot for ghost text
// ---------------------------------------------------------------------------

/**
 * A point-in-time snapshot of the editor state needed for ghost-text requests.
 * This mirrors the fields from EditorContextSnapshot relevant to FIM.
 */
struct GhostContext {
    std::string documentText;  ///< full document text
    std::string uri;           ///< file:// URI or synthetic URI
    std::string languageId;    ///< "hathor" or "chuck"
    int         line      = 0; ///< 0-based cursor line
    int         character = 0; ///< 0-based cursor character offset
    int         revision  = 0; ///< incremented on each document edit
};

// ---------------------------------------------------------------------------
// FIM context builder
// ---------------------------------------------------------------------------

/**
 * Built FIM context from a document + cursor position.
 *
 * llm-ls expects:
 *   - prefix: text before cursor, with each line reversed and joined.
 *     (This is a quirk of how llm-ls builds its prompt — it reverses the
 *      prefix line-by-line so the model sees the most recent characters first.)
 *   - suffix: text after cursor, forward line-by-line.
 *   - middle: left empty for llm-ls to fill from the tokenizer config.
 *
 * The prefix and suffix are built from the raw document text.
 */
struct FimContext {
    std::string prefix;     ///< reversed-prefix, line-by-line
    std::string suffix;     ///< forward-suffix, line-by-line
    std::string middle;     ///< empty (llm-ls fills from tokenizer)
};

/**
 * Build FIM context (prefix/suffix/middle) from the document and cursor.
 *
 * @param documentText  Full document text.
 * @param line          0-based cursor line.
 * @param character     0-based cursor character offset on the line.
 * @return FimContext with prefix (reversed, line-by-line), suffix (forward),
 *         and empty middle.
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
     */
    std::optional<AcceptCompletionParams> onAccept();

    /**
     * Called when the user dismisses or modifies the ghost text.
     * Clears the active ghost and sends a reject notification.
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

    /**
     * Get the current editor context (for building requests).
     * Used by onTimeout/onProviderFailure to know what was being requested.
     */
    const GhostContext& currentContext() const noexcept { return currentCtx_; }

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

private:
    // -----------------------------------------------------------------------
    // Config
    // -----------------------------------------------------------------------
    bool  enabled_     = false;
    int   debounceMs_  = 300;
    int   timeoutMs_   = 5000;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    GhostContext currentCtx_;
    int         revision_         = 0;
    int64_t     lastChangeTimeMs_ = 0;  ///< ms since epoch of last editor change
    bool        debouncePending_  = false;

    /// The current in-flight request (requestId + revision at time of request).
    struct PendingRequest {
        std::string requestId;
        int         revision;
        int64_t     sentAtMs;
    };
    std::optional<PendingRequest> pendingRequest_;

    /// The currently displayed ghost text (cleared on cursor move, accept,
    /// reject, or timeout).
    struct ActiveGhost {
        GhostResult   result;
        std::string   requestId;
        int           revision;
    };
    std::optional<ActiveGhost> activeGhost_;
};

} // namespace hathor::lsp
