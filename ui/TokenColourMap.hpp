// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * TokenColourMap.hpp — JUCE-free colour-index mapping for TokenKind.
 *
 * Maps hathor::TokenKind values to JUCE CodeTokeniser colour indices per
 * the specification in Req 27.3.  Extracted from MiniNotationTokeniser so
 * that the mapping can be tested without linking the full JUCE GUI stack
 * (the hathor-ui-tests target builds with HATHOR_BUILD_APP=OFF).
 *
 * Both MiniNotationTokeniser (production) and the P3 bijection test share
 * this single implementation — no logic is duplicated.
 *
 * Requirement references: 27.3
 */

#include <hathor/MiniTokeniser.hpp>

namespace hathor::ui {

/// Map a TokenKind to the JUCE colour index per the Spec table (Req 27.3):
///   0 — TK_ATOM / default
///   1 — TK_INT
///   2 — TK_TILDE
///   3 — TK_LBRACKET, TK_RBRACKET, TK_LANGLE, TK_RANGLE
///   4 — TK_STAR, TK_SLASH, TK_BANG
///   5 — TK_LPAREN, TK_RPAREN, TK_COMMA
///   6 — TK_ERROR
inline int tokenKindToColourIndex(hathor::TokenKind kind) noexcept
{
    using K = hathor::TokenKind;
    switch (kind)
    {
        case K::TK_ATOM:                              return 0;
        case K::TK_INT:                               return 1;
        case K::TK_TILDE:                             return 2;
        case K::TK_LBRACKET:
        case K::TK_RBRACKET:
        case K::TK_LANGLE:
        case K::TK_RANGLE:                            return 3;
        case K::TK_STAR:
        case K::TK_SLASH:
        case K::TK_BANG:                              return 4;
        case K::TK_LPAREN:
        case K::TK_RPAREN:
        case K::TK_COMMA:                             return 5;
        case K::TK_ERROR:                             return 6;
        case K::TK_EOF:
        default:                                      return 0;
    }
}

} // namespace hathor::ui
