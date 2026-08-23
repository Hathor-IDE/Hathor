// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_agent_registry.cpp — unit tests for the JUCE-free AgentRegistry.
 *
 * Covers: default presets, JSON round-trip (user add/override persist),
 * PATH detection, and command-line tokenisation.  Compiled into
 * hathor-ui-tests (JUCE-free).
 *
 * Requirements: A2
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "AgentRegistry.hpp"

using hathor::ui::AgentRegistry;

namespace {

/// Write a temp JSON config file and return its path.
std::string writeTempConfig(const std::string& json)
{
    const std::string dir =
        std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
        "/.hathor-test-tmp";
    std::filesystem::create_directories(dir);
    const std::string path = dir + "/agent-presets.json";
    std::ofstream out(path);
    out << json;
    out.close();
    return path;
}

/// Remove temp config dir created by writeTempConfig.
void clearTempConfig()
{
    const std::string dir =
        std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
        "/.hathor-test-tmp";
    std::filesystem::remove_all(dir);
}

} // namespace

// ---------------------------------------------------------------------------
// Bundled defaults
// ---------------------------------------------------------------------------

TEST_CASE("AgentRegistry ships known presets", "[agent-registry]")
{
    const auto presets = AgentRegistry::defaultPresets();
    REQUIRE_FALSE(presets.empty());

    // Must include the five documented agents + the Custom pseudo-preset.
    auto hasId = [&](const std::string& id)
    {
        return std::any_of(presets.begin(), presets.end(),
                           [&](const AgentRegistry::Preset& p) { return p.id == id; });
    };

    REQUIRE(hasId("__custom__"));
    REQUIRE(hasId("claude-code"));
    REQUIRE(hasId("gemini"));
    REQUIRE(hasId("codex"));
    REQUIRE(hasId("cline"));
    REQUIRE(hasId("kilo"));
}

TEST_CASE("Gemini preset carries its ACP flag", "[agent-registry]")
{
    const auto presets = AgentRegistry::defaultPresets();
    const auto it = std::find_if(
        presets.begin(), presets.end(),
        [](const AgentRegistry::Preset& p) { return p.id == "gemini"; });
    REQUIRE(it != presets.end());
    REQUIRE(it->argv.size() == 2);
    REQUIRE(it->argv[0] == "gemini");
    REQUIRE(it->argv[1] == "--experimental-acp");
}

// ---------------------------------------------------------------------------
// JSON load / save (user additions + overrides)
// ---------------------------------------------------------------------------

TEST_CASE("Missing config file yields defaults", "[agent-registry]")
{
    AgentRegistry reg;
    reg.setConfigPath("/nonexistent/hathor/presets/agent-presets.json");
    REQUIRE(reg.load());
    REQUIRE_FALSE(reg.presets().empty());
    REQUIRE(reg.findById("claude-code") != nullptr);
}

TEST_CASE("User-added preset persists across save+load", "[agent-registry]")
{
    clearTempConfig();
    const auto path = writeTempConfig("[]");

    AgentRegistry reg;
    reg.setConfigPath(path);
    REQUIRE(reg.load());

    reg.addOrUpdatePreset(AgentRegistry::Preset{
        .id = "my-llama",
        .name = "Local Llama ACP",
        .argv = {"npx", "@acp/llama-server", "--port", "9090"},
        .notes = "Custom local server.",
        .isBundled = false,
    });
    REQUIRE(reg.save());

    // Reload into a fresh registry — the user entry must survive.
    AgentRegistry reg2;
    reg2.setConfigPath(path);
    REQUIRE(reg2.load());
    REQUIRE(reg2.findById("my-llama") != nullptr);
    const auto* p = reg2.findById("my-llama");
    REQUIRE(p->argv.size() == 4);
    REQUIRE(p->argv[2] == "--port");
    REQUIRE(p->argv[3] == "9090");

    clearTempConfig();
}

TEST_CASE("User override of a bundled preset is persisted", "[agent-registry]")
{
    clearTempConfig();
    const auto path = writeTempConfig("[]");

    AgentRegistry reg;
    reg.setConfigPath(path);
    REQUIRE(reg.load());

    auto* original = reg.findById("gemini");
    REQUIRE(original != nullptr);
    auto modified = *original;
    modified.argv = {"my-gemini-wrapper", "--acp"};
    reg.addOrUpdatePreset(std::move(modified));

    REQUIRE(reg.save());

    AgentRegistry reg2;
    reg2.setConfigPath(path);
    REQUIRE(reg2.load());
    const auto* overridden = reg2.findById("gemini");
    REQUIRE(overridden != nullptr);
    REQUIRE(overridden->argv[0] == "my-gemini-wrapper");
    REQUIRE(overridden->argv[1] == "--acp");

    clearTempConfig();
}

TEST_CASE("Corrupt JSON degrades to defaults", "[agent-registry]")
{
    clearTempConfig();
    const auto path = writeTempConfig("{not valid json");

    AgentRegistry reg;
    reg.setConfigPath(path);
    REQUIRE_FALSE(reg.load()); // signals failure
    // Defaults are still present.
    REQUIRE(reg.findById("claude-code") != nullptr);
    REQUIRE(reg.findById("kilo") != nullptr);

    clearTempConfig();
}

// ---------------------------------------------------------------------------
// PATH detection
// ---------------------------------------------------------------------------

TEST_CASE("findOnPath locates a known executable", "[agent-registry]")
{
    // `ls` and `cat` are universally available on POSIX dev machines.
    const auto found = AgentRegistry::findOnPath("ls");
    REQUIRE(found.has_value());
    REQUIRE_FALSE(found->empty());
#if defined(__APPLE__) || defined(__linux__)
    // Must be an absolute path.
    REQUIRE(found->at(0) == '/');
#endif
}

TEST_CASE("findOnPath returns nullopt for missing binary", "[agent-registry]")
{
    const auto found =
        AgentRegistry::findOnPath("this-agent-name-does-not-exist-zzz");
    REQUIRE_FALSE(found.has_value());
}

TEST_CASE("findOnPath handles empty input", "[agent-registry]")
{
    const auto found = AgentRegistry::findOnPath("");
    REQUIRE_FALSE(found.has_value());
}

// ---------------------------------------------------------------------------
// Command-line tokeniser
// ---------------------------------------------------------------------------

TEST_CASE("splitCommandLine handles simple args", "[agent-registry]")
{
    const auto toks = AgentRegistry::splitCommandLine("gemini --experimental-acp");
    REQUIRE(toks.size() == 2);
    REQUIRE(toks[0] == "gemini");
    REQUIRE(toks[1] == "--experimental-acp");
}

TEST_CASE("splitCommandLine handles double quotes", "[agent-registry]")
{
    const auto toks = AgentRegistry::splitCommandLine("echo \"hello world\"");
    REQUIRE(toks.size() == 2);
    REQUIRE(toks[1] == "hello world");
}

TEST_CASE("splitCommandLine handles single quotes", "[agent-registry]")
{
    const auto toks = AgentRegistry::splitCommandLine("echo 'hello world'");
    REQUIRE(toks.size() == 2);
    REQUIRE(toks[1] == "hello world");
}

TEST_CASE("splitCommandLine handles escape in double quotes", "[agent-registry]")
{
    const auto toks =
        AgentRegistry::splitCommandLine("echo \"she said \\\"hi\\\"");
    REQUIRE(toks.size() == 2);
    REQUIRE(toks[1] == "she said \"hi\"");
}

TEST_CASE("splitCommandLine handles no args", "[agent-registry]")
{
    const auto toks = AgentRegistry::splitCommandLine("claude-code-acp");
    REQUIRE(toks.size() == 1);
    REQUIRE(toks[0] == "claude-code-acp");
}

TEST_CASE("splitCommandLine handles empty input", "[agent-registry]")
{
    const auto toks = AgentRegistry::splitCommandLine("");
    REQUIRE(toks.empty());
}

TEST_CASE("splitCommandLine handles trailing whitespace", "[agent-registry]")
{
    const auto toks =
        AgentRegistry::splitCommandLine("gemini --experimental-acp   ");
    REQUIRE(toks.size() == 2);
    REQUIRE(toks[0] == "gemini");
    REQUIRE(toks[1] == "--experimental-acp");
}
