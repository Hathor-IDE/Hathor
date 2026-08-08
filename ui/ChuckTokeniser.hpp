// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPV-3.0-or-later

#pragma once

/**
 * ChuckTokeniser.hpp — juce::CodeTokeniser subclass for ChucK (.ck) source files.
 *
 * The lexical categories are adapted from the public ChucK TextMate grammar:
 *   forrcaho/vscode-chuck  (itself a port of cjwilburn/language-chuck)
 *   https://github.com/forrcaho/vscode-chuck/blob/main/syntaxes/chuck.tmLanguage.json
 *
 * This tokeniser provides syntax highlighting only — no parsing, no semantic
 * analysis, no evaluation.  Real ChucK execution is wired in Phase B4.
 *
 * Colour indices map to the existing Hathor Palette code-syntax colours
 * (HathorLookAndFeel.hpp).  ChucK highlighting is intentionally distinct from
 * the mini-notation tokeniser: ChucK keywords/types/classes get their own
 * colour indices while sharing the same palette tokens (codeKeyword, codeType,
 * codeString, codeComment, codeFunction, codeMacro, etc.).
 *
 * Colour index mapping:
 *   0 — TK_DEFAULT  (palette.codeText — plain text / identifiers)
 *   1 — TK_KEYWORD  (palette.codeKeyword — control flow + fun/spork/etc.)
 *   2 — TK_TYPE     (palette.codeType — primitive types + class modifiers)
 *   3 — TK_STRING   (palette.codeString — quoted strings)
 *   4 — TK_COMMENT  (palette.codeComment — //, <--, /* */ comments)
 *   5 — TK_NUMBER   (palette.codeKeyword — constants like now/true/false/dac)
 *   6 — TK_UGEN     (palette.codeFunction — UGens like SinOsc, UGen)
 *   7 — TK_LIBRARY   (palette.codeMacro — library classes like Machine, Math)
 *   8 — TK_OPERATOR  (palette.codeBracket — operators & punctuation)
 *   9 — TK_DEBUG     (palette.codeMacro — <<< >>> debug printing)
 *
 * The JUCE-free keyword sets and classifier live in ChuckKeywords.hpp so they
 * can be unit-tested without linking JUCE.
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include "ChuckKeywords.hpp"

#include <string>
#include <string_view>

namespace hathor::ui {

class ChuckTokeniser : public juce::CodeTokeniser
{
public:
    ChuckTokeniser() = default;
    ~ChuckTokeniser() override = default;

    // -----------------------------------------------------------------------
    // juce::CodeTokeniser interface
    // -----------------------------------------------------------------------

    int readNextToken(juce::CodeDocument::Iterator& iterator) override;

    juce::CodeEditorComponent::ColourScheme getDefaultColourScheme() override;

    /** Return true if @p file looks like a ChucK source file (.ck). */
    static bool isChuckFile(const juce::File& file) noexcept;
};

} // namespace hathor::ui
