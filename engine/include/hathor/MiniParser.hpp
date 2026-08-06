// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_MINIPARSER_HPP
#define HATHOR_MINIPARSER_HPP

/**
 * MiniParser.hpp — public API for the mini-notation parser.
 *
 * Exposes parseMini(string_view) which returns either a CompiledPattern
 * (a Pattern<std::string> plus its AST for pretty-printing) or a ParseError.
 *
 * The internal AST type MiniNode is defined in engine/src/MiniAst.hpp
 * (not part of the public API). CompiledPattern uses a forward-declared
 * MiniNode with an out-of-line destructor (defined in MiniParser.cpp where
 * MiniAst.hpp is included) so that this header stays free of internal
 * implementation details.
 *
 * Requirement references: 5.1, 5.2, 5.3, 5.6
 */

#include "hathor/Pattern.hpp"

#include <string>
#include <string_view>
#include <variant>
#include <memory>
#include <cstddef>

namespace hathor {

// Forward declaration of the internal AST node type.
// Full definition lives in engine/src/MiniAst.hpp (internal only).
struct MiniNode;

// ---------------------------------------------------------------------------
// ParseError
// ---------------------------------------------------------------------------

/**
 * Returned by parseMini() when the input string is not valid mini-notation.
 * Contains the byte position of the first error and a human-readable message.
 *
 * parseMini() never throws for user-input parse errors — all errors are
 * communicated via this type.
 *
 * Requirement: 5.3
 */
struct ParseError {
    std::size_t position; ///< byte offset of the first error in the input
    std::string message;  ///< human-readable description
};

// ---------------------------------------------------------------------------
// CompiledPattern
// ---------------------------------------------------------------------------

/**
 * A successfully parsed and compiled pattern, bundled with its AST so that
 * PrettyPrinter can serialise it back to canonical mini-notation text.
 *
 * Move-only (the AST contains unique_ptr children that cannot be cheaply
 * copied).
 *
 * Requirement: 5.4 (AST kept for pretty-printer)
 */
struct CompiledPattern {
    Pattern<std::string>   pattern; ///< the compiled, queryable pattern
    std::unique_ptr<MiniNode> ast;  ///< AST root (never nullptr on success)

    // Out-of-line special members so MiniNode can remain incomplete here.
    CompiledPattern();
    ~CompiledPattern();
    CompiledPattern(CompiledPattern&&) noexcept;
    CompiledPattern& operator=(CompiledPattern&&) noexcept;

    // Non-copyable — unique_ptr children make copy semantics expensive.
    CompiledPattern(const CompiledPattern&)            = delete;
    CompiledPattern& operator=(const CompiledPattern&) = delete;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Parse a mini-notation string and compile it to a Pattern<std::string>.
 *
 * On success: returns a CompiledPattern with the compiled pattern and its AST.
 * On failure: returns a ParseError with the byte position and error message.
 *
 * Never throws for user-input errors.
 *
 * Requirement: 5.1, 5.2, 5.3, 5.6
 */
std::variant<CompiledPattern, ParseError> parseMini(std::string_view input);

} // namespace hathor

#endif // HATHOR_MINIPARSER_HPP
