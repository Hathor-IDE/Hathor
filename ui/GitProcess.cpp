// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * GitProcess.cpp — L-5: async Git command runner implementation.
 *
 * Delegates to the existing TerminalProcess for subprocess lifecycle
 * management (posix_spawn + lock-free SPSC ring + worker thread), adding
 * only async-completion-callback dispatch.
 *
 * Requirement references: L-5 §Concurrency / Audio Safety
 */

#include "GitProcess.hpp"

#include <algorithm>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GitProcess::GitProcess() = default;

GitProcess::~GitProcess()
{
    cancel();
    process_.shutdown();

    if (asyncThread_.joinable())
        asyncThread_.join();
}

// ---------------------------------------------------------------------------
// Sync run
// ---------------------------------------------------------------------------

GitProcess::CompletionResult
GitProcess::runSync(const std::vector<std::string>& argv,
                    const std::string& cwd,
                    int timeoutMs)
{
    cancelled_.store(false, std::memory_order_release);

    // Prepend "git" to the argv if not already present.
    std::vector<std::string> fullArgv = argv;
    if (fullArgv.empty() || fullArgv[0] != "git")
        fullArgv.insert(fullArgv.begin(), "git");

    CompletionResult result;

    // Build the command string for diagnostics.
    std::string cmdStr;
    for (const auto& a : fullArgv)
    {
        if (!cmdStr.empty())
            cmdStr += ' ';
        cmdStr += a;
    }
    result.command = cmdStr;

    if (!process_.launch(fullArgv, cwd))
    {
        result.exitCode = -1;
        result.output = process_.lastError();
        return result;
    }

    // Wait for exit (blocks calling thread — caller must NOT be on audio thread).
    auto exitStatus = process_.waitForExit(timeoutMs);

    // Drain any remaining output.
    char buf[4096];
    std::string allOutput;
    for (int i = 0; i < 200; ++i)
    {
        std::size_t n = process_.drainOutput(buf, sizeof(buf));
        if (n > 0)
            allOutput.append(buf, n);
        else
            break;
    }

    if (exitStatus)
    {
        result.exitCode = exitStatus->exitCode;
        result.output = std::move(allOutput);
        result.timedOut = false;
    }
    else
    {
        result.timedOut = true;
        result.exitCode = -1;
        process_.cancel();
    }

    return result;
}

// ---------------------------------------------------------------------------
// Async run
// ---------------------------------------------------------------------------

bool GitProcess::runAsync(const std::vector<std::string>& argv,
                          const std::string& cwd,
                          std::function<void(const CompletionResult&)> onCompleted,
                          int timeoutMs)
{
    if (isRunning())
        return false;

    cancelled_.store(false, std::memory_order_release);
    pendingCallback_ = std::move(onCompleted);

    // Prepend "git" if not present.
    std::vector<std::string> fullArgv = argv;
    if (fullArgv.empty() || fullArgv[0] != "git")
        fullArgv.insert(fullArgv.begin(), "git");

    // Build the command string for diagnostics.
    std::string cmdStr;
    for (const auto& a : fullArgv)
    {
        if (!cmdStr.empty())
            cmdStr += ' ';
        cmdStr += a;
    }

    if (!process_.launch(fullArgv, cwd))
    {
        CompletionResult result;
        result.exitCode = -1;
        result.output = process_.lastError();
        result.command = cmdStr;

        if (pendingCallback_)
        {
            auto cb = std::move(pendingCallback_);
            auto r  = std::move(result);
            cb(r);  // invoked on worker thread
        }
        return false;
    }

    // Launch the async worker thread.
    asyncThread_ = std::thread([this, cmdStr, timeoutMs]() mutable {
        auto exitStatus = process_.waitForExit(timeoutMs);

        // Drain output.
        char buf[4096];
        std::string allOutput;
        for (int i = 0; i < 200; ++i)
        {
            std::size_t n = process_.drainOutput(buf, sizeof(buf));
            if (n > 0)
                allOutput.append(buf, n);
            else
                break;
        }

        CompletionResult result;
        result.command = cmdStr;

        if (exitStatus)
        {
            result.exitCode = exitStatus->exitCode;
            result.output = std::move(allOutput);
            result.timedOut = false;
        }
        else
        {
            result.timedOut = true;
            result.exitCode = -1;
            process_.cancel();
        }

        // Invoke callback on the worker thread.
        // Callers on the JUCE message thread must marshal this themselves.
        if (pendingCallback_)
        {
            auto cb = std::move(pendingCallback_);
            auto r  = std::move(result);
            cb(r);
        }
    });
    asyncThread_.detach();

    return true;
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

bool GitProcess::isRunning() const noexcept
{
    return process_.state() == TerminalProcess::State::Running;
}

void GitProcess::cancel()
{
    cancelled_.store(true, std::memory_order_release);
    process_.cancel();
}

} // namespace hathor::ui
