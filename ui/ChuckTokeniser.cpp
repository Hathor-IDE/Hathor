// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckTokeniser.cpp — implementation of ChuckTokeniser.
 *
 * Lexing is adapted from the public ChucK TextMate grammar:
 *   forrcaho/vscode-chuck  (port of cjwilburn/language-chuck)
 *   https://github.com/forrcaho/vscode-chuck
 *
 * The grammar's regex-based patterns are reimplemented here as a deterministic
 * single-pass character scanner suitable for juce::CodeEditorComponent.
 * Only lexical/tokenisation categories are reproduced — no semantic analysis,
 * no parsing, no evaluation.
 */

#include "ChuckTokeniser.hpp"

#include <cctype>
#include <string_view>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// peekLineText
// ---------------------------------------------------------------------------

juce::String ChuckTokeniser::peekLineText(juce::CodeDocument::Iterator it)
{
    juce::String line;
    while (!it.isEOF() && it.peekNextChar() != '\n')
        line += it.nextChar();
    return line;
}

// ---------------------------------------------------------------------------
// Keyword / class sets — adapted from vscode-chuck grammar
// ---------------------------------------------------------------------------
// Each set corresponds to a `match` pattern in the TextMate grammar.
// Order: most specific first where overlap is possible (the classifier
// checks keyword → type → modifier → variable-language → constant before
// ugen/library, so identifiers never match class patterns unless they are
// truly class names).

static const std::unordered_set<std::string_view>& chuckKeywords() noexcept
{
    // keyword.control.chuck — from vscode-chuck grammar:
    //   break | continue | do | else | for | if | repeat | return | switch | until | while
    static const std::unordered_set<std::string_view> s = {
        "break", "continue", "do", "else", "for", "if",
        "repeat", "return", "switch", "until", "while",
    };
    return s;
}

static const std::unordered_set<std::string_view>& chuckModifiers() noexcept
{
    // keyword.control.chuck (function/spork group in vscode-chuck):
    //   const | fun | function | new | spork
    static const std::unordered_set<std::string_view> s = {
        "const", "fun", "function", "new", "spork",
    };
    return s;
}

static const std::unordered_set<std::string_view>& chuckTypeKeywords() noexcept
{
    // storage.type.class.chuck:  class | interface
    // storage.modifier.class.chuck: extends | implements | private | protected | public | pure | static
    static const std::unordered_set<std::string_view> s = {
        "class", "interface",
        "extends", "implements", "private", "protected", "public", "pure", "static",
    };
    return s;
}

static const std::unordered_set<std::string_view>& chuckTypes() noexcept
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

static const std::unordered_set<std::string_view>& chuckVariableLanguage() noexcept
{
    // variable.language.chuck: this | super
    static const std::unordered_set<std::string_view> s = {
        "this", "super",
    };
    return s;
}

static const std::unordered_set<std::string_view>& chuckConstants() noexcept
{
    // constant.special.chuck — vscode-chuck:
    //   adc | blackhole | cherr | chout | dac | day | false | hour | maybe |
    //   me | minute | ms | now | null | NULL | samp | second | true | week
    // language-chuck is the same set.
    static const std::unordered_set<std::string_view> s = {
        "adc", "blackhole", "cherr", "chout", "dac",
        "day", "false", "hour", "maybe",
        "me", "minute", "ms", "now", "null", "NULL",
        "samp", "second", "true", "week",
    };
    return s;
}

// --- UGen class names (support.class.ugen.chuck) ---
// From vscode-chuck grammar, merged with language-chuck additions.
// These are ChucK's built-in unit generator classes.
static const std::unordered_set<std::string_view>& chuckUgens() noexcept
{
    static const std::unordered_set<std::string_view> s = {
        // Core UGens (from both grammars)
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
        // STK models
        "BandedWG", "BlowBotl", "BlowHole", "Bowed", "Brass", "Clarinet",
        "Flute", "Mandolin", "ModalBar", "Moog", "Saxofony", "Shakers",
        "Sitar", "StifKarp", "VoicForm",
        // FM / electric pianos / organs
        "FM", "BeeThree", "FMVoices", "HevyMetl", "PercFlut", "Rhodey",
        "TubeBell", "Wurley",
        // Chugraph/Chugen
        "Chugen", "Chugraph", "Chubgraph",
        // Gen
        "Gen5", "Gen7", "Gen9", "Gen10", "Gen17", "GenX",
        // Other
        "BLT", "CNoise", "FilterBasic", "FilterStk", "LiSa10",
    };
    return s;
}

// --- Library class names (support.class.library.chuck) ---
static const std::unordered_set<std::string_view>& chuckLibraries() noexcept
{
    static const std::unordered_set<std::string_view> s = {
        "Machine", "Math", "Object", "RegEx", "Shred", "Std",
    };
    return s;
}

// ---------------------------------------------------------------------------
// Static-set accessor implementations (delegate to the static functions above)
// ---------------------------------------------------------------------------

const std::unordered_set<std::string_view>& ChuckTokeniser::keywordSet() noexcept
{
    return chuckKeywords();
}

const std::unordered_set<std::string_view>& ChuckTokeniser::typeSet() noexcept
{
    return chuckTypeKeywords();
}

const std::unordered_set<std::string_view>& ChuckTokeniser::ugenSet() noexcept
{
    return chuckUgens();
}

const std::unordered_set<std::string_view>& ChuckTokeniser::librarySet() noexcept
{
    return chuckLibraries();
}

const std::unordered_set<std::string_view>& ChuckTokeniser::constantSet() noexcept
{
    return chuckConstants();
}

const std::unordered_set<std::string_view>& ChuckTokeniser::modifierSet() noexcept
{
    return chuckModifiers();
}

const std::unordered_set<std::string_view>& ChuckTokeniser::variableLanguageSet() noexcept
{
    return chuckVariableLanguage();
}

// ---------------------------------------------------------------------------
// classifyIdentifier
// ---------------------------------------------------------------------------

int ChuckTokeniser::classifyIdentifier(std::string_view word) noexcept
{
    // Check order matters: built-in constants and types first, then classes.
    // A word can only be one category at a time.
    if (chuckKeywords().count(word))
        return 1;  // TK_KEYWORD
    if (chuckTypes().count(word))
        return 2;  // TK_TYPE  (storage.type.chuck)
    if (chuckTypeKeywords().count(word))
        return 2;  // TK_TYPE  (storage.type.class / storage.modifier)
    if (chuckModifiers().count(word))
        return 1;  // TK_KEYWORD (fun/function/spork/const/new)
    if (chuckVariableLanguage().count(word))
        return 1;  // TK_KEYWORD (this/super)
    if (chuckConstants().count(word))
        return 5;  // TK_NUMBER — constants share the "literal" colour slot
    if (chuckUgens().count(word))
        return 6;  // TK_UGEN (support.class.ugen)
    if (chuckLibraries().count(word))
        return 7;  // TK_LIBRARY (support.class.library)
    return 0;      // default / plain identifier
}

// ---------------------------------------------------------------------------
// isChuckFile / isChuckExtension
// ---------------------------------------------------------------------------

bool ChuckTokeniser::isChuckExtension(std::string_view ext) noexcept
{
    // Case-insensitive comparison of ".ck".
    if (ext.size() != 3 || ext[0] != '.' && ext[0] != '.')
        return false;
    if (ext[0] != '.')
        return false;
    char a = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[1])));
    char b = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[2])));
    return a == 'c' && b == 'k';
}

bool ChuckTokeniser::isChuckFile(const juce::File& file) noexcept
{
    return isChuckExtension(file.getFileExtension().toStdString());
}

// ---------------------------------------------------------------------------
// readNextToken
// ---------------------------------------------------------------------------

int ChuckTokeniser::readNextToken(juce::CodeDocument::Iterator& iterator)
{
    if (iterator.isEOF())
        return 0;

    const juce::juce_wchar c = iterator.peekNextChar();

    // -----------------------------------------------------------------------
    // Whitespace
    // -----------------------------------------------------------------------
    if (std::iswspace(c))
    {
        iterator.nextChar();
        return 0;
    }

    // -----------------------------------------------------------------------
    // Line comments:  //  and  <--  (vscode-chuck grammar)
    // -----------------------------------------------------------------------
    if (c == '/' && iterator.peekNextChar() == '/')
    {
        // Consume to end of line (not including the newline).
        while (!iterator.isEOF())
        {
            juce::juce_wchar ch = iterator.peekNextChar();
            if (ch == '\n' || ch == '\r')
                break;
            iterator.nextChar();
        }
        return 4;  // TK_COMMENT
    }

    // <-- line comment
    if (c == '<' && iterator.peekNextChar() == '-' && iterator.peekPreviousChar() == '<')
    {
        // Actually we need to check two chars: "<--"
        // But peekPreviousChar from the current position isn't right — let's
        // check the pattern differently. We're at '<', need to see if next chars
        // are '-' and '-' — wait, the grammar says "<--" (3 chars). Let me re-check.
        // Grammar: begin: "(^[ \t]+)?(?=//|<\\-\\-)"
        // match for comment start: "//|<\\-\\-"
        // So "<--" is the comment form. We have c == '<' already.
        // Check if next two chars are '-' '-'
    }

    // Re-check: we need to look ahead two characters from '<' to see "<--"
    // Actually, the grammar pattern is "<\\-\\-" which is literally "<--"
    // Let's handle this properly: check current char '<' and peek next two.
    if (c == '<')
    {
        // Save position for restore if this isn't a comment
        auto saveIter = iterator;
        juce::String twoChars;
        twoChars += iterator.nextChar();  // '<'
        twoChars += iterator.nextChar();  // next char
        twoChars += iterator.nextChar();  // next next char
        if (twoChars == "<--")
        {
            // It's a <-- comment — consume to end of line.
            while (!iterator.isEOF())
            {
                juce::juce_wchar ch = iterator.peekNextChar();
                if (ch == '\n' || ch == '\r')
                    break;
                iterator.nextChar();
            }
            return 4;  // TK_COMMENT
        }
        // Not a comment — restore iterator.
        iterator = saveIter;
    }

    // -----------------------------------------------------------------------
    // Block comments:  /* ... */  (including empty /*/)
    // -----------------------------------------------------------------------
    if (c == '/' && iterator.peekNextChar() == '*')
    {
        iterator.nextChar();  // consume '/'
        iterator.nextChar();  // consume '*'

        // Handle empty block comment /*/
        if (iterator.peekNextChar() == '/')
        {
            iterator.nextChar();
            return 4;
        }

        // Consume until */
        while (!iterator.isEOF())
        {
            juce::juce_wchar ch = iterator.nextChar();
            if (ch == '*' && !iterator.isEOF() && iterator.peekNextChar() == '/')
            {
                iterator.nextChar();  // consume '/'
                break;
            }
        }
        return 4;  // TK_COMMENT
    }

    // -----------------------------------------------------------------------
    // Debug output: <<< and >>> (support.function.debug.chuck)
    // -----------------------------------------------------------------------
    if (c == '<' && iterator.peekNextChar() == '<' &&
        iterator.peekNextCharAt(2) == '<')
    {
        iterator.nextChar();
        iterator.nextChar();
        iterator.nextChar();
        return 9;  // TK_DEBUG
    }

    if (c == '>' && iterator.peekNextChar() == '>' &&
        iterator.peekNextCharAt(2) == '>')
    {
        iterator.nextChar();
        iterator.nextChar();
        iterator.nextChar();
        return 9;  // TK_DEBUG
    }

    // -----------------------------------------------------------------------
    // String literals: double-quoted " ... " with escape sequences
    // -----------------------------------------------------------------------
    if (c == '"')
    {
        iterator.nextChar();  // consume opening '"'

        while (!iterator.isEOF())
        {
            juce::juce_wchar ch = iterator.nextChar();
            if (ch == '\\')
            {
                // Escape sequence: skip the next char (e.g. \", \\, \n, \t)
                if (!iterator.isEOF())
                    iterator.nextChar();
            }
            else if (ch == '"')
            {
                break;  // closing quote
            }
        }
        return 3;  // TK_STRING
    }

    // -----------------------------------------------------------------------
    // Character literals: single-quoted ' ... '
    // -----------------------------------------------------------------------
    if (c == '\'')
    {
        iterator.nextChar();  // consume opening '\''

        while (!iterator.isEOF())
        {
            juce::juce_wchar ch = iterator.nextChar();
            if (ch == '\\')
            {
                if (!iterator.isEOF())
                    iterator.nextChar();
            }
            else if (ch == '\'')
            {
                break;
            }
        }
        return 3;  // TK_STRING
    }

    // -----------------------------------------------------------------------
    // Numbers — constant.numeric.chuck
    // Hex: 0x... / 0X...
    // Binary: 0b... / 0B...
    // Decimal/int: digits with optional _ separators and . / e notation
    // pi: the bare token "pi"
    // -----------------------------------------------------------------------
    if (std::isdigit(c) || (c == '.' && iterator.peekNextChar() != '.'))
    {
        // Could be hex (0x), binary (0b), or decimal.
        if (c == '0')
        {
            juce::juce_wchar next = iterator.peekNextChar();
            if (next == 'x' || next == 'X')
            {
                // Hex number: 0x... or 0X...
                iterator.nextChar();  // '0'
                iterator.nextChar();  // 'x'/'X'
                while (!iterator.isEOF())
                {
                    juce::juce_wchar hc = iterator.peekNextChar();
                    if (std::isxdigit(hc))
                        iterator.nextChar();
                    else
                        break;
                }
                return 5;  // TK_NUMBER
            }
            if (next == 'b' || next == 'B')
            {
                // Binary number: 0b... or 0B...
                iterator.nextChar();  // '0'
                iterator.nextChar();  // 'b'/'B'
                while (!iterator.isEOF())
                {
                    juce::juce_wchar bc = iterator.peekNextChar();
                    if (bc == '0' || bc == '1')
                        iterator.nextChar();
                    else
                        break;
                }
                return 5;  // TK_NUMBER
            }
        }

        // Decimal number: \d(?>_?\d)* (with optional . digit* and [eE][-+]?\d+)
        while (!iterator.isEOF())
        {
            juce::juce_wchar nc = iterator.peekNextChar();
            if (std::isdigit(static_cast<unsigned char>(nc)))
            {
                iterator.nextChar();
                // consume following digits and underscores
                while (!iterator.isEOF())
                {
                    juce::juce_wchar cc = iterator.peekNextChar();
                    if (std::isdigit(static_cast<unsigned char>(cc)) || cc == '_')
                        iterator.nextChar();
                    else
                        break;
                }
            }
            else if (nc == '.')
            {
                // Look ahead: if char after '.' is a digit, it's a float.
                juce::CodeDocument::Iterator lookahead = iterator;
                lookahead.nextChar();  // consume '.'
                if (!lookahead.isEOF())
                {
                    juce::juce_wchar afterDot = lookahead.peekNextChar();
                    if (std::isdigit(static_cast<unsigned char>(afterDot)))
                    {
                        iterator.nextChar();  // consume '.'
                        // consume fractional digits and underscores
                        while (!iterator.isEOF())
                        {
                            juce::juce_wchar cc = iterator.peekNextChar();
                            if (std::isdigit(static_cast<unsigned char>(cc)) || cc == '_')
                                iterator.nextChar();
                            else
                                break;
                        }
                    }
                    else
                    {
                        // Not a float — stop without consuming the '.'
                        break;
                    }
                }
                else
                {
                    break;
                }
            }
            else if (nc == 'e' || nc == 'E')
            {
                // Exponent: e/E followed by optional +/- and digits
                juce::CodeDocument::Iterator lookahead = iterator;
                lookahead.nextChar();  // consume 'e'/'E'
                if (!lookahead.isEOF())
                {
                    juce::juce_wchar maybeSign = lookahead.peekNextChar();
                    if (maybeSign == '+' || maybeSign == '-')
                        lookahead.nextChar();
                    if (!lookahead.isEOF() && std::isdigit(static_cast<unsigned char>(lookahead.peekNextChar())))
                    {
                        // It's a real exponent — commit.
                        iterator = lookahead;
                        while (!iterator.isEOF())
                        {
                            juce::juce_wchar cc = iterator.peekNextChar();
                            if (std::isdigit(static_cast<unsigned char>(cc)))
                                iterator.nextChar();
                            else
                                break;
                        }
                    }
                    else
                    {
                        // Not a real exponent — stop.
                        break;
                    }
                }
                else
                {
                    break;
                }
            }
            else
            {
                break;
            }
        }
        return 5;  // TK_NUMBER
    }

    // -----------------------------------------------------------------------
    // Identifiers and keywords: [A-Za-z_$][\w$]*
    // -----------------------------------------------------------------------
    if (std::iswalpha(c) || c == '_' || c == '$')
    {
        juce::String word;
        while (!iterator.isEOF())
        {
            juce::juce_wchar ch = iterator.peekNextChar();
            if (std::iswalnum(ch) || ch == '_' || ch == '$')
            {
                word += iterator.nextChar();
            }
            else
            {
                break;
            }
        }

        // Check if it's the special "pi" constant.
        if (word == "pi")
            return 5;  // TK_NUMBER (grammar treats pi as constant.numeric)

        return classifyIdentifier(word.toStdString());
    }

    // -----------------------------------------------------------------------
    // Operators and punctuation — keyword.operator.chuck
    // Grammar: =>|=<|@=>|\+=>|\-=>|\*=>|\/=>|%=>|\+\+|\+|--|-|\*|\/(?!/|%|
    //          ==|!=|<=|>=|<<|>>|<|>|&&|\|\||&|\|||\^|\$|::
    // -----------------------------------------------------------------------
    if (c == '=' || c == '<' || c == '>' || c == '+' || c == '-' ||
        c == '*' || c == '/' || c == '%' || c == '&' || c == '|' ||
        c == '^' || c == '$' || c == '!' || c == ':' )
    {
        // Try two-char operators first.
        juce::juce_wchar next = iterator.peekNextChar();

        // => assignment (Chuck)
        if (c == '=' && next == '>')
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;  // TK_OPERATOR
        }
        // =>=<  (not Chuck, but =< is a comparison)
        if (c == '=' && next == '<')
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;
        }
        // @=> (variable declaration)
        if (c == '@' && next == '=')
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;
        }
        // +=, -=, *=, /=, %=
        if ((next == '=') && (c == '+' || c == '-' || c == '*' || c == '/' || c == '%'))
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;
        }
        // ++, --
        if (next == c && (c == '+' || c == '-'))
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;
        }
        // ==, !=, <=, >=, <<, >>, &&, ||
        if (next == '=' && (c == '=' || c == '!' || c == '<' || c == '>' || c == '&' || c == '|'))
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;
        }
        // <<, >>  (already handled by next=='=' check? No: < followed by <)
        if ((c == '<' && next == '<') || (c == '>' && next == '>'))
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;
        }
        // &&, ||
        if ((c == '&' && next == '&') || (c == '|' && next == '|'))
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;
        }
        // ::
        if (c == ':' && next == ':')
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;
        }

        // Single-char operator
        iterator.nextChar();

        // Avoid matching '/' as start of comment here — comments are handled
        // earlier in this function (before we reach this section).
        // But '//' would be caught by the line-comment check above, and
        // '/*' by the block-comment check.  A bare '/' that is not part of
        // a comment is a division operator.
        return 8;  // TK_OPERATOR
    }

    // -----------------------------------------------------------------------
    // Remaining single-char punctuation: ( ) [ ] { } , ; .
    // -----------------------------------------------------------------------
    if (std::iswcntrl(c) && c != '\n' && c != '\r')
    {
        // Non-printable but not whitespace/newline — skip to avoid infinite loop.
        iterator.nextChar();
        return 0;
    }

    // Default: consume a single character and return default colour.
    iterator.nextChar();
    return 0;
}

// ---------------------------------------------------------------------------
// getDefaultColourScheme
// ---------------------------------------------------------------------------

juce::CodeEditorComponent::ColourScheme ChuckTokeniser::getDefaultColourScheme()
{
    // Map colour indices to the existing Hathor Palette code-syntax colours
    // (HathorLookAndFeel.hpp Palette struct, design token layer).
    //
    // The colour *names* here must match what JUCE's CodeEditorComponent
    // expects, but the ColourScheme is set per-editor via setColourScheme().
    // We use the same palette tokens as the mini-notation tokeniser but with
    // distinct names so the colour table can be managed separately if needed.
    //
    // The actual colours come from the Palette, but JUCE's ColourScheme is a
    // static table — so we read from the global palette.
    const Palette& p = HathorLookAndFeel::globalPalette();

    static const struct { const char* name; int index; } entries[] =
    {
        { "Default",  0 },
        { "Keyword",  1 },
        { "Type",     2 },
        { "String",   3 },
        { "Comment",  4 },
        { "Number",   5 },
        { "UGen",     6 },
        { "Library",  7 },
        { "Operator", 8 },
        { "Debug",    9 },
    };

    juce::CodeEditorComponent::ColourScheme scheme;
    for (const auto& e : entries)
        scheme.set(e.name, p.codeText);  // placeholder — set below

    scheme.set("Default",  p.codeText);
    scheme.set("Keyword",  p.codeKeyword);
    scheme.set("Type",     p.codeType);
    scheme.set("String",   p.codeString);
    scheme.set("Comment",  p.codeComment);
    scheme.set("Number",   p.codeKeyword);  // constants share keyword colour
    scheme.set("UGen",     p.codeFunction); // UGens get the function colour
    scheme.set("Library",  p.codeMacro);    // library classes get macro colour
    scheme.set("Operator", p.codeBracket);  // operators share bracket colour
    scheme.set("Debug",    p.codeMacro);    // <<< >>> share macro colour

    return scheme;
}

} // namespace hathor::ui
