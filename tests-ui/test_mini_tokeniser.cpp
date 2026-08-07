// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_mini_tokeniser.cpp — Property test P3 for MiniNotationTokeniser.
 *
 * P3: Colour-kind bijection — the TokenKind sequence from hathor::tokenise()
 *     and the colour-index sequence from MiniNotationTokeniser agree at every
 *     index i: colourOf(kinds[i]) == colours[i] (Req 27.4).
 *
 * Requirements: 27.4
 *
 * NOTE: Full property test implementation is in Task 2.3.
 *       This stub exists so the hathor-ui-tests CMake target compiles.
 */

#include <catch2/catch_test_macros.hpp>
#include "hathor/MiniTokeniser.hpp"

// Stub — replaced by Task 2.3
TEST_CASE("MiniNotationTokeniser colour-kind bijection stub", "[tokeniser][stub]") {
    // Smoke-test: tokenise a simple pattern and check it doesn't crash.
    auto tokens = hathor::tokenise("bd sn hh cp");
    REQUIRE(!tokens.empty());
    REQUIRE(tokens.back().kind == hathor::TokenKind::TK_EOF);
}
