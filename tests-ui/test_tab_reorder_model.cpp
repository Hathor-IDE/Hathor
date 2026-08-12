// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_tab_reorder_model.cpp — unit tests for TabReorderModel.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
 */

#include <catch2/catch_test_macros.hpp>

#include "TabReorderModel.hpp"

using namespace hathor::ui;

// ===========================================================================
// Basic operations
// ===========================================================================

TEST_CASE("TabReorderModel: resize and isPinned default false", "[tabreorder]")
{
    TabReorderModel m;
    m.resize(3);

    REQUIRE_FALSE(m.isPinned(0));
    REQUIRE_FALSE(m.isPinned(1));
    REQUIRE_FALSE(m.isPinned(2));
}

TEST_CASE("TabReorderModel: togglePin just toggles flag", "[tabreorder]")
{
    TabReorderModel m;
    m.resize(3);

    m.togglePin(1);
    REQUIRE_FALSE(m.isPinned(0));
    REQUIRE(m.isPinned(1));
    REQUIRE_FALSE(m.isPinned(2));

    m.togglePin(1);
    REQUIRE_FALSE(m.isPinned(1));
}

// ===========================================================================
// Drop index computation
// ===========================================================================

TEST_CASE("TabReorderModel: computeDropIndex basic reorder", "[tabreorder]")
{
    TabReorderModel m;
    m.resize(3);

    // Boundaries for 3 equal-width tabs of width 100
    std::vector<float> bounds = {0, 100, 200, 300};

    // Drag tab 0 to position of tab 2
    size_t dropIdx = m.computeDropIndex(0, 250, bounds);
    REQUIRE(dropIdx == 2);
}

TEST_CASE("TabReorderModel: computeDropIndex returns fromIndex if same", "[tabreorder]")
{
    TabReorderModel m;
    m.resize(3);

    std::vector<float> bounds = {0, 100, 200, 300};

    // Drag tab 0 to position of tab 0
    size_t dropIdx = m.computeDropIndex(0, 50, bounds);
    REQUIRE(dropIdx == 0);  // same tab, no reorder
}

TEST_CASE("TabReorderModel: computeDropIndex clamped for pinned drag", "[tabreorder]")
{
    TabReorderModel m;
    m.resize(4);
    m.togglePin(0);  // pin tab 0

    // After pinning tab 0, it's already at front. Now pin tab 1 too.
    // Pinned count should be 1 (only index 0)
    // Try dragging tab 0 (pinned) into unpinned territory
    std::vector<float> bounds = {0, 100, 200, 300, 400};

    // Mouse is in the unpinned region (after first 100px)
    size_t dropIdx = m.computeDropIndex(0, 250, bounds);
    REQUIRE(dropIdx == 1);  // clamped to last pinned position + 1 = pinned count
}

TEST_CASE("TabReorderModel: computeDropIndex non-pinned can't enter pinned", "[tabreorder]")
{
    TabReorderModel m;
    m.resize(4);
    m.togglePin(0);  // tab at index 0 is pinned

    std::vector<float> bounds = {0, 100, 200, 300, 400};

    // Try dragging tab 1 (unpinned) into pinned territory (first 100px)
    size_t dropIdx = m.computeDropIndex(1, 50, bounds);
    REQUIRE(dropIdx == 1);  // clamped to first non-pinned position
}

// ===========================================================================
// Apply reorder
// ===========================================================================

TEST_CASE("TabReorderModel: applyReorder adjusts indices", "[tabreorder]")
{
    TabReorderModel m;
    m.resize(4);

    // Move tab 0 to position 2
    size_t newIdx = m.applyReorder(0, 2);
    // After erase at 0, everything shifts down. Insert at adjusted position.
    // Original: [0, 1, 2, 3] → erase 0 → [1, 2, 3] → insert at 1 → [1, 0, 2, 3]
    REQUIRE(newIdx == 1);

    // Tab at original position 0 should now be at position 1
    // After erase, positions shift
}

TEST_CASE("TabReorderModel: applyReorder to front", "[tabreorder]")
{
    TabReorderModel m;
    m.resize(4);

    // Move tab 3 to position 0
    size_t newIdx = m.applyReorder(3, 0);
    REQUIRE(newIdx == 0);
}

TEST_CASE("TabReorderModel: applyReorder same index returns same", "[tabreorder]")
{
    TabReorderModel m;
    m.resize(3);

    size_t newIdx = m.applyReorder(1, 1);
    REQUIRE(newIdx == 1);
}

// ===========================================================================
// Edge cases
// ===========================================================================

TEST_CASE("TabReorderModel: isPinned out of bounds returns false", "[tabreorder]")
{
    TabReorderModel m;
    m.resize(1);
    REQUIRE_FALSE(m.isPinned(5));
}

TEST_CASE("TabReorderModel: applyReorder out of bounds returns fromIndex", "[tabreorder]")
{
    TabReorderModel m;
    m.resize(2);

    size_t result = m.applyReorder(5, 0);
    REQUIRE(result == 5);
}
