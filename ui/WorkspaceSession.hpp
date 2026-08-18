// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * WorkspaceSession.hpp — JUCE-free model for serialising/restoring the editor
 * workspace (open tabs, cursor positions, slot assignments, pattern/slot state)
 * into a juce::PropertiesFile-compatible JSON string.
 *
 * This struct is intentionally JUCE-free so that it can be unit-tested without
 * a JUCE build (compiled into hathor-ui-tests which links hathor-engine only).
 *
 * Schema versioning:
 *   - kSchemaVersion is the current on-disk schema version.
 *   - fromJson() rejects any JSON whose "schemaVersion" field does not match
 *     kSchemaVersion (fail-safe: unknown/future schemas are ignored, not
 *     silently misinterpreted).
 *
 * Requirements: 20.7 (workspace persistence)
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "HathorFileParser.hpp"  // FrontMatter

namespace hathor::ui {

/**
 * TabState — persisted descriptor for a single editor tab.
 *
 * Fields:
 *   filePath       — absolute file path, empty for untitled buffers
 *   slotIndex      — AudioEngine pattern slot [0, kNumSlots)
 *   isChuckTab     — true when the tab uses the ChucK tokeniser (.ck)
 *   cursorOffset   — absolute character offset of the caret in the document
 *   content        — buffer text; populated for untitled tabs (and retained as
 *                    a fallback when a file-backed tab's file is missing)
 *   frontMatter    — parsed front-matter metadata (slot name, bpm, label, etc.)
 *   displayLabel   — explicit display label override (usually from front-matter)
 */
struct TabState {
    std::string filePath;
    int         slotIndex     = 0;
    bool        isChuckTab    = false;
    int64_t     cursorOffset  = 0;
    std::string content;
    std::optional<FrontMatter> frontMatter;
    std::optional<std::string> displayLabel;
};

/**
 * WorkspaceSession — serialisable description of the full editor workspace.
 *
 * Stored as a JSON string inside a single juce::PropertiesFile value
 * ("workspaceData").  The integer "workspaceSchemaVersion" property key
 * tracks the schema so incompatible versions can be detected without
 * attempting to parse the JSON blob.
 */
struct WorkspaceSession {
    /// Current workspace schema version (bump on incompatible change).
    static constexpr int kSchemaVersion = 1;

    int                schemaVersion  = kSchemaVersion;
    int                activeIndex    = -1;  ///< -1 = no HathorTab active
    bool               settingsActive = false;
    std::vector<TabState> tabs;

    /** Serialise to a compact JSON string. */
    std::string toJson() const;

    /**
     * Parse a JSON string into a WorkspaceSession.
     *
     * Returns nullopt if:
     *   - the JSON is malformed / truncated
     *   - the schemaVersion field is missing or does not match kSchemaVersion
     *   - any tab entry is missing required fields
     */
    static std::optional<WorkspaceSession> fromJson(const std::string& jsonStr);

    /**
     * Parse with an explicit accepted schema version (for migration logic).
     * Returns nullopt on malformed JSON or version mismatch.
     */
    static std::optional<WorkspaceSession> fromJson(const std::string& jsonStr,
                                                     int acceptedVersion);
};

} // namespace hathor::ui
