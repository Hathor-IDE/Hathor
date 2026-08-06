// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * Commands.hpp — protocol primitives for the ACP control interface.
 *
 * Provides the thread-safe respond() free function that serialises a
 * nlohmann::json value to a single line on stdout.
 *
 * Rules (Req 12.2, 16.1, 16.4):
 *   - stdout carries JSON only; every message is one line terminated by '\n'.
 *   - All diagnostic/debug output goes to stderr.
 *   - respond() is protected by a mutex so it is safe to call from both the
 *     main thread and the WorkerThread's onComplete callback.
 */

#include <nlohmann/json.hpp>

#include <cstdio>
#include <mutex>

namespace hathor::control {

/// The mutex that serialises all writes to stdout.
/// Defined in ControlInterface.cpp; declared here so WorkerThread.cpp
/// can also call respond() safely.
extern std::mutex g_stdoutMutex;

/**
 * Serialise @p j to a single JSON line on stdout and flush.
 *
 * Thread-safe: acquires g_stdoutMutex before writing.
 *
 * Requirement: 12.2, 16.1, 16.4
 */
inline void respond(const nlohmann::json& j)
{
    std::lock_guard<std::mutex> lock(g_stdoutMutex);
    // Write the serialised JSON followed by a newline, then flush stdout so
    // the consumer receives the message immediately.  All output goes through
    // this function — stdout is never written elsewhere.
    const std::string text = j.dump();
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace hathor::control
