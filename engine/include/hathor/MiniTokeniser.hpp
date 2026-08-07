// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_MINI_TOKENISER_HPP
#define HATHOR_MINI_TOKENISER_HPP

/**
 * MiniTokeniser.hpp — public header exposing TokenKind, Token, and tokenise().
 *
 * The implementation lives in engine/src/MiniParser.cpp. This header makes
 * the tokeniser available to the UI layer (MiniNotationTokeniser) without
 * duplicating any logic.
 *
 * Requirement references: 27.1, 31.2
 */

#include <cstddef>
#include <string_view>
#include <vector>

namespace hathor {

/// Kinds of token produced by the mini-notation tokeniser.
enum class TokenKind {
    TK_ATOM,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_LANGLE,
    TK_RANGLE,
    TK_STAR,
    TK_SLASH,
    TK_BANG,
    TK_INT,
    TK_TILDE,
    TK_LPAREN,   ///< (
    TK_RPAREN,   ///< )
    TK_COMMA,    ///< ,
    TK_EOF,
    TK_ERROR
};

/// A single token from the mini-notation tokeniser.
struct Token {
    TokenKind        kind;
    std::string_view text; ///< slice of input (for TK_ATOM / TK_INT)
    std::size_t      pos;  ///< byte position in input
};

/// Tokenise `input` into a flat token list. Single-pass, no regex.
/// The returned vector always ends with a TK_EOF token.
std::vector<Token> tokenise(std::string_view input);

} // namespace hathor

#endif // HATHOR_MINI_TOKENISER_HPP
