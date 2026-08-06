// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * PrettyPrinter.cpp — serialise MiniNode AST back to canonical mini-notation.
 *
 * Requirement references: 5.4
 */

#include "hathor/PrettyPrinter.hpp"
#include "MiniAst.hpp"

#include <string>
#include <variant>

namespace hathor {

// ---------------------------------------------------------------------------
// Recursive tree walk
// ---------------------------------------------------------------------------

/// Print a single node. `nested` indicates whether this node is a child of
/// a modifier or bracket group (affects whether we wrap sequences in brackets).
static std::string printNode(const MiniNode& node, bool nested);

/// Join a vector of nodes with spaces.
static std::string joinSteps(const std::vector<MiniNodePtr>& steps, bool nested)
{
    std::string result;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (i > 0) result += ' ';
        result += printNode(*steps[i], nested);
    }
    return result;
}

static std::string printNode(const MiniNode& node, bool nested)
{
    return std::visit([nested](const auto& alt) -> std::string {
        using T = std::decay_t<decltype(alt)>;

        if constexpr (std::is_same_v<T, MiniAtom>) {
            return alt.token;
        }
        else if constexpr (std::is_same_v<T, MiniSeq>) {
            std::string inner = joinSteps(alt.steps, true);
            // Wrap in [...] if:
            // - explicitly bracketed (came from a [...] group in the source), OR
            // - nested as a child of a modifier/group (to preserve structure)
            if (alt.bracketed || nested) {
                return "[" + inner + "]";
            }
            return inner;
        }
        else if constexpr (std::is_same_v<T, MiniSlowSeq>) {
            std::string inner = joinSteps(alt.steps, true);
            return "<" + inner + ">";
        }
        else if constexpr (std::is_same_v<T, MiniFast>) {
            // child*N
            std::string childStr = printNode(*alt.child, true);
            return childStr + "*" + std::to_string(alt.factor);
        }
        else if constexpr (std::is_same_v<T, MiniSlow>) {
            // child/N
            std::string childStr = printNode(*alt.child, true);
            return childStr + "/" + std::to_string(alt.factor);
        }
        else if constexpr (std::is_same_v<T, MiniRep>) {
            // child!N
            std::string childStr = printNode(*alt.child, true);
            return childStr + "!" + std::to_string(alt.count);
        }
        else if constexpr (std::is_same_v<T, MiniEuclid>) {
            // child(pulses, steps[, rotation])
            std::string childStr = printNode(*alt.child, true);
            std::string result = childStr + "(" + std::to_string(alt.pulses)
                               + ", " + std::to_string(alt.steps);
            if (alt.rotation != 0)
                result += ", " + std::to_string(alt.rotation);
            result += ")";
            return result;
        }
        else {
            return std::string{};
        }
    }, static_cast<const std::variant<MiniAtom, MiniSeq, MiniSlowSeq, MiniFast, MiniSlow, MiniRep, MiniEuclid>&>(node));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string printMini(const CompiledPattern& cp)
{
    if (!cp.ast) return {};
    return printNode(*cp.ast, /*nested=*/false);
}

} // namespace hathor
