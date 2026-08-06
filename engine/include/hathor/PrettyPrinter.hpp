// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_PRETTYPRINTER_HPP
#define HATHOR_PRETTYPRINTER_HPP

/**
 * PrettyPrinter.hpp — serialise a CompiledPattern back to canonical
 * mini-notation text.
 *
 * Requirement references: 5.4, 5.5
 */

#include "hathor/MiniParser.hpp"

#include <string>

namespace hathor {

/**
 * Serialise a CompiledPattern to its canonical mini-notation string.
 *
 * The output is a round-trip-stable representation: for any valid input s,
 *   parseMini(printMini(parseMini(s)))
 * produces the same query results as parseMini(s).
 *
 * Returns an empty string if cp.ast is nullptr.
 *
 * Requirement: 5.4
 */
std::string printMini(const CompiledPattern& cp);

} // namespace hathor

#endif // HATHOR_PRETTYPRINTER_HPP
