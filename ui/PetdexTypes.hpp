// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetdexTypes.hpp — internal Petdex data model (Phase G / D1).
 *
 * JUCE-free by design: this model is shared by the manifest parser, the cache
 * store, the D4 attribution gate, the background manifest service, and the
 * Settings UI, so it lives in plain C++ with no framework dependencies.
 *
 * The model intentionally carries only the fields Hathor actually needs from
 * the live Petdex manifest (verified against https://petdex.dev/api/manifest):
 *   slug, displayName, kind, submittedBy, spritesheetUrl, petJsonUrl,
 *   zipUrl, spriteVersionNumber.
 *
 * Selection state vs resource state (PROGRAM.md Phase G / D1, requirement 11):
 *   - A *selected* pet is just a `slug` string persisted via the A2 settings
 *     model ("settings.petSelection"). Selecting a pet never downloads it.
 *   - The *catalog* (PetdexManifest) is metadata cached by PetdexManifestService.
 *   - Downloaded sprite/zip resources do not exist yet; they belong to D2/D3
 *     and will be a separate cache keyed by slug. Nothing in this file
 *     conflates the two.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// PetdexPet — one entry from the remote Petdex manifest.
// ---------------------------------------------------------------------------

struct PetdexPet
{
    std::string slug;              ///< stable, URL-safe id — the selection key
    std::string displayName;       ///< human-readable name
    std::string kind;              ///< character / creature / object (optional)
    std::string submittedBy;       ///< submitter handle — the ONLY attribution source
    std::string spritesheetUrl;    ///< WebP spritesheet (https) — verified format
    std::string petJsonUrl;        ///< pet.json descriptor URL (https)
    std::string zipUrl;            ///< package zip URL (https) — still WebP inside
    int         spriteVersionNumber = 1;  ///< sheet layout version (v1 = 8x9 grid)
};

// ---------------------------------------------------------------------------
// PetdexManifest — parsed catalog. `pets` is the source of truth; `total` is
// always the parsed entry count (the remote `total` field is informational —
// the manifest is dynamic and must not be hard-coded anywhere).
// ---------------------------------------------------------------------------

struct PetdexManifest
{
    std::string generatedAt;             ///< remote generation timestamp (informational)
    int         total = 0;               ///< == pets.size()
    std::vector<PetdexPet> pets;
};

// ---------------------------------------------------------------------------
// PetdexManifestStatus — what the UI shows about the catalog source.
// ---------------------------------------------------------------------------

enum class PetdexManifestStatus
{
    Idle,        ///< nothing requested yet
    Loading,     ///< cache/network resolution in progress
    Ready,       ///< usable catalog (fresh from network OR fresh cache)
    UsingCache,  ///< usable catalog from a stale cache while refresh runs/failed
    Offline,     ///< no network and no usable cache — cannot browse
};

// ---------------------------------------------------------------------------
// PetdexManifestResult — a single deliverable manifest state.
// ---------------------------------------------------------------------------

struct PetdexManifestResult
{
    PetdexManifestStatus status    = PetdexManifestStatus::Idle;
    PetdexManifest       manifest;
    bool                 fromCache = false;
    std::string          message;            ///< human-readable detail for the UI
    std::int64_t         fetchedAtEpochMs = 0; ///< when this data was obtained
};

} // namespace hathor::ui
