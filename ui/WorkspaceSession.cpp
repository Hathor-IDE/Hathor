// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * WorkspaceSession.cpp — JSON serialisation for WorkspaceSession.
 *
 * Requirements: 20.7 (workspace persistence)
 */

#include "WorkspaceSession.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// FrontMatter JSON helpers (JUCE-free)
// ---------------------------------------------------------------------------

static nlohmann::json frontMatterToJson(const FrontMatter& fm)
{
    nlohmann::json j = nlohmann::json::object();
    if (fm.slot.has_value())   j["slot"]  = *fm.slot;
    if (fm.bpm.has_value())   j["bpm"]   = *fm.bpm;
    if (fm.bank.has_value())  j["bank"]  = *fm.bank;
    if (fm.label.has_value()) j["label"] = *fm.label;
    if (fm.color.has_value()) j["color"] = *fm.color;
    return j;
}

static std::optional<FrontMatter> frontMatterFromJson(const nlohmann::json& j)
{
    if (!j.is_object())
        return std::nullopt;

    FrontMatter fm;

    if (j.contains("slot") && j["slot"].is_string())
        fm.slot = j["slot"].get<std::string>();
    if (j.contains("bpm") && j["bpm"].is_number())
        fm.bpm = j["bpm"].get<double>();
    if (j.contains("bank") && j["bank"].is_string())
        fm.bank = j["bank"].get<std::string>();
    if (j.contains("label") && j["label"].is_string())
        fm.label = j["label"].get<std::string>();
    if (j.contains("color") && j["color"].is_string())
        fm.color = j["color"].get<std::string>();

    return fm;
}

// ---------------------------------------------------------------------------
// TabState JSON helpers
// ---------------------------------------------------------------------------

static nlohmann::json tabStateToJson(const TabState& ts)
{
    nlohmann::json j = nlohmann::json::object();
    j["filePath"]      = ts.filePath;
    j["slotIndex"]     = ts.slotIndex;
    j["isChuckTab"]    = ts.isChuckTab;
    j["cursorOffset"]  = ts.cursorOffset;
    j["content"]       = ts.content;
    if (ts.frontMatter.has_value())
        j["frontMatter"]  = frontMatterToJson(*ts.frontMatter);
    if (ts.displayLabel.has_value())
        j["displayLabel"] = *ts.displayLabel;
    return j;
}

static std::optional<TabState> tabStateFromJson(const nlohmann::json& j)
{
    if (!j.is_object())
        return std::nullopt;

    TabState ts;

    if (!j.contains("filePath") || !j["filePath"].is_string())
        return std::nullopt;
    ts.filePath = j["filePath"].get<std::string>();

    if (!j.contains("slotIndex") || !j["slotIndex"].is_number_integer())
        return std::nullopt;
    ts.slotIndex = j["slotIndex"].get<int>();

    if (!j.contains("isChuckTab") || !j["isChuckTab"].is_boolean())
        return std::nullopt;
    ts.isChuckTab = j["isChuckTab"].get<bool>();

    if (!j.contains("cursorOffset") || !j["cursorOffset"].is_number_integer())
        return std::nullopt;
    ts.cursorOffset = j["cursorOffset"].get<int64_t>();

    if (j.contains("content") && j["content"].is_string())
        ts.content = j["content"].get<std::string>();

    if (j.contains("frontMatter") && j["frontMatter"].is_object())
    {
        auto fm = frontMatterFromJson(j["frontMatter"]);
        if (fm.has_value())
            ts.frontMatter = std::move(*fm);
    }

    if (j.contains("displayLabel") && j["displayLabel"].is_string())
        ts.displayLabel = j["displayLabel"].get<std::string>();

    return ts;
}

// ---------------------------------------------------------------------------
// WorkspaceSession serialisation
// ---------------------------------------------------------------------------

std::string WorkspaceSession::toJson() const
{
    nlohmann::json j = nlohmann::json::object();
    j["schemaVersion"]   = schemaVersion;
    j["activeIndex"]     = activeIndex;
    j["settingsActive"]  = settingsActive;
    j["tabs"]            = nlohmann::json::array();

    for (const auto& tab : tabs)
        j["tabs"].push_back(tabStateToJson(tab));

    return j.dump();
}

std::optional<WorkspaceSession>
WorkspaceSession::fromJson(const std::string& jsonStr, int acceptedVersion)
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
    if (version != acceptedVersion)
        return std::nullopt;

    WorkspaceSession session;
    session.schemaVersion = version;

    if (j.contains("activeIndex") && j["activeIndex"].is_number_integer())
        session.activeIndex = j["activeIndex"].get<int>();
    else
        session.activeIndex = -1;

    if (j.contains("settingsActive") && j["settingsActive"].is_boolean())
        session.settingsActive = j["settingsActive"].get<bool>();
    else
        session.settingsActive = false;

    if (j.contains("tabs") && j["tabs"].is_array())
    {
        for (const auto& tabJson : j["tabs"])
        {
            auto tab = tabStateFromJson(tabJson);
            if (!tab.has_value())
                return std::nullopt;
            session.tabs.push_back(std::move(*tab));
        }
    }

    return session;
}

std::optional<WorkspaceSession>
WorkspaceSession::fromJson(const std::string& jsonStr)
{
    return fromJson(jsonStr, kSchemaVersion);
}

} // namespace hathor::ui
