// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * LspJsonRpc.hpp — JUCE-free JSON-RPC 2.0 + LSP message dispatch.
 *
 * This module provides:
 *   - LspJsonRpc: a lightweight JSON-RPC 2.0 client/server message builder and
 *     parser, built on top of LspMessageFramer. It handles request/response
 *     correlation by id, serializes outgoing messages, and parses incoming
 *     messages into typed structures.
 *   - LspProtocolParser: static functions to parse LSP JSON responses into
 *     the typed structures defined in LspProtocol.hpp.
 *
 * All code is JUCE-free and depends only on the standard library and
 * nlohmann/json (already a project dependency). This makes the protocol
 * and completion logic fully testable in the hathor-ui-tests target.
 *
 * Requirement references: AI-4
 */

#include "LspProtocol.hpp"
#include "LspMessageFramer.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// JSON-RPC message types
// ---------------------------------------------------------------------------

/**
 * A JSON-RPC 2.0 request message ready to send.
 * The id may be an integer (our requests) or a string (server requests
 * use either); rawId preserves whichever form arrived so responses echo
 * it back verbatim.
 */
struct JsonRpcRequest {
    int                 id = 0;
    nlohmann::json      rawId = nullptr;
    std::string         method;
    nlohmann::json      params;
};

/**
 * A JSON-RPC 2.0 response — either a result or an error.
 */
struct JsonRpcResponse {
    std::optional<int>  id;
    nlohmann::json      result;
    bool                isError = false;
    int                 errorCode = 0;
    std::string         errorMessage;
};

/**
 * A JSON-RPC 2.0 notification (no response expected).
 */
struct JsonRpcNotification {
    std::string method;
    nlohmann::json params;
};

/**
 * A parsed incoming message — discriminated union of request, response, or notification.
 */
struct IncomingMessage {
    enum class Type { Request, Response, Notification };
    Type type;
    JsonRpcResponse   response;     ///< when type == Response
    JsonRpcRequest    request;      ///< when type == Request (server-side only)
    JsonRpcNotification notification; ///< when type == Notification
};

// ---------------------------------------------------------------------------
// LspJsonRpc — message serialization + deserialization
// ---------------------------------------------------------------------------

/**
 * LspJsonRpc
 *
 * Serializes outgoing JSON-RPC messages and parses incoming ones.
 * Uses LspMessageFramer for the Content-Length wire format.
 *
 * This class does NOT do I/O — it only handles serialization and parsing.
 * The JUCE-dependent HathorLspClient handles process I/O and calls into
 * this class for (de)serialization.
 */
class LspJsonRpc
{
public:
    LspJsonRpc() = default;
    ~LspJsonRpc() = default;

    // -----------------------------------------------------------------------
    // Outgoing message serialization
    // -----------------------------------------------------------------------

    /**
     * Serialize a request as a framed LSP message string (ready to write to stdio).
     * The id is assigned from nextId_.
     */
    std::string serializeRequest(std::string_view method, const nlohmann::json& params);

    /**
     * Serialize a notification as a framed LSP message string.
     */
    std::string serializeNotification(std::string_view method, const nlohmann::json& params);

    /**
     * Serialize an initialize request with standard LSP client capabilities.
     */
    std::string serializeInitialize(std::string_view uri);

    /**
     * Serialize a textDocument/didOpen notification.
     */
    std::string serializeDidOpen(std::string_view uri,
                                  std::string_view languageId,
                                  int version,
                                  std::string_view text);

    /**
     * Serialize a textDocument/didChange notification (incremental, Full sync).
     */
    std::string serializeDidChange(std::string_view uri, int version, std::string_view text);

    /**
     * Serialize a textDocument/didClose notification.
     */
    std::string serializeDidClose(std::string_view uri);

    /**
     * Serialize a textDocument/completion request.
     * @return Pair of (requestId, framedMessage).
     */
    std::pair<int, std::string> serializeCompletion(std::string_view uri, int line, int character);

    /**
     * Serialize a textDocument/hover request.
     * @return Pair of (requestId, framedMessage).
     */
    std::pair<int, std::string> serializeHover(std::string_view uri, int line, int character);

    /**
     * Serialize a textDocument/signatureHelp request.
     * @return Pair of (requestId, framedMessage).
     */
    std::pair<int, std::string> serializeSignatureHelp(std::string_view uri, int line, int character);

    // -----------------------------------------------------------------------
    // Navigation request serialization (definition, references, rename, symbols)
    // -----------------------------------------------------------------------

    /**
     * Serialize a textDocument/definition request.
     * @return Pair of (requestId, framedMessage).
     */
    std::pair<int, std::string> serializeDefinition(std::string_view uri, int line, int character);

    /**
     * Serialize a textDocument/references request.
     * @param includeDeclaration If true, results include the declaration.
     * @return Pair of (requestId, framedMessage).
     */
    std::pair<int, std::string> serializeReferences(std::string_view uri, int line, int character,
                                                     bool includeDeclaration = true);

    /**
     * Serialize a textDocument/typeDefinition request.
     * @return Pair of (requestId, framedMessage).
     */
    std::pair<int, std::string> serializeTypeDefinition(std::string_view uri, int line, int character);

    /**
     * Serialize a textDocument/declaration request.
     * @return Pair of (requestId, framedMessage).
     */
    std::pair<int, std::string> serializeDeclaration(std::string_view uri, int line, int character);

    /**
     * Serialize a textDocument/rename request.
     * @return Pair of (requestId, framedMessage).
     */
    std::pair<int, std::string> serializeRename(std::string_view uri, int line, int character,
                                                 std::string_view newName);

    /**
     * Serialize a textDocument/documentSymbol request (flat, non-hierarchical).
     * @return Pair of (requestId, framedMessage).
     */
    std::pair<int, std::string> serializeDocumentSymbol(std::string_view uri);

    /**
     * Serialize a workspace/symbol request.
     * @return Pair of (requestId, framedMessage).
     */
    std::pair<int, std::string> serializeWorkspaceSymbol(std::string_view query);

    /**
     * Serialize a textDocument/prepareRename request to validate a rename.
     * @return Pair of (requestId, framedMessage).
     */
    std::pair<int, std::string> serializePrepareRename(std::string_view uri, int line, int character);

    /**
     * Serialize a $/cancelRequest notification for a superseded request id.
     */
    std::string serializeCancelRequest(int id);

    /** Serialize the shutdown request (client must follow with exit). */
    std::string serializeShutdown();

    /** Serialize the exit notification (ends the server process). */
    std::string serializeExit();

    /**
     * Serialize a JSON-RPC response to a server-initiated request,
     * echoing the request's id verbatim (integer or string).
     */
    std::string serializeResponse(const nlohmann::json& id,
                                  const nlohmann::json& result);

    /**
     * Serialize a JSON-RPC error response (e.g. MethodNotFound for
     * server→client methods we don't implement).
     */
    std::string serializeError(const nlohmann::json& id, int code,
                               std::string_view message);

    // -----------------------------------------------------------------------
    // Navigation response parsing
    // -----------------------------------------------------------------------

    /**
     * Parse a Location or array of Locations from a definition/references response.
     */
    static NavigationResult parseNavigationResult(const nlohmann::json& j);

    /**
     * Parse SymbolInformation array from a documentSymbol / workspaceSymbol response.
     */
    static DocumentSymbolResult parseDocumentSymbolResult(const nlohmann::json& j);

    /**
     * Parse SymbolInformation array from workspace/symbol response.
     */
    static WorkspaceSymbolResult parseWorkspaceSymbolResult(const nlohmann::json& j);

    /**
     * Parse a prepareRename response — returns true if position is renamable.
     */
    static bool parsePrepareRename(const nlohmann::json& j);

    /**
     * Parse a JSON string into an IncomingMessage.
     * Returns std::nullopt if the message is not valid JSON-RPC 2.0.
     */
    std::optional<IncomingMessage> parseIncoming(std::string_view jsonStr) const;

    /**
     * Parse a CompletionList from a JSON response.
     */
    static CompletionList parseCompletionList(const nlohmann::json& j);

    /**
     * Parse a Hover from a JSON response.
     */
    static std::optional<Hover> parseHover(const nlohmann::json& j);

    /**
     * Parse a SignatureHelp from a JSON response.
     */
    static std::optional<SignatureHelp> parseSignatureHelp(const nlohmann::json& j);

    /**
     * Parse diagnostics from a publishDiagnostics notification params.
     */
    static std::pair<std::string, std::vector<Diagnostic>> parseDiagnostics(const nlohmann::json& params);

private:
    int nextId_ = 1;
};

} // namespace hathor::lsp
