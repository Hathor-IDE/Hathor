// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * LspJsonRpc.cpp — JUCE-free implementation of LspJsonRpc.
 *
 * Requirement references: AI-4
 */

#include "LspJsonRpc.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

using json = nlohmann::json;

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// Outgoing message serialization
// ---------------------------------------------------------------------------

std::string LspJsonRpc::serializeRequest(std::string_view method, const json& params)
{
    int id = nextId_++;
    json msg = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", std::string(method)},
        {"params", params}
    };
    return LspMessageFramer::frameWrite(msg.dump());
}

std::string LspJsonRpc::serializeNotification(std::string_view method, const json& params)
{
    json msg = {
        {"jsonrpc", "2.0"},
        {"method", std::string(method)},
        {"params", params}
    };
    return LspMessageFramer::frameWrite(msg.dump());
}

std::string LspJsonRpc::serializeInitialize(std::string_view uri)
{
    json params = {
        {"processId", static_cast<int>(0)},
        {"rootUri", std::string(uri)},
        {"capabilities", {
            {"textDocument", {
                {"completion", {
                    {"dynamicRegistration", false},
                    {"triggerCharacters", json::array({"\"", "(", ")", ".", "!", "*", "/", "|", "<", ">", "[", "]", ":"})},
                    {"completionItem", {
                        {"snippetSupport", false}
                    }}
                }},
                {"hover", {{"dynamicRegistration", false}}},
                {"signatureHelp", {
                    {"dynamicRegistration", false},
                    {"triggerCharacters", json::array({"(", ","})}
                }},
                {"definition", {{"dynamicRegistration", false}}},
                {"references", {{"dynamicRegistration", false}}},
                {"typeDefinition", {{"dynamicRegistration", false}}},
                {"declaration", {{"dynamicRegistration", false}}},
                {"rename", {
                    {"dynamicRegistration", false},
                    {"prepareProvider", true}
                }},
                {"documentSymbol", {{"dynamicRegistration", false}}},
                {"workspaceSymbol", {{"dynamicRegistration", false}}},
                {"diagnostic", {
                    {"dynamicRegistration", false}
                }},
                {"synchronization", {
                    {"dynamicRegistration", false},
                    {"willSave", false},
                    {"didSave", true},
                    {"save", {{"includeText", false}}}
                }}
            }},
            {"workspace", {
                {"symbol", {{"dynamicRegistration", false}}}
            }}
        }}
    };
    return serializeRequest("initialize", params);
}

std::string LspJsonRpc::serializeDidOpen(std::string_view uri,
                                          std::string_view languageId,
                                          int version,
                                          std::string_view text)
{
    json params = {
        {"textDocument", {
            {"uri", std::string(uri)},
            {"languageId", std::string(languageId)},
            {"version", version},
            {"text", std::string(text)}
        }}
    };
    return serializeNotification("textDocument/didOpen", params);
}

std::string LspJsonRpc::serializeDidChange(std::string_view uri, int version, std::string_view text)
{
    json params = {
        {"textDocument", {
            {"uri", std::string(uri)},
            {"version", version}
        }},
        {"contentChanges", json::array({
            {{"text", std::string(text)}}
        })}
    };
    return serializeNotification("textDocument/didChange", params);
}

std::string LspJsonRpc::serializeDidClose(std::string_view uri)
{
    json params = {
        {"textDocument", {
            {"uri", std::string(uri)}
        }}
    };
    return serializeNotification("textDocument/didClose", params);
}

std::pair<int, std::string> LspJsonRpc::serializeCompletion(std::string_view uri, int line, int character)
{
    json params = {
        {"textDocument", {{"uri", std::string(uri)}}},
        {"position", {{"line", line}, {"character", character}}}
    };
    return {nextId_, serializeRequest("textDocument/completion", params)};
}

std::pair<int, std::string> LspJsonRpc::serializeHover(std::string_view uri, int line, int character)
{
    json params = {
        {"textDocument", {{"uri", std::string(uri)}}},
        {"position", {{"line", line}, {"character", character}}}
    };
    return {nextId_, serializeRequest("textDocument/hover", params)};
}

std::pair<int, std::string> LspJsonRpc::serializeSignatureHelp(std::string_view uri, int line, int character)
{
    json params = {
        {"textDocument", {{"uri", std::string(uri)}}},
        {"position", {{"line", line}, {"character", character}}}
    };
    return {nextId_, serializeRequest("textDocument/signatureHelp", params)};
}

// ---------------------------------------------------------------------------
// Navigation request serialization
// ---------------------------------------------------------------------------

std::pair<int, std::string> LspJsonRpc::serializeDefinition(std::string_view uri, int line, int character)
{
    json params = {
        {"textDocument", {{"uri", std::string(uri)}}},
        {"position", {{"line", line}, {"character", character}}}
    };
    return {nextId_, serializeRequest("textDocument/definition", params)};
}

std::pair<int, std::string> LspJsonRpc::serializeReferences(std::string_view uri, int line, int character,
                                                                bool includeDeclaration)
{
    json params = {
        {"textDocument", {{"uri", std::string(uri)}}},
        {"position", {{"line", line}, {"character", character}}},
        {"context", {{"includeDeclaration", includeDeclaration}}}
    };
    return {nextId_, serializeRequest("textDocument/references", params)};
}

std::pair<int, std::string> LspJsonRpc::serializeTypeDefinition(std::string_view uri, int line, int character)
{
    json params = {
        {"textDocument", {{"uri", std::string(uri)}}},
        {"position", {{"line", line}, {"character", character}}}
    };
    return {nextId_, serializeRequest("textDocument/typeDefinition", params)};
}

std::pair<int, std::string> LspJsonRpc::serializeDeclaration(std::string_view uri, int line, int character)
{
    json params = {
        {"textDocument", {{"uri", std::string(uri)}}},
        {"position", {{"line", line}, {"character", character}}}
    };
    return {nextId_, serializeRequest("textDocument/declaration", params)};
}

std::pair<int, std::string> LspJsonRpc::serializeRename(std::string_view uri, int line, int character,
                                                          std::string_view newName)
{
    json params = {
        {"textDocument", {{"uri", std::string(uri)}}},
        {"position", {{"line", line}, {"character", character}}},
        {"newName", std::string(newName)}
    };
    return {nextId_, serializeRequest("textDocument/rename", params)};
}

std::pair<int, std::string> LspJsonRpc::serializeDocumentSymbol(std::string_view uri)
{
    json params = {
        {"textDocument", {{"uri", std::string(uri)}}}
    };
    return {nextId_, serializeRequest("textDocument/documentSymbol", params)};
}

std::pair<int, std::string> LspJsonRpc::serializeWorkspaceSymbol(std::string_view query)
{
    json params = {
        {"query", std::string(query)}
    };
    return {nextId_, serializeRequest("workspace/symbol", params)};
}

std::pair<int, std::string> LspJsonRpc::serializePrepareRename(std::string_view uri, int line, int character)
{
    json params = {
        {"textDocument", {{"uri", std::string(uri)}}},
        {"position", {{"line", line}, {"character", character}}}
    };
    return {nextId_, serializeRequest("textDocument/prepareRename", params)};
}

// ---------------------------------------------------------------------------
// Navigation response parsing
// ---------------------------------------------------------------------------

static SymbolKind symbolKindFromJson(int k)
{
    switch (k)
    {
        case 1:  return SymbolKind::File;
        case 2:  return SymbolKind::Module;
        case 3:  return SymbolKind::Namespace;
        case 4:  return SymbolKind::Package;
        case 5:  return SymbolKind::Class;
        case 6:  return SymbolKind::Method;
        case 7:  return SymbolKind::Property;
        case 8:  return SymbolKind::Field;
        case 9:  return SymbolKind::Constructor;
        case 10: return SymbolKind::Enum;
        case 11: return SymbolKind::Interface;
        case 12: return SymbolKind::Function;
        case 13: return SymbolKind::Variable;
        case 14: return SymbolKind::Constant;
        case 15: return SymbolKind::String;
        case 16: return SymbolKind::Number;
        case 17: return SymbolKind::Boolean;
        case 18: return SymbolKind::Array;
        case 19: return SymbolKind::Object;
        case 20: return SymbolKind::Key;
        case 21: return SymbolKind::Null;
        case 22: return SymbolKind::Struct;
        case 23: return SymbolKind::Event;
        case 24: return SymbolKind::Operator;
        case 25: return SymbolKind::TypeParameter;
        default: return SymbolKind::Function;
    }
}

static std::optional<Location> parseLocation(const json& j)
{
    if (j.is_null())
        return std::nullopt;

    Location loc;
    if (j.is_string())
    {
        loc.uri = j.get<std::string>();
        loc.range = Range{{0, 0}, {0, 0}};
    }
    else if (j.is_object())
    {
        loc.uri = j.value("uri", "");
        if (j.contains("range"))
        {
            loc.range = Range{
                {j["range"]["start"]["line"].get<int>(), j["range"]["start"]["character"].get<int>()},
                {j["range"]["end"]["line"].get<int>(), j["range"]["end"]["character"].get<int>()}
            };
        }
    }
    else
    {
        return std::nullopt;
    }

    return loc;
}

static SymbolInformation parseSymbolInformation(const json& j)
{
    SymbolInformation sym;
    sym.name = j.value("name", "");
    if (j.contains("kind"))
        sym.kind = symbolKindFromJson(j["kind"].get<int>());
    if (j.contains("deprecated"))
        sym.deprecated = j["deprecated"].get<bool>();
    sym.detail = j.contains("detail") ? std::optional<std::string>(j["detail"].get<std::string>()) : std::nullopt;
    if (j.contains("location"))
    {
        if (auto loc = parseLocation(j["location"]))
            sym.location = *loc;
    }
    sym.containerName = j.value("containerName", "");
    if (j.contains("flags"))
        sym.flags = j["flags"].get<int>();
    return sym;
}

NavigationResult LspJsonRpc::parseNavigationResult(const json& j)
{
    NavigationResult result;
    if (j.is_null())
        return result;

    if (j.is_array())
    {
        for (const auto& item : j)
        {
            if (auto loc = parseLocation(item))
                result.locations.push_back(*loc);
        }
    }
    else
    {
        if (auto loc = parseLocation(j))
            result.locations.push_back(*loc);
    }

    return result;
}

DocumentSymbolResult LspJsonRpc::parseDocumentSymbolResult(const json& j)
{
    DocumentSymbolResult result;
    if (j.is_null() || !j.is_array())
        return result;

    for (const auto& item : j)
    {
        result.symbols.push_back(parseSymbolInformation(item));
    }

    return result;
}

WorkspaceSymbolResult LspJsonRpc::parseWorkspaceSymbolResult(const json& j)
{
    WorkspaceSymbolResult result;
    if (j.is_null() || !j.is_array())
        return result;

    for (const auto& item : j)
    {
        result.symbols.push_back(parseSymbolInformation(item));
    }

    return result;
}

bool LspJsonRpc::parsePrepareRename(const json& j)
{
    if (j.is_null())
        return false;

    if (j.is_object() && j.contains("range"))
        return true;

    if (j.is_boolean())
        return j.get<bool>();

    return false;
}

// ---------------------------------------------------------------------------
// Incoming message parsing
// ---------------------------------------------------------------------------

std::optional<IncomingMessage> LspJsonRpc::parseIncoming(std::string_view jsonStr) const
{
    json j;
    try
    {
        j = json::parse(jsonStr);
    }
    catch (const json::parse_error&)
    {
        return std::nullopt;
    }

    if (!j.contains("jsonrpc") || j["jsonrpc"] != "2.0")
        return std::nullopt;

    IncomingMessage msg;

    if (j.contains("method"))
    {
        // Could be a request or notification
        if (j.contains("id"))
        {
            // Request
            msg.type = IncomingMessage::Type::Request;
            if (j["id"].is_number_integer())
                msg.request.id = j["id"].get<int>();
            msg.request.method = j["method"].get<std::string>();
            msg.request.params = j.value("params", json::object());
        }
        else
        {
            // Notification
            msg.type = IncomingMessage::Type::Notification;
            msg.notification.method = j["method"].get<std::string>();
            msg.notification.params = j.value("params", json::object());
        }
    }
    else if (j.contains("id") && (j.contains("result") || j.contains("error")))
    {
        // Response
        msg.type = IncomingMessage::Type::Response;
        if (j["id"].is_number_integer())
        {
            msg.response.id = j["id"].get<int>();
        }
        else if (j["id"].is_null())
        {
            msg.response.id = std::nullopt;
        }

        if (j.contains("error"))
        {
            msg.response.isError = true;
            msg.response.errorCode = j["error"].value("code", -32603);
            msg.response.errorMessage = j["error"].value("message", "Unknown error");
        }
        else
        {
            msg.response.result = j.value("result", json::object());
        }
    }
    else
    {
        return std::nullopt;
    }

    return msg;
}

// ---------------------------------------------------------------------------
// Response parsers
// ---------------------------------------------------------------------------

static CompletionItemKind kindFromJson(int k)
{
    switch (k)
    {
        case 1:  return CompletionItemKind::Text;
        case 2:  return CompletionItemKind::Method;
        case 3:  return CompletionItemKind::Function;
        case 4:  return CompletionItemKind::Constructor;
        case 5:  return CompletionItemKind::Field;
        case 6:  return CompletionItemKind::Variable;
        case 7:  return CompletionItemKind::Class;
        case 8:  return CompletionItemKind::Interface;
        case 9:  return CompletionItemKind::Module;
        case 10: return CompletionItemKind::Property;
        case 11: return CompletionItemKind::Unit;
        case 12: return CompletionItemKind::Value;
        case 13: return CompletionItemKind::Enum;
        case 14: return CompletionItemKind::Keyword;
        case 15: return CompletionItemKind::Snippet;
        default: return CompletionItemKind::Text;
    }
}

static std::optional<MarkupContent> parseMarkupContent(const json& j)
{
    if (j.is_null())
        return std::nullopt;
    MarkupContent mc;
    mc.kind = j.value("kind", "plaintext");
    mc.value = j.value("value", "");
    return mc;
}

CompletionList LspJsonRpc::parseCompletionList(const json& j)
{
    CompletionList list;
    if (j.is_null())
        return list;

    // Helper for optional string extraction
    auto optString = [](const json& obj, const char* key) -> std::optional<std::string> {
        if (obj.contains(key))
            return obj[key].get<std::string>();
        return std::nullopt;
    };

    // Could be a raw array of CompletionItems or a CompletionList object
    if (j.is_array())
    {
        for (const auto& item : j)
        {
            CompletionItem ci;
            ci.label = item.value("label", "");
            if (item.contains("kind"))
                ci.kind = kindFromJson(item["kind"].get<int>());
            ci.detail = optString(item, "detail");
            if (item.contains("documentation"))
                ci.documentation = parseMarkupContent(item["documentation"]);
            ci.insertText = optString(item, "insertText");
            ci.sortText = optString(item, "sortText");
            list.items.push_back(std::move(ci));
        }
    }
    else if (j.is_object())
    {
        list.isIncomplete = j.value("isIncomplete", false);
        if (j.contains("items"))
        {
            for (const auto& item : j["items"])
            {
                CompletionItem ci;
                ci.label = item.value("label", "");
                if (item.contains("kind"))
                    ci.kind = kindFromJson(item["kind"].get<int>());
                ci.detail = optString(item, "detail");
                if (item.contains("documentation"))
                    ci.documentation = parseMarkupContent(item["documentation"]);
                ci.insertText = optString(item, "insertText");
                ci.sortText = optString(item, "sortText");
                list.items.push_back(std::move(ci));
            }
        }
    }

    return list;
}

std::optional<Hover> LspJsonRpc::parseHover(const json& j)
{
    if (j.is_null())
        return std::nullopt;

    Hover h;
    if (j.contains("range"))
    {
        h.range = Range{
            {j["range"]["start"]["line"].get<int>(), j["range"]["start"]["character"].get<int>()},
            {j["range"]["end"]["line"].get<int>(), j["range"]["end"]["character"].get<int>()}
        };
    }

    if (j.contains("contents"))
    {
        const auto& c = j["contents"];
        if (c.is_string())
        {
            h.contents.push_back({.kind = "markdown", .value = c.get<std::string>()});
        }
        else if (c.is_object())
        {
            h.contents.push_back({
                .kind = c.value("kind", "plaintext"),
                .value = c.value("value", "")
            });
        }
        else if (c.is_array())
        {
            for (const auto& item : c)
            {
                if (item.is_string())
                    h.contents.push_back({.kind = "markdown", .value = item.get<std::string>()});
                else if (item.is_object())
                    h.contents.push_back({
                        .kind = item.value("kind", "plaintext"),
                        .value = item.value("value", "")
                    });
            }
        }
    }

    if (h.contents.empty())
        return std::nullopt;

    return h;
}

std::optional<SignatureHelp> LspJsonRpc::parseSignatureHelp(const json& j)
{
    if (j.is_null())
        return std::nullopt;

    SignatureHelp sh;
    if (j.contains("signatures"))
    {
        for (const auto& sig : j["signatures"])
        {
            SignatureInformation si;
            si.label = sig.value("label", "");
            if (sig.contains("documentation"))
                si.documentation = parseMarkupContent(sig["documentation"]);
            if (sig.contains("parameters"))
            {
                for (const auto& param : sig["parameters"])
                {
                    ParameterInformation pi;
                    if (param.contains("label"))
                    {
                        if (param["label"].is_string())
                            pi.label = param["label"].get<std::string>();
                        else
                            pi.label = param["label"].dump();
                    }
                    if (param.contains("documentation"))
                        pi.documentation = parseMarkupContent(param["documentation"]);
                    si.parameters.push_back(std::move(pi));
                }
            }
            sh.signatures.push_back(std::move(si));
        }
    }
    sh.activeSignature = j.value("activeSignature", 0);
    sh.activeParameter = j.value("activeParameter", 0);
    return sh;
}

std::pair<std::string, std::vector<Diagnostic>> LspJsonRpc::parseDiagnostics(const json& params)
{
    std::string uri = params.value("uri", "");
    std::vector<Diagnostic> diags;

    if (!params.contains("diagnostics"))
        return {uri, diags};

    for (const auto& d : params["diagnostics"])
    {
        Diagnostic diag;
        if (d.contains("range"))
        {
            diag.range = Range{
                {d["range"]["start"]["line"].get<int>(), d["range"]["start"]["character"].get<int>()},
                {d["range"]["end"]["line"].get<int>(), d["range"]["end"]["character"].get<int>()}
            };
        }
        if (d.contains("severity"))
        {
            int sev = d["severity"].get<int>();
            if (sev >= 1 && sev <= 4)
                diag.severity = static_cast<DiagnosticSeverity>(sev);
        }
        diag.message = d.value("message", "");
        diag.source = d.contains("source") ? std::optional<std::string>(d["source"].get<std::string>()) : std::nullopt;
        diag.code = d.contains("code") ? std::optional<std::string>(d["code"].get<std::string>()) : std::nullopt;
        diags.push_back(std::move(diag));
    }

    return {uri, diags};
}

} // namespace hathor::lsp
