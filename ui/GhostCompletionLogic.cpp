// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * GhostCompletionLogic.cpp — JUCE-free implementation of ghost-text lifecycle.
 *
 * Handles debounce, revision tracking for stale-response rejection,
 * latency timeouts (client-side cancellation since llm-ls lacks $/cancelRequest),
 * FIM prompt building, and GhostResult extraction.
 *
 * Requirement references: AI-4, AI-8 §4, §7
 */

#include "GhostCompletionLogic.hpp"

#include <algorithm>
#include <cctype>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// FIM context builder
// ---------------------------------------------------------------------------

FimContext buildFimContext(std::string_view documentText, int line, int character)
{
    FimContext result;

    // Clamp line to valid range
    int totalLines = 1;
    for (size_t i = 0; i < documentText.size(); ++i)
    {
        if (documentText[i] == '\n')
            ++totalLines;
    }
    line = std::clamp(line, 0, totalLines - 1);

    // Extract the current line text (0-based line from '\n' splitting)
    int currentLineStart = 0;
    int currentLineNum = 0;
    for (size_t i = 0; i <= documentText.size(); ++i)
    {
        if (currentLineNum == line)
        {
            currentLineStart = static_cast<int>(i);
            break;
        }
        if (i < documentText.size() && documentText[i] == '\n')
        {
            ++currentLineNum;
        }
    }

    // Find end of current line
    int currentLineEnd = currentLineStart;
    while (currentLineEnd < static_cast<int>(documentText.size()) &&
           documentText[currentLineEnd] != '\n')
    {
        ++currentLineEnd;
    }

    // Clamp character to line bounds
    character = std::clamp(character, 0, currentLineEnd - currentLineStart);

    // --- Build prefix (reversed, line-by-line) ---
    // llm-ls reverses each line of the prefix so the model sees the most
    // recent characters first, improving FIM completion quality.
    //
    // We take all text from (0,0) up to (line, character), split by lines,
    // reverse each line, and join.
    {
        std::string prefixText;
        prefixText.reserve(static_cast<size_t>(currentLineStart + character));

        // Lines before the cursor line (in reverse order, reversed text)
        int pos = currentLineStart - 1;
        int lineNum = line;
        while (lineNum > 0 && pos >= 0)
        {
            // Find the start of the previous line
            int nextLineStart = pos;
            while (pos >= 0 && documentText[pos] != '\n')
                --pos;
            // pos is at '\n' or -1
            int lineStart = pos + 1;
            int lineEnd = nextLineStart;

            // Extract and reverse this line
            std::string_view lineView = documentText.substr(
                static_cast<size_t>(lineStart),
                static_cast<size_t>(lineEnd - lineStart));

            std::string reversed(lineView.rbegin(), lineView.rend());
            prefixText += reversed;
            prefixText += '\n';  // line separator (llm-ls uses \n between reversed lines)

            --lineNum;
            --pos;
        }

        // Current line prefix (before cursor), reversed
        std::string_view currentPrefix = documentText.substr(
            static_cast<size_t>(currentLineStart),
            static_cast<size_t>(character));
        std::string reversedCurrent(currentPrefix.rbegin(), currentPrefix.rend());
        prefixText += reversedCurrent;

        result.prefix = std::move(prefixText);
    }

    // --- Build suffix (forward, line-by-line) ---
    // Text after cursor on the current line, then subsequent lines.
    {
        std::string suffixText;

        // Rest of the current line (after cursor)
        int restLen = currentLineEnd - (currentLineStart + character);
        if (restLen > 0)
        {
            suffixText += std::string(documentText.substr(
                static_cast<size_t>(currentLineStart + character),
                static_cast<size_t>(restLen)));
        }

        // Subsequent lines
        int pos = currentLineEnd + 1; // skip the '\n'
        while (pos <= static_cast<int>(documentText.size()))
        {
            int lineEnd = pos;
            while (lineEnd < static_cast<int>(documentText.size()) &&
                   documentText[lineEnd] != '\n')
            {
                ++lineEnd;
            }

            int lineLen = lineEnd - pos;
            if (lineLen > 0)
            {
                suffixText += '\n';
                suffixText += std::string(documentText.substr(
                    static_cast<size_t>(pos),
                    static_cast<size_t>(lineLen)));
            }

            if (lineEnd >= static_cast<int>(documentText.size()))
                break;
            pos = lineEnd + 1;
        }

        result.suffix = std::move(suffixText);
    }

    // Middle is left empty — llm-ls fills from tokenizer config
    result.middle.clear();

    return result;
}

// ---------------------------------------------------------------------------
// GhostCompletionLogic implementation
// ---------------------------------------------------------------------------

std::optional<std::pair<GhostCompletionRequest, std::string>>
GhostCompletionLogic::onEditorChanged(const GhostContext& ctx)
{
    if (!enabled_)
        return std::nullopt;

    // Update the current context and revision
    currentCtx_ = ctx;
    ++revision_;
    lastChangeTimeMs_ = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count());
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
            // Timeout — clear pending request, don't send new one yet
            pendingRequest_.reset();
        }
    }

    // Check debounce
    if (debouncePending_ && pendingRequest_.has_value())
    {
        // Still have a pending request — wait for it to complete or timeout
        return std::nullopt;
    }

    if (debouncePending_ && !pendingRequest_.has_value())
    {
        // Debounce window expired
        if (nowMs - lastChangeTimeMs_ >= static_cast<int64_t>(debounceMs_))
        {
            debouncePending_ = false;

            GhostCompletionRequest req;
            req.uri = currentCtx_.uri;
            req.languageId = currentCtx_.languageId;
            req.line = currentCtx_.line;
            req.character = currentCtx_.character;
            req.textDocument = currentCtx_.documentText;

            // Build FIM context
            auto fim = buildFimContext(currentCtx_.documentText,
                                       currentCtx_.line,
                                       currentCtx_.character);
            req.fim.enabled = true;
            req.fim.prefix = std::move(fim.prefix);
            req.fim.suffix = std::move(fim.suffix);
            req.fim.middle = std::move(fim.middle);

            // Generate a request ID (will be matched in onGhostResponse)
            std::string requestId = GhostJsonRpc::generateRequestId();

            pendingRequest_ = PendingRequest{
                requestId, revision_, nowMs
            };

            return std::make_optional(std::make_pair(req, requestId));
        }
    }

    return std::nullopt;
}

std::optional<GhostResult> GhostCompletionLogic::onGhostResponse(
    const std::string& requestId,
    const GhostCompletionResponse& response)
{
    if (!pendingRequest_.has_value())
        return std::nullopt;

    const auto& pending = pendingRequest_.value();

    // 1. Check request ID match
    if (pending.requestId != requestId)
        return std::nullopt;

    // 2. Check revision match (stale response rejection)
    if (pending.revision != revision_)
        return std::nullopt; // stale — editor changed since this request

    // 3. Check timeout
    int64_t nowMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count());
    if (nowMs - pending.sentAtMs > static_cast<int64_t>(timeoutMs_))
        return std::nullopt;

    // 4. Clear the pending request
    pendingRequest_.reset();

    // 5. Extract the best completion from the response
    if (response.completions.empty())
        return std::nullopt;

    // Use the first completion (llm-ls typically returns one)
    const auto& comp = response.completions[0];
    std::string text = comp.generatedText;

    // Trim whitespace from the generated text
    // llm-ls may return text with leading/trailing whitespace
    while (!text.empty() &&
           (text[0] == ' ' || text[0] == '\t' || text[0] == '\n' || text[0] == '\r'))
        text.erase(0, 1);
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

    if (text.empty())
        return std::nullopt;

    // Build the GhostResult
    GhostResult result;
    result.text = text;
    result.displayText = text;
    result.insertText = text;
    result.requestId = requestId;

    // Store as active ghost
    activeGhost_ = ActiveGhost{result, requestId, revision_};

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
    params.uri = currentCtx_.uri;
    params.line = currentCtx_.line;
    params.character = currentCtx_.character;

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
    params.uri = currentCtx_.uri;

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
    req.fim.enabled = true;
    req.fim.prefix = std::move(fim.prefix);
    req.fim.suffix = std::move(fim.suffix);
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
