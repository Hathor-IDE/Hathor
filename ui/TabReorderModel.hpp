// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * TabReorderModel.hpp — JUCE-free model for tab drag-reorder and pinning.
 *
 * Encapsulates the pure-logic portion of tab management that doesn't need
 * JUCE components: determining the valid drop target during a drag, applying
 * a reorder, and tracking which tabs are pinned (and therefore immovable).
 *
 * Requirement references: L-1 §1 (tab rearrangement, pinning)
 */

#include <cstddef>
#include <vector>

namespace hathor::ui {

/**
 * TabReorderModel maintains the ordered list of tab indices and their
 * pinned status.  "Pinned" tabs are locked to the left of the tab bar
 * and cannot be dragged or closed accidentally.
 */
class TabReorderModel
{
public:
    struct TabEntry
    {
        bool pinned = false;
    };

    /** Set the number of tabs (all unpinned initially). */
    void resize(size_t count);

    /** Number of tabs. */
    size_t size() const noexcept { return entries_.size(); }

    /** True if tab at `index` is pinned. */
    bool isPinned(size_t index) const noexcept;

    /**
     * Toggle the pinned flag for a tab.
     * Pinned tabs move to the front (lowest indices) if not already there.
     */
    void togglePin(size_t index);

    /**
     * Compute the drop index when dragging tab `fromIndex` to a position
     * indicated by `mouseX` (pixel coordinate within the tab bar) and the
     * per-tab pixel widths.
     *
     * Pinned tabs cannot be dropped after any non-pinned tab, and vice-versa:
     * a pinned tab dragged into the non-pinned region is clamped to the
     * last pinned position; a non-pinned tab dragged into the pinned region
     * is clamped to the first non-pinned position.
     *
     * Returns the clamped insertion index, or fromIndex if the drag is
     * invalid (e.g. dropping on itself).
     */
    size_t computeDropIndex(size_t fromIndex,
                            float mouseX,
                            const std::vector<float>& tabBoundaries) const;

    /**
     * Apply a reorder: move the tab at `fromIndex` to `toIndex`.
     * Pinned status is preserved.  Returns the new index of the moved tab
     * (may differ from toIndex if clamping occurred).
     */
    size_t applyReorder(size_t fromIndex, size_t toIndex);

private:
    std::vector<TabEntry> entries_;

    /// Count of pinned tabs at the front of the list.
    size_t pinnedCount() const noexcept;
};

} // namespace hathor::ui
