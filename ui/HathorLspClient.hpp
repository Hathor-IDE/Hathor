// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * HathorLspClient.hpp — JUCE-dependent LSP client that manages the Strudel
 * LSP server process and transports JSON-RPC messages over stdio.
 *
 * Architecture (AI-4):
 *
 *   HathorTab  →  HathorLspClient  →  LspJsonRpc  →  strudel-lsp-server.js (Node.js)
 *                                  →  LspMessageFramer
 *                                  →  LspCompletionLogic (+ LanguageMetadata fallback)
 *
 * Responsibilities:
 *   - Spawn and manage the Node.js LSP server process (strudel-lsp-server.cjs).
 *   - Transport: poll child process stdout/stdin via a JUCE Timer (non-blocking).
 *   - Serialize outgoing requests using LspJsonRpc.
 *   - Parse incoming responses/notifications using LspJsonRpc + LspMessageFramer.
 *   - Correlate responses by id → invoke completion/hover callbacks.
 *   - Merge LSP results with LanguageMetadata fallback (AI-3) via LspCompletionLogic.
 *
 * The JUCE-free protocol layer (LspMessageFramer, LspJsonRpc, LspCompletionLogic)
 * is fully unit-tested in hathor-ui-tests. This class is the JUCE glue layer
 * that adds process management and async I/O.
 *
 * Requirement references: AI-4, AI-3 decision #18
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "LspCompletionLogic.hpp"
#include "LspJsonRpc.hpp"
#include "LanguageMetadata.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace hathor::ui {

/**
 * Callback invoked when completion results are ready.
 * Receives the merged CompletionResult (LSP + metadata fallback).
 */
using CompletionCallback = std::function<void(const lsp::CompletionResult&)>;

/**
 * Callback invoked when hover results are ready.
 * Receives nullopt if no hover content is available.
 */
using HoverCallback = std::function<void(const std::optional<lsp::Hover>&)>;

/**
 * Callback invoked when signature help results are ready.
 */
using SignatureCallback = std::function<void(const std::optional<lsp::SignatureHelp>&)>;

/**
 * Callback invoked when diagnostics are published for a document.
 */
using DiagnosticsCallback = std::function<void(const std::string& uri,
                                                 const std::vector<lsp::Diagnostic>&)>;

/**
 * HathorLspClient
 *
 * Manages the LSP server lifecycle and message dispatch. Not a juce::Component —
 * it is a pure logic class with a juce::Timer for polling I/O.
 *
 * The client must be created on and used from the JUCE message thread.
 * All callbacks fire on the message thread.
 */
class HathorLspClient : private juce::Timer
{
public:
    /**
     * Construct the LSP client.
     *
     * @param serverScriptPath  Path to strudel-lsp-server.cjs (or .js).
     * @param nodeExePath       Path to the Node.js executable.
     * @param metadata          Pointer to loaded LanguageMetadata (optional,
     *                          for fallback). May be nullptr if metadata is
     *                          unavailable (LSP-only mode).
     */
    HathorLspClient(std::string serverScriptPath,
                    std::string nodeExePath,
                    const hathor::language::LanguageMetadata* metadata = nullptr,
                    const hathor::language::MetadataCompatibility* compatibility = nullptr);

    ~HathorLspClient();

    // Non-copyable, non-movable
    HathorLspClient(const HathorLspClient&) = delete;
    HathorLspClient& operator=(const HathorLspClient&) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /** Start the LSP server process and send the initialize request. */
    void start();

    /** Stop the LSP server process (sends shutdown + exit). */
    void stop();

    /** True if the LSP server process is running. */
    bool isRunning() const noexcept;

    // -----------------------------------------------------------------------
    // Document management
    // -----------------------------------------------------------------------

    /**
     * Notify the LSP server that a document is now open.
     * @param uri            Document URI (e.g. "file:///path/to/file.hathor").
     * @param text           Full document text.
     * @param languageId     "hathor" or "chuck".
     */
    void didOpenDocument(const std::string& uri,
                         const std::string& text,
                         const std::string& languageId);

    /**
     * Notify the LSP server that a document has changed.
     */
    void didChangeDocument(const std::string& uri,
                           int version,
                           const std::string& text);

    /**
     * Notify the LSP server that a document is being closed.
     */
    void didCloseDocument(const std::string& uri);

    // -----------------------------------------------------------------------
    // Requests
    // -----------------------------------------------------------------------

    /**
     * Request completions at the given position.
     * The callback is invoked (on the message thread) when the response arrives.
     */
    void requestCompletion(const std::string& uri,
                           int line, int character,
                           CompletionCallback callback);

    /**
     * Request hover information at the given position.
     */
    void requestHover(const std::string& uri,
                      int line, int character,
                      HoverCallback callback);

    /**
     * Request signature help at the given position.
     */
    void requestSignatureHelp(const std::string& uri,
                              int line, int character,
                              SignatureCallback callback);

    // -----------------------------------------------------------------------
    // Diagnostics callback registration
    // -----------------------------------------------------------------------

    /**
     * Install a callback that fires when the LSP server publishes diagnostics.
     * Called on the message thread.
     */
    void setDiagnosticsCallback(DiagnosticsCallback callback) { diagnosticsCb_ = std::move(callback); }

private:
    // -----------------------------------------------------------------------
    // juce::Timer — polls child process stdout for incoming messages
    // -----------------------------------------------------------------------

    static constexpr int kPollIntervalMs = 25;

    void timerCallback() override;

    // -----------------------------------------------------------------------
    // Internal: process management
    // -----------------------------------------------------------------------

    /** Build the command line arguments for the Node.js LSP server. */
    juce::StringArray buildCommandLine() const;

    /** Handle an incoming JSON-RPC message (response or notification). */
    void handleMessage(const lsp::IncomingMessage& msg);

    /** Handle a request response — match by id and invoke callback. */
    void handleResponse(int id, const nlohmann::json& result,
                        bool isError, int errorCode, const std::string& errorMsg);

    /** Handle a notification (e.g. publishDiagnostics). */
    void handleNotification(const std::string& method, const nlohmann::json& params);

    // -----------------------------------------------------------------------
    // Internal: sending
    // -----------------------------------------------------------------------

    /** Write a raw framed message to the server's stdin. */
    bool writeToStdin(const std::string& framedMessage);

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------

    std::string serverScriptPath_;
    std::string nodeExePath_;

    const hathor::language::LanguageMetadata* metadata_;
    const hathor::language::MetadataCompatibility* compatibility_;

    std::unique_ptr<juce::ChildProcess> process_;

    lsp::LspJsonRpc rpc_;
    lsp::LspMessageFramer framer_;

    // Pending callbacks keyed by request id
    struct PendingRequest
    {
        CompletionCallback   completionCb;
        HoverCallback        hoverCb;
        SignatureCallback    signatureCb;
        enum Type { Completion, Hover, Signature } type;
    };
    std::unordered_map<int, PendingRequest> pendingRequests_;

    DiagnosticsCallback diagnosticsCb_;

    // Document versions (for didChange tracking)
    std::unordered_map<std::string, int> docVersions_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HathorLspClient)
};

} // namespace hathor::ui
