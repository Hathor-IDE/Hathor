// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_lsp_jsonrpc_navigation.cpp — unit tests for LSP navigation request
 * serialization and response parsing.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target.
 *
 * Requirement references: AI-4, L-2
 */

#include <catch2/catch_test_macros.hpp>

#include "LspJsonRpc.hpp"
#include "LspProtocol.hpp"

#include <nlohmann/json.hpp>

#include <string>

using namespace hathor::lsp;

namespace {
std::string extractJsonBody(const std::string& framed)
{
    auto headerEnd = framed.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
        return {};
    return framed.substr(headerEnd + 4);
}
}

// ===========================================================================
// Initialize capabilities
// ===========================================================================

TEST_CASE("LspJsonRpc.serializeInitialize advertises navigation capabilities", "[lsp][nav]")
{
    LspJsonRpc rpc;
    std::string serialized = rpc.serializeInitialize("file:///workspace");

    auto j = nlohmann::json::parse(extractJsonBody(serialized));
    REQUIRE(j["jsonrpc"] == "2.0");
    REQUIRE(j["method"] == "initialize");

    const auto& caps = j["params"]["capabilities"];
    REQUIRE(caps["textDocument"]["definition"].contains("dynamicRegistration"));
    REQUIRE(caps["textDocument"]["references"].contains("dynamicRegistration"));
    REQUIRE(caps["textDocument"]["rename"].contains("prepareProvider"));
    REQUIRE(caps["textDocument"]["documentSymbol"].contains("dynamicRegistration"));
    REQUIRE(caps["workspace"]["symbol"].contains("dynamicRegistration"));
}

// ===========================================================================
// Request serialization
// ===========================================================================

TEST_CASE("LspJsonRpc: serializeDefinition produces correct request", "[lsp][nav]")
{
    LspJsonRpc rpc;
    auto [id, msg] = rpc.serializeDefinition("file:///test.hathor", 3, 5);

    // The framed message should contain "textDocument/definition"
    REQUIRE(msg.find("textDocument/definition") != std::string::npos);
    REQUIRE(msg.find("\"line\":3") != std::string::npos);
    REQUIRE(msg.find("\"character\":5") != std::string::npos);
    REQUIRE(msg.find("\"uri\":\"file:///test.hathor\"") != std::string::npos);
}

TEST_CASE("LspJsonRpc: serializeReferences includes context", "[lsp][nav]")
{
    LspJsonRpc rpc;
    auto [id, msg] = rpc.serializeReferences("file:///test.hathor", 10, 20, true);

    REQUIRE(msg.find("textDocument/references") != std::string::npos);
    REQUIRE(msg.find("includeDeclaration") != std::string::npos);
}

TEST_CASE("LspJsonRpc: serializeReferences with includeDeclaration=false", "[lsp][nav]")
{
    LspJsonRpc rpc;
    auto [id, msg] = rpc.serializeReferences("file:///test.hathor", 10, 20, false);

    REQUIRE(msg.find("includeDeclaration") != std::string::npos);
    REQUIRE(msg.find("\"includeDeclaration\":false") != std::string::npos);
}

TEST_CASE("LspJsonRpc: serializeRename includes newName", "[lsp][nav]")
{
    LspJsonRpc rpc;
    auto [id, msg] = rpc.serializeRename("file:///test.hathor", 2, 3, "newName");

    REQUIRE(msg.find("textDocument/rename") != std::string::npos);
    REQUIRE(msg.find("\"newName\":\"newName\"") != std::string::npos);
}

TEST_CASE("LspJsonRpc: serializeDocumentSymbol produces correct request", "[lsp][nav]")
{
    LspJsonRpc rpc;
    auto [id, msg] = rpc.serializeDocumentSymbol("file:///test.hathor");

    REQUIRE(msg.find("textDocument/documentSymbol") != std::string::npos);
    REQUIRE(msg.find("\"uri\":\"file:///test.hathor\"") != std::string::npos);
}

TEST_CASE("LspJsonRpc: serializeWorkspaceSymbol produces correct request", "[lsp][nav]")
{
    LspJsonRpc rpc;
    auto [id, msg] = rpc.serializeWorkspaceSymbol("fast");

    REQUIRE(msg.find("workspace/symbol") != std::string::npos);
    REQUIRE(msg.find("\"query\":\"fast\"") != std::string::npos);
}

TEST_CASE("LspJsonRpc: serializePrepareRename produces correct request", "[lsp][nav]")
{
    LspJsonRpc rpc;
    auto [id, msg] = rpc.serializePrepareRename("file:///test.hathor", 2, 3);

    REQUIRE(msg.find("textDocument/prepareRename") != std::string::npos);
}

// ===========================================================================
// Response parsing
// ===========================================================================

TEST_CASE("LspJsonRpc: parseNavigationResult handles location array", "[lsp][nav]")
{
    nlohmann::json j = nlohmann::json::array({
        {
            {"uri", "file:///test.hathor"},
            {"range", {
                {"start", {{"line", 3}, {"character", 5}}},
                {"end", {{"line", 3}, {"character", 8}}}
            }}
        },
        {
            {"uri", "file:///other.hathor"},
            {"range", {
                {"start", {{"line", 1}, {"character", 0}}},
                {"end", {{"line", 1}, {"character", 4}}}
            }}
        }
    });

    auto result = LspJsonRpc::parseNavigationResult(j);
    REQUIRE(result.locations.size() == 2);

    REQUIRE(result.locations[0].uri == "file:///test.hathor");
    REQUIRE(result.locations[0].range.start.line == 3);
    REQUIRE(result.locations[0].range.start.character == 5);
    REQUIRE(result.locations[0].range.end.line == 3);
    REQUIRE(result.locations[0].range.end.character == 8);

    REQUIRE(result.locations[1].uri == "file:///other.hathor");
}

TEST_CASE("LspJsonRpc: parseNavigationResult handles single location", "[lsp][nav]")
{
    nlohmann::json j = {
        {"uri", "file:///test.hathor"},
        {"range", {
            {"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 5}}}
        }}
    };

    auto result = LspJsonRpc::parseNavigationResult(j);
    REQUIRE(result.locations.size() == 1);
    REQUIRE(result.locations[0].uri == "file:///test.hathor");
}

TEST_CASE("LspJsonRpc: parseNavigationResult handles null response", "[lsp][nav]")
{
    auto result = LspJsonRpc::parseNavigationResult(nullptr);
    REQUIRE(result.locations.empty());
}

TEST_CASE("LspJsonRpc: parseNavigationResult handles empty array", "[lsp][nav]")
{
    auto result = LspJsonRpc::parseNavigationResult(nlohmann::json::array());
    REQUIRE(result.locations.empty());
}

// ===========================================================================
// Symbol parsing
// ===========================================================================

TEST_CASE("LspJsonRpc: parseDocumentSymbolResult handles symbols", "[lsp][nav]")
{
    nlohmann::json j = nlohmann::json::array({
        {
            {"name", "fast"},
            {"kind", 12},
            {"detail", "fast(multiplier: number)"},
            {"deprecated", false},
            {"location", {
                {"uri", "file:///test.hathor"},
                {"range", {
                    {"start", {{"line", 0}, {"character", 0}}},
                    {"end", {{"line", 0}, {"character", 10}}}
                }}
            }}
        },
        {
            {"name", "s"},
            {"kind", 12},
            {"detail", "s(pattern: string)"},
            {"location", {
                {"uri", "file:///test.hathor"},
                {"range", {
                    {"start", {{"line", 2}, {"character", 0}}},
                    {"end", {{"line", 2}, {"character", 5}}}
                }}
            }}
        }
    });

    auto result = LspJsonRpc::parseDocumentSymbolResult(j);
    REQUIRE(result.symbols.size() == 2);
    REQUIRE(result.symbols[0].name == "fast");
    REQUIRE(result.symbols[0].kind == SymbolKind::Function);
    REQUIRE(result.symbols[0].detail.value() == "fast(multiplier: number)");
    REQUIRE(result.symbols[0].location.uri == "file:///test.hathor");
}

TEST_CASE("LspJsonRpc: parseWorkspaceSymbolResult handles symbols", "[lsp][nav]")
{
    nlohmann::json j = nlohmann::json::array({
        {
            {"name", "bd"},
            {"kind", 14},
            {"detail", "sample"},
            {"location", {
                {"uri", "hathor://builtin/samples"},
                {"range", {
                    {"start", {{"line", 0}, {"character", 0}}},
                    {"end", {{"line", 0}, {"character", 2}}}
                }}
            }}
        }
    });

    auto result = LspJsonRpc::parseWorkspaceSymbolResult(j);
    REQUIRE(result.symbols.size() == 1);
    REQUIRE(result.symbols[0].name == "bd");
    REQUIRE(result.symbols[0].kind == SymbolKind::Constant);
}

// ===========================================================================
// Prepare rename parsing
// ===========================================================================

TEST_CASE("LspJsonRpc: parsePrepareRename handles valid range", "[lsp][nav]")
{
    nlohmann::json j = {
        {"placeholder", "fast"},
        {"range", {
            {"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 4}}}
        }}
    };

    REQUIRE(LspJsonRpc::parsePrepareRename(j) == true);
}

TEST_CASE("LspJsonRpc: parsePrepareRename handles null", "[lsp][nav]")
{
    REQUIRE(LspJsonRpc::parsePrepareRename(nullptr) == false);
}

TEST_CASE("LspJsonRpc: parsePrepareRename handles boolean true", "[lsp][nav]")
{
    REQUIRE(LspJsonRpc::parsePrepareRename(true) == true);
}

TEST_CASE("LspJsonRpc: parsePrepareRename handles boolean false", "[lsp][nav]")
{
    REQUIRE(LspJsonRpc::parsePrepareRename(false) == false);
}

// ===========================================================================
// SymbolKind enum
// ===========================================================================

TEST_CASE("LspJsonRpc: parseDocumentSymbolResult maps kind correctly", "[lsp][nav]")
{
    nlohmann::json j = nlohmann::json::array();
    j.push_back(nlohmann::json::object({
        {"name", "MyFunction"}, {"kind", 12},
        {"location", {{"uri", "file:///a.hathor"},
                      {"range", {{"start", {{"line", 0}, {"character", 0}}},
                                 {"end", {{"line", 0}, {"character", 5}}}}}}}
    }));
    j.push_back(nlohmann::json::object({
        {"name", "MyClass"}, {"kind", 5},
        {"location", {{"uri", "file:///a.hathor"},
                      {"range", {{"start", {{"line", 0}, {"character", 0}}},
                                 {"end", {{"line", 0}, {"character", 5}}}}}}}
    }));
    j.push_back(nlohmann::json::object({
        {"name", "MySample"}, {"kind", 14},
        {"location", {{"uri", "file:///a.hathor"},
                      {"range", {{"start", {{"line", 0}, {"character", 0}}},
                                 {"end", {{"line", 0}, {"character", 5}}}}}}}
    }));
    j.push_back(nlohmann::json::object({
        {"name", "Unknown"}, {"kind", 999},
        {"location", {{"uri", "file:///a.hathor"},
                      {"range", {{"start", {{"line", 0}, {"character", 0}}},
                                 {"end", {{"line", 0}, {"character", 5}}}}}}}
    }));

    auto result = LspJsonRpc::parseDocumentSymbolResult(j);
    REQUIRE(result.symbols.size() == 4);
    REQUIRE(result.symbols[0].kind == SymbolKind::Function);
    REQUIRE(result.symbols[1].kind == SymbolKind::Class);
    REQUIRE(result.symbols[2].kind == SymbolKind::Constant);
    REQUIRE(result.symbols[3].kind == SymbolKind::Function); // default fallback
}
