// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * MiniNotationTokeniser.hpp — juce::CodeTokeniser subclass that delegates
 * syntax highlighting to hathor::tokenise().
 *
 * Requirements: 27.3, 27.5, 22.4
 */

#include <juce_gui_extra/juce_gui_extra.h>
#include <hathor/MiniTokeniser.hpp>
#include "TokenColourMap.hpp"

#include <string>
#include <vector>

namespace hathor::ui {

/**
 * MiniNotationTokeniser
 *
 * Implements juce::CodeTokeniser so that a juce::CodeEditorComponent can
 * highlight mini-notation syntax.  The tokeniser delegates to
 * hathor::tokenise() and provides a per-line cache so that each unique line
 * is only tokenised once regardless of how many tokens it contains
 * (Req 27.5).
 *
 * Colour index mapping (Req 27.3):
 *   0 — TK_ATOM / default
 *   1 — TK_INT
 *   2 — TK_TILDE
 *   3 — TK_LBRACKET, TK_RBRACKET, TK_LANGLE, TK_RANGLE
 *   4 — TK_STAR, TK_SLASH, TK_BANG
 *   5 — TK_LPAREN, TK_RPAREN, TK_COMMA
 *   6 — TK_ERROR
 *
 * Front-matter exclusion (Req 22.4):
 *   Lines that belong to the [hathor] front-matter header (before the first
 *   blank separator line) are returned with colour index 0 (no highlighting).
 */
class MiniNotationTokeniser : public juce::CodeTokeniser
{
public:
    MiniNotationTokeniser() = default;
    ~MiniNotationTokeniser() override = default;

    // -----------------------------------------------------------------------
    // juce::CodeTokeniser interface
    // -----------------------------------------------------------------------

    int readNextToken(juce::CodeDocument::Iterator& iterator) override;

    juce::CodeEditorComponent::ColourScheme getDefaultColourScheme() override;

private:
    // -----------------------------------------------------------------------
    // Per-line cache (Req 27.5)
    // -----------------------------------------------------------------------
    std::string              lastLine_;
    std::vector<hathor::Token> lastTokens_;
    int                      lastLineNum_{ -1 };

    // -----------------------------------------------------------------------
    // Front-matter state (Req 22.4)
    // Reset when we see line 0 to avoid stale state between repaints.
    // -----------------------------------------------------------------------
    bool inFrontMatter_{ true };
    bool frontMatterDone_{ false };
    bool sawHathorHeader_{ false };

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

     /// Read the text from the current iterator position to end-of-line
    /// using a *copy* of the iterator so the real iterator is not advanced.
    static juce::String peekLineText(juce::CodeDocument::Iterator it);

    /// Return true if the line looks like a front-matter line
    /// (i.e. "[hathor]" header or "key = value" / "key=value" pair).
    static bool isFrontMatterLine(const juce::String& line) noexcept;
};

} // namespace hathor::ui
