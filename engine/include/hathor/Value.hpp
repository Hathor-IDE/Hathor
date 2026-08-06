// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_VALUE_HPP
#define HATHOR_VALUE_HPP

#include <cstdint>
#include <string>
#include <variant>

namespace hathor {

/**
 * Polymorphic event payload type.
 *
 * Holds exactly one of: a 64-bit float, a UTF-8 string, or a 64-bit integer.
 *
 * Requirement references: 6.1
 */
using Value = std::variant<double, std::string, int64_t>;

} // namespace hathor

#endif // HATHOR_VALUE_HPP
