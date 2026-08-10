// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_lsp_jsonrpc.cpp — unit tests for JSON-RPC 2.0 + LSP message dispatch.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
 */

#include <catch2/catch_test_macros.hpp>

#include "LspJsonRpc.hpp"
#include "LspProtocol.hpp"
#include "LspMessageFramer.hpp"

#include <nlohmann/json.hpp>

#include <string>

using namespace hathor::lsp;

// ===========================================================================
// MessageFramer tests
// ===========================================================================

TEST_CASE("MessageFramer.frameWrite produces valid framed message", "[lsp][framing]")
{
    std::string body = "{\"jsonrpc\":\"2.0\"}";
    std::string framed = MessageFramer::frameWrite(body);

    REQUIRE(framed.find("Content-Length: 14\r\n\r\n") != std::string::npos);
    REQUIRE(framed.size() > body.size());
}

TEST_CASE("MessageFramer parses a complete message via feed + tryNextMessage", "[lsp][framing]")
{
    MessageFramer framer;

    std::string framed = MessageFramer::frameWrite("{\"jsonrpc\":\"2.0\"}");
    framer.feed(framed);

    auto msg = framer.tryNextMessage();
    REQUIRE(msg.has_value());
    REQUIRE(msg->contentLength == 14);
    REQUIRE(msg->body == "{\"jsonrpc\":\"2.0\"}");
}

TEST_CASE("MessageFramer returns nullopt for incomplete body", "[lsp][framing]")
{
    MessageFramer framer;

    std::string partial = "Content-Length: 100\r\n\r\n{\"jsonrpc\":\"2.0\"";
    framer.feed(partial);

    auto msg = framer.tryNextMessage();
    REQUIRE_FALSE(msg.has_value());
    REQUIRE(framer.hasBufferedData());
}

TEST_CASE("MessageFramer parses multiple messages pipelined", "[lsp][framing]")
{
    MessageFramer framer;

    std::string msg1 = MessageFramer::frameWrite("{\"jsonrpc\":\"2.0\"}");
    std::string msg2 = MessageFramer::frameWrite("{\"jsonrpc\":\"2.1\"}");
    framer.feed(msg1 + msg2);

    auto m1 = framer.tryNextMessage();
    REQUIRE(m1.has_value());
    REQUIRE(m1->body == "{\"jsonrpc\":\"2.0\"}");

    auto m2 = framer.tryNextMessage();
    REQUIRE(m2.has_value());
    REQUIRE(m2->body == "{\"jsonrpc\":\"2.1\"}");

    // Buffer should be empty now
    REQUIRE_FALSE(framer.hasBufferedData());
}

TEST_CASE("MessageFramer handles partial header", "[lsp][framing]")
{
    MessageFramer framer;

    // Feed just part of the header
    framer.feed("Content-Length: 14\r");
    REQUIRE_FALSE(framer.tryNextMessage().has_value());
    REQUIRE(framer.hasBufferedData());

    // Complete the header + body
    framer.feed("\r\n\r\n{\"jsonrpc\":\"2.0\"}");

    auto msg = framer.tryNextMessage();
    REQUIRE(msg.has_value());
    REQUIRE(msg->body == "{\"jsonrpc\":\"2.0\"}");
}

// ===========================================================================
// LspJsonRpc serialization tests
// ===========================================================================

TEST_CASE("LspJsonRpc.serializeRequest produces framed JSON-RPC request", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;

    auto serialized = rpc.serializeRequest("textDocument/completion",
        nlohmann::json::object({
            {"uri", "file:///test.hathor"},
            {"line", 0},
            {"character", 5}
        }));

    // Should contain Content-Length header
    REQUIRE(serialized.find("Content-Length:") != std::string::npos);

    // Parse the JSON body
    auto framed = MessageFramer::frameWrite("");
    (void)framed; // just verify we can call it

    // Extract and parse body
    auto headerEnd = serialized.find("\r\n\r\n");
    REQUIRE(headerEnd != std::string::npos);
    std::string body = serialized.substr(headerEnd + 4);

    auto j = nlohmann::json::parse(body);
    REQUIRE(j["jsonrpc"] == "2.0");
    REQUIRE(j["method"] == "textDocument/completion");
    REQUIRE(j["params"]["uri"] == "file:///test.hathor");
    REQUIRE(j["id"].is_number());
}

TEST_CASE("LspJsonRpc.serializeRequest assigns incrementing IDs", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;

    auto s1 = rpc.serializeRequest("method1", nlohmann::json::object());
    auto s2 = rpc.serializeRequest("method2", nlohmann::json::object());

    auto extractId = [](std::string_view s) -> int {
        auto headerEnd = s.find("\r\n\r\n");
        std::string body = std::string(s.substr(headerEnd + 4));
        auto j = nlohmann::json::parse(body);
        return j["id"].get<int>();
    };

    int id1 = extractId(s1);
    int id2 = extractId(s2);
    REQUIRE(id2 == id1 + 1);
}

TEST_CASE("LspJsonRpc.serializeNotification omits id", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;

    std::string serialized = rpc.serializeNotification("textDocument/didOpen",
        nlohmann::json::object({
            {"uri", "file:///test.hathor"},
            {"text", "bd sn"}
        }));

    auto headerEnd = serialized.find("\r\n\r\n");
    std::string body = serialized.substr(headerEnd + 4);
    auto j = nlohmann::json::parse(body);

    REQUIRE(j["jsonrpc"] == "2.0");
    REQUIRE(j["method"] == "textDocument/didOpen");
    REQUIRE_FALSE(j.contains("id"));
    REQUIRE(j["params"]["uri"] == "file:///test.hathor");
}

TEST_CASE("LspJsonRpc.serializeDidOpen creates didOpen notification", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;
    std::string framed = rpc.serializeDidOpen("file:///test.hathor", "hathor", 1, "bd sn");

    auto headerEnd = framed.find("\r\n\r\n");
    REQUIRE(headerEnd != std::string::npos);
    std::string body = framed.substr(headerEnd + 4);
    auto j = nlohmann::json::parse(body);

    REQUIRE(j["method"] == "textDocument/didOpen");
    REQUIRE(j["params"]["textDocument"]["uri"] == "file:///test.hathor");
    REQUIRE(j["params"]["textDocument"]["languageId"] == "hathor");
    REQUIRE(j["params"]["textDocument"]["version"] == 1);
    REQUIRE(j["params"]["textDocument"]["text"] == "bd sn");
}

TEST_CASE("LspJsonRpc.serializeDidChange creates didChange notification", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;
    std::string framed = rpc.serializeDidChange("file:///test.hathor", 2, "bd sn hh");

    auto headerEnd = framed.find("\r\n\r\n");
    std::string body = framed.substr(headerEnd + 4);
    auto j = nlohmann::json::parse(body);

    REQUIRE(j["method"] == "textDocument/didChange");
    REQUIRE(j["params"]["textDocument"]["uri"] == "file:///test.hathor");
    REQUIRE(j["params"]["textDocument"]["version"] == 2);
    REQUIRE(j["params"]["contentChanges"][0]["text"] == "bd sn hh");
}

TEST_CASE("LspJsonRpc.serializeDidClose creates didClose notification", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;
    std::string framed = rpc.serializeDidClose("file:///test.hathor");

    auto headerEnd = framed.find("\r\n\r\n");
    std::string body = framed.substr(headerEnd + 4);
    auto j = nlohmann::json::parse(body);

    REQUIRE(j["method"] == "textDocument/didClose");
    REQUIRE(j["params"]["textDocument"]["uri"] == "file:///test.hathor");
}

TEST_CASE("LspJsonRpc.serializeCompletion returns id and framed message", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;
    auto [id, framed] = rpc.serializeCompletion("file:///test.hathor", 0, 5);

    REQUIRE(id > 0);

    auto headerEnd = framed.find("\r\n\r\n");
    std::string body = framed.substr(headerEnd + 4);
    auto j = nlohmann::json::parse(body);

    REQUIRE(j["method"] == "textDocument/completion");
    REQUIRE(j["params"]["textDocument"]["uri"] == "file:///test.hathor");
    REQUIRE(j["params"]["position"]["line"] == 0);
    REQUIRE(j["params"]["position"]["character"] == 5);
}

TEST_CASE("LspJsonRpc.serializeHover returns id and framed message", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;
    auto [id, framed] = rpc.serializeHover("file:///test.hathor", 3, 0);

    REQUIRE(id > 0);

    auto headerEnd = framed.find("\r\n\r\n");
    std::string body = framed.substr(headerEnd + 4);
    auto j = nlohmann::json::parse(body);

    REQUIRE(j["method"] == "textDocument/hover");
    REQUIRE(j["params"]["position"]["line"] == 3);
}

// ===========================================================================
// LspJsonRpc parsing tests
// ===========================================================================

TEST_CASE("LspJsonRpc.parseIncoming detects a response", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;

    std::string raw = R"({
        "jsonrpc": "2.0",
        "id": 1,
        "result": {"isIncomplete": false, "items": []}
    })";

    auto msg = rpc.parseIncoming(raw);
    REQUIRE(msg.has_value());
    REQUIRE(msg->type == IncomingMessage::Type::Response);
    REQUIRE(msg->response.id.has_value());
    REQUIRE(msg->response.id.value() == 1);
    REQUIRE(msg->response.result["isIncomplete"] == false);
}

TEST_CASE("LspJsonRpc.parseIncoming detects a notification", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;

    std::string raw = R"({
        "jsonrpc": "2.0",
        "method": "textDocument/publishDiagnostics",
        "params": {"uri": "file:///test.hathor", "diagnostics": []}
    })";

    auto msg = rpc.parseIncoming(raw);
    REQUIRE(msg.has_value());
    REQUIRE(msg->type == IncomingMessage::Type::Notification);
    REQUIRE(msg->notification.method == "textDocument/publishDiagnostics");
}

TEST_CASE("LspJsonRpc.parseIncoming returns nullopt for invalid JSON", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;

    std::string raw = "not valid json {{{";
    auto msg = rpc.parseIncoming(raw);
    REQUIRE_FALSE(msg.has_value());
}

TEST_CASE("LspJsonRpc.parseIncoming returns nullopt for missing jsonrpc", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;

    std::string raw = R"({"method": "test"})";
    auto msg = rpc.parseIncoming(raw);
    REQUIRE_FALSE(msg.has_value());
}

TEST_CASE("LspJsonRpc.parseIncoming detects error response", "[lsp][jsonrpc]")
{
    LspJsonRpc rpc;

    std::string raw = R"({
        "jsonrpc": "2.0",
        "id": 9,
        "error": {"code": -32601, "message": "Method not found"}
    })";

    auto msg = rpc.parseIncoming(raw);
    REQUIRE(msg.has_value());
    REQUIRE(msg->type == IncomingMessage::Type::Response);
    REQUIRE(msg->response.isError == true);
    REQUIRE(msg->response.errorCode == -32601);
    REQUIRE(msg->response.errorMessage == "Method not found");
}

// ===========================================================================
// Static parsing functions
// ===========================================================================

TEST_CASE("parseCompletionList extracts items from JSON", "[lsp][jsonrpc]")
{
    nlohmann::json j = {
        {"isIncomplete", false},
        {"items", {
            {{"label", "fast"}, {"kind", 3}, {"insertText", "fast"}},
            {{"label", "slow"}, {"kind", 3}, {"insertText", "slow"}}
        }}
    };

    auto list = LspJsonRpc::parseCompletionList(j);
    REQUIRE_FALSE(list.isIncomplete);
    REQUIRE(list.items.size() == 2);
    REQUIRE(list.items[0].label == "fast");
    REQUIRE(list.items[0].kind.has_value());
    REQUIRE(list.items[0].kind.value() == CompletionItemKind::Function);
}

TEST_CASE("parseCompletionList handles CompletionList format", "[lsp][jsonrpc]")
{
    nlohmann::json j = {
        {"isIncomplete", true},
        {"items", {
            {{"label", "bd"}, {"kind", 12}}
        }}
    };

    auto list = LspJsonRpc::parseCompletionList(j);
    REQUIRE(list.isIncomplete == true);
    REQUIRE(list.items.size() == 1);
    REQUIRE(list.items[0].label == "bd");
    REQUIRE(list.items[0].kind.value() == CompletionItemKind::Value);
}

TEST_CASE("parseHover parses contents array", "[lsp][jsonrpc]")
{
    nlohmann::json j = {
        {"contents", {
            {{"language", "hathor"}, {"value", "fast(2) // speed up"}}
        }},
        {"range", {
            {"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 10}}}
        }}
    };

    auto hover = LspJsonRpc::parseHover(j);
    REQUIRE(hover.has_value());
    REQUIRE(hover->contents.size() == 1);
    REQUIRE(hover->contents[0].kind == "hathor");
    REQUIRE(hover->contents[0].value.find("speed up") != std::string::npos);
    REQUIRE(hover->range.has_value());
}

TEST_CASE("parseHover returns nullopt for null result", "[lsp][jsonrpc]")
{
    nlohmann::json j = nullptr;
    auto hover = LspJsonRpc::parseHover(j);
    REQUIRE_FALSE(hover.has_value());
}

TEST_CASE("parseSignatureHelp parses signatures", "[lsp][jsonrpc]")
{
    nlohmann::json j = {
        {"signatures", {
            {
                {"label", "fast(multiplier: number)"},
                {"parameters", {
                    {{"label", "multiplier"}}
                }}
            }
        }},
        {"activeSignature", 0}
    };

    auto sigh = LspJsonRpc::parseSignatureHelp(j);
    REQUIRE(sigh.has_value());
    REQUIRE(sigh->signatures.size() == 1);
    REQUIRE(sigh->signatures[0].label == "fast(multiplier: number)");
    REQUIRE(sigh->signatures[0].parameters.size() == 1);
    REQUIRE(sigh->signatures[0].parameters[0].label == "multiplier");
}

TEST_CASE("parseDiagnostics extracts URI and diagnostics", "[lsp][jsonrpc]")
{
    nlohmann::json params = {
        {"uri", "file:///test.hathor"},
        {"diagnostics", {
            {
                {"range", {
                    {"start", {{"line", 0}, {"character", 0}}},
                    {"end", {{"line", 0}, {"character", 5}}}
                }},
                {"severity", 1},
                {"message", "Unexpected token"}
            }
        }}
    };

    auto [uri, diags] = LspJsonRpc::parseDiagnostics(params);
    REQUIRE(uri == "file:///test.hathor");
    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].severity.value() == DiagnosticSeverity::Error);
    REQUIRE(diags[0].message == "Unexpected token");
    REQUIRE(diags[0].range.start.line == 0);
}
