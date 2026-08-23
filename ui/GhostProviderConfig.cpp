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

#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include <nlohmann/json.hpp>

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

    // Agent 1.3: honor a persisted per-backend URL override (Settings UI).
    // Priority: override > GHOST_URL env > backend default (above).
    {
        const std::string overrideUrl = getUrlOverride(config.backend);
        if (!overrideUrl.empty())
            config.url = overrideUrl;
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

// ===========================================================================
// Agent 1.3: persisted per-backend URL overrides
// ===========================================================================
// JUCE-free persistence so the resolver stays unit-testable. Overrides live in
// an in-memory cache (lazy-loaded from the JSON file on first resolve()) and
// are consulted with priority: override > GHOST_URL env > backend default.

namespace {
struct OverrideStore
{
    std::unordered_map<hathor::lsp::LlmBackend, std::string> overrides;
    std::mutex mutex;
    std::string filePath;   ///< explicit path (tests); "" => default location
    bool  filePathSet   = false;
    bool  loaded        = false;
};

OverrideStore& overrideStore() noexcept
{
    static OverrideStore s;
    return s;
}

std::string defaultOverridesFilePath() noexcept
{
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0')
        return "";
    return std::string(home) + "/Library/Application Support/Hathor/ghost-endpoints.json";
}
} // namespace

std::string GhostProviderResolver::defaultUrlForBackend(LlmBackend backend) noexcept
{
    switch (backend)
    {
        case LlmBackend::Ollama:      return "http://localhost:11434";
        case LlmBackend::LlamaCpp:    return "http://localhost:8080";
        case LlmBackend::OpenAi:      return "https://api.openai.com";
        case LlmBackend::HuggingFace: return "https://api-inference.huggingface.co";
        case LlmBackend::Tgi:         return ""; // resolved from GHOST_TGI_URL (no default)
    }
    return "";
}

bool GhostProviderResolver::isValidGhostUrl(std::string_view url) noexcept
{
    const auto first = url.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return true; // blank => falls back to default (valid)
    const auto last = url.find_last_not_of(" \t\r\n");
    const std::string_view s = url.substr(first, last - first + 1);

    const auto sep = s.find("://");
    if (sep == std::string_view::npos)
        return false;

    std::string scheme(s.substr(0, sep));
    for (auto& c : scheme)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (scheme != "http" && scheme != "https")
        return false;

    const std::string_view rest = s.substr(sep + 3);
    const auto hostEnd = rest.find_first_of("/?#");
    const std::string_view host = rest.substr(0, hostEnd);
    if (host.empty())
        return false;
    if (host.find_first_of(" \t\r\n") != std::string_view::npos)
        return false;
    return true;
}

void GhostProviderResolver::setOverridesFilePath(const std::string& path)
{
    auto& s = overrideStore();
    const std::lock_guard<std::mutex> lk(s.mutex);
    s.filePath    = path;
    s.filePathSet = true;
    s.loaded      = false; // force a re-read from the new path on next resolve
}

std::string GhostProviderResolver::getOverridesFilePath() noexcept
{
    auto& s = overrideStore();
    const std::lock_guard<std::mutex> lk(s.mutex);
    return s.filePathSet ? s.filePath : defaultOverridesFilePath();
}

void GhostProviderResolver::ensureOverridesLoaded()
{
    auto& s = overrideStore();
    {
        const std::lock_guard<std::mutex> lk(s.mutex);
        if (s.loaded)
            return;
        s.loaded = true;
    }

    const std::string path = getOverridesFilePath();
    if (path.empty())
        return; // no persistent file (tests / no HOME)

    std::ifstream in(path);
    if (!in)
        return;
    nlohmann::json j;
    try { in >> j; } catch (...) { return; }

    const auto ov = j.value("overrides", nlohmann::json::object());
    if (!ov.is_object())
        return;

    const std::lock_guard<std::mutex> lk(s.mutex);
    for (auto& [key, val] : ov.items())
    {
        const auto backend = parseBackend(key);
        if (!backend || !val.is_string())
            continue;
        s.overrides[*backend] = val.get<std::string>();
    }
}

void GhostProviderResolver::writeOverridesFile()
{
    auto& s = overrideStore();
    std::string path;
    {
        const std::lock_guard<std::mutex> lk(s.mutex);
        path = s.filePathSet ? s.filePath : defaultOverridesFilePath();
    }
    if (path.empty())
        return;

    nlohmann::json j;
    nlohmann::json ov = nlohmann::json::object();
    {
        const std::lock_guard<std::mutex> lk(s.mutex);
        for (const auto& [backend, url] : s.overrides)
            if (!url.empty())
                ov[backendToString(backend)] = url;
    }
    j["overrides"] = std::move(ov);

    const std::filesystem::path fp(path);
    std::error_code ec;
    std::filesystem::create_directories(fp.parent_path(), ec);

    std::ofstream out(path);
    if (!out)
        return;
    out << j.dump(4);
}

void GhostProviderResolver::setUrlOverride(LlmBackend backend, std::string_view url)
{
    auto& s = overrideStore();
    {
        const std::lock_guard<std::mutex> lk(s.mutex);
        if (url.empty())
            s.overrides.erase(backend);
        else
            s.overrides[backend] = std::string(url);
    }
    writeOverridesFile();
}

std::string GhostProviderResolver::getUrlOverrideUnlocked(LlmBackend backend) noexcept
{
    auto& s = overrideStore();
    const auto  it = s.overrides.find(backend);
    if (it == s.overrides.end())
        return "";
    return it->second;
}

void GhostProviderResolver::clearUrlOverrides() noexcept
{
    auto& s = overrideStore();
    const std::lock_guard<std::mutex> lk(s.mutex);
    s.overrides.clear();
    s.loaded = false;
}

std::string GhostProviderResolver::getUrlOverride(LlmBackend backend)
{
    ensureOverridesLoaded();
    auto& s = overrideStore();
    const std::lock_guard<std::mutex> lk(s.mutex);
    return getUrlOverrideUnlocked(backend);
}

} // namespace hathor::lsp
