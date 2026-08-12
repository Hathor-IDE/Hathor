// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * GitProcess.hpp — L-5: async Git command runner (JUCE-free).
 *
 * Runs `git` as a subprocess on a dedicated worker thread so that no Git
 * operation ever touches the JUCE message thread or the real-time audio
 * thread. Output is captured through TerminalProcess (which uses a lock-free
 * SPSC ring buffer), and completion is delivered via a callback.
 *
 * Threading boundary:
 *   - Caller thread (typically JUCE message thread): calls runSync() or
 *     runAsync(). runSync() blocks the calling thread (with timeout) — the
 *     caller MUST NOT be on the audio thread. runAsync() returns immediately.
 *   - Worker thread (owned by this object): runs the process, reads
 *     stdout/stderr, waits for exit.
 *   - Audio thread: NEVER touches this class.
 *
 * Design decision (L-5 §Architecture): We use the system `git` binary via
 * posix_spawn (through TerminalProcess) rather than linking libgit2, because:
 *   1. The system git is the canonical, mature Git implementation.
 *   2. The Hathor codebase already has the exact async-subprocess pattern
 *      (TerminalProcess) with proven real-time-safety properties.
 *   3. Avoids adding a heavy native dependency + build-system complexity
 *      that FetchContent(libgit2) would introduce.
 *
 * All Git *repository logic* (parsing status output into typed structures,
 * commit history, diffs, branch lists) lives in GitRepository — a separate,
 * JUCE-free data model. GitProcess is only the transport layer.
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "TerminalProcess.hpp"

namespace hathor::ui {

/**
 * GitProcess
 *
 * A thin async wrapper around the system `git` binary. Manages process
 * lifecycle, captures combined stdout/stderr, and invokes a completion
 * callback when the process exits.
 *
 * The completion callback is invoked on the worker thread — callers that
 * need it on the JUCE message thread must marshal it themselves (e.g. via
 * juce::MessageManager::callAsync or a juce::Timer-based poll).
 */
class GitProcess
{
public:
    /// Result of a Git command run.
    struct CompletionResult
    {
        int         exitCode = -1;      ///< git exit code (0 = success)
        std::string output;            ///< combined stdout + stderr
        bool        timedOut = false;  ///< true if the wait timed out
        std::string command;           ///< the git command that was run
    };

    GitProcess();
    ~GitProcess();

    GitProcess(const GitProcess&) = delete;
    GitProcess& operator=(const GitProcess&) = delete;

    // -----------------------------------------------------------------------
    // Sync run (blocks calling thread until exit, with timeout)
    // -----------------------------------------------------------------------

    /**
     * Run a git command synchronously on the calling thread (NOT the audio
     * thread). Returns the exit code and combined output.
     *
     * @param argv   Argument vector: may or may not start with "git".
     * @param cwd    Working directory (the repository root).
     * @param timeoutMs  Maximum milliseconds to wait (0 = no timeout).
     * @return CompletionResult with exit code, output, and timeout flag.
     */
    CompletionResult runSync(const std::vector<std::string>& argv,
                             const std::string& cwd,
                             int timeoutMs = 10000);

    // -----------------------------------------------------------------------
    // Async run (non-blocking; result delivered via callback)
    // -----------------------------------------------------------------------

    /**
     * Run a git command asynchronously. The command executes on a worker
     * thread; when it completes, onCompleted is invoked.
     *
     * The callback is invoked on a worker thread. If the caller needs the
     * callback on the JUCE message thread, it must marshal it (e.g. via
     * juce::MessageManager::callAsync or a polling timer).
     *
     * @param argv   Argument vector: may or may not start with "git".
     * @param cwd    Working directory (the repository root).
     * @param onCompleted  Callback invoked with the result.
     * @param timeoutMs  Maximum milliseconds to wait (0 = no timeout).
     * @return true if the command was launched, false if already running or
     *         launch failed.
     */
    bool runAsync(const std::vector<std::string>& argv,
                  const std::string& cwd,
                  std::function<void(const CompletionResult&)> onCompleted,
                  int timeoutMs = 30000);

    /** True if a command is currently in flight. */
    bool isRunning() const noexcept;

    /** Cancel and terminate the running process (if any). */
    void cancel();

    /** True if a cancellation was requested. */
    bool wasCancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }

    /** Current process state. Thread-safe. */
    TerminalProcess::State state() const noexcept { return process_.state(); }

    /** Get the last error message (if launch failed). */
    std::string lastError() const { return process_.lastError(); }

private:
    TerminalProcess process_;

    std::atomic<bool>     cancelled_{false};
    std::thread           asyncThread_;          ///< detaches after completion
    std::function<void(const CompletionResult&)> pendingCallback_;
};

} // namespace hathor::ui
