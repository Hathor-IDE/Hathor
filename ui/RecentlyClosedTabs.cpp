// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RecentlyClosedTabs.hpp"

#include <algorithm>

namespace hathor::ui {

void RecentlyClosedTabs::push(TabSnapshot&& snap)
{
    stack_.push_back(std::move(snap));

    // Enforce max history by dropping oldest entries
    while (stack_.size() > maxHistory_)
        stack_.erase(stack_.begin());
}

std::optional<TabSnapshot> RecentlyClosedTabs::pop()
{
    if (stack_.empty())
        return std::nullopt;

    auto snap = std::move(stack_.back());
    stack_.pop_back();
    return snap;
}

std::optional<std::reference_wrapper<const TabSnapshot>> RecentlyClosedTabs::peek() const noexcept
{
    if (stack_.empty())
        return std::nullopt;
    return std::cref(stack_.back());
}

} // namespace hathor::ui
