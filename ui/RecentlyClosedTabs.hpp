// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * RecentlyClosedTabs.hpp — JUCE-free model for recently closed tab history.
 *
 * Maintains a bounded stack of tab snapshots (label + content + cursor
 * offset) so users can reopen accidentally closed tabs via Ctrl+Shift+T.
 *
 * Requirement references: L-1 §1 (close protection, reopen closed tabs)
 */

#include <cstddef>
#include <string>
#include <vector>

namespace hathor::ui {

struct TabSnapshot
{
    std::string label;
    std::string fileName;
    std::string content;
    size_t      cursorOffset = 0;
};

class RecentlyClosedTabs
{
public:
    explicit RecentlyClosedTabs(size_t maxHistory = 20) noexcept
        : maxHistory_{maxHistory} {}

    /** Push a tab snapshot onto the stack. */
    void push(TabSnapshot&& snap);

    /** Pop and return the most recently closed tab. */
    std::optional<TabSnapshot> pop();

    /** Peek at the top without removing. */
    std::optional<std::reference_wrapper<const TabSnapshot>> peek() const noexcept;

    /** Number of snapshots available. */
    size_t size() const noexcept { return stack_.size(); }

    /** Clear all history. */
    void clear() noexcept { stack_.clear(); }

    /** True if empty. */
    bool empty() const noexcept { return stack_.empty(); }

private:
    size_t                   maxHistory_;
    std::vector<TabSnapshot> stack_;
};

} // namespace hathor::ui
