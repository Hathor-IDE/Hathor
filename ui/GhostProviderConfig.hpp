// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * GhostProviderConfig.hpp — llm-ls backend provider configuration.
 *
 * Reads LLM provider settings from environment variables so that
 * credentials never appear in editor text, project source, or MCP payloads.
 *
 * The config is JUCE-free and fully testable in hathor-ui-tests.
 *
 * Requirement references: AI-4
 */

#include "GhostProtocol.hpp"

#include <optional>
#include <string>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// GhostProviderConfig — resolved provider settings for a single completion
// ---------------------------------------------------------------------------

/**
 * Resolved LLM provider configuration.
 *
 * This is the concrete config that gets embedded in a GhostCompletionRequest.
 * It is assembled from environment variables + defaults at request time.
 */
struct GhostProviderConfig : BackendConfig {
    std::string apiToken;        ///< bearer token (never persisted)
    std::string tokenizerConfig; ///< tokenizer config path
    int         contextWindow  = 2048;
    bool        tlsSkipVerify  = false;

    /** True if all required fields are populated for the selected backend. */
    bool valid() const noexcept
    {
        return !model.empty() && (!apiToken.empty() || backend != LlmBackend::HuggingFace);
    }
};

// ---------------------------------------------------------------------------
// GhostProviderResolver — resolves config from environment
// ---------------------------------------------------------------------------

/**
 * Resolves the LLM provider configuration from environment variables.
 *
 * Supported env vars:
 *   - GHOST_BACKEND: "huggingface" | "ollama" | "openai" | "tgi" | "llamacpp"
 *                    (default: "huggingface")
 *   - GHOST_MODEL:   model identifier (required for backends that need it)
 *   - HF_API_TOKEN:  HuggingFace API token (for HuggingFace backend)
 *   - OPENAI_API_KEY: OpenAI API key (for OpenAi backend)
 *   - GHOST_URL:     custom URL for Tgi/LlamaCpp/Ollama backends
 *   - GHOST_CONTEXT_WINDOW: max context window (default: 2048)
 *   - GHOST_TLS_SKIP_VERIFY: "1" or "true" to skip TLS verification (dev only)
 *
 * All methods are JUCE-free and safe to call from any thread (they read
 * environment variables which are process-global read-only at runtime).
 */
class GhostProviderResolver
{
public:
    /**
     * Resolve the current provider configuration from environment variables.
     *
     * @return Resolved config, or std::nullopt if no valid backend is configured.
     */
    static std::optional<GhostProviderConfig> resolve();

    /**
     * Check if ghost text is enabled at all.
     * Returns false if GHOST_ENABLED is unset or "0".
     */
    static bool isEnabled();

    /**
     * Resolve config for a specific backend override.
     * Used when the caller wants to force a particular backend.
     */
    static std::optional<GhostProviderConfig> resolveForBackend(
        std::string_view backendName,
        std::string_view modelOverride,
        std::string_view tokenOverride);

private:
    /**
     * Parse a backend name string to LlmBackend enum.
     * Returns std::nullopt for unknown backends.
     */
    static std::optional<LlmBackend> parseBackend(std::string_view name);

    /**
     * Read an environment variable, returning empty string if unset.
     */
    static std::string getenv_(const char* name);

    /**
     * Parse "true"/"1"/etc. to bool.
     */
    static bool parseBool(std::string_view val) noexcept;
};

} // namespace hathor::lsp
