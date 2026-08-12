// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetdexCacheStore.hpp — JUCE-free disk cache for the Petdex manifest.
 *
 * Stores the cache envelope produced by PetdexManifestParser::makeCacheEnvelope
 * at <dir>/manifest.json. Staleness is decided by the *fetch time* recorded in
 * the envelope (not the file mtime), so tests can drive the policy
 * deterministically and the policy stays a pure function.
 *
 * The store is deliberately NOT a general-purpose cache: it knows exactly one
 * file ("manifest.json"), one envelope format, and one freshness policy.
 * PetdexManifestService owns the policy decision (when to fetch), while this
 * class only persists/reads/ages.
 */

#include <cstdint>
#include <filesystem>
#include <string>

namespace hathor::ui {

class PetdexCacheStore
{
public:
    struct ReadResult
    {
        bool        present = false;  ///< file existed and was readable
        std::string json;             ///< raw envelope content (may be corrupt — caller parses)
    };

    /// Path of the single cache file inside @p dir.
    static std::filesystem::path manifestPath(const std::filesystem::path& dir);

    /// Read the cache file. present=false if missing/unreadable.
    static ReadResult read(const std::filesystem::path& dir);

    /// Write the envelope (creating @p dir as needed). Returns success.
    static bool write(const std::filesystem::path& dir, const std::string& envelopeJson);

    /// Age of a cache entry in ms. Ages are clamped at 0 (future timestamps
    /// are treated as fresh rather than negative).
    static std::int64_t ageMs(std::int64_t fetchedAtEpochMs,
                              std::int64_t nowEpochMs);

    /// A cache entry is stale once its age reaches maxAgeMs (boundary inclusive).
    static bool isStale(std::int64_t fetchedAtEpochMs,
                        std::int64_t nowEpochMs,
                        std::int64_t maxAgeMs);

    static constexpr const char* kFileName = "manifest.json";
};

} // namespace hathor::ui
