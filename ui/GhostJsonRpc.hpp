// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * GhostJsonRpc.hpp — JUCE-free JSON-RPC serialization for llm-ls.
 *
 * Extends the existing LspMessageFramer + LspJsonRpc infrastructure to
 * handle llm-ls's custom LSP method `llm-ls/getCompletions` and its
 * associated notifications.
 *
 * This class is JUCE-free and compiles into both hathor-ui and hathor-ui-tests.
 *
 * Requirement references: AI-4
 */

#include "GhostProtocol.hpp"
#include "LspMessageFramer.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace hathor::lsp {

/**
 * GhostJsonRpc
 *
 * Serializes outgoing llm-ls JSON-RPC messages and parses incoming responses.
 * Uses LspMessageFramer for the Content-Length wire format (same as standard LSP).
 *
 * This class does NOT do I/O — it only handles serialization and parsing.
 * The JUCE-dependent GhostLlmClient handles process management and calls
 * into this class for (de)serialization.
 */
class GhostJsonRpc
{
public:
    GhostJsonRpc() = default;
    ~GhostJsonRpc() = default;

    // -----------------------------------------------------------------------
    // Outgoing message serialization
    // -----------------------------------------------------------------------

    /**
     * Serialize an initialize request for llm-ls.
     * llm-ls supports standard LSP initialize; we advertise FIM capability.
     */
    static std::string serializeInitialize();

    /**
     * Serialize the `initialized` notification.
     */
    static std::string serializeInitialized();

    /**
     * Serialize a `llm-ls/getCompletions` request with FIM context.
     * @return Pair of (requestId, framedMessage).
     */
    static std::pair<std::string, std::string> serializeGhostCompletion(
        const GhostCompletionRequest& req);

    /**
     * Serialize the `llm-ls/acceptCompletion` notification.
     */
    static std::string serializeAcceptCompletion(const AcceptCompletionParams& params);

    /**
     * Serialize the `llm-ls/rejectCompletion` notification.
     */
    static std::string serializeRejectCompletion(const RejectCompletionParams& params);

    /**
     * Serialize a `textDocument/didOpen` notification with the given languageId.
     * Used by GhostLlmClient to keep document state synchronized with llm-ls.
     */
    static std::pair<int, std::string> serializeDidOpen(
        std::string_view uri,
        std::string_view languageId,
        int version,
        std::string_view text);

    /**
     * Serialize a `textDocument/didChange` notification.
     */
    static std::pair<int, std::string> serializeDidChange(
        std::string_view uri,
        int version,
        std::string_view text);

    /**
     * Serialize a `textDocument/didClose` notification.
     */
    static std::string serializeDidClose(std::string_view uri);

    /**
     * Serialize a shutdown/exit sequence.
     */
    static std::string serializeShutdown();
    static std::string serializeExit();

    // -----------------------------------------------------------------------
    // Incoming message parsing
    // -----------------------------------------------------------------------

    /**
     * Parse a JSON string into a JSON object for further inspection.
     * Returns std::nullopt if the message is not valid JSON or JSON-RPC.
     */
    static std::optional<nlohmann::json> parseJson(std::string_view jsonStr);

    /**
     * Parse a JSON-RPC response and extract the result JSON.
     * Returns std::nullopt if not a valid response.
     */
    static std::optional<nlohmann::json> parseResponse(
        std::string_view jsonStr,
        std::string& outId);

    // -----------------------------------------------------------------------
    // Request ID generation
    // -----------------------------------------------------------------------

    /**
     * Generate a UUID v4 string for request correlation.
     * llm-ls uses string IDs (not integer IDs like standard LSP).
     * Public so GhostCompletionLogic can generate IDs for its pending-request
     * tracking before the client serializes the request.
     */
    static std::string generateRequestId();
};

} // namespace hathor::lsp