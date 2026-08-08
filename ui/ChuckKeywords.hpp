// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ChuckKeywords.hpp — JUCE-free ChucK lexical keyword/class sets and
 * classification logic.
 *
 * Adapted from the public ChucK TextMate grammar:
 *   forrcaho/vscode-chuck  (port of cjwilburn/language-chuck)
 *   https://github.com/forrcaho/vscode-chuck/blob/main/syntaxes/chuck.tmLanguage.json
 *
 * This file is intentionally JUCE-free so that the lexical category sets
 * and the identifier classifier can be unit-tested in the headless
 * hathor-ui-tests target (which does not link JUCE).
 *
 * The ChuckTokeniser (ui layer, JUCE-dependent) delegates to these sets
 * via ChuckTokeniser::classifyIdentifier and the accessor functions below.
 */

#include <string_view>
#include <unordered_set>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Colour index constants — match ChuckTokeniser's readNextToken return values
// ---------------------------------------------------------------------------

enum ChuckColourIndex : int
{
    TK_CK_DEFAULT = 0,   ///< plain text / identifiers / whitespace
    TK_CK_KEYWORD = 1,   ///< keyword.control.chuck (if, for, while, etc.)
    TK_CK_TYPE = 2,      ///< storage.type.chuck / storage.type.class / storage.modifier
    TK_CK_STRING = 3,    ///< string.quoted.double / string.quoted.single
    TK_CK_COMMENT = 4,   ///< comment.block / comment.line
    TK_CK_NUMBER = 5,    ///< constant.numeric + constant.special (now, true, etc.)
    TK_CK_UGEN = 6,      ///< support.class.ugen (SinOsc, ADSR, etc.)
    TK_CK_LIBRARY = 7,   ///< support.class.library (Machine, Math, Std, etc.)
    TK_CK_OPERATOR = 8,  ///< keyword.operator.chuck
    TK_CK_DEBUG = 9,     ///< support.function.debug (<<< >>>)
};

// ---------------------------------------------------------------------------
// Keyword / class sets — each corresponds to a match pattern in the grammar
// ---------------------------------------------------------------------------

/// keyword.control.chuck — control-flow: break, continue, do, else, for, if,
/// repeat, return, switch, until, while
const std::unordered_set<std::string_view>& chuckKeywordSet() noexcept;

/// keyword.control.chuck (function group): const, fun, function, new, spork
const std::unordered_set<std::string_view>& chuckModifierSet() noexcept;

/// storage.type.class.chuck + storage.modifier.class.chuck:
/// class, interface, extends, implements, private, protected, public, pure, static
const std::unordered_set<std::string_view>& chuckTypeKeywordSet() noexcept;

/// storage.type.chuck — primitive types:
/// complex, dur, float, int, polar, same, string, time, void, vec3, vec4
const std::unordered_set<std::string_view>& chuckTypeSet() noexcept;

/// variable.language.chuck: this, super
const std::unordered_set<std::string_view>& chuckVariableLanguageSet() noexcept;

/// constant.special.chuck:
/// adc, blackhole, cherr, chout, dac, day, false, hour, maybe,
/// me, minute, ms, now, null, NULL, samp, second, true, week
const std::unordered_set<std::string_view>& chuckConstantSet() noexcept;

/// support.class.ugen.chuck — built-in unit generator classes (SinOsc, etc.)
const std::unordered_set<std::string_view>& chuckUgenSet() noexcept;

/// support.class.library.chuck — library classes (Machine, Math, etc.)
const std::unordered_set<std::string_view>& chuckLibrarySet() noexcept;

// ---------------------------------------------------------------------------
// Identifier classification
// ---------------------------------------------------------------------------

/// Classify a ChucK identifier and return its colour index.
/// Returns TK_CK_DEFAULT (0) for plain identifiers.
int classifyChuckIdentifier(std::string_view word) noexcept;

/// Return true if @p ext (including leading dot, lowercase) is a ChucK file
/// extension.  Case-insensitive: ".ck", ".CK", ".Ck" all match.
bool isChuckExtension(std::string_view ext) noexcept;

} // namespace hathor::ui
