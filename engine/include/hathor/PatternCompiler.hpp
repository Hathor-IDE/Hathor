// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_PATTERNCOMPILER_HPP
#define HATHOR_PATTERNCOMPILER_HPP

#include "hathor/ParamMap.hpp"
#include "hathor/Pattern.hpp"

#include <string>

namespace hathor {

/**
 * PatternCompiler — lowers a Pattern<std::string> to a Pattern<ParamMap>.
 *
 * This is the seam between the pure pattern engine and the worker-thread
 * layer. Allocation (pre-sizing the inner event buffer) is performed once
 * at compile/set time on the worker thread. The resulting Pattern<ParamMap>
 * query path is allocation-free.
 *
 * Only the "s" (sample folder name) and "n" (sample index, default 0) keys
 * are populated here. Gain, speed, pan, begin, end, and cut are intentionally
 * absent — VoicePool applies its own defaults for missing keys.
 *
 * Requirement references: 7.1, 7.2, 7.3
 */

/**
 * Lower a Pattern<std::string> to a Pattern<ParamMap>.
 *
 * For each Event<std::string> produced by @p src, the resulting pattern
 * emits a corresponding Event<ParamMap> with:
 *   - keys::kS  set to the string value
 *   - keys::kN  set to int64_t{0}
 *
 * The per-cycle event budget (maxEventsPerCycle) is inherited from @p src.
 * A pre-allocated buffer of Event<std::string> is created at call time and
 * reused on every query, keeping the hot path allocation-free.
 *
 * If src.maxEventsPerCycle() exceeds 512 a warning is emitted to std::cerr.
 * This is a warning, not a hard error; the pattern is still returned.
 *
 * @param src  The source Pattern<std::string> to lower.
 * @returns    A Pattern<ParamMap> with the same maxEventsPerCycle as @p src.
 */
Pattern<ParamMap> lowerToParamMap(const Pattern<std::string>& src);

} // namespace hathor

#endif // HATHOR_PATTERNCOMPILER_HPP
