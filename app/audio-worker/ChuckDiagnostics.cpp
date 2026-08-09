// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckDiagnostics.cpp — implementation of validateChuckSource (B4-K4).
 *
 * This is the same validation logic that was previously inlined in
 * ChuckCompiler::dispatcherLoop().  Extracted into a separate, JUCE-free
 * translation unit so that:
 *   1. ChuckCompiler::dispatcherLoop() can call it (the real compile path).
 *   2. ProjectReadFacade::getDiagnostics() can call it (the AI-2 read-only path).
 *
 * When libchuck is vendored in, ck.compileCode() will replace this validation.
 * Until then, this IS the canonical ChucK diagnostic source.
 */

#include "ChuckDiagnostics.hpp"

namespace hathor::audio_worker {

ChuckDiagnostic validateChuckSource(std::string_view src)
{
    int parenDepth = 0, braceDepth = 0, bracketDepth = 0;
    bool hasSporkOrAssignment = false;
    int line = 1, col = 1;

    for (std::size_t i = 0; i < src.size(); ++i) {
        char c = src[i];

        if (c == '\n') { ++line; col = 1; continue; }
        ++col;

        // Track bracket depth.
        if (c == '(') ++parenDepth;
        else if (c == ')') --parenDepth;
        else if (c == '{') ++braceDepth;
        else if (c == '}') --braceDepth;
        else if (c == '[') ++bracketDepth;
        else if (c == ']') --bracketDepth;

        // Track the => sporking operator.
        if (c == '=' && i + 1 < src.size() && src[i + 1] == '>')
            hasSporkOrAssignment = true;

        // Check for negative depth (unbalanced close).
        if (parenDepth < 0 || braceDepth < 0 || bracketDepth < 0) {
            return {false, line, col,
                "unexpected ')' or '}' or ']' at mismatched position"};
        }
    }

    // Final depth check.
    if (parenDepth > 0)
        return {false, line, col, "unbalanced parentheses: missing ')'"};
    if (braceDepth > 0)
        return {false, line, col, "unbalanced braces: missing '}'"};
    if (bracketDepth > 0)
        return {false, line, col, "unbalanced brackets: missing ']'"};
    if (parenDepth < 0)
        return {false, line, col, "unbalanced parentheses: extra ')'"};
    if (braceDepth < 0)
        return {false, line, col, "unbalanced braces: extra '}'"};
    if (bracketDepth < 0)
        return {false, line, col, "unbalanced brackets: extra ']'"};

    // Every ChucK program needs at least one statement (with => or a
    // declaration). For now, just require the => operator or a semicolon.
    if (!hasSporkOrAssignment) {
        // Check if there's at least a semicolon (could be a declaration).
        if (src.find(';') == std::string::npos) {
            return {false, 1, 1,
                "expected ChucK sporking operator (=>) or statement terminator (;)"};
        }
    }

    return {true, 0, 0, {}};
}

} // namespace hathor::audio_worker
