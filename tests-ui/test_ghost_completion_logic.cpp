// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_ghost_completion_logic.cpp — unit tests for JUCE-free ghost-text logic.
 *
 * Tests cover:
 *   - FIM context builder (prefix reversed, suffix forward)
 *   - GhostCompletionLogic debounce, revision tracking, stale rejection,
 *     timeout, accept/reject, request building
 *   - GhostJsonRpc UUID generation and response parsing
 *   - GhostProviderResolver env-var resolution
 *
 * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
 */

#include <catch2/catch_test_macros.hpp>

#include "GhostCompletionLogic.hpp"
#include "GhostJsonRpc.hpp"
#include "GhostProtocol.hpp"
#include "GhostProviderConfig.hpp"
#include "LspMessageFramer.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <string>

using namespace hathor::lsp;

// ===========================================================================
// FIM context builder — AI-G2: explicit prefix/suffix computation
// ===========================================================================
// buildFimContext computes the actual document prefix (text before cursor)
// and suffix (text after cursor) from the cursor position. These are used
// for client-side FIM response trimming and result verification.
// llm-ls extracts prefix/suffix from the synced document; our docPrefix/
// docSuffix are for trimming only.

TEST_CASE("buildFimContext computes docPrefix/docSuffix from cursor position", "[ghost][fim]")
{
    std::string doc = "line1\nline2\nline3";
    auto fim = buildFimContext(doc, 2, 0);

    // Cursor at line 2, char 0 → prefix = everything before line 3 start
    REQUIRE(fim.docPrefix == "line1\nline2\n");
    REQUIRE(fim.docSuffix == "line3");
    REQUIRE(fim.middle.empty());
}

TEST_CASE("buildFimContext computes prefix/suffix for mid-line cursor", "[ghost][fim]")
{
    std::string doc = "hello world";
    auto fim = buildFimContext(doc, 0, 6);

    REQUIRE(fim.docPrefix == "hello ");
    REQUIRE(fim.docSuffix == "world");
}

TEST_CASE("buildFimContext handles out-of-range line (clamped to end)", "[ghost][fim]")
{
    std::string doc = "one line";
    auto fim = buildFimContext(doc, 100, 0);

    // Out-of-range line → offset clamped to end of document
    REQUIRE(fim.docPrefix == "one line");
    REQUIRE(fim.docSuffix == "");
}

TEST_CASE("buildFimContext handles empty document", "[ghost][fim]")
{
    std::string doc = "";
    auto fim = buildFimContext(doc, 0, 0);

    REQUIRE(fim.docPrefix == "");
    REQUIRE(fim.docSuffix == "");
    REQUIRE(fim.middle.empty());
}

TEST_CASE("buildFimContext handles cursor at end of document", "[ghost][fim]")
{
    std::string doc = "hello world";
    auto fim = buildFimContext(doc, 0, 11);

    REQUIRE(fim.docPrefix == "hello world");
    REQUIRE(fim.docSuffix == "");
}

TEST_CASE("buildFimContext handles multi-line document with cursor mid-line", "[ghost][fim]")
{
    std::string doc = "first\nsecond\nthird";
    auto fim = buildFimContext(doc, 1, 3);

    // line 0: "first\n" (6 chars), cursor on line 1 at char 3 → offset = 6+3 = 9
    REQUIRE(fim.docPrefix == "first\nsec");
    REQUIRE(fim.docSuffix == "ond\nthird");
}

// ===========================================================================
// GhostCompletionLogic — basic state
// ===========================================================================

TEST_CASE("GhostCompletionLogic starts disabled", "[ghost][logic]")
{
    GhostCompletionLogic logic;

    GhostContext ctx;
    ctx.documentText = "bd sn";
    ctx.languageId = "hathor";
    ctx.uri = "file:///test.hathor";

    auto result = logic.onEditorChanged(ctx, 0);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("GhostCompletionLogic returns nullopt on editor change (debounce mode)", "[ghost][logic]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "bd";
    ctx.languageId = "hathor";
    ctx.uri = "file:///test.hathor";

    auto result = logic.onEditorChanged(ctx, 0);
    REQUIRE_FALSE(result.has_value());

    REQUIRE(logic.hasPendingRequest() == false);
    REQUIRE(logic.currentRevision() == 1);
}

// ===========================================================================
// GhostCompletionLogic — debounce
// ===========================================================================

TEST_CASE("GhostCompletionLogic sends request after debounce expires", "[ghost][logic]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "bd";
    ctx.languageId = "hathor";
    ctx.uri = "file:///test.hathor";

    logic.onEditorChanged(ctx, 0);

    // Not enough time has elapsed
    int64_t t1 = 100;
    auto r1 = logic.onTimerTick(t1);
    REQUIRE_FALSE(r1.has_value());

    // Debounce elapsed — should return a request
    int64_t t2 = 500;
    auto r2 = logic.onTimerTick(t2);
    REQUIRE(r2.has_value());

    auto [req, requestId] = r2.value();
    REQUIRE(!requestId.empty());
    REQUIRE(req.uri == "file:///test.hathor");
    REQUIRE(req.languageId == "hathor");
    REQUIRE(req.textDocument == "bd");
    REQUIRE(req.fim.enabled == true);
    REQUIRE(req.line == 0);
    REQUIRE(req.character == 0);
}

// ===========================================================================
// GhostCompletionLogic — stale response rejection
// ===========================================================================

TEST_CASE("GhostCompletionLogic rejects stale response (revision mismatch)", "[ghost][logic]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(100);
    logic.setTimeoutMs(5000);

    GhostContext ctx;
    ctx.documentText = "bd";
    ctx.uri = "file:///test.hathor";

    // First request cycle at t=0
    logic.onEditorChanged(ctx, 0);
    auto r1 = logic.onTimerTick(200);
    REQUIRE(r1.has_value());
    std::string requestId1 = r1.value().second;
    REQUIRE(logic.hasPendingRequest());

    // Editor changed during flight — revision increments, makes response stale
    ctx.documentText = "bd sn";
    logic.onEditorChanged(ctx, 300);
    REQUIRE(logic.hasPendingRequest()); // still pending

    // Response for the old request arrives — stale
    GhostCompletionResponse staleResp;
    staleResp.request_id = requestId1;
    staleResp.completions = {{.generatedText = "bd sn hh cp"}};
    auto result = logic.onGhostResponse(requestId1, staleResp, 400);
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(logic.hasPendingRequest());
}

// ===========================================================================
// GhostCompletionLogic — valid response
// ===========================================================================

TEST_CASE("GhostCompletionLogic accepts valid response", "[ghost][logic]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(100);
    logic.setTimeoutMs(5000);

    GhostContext ctx;
    ctx.documentText = "bd";
    ctx.uri = "file:///test.hathor";

    logic.onEditorChanged(ctx, 0);
    auto r1 = logic.onTimerTick(200);
    REQUIRE(r1.has_value());
    std::string requestId = r1.value().second;

    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = "bd sn hh cp"}};

    auto result = logic.onGhostResponse(requestId, resp, 300);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "bd sn hh cp");
    REQUIRE(result->cursorLine == 0);
    REQUIRE(result->character == 0);
    REQUIRE(logic.hasActiveGhost());
}

TEST_CASE("GhostCompletionLogic rejects mismatched request ID", "[ghost][logic]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(100);
    logic.setTimeoutMs(5000);

    GhostContext ctx;
    ctx.uri = "file:///test.hathor";
    logic.onEditorChanged(ctx, 0);
    auto r1 = logic.onTimerTick(200);
    REQUIRE(r1.has_value());
    std::string correctId = r1.value().second;

    GhostCompletionResponse resp;
    resp.request_id = "wrong-id";
    resp.completions = {{.generatedText = "text"}};

    auto result = logic.onGhostResponse(correctId, resp, 300);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(logic.hasPendingRequest());
}

// ===========================================================================
// GhostCompletionLogic — timeout
// ===========================================================================

TEST_CASE("GhostCompletionLogic timeout clears pending request", "[ghost][logic]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(100);

    GhostContext ctx;
    ctx.uri = "file:///test.hathor";
    logic.onEditorChanged(ctx, 0);

    // Request fires immediately (debounce=0)
    auto r1 = logic.onTimerTick(0);
    REQUIRE(r1.has_value());
    REQUIRE(logic.hasPendingRequest());

    // Wait for timeout (sentAtMs=0, nowMs=200, 200 > 100 → timeout)
    auto r2 = logic.onTimerTick(200);
    REQUIRE_FALSE(r2.has_value());
    REQUIRE_FALSE(logic.hasPendingRequest());
}

// ===========================================================================
// GhostCompletionLogic — accept / reject
// ===========================================================================

TEST_CASE("GhostCompletionLogic onAccept returns params and clears ghost", "[ghost][logic]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    GhostContext ctx;
    ctx.uri = "file:///test.hathor";
    ctx.line = 0;
    ctx.character = 2;
    logic.onEditorChanged(ctx, 0);

    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    std::string requestId = r.value().second;

    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = " completion"}};

    auto ghostResult = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(ghostResult.has_value());
    REQUIRE(logic.hasActiveGhost());

    auto acceptParams = logic.onAccept();
    REQUIRE(acceptParams.has_value());
    REQUIRE(acceptParams->requestId == requestId);
    REQUIRE(acceptParams->acceptedCompletion == 0);
    REQUIRE(acceptParams->shownCompletions.size() == 1);
    REQUIRE(acceptParams->shownCompletions[0] == 0);

    REQUIRE_FALSE(logic.hasActiveGhost());
}

TEST_CASE("GhostCompletionLogic onReject returns params and clears ghost", "[ghost][logic]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    GhostContext ctx;
    ctx.uri = "file:///test.hathor";
    ctx.line = 0;
    ctx.character = 2;
    logic.onEditorChanged(ctx, 0);

    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    std::string requestId = r.value().second;

    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = " foo"}};

    auto ghostResult = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(ghostResult.has_value());
    REQUIRE(logic.hasActiveGhost());

    auto rejectParams = logic.onReject();
    REQUIRE(rejectParams.has_value());
    REQUIRE(rejectParams->requestId == requestId);
    REQUIRE(rejectParams->shownCompletions.size() == 1);
    REQUIRE(rejectParams->shownCompletions[0] == 0);

    REQUIRE_FALSE(logic.hasActiveGhost());
}

// ===========================================================================
// GhostCompletionLogic — cancel and provider failure
// ===========================================================================

TEST_CASE("GhostCompletionLogic cancelPendingRequest clears state", "[ghost][logic]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    GhostContext ctx;
    ctx.uri = "file:///test.hathor";
    logic.onEditorChanged(ctx, 0);

    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    REQUIRE(logic.hasPendingRequest());

    logic.cancelPendingRequest();
    REQUIRE_FALSE(logic.hasPendingRequest());
}

TEST_CASE("GhostCompletionLogic onProviderFailure clears ghost", "[ghost][logic]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    GhostContext ctx;
    ctx.uri = "file:///test.hathor";
    logic.onEditorChanged(ctx, 0);

    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    std::string requestId = r.value().second;

    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = "test"}};
    logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(logic.hasActiveGhost());

    auto cleared = logic.onProviderFailure();
    REQUIRE(cleared.has_value());
    REQUIRE_FALSE(logic.hasActiveGhost());
    REQUIRE_FALSE(logic.hasPendingRequest());
}

// ===========================================================================
// GhostCompletionLogic — buildRequest
// ===========================================================================

TEST_CASE("GhostCompletionLogic.buildRequest populates FIM and backend", "[ghost][logic]")
{
    GhostContext ctx;
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 2;
    ctx.character = 3;
    ctx.documentText = "first\nsecond\nthird";

    GhostProviderConfig config;
    config.backend = LlmBackend::HuggingFace;
    config.url = "https://api-inference.huggingface.co";
    config.model = "bigcode/starcoder";
    config.apiToken = "test-token";
    config.contextWindow = 4096;

    GhostCompletionRequest req = GhostCompletionLogic::buildRequest(ctx, config);

    REQUIRE(req.uri == "file:///test.hathor");
    REQUIRE(req.languageId == "hathor");
    REQUIRE(req.line == 2);
    REQUIRE(req.character == 3);
    REQUIRE(req.textDocument == "first\nsecond\nthird");
    REQUIRE(req.fim.enabled == true);
    REQUIRE(req.backend.backend == LlmBackend::HuggingFace);
    REQUIRE(req.backend.url == "https://api-inference.huggingface.co");
    REQUIRE(req.backend.model == "bigcode/starcoder");
    REQUIRE(req.apiToken == "test-token");
    REQUIRE(req.contextWindow == 4096);
    REQUIRE(req.tokenizerConfig.empty());

    // AI-G2: verify docPrefix/docSuffix are computed
    REQUIRE(req.docPrefix == "first\nsecond\nthi");
    REQUIRE(req.docSuffix == "rd");
}

TEST_CASE("GhostCompletionLogic.currentContext returns last edit context", "[ghost][logic]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);

    GhostContext ctx;
    ctx.uri = "file:///test.hathor";
    ctx.documentText = "hello";
    ctx.line = 1;
    ctx.character = 3;

    logic.onEditorChanged(ctx, 0);

    const auto& stored = logic.currentContext();
    REQUIRE(stored.uri == "file:///test.hathor");
    REQUIRE(stored.documentText == "hello");
    REQUIRE(stored.line == 1);
    REQUIRE(stored.character == 3);
}

// ===========================================================================
// GhostJsonRpc — request ID generation
// ===========================================================================

TEST_CASE("GhostJsonRpc.generateRequestId produces UUID v4 strings", "[ghost][jsonrpc]")
{
    std::string id1 = GhostJsonRpc::generateRequestId();
    std::string id2 = GhostJsonRpc::generateRequestId();

    REQUIRE_FALSE(id1.empty());
    REQUIRE_FALSE(id2.empty());
    REQUIRE(id1 != id2);

    // UUID v4 format: 8-4-4-4-12 hex chars with hyphens
    REQUIRE(id1.length() == 36);
    REQUIRE(id1[8] == '-');
    REQUIRE(id1[13] == '-');
    REQUIRE(id1[18] == '-');
    REQUIRE(id1[23] == '-');

    // Version nibble should be 4 (UUID v4)
    REQUIRE(id1[14] == '4');
}

// ===========================================================================
// GhostJsonRpc — response parsing
// ===========================================================================

TEST_CASE("GhostJsonRpc.parseResponse extracts string ID", "[ghost][jsonrpc]")
{
    std::string jsonStr = R"({
        "jsonrpc": "2.0",
        "id": "abc-123-def",
        "result": {"request_id": "abc-123-def", "completions": []}
    })";

    std::string id;
    auto result = GhostJsonRpc::parseResponse(jsonStr, id);

    REQUIRE(result.has_value());
    REQUIRE(id == "abc-123-def");
    REQUIRE(result->contains("request_id"));
    REQUIRE(result->contains("completions"));
}

TEST_CASE("GhostJsonRpc.parseResponse extracts integer ID as string", "[ghost][jsonrpc]")
{
    std::string jsonStr = R"({
        "jsonrpc": "2.0",
        "id": 42,
        "result": {"key": "value"}
    })";

    std::string id;
    auto result = GhostJsonRpc::parseResponse(jsonStr, id);

    REQUIRE(result.has_value());
    REQUIRE(id == "42");
}

TEST_CASE("GhostJsonRpc.parseResponse returns nullopt for invalid JSON", "[ghost][jsonrpc]")
{
    std::string id;
    auto result = GhostJsonRpc::parseResponse("not json", id);
    REQUIRE_FALSE(result.has_value());
}

// ===========================================================================
// GhostJsonRpc — initialize / notification serialization
// ===========================================================================

TEST_CASE("GhostJsonRpc.serializeInitialize produces framed LSP message", "[ghost][jsonrpc]")
{
    GhostJsonRpc rpc;
    std::string msg = rpc.serializeInitialize();

    REQUIRE(msg.find("Content-Length:") != std::string::npos);
    REQUIRE(msg.find("initialize") != std::string::npos);
    REQUIRE(msg.find("jsonrpc") != std::string::npos);
}

TEST_CASE("GhostJsonRpc.serializeInitialized produces notification", "[ghost][jsonrpc]")
{
    GhostJsonRpc rpc;
    std::string msg = rpc.serializeInitialized();

    auto headerEnd = msg.find("\r\n\r\n");
    REQUIRE(headerEnd != std::string::npos);
    auto body = msg.substr(headerEnd + 4);
    auto j = nlohmann::json::parse(body);

    REQUIRE(j["jsonrpc"] == "2.0");
    REQUIRE(j["method"] == "initialized");
    REQUIRE_FALSE(j.contains("id"));
}

TEST_CASE("GhostJsonRpc.serializeGhostCompletion returns request ID and framed message", "[ghost][jsonrpc]")
{
    GhostCompletionRequest req;
    req.uri = "file:///test.hathor";
    req.languageId = "hathor";
    req.line = 0;
    req.character = 5;
    req.textDocument = "bd sn";
    req.fim.enabled = true;
    req.fim.prefix = "";
    req.fim.suffix = "";
    req.fim.middle = "";
    req.backend.model = "starcoder";
    req.apiToken = "token123";

    GhostJsonRpc rpc;
    auto [requestId, framed] = rpc.serializeGhostCompletion(req);

    REQUIRE_FALSE(requestId.empty());
    REQUIRE(framed.find("Content-Length:") != std::string::npos);

    auto headerEnd = framed.find("\r\n\r\n");
    auto body = framed.substr(headerEnd + 4);
    auto j = nlohmann::json::parse(body);

    REQUIRE(j["jsonrpc"] == "2.0");
    REQUIRE(j["id"] == requestId);
    REQUIRE(j["method"] == "llm-ls/getCompletions");
    REQUIRE(j["params"]["textDocument"]["uri"] == "file:///test.hathor");
    REQUIRE(j["params"]["textDocument"]["languageId"] == "hathor");
    REQUIRE_FALSE(j["params"]["textDocument"].contains("text"));
    REQUIRE(j["params"]["position"]["line"] == 0);
    REQUIRE(j["params"]["fim"]["enabled"] == true);
    REQUIRE(j["params"]["model"] == "starcoder");
    REQUIRE(j["params"]["api_token"] == "token123");
    REQUIRE(j["params"]["ide"] == "unknown");
    REQUIRE(j["params"]["backend"]["backend"] == "huggingface");
    REQUIRE(j["params"]["tokenizer_config"].is_null());
}

TEST_CASE("GhostJsonRpc.serializeAcceptCompletion produces notification", "[ghost][jsonrpc]")
{
    AcceptCompletionParams params;
    params.requestId = "req-123";
    params.acceptedCompletion = 0;
    params.shownCompletions = {0};

    GhostJsonRpc rpc;
    std::string framed = rpc.serializeAcceptCompletion(params);

    auto headerEnd = framed.find("\r\n\r\n");
    auto body = framed.substr(headerEnd + 4);
    auto j = nlohmann::json::parse(body);

    REQUIRE(j["jsonrpc"] == "2.0");
    REQUIRE(j["method"] == "llm-ls/acceptCompletion");
    REQUIRE(j["params"]["request_id"] == "req-123");
    REQUIRE(j["params"]["accepted_completion"] == 0);
    REQUIRE(j["params"]["shown_completions"][0] == 0);
}

TEST_CASE("GhostJsonRpc.serializeRejectCompletion produces notification", "[ghost][jsonrpc]")
{
    RejectCompletionParams params;
    params.requestId = "req-456";
    params.shownCompletions = {0};

    GhostJsonRpc rpc;
    std::string framed = rpc.serializeRejectCompletion(params);

    auto headerEnd = framed.find("\r\n\r\n");
    auto body = framed.substr(headerEnd + 4);
    auto j = nlohmann::json::parse(body);

    REQUIRE(j["jsonrpc"] == "2.0");
    REQUIRE(j["method"] == "llm-ls/rejectCompletion");
    REQUIRE(j["params"]["request_id"] == "req-456");
    REQUIRE(j["params"]["shown_completions"][0] == 0);
}

TEST_CASE("GhostJsonRpc.serializeDidOpen / DidChange / DidClose", "[ghost][jsonrpc]")
{
    GhostJsonRpc rpc;

    auto [ver, framedOpen] = rpc.serializeDidOpen("file:///test.hathor", "hathor", 1, "bd sn");
    REQUIRE(ver == 1);

    auto headerEnd = framedOpen.find("\r\n\r\n");
    auto body = framedOpen.substr(headerEnd + 4);
    auto j = nlohmann::json::parse(body);
    REQUIRE(j["method"] == "textDocument/didOpen");
    REQUIRE(j["params"]["textDocument"]["uri"] == "file:///test.hathor");
    REQUIRE(j["params"]["textDocument"]["languageId"] == "hathor");
    REQUIRE(j["params"]["textDocument"]["version"] == 1);
    REQUIRE(j["params"]["textDocument"]["text"] == "bd sn");

    auto [ver2, framedChange] = rpc.serializeDidChange("file:///test.hathor", 2, "bd sn hh");
    REQUIRE(ver2 == 2);

    headerEnd = framedChange.find("\r\n\r\n");
    body = framedChange.substr(headerEnd + 4);
    j = nlohmann::json::parse(body);
    REQUIRE(j["method"] == "textDocument/didChange");
    REQUIRE(j["params"]["textDocument"]["version"] == 2);
    REQUIRE(j["params"]["contentChanges"][0]["text"] == "bd sn hh");

    std::string framedClose = rpc.serializeDidClose("file:///test.hathor");
    headerEnd = framedClose.find("\r\n\r\n");
    body = framedClose.substr(headerEnd + 4);
    j = nlohmann::json::parse(body);
    REQUIRE(j["method"] == "textDocument/didClose");
    REQUIRE(j["params"]["textDocument"]["uri"] == "file:///test.hathor");
}

// ===========================================================================
// parseGhostCompletionResponse
// ===========================================================================

TEST_CASE("parseGhostCompletionResponse parses standard response", "[ghost][jsonrpc]")
{
    nlohmann::json j = {
        {"request_id", "abc-123"},
        {"completions", {
            {{"generated_text", "bd sn hh cp"}}
        }}
    };

    auto resp = parseGhostCompletionResponse(j);
    REQUIRE(resp.has_value());
    REQUIRE(resp->request_id == "abc-123");
    REQUIRE(resp->completions.size() == 1);
    REQUIRE(resp->completions[0].generatedText == "bd sn hh cp");
}

TEST_CASE("parseGhostCompletionResponse parses wrapped-in-result response", "[ghost][jsonrpc]")
{
    nlohmann::json j = {
        {"jsonrpc", "2.0"},
        {"id", "abc-123"},
        {"result", {
            {"request_id", "abc-123"},
            {"completions", {
                {{"generated_text", "result"}}
            }}
        }}
    };

    auto resp = parseGhostCompletionResponse(j);
    REQUIRE(resp.has_value());
    REQUIRE(resp->request_id == "abc-123");
    REQUIRE(resp->completions.size() == 1);
    REQUIRE(resp->completions[0].generatedText == "result");
}

TEST_CASE("parseGhostCompletionResponse returns nullopt for null", "[ghost][jsonrpc]")
{
    nlohmann::json j = nullptr;
    auto resp = parseGhostCompletionResponse(j);
    REQUIRE_FALSE(resp.has_value());
}

TEST_CASE("parseGhostCompletionResponse handles multiple completions", "[ghost][jsonrpc]")
{
    nlohmann::json j = {
        {"request_id", "multi-1"},
        {"completions", {
            {{"generated_text", "first"}},
            {{"generated_text", "second"}}
        }}
    };

    auto resp = parseGhostCompletionResponse(j);
    REQUIRE(resp.has_value());
    REQUIRE(resp->completions.size() == 2);
    REQUIRE(resp->completions[0].generatedText == "first");
    REQUIRE(resp->completions[1].generatedText == "second");
}

// ===========================================================================
// GhostProviderResolver — env-var resolution
// ===========================================================================

TEST_CASE("GhostProviderResolver.isEnabled respects GHOST_ENABLED", "[ghost][provider]")
{
    const char* saved = std::getenv("GHOST_ENABLED");
    std::string savedVal = saved ? saved : "";

    unsetenv("GHOST_ENABLED");
    REQUIRE_FALSE(GhostProviderResolver::isEnabled());

    setenv("GHOST_ENABLED", "1", 1);
    REQUIRE(GhostProviderResolver::isEnabled());

    setenv("GHOST_ENABLED", "true", 1);
    REQUIRE(GhostProviderResolver::isEnabled());

    setenv("GHOST_ENABLED", "0", 1);
    REQUIRE_FALSE(GhostProviderResolver::isEnabled());

    setenv("GHOST_ENABLED", "false", 1);
    REQUIRE_FALSE(GhostProviderResolver::isEnabled());

    if (savedVal.empty())
        unsetenv("GHOST_ENABLED");
    else
        setenv("GHOST_ENABLED", savedVal.c_str(), 1);
}

TEST_CASE("GhostProviderResolver.resolve returns nullopt when no model set", "[ghost][provider]")
{
    const char* savedEnabled = std::getenv("GHOST_ENABLED");
    const char* savedBackend = std::getenv("GHOST_BACKEND");
    const char* savedModel = std::getenv("GHOST_MODEL");
    std::string savedEnabledVal = savedEnabled ? savedEnabled : "";
    std::string savedBackendVal = savedBackend ? savedBackend : "";
    std::string savedModelVal = savedModel ? savedModel : "";

    setenv("GHOST_ENABLED", "1", 1);
    setenv("GHOST_BACKEND", "huggingface", 1);
    unsetenv("GHOST_MODEL");

    auto result = GhostProviderResolver::resolve();
    REQUIRE_FALSE(result.has_value());

    // Restore
    if (savedEnabledVal.empty()) unsetenv("GHOST_ENABLED");
    else setenv("GHOST_ENABLED", savedEnabledVal.c_str(), 1);
    if (savedBackendVal.empty()) unsetenv("GHOST_BACKEND");
    else setenv("GHOST_BACKEND", savedBackendVal.c_str(), 1);
    if (savedModelVal.empty()) unsetenv("GHOST_MODEL");
    else setenv("GHOST_MODEL", savedModelVal.c_str(), 1);
}

TEST_CASE("GhostProviderResolver.resolve returns nullopt for unknown backend", "[ghost][provider]")
{
    const char* savedEnabled = std::getenv("GHOST_ENABLED");
    const char* savedBackend = std::getenv("GHOST_BACKEND");
    const char* savedModel = std::getenv("GHOST_MODEL");
    std::string savedEnabledVal = savedEnabled ? savedEnabled : "";
    std::string savedBackendVal = savedBackend ? savedBackend : "";
    std::string savedModelVal = savedModel ? savedModel : "";

    setenv("GHOST_ENABLED", "1", 1);
    setenv("GHOST_BACKEND", "unknown-backend", 1);
    setenv("GHOST_MODEL", "test-model", 1);

    auto result = GhostProviderResolver::resolve();
    REQUIRE_FALSE(result.has_value());

    // Restore
    if (savedEnabledVal.empty()) unsetenv("GHOST_ENABLED");
    else setenv("GHOST_ENABLED", savedEnabledVal.c_str(), 1);
    if (savedBackendVal.empty()) unsetenv("GHOST_BACKEND");
    else setenv("GHOST_BACKEND", savedBackendVal.c_str(), 1);
    if (savedModelVal.empty()) unsetenv("GHOST_MODEL");
    else setenv("GHOST_MODEL", savedModelVal.c_str(), 1);
}

TEST_CASE("GhostProviderResolver.resolve resolves HuggingFace config", "[ghost][provider]")
{
    const char* savedEnabled = std::getenv("GHOST_ENABLED");
    const char* savedBackend = std::getenv("GHOST_BACKEND");
    const char* savedModel = std::getenv("GHOST_MODEL");
    const char* savedToken = std::getenv("HF_API_TOKEN");
    std::string savedEnabledVal = savedEnabled ? savedEnabled : "";
    std::string savedBackendVal = savedBackend ? savedBackend : "";
    std::string savedModelVal = savedModel ? savedModel : "";
    std::string savedTokenVal = savedToken ? savedToken : "";

    setenv("GHOST_ENABLED", "1", 1);
    setenv("GHOST_BACKEND", "huggingface", 1);
    setenv("GHOST_MODEL", "bigcode/starcoder", 1);
    setenv("HF_API_TOKEN", "hf-test-token", 1);

    auto result = GhostProviderResolver::resolve();
    REQUIRE(result.has_value());
    REQUIRE(result->backend == LlmBackend::HuggingFace);
    REQUIRE(result->model == "bigcode/starcoder");
    REQUIRE(result->apiToken == "hf-test-token");
    REQUIRE(result->url == "https://api-inference.huggingface.co");
    REQUIRE(result->contextWindow == 2048);
    REQUIRE_FALSE(result->tlsSkipVerify);
    REQUIRE(result->tokenizerConfig == "default");

    // Restore
    if (savedEnabledVal.empty()) unsetenv("GHOST_ENABLED");
    else setenv("GHOST_ENABLED", savedEnabledVal.c_str(), 1);
    if (savedBackendVal.empty()) unsetenv("GHOST_BACKEND");
    else setenv("GHOST_BACKEND", savedBackendVal.c_str(), 1);
    if (savedModelVal.empty()) unsetenv("GHOST_MODEL");
    else setenv("GHOST_MODEL", savedModelVal.c_str(), 1);
    if (savedTokenVal.empty()) unsetenv("HF_API_TOKEN");
    else setenv("HF_API_TOKEN", savedTokenVal.c_str(), 1);
}

TEST_CASE("GhostProviderResolver.resolve resolves OpenAi config", "[ghost][provider]")
{
    const char* savedEnabled = std::getenv("GHOST_ENABLED");
    const char* savedBackend = std::getenv("GHOST_BACKEND");
    const char* savedModel = std::getenv("GHOST_MODEL");
    const char* savedToken = std::getenv("OPENAI_API_KEY");
    std::string savedEnabledVal = savedEnabled ? savedEnabled : "";
    std::string savedBackendVal = savedBackend ? savedBackend : "";
    std::string savedModelVal = savedModel ? savedModel : "";
    std::string savedTokenVal = savedToken ? savedToken : "";

    setenv("GHOST_ENABLED", "1", 1);
    setenv("GHOST_BACKEND", "openai", 1);
    setenv("GHOST_MODEL", "gpt-3.5-turbo", 1);
    setenv("OPENAI_API_KEY", "sk-test-123", 1);

    auto result = GhostProviderResolver::resolve();
    REQUIRE(result.has_value());
    REQUIRE(result->backend == LlmBackend::OpenAi);
    REQUIRE(result->model == "gpt-3.5-turbo");
    REQUIRE(result->apiToken == "sk-test-123");
    REQUIRE(result->url == "https://api.openai.com");

    // Restore
    if (savedEnabledVal.empty()) unsetenv("GHOST_ENABLED");
    else setenv("GHOST_ENABLED", savedEnabledVal.c_str(), 1);
    if (savedBackendVal.empty()) unsetenv("GHOST_BACKEND");
    else setenv("GHOST_BACKEND", savedBackendVal.c_str(), 1);
    if (savedModelVal.empty()) unsetenv("GHOST_MODEL");
    else setenv("GHOST_MODEL", savedModelVal.c_str(), 1);
    if (savedTokenVal.empty()) unsetenv("OPENAI_API_KEY");
    else setenv("OPENAI_API_KEY", savedTokenVal.c_str(), 1);
}

TEST_CASE("GhostProviderResolver.resolve respects GHOST_URL override", "[ghost][provider]")
{
    const char* savedEnabled = std::getenv("GHOST_ENABLED");
    const char* savedBackend = std::getenv("GHOST_BACKEND");
    const char* savedModel = std::getenv("GHOST_MODEL");
    const char* savedUrl = std::getenv("GHOST_URL");
    std::string savedEnabledVal = savedEnabled ? savedEnabled : "";
    std::string savedBackendVal = savedBackend ? savedBackend : "";
    std::string savedModelVal = savedModel ? savedModel : "";
    std::string savedUrlVal = savedUrl ? savedUrl : "";

    setenv("GHOST_ENABLED", "1", 1);
    setenv("GHOST_BACKEND", "ollama", 1);
    setenv("GHOST_MODEL", "codellama", 1);
    setenv("GHOST_URL", "http://localhost:11434", 1);

    auto result = GhostProviderResolver::resolve();
    REQUIRE(result.has_value());
    REQUIRE(result->backend == LlmBackend::Ollama);
    REQUIRE(result->model == "codellama");
    REQUIRE(result->url == "http://localhost:11434");

    // Restore
    if (savedEnabledVal.empty()) unsetenv("GHOST_ENABLED");
    else setenv("GHOST_ENABLED", savedEnabledVal.c_str(), 1);
    if (savedBackendVal.empty()) unsetenv("GHOST_BACKEND");
    else setenv("GHOST_BACKEND", savedBackendVal.c_str(), 1);
    if (savedModelVal.empty()) unsetenv("GHOST_MODEL");
    else setenv("GHOST_MODEL", savedModelVal.c_str(), 1);
    if (savedUrlVal.empty()) unsetenv("GHOST_URL");
    else setenv("GHOST_URL", savedUrlVal.c_str(), 1);
}

TEST_CASE("GhostProviderResolver.resolve respects GHOST_CONTEXT_WINDOW", "[ghost][provider]")
{
    const char* savedEnabled = std::getenv("GHOST_ENABLED");
    const char* savedBackend = std::getenv("GHOST_BACKEND");
    const char* savedModel = std::getenv("GHOST_MODEL");
    const char* savedToken = std::getenv("HF_API_TOKEN");
    const char* savedCw = std::getenv("GHOST_CONTEXT_WINDOW");
    std::string savedEnabledVal = savedEnabled ? savedEnabled : "";
    std::string savedBackendVal = savedBackend ? savedBackend : "";
    std::string savedModelVal = savedModel ? savedModel : "";
    std::string savedTokenVal = savedToken ? savedToken : "";
    std::string savedCwVal = savedCw ? savedCw : "";

    setenv("GHOST_ENABLED", "1", 1);
    setenv("GHOST_BACKEND", "huggingface", 1);
    setenv("GHOST_MODEL", "starcoder", 1);
    setenv("HF_API_TOKEN", "tok", 1);
    setenv("GHOST_CONTEXT_WINDOW", "8192", 1);

    auto result = GhostProviderResolver::resolve();
    REQUIRE(result.has_value());
    REQUIRE(result->contextWindow == 8192);

    // Restore
    if (savedEnabledVal.empty()) unsetenv("GHOST_ENABLED");
    else setenv("GHOST_ENABLED", savedEnabledVal.c_str(), 1);
    if (savedBackendVal.empty()) unsetenv("GHOST_BACKEND");
    else setenv("GHOST_BACKEND", savedBackendVal.c_str(), 1);
    if (savedModelVal.empty()) unsetenv("GHOST_MODEL");
    else setenv("GHOST_MODEL", savedModelVal.c_str(), 1);
    if (savedTokenVal.empty()) unsetenv("HF_API_TOKEN");
    else setenv("HF_API_TOKEN", savedTokenVal.c_str(), 1);
    if (savedCwVal.empty()) unsetenv("GHOST_CONTEXT_WINDOW");
    else setenv("GHOST_CONTEXT_WINDOW", savedCwVal.c_str(), 1);
}

// ===========================================================================
// GhostProviderConfig — validity
// ===========================================================================

TEST_CASE("GhostProviderConfig.valid returns false when model empty", "[ghost][provider]")
{
    GhostProviderConfig config;
    config.model = "";
    config.backend = LlmBackend::HuggingFace;
    config.apiToken = "token";
    REQUIRE_FALSE(config.valid());
}

TEST_CASE("GhostProviderConfig.valid returns true for HuggingFace with token", "[ghost][provider]")
{
    GhostProviderConfig config;
    config.model = "starcoder";
    config.backend = LlmBackend::HuggingFace;
    config.apiToken = "token";
    REQUIRE(config.valid());
}

TEST_CASE("GhostProviderConfig.valid returns true for local backend without token", "[ghost][provider]")
{
    GhostProviderConfig config;
    config.model = "codellama";
    config.backend = LlmBackend::Ollama;
    config.apiToken = "";
    REQUIRE(config.valid());
}

// ===========================================================================
// backendToString
// ===========================================================================

TEST_CASE("backendToString maps all enum values", "[ghost][protocol]")
{
    REQUIRE(backendToString(LlmBackend::HuggingFace) == "huggingface");
    REQUIRE(backendToString(LlmBackend::LlamaCpp) == "llamacpp");
    REQUIRE(backendToString(LlmBackend::Ollama) == "ollama");
    REQUIRE(backendToString(LlmBackend::OpenAi) == "openai");
    REQUIRE(backendToString(LlmBackend::Tgi) == "tgi");
}

// ===========================================================================
// Language identification: .hathor and .ck
// ===========================================================================

TEST_CASE("GhostCompletionRequest serializes .hathor languageId", "[ghost][language]")
{
    GhostCompletionRequest req;
    req.uri = "file:///test.hathor";
    req.languageId = "hathor";
    req.textDocument = "busking in hathor";
    req.fim.enabled = false;
    req.backend.model = "starcoder";

    nlohmann::json j = req.toJson();
    REQUIRE(j["textDocument"]["languageId"] == "hathor");
}

TEST_CASE("GhostCompletionRequest serializes .ck languageId as chuck", "[ghost][language]")
{
    GhostCompletionRequest req;
    req.uri = "file:///test.ck";
    req.languageId = "chuck";
    req.textDocument = "SinOsc s => dac;";
    req.fim.enabled = false;
    req.backend.model = "starcoder";

    nlohmann::json j = req.toJson();
    REQUIRE(j["textDocument"]["languageId"] == "chuck");
}

// ===========================================================================
// GhostCompletionRequest::toJson
// ===========================================================================

TEST_CASE("GhostCompletionRequest.toJson produces expected structure", "[ghost][protocol]")
{
    GhostCompletionRequest req;
    req.uri = "file:///test.hathor";
    req.languageId = "hathor";
    req.line = 1;
    req.character = 2;
    req.textDocument = "hello world";
    req.fim.enabled = true;
    req.fim.prefix = "";
    req.fim.suffix = "";
    req.backend.backend = LlmBackend::HuggingFace;
    req.backend.url = "https://api-inference.huggingface.co";
    req.backend.model = "starcoder";
    req.apiToken = "tok";
    req.tokenizerConfig = "my-tokenizer";
    req.contextWindow = 1024;
    req.tlsSkipVerify = false;

    nlohmann::json j = req.toJson();

    REQUIRE(j["textDocument"]["uri"] == "file:///test.hathor");
    REQUIRE(j["textDocument"]["languageId"] == "hathor");
    REQUIRE_FALSE(j["textDocument"].contains("text"));
    REQUIRE(j["position"]["line"] == 1);
    REQUIRE(j["position"]["character"] == 2);
    REQUIRE(j["ide"] == "unknown");
    REQUIRE(j["fim"]["enabled"] == true);
    REQUIRE(j["backend"]["backend"] == "huggingface");
    REQUIRE(j["backend"]["url"] == "https://api-inference.huggingface.co");
    REQUIRE(j["model"] == "starcoder");
    REQUIRE(j["api_token"] == "tok");
    REQUIRE(j["tokenizer_config"] == nlohmann::json{{"path", "my-tokenizer"}});
    REQUIRE(j["context_window"] == 1024);
    REQUIRE(j["tls_skip_verify_insecure"] == false);
}

TEST_CASE("GhostCompletionRequest.toJson sends null for empty api_token", "[ghost][protocol]")
{
    GhostCompletionRequest req;
    req.uri = "file:///test.hathor";
    req.apiToken = "";
    req.backend.model = "starcoder";
    req.tokenizerConfig = "";
    req.contextWindow = 2048;

    nlohmann::json j = req.toJson();
    REQUIRE(j["api_token"].is_null());
    REQUIRE(j["tokenizer_config"].is_null());
}

// ===========================================================================
// AI-G2: FIM response trimming
// ===========================================================================

TEST_CASE("onGhostResponse trims suffix overlap from generated text", "[ghost][fim]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    // Allow ghost in strings — this FIM test exercises cursor inside a string
    // to verify suffix-trimming behavior; we bypass the trigger policy check.
    GhostTriggerPolicyConfig cfg;
    cfg.allowInStrings = true;
    logic.setTriggerPolicyConfig(cfg);

    // Document: "s(\"bd  sd hh\")\n" (two spaces between bd and sd)
    // docPrefix = "s(\"bd  ", docSuffix = "sd hh\")\n"
    GhostContext ctx;
    ctx.documentText = "s(\"bd  sd hh\")\n";
    ctx.languageId = "hathor";
    ctx.uri = "file:///test.hathor";
    ctx.line = 0;
    ctx.character = 7;

    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    std::string requestId = r.value().second;

    // LLM generates text that includes the suffix " sd hh\")\n" at the end.
    // After suffix trimming, only " sn" should remain.
    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = "sn sd hh\")\n"}};

    auto result = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "sn");
    REQUIRE(result->docPrefix == "s(\"bd  ");
    REQUIRE(result->docSuffix == "sd hh\")\n");
}

TEST_CASE("onGhostResponse trims prefix overlap from generated text", "[ghost][fim]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    // Document: "bd sd" + cursor + ""
    // docPrefix = "bd sd", docSuffix = ""
    GhostContext ctx;
    ctx.documentText = "bd sd";
    ctx.languageId = "hathor";
    ctx.uri = "file:///test.hathor";
    ctx.line = 0;
    ctx.character = 5;

    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    std::string requestId = r.value().second;

    // LLM repeats "bd sd" at the start, then generates " hh cp"
    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = "bd sd hh cp"}};

    auto result = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(result.has_value());
    // trimPrefixOverlap removes "bd sd" → " hh cp"; stripWhitespace removes the leading space
    REQUIRE(result->text == "hh cp");
}

TEST_CASE("onGhostResponse trims both prefix and suffix overlap", "[ghost][fim]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    // Document: "p())" — cursor at position 2 (inside the parens)
    // docPrefix = "p(", docSuffix = "))"
    GhostContext ctx;
    ctx.documentText = "p())";
    ctx.languageId = "hathor";
    ctx.uri = "file:///test.hathor";
    ctx.line = 0;
    ctx.character = 2;

    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    std::string requestId = r.value().second;

    // LLM generates "p( inner ))" — repeats prefix "p(" at start and suffix "))" at end
    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = "p( inner ))"}};

    auto result = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(result.has_value());
    // trimPrefixOverlap removes "p(" → " inner ))"
    // trimSuffixOverlap removes "))" → " inner "
    // stripWhitespace → "inner"
    REQUIRE(result->text == "inner");
}

TEST_CASE("onGhostResponse does not trim when no overlap exists", "[ghost][fim]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    GhostContext ctx;
    ctx.documentText = "bd ";
    ctx.languageId = "hathor";
    ctx.uri = "file:///test.hathor";
    ctx.line = 0;
    ctx.character = 3;

    // docPrefix = "bd ", docSuffix = ""
    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    std::string requestId = r.value().second;

    // LLM generates text with no overlap
    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = "sn hh"}};

    auto result = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "sn hh");
}

TEST_CASE("onGhostResponse strips whitespace before and after trimming", "[ghost][fim]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    GhostContext ctx;
    ctx.documentText = "bd ";
    ctx.languageId = "hathor";
    ctx.uri = "file:///test.hathor";
    ctx.line = 0;
    ctx.character = 3;

    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    std::string requestId = r.value().second;

    // LLM generates text with surrounding whitespace
    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = "  sn hh  "}};

    auto result = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "sn hh");
}

TEST_CASE("onGhostResponse trims and leaves non-overlapping text intact", "[ghost][fim]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    // Document: "start " + cursor + "end"
    // docPrefix = "start ", docSuffix = "end"
    GhostContext ctx;
    ctx.documentText = "start end";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 6;

    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    std::string requestId = r.value().second;

    // LLM generates text with no overlap with prefix or suffix
    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = "middle stuff"}};

    auto result = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "middle stuff");
    REQUIRE(result->docPrefix == "start ");
    REQUIRE(result->docSuffix == "end");
}

// ===========================================================================
// AI-G2: AI-8 authoring context propagation
// ===========================================================================

TEST_CASE("onTimerTick includes authoring context in fim.prefix", "[ghost][logic][ai8]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    GhostContext ctx;
    ctx.documentText = "bd";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 2;

    // AI-8: Set authoring context with supported-surface info
    nlohmann::json ai8 = {
        {"ok", true},
        {"sections", {
            {"metadata", {
                {"functions", {{"name", "s"}, {"name", "bd"}}},
                {"chuckApi", {{"name", "SinOsc"}}}
            }}
        }}
    };
    ctx.authoringContext = ai8;

    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());

    auto [req, requestId] = r.value();
    REQUIRE(req.fim.enabled == true);
    REQUIRE_FALSE(req.fim.prefix.empty());
    REQUIRE(req.docPrefix == "bd");
    REQUIRE(req.docSuffix == "");
}

TEST_CASE("onTimerTick handles null authoring context gracefully", "[ghost][logic][ai8]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    GhostContext ctx;
    ctx.documentText = "bd";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 2;

    // No authoring context set (null)
    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());

    auto [req, requestId] = r.value();
    REQUIRE(req.fim.enabled == true);
    REQUIRE(req.fim.prefix.empty());
}

TEST_CASE("buildRequest includes authoring context and doc prefix/suffix", "[ghost][logic][ai8]")
{
    GhostContext ctx;
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 1;
    ctx.character = 2;
    ctx.documentText = "hello\nworld";

    nlohmann::json ai8 = {
        {"ok", true},
        {"sections", {
            {"metadata", {{"functions", {{"name", "bd"}}}}}
        }}
    };
    ctx.authoringContext = ai8;

    GhostProviderConfig config;
    config.backend = LlmBackend::HuggingFace;
    config.model = "starcoder";

    GhostCompletionRequest req = GhostCompletionLogic::buildRequest(ctx, config);

    REQUIRE(req.fim.enabled == true);
    REQUIRE_FALSE(req.fim.prefix.empty());
    REQUIRE(req.docPrefix == "hello\nwo");
    REQUIRE(req.docSuffix == "rld");
    REQUIRE_FALSE(req.authoringContext.is_null());
    REQUIRE(req.authoringContext["ok"] == true);
}

// ===========================================================================
// AI-G2: FIM contract — prefix/suffix preservation in onGhostResponse
// ===========================================================================

TEST_CASE("GhostResult carries docPrefix/docSuffix from request", "[ghost][fim][result]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    // Document: "p())" — p, (, ), ) — cursor inside the parens at char 2
    // docPrefix = "p(", docSuffix = "))"
    GhostContext ctx;
    ctx.documentText = "p())";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 2;

    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    std::string requestId = r.value().second;

    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = "inner"}};

    auto result = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(result.has_value());
    REQUIRE(result->docPrefix == "p(");
    REQUIRE(result->docSuffix == "))");
    REQUIRE(result->text == "inner");
}

// ===========================================================================
// AI-G2: Empty document FIM edge cases
// ===========================================================================

TEST_CASE("onGhostResponse handles empty docPrefix and docSuffix", "[ghost][fim]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    GhostContext ctx;
    ctx.documentText = "";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 0;

    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    std::string requestId = r.value().second;

    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = "bd sn"}};

    auto result = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE(result.has_value());
    REQUIRE(result->docPrefix == "");
    REQUIRE(result->docSuffix == "");
    REQUIRE(result->text == "bd sn");
}

TEST_CASE("onGhostResponse with response text shorter than overlap does not over-trim", "[ghost][fim]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    // Document: "hello " + cursor + "world"
    // docPrefix = "hello ", docSuffix = "world"
    GhostContext ctx;
    ctx.documentText = "hello world";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 6;

    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(0);
    REQUIRE(r.has_value());
    std::string requestId = r.value().second;

    // LLM generates just "w" — which matches the start of docSuffix "world"
    // The full suffix is "world" (5 chars), text is "w" (1 char).
    // overlap=1: suffixHead="w", textTail="w" → match! bestOverlap=1
    // After trimming: text is empty → should return nullopt
    GhostCompletionResponse resp;
    resp.request_id = requestId;
    resp.completions = {{.generatedText = "w"}};

    auto result = logic.onGhostResponse(requestId, resp, 0);
    REQUIRE_FALSE(result.has_value());
}

// ===========================================================================
// AI-G2: Stale response after document change with new cursor position
// ===========================================================================

TEST_CASE("onGhostResponse rejects stale response with correct revision check", "[ghost][fim][stale]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);
    logic.setTimeoutMs(5000);

    GhostContext ctx;
    ctx.documentText = "bd";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 2;

    // First request cycle at t=0
    logic.onEditorChanged(ctx, 0);
    auto r1 = logic.onTimerTick(0);
    REQUIRE(r1.has_value());
    std::string requestId1 = r1.value().second;

    // Editor changed during flight — in the real flow, onDocumentChanged()
    // would call cancelPendingRequest() before onEditorChanged. Simulate that.
    logic.cancelPendingRequest();
    ctx.documentText = "bd sn";
    ctx.character = 5;
    logic.onEditorChanged(ctx, 10);

    // Response for the old request arrives — stale (revision mismatch)
    GhostCompletionResponse oldResp;
    oldResp.request_id = requestId1;
    oldResp.completions = {{.generatedText = "old"}};
    auto result = logic.onGhostResponse(requestId1, oldResp, 50);
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(logic.hasPendingRequest()); // cleared by revision mismatch

    // Now a new request can be sent (debounce was set by onEditorChanged)
    auto r2 = logic.onTimerTick(110);
    REQUIRE(r2.has_value());
    std::string requestId2 = r2.value().second;
    REQUIRE(requestId1 != requestId2);
}

// ===========================================================================
// GhostTriggerPolicy integration — J-1 triggering rules
// ===========================================================================
// These tests verify that GhostCompletionLogic's onEditorChanged consults the
// trigger policy and respects suppression decisions.

TEST_CASE("GhostCompletionLogic: trigger policy suppresses in string literal", "[ghost][trigger]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "bd \"sd sn\"";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 6;  // inside "sd sn"

    logic.onEditorChanged(ctx, 0);

    // Even after debounce expires, no request should be produced
    auto r = logic.onTimerTick(500);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("GhostCompletionLogic: trigger policy suppresses in ChucK comment", "[ghost][trigger]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "// this is a comment";
    ctx.uri = "file:///test.ck";
    ctx.languageId = "chuck";
    ctx.line = 0;
    ctx.character = 5;

    logic.onEditorChanged(ctx, 0);

    auto r = logic.onTimerTick(500);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("GhostCompletionLogic: trigger policy suppresses mid-token", "[ghost][trigger]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "bdsn";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 2;  // between 'b' and 'd' — both word chars

    logic.onEditorChanged(ctx, 0);

    auto r = logic.onTimerTick(500);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("GhostCompletionLogic: trigger policy allows at meaningful boundary", "[ghost][trigger]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "bd ";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 3;  // after space following "bd"

    logic.onEditorChanged(ctx, 0);

    // Debounce not yet elapsed
    auto r1 = logic.onTimerTick(100);
    REQUIRE_FALSE(r1.has_value());

    // Debounce elapsed — should fire
    auto r2 = logic.onTimerTick(500);
    REQUIRE(r2.has_value());
}

TEST_CASE("GhostCompletionLogic: deterministic popup active suppresses trigger", "[ghost][trigger]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "bd ";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 3;

    logic.setDeterministicPopupActive(true);
    logic.onEditorChanged(ctx, 0);

    auto r = logic.onTimerTick(500);
    REQUIRE_FALSE(r.has_value());

    // Resume after popup dismissed
    logic.setDeterministicPopupActive(false);
    logic.onEditorChanged(ctx, 501);

    auto r2 = logic.onTimerTick(900);
    REQUIRE(r2.has_value());
}

TEST_CASE("GhostCompletionLogic: duplicate context suppressed (AI-G6)", "[ghost][trigger]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "bd ";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 3;

    // First editor change → debounce → request fires
    logic.onEditorChanged(ctx, 0);
    auto r1 = logic.onTimerTick(500);
    REQUIRE(r1.has_value());
    std::string requestId1 = r1.value().second;

    // Simulate a response so the request is cleared
    GhostCompletionResponse resp;
    resp.request_id = requestId1;
    resp.completions = {{.generatedText = "sn"}};
    logic.onGhostResponse(requestId1, resp, 500);

    // Same context again (no doc change, cursor didn't move) → duplicate
    logic.onEditorChanged(ctx, 600);
    auto r2 = logic.onTimerTick(1000);
    REQUIRE_FALSE(r2.has_value());
}

TEST_CASE("GhostCompletionLogic: non-duplicate triggers after cursor move", "[ghost][trigger]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "bd sn";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 3;  // end of "bd"

    // First request cycle
    logic.onEditorChanged(ctx, 0);
    auto r1 = logic.onTimerTick(500);
    REQUIRE(r1.has_value());
    std::string requestId1 = r1.value().second;

    // Simulate a response so the pending request is cleared
    GhostCompletionResponse resp;
    resp.request_id = requestId1;
    resp.completions = {{.generatedText = " sn"}};
    logic.onGhostResponse(requestId1, resp, 500);

    // Cursor moves to a different position → NOT a duplicate
    ctx.character = 6;  // end of "sn"
    logic.onEditorChanged(ctx, 600);
    auto r2 = logic.onTimerTick(1000);
    REQUIRE(r2.has_value());
}

TEST_CASE("GhostCompletionLogic: cancelPendingRequest clears duplicate suppression", "[ghost][trigger]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    GhostContext ctx;
    ctx.documentText = "bd ";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 3;

    // Trigger first request
    logic.onEditorChanged(ctx, 0);
    auto r1 = logic.onTimerTick(0);
    REQUIRE(r1.has_value());
    std::string requestId1 = r1.value().second;

    // Respond
    GhostCompletionResponse resp;
    resp.request_id = requestId1;
    resp.completions = {{.generatedText = "sn"}};
    logic.onGhostResponse(requestId1, resp, 0);

    // Now simulate Ctrl+Shift+Space: cancel + re-trigger
    logic.cancelPendingRequest();
    logic.onEditorChanged(ctx, 1);
    auto r2 = logic.onTimerTick(1);
    REQUIRE(r2.has_value());  // Should NOT be suppressed as duplicate
}

TEST_CASE("GhostCompletionLogic: trigger policy after deterministic popup dismiss resumes", "[ghost][trigger]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "bd ";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 3;

    // Simulate Ctrl+Space → popup active
    logic.setDeterministicPopupActive(true);
    logic.onEditorChanged(ctx, 0);
    auto r1 = logic.onTimerTick(500);
    REQUIRE_FALSE(r1.has_value());  // suppressed while popup active

    // Popup dismissed
    logic.setDeterministicPopupActive(false);
    logic.onEditorChanged(ctx, 600);
    auto r2 = logic.onTimerTick(1000);
    REQUIRE(r2.has_value());  // now allowed
}

TEST_CASE("GhostCompletionLogic: configurable trigger policy via setTriggerPolicyConfig", "[ghost][trigger]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    // Allow ghost in strings
    GhostTriggerPolicyConfig cfg;
    cfg.allowInStrings = true;
    logic.setTriggerPolicyConfig(cfg);

    REQUIRE(logic.triggerPolicyConfig().allowInStrings == true);

    GhostContext ctx;
    ctx.documentText = "bd \"";  // inside an unclosed string
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 4;  // right after the quote

    // With allowInStrings=true, the string suppression is bypassed.
    // The cursor is at end of line (a meaningful boundary), so the
    // policy should allow the trigger.
    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(500);
    REQUIRE(r.has_value());
}

TEST_CASE("GhostCompletionLogic: ChucK cursor after => triggers", "[ghost][trigger]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "SinOsc s =>";
    ctx.uri = "file:///test.ck";
    ctx.languageId = "chuck";
    ctx.line = 0;
    ctx.character = 11;  // cursor at end after '=>'

    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(500);
    REQUIRE(r.has_value());
}

TEST_CASE("GhostCompletionLogic: trigger policy after ('(' triggers", "[ghost][trigger]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "bd(";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 3;

    logic.onEditorChanged(ctx, 0);
    auto r = logic.onTimerTick(500);
    REQUIRE(r.has_value());
}

// ===========================================================================
// J-4: Selection-aware triggering — ghost completion suppressed when a
// non-empty selection is active.
// ===========================================================================

TEST_CASE("GhostCompletionLogic: non-empty selection suppresses ghost trigger (J-4)",
          "[ghost][trigger][j4]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "bd sn hh";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 3;       // cursor after "bd"
    ctx.hasSelection = true;
    ctx.selectedText = "sn hh";

    logic.onEditorChanged(ctx, 0);

    // Even after debounce expires, no request should be produced.
    auto r = logic.onTimerTick(500);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("GhostCompletionLogic: empty selection does NOT suppress trigger (J-4)",
          "[ghost][trigger][j4]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(300);

    GhostContext ctx;
    ctx.documentText = "bd sn";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 3;         // end of "bd" — meaningful boundary
    ctx.hasSelection = false;  // no selection
    ctx.selectedText = "";

    logic.onEditorChanged(ctx, 0);

    auto r = logic.onTimerTick(500);
    REQUIRE(r.has_value()); // boundary reached → trigger allowed
}

TEST_CASE("GhostCompletionLogic: selection is part of duplicate-context detection (J-4)",
          "[ghost][trigger][j4]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);  // no debounce delay

    // First request: no selection at cursor position 3.
    GhostContext ctx;
    ctx.documentText = "bd sn";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 3;
    ctx.hasSelection = false;

    logic.onEditorChanged(ctx, 0);
    auto r1 = logic.onTimerTick(1);
    REQUIRE(r1.has_value());

    // Simulate response so lastRequestedCtx_ is set with hasSelection=false.
    GhostCompletionResponse resp;
    resp.request_id = r1.value().second;
    resp.completions = {{.generatedText = " sn"}};
    logic.onGhostResponse(r1.value().second, resp, 1);

    // Now the same cursor position but WITH a selection → should NOT be
    // treated as a duplicate (selection changed).
    ctx.hasSelection = true;
    ctx.selectedText = "sn";
    logic.onEditorChanged(ctx, 2);
    // Selection is active → suppressed regardless of duplicate detection.
    auto r2 = logic.onTimerTick(3);
    REQUIRE_FALSE(r2.has_value());
}

TEST_CASE("GhostCompletionLogic: selection cleared re-enables trigger at same position (J-4)",
          "[ghost][trigger][j4]")
{
    GhostCompletionLogic logic;
    logic.setEnabled(true);
    logic.setDebounceMs(0);

    GhostContext ctx;
    ctx.documentText = "bd sn";
    ctx.uri = "file:///test.hathor";
    ctx.languageId = "hathor";
    ctx.line = 0;
    ctx.character = 3;

    // With selection → suppressed
    ctx.hasSelection = true;
    ctx.selectedText = "sn";
    logic.onEditorChanged(ctx, 0);
    auto r1 = logic.onTimerTick(1);
    REQUIRE_FALSE(r1.has_value());

    // Selection cleared → trigger allowed at boundary
    ctx.hasSelection = false;
    ctx.selectedText = "";
    logic.onEditorChanged(ctx, 2);
    auto r2 = logic.onTimerTick(3);
    REQUIRE(r2.has_value());
}