// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * NavigationHistory.cpp — implementation of NavigationHistory.
 *
 * JUCE-free, unit-testable in hathor-ui-tests.
 *
 * Requirement references: L-2 §2
 */

#include "NavigationHistory.hpp"

#include <algorithm>

namespace hathor::ui {

NavigationHistory::NavigationHistory(std::size_t maxSize)
    : maxSize_(maxSize)
{
}

bool NavigationHistory::canGoBack() const noexcept
{
    return !backStack_.empty();
}

bool NavigationHistory::canGoForward() const noexcept
{
    return !forwardStack_.empty();
}

void NavigationHistory::navigateTo(const NavigationEntry& entry)
{
    if (current_.has_value())
    {
        backStack_.push_back(*current_);
        if (backStack_.size() > maxSize_)
            backStack_.erase(backStack_.begin());
    }

    current_ = entry;
    forwardStack_.clear();
}

void NavigationHistory::setCurrent(const NavigationEntry& entry)
{
    current_ = entry;
}

std::optional<NavigationEntry> NavigationHistory::goBack()
{
    if (backStack_.empty())
        return std::nullopt;

    if (current_.has_value())
        forwardStack_.push_back(*current_);

    current_ = backStack_.back();
    backStack_.pop_back();

    return current_;
}

std::optional<NavigationEntry> NavigationHistory::goForward()
{
    if (forwardStack_.empty())
        return std::nullopt;

    if (current_.has_value())
        backStack_.push_back(*current_);

    current_ = forwardStack_.back();
    forwardStack_.pop_back();

    return current_;
}

std::optional<NavigationEntry> NavigationHistory::current() const noexcept
{
    return current_;
}

void NavigationHistory::clear() noexcept
{
    backStack_.clear();
    forwardStack_.clear();
    current_.reset();
}

std::size_t NavigationHistory::backCount() const noexcept
{
    return backStack_.size();
}

std::size_t NavigationHistory::forwardCount() const noexcept
{
    return forwardStack_.size();
}

} // namespace hathor::ui
