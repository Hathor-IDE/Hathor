// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
  * GhostTriggerPolicy.hpp — JUCE-free, configurable trigger policy for ghost-text
  * (LLM inline completion) requests.
  *
  * This policy decides *whether* a ghost completion request should be issued
  * at the current cursor position and document state. It runs entirely on the
  * JUCE message thread (via handleCursorMove / ghostTick), never on the JUCE
  * real-time audio thread. All language/context detection is performed on
  * raw text + line/character positions — no JUCE dependencies.
  *
  * Suppression conditions (the policy returns shouldTrigger=false):
  *   - The cursor is inside a string literal
  *   - The cursor is inside a comment (ChucK: line and block comments)
  *   - The user is actively typing through the middle of a token/word
  *   - The surrounding construct is syntactically incomplete enough that
  *     completion would be unreliable (unclosed string/comment on the line)
  *   - A deterministic completion popup is already active (AI-G5 precedence)
  *   - An equivalent request for the same document/context is already pending
  *   - The editor state has not changed since the last request (duplicate)
  *   - The cursor is not at a meaningful completion boundary
  *
     * Trigger conditions (the policy returns shouldTrigger=true; actual request
     * is still debounced by GhostCompletionLogic):
     *   - After '('
     *   - After '.'
     *   - After the pattern patching arrow operator (Unicode arrow)
     *   - After '=>' (ChucK chucking operator)
     *   - After a space following a completed word/token
     *   - At the end of a token
     *   - At the start of the document (empty file)
     *   - After delimiters: '{'; '}', '[', ']', ';', ',', ')'
     *
     * Suppression when a non-empty selection is active (J-4):
     *   Ghost completion is a zero-width-insertion-point affordance. When the
     *   user has a non-empty selection, the intent is to replace/move/copy
     *   text, not to complete — so the policy suppresses. Selection state
     *   arrives via GhostContext.hasSelection / selectedText, which is
     *   populated from the same editor snapshot as the AI-8 authoring context
     *   (no second context model).
     *
     * Requirement references: J-1, J-4, AI-G1, AI-G5, AI-G6
  */

#include "GhostProtocol.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// TriggerDecision — result of the trigger policy evaluation
// ---------------------------------------------------------------------------

/**
  * The result of evaluating the trigger policy for a given editor context.
  *
  * `shouldTrigger` is true when the policy believes a ghost completion
  * request *may* be issued (subject to debounce + no-pending-request).
  * `reason` is a human-readable diagnostic string for logging/debugging.
  */
struct TriggerDecision {
    bool        shouldTrigger;
    std::string reason;

    static TriggerDecision allow(std::string_view r = "")
    {
        return {true, std::string(r)};
    }
    static TriggerDecision suppress(std::string_view r = "")
    {
        return {false, std::string(r)};
    }
};

// ---------------------------------------------------------------------------
// GhostTriggerPolicyConfig — tunable parameters
// ---------------------------------------------------------------------------

/**
  * Configuration for the ghost completion trigger policy.
  *
  * All fields are simple scalars/booleans so they can be exposed as
  * preferences in the Settings tab without restructuring the architecture.
  */
struct GhostTriggerPolicyConfig {
    /** Minimum number of characters in the current word before triggering.
      * If the cursor is at the end of a partial word shorter than this,
      * we suppress to avoid triggering on the very first keystroke. */
    int minWordLengthForTrigger = 2;

    /** Allow ghost completion inside string literals. Usually false. */
    bool allowInStrings = false;

    /** Allow ghost completion inside comments. Usually false. */
    bool allowInComments = false;

    /** Whether ghost should trigger at an empty document (0-length text). */
    bool allowOnEmptyDocument = true;
};

// ---------------------------------------------------------------------------
// GhostTriggerPolicy — the policy engine
// ---------------------------------------------------------------------------

/**
  * GhostTriggerPolicy
  *
  * A pure-logic, JUCE-free component that decides whether a ghost completion
  * request should be issued for a given editor context. It is designed to be
  * owned by GhostCompletionLogic (which owns the debounce timer and revision
  * tracking) and configured via GhostTriggerPolicyConfig.
  *
  * The policy is stateless with respect to editor events — it evaluates
  * each call independently based on the provided context and the last-requested
  * context. This makes it trivially unit-testable.
  *
  * Thread safety: all methods are const and operate only on their arguments.
  * Safe to call from the JUCE message thread only (the GhostCompletionLogic
  * that owns it is message-thread-only). The audio thread never reaches this
  * code path.
  *
  * Requirement references: J-1, AI-G1, AI-G5, AI-G6
  */
class GhostTriggerPolicy
{
public:
    GhostTriggerPolicy() = default;
    ~GhostTriggerPolicy() = default;

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    /** Set the tunable configuration. */
    void setConfig(const GhostTriggerPolicyConfig& c) noexcept { config_ = c; }

    /** Read the current configuration. */
    const GhostTriggerPolicyConfig& config() const noexcept { return config_; }

    // -----------------------------------------------------------------------
    // Main evaluation entry point
    // -----------------------------------------------------------------------

    /**
      * Evaluate whether a ghost completion request should be issued for the
      * given editor context.
      *
      * @param ctx                    Current editor snapshot.
      * @param lastRequestedCtx       The context from the last request that was
      *                               actually sent (for duplicate detection).
      * @param deterministicPopupActive  True if a deterministic (LSP/metadata)
      *                                  completion popup is currently visible.
      * @param hasPendingRequest      True if a ghost request is already in-flight.
      * @return TriggerDecision — allow or suppress with a diagnostic reason.
      */
    TriggerDecision shouldTrigger(
        const GhostContext& ctx,
        const std::optional<GhostContext>& lastRequestedCtx,
        bool deterministicPopupActive,
        bool hasPendingRequest) const;

    // -----------------------------------------------------------------------
    // Language-aware checks (public for unit testing)
    // -----------------------------------------------------------------------

    /** True if the cursor is inside a string literal at the given context.
      * Works for both ChucK ("..." and '...') and mini-notation ("...").
      */
    bool isInStringLiteral(const GhostContext& ctx) const;

    /** True if the cursor is inside a comment at the given context.
      * ChucK supports: line comments and block comments. Mini-notation has no comments.
      */
    bool isInComment(const GhostContext& ctx) const;

    /** True if the cursor is in the middle of a word/token (i.e., word
      * characters appear on BOTH sides of the cursor). This means the
      * user is still typing the token and ghost completion would fire
      * on every keystroke.
      */
    bool isMidToken(const GhostContext& ctx) const;

    /** True if the cursor is at a position where ghost completion is
      * meaningful: after an operator, at end of a token, after a space
      * following a word, or at a syntactically complete boundary.
      */
    bool isAtMeaningfulBoundary(const GhostContext& ctx) const;

    /** True if the context is a duplicate of the last requested context
      * (same document text, same cursor position, same language).
      */
    bool isDuplicateContext(
        const GhostContext& ctx,
        const std::optional<GhostContext>& lastRequestedCtx) const;

    /** True if the surrounding construct is too syntactically incomplete
      * for reliable completion (e.g., unclosed string or comment on the
      * current line before the cursor).
      */
    bool isSyntacticallyUnreliable(const GhostContext& ctx) const;

private:
    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    /** Convert a (line, character) to a byte offset in the document. */
    static std::size_t cursorToOffset(std::string_view documentText,
                                      int line, int character) noexcept;

    /** Get the text of the line containing the cursor (0-based line index). */
    static std::string_view getLineText(std::string_view documentText, int line) noexcept;

    /** True if c is a word character: [A-Za-z0-9_] or Unicode letter/digit. */
    static bool isWordChar(char c) noexcept;

    /** True if c is a ChucK/mini-notation identifier character. */
    static bool isIdentChar(char c) noexcept;

    /** Scan backwards from offset on the line to determine if the cursor
      * is inside a string literal. Returns the delimiter char if so, '\0' otherwise.
      * For ChucK: supports "..." and '...'. For mini-notation: "..." and '...'.
      */
    char checkStringAt(std::string_view lineText, std::size_t cursorInLine) const;

    /** Scan backwards from offset on the line to determine if the cursor
      * is inside a comment. Only checks for single-line comments and
      * block comments on the same line. Does NOT scan across lines
      * for performance.
      */
    bool checkCommentAt(std::string_view lineText, std::size_t cursorInLine,
                        std::string_view languageId) const;

    GhostTriggerPolicyConfig config_;
};

} // namespace hathor::lsp
