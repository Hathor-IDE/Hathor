// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TabReorderModel.hpp"

#include <algorithm>

namespace hathor::ui {

void TabReorderModel::resize(size_t count)
{
    entries_.resize(count);
}

bool TabReorderModel::isPinned(size_t index) const noexcept
{
    return index < entries_.size() && entries_[index].pinned;
}

size_t TabReorderModel::pinnedCount() const noexcept
{
    size_t count = 0;
    for (const auto& e : entries_)
    {
        if (e.pinned)
            ++count;
        else
            break;
    }
    return count;
}

void TabReorderModel::togglePin(size_t index)
{
    if (index >= entries_.size())
        return;
    entries_[index].pinned = !entries_[index].pinned;
}

size_t TabReorderModel::computeDropIndex(size_t fromIndex,
                                          float mouseX,
                                          const std::vector<float>& tabBoundaries) const
{
    if (fromIndex >= entries_.size() || tabBoundaries.empty())
        return fromIndex;

    // Find which tab the mouse is hovering over.
    size_t hoverTab = 0;
    for (size_t i = 0; i < tabBoundaries.size() - 1; ++i)
    {
        if (mouseX >= tabBoundaries[i] && mouseX < tabBoundaries[i + 1])
        {
            hoverTab = i;
            break;
        }
        if (mouseX < tabBoundaries[i])
            break;
        hoverTab = i;
    }

    // Decide whether the dragged tab is pinned
    bool draggingPinned = entries_[fromIndex].pinned;
    size_t pinnedCnt = pinnedCount();

    // Clamp: a pinned tab cannot go past the pinned region boundary.
    if (draggingPinned && hoverTab >= pinnedCnt)
        hoverTab = pinnedCnt;

    // A non-pinned tab cannot be dragged into the pinned region.
    if (!draggingPinned && hoverTab < pinnedCnt)
        hoverTab = pinnedCnt;

    // Don't drop on itself
    if (hoverTab == fromIndex)
        return fromIndex;

    // Insertion index is hoverTab (insert before that tab)
    return hoverTab;
}

size_t TabReorderModel::applyReorder(size_t fromIndex, size_t toIndex)
{
    if (fromIndex >= entries_.size() || toIndex >= entries_.size())
        return fromIndex;
    if (fromIndex == toIndex)
        return fromIndex;

    auto entry = entries_[fromIndex];
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(fromIndex));

    // Adjust toIndex after erase
    if (toIndex > fromIndex)
        --toIndex;

    entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(toIndex), entry);

    return toIndex;
}

} // namespace hathor::ui
