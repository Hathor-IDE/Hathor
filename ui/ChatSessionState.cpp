// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChatSessionState.cpp — JSON serialisation for ChatSessionState.
 *
 * Requirements: B6 (chat thread persistence)
 */

#include "ChatSessionState.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// ThreadState JSON helpers
// ---------------------------------------------------------------------------

static nlohmann::json threadStateToJson(const ChatThreadState& ts)
{
    nlohmann::json j = nlohmann::json::object();
    j["title"] = ts.title;
    j["messages"] = nlohmann::json::array();
    for (const auto& m : ts.messages)
    {
        nlohmann::json jm = nlohmann::json::object();
        jm["role"] = m.role;
        jm["text"] = m.text;
        j["messages"].push_back(std::move(jm));
    }
    return j;
}

static std::optional<ChatThreadState> threadStateFromJson(const nlohmann::json& j)
{
    if (!j.is_object())
        return std::nullopt;

    ChatThreadState ts;

    if (!j.contains("title") || !j["title"].is_string())
        return std::nullopt;
    ts.title = j["title"].get<std::string>();

    // messages optional (v1 files have none) — corrupt entries degrade to empty.
    if (j.contains("messages") && j["messages"].is_array())
    {
        for (const auto& jm : j["messages"])
        {
            if (!jm.is_object())
                continue;
            ChatThreadState::Message m;
            if (jm.contains("role") && jm["role"].is_number_integer())
                m.role = jm["role"].get<int>();
            if (jm.contains("text") && jm["text"].is_string())
                m.text = jm["text"].get<std::string>();
            if (m.role < 0 || m.role > 3)
                continue;
            if (m.text.size() > 65536)
                m.text.resize(65536);
            ts.messages.push_back(std::move(m));
        }
        if (ts.messages.size() > 200)
            ts.messages.erase(ts.messages.begin(),
                              ts.messages.begin() + (ts.messages.size() - 200));
    }

    return ts;
}

// ---------------------------------------------------------------------------
// ChatSessionState serialisation
// ---------------------------------------------------------------------------

std::string ChatSessionState::toJson() const
{
    nlohmann::json j = nlohmann::json::object();
    j["schemaVersion"] = schemaVersion;
    j["activeIndex"]   = activeIndex;
    j["threads"]       = nlohmann::json::array();

    for (const auto& thread : threads)
        j["threads"].push_back(threadStateToJson(thread));

    return j.dump();
}

std::optional<ChatSessionState>
ChatSessionState::fromJson(const std::string& jsonStr, int acceptedVersion)
{
    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(jsonStr);
    }
    catch (const nlohmann::json::exception&)
    {
        return std::nullopt;
    }

    if (!j.is_object())
        return std::nullopt;

    if (!j.contains("schemaVersion") || !j["schemaVersion"].is_number_integer())
        return std::nullopt;

    const int version = j["schemaVersion"].get<int>();
    // Accept current (v2) plus legacy v1 titles-only files.
    if (version != acceptedVersion && version != 1)
        return std::nullopt;

    ChatSessionState state;
    state.schemaVersion = version;

    if (j.contains("activeIndex") && j["activeIndex"].is_number_integer())
        state.activeIndex = j["activeIndex"].get<int>();
    else
        state.activeIndex = -1;

    if (j.contains("threads") && j["threads"].is_array())
    {
        for (const auto& threadJson : j["threads"])
        {
            auto ts = threadStateFromJson(threadJson);
            if (!ts.has_value())
                return std::nullopt;
            state.threads.push_back(std::move(*ts));
        }
    }

    return state;
}

std::optional<ChatSessionState>
ChatSessionState::fromJson(const std::string& jsonStr)
{
    return fromJson(jsonStr, kSchemaVersion);
}

} // namespace hathor::ui
