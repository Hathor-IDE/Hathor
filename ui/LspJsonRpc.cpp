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
                {"diagnostic", {
                    {"dynamicRegistration", false}
                }},
                {"synchronization", {
                    {"dynamicRegistration", false},
                    {"willSave", false},
                    {"didSave", true},
                    {"save", {{"includeText", false}}}
                }}
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
