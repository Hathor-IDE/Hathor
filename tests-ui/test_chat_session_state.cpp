// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_chat_session_state.cpp — unit tests for the JUCE-free ChatSessionState
 * serialization model (B6: chat thread persistence).
 *
 * Runs headless (no JUCE audio device) as part of hathor-ui-tests.
 */

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "ChatSessionState.hpp"

using hathor::ui::ChatSessionState;
using hathor::ui::ChatThreadState;

// ===========================================================================
// round-trip tests
// ===========================================================================

TEST_CASE("ChatSessionState: empty state round-trips", "[chat][persistence]")
{
    ChatSessionState state;
    std::string json = state.toJson();

    auto restored = ChatSessionState::fromJson(json);
    REQUIRE(restored.has_value());
    REQUIRE(restored->threads.empty());
    REQUIRE(restored->activeIndex == -1);
    REQUIRE(restored->schemaVersion == ChatSessionState::kSchemaVersion);
}

TEST_CASE("ChatSessionState: single thread round-trips", "[chat][persistence]")
{
    ChatSessionState state;
    state.activeIndex = 0;

    ChatThreadState ts;
    ts.title = "Agent Session";
    state.threads.push_back(std::move(ts));

    std::string json = state.toJson();
    auto restored = ChatSessionState::fromJson(json);

    REQUIRE(restored.has_value());
    REQUIRE(restored->threads.size() == 1);
    REQUIRE(restored->threads[0].title == "Agent Session");
    REQUIRE(restored->activeIndex == 0);
}

TEST_CASE("ChatSessionState: multiple threads round-trip with active index", "[chat][persistence]")
{
    ChatSessionState state;
    state.activeIndex = 2;

    ChatThreadState t1; t1.title = "Thread 1";
    ChatThreadState t2; t2.title = "Thread 2";
    ChatThreadState t3; t3.title = "Agent Chat";
    state.threads.push_back(std::move(t1));
    state.threads.push_back(std::move(t2));
    state.threads.push_back(std::move(t3));

    std::string json = state.toJson();
    auto restored = ChatSessionState::fromJson(json);

    REQUIRE(restored.has_value());
    REQUIRE(restored->threads.size() == 3);
    REQUIRE(restored->threads[0].title == "Thread 1");
    REQUIRE(restored->threads[1].title == "Thread 2");
    REQUIRE(restored->threads[2].title == "Agent Chat");
    REQUIRE(restored->activeIndex == 2);
}

// ===========================================================================
// version-mismatch / malformed-JSON tests
// ===========================================================================

TEST_CASE("ChatSessionState: rejects mismatched schema version", "[chat][persistence]")
{
    ChatSessionState state;
    ChatThreadState ts;
    ts.title = "Thread 1";
    state.threads.push_back(std::move(ts));

    std::string json = state.toJson();
    auto j = nlohmann::json::parse(json);
    j["schemaVersion"] = 999;

    auto restored = ChatSessionState::fromJson(j.dump());
    REQUIRE_FALSE(restored.has_value());
}

TEST_CASE("ChatSessionState: rejects malformed JSON", "[chat][persistence]")
{
    auto restored = ChatSessionState::fromJson("{not valid json");
    REQUIRE_FALSE(restored.has_value());
}

TEST_CASE("ChatSessionState: rejects non-object JSON", "[chat][persistence]")
{
    auto restored = ChatSessionState::fromJson("[1, 2, 3]");
    REQUIRE_FALSE(restored.has_value());
}

TEST_CASE("ChatSessionState: rejects empty string", "[chat][persistence]")
{
    auto restored = ChatSessionState::fromJson("");
    REQUIRE_FALSE(restored.has_value());
}

TEST_CASE("ChatSessionState: rejects missing schemaVersion", "[chat][persistence]")
{
    auto j = nlohmann::json::object();
    j["activeIndex"] = 0;
    j["threads"] = nlohmann::json::array();

    auto restored = ChatSessionState::fromJson(j.dump());
    REQUIRE_FALSE(restored.has_value());
}

// ===========================================================================
// thread validation tests
// ===========================================================================

TEST_CASE("ChatSessionState: rejects thread missing title", "[chat][persistence]")
{
    auto j = nlohmann::json::object();
    j["schemaVersion"] = ChatSessionState::kSchemaVersion;
    j["activeIndex"] = 0;
    j["threads"] = nlohmann::json::array();
    auto tab = j.at("threads").emplace_back();
    tab["titleNumber"] = 42;

    auto restored = ChatSessionState::fromJson(j.dump());
    REQUIRE_FALSE(restored.has_value());
}

TEST_CASE("ChatSessionState: rejects thread with wrong title type", "[chat][persistence]")
{
    auto j = nlohmann::json::object();
    j["schemaVersion"] = ChatSessionState::kSchemaVersion;
    j["activeIndex"] = 0;
    j["threads"] = nlohmann::json::array();
    auto tab = j.at("threads").emplace_back();
    tab["title"] = 12345;

    auto restored = ChatSessionState::fromJson(j.dump());
    REQUIRE_FALSE(restored.has_value());
}

// ===========================================================================
// fromJson with explicit version
// ===========================================================================

TEST_CASE("ChatSessionState: fromJson with explicit version", "[chat][persistence]")
{
    ChatSessionState state;
    ChatThreadState ts;
    ts.title = "Test Thread";
    state.threads.push_back(std::move(ts));

    std::string json = state.toJson();
    REQUIRE(ChatSessionState::fromJson(json, ChatSessionState::kSchemaVersion).has_value());
    REQUIRE_FALSE(ChatSessionState::fromJson(json, ChatSessionState::kSchemaVersion + 1).has_value());
}

// ===========================================================================
// activeIndex defaults
// ===========================================================================

TEST_CASE("ChatSessionState: activeIndex defaults to -1 when missing", "[chat][persistence]")
{
    auto j = nlohmann::json::object();
    j["schemaVersion"] = ChatSessionState::kSchemaVersion;
    j["threads"] = nlohmann::json::array();
    j["threads"].push_back({{"title", "Thread 1"}});

    auto restored = ChatSessionState::fromJson(j.dump());
    REQUIRE(restored.has_value());
    REQUIRE(restored->activeIndex == -1);
}

// ===========================================================================
// thread-removal simulation
// ===========================================================================

TEST_CASE("ChatSessionState: removing a thread produces a shorter list", "[chat][persistence]")
{
    ChatSessionState state;
    state.activeIndex = 0;

    ChatThreadState t1; t1.title = "Thread 1";
    ChatThreadState t2; t2.title = "Thread 2";
    ChatThreadState t3; t3.title = "Thread 3";
    state.threads.push_back(std::move(t1));
    state.threads.push_back(std::move(t2));
    state.threads.push_back(std::move(t3));

    // Simulate closing thread at index 1 (like closeTab would).
    state.threads.erase(state.threads.begin() + 1);
    if (state.activeIndex > 1)
        --state.activeIndex;

    std::string json = state.toJson();
    auto restored = ChatSessionState::fromJson(json);

    REQUIRE(restored.has_value());
    REQUIRE(restored->threads.size() == 2);
    REQUIRE(restored->threads[0].title == "Thread 1");
    REQUIRE(restored->threads[1].title == "Thread 3");
    // Thread 2 is gone — does not reappear after round-trip.
    bool foundRemoved = false;
    for (const auto& t : restored->threads)
    {
        if (t.title == "Thread 2")
            foundRemoved = true;
    }
    REQUIRE_FALSE(foundRemoved);
}
