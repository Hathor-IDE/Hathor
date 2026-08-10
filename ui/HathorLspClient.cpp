// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * HathorLspClient.cpp — implementation of the JUCE-dependent LSP client.
 *
 * Requirement references: AI-4
 */

#include "HathorLspClient.hpp"
#include "LspProtocol.hpp"

#include <nlohmann/json.hpp>

#include <sstream>

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

    process_ = std::make_unique<juce::ChildProcess>();

    juce::StringArray cmd = buildCommandLine();
    if (!process_->start(cmd))
    {
        process_.reset();
        return;
    }

    // Wait briefly for the server to start
    juce::UInt64 startTime = juce::Time::getMillisecondCounter();
    while (!process_->isRunning() &&
           juce::Time::getMillisecondCounter() - startTime < 1000)
    {
        juce::Thread::sleep(10);
    }

    // Start polling timer for stdin/stdout I/O
    startTimer(kPollIntervalMs);

    // Send initialize request (no response needed — we don't block on it)
    std::string initMsg = rpc_.serializeInitialize("file:///");
    writeToStdin(initMsg);

    // Small delay before sending initialized notification
    juce::MessageManager::callAsync([this]() {
        if (process_ && process_->isRunning())
        {
            std::string notif = rpc_.serializeNotification("initialized", nlohmann::json::object());
            writeToStdin(notif);
        }
    });
}

void HathorLspClient::stop()
{
    if (!isRunning())
        return;

    // Send shutdown request
    int reqId = 0;
    {
        nlohmann::json params = nlohmann::json::object();
        std::string msg = rpc_.serializeRequest("shutdown", params);
        writeToStdin(msg);
    }

    // Send exit notification
    std::string exitMsg = rpc_.serializeNotification("exit", nlohmann::json::object());
    writeToStdin(exitMsg);

    // Wait briefly for graceful shutdown
    if (process_)
    {
        process_->waitForProcessFinished(2000);
    }

    stopTimer();
    process_.reset();
}

bool HathorLspClient::isRunning() const noexcept
{
    return process_ != nullptr && process_->isRunning();
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
    if (!process_ || !process_->isRunning())
        return false;

    return process_->writeProcessInput(framedMessage.data(), framedMessage.size())
        == static_cast<int>(framedMessage.size());
}

// ---------------------------------------------------------------------------
// Timer poll — reads available output from the LSP server
// ---------------------------------------------------------------------------

void HathorLspClient::timerCallback()
{
    if (!process_ || !process_->isRunning())
    {
        // Process died — stop polling and clean up
        stopTimer();
        process_.reset();
        return;
    }

    // Read available output (up to 4KB per poll)
    char buffer[4096];
    int bytesRead = process_->readProcessOutput(buffer, sizeof(buffer));

    if (bytesRead > 0)
    {
        framer_.feed(std::string_view(buffer, static_cast<std::size_t>(bytesRead)));

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
            // (we don't expect requests from the LSP server in this configuration)
            break;
        }
    }
}

void HathorLspClient::handleResponse(int id,
                                      const nlohmann::json& result,
                                      bool isError,
                                      int errorCode,
                                      const std::string& errorMsg)
{
    auto it = pendingRequests_.find(id);
    if (it == pendingRequests_.end())
        return; // Unknown id — could be a response to initialize

    PendingRequest req = std::move(it->second);
    pendingRequests_.erase(it);

    if (isError)
    {
        // On error, invoke callback with empty result
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

            // Merge with LSP completion results
            // Need document text for context analysis — we don't have it here,
            // so we pass an empty context for now (the caller can enrich later).
            // Actually, for L1 completion, we need the context. But the text
            // isn't available in this callback. The HathorTab that initiated
            // the request has the text. So the callback should be a bridge.
            //
            // For now, parse the LSP result into CompletionResult.
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

        // Merge with metadata-aware diagnostics if we have the document text
        // We don't have the text here, but we can still send the LSP diagnostics
        // The HathorTab can request metadata-augmented diagnostics separately.
        if (diagnosticsCb_)
            diagnosticsCb_(uri, diags);
    }
}

} // namespace hathor::ui
