// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * SocketServer.hpp — Unix-socket accept/read loop for the MCP control path.
 *
 * Runs an `accept()`/`read()` loop against the Unix domain listener created by
 * AcpAgentSession ($TMPDIR/hathor-<pid>-<seq>.sock).  Each accepted connection
 * is read newline-delimited; every non-empty line is handed to a caller-provided
 * dispatcher, and the dispatcher's (possibly asynchronous) response is written
 * back to the client as a newline-terminated line.
 *
 * Thread model:
 *   - The loop is intended to run on a dedicated worker thread (never the JUCE
 *     message thread nor the audio thread).  It is poll()-based and checks the
 *     caller's stop flag each iteration, so it exits cleanly on shutdown even
 *     while blocked waiting for a connection.
 *   - Commands are serialised on this worker thread; a per-command response
 *     await is bounded by a timeout so a slow/stuck command cannot hang
 *     shutdown.
 *   - No JUCE symbols are used; control/ stays GUI-free.
 *
 * The dispatcher may invoke the response callback synchronously (bpm, play,
 * stop, set-gain, ping, list-patterns, clear-pattern) or asynchronously
 * (set-pattern, via the WorkerThread).  Its lifetime is scoped to a shared
 * per-command outcome object, so a late callback after the loop exits cannot
 * touch freed accept-loop stack state.
 *
 * Requirements: Phase 2.5 H0
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace hathor::control {

/// Receives a newline-delimited command line and must eventually invoke
/// @p response with the (JSON) line to send back to the client.  May be called
/// synchronously or asynchronously, and on any thread.
using SocketDispatcher = std::function<void(std::string command,
                                            std::function<void(std::string response)> response)>;

/**
 * Blocking accept/read loop.
 *
 * @param listenerFd The listening Unix socket fd (owned by the caller; not
 *                   closed by this function).
 * @param stop       Atomic flag; when true the loop exits promptly.
 * @param dispatcher Callback that turns a command line into a response line.
 *
 * Returns when @p stop is set or the listener is closed (EBADF / EOF).  All
 * accepted connection fds are closed before returning.
 */
void runSocketAcceptLoop(int listenerFd,
                         const std::atomic<bool>& stop,
                         SocketDispatcher dispatcher);

} // namespace hathor::control