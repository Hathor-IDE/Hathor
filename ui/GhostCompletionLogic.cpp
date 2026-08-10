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
#include "GhostJsonRpc.hpp"

#include <algorithm>
#include <cctype>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// FIM context builder
// ---------------------------------------------------------------------------

FimContext buildFimContext(std::string_view documentText, int line, int character)
{
    FimContext result;

    // Split document into lines for clean boundary detection.
    std::vector<std::string_view> lines;
    size_t start = 0;
    size_t pos = 0;
    while ((pos = documentText.find('\n', start)) != std::string_view::npos)
    {
        lines.push_back(documentText.substr(start, pos - start));
        start = pos + 1;
    }
    lines.push_back(documentText.substr(start));

    if (lines.empty())
        return result;

    // Clamp line to valid range.
    if (line < 0)
        line = 0;
    if (line >= static_cast<int>(lines.size()))
        line = static_cast<int>(lines.size()) - 1;

    // Clamp character to line bounds.
    if (character < 0)
        character = 0;
    if (character >= static_cast<int>(lines[line].size()))
        character = static_cast<int>(lines[line].size());

    // Build prefix: reversed lines [0..line-1] in reverse order, then reversed
    // current-line prefix. llm-ls reverses each line so the model sees the
    // most recent characters first, improving FIM completion quality.
    {
        std::string prefixText;
        for (int i = line; i >= 0; --i)
        {
            if (i == line)
            {
                std::string_view linePrefix = lines[i].substr(0, static_cast<size_t>(character));
                prefixText += std::string(linePrefix.rbegin(), linePrefix.rend());
            }
            else
            {
                std::string reversed(lines[i].rbegin(), lines[i].rend());
                prefixText += reversed;
                prefixText += '\n';
            }
        }
        result.prefix = std::move(prefixText);
    }

    // Build suffix: rest of current line after cursor, then subsequent lines.
    {
        std::string suffixText;
        std::string_view lineSuffix = lines[line].substr(static_cast<size_t>(character));
        suffixText += std::string(lineSuffix);
        for (int i = line + 1; i < static_cast<int>(lines.size()); ++i)
        {
            suffixText += '\n';
            suffixText += lines[i];
        }
        result.suffix = std::move(suffixText);
    }

    // Middle is empty — llm-ls fills from tokenizer config.
    result.middle.clear();

    return result;
}

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
    result.cursorLine = currentCtx_.line;
    result.character = currentCtx_.character;

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
