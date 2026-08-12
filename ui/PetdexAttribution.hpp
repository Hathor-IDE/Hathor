// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetdexAttribution.hpp — D4 licensing/attribution gate (JUCE-free).
 *
 * Verified facts this gate is built on (see docs/design/petdex-d1-d4-decision.md):
 *   - The Petdex manifest has NO license field. The pet zip/pet.json packages
 *     contain no license or attribution file either.
 *   - The ONLY attribution information anywhere is the manifest's `submittedBy`
 *     handle (plus the platform credit for petdex.dev itself).
 *
 * The gate therefore:
 *   - never invents or claims a license (there is none to claim);
 *   - resolves the attribution that CAN legitimately be displayed — the
 *     submitter handle — for every pet that is actually displayed;
 *   - blocks display (canDisplay == false) when attribution cannot be
 *     established (no submitter), with a clear explanatory notice so the UI
 *     never silently renders such a pet.
 *
 * D2/D3 rendering code must consult canDisplay() before showing any pet.
 */

#include "PetdexTypes.hpp"

#include <string>

namespace hathor::ui {

class PetdexAttribution
{
public:
    struct Info
    {
        bool        canDisplay = false;  ///< D4 gate: may this pet be rendered?
        std::string submitter;           ///< resolved attribution credit
        std::string creditLine;          ///< display-ready attribution ("Submitted by …")
        std::string notice;              ///< license-status note for the UI
    };

    /// Resolve attribution for a manifest entry.
    static Info resolve(const PetdexPet& pet);

    /// Build a persisted attribution snapshot from a manifest entry (D4 record).
    static PetdexAttributionSnapshot buildSnapshot(const PetdexPet& pet);

    /// Platform credit appended to every displayed pet.
    static constexpr const char* kPlatformCredit = "Petdex community gallery (petdex.dev)";

    /// Shown whenever a pet IS displayable — documents the no-license reality.
    static constexpr const char* kNoLicenseNotice =
        "The Petdex manifest declares no per-pet license; attribution is by submitter only.";

    /// Shown when attribution cannot be established (blocking display).
    static constexpr const char* kMissingAttributionNotice =
        "This pet cannot be displayed: the Petdex manifest provides no license "
        "field and this entry has no submitter, so attribution cannot be established.";
};

} // namespace hathor::ui
