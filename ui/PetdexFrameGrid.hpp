// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetdexFrameGrid.hpp — spritesheet geometry + animation-state convention (D3).
 *
 * JUCE-free. All facts below were VERIFIED against real data (2026-08-12,
 * documented in docs/design/petdex-d1-d4-decision.md):
 *   - The manifest's spritesheets are WebP, decoded dimensions for a live pet
 *     ("homelander") are exactly 1536 x 1872.
 *   - Frame size is 192 x 208. The v1 sheet is 8 columns x 9 rows
 *     (1536 x 1872); v2 is 8 columns x 11 rows (1536 x 2288, extra rows
 *     reserved for client extensions).
 *   - Row-to-state mapping comes from the petdex project's pet-states table
 *     (verified against crafter-station/petdex src/lib/pet-states.ts):
 *       row 0 idle, row 1 running-right, row 2 running-left, row 3 waving,
 *       row 4 jumping, row 5 failed, row 6 waiting, row 7 running, row 8 review.
 *   - Neither the manifest nor pet.json/petjson.json encodes the grid or the
 *     state rows, so the grid is derived from the DECODED sheet dimensions
 *     (source of truth) and the convention fills in the state rows.
 *
 * Only 8-column sheets are supported (the Petdex convention). A sheet whose
 * dimensions are not a multiple of 192 x 208, or whose column count differs
 * from 8, is reported invalid so the UI can surface a clear error instead of
 * slicing garbage.
 */

#include <string>
#include <vector>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// One animation state (a row of the spritesheet).
// ---------------------------------------------------------------------------

struct PetdexAnimationState
{
    std::string id;        ///< stable id, e.g. "idle", "running"
    std::string label;     ///< display label, e.g. "Run Right"
    int         row = 0;   ///< zero-based spritesheet row
    int         frames = 0;    ///< number of frames in the row's sequence
    int         durationMs = 0; ///< whole-sequence duration (deterministic timing)
};

// ---------------------------------------------------------------------------
// A rectangle within the sheet (frame geometry).
// ---------------------------------------------------------------------------

struct PetdexFrameRect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// ---------------------------------------------------------------------------
// The analyzed sheet.
// ---------------------------------------------------------------------------

class PetdexFrameGrid
{
public:
    static constexpr int kFrameWidth  = 192;
    static constexpr int kFrameHeight = 208;
    static constexpr int kConventionCols = 8;
    static constexpr int kConventionStateRows = 9;

    /// The verified Petdex state convention (rows 0..8), in row order.
    static const std::vector<PetdexAnimationState>& conventionStates();

    /// The default state id when nothing else applies ("idle").
    static constexpr const char* kDefaultStateId = "idle";

    /// Analyze a decoded sheet. `spriteVersionNumber` is informational only —
    /// the actual decoded dimensions are the source of truth.
    static PetdexFrameGrid analyze(int sheetWidth, int sheetHeight);

    // --- Result fields ------------------------------------------------------
    bool   valid = false;
    int    sheetWidth  = 0;
    int    sheetHeight = 0;
    int    cols = 0;
    int    rows = 0;
    std::vector<PetdexAnimationState> states;   ///< convention states whose row < rows
    std::string error;                          ///< populated when !valid

    /// Rectangle of (row, frame) — valid only when valid == true and the row
    /// is within [0, rows) and frame within [0, 8).
    PetdexFrameRect frameRect(int row, int frame) const noexcept;

    /// Look up a state by id; nullptr if unknown or its row >= rows.
    const PetdexAnimationState* findState(const std::string& id) const noexcept;
};

} // namespace hathor::ui
