// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ChatSessionState.hpp — JUCE-free model for serialising/restoring chat
 * sidebar thread tabs (titles + active index) into a juce::PropertiesFile-
 * compatible JSON string.
 *
 * This struct is intentionally JUCE-free so it can be unit-tested without
 * a JUCE message loop (compiled into hathor-ui-tests alongside
 * WorkspaceSession).  It mirrors the WorkspaceSession pattern (Req 20.7).
 *
 * Requirements: B6 (chat thread persistence)
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hathor::ui {

/**
 * ThreadState — persisted descriptor for a single chat thread tab.
 *
 * Fields:
 *   title  — display title for the tab (e.g. "Thread 1", "Agent Session")
 */
struct ChatThreadState {
    std::string title;
};

/**
 * ChatSessionState — serialisable description of the chat sidebar's
 * thread list.  Stored as a JSON string inside a single
 * juce::PropertiesFile value ("chatThreadsData").
 */
struct ChatSessionState {
    /// Current schema version (bump on incompatible change).
    static constexpr int kSchemaVersion = 1;

    int                       schemaVersion = kSchemaVersion;
    int                       activeIndex   = -1;  ///< -1 = no thread active
    std::vector<ChatThreadState> threads;

    /** Serialise to a compact JSON string. */
    std::string toJson() const;

    /**
     * Parse a JSON string into a ChatSessionState.
     *
     * Returns nullopt if:
     *   - the JSON is malformed / truncated
     *   - the schemaVersion field is missing or does not match kSchemaVersion
     */
    static std::optional<ChatSessionState> fromJson(const std::string& jsonStr);

    /**
     * Parse with an explicit accepted schema version (for migration logic).
     */
    static std::optional<ChatSessionState> fromJson(const std::string& jsonStr,
                                                     int acceptedVersion);
};

} // namespace hathor::ui
