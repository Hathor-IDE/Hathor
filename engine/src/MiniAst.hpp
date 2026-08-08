// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_MINIAST_HPP
#define HATHOR_MINIAST_HPP

/**
 * MiniAst.hpp — internal AST for the mini-notation parser.
 *
 * This header is INTERNAL to engine/src/ and must NOT be included from
 * any public engine/include/hathor/ header.
 *
 * Requirement references: 5.1, 5.2
 */

#include <string>
#include <vector>
#include <memory>
#include <variant>

namespace hathor {

// Forward declaration so MiniNodePtr can reference MiniNode.
struct MiniNode;
using MiniNodePtr = std::unique_ptr<MiniNode>;

// ---------------------------------------------------------------------------
// AST node alternatives
// ---------------------------------------------------------------------------

/// A leaf token: "bd", "sn", "~", etc.
struct MiniAtom {
    std::string token;
    std::size_t sourceOffset = 0; ///< byte offset of this atom's token in the original source (B2)
};

/// A space-separated or [...] sequence — lowers to stepcat (equal weights).
struct MiniSeq {
    std::vector<MiniNodePtr> steps;
    bool                     bracketed = false; ///< true if written as [...]
};

/// A comma-separated sequence — lowers to stack (all play concurrently).
struct MiniStack {
    std::vector<MiniNodePtr> steps;
};

/// An <...> slow sequence — lowers to slowcat.
struct MiniSlowSeq {
    std::vector<MiniNodePtr> steps;
};

/// child*N — lowers to fast(N, child).
struct MiniFast {
    MiniNodePtr child;
    int         factor; ///< N > 0
};

/// child/N — lowers to slow(N, child).
struct MiniSlow {
    MiniNodePtr child;
    int         factor; ///< N > 0
};

/// child!N — lowers to fastcat of N copies.
struct MiniRep {
    MiniNodePtr child;
    int         count; ///< N > 0
};

/// child(pulses, steps[, rotation]) — lowers to euclid.
struct MiniEuclid {
    MiniNodePtr child;
    int         pulses;   ///< number of onsets (>= 0)
    int         steps;    ///< total steps (must be > 0)
    int         rotation; ///< rotation in steps (default 0)
};

// ---------------------------------------------------------------------------
// Polymorphic node
// ---------------------------------------------------------------------------

struct MiniNode
    : std::variant<MiniAtom, MiniSeq, MiniStack, MiniSlowSeq, MiniFast, MiniSlow, MiniRep, MiniEuclid>
{
    using variant::variant;
};

} // namespace hathor

#endif // HATHOR_MINIAST_HPP
