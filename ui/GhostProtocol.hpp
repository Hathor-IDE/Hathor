// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * GhostProtocol.hpp — JUCE-free protocol types for llm-ls integration.
 *
 * llm-ls (v0.5.3) is a Rust-based LSP server for LLM-powered code completion.
 * It implements the custom LSP method `llm-ls/getCompletions` (NOT the
 * standard `textDocument/completion`). The request sends FIM (Fill-in-the-Middle)
 * context: prefix, suffix, and middle marker. The response returns an array
 * of completion strings.
 *
 * These structs mirror the subset of llm-ls message types that Hathor's
 * ghost-text integration needs. All types are plain C++ with no JUCE
 * dependencies, so the GhostCompletionLogic that consumes them is fully
 * unit-testable in hathor-ui-tests.
 *
 * Architecture (Hathor ghost text):
 *
 *   HathorTab → GhostLlmClient (JUCE, process mgmt) → LspMessageFramer
 *                                      → GhostJsonRpc (JUCE-free serialization)
 *                                      → GhostCompletionLogic (JUCE-free, tested)
 *                                      → GhostTextOverlay (JUCE, rendering)
 *
 * Requirement references: AI-4, AI-3 decision #18
 */

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// llm-ls backend type
// ---------------------------------------------------------------------------

/**
 * The LLM backend used by llm-ls.
 * Maps to the Rust `Backend` enum in llm-ls.
 */
enum class LlmBackend {
    HuggingFace,  ///< api-inference.huggingface.co
    LlamaCpp,     ///< local llama.cpp server
    Ollama,        ///< local Ollama server
    OpenAi,       ///< OpenAI-compatible API
    Tgi,          ///< Text Generation Inference
};

/**
 * Serialize a backend to its llm-ls string representation.
 */
inline std::string backendToString(LlmBackend b)
{
    switch (b)
    {
        case LlmBackend::HuggingFace: return "HuggingFace";
        case LlmBackend::LlamaCpp:    return "LlamaCpp";
        case LlmBackend::Ollama:      return "Ollama";
        case LlmBackend::OpenAi:      return "OpenAi";
        case LlmBackend::Tgi:         return "Tgi";
    }
    return "HuggingFace";
}

// ---------------------------------------------------------------------------
// llm-ls completion request parameters
// ---------------------------------------------------------------------------

/**
 * FIM (Fill-in-the-Middle) context for llm-ls.
 *
 * When fim.enabled is true, the LLM receives:
 *   - prefix:  text before the cursor (the LLM should complete from here)
 *   - suffix:  text after the cursor (the LLM should preserve this)
 *   - middle:  the FIM middle marker token (model-specific, usually "<fim-middle>")
 *
 * The LLM generates text that bridges prefix → suffix.
 */
struct FimParams {
    bool        enabled = false;
    std::string prefix;   ///< text before cursor (reversed, line-by-line in llm-ls)
    std::string suffix;   ///< text after cursor (forward, line-by-line in llm-ls)
    std::string middle;   ///< FIM middle marker (left empty; llm-ls fills from tokenizer)
};

/**
 * Backend configuration for the llm-ls request.
 *
 * Each request carries its own backend config so that:
 *   - Credentials are provided per-request (never stored in editor text)
 *   - The model can be selected at invocation time
 *   - The context window size is explicit
 */
struct BackendConfig {
    LlmBackend  backend     = LlmBackend::HuggingFace;
    std::string url;         ///< API endpoint URL (empty for HuggingFace default)
    std::string model;       ///< model identifier (e.g. "bigcode/starcoder")
};

/**
 * Parameters for a single `llm-ls/getCompletions` request.
 *
 * These map directly to the serde fields llm-ls v0.5.3 expects.
 * Field names use camelCase to match the Rust struct serialization.
 */
struct GhostCompletionRequest {
    // --- Document identity ---
    std::string uri;           ///< file:// URI or synthetic URI
    std::string languageId;    ///< "hathor" or "chuck"

    // --- Cursor position ---
    int line      = 0;         ///< 0-based line number
    int character = 0;         ///< 0-based character offset on the line

    // --- Text context ---
    std::string textDocument;  ///< full document text (for context-aware completion)

    // --- FIM context ---
    FimParams   fim;

    // --- Backend ---
    BackendConfig backend;

    // --- Request-level credentials (env-var sourced, never persisted) ---
    std::string apiToken;      ///< bearer token (from HF_API_TOKEN / OPENAI_API_KEY)

    // --- Generation parameters ---
    std::string tokenizerConfig;  ///< tokenizer configuration path
    int         contextWindow    = 2048;  ///< max context window
    bool        tlsSkipVerify    = false;  ///< skip TLS verification (dev only)

    // --- Request body for custom backends ---
    // If non-empty, llm-ls sends this as the raw body instead of building
    // its own request from the standard fields.
    nlohmann::json requestBody;

    // --- Misc ---
    bool disableUrlPathCompletion = false;  ///< don't append URI path as suffix
    std::vector<std::string> tokensToClear;  ///< tokens to clear before completion

    /** Serialize this request into the nlohmann::json params for llm-ls. */
    nlohmann::json toJson() const;
};

// ---------------------------------------------------------------------------
// llm-ls completion response
// ---------------------------------------------------------------------------

/**
 * A single completion result from llm-ls.
 *
 * llm-ls returns completions as an array of objects, each with a
 * "generated_text" field containing the LLM's output text.
 */
struct GhostCompletion {
    std::string generatedText;
};

/**
 * The full response from a `llm-ls/getCompletions` request.
 *
 * llm-ls responses include a `request_id` (UUID) that we use to correlate
 * the response with our pending request. Since llm-ls does not support
 * `$/cancelRequest`, we track staleness via revision counters in
 * GhostCompletionLogic.
 */
struct GhostCompletionResponse {
    std::string request_id;
    std::vector<GhostCompletion> completions;
};

/**
 * Parse an llm-ls completion response JSON into a typed struct.
 * Returns std::nullopt if parsing fails.
 */
std::optional<GhostCompletionResponse> parseGhostCompletionResponse(
    const nlohmann::json& j);

// ---------------------------------------------------------------------------
// llm-ls notification types (accept/reject)
// ---------------------------------------------------------------------------

/**
 * Parameters for the `llm-ls/acceptCompletion` notification.
 * Sent when the user accepts a ghost completion suggestion.
 */
struct AcceptCompletionParams {
    std::string requestId;     ///< the request_id from the completion response
    std::string uri;
    int         line      = 0;
    int         character = 0;
};

/**
 * Parameters for the `llm-ls/rejectCompletion` notification.
 * Sent when the user dismisses or modifies a ghost completion suggestion.
 */
struct RejectCompletionParams {
    std::string requestId;     ///< the request_id from the completion response
    std::string uri;
};

// ---------------------------------------------------------------------------
// Ghost completion result — consumed by the JUCE overlay
// ---------------------------------------------------------------------------

/**
 * The final ghost text to display, after processing by GhostCompletionLogic.
 *
 * The `text` field is the completion text to show as ghost/inlay text
 * starting from the cursor position. If `displayText` differs from `insertText`,
 * it should be shown to the user while `insertText` is what gets inserted.
 */
struct GhostResult {
    std::string text;           ///< text to display as ghost text
    std::string displayText;    ///< text to show in the overlay (may include hints)
    std::string insertText;     ///< text to insert on accept
    std::string requestId;      ///< for accept/reject notifications
    bool        isEmpty() const noexcept { return text.empty(); }
};

} // namespace hathor::lsp