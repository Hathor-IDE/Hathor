// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PetdexFrameGrid.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Verified Petdex state convention (row -> state), from the petdex project's
// src/lib/pet-states.ts (fetched and verified 2026-08-12).
// ---------------------------------------------------------------------------

const std::vector<PetdexAnimationState>& PetdexFrameGrid::conventionStates()
{
    static const std::vector<PetdexAnimationState> states = {
        { "idle",           "Idle",      0, 6, 1100 },
        { "running-right",  "Run Right", 1, 8, 1060 },
        { "running-left",   "Run Left",  2, 8, 1060 },
        { "waving",         "Waving",    3, 4,  700 },
        { "jumping",        "Jumping",   4, 5,  840 },
        { "failed",         "Failed",    5, 8, 1220 },
        { "waiting",        "Waiting",   6, 6, 1010 },
        { "running",        "Running",   7, 6,  820 },
        { "review",         "Review",    8, 6, 1030 },
    };
    return states;
}

PetdexFrameGrid PetdexFrameGrid::analyze(int sheetWidth, int sheetHeight)
{
    PetdexFrameGrid grid;
    grid.sheetWidth  = sheetWidth;
    grid.sheetHeight = sheetHeight;

    if (sheetWidth <= 0 || sheetHeight <= 0)
    {
        grid.error = "spritesheet has no decoded dimensions";
        return grid;
    }

    if (sheetWidth % kFrameWidth != 0 || sheetHeight % kFrameHeight != 0)
    {
        grid.error = "spritesheet dimensions "
                   + std::to_string(sheetWidth) + "x" + std::to_string(sheetHeight)
                   + " are not a multiple of the " + std::to_string(kFrameWidth)
                   + "x" + std::to_string(kFrameHeight) + " frame size";
        return grid;
    }

    grid.cols = sheetWidth / kFrameWidth;
    grid.rows = sheetHeight / kFrameHeight;

    if (grid.cols != kConventionCols)
    {
        grid.error = "expected " + std::to_string(kConventionCols)
                   + " frame columns (Petdex convention), got "
                   + std::to_string(grid.cols);
        return grid;
    }

    // Rows 0..8 map to the convention states; extra rows (v2) have no
    // defined state and are simply not advertised.
    for (const auto& state : conventionStates())
        if (state.row < grid.rows)
            grid.states.push_back(state);

    grid.valid = true;
    return grid;
}

PetdexFrameRect PetdexFrameGrid::frameRect(int row, int frame) const noexcept
{
    PetdexFrameRect r;
    r.w = kFrameWidth;
    r.h = kFrameHeight;
    r.x = frame * kFrameWidth;
    r.y = row * kFrameHeight;
    return r;
}

const PetdexAnimationState* PetdexFrameGrid::findState(const std::string& id) const noexcept
{
    for (const auto& state : states)
        if (state.id == id)
            return &state;
    return nullptr;
}

} // namespace hathor::ui
