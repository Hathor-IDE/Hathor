// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * GhostLlmClient.cpp — JUCE-dependent llm-ls client implementation.
 *
 * Spawns the llm-ls binary, transports JSON-RPC over stdio using
 * LspMessageFramer, and dispatches responses to callbacks.
 *
 * IPC strategy: POSIX pipes + fork/execvp on macOS/Linux.
 * juce::ChildProcess is insufficient because it only supports reading
 * stdout, not writing to stdin.
 *
 * Requirement references: AI-4
 */

#include "GhostLlmClient.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#if JUCE_WINDOWS
#error "GhostLlmClient is not yet implemented for Windows"
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace hathor::ui {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

GhostLlmClient::GhostLlmClient(std::string llmLsBinaryPath,
                               std::string configPath)
    : llmLsBinaryPath_(std::move(llmLsBinaryPath))
    , configPath_(std::move(configPath))
    , ghostEnabled_(lsp::GhostProviderResolver::isEnabled())
{
}

GhostLlmClient::~GhostLlmClient()
{
    stop();
}

bool GhostLlmClient::hasBinary() const noexcept
{
    if (llmLsBinaryPath_.empty())
        return false;
    // Phase 6.3: verify the binary actually exists on disk, not just that a
    // path string was provided.  A missing binary must be detectable so the UI
    // can distinguish "ghost text unavailable" from "no completion".
    return juce::File(llmLsBinaryPath_).existsAsFile();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void GhostLlmClient::start()
{
    if (!ghostEnabled_ || isRunning())
        return;

    if (!hasBinary())
    {
        if (ghostEnabled_)
            std::cerr << "[GhostLlmClient] GHOST_ENABLED=1 but llm-ls binary not found at "
                      << llmLsBinaryPath_ << " — ghost text disabled." << std::endl;
        return;
    }

    if (!launchProcess())
        return;

    // Brief grace period for the server to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Start polling timer for stdout I/O
    startTimer(kPollIntervalMs);

    // Send initialize request
    std::string initMsg = lsp::GhostJsonRpc::serializeInitialize();
    writeToStdin(initMsg);

    // Send initialized notification
    juce::MessageManager::callAsync([this]() {
        if (isRunning())
        {
            std::string notif = lsp::GhostJsonRpc::serializeInitialized();
            writeToStdin(notif);
        }
    });

    if (statusCb_)
        statusCb_(true);
}

void GhostLlmClient::stop()
{
    if (!isProcessAlive())
        return;

    // Send shutdown request
    std::string msg = lsp::GhostJsonRpc::serializeShutdown();
    writeToStdin(msg);

    // Send exit notification
    std::string exitMsg = lsp::GhostJsonRpc::serializeExit();
    writeToStdin(exitMsg);

    // Wait briefly for graceful shutdown
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    terminateProcess();

    stopTimer();

    // Clear pending requests
    pendingGhostRequests_.clear();

    if (statusCb_)
        statusCb_(false);
}

bool GhostLlmClient::isRunning() const noexcept
{
    return pid_ > 0 && isProcessAlive();
}

// ---------------------------------------------------------------------------
// Platform-specific process management (POSIX)
// ---------------------------------------------------------------------------

bool GhostLlmClient::launchProcess()
{
#ifndef _WIN32
    int stdinPipe[2];   // [0] = read end (child), [1] = write end (parent)
    int stdoutPipe[2];  // [0] = read end (parent), [1] = write end (child)

    if (pipe(stdinPipe) != 0 || pipe(stdoutPipe) != 0)
        return false;

    // Set non-blocking on stdout read end for timer-based polling
    int flags = fcntl(stdoutPipe[0], F_GETFL, 0);
    fcntl(stdoutPipe[0], F_SETFL, flags | O_NONBLOCK);

    pid_t pid = fork();
    if (pid < 0)
    {
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        return false;
    }

    if (pid == 0)
    {
        // Child process
        // Set stdin, stdout, stderr
        dup2(stdinPipe[0], STDIN_FILENO);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stdoutPipe[1], STDERR_FILENO);

        // Close all pipe ends in the child
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);

        // Build argument list
        // llm-ls accepts: llm-ls --port <N> --host 127.0.0.1
        // But we use stdio transport, so just run the binary directly
        // The binary should accept stdio mode when no port is specified
        std::vector<char*> args;
        args.push_back(const_cast<char*>(llmLsBinaryPath_.c_str()));
        if (!configPath_.empty())
        {
            args.push_back(const_cast<char*>("--config"));
            args.push_back(const_cast<char*>(configPath_.c_str()));
        }
        args.push_back(nullptr);

        execvp(llmLsBinaryPath_.c_str(), args.data());
        _exit(127);
    }

    // Parent process
    close(stdinPipe[0]);   // close child's stdin read end
    close(stdoutPipe[1]);  // close child's stdout write end

    pid_ = pid;
    stdinWrite_ = stdinPipe[1];   // write end for parent
    stdoutRead_ = stdoutPipe[0];  // read end for parent
    childStdinRead_ = stdinPipe[0];
    childStdoutWrite_ = stdoutPipe[1];

    return true;
#else
    return false;
#endif
}

void GhostLlmClient::terminateProcess()
{
#ifndef _WIN32
    if (pid_ > 0)
    {
        kill(pid_, SIGTERM);
        int status;
        waitpid(pid_, &status, WNOHANG);
        pid_ = -1;
    }
    if (stdinWrite_ >= 0)
    {
        close(stdinWrite_);
        stdinWrite_ = -1;
    }
    if (stdoutRead_ >= 0)
    {
        close(stdoutRead_);
        stdoutRead_ = -1;
    }
#else
    // Windows: unreachable — the #error at the top of this file prevents
    // compilation on Windows (out of beta scope; macOS/Linux only).
    // Intentional no-op on the non-Windows paths below.
#endif
}

bool GhostLlmClient::isProcessAlive() const noexcept
{
#ifndef _WIN32
    if (pid_ <= 0)
        return false;

    int status;
    pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == 0)
        return true;  // Still running
    if (result == pid_)
    {
        // Process exited
        return false;
    }
    return false;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Document management
// ---------------------------------------------------------------------------

void GhostLlmClient::didOpenDocument(const std::string& uri,
                                      const std::string& text,
                                      const std::string& languageId)
{
    if (!isRunning())
        return;

    auto [version, msg] = lsp::GhostJsonRpc::serializeDidOpen(uri, languageId, 1, text);
    docVersion_ = version;
    writeToStdin(msg);
}

void GhostLlmClient::didChangeDocument(const std::string& uri,
                                        int version,
                                        const std::string& text)
{
    if (!isRunning())
        return;

    docVersion_ = version;
    auto [ver, msg] = lsp::GhostJsonRpc::serializeDidChange(uri, version, text);
    writeToStdin(msg);
}

void GhostLlmClient::didCloseDocument(const std::string& uri)
{
    if (!isRunning())
        return;

    std::string msg = lsp::GhostJsonRpc::serializeDidClose(uri);
    writeToStdin(msg);
}

// ---------------------------------------------------------------------------
// Ghost completion requests
// ---------------------------------------------------------------------------

void GhostLlmClient::requestGhostCompletion(
    const lsp::GhostCompletionRequest& req,
    const std::string& requestId,
    GhostResponseCallback callback)
{
    if (!isRunning())
    {
        if (callback)
            callback(requestId, lsp::GhostCompletionResponse{});
        return;
    }

    nlohmann::json params = req.toJson();

    nlohmann::json msg = {
        {"jsonrpc", "2.0"},
        {"id", requestId},
        {"method", "llm-ls/getCompletions"},
        {"params", params}
    };

    std::string framed = lsp::LspMessageFramer::frameWrite(msg.dump());
    writeToStdin(framed);

    PendingGhostRequest pending;
    pending.callback = std::move(callback);
    pending.sentAtMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count());
    pendingGhostRequests_[requestId] = std::move(pending);
}

void GhostLlmClient::sendAccept(const lsp::AcceptCompletionParams& params)
{
    if (!isRunning())
        return;

    std::string msg = lsp::GhostJsonRpc::serializeAcceptCompletion(params);
    writeToStdin(msg);
}

void GhostLlmClient::sendReject(const lsp::RejectCompletionParams& params)
{
    if (!isRunning())
        return;

    std::string msg = lsp::GhostJsonRpc::serializeRejectCompletion(params);
    writeToStdin(msg);
}

// ---------------------------------------------------------------------------
// I/O — writing to stdin
// ---------------------------------------------------------------------------

bool GhostLlmClient::writeToStdin(const std::string& framedMessage)
{
    if (stdinWrite_ < 0)
        return false;

    std::size_t written = 0;
    const char* data = framedMessage.data();
    std::size_t remaining = framedMessage.size();

    while (remaining > 0)
    {
        ssize_t n = ::write(stdinWrite_, data + written, remaining);
        if (n <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                juce::Thread::sleep(1);
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(n);
        remaining -= static_cast<std::size_t>(n);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Timer poll — reads available output from the llm-ls process
// ---------------------------------------------------------------------------

void GhostLlmClient::timerCallback()
{
    if (stdoutRead_ < 0)
    {
        stopTimer();
        return;
    }

    char buffer[4096];
    ssize_t n = 0;
    while ((n = ::read(stdoutRead_, buffer, sizeof(buffer))) > 0)
    {
        framer_.feed(std::string_view(buffer, static_cast<std::size_t>(n)));

        // Extract and process complete messages
        while (auto msg = framer_.tryNextMessage())
        {
            std::string id;
            auto result = lsp::GhostJsonRpc::parseResponse(msg->body, id);
            if (result.has_value())
            {
                handleResponse(id, result.value(), result.value().contains("__error"));
            }
        }
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        // Read error — process likely died
        if (!isProcessAlive())
        {
            // Fire status callback
            if (statusCb_)
                statusCb_(false);

            // Fail all pending requests — deliver empty responses
            for (auto& [id, req] : pendingGhostRequests_)
            {
                if (req.callback)
                    req.callback(id, lsp::GhostCompletionResponse{});
            }
            pendingGhostRequests_.clear();

            stopTimer();
            pid_ = -1;
            close(stdinWrite_);
            stdinWrite_ = -1;
            close(stdoutRead_);
            stdoutRead_ = -1;
        }
    }

    // Check timeouts
    int64_t nowMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count());
    checkTimeout(nowMs);
}

// ---------------------------------------------------------------------------
// Message dispatch
// ---------------------------------------------------------------------------

void GhostLlmClient::handleResponse(const std::string& id,
                                     const nlohmann::json& result,
                                     bool isError)
{
    auto it = pendingGhostRequests_.find(id);
    if (it == pendingGhostRequests_.end())
        return;

    PendingGhostRequest req = std::move(it->second);
    pendingGhostRequests_.erase(it);

    if (isError)
    {
        if (req.callback)
            req.callback(id, lsp::GhostCompletionResponse{});
        return;
    }

    // Parse the llm-ls completion response
    auto ghostResponse = lsp::parseGhostCompletionResponse(result);
    if (!ghostResponse.has_value())
    {
        if (req.callback)
            req.callback(id, lsp::GhostCompletionResponse{});
        return;
    }

    // Deliver the raw response to the callback.
    // The caller (HathorTab via GhostCompletionLogic) will check staleness
    // via onGhostResponse() and build the final GhostResult.
    if (req.callback)
        req.callback(id, *ghostResponse);
}

void GhostLlmClient::checkTimeout(int64_t nowMs)
{
    // Check all pending requests for timeout
    std::vector<std::string> timedOut;

    for (const auto& [id, req] : pendingGhostRequests_)
    {
        if (nowMs - req.sentAtMs > 5000) // 5s timeout
        {
            timedOut.push_back(id);
        }
    }

    for (const auto& id : timedOut)
    {
        auto it = pendingGhostRequests_.find(id);
        if (it != pendingGhostRequests_.end())
        {
            auto req = std::move(it->second);
            pendingGhostRequests_.erase(it);
            if (req.callback)
                req.callback(id, lsp::GhostCompletionResponse{});
        }
    }
}

} // namespace hathor::ui
