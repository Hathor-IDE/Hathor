// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * AgentRegistry.hpp — JUCE-free known-agent registry (Agent 1.2, audit A2).
 *
 * Resolves the "no known-agent registry" gap: ships a table of well-known
 * ACP-compatible agent CLIs (Claude Code, Gemini CLI, Codex, Cline, Kilo) plus
 * a user-extensible JSON store in the app-data folder.
 *
 * This type is intentionally JUCE-free (like ChatSessionState / WorkspaceSession)
 * so it can be compiled into hathor-ui-tests and unit-tested without a JUCE
 * message loop.  It depends only on the C++ standard library and nlohmann::json.
 *
 * A "preset" describes how to launch an agent:
 *   - id      : stable identifier used for persistence / UI selection
 *   - name    : human-readable label shown in the picker
 *   - argv    : command template; argv[0] is the executable name (resolved via
 *               PATH or an absolute Browse path), argv[1..] are default args
 *   - notes   : short guidance surfaced in the UI tooltip
 *
 * Bundled defaults are compiled in.  User additions and overrides are merged
 * on top from a JSON file (agent-presets.json) in the config directory.  The
 * "Custom" pseudo-preset (id="__custom__") lets the user hand-pick any binary
 * via Browse and supply arbitrary args.
 *
 * Requirements: A2
 */

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hathor::ui {

class AgentRegistry
{
public:
    /** A single known-agent entry. */
    struct Preset
    {
        std::string id;                 ///< stable key
        std::string name;               ///< display label
        std::vector<std::string> argv;  ///< {exe-name, arg1, arg2, ...}
        std::string notes;              ///< tooltip / guidance
        bool isBundled = false;         ///< true = compiled-in default
    };

    AgentRegistry() = default;
    ~AgentRegistry() = default;

    AgentRegistry(const AgentRegistry&)            = default;
    AgentRegistry& operator=(const AgentRegistry&) = default;

    // -----------------------------------------------------------------------
    // Bundled defaults (always available, compiled in).
    // Includes the "__custom__" pseudo-preset so the picker always offers a
    // Browse fallback.
    // -----------------------------------------------------------------------
    static std::vector<Preset> defaultPresets();

    // -----------------------------------------------------------------------
    // Config file management.
    //
    // The config file is a JSON array of Preset objects (user additions and
    // overrides).  On load(), bundled defaults are seeded first, then user
    // entries are merged (same id → override, new id → append).
    //
    // @param configPath  Full path to agent-presets.json.  If empty, the
    //                    platform-default app-data directory is used.
    // -----------------------------------------------------------------------
    void setConfigPath(std::string configPath) { configPath_ = std::move(configPath); }
    const std::string& configPath() const noexcept { return configPath_; }

    /**
     * Load presets: seed from bundled defaults, then merge user entries from
     * the JSON config file (if present and valid).  Safe to call on a fresh
     * install — missing/corrupt files simply leave the defaults intact.
     *
     * @return true if the config file was loaded successfully (or absent);
     *         false only if it existed but failed to parse.
     */
    bool load();

    /**
     * Persist the current preset list (all entries — bundled + user — are
     * written so that customizations survive a restart).  Creates the parent
     * directory if needed.
     *
     * @return true on success.
     */
    bool save() const;

    // -----------------------------------------------------------------------
    // Accessors (valid after load()).
    // -----------------------------------------------------------------------
    const std::vector<Preset>& presets() const noexcept { return presets_; }

    /// Find a preset by stable id (e.g. "claude-code", "gemini", "__custom__").
    const Preset* findById(const std::string& id) const noexcept;

    /// Find a preset whose argv[0] matches @p exeName (e.g. "gemini").
    const Preset* findByExeName(const std::string& exeName) const noexcept;

    // -----------------------------------------------------------------------
    // PATH detection.
    //
    // Search $PATH for an executable named @p exeName.  Returns the resolved
    // absolute path if found and executable, or nullopt otherwise.
    // Mirrors `which`/`command -v` semantics (POSIX).
    // -----------------------------------------------------------------------
    static std::optional<std::string> findOnPath(const std::string& exeName);

    // -----------------------------------------------------------------------
    // Command-line splitting.
    //
    // Tokenise a command-line string into argv tokens, honouring single and
    // double quotes and backslash escapes inside double quotes.  Empty input
    // yields an empty vector.
    //   "gemini --experimental-acp"  → { "gemini", "--experimental-acp" }
    //   "echo \"hello world\""        → { "echo", "hello world" }
    // -----------------------------------------------------------------------
    static std::vector<std::string> splitCommandLine(const std::string& commandLine);

    // -----------------------------------------------------------------------
    // User management.
    // -----------------------------------------------------------------------

    /// Add or replace a preset (id must be non-empty and not "__custom__").
    /// User-added presets (isBundled=false) may also override bundled ones.
    void addOrUpdatePreset(Preset preset);

    /// Remove a user preset by id.  Bundled defaults cannot be removed
    /// (they are reseeded on every load()).
    void removePreset(const std::string& id);

private:
    std::string configPath_;
    std::vector<Preset> presets_;

    /// Resolve the platform-default config directory.
    /// macOS:  $HOME/Library/Application Support/Hathor
    /// Linux:  $XDG_CONFIG_HOME/Hathor  (fall back to $HOME/.config/Hathor)
    static std::string defaultConfigDir();

    /// (Re)build the in-memory list from bundled defaults + a parsed JSON array.
    void mergeFromJson(const nlohmann::json& j);
};

} // namespace hathor::ui
