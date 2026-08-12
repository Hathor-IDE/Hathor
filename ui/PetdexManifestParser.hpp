// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetdexManifestParser.hpp — robust, JUCE-free parsing of the Petdex manifest.
 *
 * Handles the live shape at https://petdex.dev/api/manifest:
 *   { "generatedAt": "...", "total": N, "pets": [ { slug, displayName, kind,
 *     submittedBy, spritesheetUrl, petJsonUrl, zipUrl, spriteVersionNumber } ] }
 * plus the local cache envelope written by PetdexCacheStore
 *   { "version": "1", "fetchedAtEpochMs": N, "manifest": { ... } }.
 *
 * Robustness rules (PROGRAM.md Phase G / D1, requirement 5):
 *   - malformed JSON       → ok=false with a diagnostic error string
 *   - missing optional fields (kind/submittedBy/urls) → defaulted to ""
 *   - entries without a usable `slug` are skipped (slug is the stable key on
 *     which selection persists; an entry without one cannot be selected)
 *   - non-http(s) URLs are discarded (defence against garbage/hostile data)
 *   - `total` is always the parsed pet count, never trusted from the payload
 */

#include "PetdexTypes.hpp"

#include <cstdint>
#include <string>

namespace hathor::ui {

class PetdexManifestParser
{
public:
    /// Result of parsing a raw manifest body.
    struct ParseResult
    {
        bool           ok = false;
        PetdexManifest manifest;
        int            skipped = 0;   ///< entries dropped (no usable slug, non-object, …)
        std::string    error;         ///< populated when ok == false
    };

    /// Result of parsing a cache envelope.
    struct EnvelopeResult
    {
        bool           ok = false;
        PetdexManifest manifest;
        std::int64_t   fetchedAtEpochMs = 0;
        std::string    error;
    };

    /// Parse a raw manifest payload (object with "pets" array, or bare array).
    static ParseResult parseManifest(const std::string& json);

    /// Parse the local cache envelope (see makeCacheEnvelope).
    static EnvelopeResult parseCacheEnvelope(const std::string& json);

    /// Serialise a manifest + fetch timestamp into the cache envelope format.
    static std::string makeCacheEnvelope(const PetdexManifest& manifest,
                                         std::int64_t fetchedAtEpochMs);

    /// Current envelope version tag.
    static constexpr const char* kCacheVersion = "1";
};

} // namespace hathor::ui
