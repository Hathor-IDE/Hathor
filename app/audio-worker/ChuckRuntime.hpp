// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ChuckRuntime.hpp — global libchuck thread-safety guards (B4-K4).
 *
 * libchuck is NOT fully thread-safe across ChucK instances.  Two documented
 * global-state hazards (verified against the vendored source in
 * build-test/_deps/chuck_src-src/src/core/chuck.cpp):
 *
 *   1. ChucK instance lifecycle: `ChucK::ChucK()` increments the static
 *      `o_numVMs` counter and lazily calls the non-locked `globalInit()`
 *      (plain static booleans).  Constructing/destroying ChucK instances
 *      concurrently from different threads is a data race.
 *
 *   2. Compiler error state: `EM_reset_msg()` / `EM_lasterror()` operate on
 *      the global `g_lasterror` buffer in chuck_errmsg.cpp, shared by ALL
 *      ChucK instances.  Concurrent `compileCode()` calls from different
 *      threads clobber each other's error capture.
 *
 * The B4-K0.5 NO-GO decision additionally forbids compileCode() concurrent
 * with run() on the SAME instance.  Hathor enforces that structurally:
 *   - each per-tab ChucK instance is created/init/started/run/compiled/
 *     destroyed exclusively on its own VM thread (between run() calls), and
 *   - ALL compileCode() calls across instances (VM-thread loads + dispatcher
 *     diagnostics) are serialized under the compile mutex below.
 *
 * Neither mutex is ever taken on the JUCE audio thread: the JUCE callback
 * only reads the shared-memory ring via AudioWorkerManager::tryReadAudioBlock.
 *
 * Requirements: B4-K4, B4-K0.5, B4-K3
 */

#include <mutex>

namespace hathor::audio_worker {

/// Serializes ChucK instance construction / init / start / destruction.
/// Guards libchuck's non-atomic static counters (o_numVMs, o_isGlobalInit).
inline std::mutex& chuckInstanceMutex()
{
    static std::mutex m;
    return m;
}

/// Serializes all ChucK::compileCode() calls across instances.
/// Guards libchuck's global error buffer (EM_reset_msg/EM_lasterror) and
/// satisfies K0.5's serialized-compilation constraint for the cross-instance
/// case (dispatcher diagnostics running concurrently with VM-thread loads).
inline std::mutex& chuckCompileMutex()
{
    static std::mutex m;
    return m;
}

} // namespace hathor::audio_worker
