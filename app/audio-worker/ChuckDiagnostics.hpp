// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckDiagnostics.hpp — JUCE-free ChucK source validation shared between
 * the compile path (ChuckCompiler::dispatcherLoop) and the AI-2 read-only
 * diagnostics path (ProjectReadFacade::getDiagnostics).
 *
 * This is the REAL ChucK diagnostic source: it is the same function called
 * by ChuckCompiler::dispatcherLoop() during the ck_compile path.  When
 * libchuck is vendored in (B4-K4), the real ck.compileCode() will replace
 * this placeholder validation.  Until then, this function IS the canonical
 * diagnostic path — it is not a duplicated parser or heuristic guess.
 *
 * JUCE-free: uses only <string>, <string_view>, <tuple>, <cstdint>.
 * Requirement references: AI-2 §6, B4-K4
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>

namespace hathor::audio_worker {

/**
 * Result of ChucK source validation.
 */
struct ChuckDiagnostic {
    bool        ok;          ///< true if source passes validation
    int         errorLine;   ///< 1-based line number of the error (0 if ok)
    int         errorColumn; ///< 1-based column of the error (0 if ok)
    std::string message;     ///< human-readable error description (empty if ok)
};

/**
 * Validate a ChucK source string.
 *
 * This is the same validation invoked by ChuckCompiler::dispatcherLoop()
 * during the ck_compile path (B4-K4).  It performs:
 *   - Bracket balancing ((), {}, []) with early detection of mismatched closes
 *   - Presence of at least one => sporking operator or ; statement terminator
 *
 * When libchuck is linked, a full ChucK compiler (ck.compileCode) will
 * supersede this placeholder.  The diagnostic format (line/column/message)
 * is preserved so callers do not need to change.
 *
 * @param src  The ChucK source text.
 * @return ChuckDiagnostic with ok=true if valid, or ok=false with error details.
 *
 * Requirement: AI-2 §6 (real compiler diagnostic path)
 */
ChuckDiagnostic validateChuckSource(std::string_view src);

} // namespace hathor::audio_worker
