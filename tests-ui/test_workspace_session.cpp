// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_workspace_session.cpp — unit tests for the JUCE-free WorkspaceSession
 * serialization model (Req 20.7).
 *
 * Runs headless (no JUCE audio device) as part of hathor-ui-tests.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <optional>
#include <string>

#include "WorkspaceSession.hpp"

using hathor::ui::WorkspaceSession;
using hathor::ui::TabState;
using hathor::ui::FrontMatter;

// ===========================================================================
// helpers
// ===========================================================================

static TabState makeFileTab(const std::string& path, int slot = 0,
                            bool isChuck = false, int64_t cursor = 0,
                            const std::string& content = "")
{
    TabState ts;
    ts.filePath = path;
    ts.slotIndex = slot;
    ts.isChuckTab = isChuck;
    ts.cursorOffset = cursor;
    ts.content = content;
    return ts;
}

static TabState makeUntitledTab(const std::string& content, int slot = 0)
{
    TabState ts;
    ts.filePath = "";
    ts.slotIndex = slot;
    ts.content = content;
    return ts;
}

static TabState makeTabWithFrontMatter(const std::string& path,
                                        const std::string& slotName,
                                        double bpm)
{
    TabState ts = makeFileTab(path);
    ts.frontMatter = FrontMatter{};
    ts.frontMatter->slot = slotName;
    ts.frontMatter->bpm = bpm;
    return ts;
}

// ===========================================================================
// round-trip tests
// ===========================================================================

TEST_CASE("WorkspaceSession: empty session round-trips", "[workspace]")
{
    WorkspaceSession session;
    std::string json = session.toJson();

    auto restored = WorkspaceSession::fromJson(json);
    REQUIRE(restored.has_value());
    REQUIRE(restored->tabs.empty());
    REQUIRE(restored->activeIndex == -1);
    REQUIRE(restored->settingsActive == false);
}

TEST_CASE("WorkspaceSession: single file tab round-trips", "[workspace]")
{
    WorkspaceSession session;
    session.activeIndex = 0;
    session.tabs.push_back(makeFileTab("/home/user/song.hathor", 3, false, 42));

    std::string json = session.toJson();
    auto restored = WorkspaceSession::fromJson(json);

    REQUIRE(restored.has_value());
    REQUIRE(restored->tabs.size() == 1);
    REQUIRE(restored->tabs[0].filePath == "/home/user/song.hathor");
    REQUIRE(restored->tabs[0].slotIndex == 3);
    REQUIRE(restored->tabs[0].isChuckTab == false);
    REQUIRE(restored->tabs[0].cursorOffset == 42);
}

TEST_CASE("WorkspaceSession: multiple tabs round-trip", "[workspace]")
{
    WorkspaceSession session;
    session.activeIndex = 1;
    session.settingsActive = true;
    session.tabs.push_back(makeFileTab("/a/b.hathor", 0));
    session.tabs.push_back(makeFileTab("/c/d.ck", 5, true, 10));
    session.tabs.push_back(makeUntitledTab("sin(440)", 7));

    std::string json = session.toJson();
    auto restored = WorkspaceSession::fromJson(json);

    REQUIRE(restored.has_value());
    REQUIRE(restored->tabs.size() == 3);
    REQUIRE(restored->activeIndex == 1);
    REQUIRE(restored->settingsActive == true);

    REQUIRE(restored->tabs[0].filePath == "/a/b.hathor");
    REQUIRE(restored->tabs[0].slotIndex == 0);
    REQUIRE(restored->tabs[0].content == "");

    REQUIRE(restored->tabs[1].filePath == "/c/d.ck");
    REQUIRE(restored->tabs[1].slotIndex == 5);
    REQUIRE(restored->tabs[1].isChuckTab == true);
    REQUIRE(restored->tabs[1].cursorOffset == 10);

    REQUIRE(restored->tabs[2].filePath == "");
    REQUIRE(restored->tabs[2].content == "sin(440)");
    REQUIRE(restored->tabs[2].slotIndex == 7);
}

// ===========================================================================
// front-matter tests
// ===========================================================================

TEST_CASE("WorkspaceSession: tab with front-matter round-trips", "[workspace]")
{
    WorkspaceSession session;
    session.tabs.push_back(makeTabWithFrontMatter("/song.hathor", "d1", 120.0));
    session.tabs.back().frontMatter->label = "My Song";
    session.tabs.back().displayLabel = "My Song";

    std::string json = session.toJson();
    auto restored = WorkspaceSession::fromJson(json);

    REQUIRE(restored.has_value());
    REQUIRE(restored->tabs.size() == 1);
    REQUIRE(restored->tabs[0].frontMatter.has_value());
    REQUIRE(restored->tabs[0].frontMatter->slot == "d1");
    REQUIRE(restored->tabs[0].frontMatter->bpm == 120.0);
    REQUIRE(restored->tabs[0].frontMatter->label == "My Song");
    REQUIRE(restored->tabs[0].displayLabel.has_value());
    REQUIRE(*restored->tabs[0].displayLabel == "My Song");
}

TEST_CASE("WorkspaceSession: tab without front-matter", "[workspace]")
{
    WorkspaceSession session;
    session.tabs.push_back(makeFileTab("/plain.hathor"));

    std::string json = session.toJson();
    auto restored = WorkspaceSession::fromJson(json);

    REQUIRE(restored.has_value());
    REQUIRE(restored->tabs[0].frontMatter.has_value() == false);
    REQUIRE(restored->tabs[0].displayLabel.has_value() == false);
}

// ===========================================================================
// version-mismatch / malformed-JSON tests
// ===========================================================================

TEST_CASE("WorkspaceSession: rejects mismatched schema version", "[workspace]")
{
    WorkspaceSession session;
    session.tabs.push_back(makeFileTab("/song.hathor"));
    std::string json = session.toJson();

    // Parse the JSON, bump the version, and re-serialize.
    auto j = nlohmann::json::parse(json);
    j["schemaVersion"] = 999;

    auto restored = WorkspaceSession::fromJson(j.dump());
    REQUIRE_FALSE(restored.has_value());
}

TEST_CASE("WorkspaceSession: rejects malformed JSON", "[workspace]")
{
    auto restored = WorkspaceSession::fromJson("{not valid json");
    REQUIRE_FALSE(restored.has_value());
}

TEST_CASE("WorkspaceSession: rejects non-object JSON", "[workspace]")
{
    auto restored = WorkspaceSession::fromJson("[1, 2, 3]");
    REQUIRE_FALSE(restored.has_value());
}

TEST_CASE("WorkspaceSession: rejects empty string", "[workspace]")
{
    auto restored = WorkspaceSession::fromJson("");
    REQUIRE_FALSE(restored.has_value());
}

TEST_CASE("WorkspaceSession: rejects missing schemaVersion", "[workspace]")
{
    auto j = nlohmann::json::object();
    j["activeIndex"] = 0;
    j["tabs"] = nlohmann::json::array();

    auto restored = WorkspaceSession::fromJson(j.dump());
    REQUIRE_FALSE(restored.has_value());
}

// ===========================================================================
// tab validation tests
// ===========================================================================

TEST_CASE("WorkspaceSession: rejects tab missing filePath", "[workspace]")
{
    auto j = nlohmann::json::object();
    j["schemaVersion"] = WorkspaceSession::kSchemaVersion;
    j["activeIndex"] = 0;
    j["tabs"] = nlohmann::json::array();
    auto tab = j.at("tabs").emplace_back();
    tab["slotIndex"] = 0;
    tab["isChuckTab"] = false;
    tab["cursorOffset"] = 0;

    auto restored = WorkspaceSession::fromJson(j.dump());
    REQUIRE_FALSE(restored.has_value());
}

TEST_CASE("WorkspaceSession: rejects tab with wrong types", "[workspace]")
{
    auto j = nlohmann::json::object();
    j["schemaVersion"] = WorkspaceSession::kSchemaVersion;
    j["activeIndex"] = 0;
    j["tabs"] = nlohmann::json::array();
    auto tab = j.at("tabs").emplace_back();
    tab["filePath"] = "/song.hathor";
    tab["slotIndex"] = "not_a_number";
    tab["isChuckTab"] = false;
    tab["cursorOffset"] = 0;

    auto restored = WorkspaceSession::fromJson(j.dump());
    REQUIRE_FALSE(restored.has_value());
}

// ===========================================================================
// fromJson with explicit version
// ===========================================================================

TEST_CASE("WorkspaceSession: fromJson with explicit version", "[workspace]")
{
    WorkspaceSession session;
    session.tabs.push_back(makeFileTab("/x.hathor"));

    std::string json = session.toJson();
    REQUIRE(WorkspaceSession::fromJson(json, WorkspaceSession::kSchemaVersion).has_value());
    REQUIRE_FALSE(WorkspaceSession::fromJson(json, WorkspaceSession::kSchemaVersion + 1).has_value());
}

TEST_CASE("WorkspaceSession: activeIndex defaults to -1 when missing", "[workspace]")
{
    auto j = nlohmann::json::object();
    j["schemaVersion"] = WorkspaceSession::kSchemaVersion;
    j["tabs"] = nlohmann::json::array();
    j["tabs"].push_back({{"filePath", "/a.hathor"}, {"slotIndex", 0},
                         {"isChuckTab", false}, {"cursorOffset", 0}});

    auto restored = WorkspaceSession::fromJson(j.dump());
    REQUIRE(restored.has_value());
    REQUIRE(restored->activeIndex == -1);
    REQUIRE(restored->settingsActive == false);
}
