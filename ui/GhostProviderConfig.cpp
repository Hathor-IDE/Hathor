// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * GhostProviderConfig.cpp — implementation of GhostProviderResolver.
 *
 * Reads LLM provider settings from environment variables at request time
 * so that credentials never persist in editor state, project files, or
 * MCP context payloads.
 *
 * Requirement references: AI-4
 */

#include "GhostProviderConfig.hpp"

#include <cstdlib>
#include <cstring>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// Environment variable access
// ---------------------------------------------------------------------------

std::string GhostProviderResolver::getenv_(const char* name)
{
    const char* val = std::getenv(name);
    if (val == nullptr || val[0] == '\0')
        return "";
    return std::string(val);
}

bool GhostProviderResolver::parseBool(std::string_view val) noexcept
{
    if (val == "1" || val == "true" || val == "TRUE" || val == "True")
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// Backend parsing
// ---------------------------------------------------------------------------

std::optional<LlmBackend> GhostProviderResolver::parseBackend(std::string_view name)
{
    if (name == "huggingface")  return LlmBackend::HuggingFace;
    if (name == "huggingfacex") return LlmBackend::HuggingFace;
    if (name == "llamacpp")     return LlmBackend::LlamaCpp;
    if (name == "llama_cpp")    return LlmBackend::LlamaCpp;
    if (name == "ollama")       return LlmBackend::Ollama;
    if (name == "openai")       return LlmBackend::OpenAi;
    if (name == "tgi")          return LlmBackend::Tgi;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// resolve — main entry point
// ---------------------------------------------------------------------------

std::optional<GhostProviderConfig> GhostProviderResolver::resolve()
{
    const std::string backendName = getenv_("GHOST_BACKEND");
    const std::string model = getenv_("GHOST_MODEL");

    if (backendName.empty() && model.empty())
        return std::nullopt;

    if (backendName.empty())
        return resolveForBackend("huggingface", model, "");

    return resolveForBackend(backendName, model, "");
}

// ---------------------------------------------------------------------------
// resolveForBackend — resolve with explicit overrides
// ---------------------------------------------------------------------------

std::optional<GhostProviderConfig> GhostProviderResolver::resolveForBackend(
    std::string_view backendName,
    std::string_view modelOverride,
    std::string_view tokenOverride)
{
    GhostProviderConfig config;

    // Parse backend type
    std::string_view bn = backendName.empty() ? "huggingface" : backendName;
    auto backend = parseBackend(bn);
    if (!backend.has_value())
        return std::nullopt;
    config.backend = backend.value();

    // Model
    config.model = modelOverride.empty()
        ? getenv_("GHOST_MODEL")
        : std::string(modelOverride);

    // API token
    if (!tokenOverride.empty())
    {
        config.apiToken = std::string(tokenOverride);
    }
    else
    {
        switch (config.backend)
        {
            case LlmBackend::HuggingFace:
                config.apiToken = getenv_("HF_API_TOKEN");
                break;
            case LlmBackend::OpenAi:
                config.apiToken = getenv_("OPENAI_API_KEY");
                break;
            case LlmBackend::Tgi:
            case LlmBackend::LlamaCpp:
            case LlmBackend::Ollama:
                // These backends may not need a token
                config.apiToken = getenv_("GHOST_API_TOKEN");
                if (config.apiToken.empty())
                    config.apiToken = getenv_("OPENAI_API_KEY");
                break;
        }
    }

    // URL (for non-default backends)
    config.url = getenv_("GHOST_URL");
    if (config.url.empty())
    {
        switch (config.backend)
        {
            case LlmBackend::Ollama:
                config.url = "http://localhost:11434";
                break;
            case LlmBackend::LlamaCpp:
                config.url = "http://localhost:8080";
                break;
            case LlmBackend::Tgi:
                config.url = getenv_("GHOST_TGI_URL");
                break;
            case LlmBackend::OpenAi:
                config.url = getenv_("OPENAI_API_BASE");
                if (config.url.empty())
                    config.url = "https://api.openai.com";
                break;
            case LlmBackend::HuggingFace:
                config.url = "https://api-inference.huggingface.co";
                break;
        }
    }

    // Context window
    std::string cwStr = getenv_("GHOST_CONTEXT_WINDOW");
    if (!cwStr.empty())
    {
        try
        {
            config.contextWindow = std::stoi(cwStr);
            if (config.contextWindow < 256)
                config.contextWindow = 256;
        }
        catch (...)
        {
            config.contextWindow = 2048;
        }
    }

    // TLS skip verify (dev only, insecure)
    config.tlsSkipVerify = parseBool(getenv_("GHOST_TLS_SKIP_VERIFY"));

    // Tokenizer config
    config.tokenizerConfig = getenv_("GHOST_TOKENIZER_CONFIG");
    if (config.tokenizerConfig.empty())
        config.tokenizerConfig = "default";

    // Validate: model is required for all backends
    if (config.model.empty())
    {
        return std::nullopt;
    }

    return config;
}

// ---------------------------------------------------------------------------
// isEnabled — global feature flag
// ---------------------------------------------------------------------------

bool GhostProviderResolver::isEnabled()
{
    std::string enabled = getenv_("GHOST_ENABLED");
    if (enabled.empty())
        return false; // disabled by default — requires explicit GHOST_ENABLED=true
    return parseBool(enabled);
}

} // namespace hathor::lsp
