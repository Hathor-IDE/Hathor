// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * HathorLspClient.cpp — implementation of the JUCE-dependent LSP client.
 *
 * IPC strategy: POSIX pipes + posix_spawn on macOS/Linux.
 * juce::ChildProcess is insufficient because it only supports reading
 * stdout, not writing to stdin.
 *
 * Requirement references: AI-4
 */

#include "HathorLspClient.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#if JUCE_WINDOWS
#error "HathorLspClient is not yet implemented for Windows"
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

HathorLspClient::HathorLspClient(std::string serverScriptPath,
                                 std::string nodeExePath,
                                 const hathor::language::LanguageMetadata* metadata,
                                 const hathor::language::MetadataCompatibility* compatibility)
    : serverScriptPath_(std::move(serverScriptPath))
    , nodeExePath_(std::move(nodeExePath))
    , metadata_(metadata)
    , compatibility_(compatibility)
{
}

HathorLspClient::~HathorLspClient()
{
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void HathorLspClient::start()
{
    if (isRunning())
        return;

    if (!launchProcess())
    {
        return;
    }

    // Brief grace period for the server to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Start polling timer for stdout I/O
    startTimer(kPollIntervalMs);

    // Send initialize request
    std::string initMsg = rpc_.serializeInitialize("file:///");
    writeToStdin(initMsg);

    // Small delay before sending initialized notification
    juce::MessageManager::callAsync([this]() {
        if (isRunning())
        {
            std::string notif = rpc_.serializeNotification("initialized", nlohmann::json::object());
            writeToStdin(notif);
        }
    });
}

void HathorLspClient::stop()
{
    if (!isProcessAlive())
        return;

    // Send shutdown request
    std::string msg = rpc_.serializeRequest("shutdown", nlohmann::json::object());
    writeToStdin(msg);

    // Send exit notification
    std::string exitMsg = rpc_.serializeNotification("exit", nlohmann::json::object());
    writeToStdin(exitMsg);

    // Wait briefly for graceful shutdown
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    terminateProcess();

    stopTimer();
}

bool HathorLspClient::isRunning() const noexcept
{
    return pid_ > 0 && isProcessAlive();
}

// ---------------------------------------------------------------------------
// Platform-specific process management (POSIX)
// ---------------------------------------------------------------------------

bool HathorLspClient::launchProcess()
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
        dup2(stdinPipe[0], STDIN_FILENO);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stdoutPipe[1], STDERR_FILENO);

        close(stdinPipe[0]);
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);

        std::vector<char*> args;
        args.push_back(const_cast<char*>(nodeExePath_.c_str()));
        args.push_back(const_cast<char*>(serverScriptPath_.c_str()));
        args.push_back(nullptr);

        execvp(nodeExePath_.c_str(), args.data());
        _exit(127);
    }

    // Parent process
    close(stdinPipe[0]);   // close child's stdin read end
    close(stdoutPipe[1]);  // close child's stdout write end

    pid_ = pid;
    stdinWrite_ = stdinPipe[1];   // write end for parent
    stdoutRead_ = stdoutPipe[0];  // read end for parent
    childStdinRead_ = stdinPipe[0];   // keep for cleanup (closed in child)
    childStdoutWrite_ = stdoutPipe[1];

    return true;
#else
    return false;
#endif
}

void HathorLspClient::terminateProcess()
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
    // Windows not implemented
#endif
}

bool HathorLspClient::isProcessAlive() const noexcept
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

juce::StringArray HathorLspClient::buildCommandLine() const
{
    juce::StringArray cmd;
    cmd.add(nodeExePath_);
    cmd.add(serverScriptPath_);
    return cmd;
}

// ---------------------------------------------------------------------------
// Document management
// ---------------------------------------------------------------------------

void HathorLspClient::didOpenDocument(const std::string& uri,
                                       const std::string& text,
                                       const std::string& languageId)
{
    if (!isRunning())
        return;

    docVersions_[uri] = 1;
    std::string msg = rpc_.serializeDidOpen(uri, languageId, 1, text);
    writeToStdin(msg);
}

void HathorLspClient::didChangeDocument(const std::string& uri,
                                         int version,
                                         const std::string& text)
{
    if (!isRunning())
        return;

    docVersions_[uri] = version;
    std::string msg = rpc_.serializeDidChange(uri, version, text);
    writeToStdin(msg);
}

void HathorLspClient::didCloseDocument(const std::string& uri)
{
    if (!isRunning())
        return;

    std::string msg = rpc_.serializeDidClose(uri);
    writeToStdin(msg);
    docVersions_.erase(uri);
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

void HathorLspClient::requestCompletion(const std::string& uri,
                                         int line, int character,
                                         CompletionCallback callback)
{
    if (!isRunning())
    {
        if (callback)
            callback({});
        return;
    }

    auto [id, msg] = rpc_.serializeCompletion(uri, line, character);
    writeToStdin(msg);

    PendingRequest req;
    req.type = PendingRequest::Completion;
    req.completionCb = std::move(callback);
    pendingRequests_[id] = std::move(req);
}

void HathorLspClient::requestHover(const std::string& uri,
                                    int line, int character,
                                    HoverCallback callback)
{
    if (!isRunning())
    {
        if (callback)
            callback(std::nullopt);
        return;
    }

    auto [id, msg] = rpc_.serializeHover(uri, line, character);
    writeToStdin(msg);

    PendingRequest req;
    req.type = PendingRequest::Hover;
    req.hoverCb = std::move(callback);
    pendingRequests_[id] = std::move(req);
}

void HathorLspClient::requestSignatureHelp(const std::string& uri,
                                            int line, int character,
                                            SignatureCallback callback)
{
    if (!isRunning())
    {
        if (callback)
            callback(std::nullopt);
        return;
    }

    auto [id, msg] = rpc_.serializeSignatureHelp(uri, line, character);
    writeToStdin(msg);

    PendingRequest req;
    req.type = PendingRequest::Signature;
    req.signatureCb = std::move(callback);
    pendingRequests_[id] = std::move(req);
}

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------

bool HathorLspClient::writeToStdin(const std::string& framedMessage)
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
// Timer poll — reads available output from the LSP server
// ---------------------------------------------------------------------------

void HathorLspClient::timerCallback()
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
            auto parsed = rpc_.parseIncoming(msg->body);
            if (parsed.has_value())
            {
                handleMessage(*parsed);
            }
        }
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        // Read error — process likely died
        if (!isProcessAlive())
        {
            stopTimer();
            pid_ = -1;
            close(stdinWrite_);
            stdinWrite_ = -1;
            close(stdoutRead_);
            stdoutRead_ = -1;
        }
    }
}

// ---------------------------------------------------------------------------
// Message dispatch
// ---------------------------------------------------------------------------

void HathorLspClient::handleMessage(const lsp::IncomingMessage& msg)
{
    switch (msg.type)
    {
        case lsp::IncomingMessage::Type::Response:
        {
            if (msg.response.id.has_value())
            {
                handleResponse(*msg.response.id,
                               msg.response.result,
                               msg.response.isError,
                               msg.response.errorCode,
                               msg.response.errorMessage);
            }
            break;
        }
        case lsp::IncomingMessage::Type::Notification:
        {
            handleNotification(msg.notification.method, msg.notification.params);
            break;
        }
        case lsp::IncomingMessage::Type::Request:
        {
            // Server requesting something from us — send empty response
            break;
        }
    }
}

void HathorLspClient::handleResponse(int id,
                                      const nlohmann::json& result,
                                      bool isError,
                                      int /*errorCode*/,
                                      const std::string& /*errorMsg*/)
{
    auto it = pendingRequests_.find(id);
    if (it == pendingRequests_.end())
        return;

    PendingRequest req = std::move(it->second);
    pendingRequests_.erase(it);

    if (isError)
    {
        switch (req.type)
        {
            case PendingRequest::Completion:
                if (req.completionCb)
                    req.completionCb({});
                break;
            case PendingRequest::Hover:
                if (req.hoverCb)
                    req.hoverCb(std::nullopt);
                break;
            case PendingRequest::Signature:
                if (req.signatureCb)
                    req.signatureCb(std::nullopt);
                break;
        }
        return;
    }

    switch (req.type)
    {
        case PendingRequest::Completion:
        {
            auto lspList = lsp::LspJsonRpc::parseCompletionList(result);

            lsp::CompletionResult merged;
            merged.isIncomplete = lspList.isIncomplete;

            for (const auto& item : lspList.items)
            {
                lsp::CompletionCandidate c;
                c.label = item.label;
                c.kind = item.kind.value_or(lsp::CompletionItemKind::Text);
                c.detail = item.detail.value_or("");
                c.documentation = item.documentation ? item.documentation->value : "";
                c.insertText = item.insertText.value_or(item.label);
                c.source = "lsp";
                merged.items.push_back(std::move(c));
            }

            if (req.completionCb)
                req.completionCb(merged);
            break;
        }
        case PendingRequest::Hover:
        {
            auto hover = lsp::LspJsonRpc::parseHover(result);
            if (req.hoverCb)
                req.hoverCb(hover);
            break;
        }
        case PendingRequest::Signature:
        {
            auto sig = lsp::LspJsonRpc::parseSignatureHelp(result);
            if (req.signatureCb)
                req.signatureCb(sig);
            break;
        }
    }
}

void HathorLspClient::handleNotification(const std::string& method,
                                          const nlohmann::json& params)
{
    if (method == "textDocument/publishDiagnostics")
    {
        auto [uri, diags] = lsp::LspJsonRpc::parseDiagnostics(params);

        // Merge with metadata-aware diagnostics if we have metadata
        std::vector<lsp::Diagnostic> mergedDiags = diags;
        if (metadata_ && compatibility_ && compatibility_->compatible)
        {
            // Document text would be needed for metadata diagnostics;
            // for now, pass LSP diagnostics through
            // The HathorTab can request metadata-augmented diagnostics separately.
        }

        if (diagnosticsCb_)
            diagnosticsCb_(uri, mergedDiags);
    }
}

} // namespace hathor::ui
