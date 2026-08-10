// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * GhostLlmClient.hpp — JUCE-dependent llm-ls client.
 *
 * Manages the llm-ls (Rust LSP server) process and transports JSON-RPC
 * messages over stdio. Mirrors the HathorLspClient pattern (POSIX fork/exec
 * + pipe + JUCE Timer polling) but for llm-ls's custom `llm-ls/getCompletions`
 * method.
 *
 * IPC strategy: POSIX pipes + fork/execvp on macOS/Linux.
 * (Windows not yet implemented — same as HathorLspClient.)
 *
 * Architecture (Hathor ghost text):
 *
 *   HathorTab → GhostLlmClient (this file) → LspMessageFramer
 *                            → GhostJsonRpc (JUCE-free serialization)
 *                            → GhostCompletionLogic (JUCE-free lifecycle)
 *                            → GhostTextOverlay (JUCE rendering)
 *
 * Requirement references: AI-4
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "GhostJsonRpc.hpp"
#include "GhostProviderConfig.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace hathor::ui {

using GhostResponseCallback = std::function<void(const std::string& requestId,
                                                    const lsp::GhostCompletionResponse& resp)>;
using GhostStatusCallback = std::function<void(bool connected)>;

/**
 * GhostLlmClient
 *
 * Manages the llm-ls process lifecycle and message dispatch for ghost text.
 * Not a juce::Component — it is a pure logic class with a juce::Timer for
 * polling I/O.
 *
 * The client must be created on and used from the JUCE message thread.
 * All callbacks fire on the message thread.
 *
 * Key difference from HathorLspClient:
 *   - llm-ls uses string request IDs (UUIDs), not integer IDs
 *   - llm-ls has no `$/cancelRequest` — cancellation is client-side
 *   - The completion method is `llm-ls/getCompletions`, not `textDocument/completion`
 */
class GhostLlmClient : private juce::Timer
{
public:
    GhostLlmClient(std::string llmLsBinaryPath,
                   std::string configPath = {});

    ~GhostLlmClient();

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * Start the llm-ls process (if enabled via GHOST_ENABLED env var).
     * Does nothing if ghost text is disabled.
     */
    void start();

    /**
     * Stop the llm-ls process and clean up.
     */
    void stop();

    /** True if the llm-ls process is running. */
    bool isRunning() const noexcept;

    // -----------------------------------------------------------------------
    // Document management
    // -----------------------------------------------------------------------

    void didOpenDocument(const std::string& uri,
                         const std::string& text,
                         const std::string& languageId);
    void didChangeDocument(const std::string& uri,
                           int version,
                           const std::string& text);
    void didCloseDocument(const std::string& uri);

    // -----------------------------------------------------------------------
    // Ghost completion requests
    // -----------------------------------------------------------------------

    /**
     * Request a ghost completion at the given position.
     *
     * @param req   The fully built GhostCompletionRequest (with FIM context).
     * @param requestId   The UUID to correlate the response.
     * @param callback  Called on the JUCE message thread when a response
     *                  arrives. The callback receives the request ID and the
     *                  parsed GhostCompletionResponse. If the request fails
     *                  or times out, the callback is called with an empty
     *                  response (completions vector is empty).
     */
    void requestGhostCompletion(const lsp::GhostCompletionRequest& req,
                                const std::string& requestId,
                                GhostResponseCallback callback);

    /**
     * Notification: user accepted a ghost completion.
     */
    void sendAccept(const lsp::AcceptCompletionParams& params);

    /**
     * Notification: user rejected a ghost completion.
     */
    void sendReject(const lsp::RejectCompletionParams& params);

    // -----------------------------------------------------------------------
    // Callbacks
    // -----------------------------------------------------------------------

    /** Set callback fired when the llm-ls connection state changes. */
    void setStatusCallback(GhostStatusCallback cb) { statusCb_ = std::move(cb); }

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    /** True if ghost text is enabled (GHOST_ENABLED=1 in env). */
    bool isGhostEnabled() const noexcept { return ghostEnabled_; }

    /** True if the llm-ls binary was found at the expected path. */
    bool hasBinary() const noexcept { return !llmLsBinaryPath_.empty(); }

private:
    // -----------------------------------------------------------------------
    // juce::Timer — polls child process stdout for incoming messages
    // -----------------------------------------------------------------------
    static constexpr int kPollIntervalMs = 50;
    void timerCallback() override;

    // -----------------------------------------------------------------------
    // Platform-specific process management (POSIX)
    // -----------------------------------------------------------------------

    bool launchProcess();
    void terminateProcess();
    bool isProcessAlive() const noexcept;

    bool writeToStdin(const std::string& framedMessage);

    // -----------------------------------------------------------------------
    // Message dispatch
    // -----------------------------------------------------------------------

    void handleResponse(const std::string& id,
                        const nlohmann::json& result,
                        bool isError);

    void checkTimeout(int64_t nowMs);

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------

    std::string llmLsBinaryPath_;
    std::string configPath_;
    bool        ghostEnabled_ = false;

    // JUCE-free protocol layer
    lsp::LspMessageFramer framer_;

    int   docVersion_ = 1;

    // Pending ghost completion requests (keyed by UUID string)
    struct PendingGhostRequest {
        GhostResponseCallback callback;
        int64_t sentAtMs;
    };
    std::unordered_map<std::string, PendingGhostRequest> pendingGhostRequests_;

    // Process handles (POSIX)
    int stdinWrite_       = -1;
    int stdoutRead_       = -1;
    int pid_              = -1;
    int childStdinRead_   = -1;
    int childStdoutWrite_ = -1;

    GhostStatusCallback statusCb_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GhostLlmClient)
};

} // namespace hathor::ui
