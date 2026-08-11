// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
  * GhostTriggerPolicy.cpp — implementation of the JUCE-free ghost completion
  * trigger policy.
  *
  * Requirement references: J-1, AI-G1, AI-G5, AI-G6
  */

#include "GhostTriggerPolicy.hpp"

#include <algorithm>
#include <cctype>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// Helpers — cursor/line text extraction
// ---------------------------------------------------------------------------

std::size_t GhostTriggerPolicy::cursorToOffset(
    std::string_view documentText, int line, int character) noexcept
{
    if (line < 0 || character < 0)
        return 0;

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
    if (!found)
    {
        if (currentLine == line)
            offset += static_cast<std::size_t>(character);
        else
            offset = documentText.size();
    }
    if (offset > documentText.size())
        offset = documentText.size();
    return offset;
}

std::string_view GhostTriggerPolicy::getLineText(
    std::string_view documentText, int line) noexcept
{
    if (line < 0)
        return "";

    int currentLine = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < documentText.size(); ++i)
    {
        if (currentLine == line)
            break;
        if (documentText[i] == '\n')
        {
            ++currentLine;
            start = i + 1;
        }
    }
    if (currentLine != line)
        return "";

    std::size_t end = documentText.find('\n', start);
    if (end == std::string_view::npos)
        end = documentText.size();
    // Strip trailing \r for \r\n line endings.
    if (end > start && documentText[end - 1] == '\r')
        --end;
    return documentText.substr(start, end - start);
}

// ---------------------------------------------------------------------------
// Helpers — character classification
// ---------------------------------------------------------------------------

bool GhostTriggerPolicy::isWordChar(char c) noexcept
{
    // Match the delimiters used in LspCompletionLogic::findWordStart/End.
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
        return true;
    if (static_cast<unsigned char>(c) > 127)
        return true;
    return false;
}

bool GhostTriggerPolicy::isIdentChar(char c) noexcept
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// ---------------------------------------------------------------------------
// String literal detection
// ---------------------------------------------------------------------------

char GhostTriggerPolicy::checkStringAt(
    std::string_view lineText, std::size_t cursorInLine) const
{
    if (cursorInLine > lineText.size())
        cursorInLine = lineText.size();

    bool inString = false;
    char stringDelim = '\0';

    for (std::size_t i = 0; i < cursorInLine; ++i)
    {
        char c = lineText[i];
        if (inString)
        {
            if (c == '\\' && i + 1 < cursorInLine)
            {
                // Skip escaped character (e.g. \", \\)
                ++i;
                continue;
            }
            if (c == stringDelim)
            {
                inString = false;
                stringDelim = '\0';
            }
        }
        else
        {
            if (c == '"' || c == '\'')
            {
                inString = true;
                stringDelim = c;
            }
        }
    }

    if (inString)
        return stringDelim;
    return '\0';
}

bool GhostTriggerPolicy::isInStringLiteral(const GhostContext& ctx) const
{
    std::string_view lineText = getLineText(ctx.documentText, ctx.line);
    std::size_t cursorInLine = static_cast<std::size_t>(ctx.character);

    char delim = checkStringAt(lineText, cursorInLine);
    return delim != '\0';
}

// ---------------------------------------------------------------------------
// Comment detection
// ---------------------------------------------------------------------------

bool GhostTriggerPolicy::checkCommentAt(
    std::string_view lineText, std::size_t cursorInLine,
    std::string_view languageId) const
{
    if (cursorInLine > lineText.size())
        cursorInLine = lineText.size();

    // Mini-notation (.hathor) has no comments.
    if (languageId == "hathor" || languageId == "mininotation")
        return false;

    // ChucK comments: //, <--, /* */
    for (std::size_t i = 0; i < cursorInLine; ++i)
    {
        char c = lineText[i];

        // Skip strings so // inside a string doesn't start a comment.
        if (c == '"' || c == '\'')
        {
            char delim = c;
            ++i;
            while (i < cursorInLine && i < lineText.size())
            {
                if (lineText[i] == '\\' && i + 1 < lineText.size())
                {
                    i += 2;
                    continue;
                }
                if (lineText[i] == delim)
                    break;
                ++i;
            }
            continue;
        }

        // Line comment: //
        if (c == '/' && i + 1 < lineText.size() && lineText[i + 1] == '/')
            return true;

        // Line comment: <--
        if (c == '<' && i + 2 < lineText.size()
            && lineText[i + 1] == '-' && lineText[i + 2] == '-')
            return true;

        // Block comment: /* ... */
        if (c == '/' && i + 1 < lineText.size() && lineText[i + 1] == '*')
        {
            // Check if the block comment is closed before the cursor.
            i += 2;
            while (i < lineText.size())
            {
                if (i + 1 < lineText.size() && lineText[i] == '*' && lineText[i + 1] == '/')
                {
                    i += 2;
                    break; // Closed on this line — not in comment
                }
                if (i >= cursorInLine)
                    return true; // Cursor is inside an unclosed block comment
                ++i;
            }
            // If the block comment wasn't closed on this line, the cursor
            // is inside it (if the cursor is past the opening).
            if (i >= cursorInLine)
                return true;
        }
    }

    return false;
}

bool GhostTriggerPolicy::isInComment(const GhostContext& ctx) const
{
    std::string_view lineText = getLineText(ctx.documentText, ctx.line);
    std::size_t cursorInLine = static_cast<std::size_t>(ctx.character);
    return checkCommentAt(lineText, cursorInLine, ctx.languageId);
}

// ---------------------------------------------------------------------------
// Mid-token detection
// ---------------------------------------------------------------------------

bool GhostTriggerPolicy::isMidToken(const GhostContext& ctx) const
{
    std::string_view lineText = getLineText(ctx.documentText, ctx.line);
    std::size_t cursorInLine = static_cast<std::size_t>(ctx.character);

    if (cursorInLine == 0 || cursorInLine > lineText.size())
        return false; // At start of line — not mid-token

    // Check if the char before the cursor is a word/identifier char
    // AND the char at the cursor is also a word/identifier char.
    // If both are word chars, the user is typing through the middle of a word.
    char prev = lineText[cursorInLine - 1];
    char next = (cursorInLine < lineText.size()) ? lineText[cursorInLine] : '\0';

    if (prev == '\0' || next == '\0')
        return false;

    bool prevIsWord = isWordChar(prev);
    bool nextIsWord = isWordChar(next);

    if (prevIsWord && nextIsWord)
        return true;

    // Also check: if we're inside a partial identifier (the current word being
    // typed is shorter than minWordLength), treat as "still typing".
    // Walk backwards to find the start of the current word.
    std::size_t wordStart = cursorInLine;
    while (wordStart > 0 && isWordChar(lineText[wordStart - 1]))
        --wordStart;

    std::size_t wordLen = cursorInLine - wordStart;
    if (wordLen > 0 && static_cast<int>(wordLen) < config_.minWordLengthForTrigger)
        return true; // Too short a word to be a meaningful boundary

    return false;
}

// ---------------------------------------------------------------------------
// Syntactically unreliable (unclosed string/comment before cursor on current line)
// ---------------------------------------------------------------------------

bool GhostTriggerPolicy::isSyntacticallyUnreliable(const GhostContext& ctx) const
{
    // Unclosed string on the current line before the cursor → unreliable
    if (checkStringAt(getLineText(ctx.documentText, ctx.line),
                      static_cast<std::size_t>(ctx.character)) != '\0')
        return true;

    // Unclosed comment on the current line before the cursor → unreliable
    if (checkCommentAt(getLineText(ctx.documentText, ctx.line),
                       static_cast<std::size_t>(ctx.character),
                       ctx.languageId))
        return true;

    return false;
}

// ---------------------------------------------------------------------------
// Meaningful boundary detection
// ---------------------------------------------------------------------------

bool GhostTriggerPolicy::isAtMeaningfulBoundary(const GhostContext& ctx) const
{
    std::string_view lineText = getLineText(ctx.documentText, ctx.line);
    std::size_t cursorInLine = static_cast<std::size_t>(ctx.character);
    std::size_t lineLen = lineText.size();

    // Empty document → boundary (trigger for initial completion)
    if (ctx.documentText.empty())
        return config_.allowOnEmptyDocument;

    // End of document → boundary
    std::size_t absOffset = cursorToOffset(ctx.documentText, ctx.line, ctx.character);
    if (absOffset >= ctx.documentText.size())
        return true;

    // If cursor is past the end of the line text, it's at end of line → boundary
    if (cursorInLine >= lineLen)
        return true;

    char prev = (cursorInLine > 0) ? lineText[cursorInLine - 1] : '\0';
    char next = lineText[cursorInLine];

    // If prev is a word char and next is a word char → mid-token (not a boundary)
    // (This is also checked by isMidToken, but we double-check here for safety.)
    if (prev != '\0' && isWordChar(prev) && isWordChar(next))
        return false;

    // Trigger characters: (, ., →, =>
    if (prev == '(')
        return true;
    if (prev == '.')
        return true;
    // Unicode arrow → (U+2192 = 0xE2 0x86 0x92 in UTF-8)
    if (prev == '\xe2')
    {
        // Check if this is the start of → (3-byte UTF-8)
        if (cursorInLine >= 3)
        {
            char b1 = lineText[cursorInLine - 3];
            char b2 = lineText[cursorInLine - 2];
            char b3 = lineText[cursorInLine - 1];
            if (static_cast<unsigned char>(b1) == 0xE2
                && static_cast<unsigned char>(b2) == 0x86
                && static_cast<unsigned char>(b3) == 0x92)
                return true;
        }
    }
    // ChucK chucking: =>
    if (prev == '>' && cursorInLine >= 2
        && lineText[cursorInLine - 2] == '=')
        return true;

    // End of a token: prev is a word char, next is not → boundary
    if (prev != '\0' && isWordChar(prev) && !isWordChar(next))
        return true;

    // Space following a completed word: prev is whitespace, and walking
    // backwards past whitespace we find a word character.
    if (prev == ' ' || prev == '\t')
    {
        std::size_t i = cursorInLine;
        while (i > 0 && (lineText[i - 1] == ' ' || lineText[i - 1] == '\t'))
            --i;
        if (i > 0 && isWordChar(lineText[i - 1]))
            return true;
        // Space after only whitespace on the line — could be a boundary
        // (e.g., empty line with just whitespace after a newline).
        // Allow it to trigger so empty-line suggestions can appear.
        return true;
    }

    // After delimiters that suggest a new expression is starting
    if (prev == '{' || prev == '}' || prev == '[' || prev == ']'
        || prev == ';' || prev == ',' || prev == ')')
        return true;

    // After a newline (start of a new line) — allow ghost completion
    // on any non-blank line start.
    if (prev == '\n' || prev == '\0')
    {
        // If the line is entirely whitespace, it's still a boundary
        return true;
    }

    // For .hathor mini-notation, common pattern separators are valid triggers.
    // The space-separated nature means any non-word char that's not a string/comment
    // start is likely a boundary.

    return false;
}

// ---------------------------------------------------------------------------
// Duplicate context detection
// ---------------------------------------------------------------------------

bool GhostTriggerPolicy::isDuplicateContext(
    const GhostContext& ctx,
    const std::optional<GhostContext>& lastRequestedCtx) const
{
    if (!lastRequestedCtx.has_value())
        return false;

    const auto& last = lastRequestedCtx.value();

    // Same document text + same cursor position + same language → duplicate
    if (ctx.documentText == last.documentText
        && ctx.line == last.line
        && ctx.character == last.character
        && ctx.languageId == last.languageId)
    {
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Main evaluation
// ---------------------------------------------------------------------------

TriggerDecision GhostTriggerPolicy::shouldTrigger(
    const GhostContext& ctx,
    const std::optional<GhostContext>& lastRequestedCtx,
    bool deterministicPopupActive,
    bool hasPendingRequest) const
{
    // 1. Deterministic popup active → suppress (AI-G5 precedence)
    if (deterministicPopupActive)
        return TriggerDecision::suppress("deterministic popup active");

    // 2. Already a request in-flight → suppress duplicate (AI-G6)
    if (hasPendingRequest)
        return TriggerDecision::suppress("request already in-flight");

    // 5. Inside a string literal → suppress (unless allowInStrings)
    if (isInStringLiteral(ctx))
    {
        if (!config_.allowInStrings)
            return TriggerDecision::suppress("cursor inside string literal");
    }

    // 6. Inside a comment → suppress (unless allowInComments)
    if (isInComment(ctx))
    {
        if (!config_.allowInComments)
            return TriggerDecision::suppress("cursor inside comment");
    }

    // 6. Mid-token / actively typing → suppress
    if (isMidToken(ctx))
        return TriggerDecision::suppress("cursor mid-token (typing through word)");

    // 7. Duplicate context (same as last request) → suppress
    if (isDuplicateContext(ctx, lastRequestedCtx))
        return TriggerDecision::suppress("duplicate context (uncaged editor state)");

    // 8. Must be at a meaningful boundary → else suppress
    if (!isAtMeaningfulBoundary(ctx))
        return TriggerDecision::suppress("not at a meaningful completion boundary");

    return TriggerDecision::allow("meaningful boundary reached");
}

} // namespace hathor::lsp
