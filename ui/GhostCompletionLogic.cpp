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

    // J-1: Trigger policy — decide whether to even start the debounce cycle.
    // If the cursor is in a string/comment, mid-token, or the context is
    // a duplicate, do NOT start the debounce so the timer tick won't fire
    // a request. We still update the context and clear any active ghost
    // (the cursor moved, so a previous ghost at a different position is stale).
    auto decision = policy_.shouldTrigger(
        ctx, lastRequestedCtx_, deterministicPopupActive_, pendingRequest_.has_value());

    // Always update the current context snapshot.
    currentCtx_ = ctx;
    ++revision_;
    lastChangeTimeMs_ = nowMs;

    // Clear any active ghost — the cursor moved so a previous ghost is stale.
    activeGhost_.reset();

    if (!decision.shouldTrigger)
    {
        // Policy says suppress — don't start the debounce cycle.
        debouncePending_ = false;
        return std::nullopt;
    }

    // Policy says allow — start (or restart) the debounce cycle.
    debouncePending_ = true;
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

        // J-2: Request a bounded number of candidate completions from the LLM
        // so the user can cycle through alternatives without re-requesting.
        req.maxCandidates = maxCandidates_;

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

        // J-1: Record this context so future onEditorChanged calls with the
        // same document text + cursor position are detected as duplicates.
        lastRequestedCtx_ = currentCtx_;

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

    // 5. Extract docPrefix/docSuffix before clearing pending request
    //    (pending is a reference to the soon-to-be-reset optional)
    std::string docPrefix = pending.docPrefix;
    std::string docSuffix = pending.docSuffix;
    std::string respRequestId = pending.requestId;

    // 5. Clear the pending request
    pendingRequest_.reset();

    // 5. Extract ALL completions from the response (J-2: multiple candidates).
    //    Each completion is independently trimmed against the FIM suffix/prefix.
    std::vector<GhostResult> candidates;
    candidates.reserve(std::min(response.completions.size(),
                                static_cast<size_t>(maxCandidates_)));

    for (size_t i = 0; i < response.completions.size(); ++i)
    {
        if (maxCandidates_ > 0 && i >= maxCandidates_)
            break;

        std::string text = response.completions[i].generatedText;
        stripWhitespace(text);
        trimPrefixOverlap(text, docPrefix);
        trimSuffixOverlap(text, docSuffix);
        stripWhitespace(text);

        if (text.empty())
            continue;

        GhostResult result;
        result.text = text;
        result.displayText = text;
        result.insertText = text;
        result.requestId = respRequestId;
        result.docPrefix = docPrefix;
        result.docSuffix = docSuffix;
        result.cursorLine = currentCtx_.line;
        result.character = currentCtx_.character;
        result.candidateIndex = static_cast<uint32_t>(i);

        candidates.push_back(std::move(result));
    }

    if (candidates.empty())
        return std::nullopt;

    // Store as active ghost with all candidates cached (J-2).
    activeGhost_ = ActiveGhost{
        std::move(candidates),
        respRequestId,
        revision_,
        0  // selectedIndex starts at first candidate
    };

    // Return the currently selected candidate for display.
    return activeGhost_->candidates[activeGhost_->selectedIndex];
}

std::optional<GhostResult> GhostCompletionLogic::onProviderFailure()
{
    // Clear any pending request
    pendingRequest_.reset();

    // Return the currently selected candidate to be cleared (if any)
    if (activeGhost_.has_value())
    {
        auto ghost = activeGhost_->candidates[activeGhost_->selectedIndex];
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

    // J-2: Report which candidate was accepted and all candidates that
    // were shown to the user (for llm-ls feedback).
    size_t selected = activeGhost_->selectedIndex;
    params.acceptedCompletion = static_cast<uint32_t>(
        activeGhost_->candidates[selected].candidateIndex);
    for (const auto& c : activeGhost_->candidates)
        params.shownCompletions.push_back(c.candidateIndex);

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

    // J-2: Report all candidate indices that were shown to the user.
    for (const auto& c : activeGhost_->candidates)
        params.shownCompletions.push_back(c.candidateIndex);

    activeGhost_.reset();
    pendingRequest_.reset();

    return params;
}

    void GhostCompletionLogic::clearActiveGhost() noexcept
{
    activeGhost_.reset();
    pendingRequest_.reset();
}

void GhostCompletionLogic::setDeterministicPopupActive(bool active) noexcept
{
    if (deterministicPopupActive_ == active)
        return;

    deterministicPopupActive_ = active;

    if (active)
    {
        // AI-G5: A deterministic popup is now visible — cancel any in-flight
        // ghost request and clear any active ghost so the popup takes precedence.
        cancelPendingRequest();
        activeGhost_.reset();
        debouncePending_ = false;
    }
    else
    {
        // Popup dismissed — allow the debounce cycle to resume on the next
        // editor change / timer tick. Clear lastRequestedCtx_ so we don't
        // suppress a fresh trigger at the same position.
        lastRequestedCtx_.reset();
    }
}

void GhostCompletionLogic::cancelPendingRequest() noexcept
{
    pendingRequest_.reset();
    debouncePending_ = false;
    // J-1: Clear the last requested context so a subsequent editor change
    // at the same position is NOT treated as a duplicate (the user may
    // want to re-trigger after a cancel, e.g. via Ctrl+Shift+Space).
    lastRequestedCtx_.reset();
}

// ---------------------------------------------------------------------------
// J-2: Candidate cycling API
// ---------------------------------------------------------------------------

size_t GhostCompletionLogic::candidateCount() const noexcept
{
    if (!activeGhost_.has_value())
        return 0;
    return activeGhost_->candidates.size();
}

size_t GhostCompletionLogic::selectedCandidateIndex() const noexcept
{
    if (!activeGhost_.has_value() || activeGhost_->candidates.empty())
        return 0;
    return activeGhost_->selectedIndex;
}

std::optional<GhostResult> GhostCompletionLogic::selectedCandidate() const noexcept
{
    if (!activeGhost_.has_value() || activeGhost_->candidates.empty())
        return std::nullopt;
    return activeGhost_->candidates[activeGhost_->selectedIndex];
}

bool GhostCompletionLogic::selectNextCandidate() noexcept
{
    if (!activeGhost_.has_value() || activeGhost_->candidates.empty())
        return false;

    size_t count = activeGhost_->candidates.size();
    if (count <= 1)
        return false;

    // Wrapping behaviour: cycle from last to first (wraps).
    activeGhost_->selectedIndex = (activeGhost_->selectedIndex + 1) % count;
    return true;
}

bool GhostCompletionLogic::selectPreviousCandidate() noexcept
{
    if (!activeGhost_.has_value() || activeGhost_->candidates.empty())
        return false;

    size_t count = activeGhost_->candidates.size();
    if (count <= 1)
        return false;

    // Wrapping behaviour: cycle from first to last (wraps).
    if (activeGhost_->selectedIndex == 0)
        activeGhost_->selectedIndex = count - 1;
    else
        activeGhost_->selectedIndex--;
    return true;
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

    // J-2: Request a bounded number of candidate completions.
    req.maxCandidates = kDefaultMaxCandidates;

    return req;
}

} // namespace hathor::lsp
