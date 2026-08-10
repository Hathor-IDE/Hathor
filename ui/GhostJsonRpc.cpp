// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * GhostJsonRpc.cpp — implementation of GhostJsonRpc.
 *
 * Serializes llm-ls custom method calls and parses their JSON responses.
 * Uses the existing LspMessageFramer for Content-Length framing.
 *
 * Requirement references: AI-4
 */

#include "GhostJsonRpc.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>

using json = nlohmann::json;

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// Outgoing message serialization
// ---------------------------------------------------------------------------

std::string GhostJsonRpc::serializeInitialize()
{
    json params = {
        {"processId", static_cast<int>(0)},
        {"rootUri", nullptr},
        {"capabilities", {
            {"textDocument", {
                {"completion", {
                    {"dynamicRegistration", false},
                    {"completionItem", {
                        {"snippetSupport", false}
                    }}
                }},
                {"hover", {{"dynamicRegistration", false}}},
                {"signatureHelp", {
                    {"dynamicRegistration", false}
                }}
            }}
        }}
    };

    json msg = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", params}
    };

    return LspMessageFramer::frameWrite(msg.dump());
}

std::string GhostJsonRpc::serializeInitialized()
{
    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "initialized"},
        {"params", json::object()}
    };

    return LspMessageFramer::frameWrite(msg.dump());
}

std::pair<std::string, std::string> GhostJsonRpc::serializeGhostCompletion(
    const GhostCompletionRequest& req)
{
    std::string requestId = generateRequestId();

    json params = {
        {"textDocument", {
            {"uri", req.uri},
            {"languageId", req.languageId}
        }},
        {"position", {
            {"line", req.line},
            {"character", req.character}
        }},
        {"ide", "unknown"},
        {"fim", {
            {"enabled", req.fim.enabled},
            {"prefix", req.fim.prefix},
            {"suffix", req.fim.suffix},
            {"middle", req.fim.middle}
        }},
        {"api_token", req.apiToken.empty() ? nullptr : json(req.apiToken)},
        {"model", req.backend.model},
        {"backend", {
            {"backend", backendToString(req.backend.backend)},
            {"url", req.backend.url}
        }},
        {"tokenizer_config", req.tokenizerConfig.empty() || req.tokenizerConfig == "default"
            ? nullptr
            : json{{"path", req.tokenizerConfig}}},
        {"context_window", req.contextWindow},
        {"tls_skip_verify_insecure", req.tlsSkipVerify},
        {"request_body", req.requestBody.is_null() ? json::object() : req.requestBody},
        {"disable_url_path_completion", req.disableUrlPathCompletion},
        {"tokens_to_clear", req.tokensToClear}
    };

    json msg = {
        {"jsonrpc", "2.0"},
        {"id", requestId},
        {"method", "llm-ls/getCompletions"},
        {"params", params}
    };

    return {requestId, LspMessageFramer::frameWrite(msg.dump())};
}

std::string GhostJsonRpc::serializeAcceptCompletion(const AcceptCompletionParams& params)
{
    json shown;
    for (auto idx : params.shownCompletions)
        shown.push_back(idx);

    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "llm-ls/acceptCompletion"},
        {"params", {
            {"request_id", params.requestId},
            {"accepted_completion", params.acceptedCompletion},
            {"shown_completions", shown}
        }}
    };

    return LspMessageFramer::frameWrite(msg.dump());
}

std::string GhostJsonRpc::serializeRejectCompletion(const RejectCompletionParams& params)
{
    json shown;
    for (auto idx : params.shownCompletions)
        shown.push_back(idx);

    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "llm-ls/rejectCompletion"},
        {"params", {
            {"request_id", params.requestId},
            {"shown_completions", shown}
        }}
    };

    return LspMessageFramer::frameWrite(msg.dump());
}

std::pair<int, std::string> GhostJsonRpc::serializeDidOpen(
    std::string_view uri,
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

    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params", params}
    };

    return {version, LspMessageFramer::frameWrite(msg.dump())};
}

std::pair<int, std::string> GhostJsonRpc::serializeDidChange(
    std::string_view uri,
    int version,
    std::string_view text)
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

    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didChange"},
        {"params", params}
    };

    return {version, LspMessageFramer::frameWrite(msg.dump())};
}

std::string GhostJsonRpc::serializeDidClose(std::string_view uri)
{
    json params = {
        {"textDocument", {
            {"uri", std::string(uri)}
        }}
    };

    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didClose"},
        {"params", params}
    };

    return LspMessageFramer::frameWrite(msg.dump());
}

std::string GhostJsonRpc::serializeShutdown()
{
    json msg = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "shutdown"},
        {"params", json::object()}
    };

    return LspMessageFramer::frameWrite(msg.dump());
}

std::string GhostJsonRpc::serializeExit()
{
    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "exit"},
        {"params", json::object()}
    };

    return LspMessageFramer::frameWrite(msg.dump());
}

// ---------------------------------------------------------------------------
// Incoming message parsing
// ---------------------------------------------------------------------------

std::optional<json> GhostJsonRpc::parseJson(std::string_view jsonStr)
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

    return j;
}

std::optional<json> GhostJsonRpc::parseResponse(std::string_view jsonStr,
                                                  std::string& outId)
{
    auto j = parseJson(jsonStr);
    if (!j.has_value())
        return std::nullopt;

    const auto& msg = j.value();

    if (!msg.contains("id") || !msg.contains("result"))
        return std::nullopt;

    if (msg.contains("id") && msg["id"].is_string())
        outId = msg["id"].get<std::string>();
    else if (msg.contains("id") && msg["id"].is_number_integer())
        outId = std::to_string(msg["id"].get<int>());
    else
        outId = "";

    if (msg.contains("error"))
    {
        json errorResult;
        errorResult["__error"] = true;
        errorResult["code"] = msg["error"].value("code", -32603);
        errorResult["message"] = msg["error"].value("message", "Unknown error");
        return errorResult;
    }

    return msg["result"];
}

// ---------------------------------------------------------------------------
// Response parsing
// ---------------------------------------------------------------------------

nlohmann::json GhostCompletionRequest::toJson() const
{
    // tokenizer_config: llm-ls expects Option<TokenizerConfig> (an enum of
    // Local/Repository/Download objects), not a plain string. When empty or
    // "default", send null so llm-ls auto-resolves the tokenizer from the
    // model repository.
    nlohmann::json tokenizerJson = nullptr;
    if (!tokenizerConfig.empty() && tokenizerConfig != "default")
    {
        tokenizerJson = nlohmann::json{{"path", tokenizerConfig}};
    }

    nlohmann::json apiTokenJson = apiToken.empty() ? nullptr : nlohmann::json(apiToken);

    return nlohmann::json{
        {"textDocument", {
            {"uri", uri},
            {"languageId", languageId}
        }},
        {"position", {
            {"line", line},
            {"character", character}
        }},
        {"ide", "unknown"},
        {"fim", {
            {"enabled", fim.enabled},
            {"prefix", fim.prefix},
            {"suffix", fim.suffix},
            {"middle", fim.middle}
        }},
        {"api_token", apiTokenJson},
        {"model", backend.model},
        {"backend", {
            {"backend", backendToString(backend.backend)},
            {"url", backend.url}
        }},
        {"tokenizer_config", tokenizerJson},
        {"context_window", contextWindow},
        {"tls_skip_verify_insecure", tlsSkipVerify},
        {"request_body", requestBody.is_null() ? nlohmann::json::object() : requestBody},
        {"disable_url_path_completion", disableUrlPathCompletion},
        {"tokens_to_clear", tokensToClear}
    };
}

std::optional<GhostCompletionResponse> parseGhostCompletionResponse(
    const json& j)
{
    if (j.is_null())
        return std::nullopt;

    // llm-ls returns either a JSON-RPC 2.0 response with the completion
    // data as the result, or a direct object with request_id + completions.
    // Handle both forms.

    json resultObj = j;
    if (j.contains("result"))
        resultObj = j["result"];

    if (j.contains("error") && !j.contains("result"))
        return std::nullopt;

    GhostCompletionResponse resp;

    if (resultObj.contains("request_id"))
        resp.request_id = resultObj["request_id"].get<std::string>();
    else if (j.contains("request_id"))
        resp.request_id = j["request_id"].get<std::string>();

    if (resultObj.contains("completions"))
    {
        for (const auto& comp : resultObj["completions"])
        {
            GhostCompletion gc;
            gc.generatedText = comp.value("generated_text", "");
            resp.completions.push_back(std::move(gc));
        }
    }
    else if (j.contains("completions"))
    {
        for (const auto& comp : j["completions"])
        {
            GhostCompletion gc;
            gc.generatedText = comp.value("generated_text", "");
            resp.completions.push_back(std::move(gc));
        }
    }

    if (resp.request_id.empty() && resp.completions.empty())
        return std::nullopt;

    return resp;
}

// ---------------------------------------------------------------------------
// Request ID generation
// ---------------------------------------------------------------------------

std::string GhostJsonRpc::generateRequestId()
{
    // Generate a UUID v4 using a thread-local random engine.
    // Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    static thread_local std::mt19937_64 rng{
        static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count())
    };

    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-4%03x-%04x-%012llx%012llx",
        static_cast<unsigned int>(dist(rng) & 0x0FFFFFFF),
        static_cast<unsigned int>(dist(rng) & 0x0000FFFF),
        static_cast<unsigned int>(dist(rng) & 0x00000FFF),
        static_cast<unsigned int>((dist(rng) & 0x00003FFF) | 0x4000),
        static_cast<unsigned long long>((dist(rng) >> 16) & 0x0000FFFFFFFFFFFFULL),
        static_cast<unsigned long long>(dist(rng) & 0x0000FFFFFFFFFFFFULL));

    return std::string(buf);
}

} // namespace hathor::lsp
