// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetdexResourceCache.hpp — on-disk cache for the SELECTED pet's resources.
 *
 * JUCE-free. Layout (under the Petdex cache root, e.g.
 * <userApplicationDataDirectory>/Hathor/Petdex):
 *
 *   manifest.json                  (D1 — manifest cache, owned by PetdexCacheStore)
 *   pets/<slug>/sprite.webp        (D2 — raw downloaded WebP, decoded on load)
 *   pets/<slug>/attribution.json   (D4 — attribution snapshot captured at selection)
 *
 * Design notes:
 *   - The cache is strictly per-selection: resources are written only after an
 *     explicit Apply of a selection, never at app startup.
 *   - The raw WebP is cached (compact, ~1-2 MB) and decoded into a JUCE image
 *     on load; decoding is fast and happens off the UI path. The in-memory
 *     decoded sheet is owned by the resource service so the same asset is not
 *     decoded repeatedly within a session.
 *   - Corrupt cache handling: the caller decodes; on decode failure it calls
 *     removeSprite() and re-downloads. readAttribution on a corrupt file
 *     simply reports not-present, which blocks display (D4) rather than
 *     showing a pet whose attribution cannot be established.
 *   - Slug sanitisation prevents path traversal (slugs are used in file paths).
 */

#include "PetdexTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hathor::ui {

class PetdexResourceCache
{
public:
    /// Directory for one pet: <root>/pets/<slug>.
    static std::filesystem::path petDir(const std::filesystem::path& root,
                                        const std::string& slug);

    /// Path of the cached WebP spritesheet.
    static std::filesystem::path spritePath(const std::filesystem::path& root,
                                            const std::string& slug);

    /// Path of the attribution snapshot JSON.
    static std::filesystem::path attributionPath(const std::filesystem::path& root,
                                                 const std::string& slug);

    /// Make a slug safe for use as a path component (no separators/traversal).
    static std::string sanitizeSlug(const std::string& slug);

    /// True if a sprite file exists for the pet.
    static bool hasSprite(const std::filesystem::path& root, const std::string& slug);

    /// Read the cached sprite bytes. Returns false if missing/unreadable.
    static bool readSprite(const std::filesystem::path& root,
                           const std::string& slug,
                           std::vector<std::uint8_t>& outBytes);

    /// Write the sprite bytes (creates directories). Returns success.
    static bool writeSprite(const std::filesystem::path& root,
                            const std::string& slug,
                            const std::uint8_t* data,
                            std::size_t size);

    /// Remove a corrupt sprite so it can be re-downloaded.
    static bool removeSprite(const std::filesystem::path& root, const std::string& slug);

    /// Persist the D4 attribution snapshot for the pet.
    static bool writeAttribution(const std::filesystem::path& root,
                                 const std::string& slug,
                                 const PetdexAttributionSnapshot& snapshot);

    /// Read the snapshot. Returns false if missing/corrupt (display is blocked).
    static bool readAttribution(const std::filesystem::path& root,
                                const std::string& slug,
                                PetdexAttributionSnapshot& outSnapshot);

    static constexpr const char* kPetsSubdir   = "pets";
    static constexpr const char* kSpriteName   = "sprite.webp";
    static constexpr const char* kAttributionName = "attribution.json";
};

} // namespace hathor::ui
