// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * MiniNotationTokeniser.cpp — implementation of MiniNotationTokeniser.
 *
 * Requirements: 27.3, 27.5, 22.4
 */

#include "MiniNotationTokeniser.hpp"
#include "HathorLookAndFeel.hpp"

#include <cctype>
#include <string_view>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

juce::String MiniNotationTokeniser::peekLineText(juce::CodeDocument::Iterator it)
{
    juce::String line;
    while (!it.isEOF() && it.peekNextChar() != '\n')
        line += it.nextChar();
    return line;
}

bool MiniNotationTokeniser::isFrontMatterLine(const juce::String& line) noexcept
{
    // Trim leading whitespace for the check
    const juce::String trimmed = line.trim();

    // [hathor] header
    if (trimmed == "[hathor]")
        return true;

    // key = value  or  key=value  (simple TOML-like front-matter)
    // Require at least one non-whitespace char before '='
    const int eqPos = trimmed.indexOfChar('=');
    if (eqPos > 0)
    {
        // Verify the LHS is a valid identifier-ish key (letters, digits, _, -)
        const juce::String key = trimmed.substring(0, eqPos).trim();
        if (key.isNotEmpty())
        {
            bool validKey = true;
            for (int i = 0; i < key.length(); ++i)
            {
                const juce::juce_wchar c = key[i];
                if (!juce::CharacterFunctions::isLetterOrDigit(c) && c != '_' && c != '-')
                {
                    validKey = false;
                    break;
                }
            }
            if (validKey)
                return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// readNextToken
// ---------------------------------------------------------------------------

int MiniNotationTokeniser::readNextToken(juce::CodeDocument::Iterator& iterator)
{
    if (iterator.isEOF())
        return 0;

    const int lineNum    = iterator.getLine();
    const int colStart   = iterator.toPosition().getIndexInLine();

    // -----------------------------------------------------------------------
    // Reset front-matter tracking state at the very beginning of the document
    // (line 0, col 0).  JUCE calls this tokeniser from scratch for every
    // repaint, so stale state from a previous paint pass must be cleared.
    // -----------------------------------------------------------------------
    if (lineNum == 0 && colStart == 0)
    {
        inFrontMatter_  = true;
        frontMatterDone_ = false;
        sawHathorHeader_ = false;
    }

    // -----------------------------------------------------------------------
    // Build / retrieve cached line text (Req 27.5)
    // Only re-tokenise when we move to a new line.
    // -----------------------------------------------------------------------
    if (lineNum != lastLineNum_)
    {
        juce::String lineText = peekLineText(iterator);
        std::string  lineStd  = lineText.toStdString();

        // -------------------------------------------------------------------
        // Front-matter detection (Req 22.4)
        // -------------------------------------------------------------------
        if (!frontMatterDone_)
        {
            // If line 0 is NOT the [hathor] header and NOT a key=value line,
            // there is no front-matter at all — treat whole document as body.
            if (lineNum == 0 && !isFrontMatterLine(lineText))
            {
                frontMatterDone_ = true;
                inFrontMatter_   = false;
            }
            else if (lineText.trim().isEmpty())
            {
                // First blank line after the header ends the front-matter.
                frontMatterDone_ = true;
                inFrontMatter_   = false;
            }
            else if (isFrontMatterLine(lineText))
            {
                if (lineText.trim() == "[hathor]")
                    sawHathorHeader_ = true;
                inFrontMatter_ = true;
            }
            else
            {
                // Non-matching, non-blank line while still scanning header:
                // front-matter ends here.
                frontMatterDone_ = true;
                inFrontMatter_   = false;
            }
        }
        else
        {
            inFrontMatter_ = false;
        }

        // -------------------------------------------------------------------
        // Cache the tokenised result for this line (Req 27.5)
        // -------------------------------------------------------------------
        if (lineStd != lastLine_)
        {
            lastLine_   = lineStd;
            lastTokens_ = hathor::tokenise(lastLine_);
        }
        lastLineNum_ = lineNum;
    }

    // -----------------------------------------------------------------------
    // Front-matter lines — advance by 1 and return colour 0 (Req 22.4)
    // -----------------------------------------------------------------------
    if (inFrontMatter_)
    {
        iterator.nextChar();
        return 0;
    }

    // -----------------------------------------------------------------------
    // Find the token at colStart
    // -----------------------------------------------------------------------
    for (const auto& tok : lastTokens_)
    {
        if (tok.kind == hathor::TokenKind::TK_EOF)
        {
            // EOF sentinel — advance by 1 and return 0
            iterator.nextChar();
            return 0;
        }

        if (static_cast<int>(tok.pos) == colStart)
        {
            // Advance the iterator by exactly token.text.size() chars
            const int len = static_cast<int>(tok.text.size());
            for (int i = 0; i < len; ++i)
                iterator.nextChar();

            return tokenKindToColourIndex(tok.kind);
        }
    }

    // No token found at this position (e.g. whitespace gap) — advance by 1
    iterator.nextChar();
    return 0;
}

// ---------------------------------------------------------------------------
// createColourScheme / getDefaultColourScheme
// ---------------------------------------------------------------------------

juce::CodeEditorComponent::ColourScheme MiniNotationTokeniser::getDefaultColourScheme()
{
    // Build the colour scheme from the active palette so that mini-notation
    // syntax highlighting tracks theme switches (B3). The palette tokens are
    // the single source of truth (§4.1); no colour literals are duplicated here.
    const Palette& p = HathorLookAndFeel::globalPalette();

    // Colour index mapping (see header doc comment):
    //   0 (Atom/default)  → palette.codeText
    //   1 (Integer)       → palette.codeKeyword  (numeric literals share keyword colour)
    //   2 (Rest/Tilde)    → palette.codeType
    //   3 (Bracket)       → palette.codeBracket
    //   4 (Operator)      → palette.codeMacro
    //   5 (Paren/Comma)   → palette.codeString
    //   6 (Error)         → palette.error       (brighter than text for visibility)
    juce::CodeEditorComponent::ColourScheme scheme;
    scheme.set("Atom",       p.codeText);
    scheme.set("Integer",    p.codeKeyword);
    scheme.set("Rest/Tilde", p.codeType);
    scheme.set("Bracket",    p.codeBracket);
    scheme.set("Operator",   p.codeMacro);
    scheme.set("Paren/Comma",p.codeString);
    scheme.set("Error",      p.error);

    return scheme;
}

} // namespace hathor::ui
