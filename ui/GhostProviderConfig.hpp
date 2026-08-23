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
#include <string_view>

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

    // -----------------------------------------------------------------------
    // Agent 1.3: persisted per-backend URL overrides (Settings UI)
    // -----------------------------------------------------------------------
    // Consulted in resolveForBackend() with the priority:
    //   1. persisted per-backend override (this API)   <- Settings UI / file
    //   2. GHOST_URL env var                           <- existing behavior
    //   3. backend-specific default                    <- existing behavior
    // A blank override (the default) falls back to (2)/(3). An empty string
    // passed to setUrlOverride() clears the override for that backend.
    //
    // Overrides are persisted to a JUCE-free JSON file so that a changed
    // endpoint survives restart and takes effect on the next completion
    // request without re-opening Settings (the file is lazily loaded on
    // first resolve()).

    /** Store/overwrite the URL override for @p backend. Pass "" to clear. */
    static void setUrlOverride(LlmBackend backend, std::string_view url);

    /** Drop the in-memory override cache. The persistence file is untouched. */
    static void clearUrlOverrides() noexcept;

    /** Return the current override for @p backend, or "" if none. Triggers the
        one-time lazy load from the persistence file on first call. */
    static std::string getUrlOverride(LlmBackend backend);

    /** Default (hardcoded) endpoint URL for a backend — used as a Settings
        hint and as the fallback in resolveForBackend(). */
    static std::string defaultUrlForBackend(LlmBackend backend) noexcept;

    /** Validate a URL entered in the Settings UI. Blank is accepted (means
        "use default"). A non-blank value must parse as http(s)://<host>...   */
    static bool isValidGhostUrl(std::string_view url) noexcept;

    /** Tests only: redirect the persistence file. Pass "" to disable file
        I/O (in-memory cache only) — keeps unit tests hermetic.             */
    static void setOverridesFilePath(const std::string& path);
    static std::string getOverridesFilePath() noexcept;

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

    // Agent 1.3: override-cache accessors (defined in .cpp).
    static std::string getUrlOverrideUnlocked(LlmBackend backend) noexcept;
    static void ensureOverridesLoaded();
    static void writeOverridesFile();
};

} // namespace hathor::lsp
