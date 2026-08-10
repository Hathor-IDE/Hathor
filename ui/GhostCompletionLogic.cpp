// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
  * GhostCompletionLogic.cpp — JUCE-free implementation of ghost-text lifecycle.
  *
  * Handles debounce, revision tracking for stale-response rejection,
  * latency timeouts (client-side cancellation since llm-ls lacks $/cancelRequest),
  * FIM prompt building (AI-G2: explicit prefix/suffix computation), and
  * GhostResult extraction with suffix-overlap trimming.
  *
  * AI-G2 (Fill-in-the-Middle as a First-Class Requirement):
  *   - buildFimContext() computes the actual document prefix/suffix from the
  *     cursor position.
  *   - onGhostResponse() trims the generated MIDDLE against the document suffix
  *     to ensure only the missing middle is inserted — the suffix is never
  *     silently discarded.
  *   - The AI-8 authoring context is included as additional FIM context.
  *
  * FIM generation is entirely outside the JUCE real-time audio thread — all
  * logic here runs on the JUCE message thread (via UITimer/ghostTick).
  *
  * Requirement references: AI-2, AI-3, AI-4, AI-8 §4, §7, AI-G1, AI-G2
  */

#include "GhostCompletionLogic.hpp"
#include "GhostJsonRpc.hpp"

#include <algorithm>
#include <cctype>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// FIM context builder
// ---------------------------------------------------------------------------

namespace {

/// Trim leading/trailing whitespace (space, tab, newline, carriage return).
void stripWhitespace(std::string& s)
{
    while (!s.empty() &&
           (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r'))
        s.erase(0, 1);
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
}

} // anonymous namespace

FimContext buildFimContext(std::string_view documentText, int line, int character)
{
    FimContext result;

    // Compute the byte offset of the cursor position within the document.
    // Lines are split on '\n'. Character is a UTF-8 byte offset within the line.
    std::size_t offset = 0;
    int currentLine = 0;
    bool found = false;
    for (std::size_t i = 0; i < documentText.size(); ++i)
    {
        if (currentLine == line)
        {
            offset += static_cast<std::size_t>(character);
            found = true;
            break;
        }
        if (documentText[i] == '\n')
        {
            ++currentLine;
            offset = i + 1;
        }
    }
    // If we didn't find the line in the loop, check if we're on the last
    // line (no trailing newline) or beyond the document.
    if (!found)
    {
        if (currentLine == line)
            offset += static_cast<std::size_t>(character);
        else
            offset = documentText.size();
    }
    // Clamp to content size.
    if (offset > documentText.size())
        offset = documentText.size();

    // docPrefix: everything before the cursor (from start of document to cursor).
    result.docPrefix = std::string(documentText.substr(0, offset));

    // docSuffix: everything after the cursor (from cursor to end of document).
    result.docSuffix = std::string(documentText.substr(offset));

    // middle: empty — llm-ls fills from tokenizer.
    result.middle.clear();

    return result;
}

// ---------------------------------------------------------------------------
// FIM response trimming helpers
// ---------------------------------------------------------------------------

namespace {

/// Trim prefix-overlap from the generated text.
///
/// The LLM may repeat the end of the document prefix as part of its output.
/// This function finds the longest overlapping suffix of `docPrefix` that
/// matches a prefix of `text`, and strips it, so the result is only the MIDDLE.
void trimPrefixOverlap(std::string& text, const std::string& docPrefix)
{
    if (docPrefix.empty() || text.empty())
        return;

    const std::size_t maxOverlap = std::min(docPrefix.size(), text.size());
    std::size_t bestOverlap = 0;
    for (std::size_t overlap = 1; overlap <= maxOverlap; ++overlap)
    {
        std::string_view prefixTail =
            std::string_view(docPrefix).substr(docPrefix.size() - overlap);
        std::string_view textHead =
            std::string_view(text).substr(0, overlap);
        if (prefixTail == textHead)
            bestOverlap = overlap;
    }
    if (bestOverlap > 0)
        text.erase(0, bestOverlap);
}

/// Trim suffix-overlap from the generated text.
///
/// The LLM may include the beginning of the document suffix as part of its
/// output. This function finds the longest overlapping prefix of `docSuffix`
/// that matches a suffix of `text`, and strips it, so the result is only the
/// MIDDLE that fits between prefix and suffix.
void trimSuffixOverlap(std::string& text, const std::string& docSuffix)
{
    if (docSuffix.empty() || text.empty())
        return;

    const std::size_t maxOverlap = std::min(docSuffix.size(), text.size());
    std::size_t bestOverlap = 0;
    for (std::size_t overlap = 1; overlap <= maxOverlap; ++overlap)
    {
        std::string_view suffixHead =
            std::string_view(docSuffix).substr(0, overlap);
        std::string_view textTail =
            std::string_view(text).substr(text.size() - overlap);
        if (suffixHead == textTail)
            bestOverlap = overlap;
    }
    if (bestOverlap > 0)
        text.erase(text.size() - bestOverlap);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// GhostCompletionLogic implementation
// ---------------------------------------------------------------------------

std::optional<std::pair<GhostCompletionRequest, std::string>>
GhostCompletionLogic::onEditorChanged(const GhostContext& ctx, int64_t nowMs)
{
    if (!enabled_)
        return std::nullopt;

    // Update the current context and revision
    currentCtx_ = ctx;
    ++revision_;
    lastChangeTimeMs_ = nowMs;
    debouncePending_ = true;

    // Clear any active ghost — the document changed while it was showing
    activeGhost_.reset();

    // Don't request immediately — wait for debounce to expire
    return std::nullopt;
}

std::optional<std::pair<GhostCompletionRequest, std::string>>
GhostCompletionLogic::onTimerTick(int64_t nowMs)
{
    if (!enabled_)
        return std::nullopt;

    // Check for timeout on in-flight request
    if (pendingRequest_.has_value())
    {
        if (nowMs - pendingRequest_->sentAtMs > static_cast<int64_t>(timeoutMs_))
        {
            // Timeout — clear pending request and debounce state so we
            // don't immediately retry; wait for the next editor change.
            pendingRequest_.reset();
            debouncePending_ = false;
        }
        // Request still in flight (or just timed out) — wait
        return std::nullopt;
    }

    // No in-flight request — check if debounce has expired
    if (debouncePending_ &&
        nowMs - lastChangeTimeMs_ >= static_cast<int64_t>(debounceMs_))
    {
        debouncePending_ = false;

        // Build explicit FIM document context from the current cursor position.
        // AI-G2: prefix/suffix are explicitly computed, not discarded.
        auto fim = buildFimContext(currentCtx_.documentText,
                                   currentCtx_.line,
                                   currentCtx_.character);

        GhostCompletionRequest req;
        req.uri = currentCtx_.uri;
        req.languageId = currentCtx_.languageId;
        req.line = currentCtx_.line;
        req.character = currentCtx_.character;
        req.textDocument = currentCtx_.documentText;

        // AI-G2: Explicitly preserve document prefix/suffix in the request.
        req.docPrefix = std::move(fim.docPrefix);
        req.docSuffix = std::move(fim.docSuffix);

        // AI-8: Include authoring context as additional FIM context (fim.prefix).
        // This carries the supported-surface info (AI-3), diagnostics, and
        // other relevant project/language information. llm-ls prepends
        // fim.prefix to the document prefix in the FIM prompt.
        req.fim.enabled = true;
        if (!currentCtx_.authoringContext.is_null())
            req.fim.prefix = currentCtx_.authoringContext.dump();
        req.fim.middle = std::move(fim.middle);

        // Store authoring context on the request for inspection
        req.authoringContext = currentCtx_.authoringContext;

        // Generate a request ID (will be matched in onGhostResponse)
        std::string requestId = GhostJsonRpc::generateRequestId();

        // Snapshot the docPrefix/docSuffix at request time for response trimming.
        // AI-G2: The pending request carries the exact prefix/suffix context
        // that was used to generate this request, so that onGhostResponse
        // can trim the generated MIDDLE correctly.
        pendingRequest_ = PendingRequest{
            requestId, revision_, nowMs,
            req.docPrefix, req.docSuffix
        };

        return std::make_optional(std::make_pair(req, requestId));
    }

    return std::nullopt;
}

std::optional<GhostResult> GhostCompletionLogic::onGhostResponse(
    const std::string& requestId,
    const GhostCompletionResponse& response,
    int64_t nowMs)
{
    if (!pendingRequest_.has_value())
        return std::nullopt;

    const auto& pending = pendingRequest_.value();

    // 1. Check request ID match (both the transport requestId and the
    //    llm-ls response field must match the pending request)
    if (pending.requestId != requestId || pending.requestId != response.request_id)
        return std::nullopt;

    // 2. Check revision match (stale response rejection)
    if (pending.revision != revision_)
    {
        // Response is stale — the editor changed since this request was sent.
        // Clear the pending request so onTimerTick can send a fresh one.
        pendingRequest_.reset();
        return std::nullopt;
    }

    // 3. Check timeout
    if (nowMs - pending.sentAtMs > static_cast<int64_t>(timeoutMs_))
    {
        pendingRequest_.reset();
        return std::nullopt;
    }

    // 4. Extract docPrefix/docSuffix before clearing pending request
    //    (pending is a reference to the soon-to-be-reset optional)
    std::string docPrefix = pending.docPrefix;
    std::string docSuffix = pending.docSuffix;
    std::string respRequestId = pending.requestId;

    // 5. Clear the pending request
    pendingRequest_.reset();

    // 5. Extract the best completion from the response
    if (response.completions.empty())
        return std::nullopt;

    // Use the first completion (llm-ls typically returns one)
    const auto& comp = response.completions[0];
    std::string text = comp.generatedText;

    // Trim whitespace from the generated text
    // llm-ls may return text with leading/trailing whitespace
    stripWhitespace(text);

    // AI-G2: Trim prefix/suffix overlap from the generated text.
    // The LLM may repeat the end of the document prefix or include the
    // beginning of the document suffix. We trim these so only the MIDDLE
    // (the code that fits between prefix and suffix) is inserted at the cursor.
    // The suffix is NEVER silently discarded — it is explicitly used for trimming.
    trimPrefixOverlap(text, docPrefix);
    trimSuffixOverlap(text, docSuffix);

    // Strip whitespace again after trimming
    stripWhitespace(text);

    if (text.empty())
        return std::nullopt;

    // Build the GhostResult
    GhostResult result;
    result.text = text;
    result.displayText = text;
    result.insertText = text;
    result.requestId = respRequestId;
    result.cursorLine = currentCtx_.line;
    result.character = currentCtx_.character;

    // AI-G2: Carry the document prefix/suffix in the result for editor
    // verification. The editor can check that the ghost text still fits
    // between these markers before displaying or inserting it.
    result.docPrefix = docPrefix;
    result.docSuffix = docSuffix;

    // Store as active ghost
    activeGhost_ = ActiveGhost{result, respRequestId, revision_};

    return result;
}

std::optional<GhostResult> GhostCompletionLogic::onProviderFailure()
{
    // Clear any pending request
    pendingRequest_.reset();

    // Return the active ghost to be cleared (if any)
    if (activeGhost_.has_value())
    {
        auto ghost = activeGhost_->result;
        activeGhost_.reset();
        return ghost;
    }

    return std::nullopt;
}

std::optional<AcceptCompletionParams> GhostCompletionLogic::onAccept()
{
    if (!activeGhost_.has_value())
        return std::nullopt;

    AcceptCompletionParams params;
    params.requestId = activeGhost_->requestId;
    // llm-ls track which completion was accepted and which were shown.
    // We only track a single ghost completion per request cycle.
    params.acceptedCompletion = 0;
    params.shownCompletions = {0};

    activeGhost_.reset();
    pendingRequest_.reset();

    return params;
}

std::optional<RejectCompletionParams> GhostCompletionLogic::onReject()
{
    if (!activeGhost_.has_value())
        return std::nullopt;

    RejectCompletionParams params;
    params.requestId = activeGhost_->requestId;
    params.shownCompletions = {0};

    activeGhost_.reset();
    pendingRequest_.reset();

    return params;
}

void GhostCompletionLogic::clearActiveGhost() noexcept
{
    activeGhost_.reset();
    pendingRequest_.reset();
}

void GhostCompletionLogic::cancelPendingRequest() noexcept
{
    pendingRequest_.reset();
    debouncePending_ = false;
}

// ---------------------------------------------------------------------------
// Static: build a complete GhostCompletionRequest
// ---------------------------------------------------------------------------

GhostCompletionRequest GhostCompletionLogic::buildRequest(
    const GhostContext& ctx,
    const GhostProviderConfig& config)
{
    GhostCompletionRequest req;
    req.uri = ctx.uri;
    req.languageId = ctx.languageId;
    req.line = ctx.line;
    req.character = ctx.character;
    req.textDocument = ctx.documentText;

    // Build FIM context
    auto fim = buildFimContext(ctx.documentText, ctx.line, ctx.character);
    req.docPrefix = std::move(fim.docPrefix);
    req.docSuffix = std::move(fim.docSuffix);

    // AI-8: Include authoring context as additional FIM context
    req.authoringContext = ctx.authoringContext;
    req.fim.enabled = true;
    if (!ctx.authoringContext.is_null())
        req.fim.prefix = ctx.authoringContext.dump();
    req.fim.middle = std::move(fim.middle);

    // Backend config
    req.backend.backend = config.backend;
    req.backend.url = config.url;
    req.backend.model = config.model;

    // Credentials (per-request, never persisted)
    req.apiToken = config.apiToken;

    // Generation parameters
    req.tokenizerConfig = config.tokenizerConfig;
    req.contextWindow = config.contextWindow;
    req.tlsSkipVerify = config.tlsSkipVerify;

    return req;
}

} // namespace hathor::lsp
