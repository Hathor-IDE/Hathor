// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPV-3.0-or-later

#pragma once

/**
 * ChuckTokeniser.hpp — juce::CodeTokeniser subclass for ChucK (.ck) source files.
 *
 * The lexical categories are adapted from the public ChucK TextMate grammar:
 *   forrcaho/vscode-chuck  (itself a port of cjwilburn/language-chuck).
 *
 * Grammar source URL:
 *   https://github.com/forrcaho/vscode-chuck/blob/main/syntaxes/chuck.tmLanguage.json
 *
 * This tokeniser provides syntax highlighting only — no parsing, no semantic
 * analysis, no evaluation.  Real ChucK execution is wired in Phase B4.
 *
 * Colour indices map to the existing Hathor Palette code-syntax colours
 * (HathorLookAndFeel.hpp).  ChucK highlighting is intentionally distinct from
 * the mini-notation tokeniser: ChucK keywords/types/classes get their own
 * colour indices while sharing the same palette tokens (codeKeyword, codeType,
 * codeString, codeComment, etc.).
 *
 * Colour index mapping:
 *   0 — TK_DEFAULT   (palette.codeText — plain text / identifiers)
 *   1 — TK_KEYWORD   (palette.codeKeyword)
 *   2 — TK_TYPE      (palette.codeType)
 *   3 — TK_STRING    (palette.codeString)
 *   4 — TK_COMMENT   (palette.codeComment)
 *   5 — TK_NUMBER    (palette.codeKeyword — shared with keywords but visually distinct via token text)
 *   6 — TK_UGEN       (palette.codeFunction — UGens like SinOsc, UGen)
 *   7 — TK_LIBRARY    (palette.codeMacro — library classes like Machine, Math, Std)
 *   8 — TK_OPERATOR   (palette.codeBracket — operators & punctuation)
 *   9 — TK_DEBUG      (palette.codeMacro — <<< >>> debug printing)
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <string>
#include <string_view>
#include <unordered_set>

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

    /** Return true if @p filename looks like a ChucK source file (.ck). */
    static bool isChuckFile(const juce::File& file) noexcept;

    /** Return true if @p ext (e.g. ".ck") is a ChucK extension. */
    static bool isChuckExtension(std::string_view ext) noexcept;

private:
    // -----------------------------------------------------------------------
    // Lexing helpers
    // -----------------------------------------------------------------------

    /** Peek the text from the current iterator position to end-of-line.
        Uses a copy of the iterator so the real iterator is not advanced. */
    static juce::String peekLineText(juce::CodeDocument::Iterator it);

    // -----------------------------------------------------------------------
    // Keyword / class sets — adapted from vscode-chuck grammar
    // -----------------------------------------------------------------------

    /// ChucK control-flow keywords: if, else, for, while, etc.
    static const std::unordered_set<std::string_view>& keywordSet() noexcept;

    /// ChucK type keywords and modifiers: int, float, dur, void, class, etc.
    static const std::unordered_set<std::string_view>& typeSet() noexcept;

    /// UGen class names (sound-generating unit generators): SinOsc, SawOsc, etc.
    static const std::unordered_set<std::string_view>& ugenSet() noexcept;

    /// Library class names: Machine, Math, Object, RegEx, Shred, Std.
    static const std::unordered_set<std::string_view>& librarySet() noexcept;

    /// Built-in constants and special values: now, true, false, dac, adc, etc.
    static const std::unordered_set<std::string_view>& constantSet() noexcept;

    /// Other keyword-like tokens: fun, function, spork, const, new.
    static const std::unordered_set<std::string_view>& modifierSet() noexcept;

    /// Variable-language tokens: this, super.
    static const std::unordered_set<std::string_view>& variableLanguageSet() noexcept;

    // -----------------------------------------------------------------------
    // Classification
    // -----------------------------------------------------------------------

    /** Classify an identifier and return its colour index, or 0 (default). */
    static int classifyIdentifier(std::string_view word) noexcept;
};

} // namespace hathor::ui
