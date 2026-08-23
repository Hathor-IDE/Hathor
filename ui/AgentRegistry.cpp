// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * AgentRegistry.cpp — implementation of the JUCE-free known-agent registry.
 *
 * Requirements: A2
 */

#include "AgentRegistry.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Bundled defaults
// ---------------------------------------------------------------------------

std::vector<AgentRegistry::Preset> AgentRegistry::defaultPresets()
{
    return {
        {
            /*id=*/"__custom__",
            /*name=*/"Custom…",
            /*argv=*/{},
            /*notes=*/"Browse to an ACP-compatible agent binary and supply any args manually.",
            /*isBundled=*/true,
        },
        {
            /*id=*/"claude-code",
            /*name=*/"Claude Code",
            /*argv=*/{"claude-code-acp"},
            /*notes=*/"Claude Code ACP server. Install via `npm i -g @anthropic-ai/claude-code`.",
            /*isBundled=*/true,
        },
        {
            /*id=*/"gemini",
            /*name=*/"Gemini CLI",
            /*argv=*/{"gemini", "--experimental-acp"},
            /*notes=*/"Google Gemini CLI in ACP mode. Requires `--experimental-acp`.",
            /*isBundled=*/true,
        },
        {
            /*id=*/"codex",
            /*name=*/"Codex",
            /*argv=*/{"codex"},
            /*notes=*/"OpenAI Codex CLI. Enable ACP server mode per your version's docs.",
            /*isBundled=*/true,
        },
        {
            /*id=*/"cline",
            /*name=*/"Cline",
            /*argv=*/{"cline"},
            /*notes=*/"Cline ACP server. See Cline docs for the ACP launch flag.",
            /*isBundled=*/true,
        },
        {
            /*id=*/"kilo",
            /*name=*/"Kilo",
            /*argv=*/{"kilo"},
            /*notes=*/"Kilo agent. Launch with ACP server mode enabled.",
            /*isBundled=*/true,
        },
    };
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

std::string AgentRegistry::defaultConfigDir()
{
    if (const char* home = std::getenv("HOME"))
    {
        std::filesystem::path base(home);
#if defined(__APPLE__)
        return (base / "Library/Application Support/Hathor").string();
#elif defined(__linux__)
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"))
            if (xdg[0] != '\0')
                return (std::filesystem::path(xdg) / "Hathor").string();
        return (base / ".config/Hathor").string();
#else
        return (base / "Hathor").string();
#endif
    }
    return std::string("Hathor");
}

void AgentRegistry::mergeFromJson(const nlohmann::json& j)
{
    if (!j.is_array())
        return;

    for (const auto& entry : j)
    {
        if (!entry.is_object())
            continue;

        Preset p;
        if (entry.contains("id") && entry["id"].is_string())
            p.id = entry["id"].get<std::string>();
        if (entry.contains("name") && entry["name"].is_string())
            p.name = entry["name"].get<std::string>();
        if (entry.contains("argv") && entry["argv"].is_array())
        {
            for (const auto& a : entry["argv"])
            {
                if (a.is_string())
                    p.argv.push_back(a.get<std::string>());
            }
        }
        if (entry.contains("notes") && entry["notes"].is_string())
            p.notes = entry["notes"].get<std::string>();
        if (entry.contains("isBundled"))
            p.isBundled = entry["isBundled"].get<bool>();

        if (p.id.empty())
            continue;

        // Override matching id, else append.
        bool replaced = false;
        for (auto& existing : presets_)
        {
            if (existing.id == p.id)
            {
                existing = std::move(p);
                replaced = true;
                break;
            }
        }
        if (!replaced)
            presets_.push_back(std::move(p));
    }
}

bool AgentRegistry::load()
{
    presets_ = defaultPresets();

    if (configPath_.empty())
        configPath_ = (std::filesystem::path(defaultConfigDir()) / "agent-presets.json").string();

    std::error_code ec;
    if (!std::filesystem::exists(configPath_, ec))
        return true; // fresh install — defaults are enough.

    std::ifstream in(configPath_);
    if (!in)
        return false;

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(in);
    }
    catch (const nlohmann::json::exception&)
    {
        return false; // corrupt file — keep defaults, signal failure.
    }

    mergeFromJson(j);
    return true;
}

bool AgentRegistry::save() const
{
    const std::string path = configPath_.empty()
        ? (std::filesystem::path(defaultConfigDir()) / "agent-presets.json").string()
        : configPath_;

    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (!dir.empty())
    {
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return false;
    }

    nlohmann::json j = nlohmann::json::array();
    for (const auto& p : presets_)
    {
        // Don't persist the synthetic "__custom__" entry — it's a UI affordance.
        if (p.id == "__custom__")
            continue;
        nlohmann::json entry = nlohmann::json::object();
        entry["id"]          = p.id;
        entry["name"]        = p.name;
        entry["argv"]        = p.argv;
        entry["notes"]       = p.notes;
        entry["isBundled"]   = p.isBundled;
        j.push_back(std::move(entry));
    }

    std::ofstream out(path);
    if (!out)
        return false;
    out << j.dump(2) << '\n';
    return static_cast<bool>(out);
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

const AgentRegistry::Preset*
AgentRegistry::findById(const std::string& id) const noexcept
{
    for (const auto& p : presets_)
        if (p.id == id)
            return &p;
    return nullptr;
}

const AgentRegistry::Preset*
AgentRegistry::findByExeName(const std::string& exeName) const noexcept
{
    for (const auto& p : presets_)
    {
        if (!p.argv.empty() && p.argv[0] == exeName)
            return &p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// PATH detection
// ---------------------------------------------------------------------------

std::optional<std::string> AgentRegistry::findOnPath(const std::string& exeName)
{
    if (exeName.empty())
        return std::nullopt;

    // Absolute or relative path containing a slash — check directly.
    if (exeName.find('/') != std::string::npos)
    {
        struct stat st;
        if (::stat(exeName.c_str(), &st) == 0
            && S_ISREG(st.st_mode)
            && ::access(exeName.c_str(), X_OK) == 0)
            return std::filesystem::absolute(exeName).string();
        return std::nullopt;
    }

    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr || pathEnv[0] == '\0')
        return std::nullopt;

    const std::filesystem::path exePath = exeName;
    std::string pathCopy(pathEnv);
    std::stringstream ss(pathCopy);
    std::string dir;

    while (std::getline(ss, dir, ':'))
    {
        if (dir.empty())
            continue;
        const std::filesystem::path candidate = std::filesystem::path(dir) / exePath;
        struct stat st;
        if (::stat(candidate.c_str(), &st) == 0
            && S_ISREG(st.st_mode)
            && ::access(candidate.c_str(), X_OK) == 0)
            return candidate.string();
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Command-line tokeniser
// ---------------------------------------------------------------------------

std::vector<std::string> AgentRegistry::splitCommandLine(const std::string& commandLine)
{
    std::vector<std::string> tokens;
    std::string current;
    enum class Mode { Between, Bare, Single, Double } mode = Mode::Between;

    auto flush = [&]()
    {
        tokens.push_back(std::exchange(current, std::string()));
    };

    for (std::size_t i = 0; i < commandLine.size(); ++i)
    {
        const char c = commandLine[i];

        switch (mode)
        {
            case Mode::Between:
                if (std::isspace(static_cast<unsigned char>(c)))
                    continue;
                if (c == '\'')
                    mode = Mode::Single;
                else if (c == '"')
                    mode = Mode::Double;
                else
                {
                    mode = Mode::Bare;
                    current.push_back(c);
                }
                break;

            case Mode::Bare:
                if (std::isspace(static_cast<unsigned char>(c)))
                {
                    flush();
                    mode = Mode::Between;
                }
                else
                    current.push_back(c);
                break;

            case Mode::Single:
                if (c == '\'')
                {
                    flush();
                    mode = Mode::Between;
                }
                else
                    current.push_back(c);
                break;

            case Mode::Double:
                if (c == '"')
                {
                    flush();
                    mode = Mode::Between;
                }
                else if (c == '\\' && i + 1 < commandLine.size())
                {
                    current.push_back(commandLine[++i]);
                }
                else
                    current.push_back(c);
                break;
        }
    }

    // Flush a trailing token (covers bare, single, and double that end the
    // input without an explicit delimiter).
    if (mode != Mode::Between)
        flush();

    return tokens;
}

// ---------------------------------------------------------------------------
// User management
// ---------------------------------------------------------------------------

void AgentRegistry::addOrUpdatePreset(Preset preset)
{
    if (preset.id.empty() || preset.id == "__custom__")
        return;

    for (auto& existing : presets_)
    {
        if (existing.id == preset.id)
        {
            existing = std::move(preset);
            return;
        }
    }
    preset.isBundled = false;
    presets_.push_back(std::move(preset));
}

void AgentRegistry::removePreset(const std::string& id)
{
    if (id == "__custom__" || id.empty())
        return;

    const auto it = std::find_if(presets_.begin(), presets_.end(),
                                 [&](const Preset& p)
                                 {
                                     // Only remove user presets; keep bundled defaults.
                                     return p.id == id && !p.isBundled;
                                 });
    if (it != presets_.end())
        presets_.erase(it);
}

} // namespace hathor::ui
