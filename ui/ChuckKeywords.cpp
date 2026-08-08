// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckKeywords.cpp — implementation of JUCE-free ChucK keyword sets.
 *
 * Lexical categories adapted from the public ChucK TextMate grammar:
 *   forrcaho/vscode-chuck  (port of cjwilburn/language-chuck)
 *   https://github.com/forrcaho/vscode-chuck/blob/main/syntaxes/chuck.tmLanguage.json
 */

#include "ChuckKeywords.hpp"

#include <cctype>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Keyword / class sets — adapted from vscode-chuck grammar
// ---------------------------------------------------------------------------

const std::unordered_set<std::string_view>& chuckKeywordSet() noexcept
{
    // keyword.control.chuck — from vscode-chuck grammar:
    //   break | continue | do | else | for | if | repeat | return | switch | until | while
    static const std::unordered_set<std::string_view> s = {
        "break", "continue", "do", "else", "for", "if",
        "repeat", "return", "switch", "until", "while",
    };
    return s;
}

const std::unordered_set<std::string_view>& chuckModifierSet() noexcept
{
    // keyword.control.chuck (function/spork group in vscode-chuck):
    //   const | fun | function | new | spork
    static const std::unordered_set<std::string_view> s = {
        "const", "fun", "function", "new", "spork",
    };
    return s;
}

const std::unordered_set<std::string_view>& chuckTypeKeywordSet() noexcept
{
    // storage.type.class.chuck:  class | interface
    // storage.modifier.class.chuck: extends | implements | private | protected | public | pure | static
    static const std::unordered_set<std::string_view> s = {
        "class", "interface",
        "extends", "implements", "private", "protected", "public", "pure", "static",
    };
    return s;
}

const std::unordered_set<std::string_view>& chuckTypeSet() noexcept
{
    // storage.type.chuck — vscode-chuck: complex | dur | float | int | polar | same | string | time | void
    // language-chuck adds: vec3 | vec4
    static const std::unordered_set<std::string_view> s = {
        "complex", "dur", "float", "int", "polar", "same",
        "string", "time", "void",
        "vec3", "vec4",
    };
    return s;
}

const std::unordered_set<std::string_view>& chuckVariableLanguageSet() noexcept
{
    // variable.language.chuck: this | super
    static const std::unordered_set<std::string_view> s = {
        "this", "super",
    };
    return s;
}

const std::unordered_set<std::string_view>& chuckConstantSet() noexcept
{
    // constant.special.chuck — vscode-chuck:
    //   adc | blackhole | cherr | chout | dac | day | false | hour | maybe |
    //   me | minute | ms | now | null | NULL | samp | second | true | week
    static const std::unordered_set<std::string_view> s = {
        "adc", "blackhole", "cherr", "chout", "dac",
        "day", "false", "hour", "maybe",
        "me", "minute", "ms", "now", "null", "NULL",
        "samp", "second", "true", "week",
    };
    return s;
}

// --- UGen class names (support.class.ugen.chuck) ---
static const std::unordered_set<std::string_view>& chuckUgenSetImpl() noexcept
{
    static const std::unordered_set<std::string_view> s = {
        "UGen", "UGen_Multi", "UGen_Stereo",
        "SinOsc", "PulseOsc", "SqrOsc", "TriOsc", "SawOsc", "Phasor",
        "Noise", "Impulse", "Step", "Gain",
        "SndBuf", "SndBuf2",
        "HalfRect", "FullRect", "Mix2", "Pan2",
        "CurveTable", "WarpTable",
        "LiSa", "Envelope", "ADSR",
        "Delay", "DelayA", "DelayL", "Echo",
        "JCRev", "NRev", "PRCRev", "Chorus",
        "Modulate", "PitShift", "SubNoise",
        "Blit", "BlitSaw", "BlitSquare",
        "WvIn", "WaveLoop", "WvOut",
        "OneZero", "TwoZero", "OnePole", "TwoPole", "PoleZero", "BiQuad",
        "Filter", "LPF", "HPF", "BPF", "BRF", "ResonZ", "Dyno",
        "StkInstrument",
        "BandedWG", "BlowBotl", "BlowHole", "Bowed", "Brass", "Clarinet",
        "Flute", "Mandolin", "ModalBar", "Moog", "Saxofony", "Shakers",
        "Sitar", "StifKarp", "VoicForm",
        "FM", "BeeThree", "FMVoices", "HevyMetl", "PercFlut", "Rhodey",
        "TubeBell", "Wurley",
        "Chugen", "Chugraph", "Chubgraph",
        "Gen5", "Gen7", "Gen9", "Gen10", "Gen17", "GenX",
        "BLT", "CNoise", "FilterBasic", "FilterStk", "LiSa10",
    };
    return s;
}

const std::unordered_set<std::string_view>& chuckUgenSet() noexcept
{
    return chuckUgenSetImpl();
}

// --- Library class names (support.class.library.chuck) ---
static const std::unordered_set<std::string_view>& chuckLibrarySetImpl() noexcept
{
    static const std::unordered_set<std::string_view> s = {
        "Machine", "Math", "Object", "RegEx", "Shred", "Std",
    };
    return s;
}

const std::unordered_set<std::string_view>& chuckLibrarySet() noexcept
{
    return chuckLibrarySetImpl();
}

// ---------------------------------------------------------------------------
// classifyChuckIdentifier
// ---------------------------------------------------------------------------

int classifyChuckIdentifier(std::string_view word) noexcept
{
    // "pi" is treated as constant.numeric in the grammar.
    if (word == "pi")
        return TK_CK_NUMBER;

    // Check order: keywords, types, modifiers, variable-language, constants,
    // then UGen/library classes.
    if (chuckKeywordSet().count(word))
        return TK_CK_KEYWORD;
    if (chuckTypeSet().count(word))
        return TK_CK_TYPE;
    if (chuckTypeKeywordSet().count(word))
        return TK_CK_TYPE;
    if (chuckModifierSet().count(word))
        return TK_CK_KEYWORD;
    if (chuckVariableLanguageSet().count(word))
        return TK_CK_KEYWORD;
    if (chuckConstantSet().count(word))
        return TK_CK_NUMBER;
    if (chuckUgenSet().count(word))
        return TK_CK_UGEN;
    if (chuckLibrarySet().count(word))
        return TK_CK_LIBRARY;
    return TK_CK_DEFAULT;
}

// ---------------------------------------------------------------------------
// isChuckExtension
// ---------------------------------------------------------------------------

bool isChuckExtension(std::string_view ext) noexcept
{
    // Case-insensitive comparison of ".ck" (3 chars: dot, 'c', 'k').
    if (ext.size() != 3 || ext[0] != '.')
        return false;
    char a = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[1])));
    char b = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[2])));
    return a == 'c' && b == 'k';
}

} // namespace hathor::ui
