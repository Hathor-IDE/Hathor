// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckTokeniser.cpp — implementation of ChuckTokeniser.
 *
 * Lexing is adapted from the public ChucK TextMate grammar:
 *   forrcaho/vscode-chuck  (port of cjwilburn/language-chuck)
 *   https://github.com/forrcaho/vscode-chuck/blob/main/syntaxes/chuck.tmLanguage.json
 *
 * The grammar's regex-based patterns are reimplemented here as a deterministic
 * single-pass character scanner suitable for juce::CodeEditorComponent.
 * Only lexical/tokenisation categories are reproduced — no semantic analysis,
 * no parsing, no evaluation.
 *
 * Keyword/class sets and identifier classification delegate to the JUCE-free
 * ChuckKeywords module so logic can be unit-tested without JUCE.
 */

#include "ChuckTokeniser.hpp"
#include "HathorLookAndFeel.hpp"

#include <cctype>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// isChuckFile
// ---------------------------------------------------------------------------

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

    // <-- line comment — check by looking ahead 3 characters from current '<'
    if (c == '<')
    {
        juce::juce_wchar n1 = iterator.peekNextChar();
        if (n1 == '-')
        {
            // Save iterator position in case this isn't "<--"
            juce::CodeDocument::Iterator save = iterator;
            iterator.nextChar();  // '<'
            iterator.nextChar();  // '-'
            if (!iterator.isEOF() && iterator.peekNextChar() == '-')
            {
                iterator.nextChar();  // second '-'
                while (!iterator.isEOF())
                {
                    juce::juce_wchar ch = iterator.peekNextChar();
                    if (ch == '\n' || ch == '\r')
                        break;
                    iterator.nextChar();
                }
                return 4;  // TK_COMMENT
            }
            // Not "<--" — restore and fall through to operator handling.
            iterator = save;
        }
    }

    // -----------------------------------------------------------------------
    // Block comments:  /* ... */  (including empty /*/)
    // -----------------------------------------------------------------------
    if (c == '/' && iterator.peekNextChar() == '*')
    {
        iterator.nextChar();  // consume '/'
        iterator.nextChar();  // consume '*'

        // Handle empty block comment /*/
        if (!iterator.isEOF() && iterator.peekNextChar() == '/')
        {
            iterator.nextChar();
            return 4;  // TK_COMMENT
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
    if (c == '<')
    {
        juce::juce_wchar n1 = iterator.peekNextChar();
        if (n1 == '<')
        {
            // Save in case it's just a < operator
            juce::CodeDocument::Iterator save = iterator;
            iterator.nextChar();  // '<'
            iterator.nextChar();  // '<'
            if (!iterator.isEOF() && iterator.peekNextChar() == '<')
            {
                iterator.nextChar();  // third '<'
                return 9;  // TK_DEBUG
            }
            // Not <<< — restore and fall through to operator handling.
            iterator = save;
        }
    }

    if (c == '>')
    {
        juce::juce_wchar n1 = iterator.peekNextChar();
        if (n1 == '>')
        {
            juce::CodeDocument::Iterator save = iterator;
            iterator.nextChar();  // '>'
            iterator.nextChar();  // '>'
            if (!iterator.isEOF() && iterator.peekNextChar() == '>')
            {
                iterator.nextChar();  // third '>'
                return 9;  // TK_DEBUG
            }
            iterator = save;
        }
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
    // pi: the bare token "pi" (handled in classifyChuckIdentifier)
    // -----------------------------------------------------------------------
    if (std::isdigit(c) || (c == '.' && iterator.peekNextChar() != '.'))
    {
        // Could be hex (0x), binary (0b), or decimal.
        if (c == '0')
        {
            juce::juce_wchar next = iterator.peekNextChar();
            if (next == 'x' || next == 'X')
            {
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
                juce::CodeDocument::Iterator lookahead = iterator;
                lookahead.nextChar();  // consume 'e'/'E'
                if (!lookahead.isEOF())
                {
                    juce::juce_wchar maybeSign = lookahead.peekNextChar();
                    if (maybeSign == '+' || maybeSign == '-')
                        lookahead.nextChar();
                    if (!lookahead.isEOF() &&
                        std::isdigit(static_cast<unsigned char>(lookahead.peekNextChar())))
                    {
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
        // Read the full identifier word.
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

        // "pi" is classified as a numeric constant.
        if (word == "pi")
            return 5;  // TK_NUMBER

        return classifyChuckIdentifier(word.toStdString());
    }

    // -----------------------------------------------------------------------
    // Operators and punctuation — keyword.operator.chuck
    // Grammar: =>|=<|@=>|\+=>|\-=>|\*=>|\/=>|%=>|\+\+|\+|--|-|\*|\/(?!/|
    //          %|==|!=|<=|>=|<<|>>|<|>|&&|\|\||&|\||\^|\$|::
    // -----------------------------------------------------------------------
    if (c == '=' || c == '<' || c == '>' || c == '+' || c == '-' ||
        c == '*' || c == '/' || c == '%' || c == '&' || c == '|' ||
        c == '^' || c == '$' || c == '!' || c == ':' || c == '@' ||
        c == '(' || c == ')' || c == '[' || c == ']' ||
        c == '{' || c == '}' || c == ',' || c == ';' || c == '.' )
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
        // @=> (variable declaration)
        if (c == '@' && next == '=')
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;  // TK_OPERATOR
        }
        // +=, -=, *=, /=, %=
        if ((next == '=') && (c == '+' || c == '-' || c == '*' || c == '/' || c == '%'))
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;  // TK_OPERATOR
        }
        // ++, --
        if (next == c && (c == '+' || c == '-'))
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;  // TK_OPERATOR
        }
        // ==, !=, <=, >=
        if (next == '=' && (c == '=' || c == '!' || c == '<' || c == '>'))
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;  // TK_OPERATOR
        }
        // <<, >>
        if ((c == '<' && next == '<') || (c == '>' && next == '>'))
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;  // TK_OPERATOR
        }
        // &&, ||
        if ((c == '&' && next == '&') || (c == '|' && next == '|'))
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;  // TK_OPERATOR
        }
        // ::
        if (c == ':' && next == ':')
        {
            iterator.nextChar();
            iterator.nextChar();
            return 8;  // TK_OPERATOR
        }

        // Single-char operator / punctuation
        iterator.nextChar();
        return 8;  // TK_OPERATOR
    }

    // -----------------------------------------------------------------------
    // Fallback: consume a single character and return default colour.
    // -----------------------------------------------------------------------
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
    // Index → Palette token:
    //   0 (Default)   → palette.codeText
    //   1 (Keyword)   → palette.codeKeyword
    //   2 (Type)      → palette.codeType
    //   3 (String)    → palette.codeString
    //   4 (Comment)   → palette.codeComment
    //   5 (Number)    → palette.codeKeyword  (constants like now/true/false/dac share keyword colour)
    //   6 (UGen)      → palette.codeFunction
    //   7 (Library)   → palette.codeMacro
    //   8 (Operator)  → palette.codeBracket
    //   9 (Debug)     → palette.codeMacro
    const Palette& p = HathorLookAndFeel::globalPalette();

    juce::CodeEditorComponent::ColourScheme scheme;
    scheme.set("Default",  p.codeText);
    scheme.set("Keyword",  p.codeKeyword);
    scheme.set("Type",     p.codeType);
    scheme.set("String",   p.codeString);
    scheme.set("Comment",  p.codeComment);
    scheme.set("Number",   p.codeKeyword);  // constants share keyword colour
    scheme.set("UGen",     p.codeFunction);
    scheme.set("Library",  p.codeMacro);
    scheme.set("Operator", p.codeBracket);
    scheme.set("Debug",    p.codeMacro);

    return scheme;
}

} // namespace hathor::ui
