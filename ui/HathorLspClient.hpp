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
#include "hathor/LanguageMetadata.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace hathor::ui {

using CompletionCallback = std::function<void(const lsp::CompletionResult&)>;
using HoverCallback = std::function<void(const std::optional<lsp::Hover>&)>;
using SignatureCallback = std::function<void(const std::optional<lsp::SignatureHelp>&)>;
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
    HathorLspClient(std::string serverScriptPath,
                    std::string nodeExePath,
                    const hathor::language::LanguageMetadata* metadata = nullptr,
                    const hathor::language::MetadataCompatibility* compatibility = nullptr);

    ~HathorLspClient();

    // Lifecycle
    void start();
    void stop();
    bool isRunning() const noexcept;

    // Document management
    void didOpenDocument(const std::string& uri,
                         const std::string& text,
                         const std::string& languageId);
    void didChangeDocument(const std::string& uri,
                           int version,
                           const std::string& text);
    void didCloseDocument(const std::string& uri);

    // Requests
    void requestCompletion(const std::string& uri,
                           int line, int character,
                           CompletionCallback callback);
    void requestHover(const std::string& uri,
                      int line, int character,
                      HoverCallback callback);
    void requestSignatureHelp(const std::string& uri,
                              int line, int character,
                              SignatureCallback callback);

    // Diagnostics callback registration
    void setDiagnosticsCallback(DiagnosticsCallback callback) { diagnosticsCb_ = std::move(callback); }

    // Accessors for HathorTab
    const hathor::language::LanguageMetadata* metadata() const noexcept { return metadata_; }
    const hathor::language::MetadataCompatibility* compatibility() const noexcept { return compatibility_; }

private:
    // juce::Timer — polls child process stdout for incoming messages
    static constexpr int kPollIntervalMs = 25;
    void timerCallback() override;

    juce::StringArray buildCommandLine() const;
    void handleMessage(const lsp::IncomingMessage& msg);
    void handleResponse(int id, const nlohmann::json& result,
                        bool isError, int /*errorCode*/, const std::string& /*errorMsg*/);
    void handleNotification(const std::string& method, const nlohmann::json& params);

    bool writeToStdin(const std::string& framedMessage);

    // Platform-specific process management (POSIX)
    bool launchProcess();
    void terminateProcess();
    bool isProcessAlive() const noexcept;

    std::string serverScriptPath_;
    std::string nodeExePath_;

    const hathor::language::LanguageMetadata* metadata_;
    const hathor::language::MetadataCompatibility* compatibility_;

#if JUCE_WINDOWS
    // Windows implementation would use CreateProcess + anonymous pipes
    // Not implemented for this platform
    void* stdinWrite_ = nullptr;
    void* stdoutRead_ = nullptr;
    int pid_ = -1;
#else
    int stdinWrite_ = -1;   ///< write end of child's stdin pipe
    int stdoutRead_ = -1;   ///< read end of child's stdout pipe
    int pid_ = -1;
    int childStdinRead_ = -1;  ///< saved for cleanup
    int childStdoutWrite_ = -1;
#endif

    lsp::LspJsonRpc rpc_;
    lsp::LspMessageFramer framer_;

    struct PendingRequest
    {
        CompletionCallback   completionCb;
        HoverCallback        hoverCb;
        SignatureCallback    signatureCb;
        enum Type { Completion, Hover, Signature } type;
    };
    std::unordered_map<int, PendingRequest> pendingRequests_;

    DiagnosticsCallback diagnosticsCb_;
    std::unordered_map<std::string, int> docVersions_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HathorLspClient)
};

} // namespace hathor::ui
