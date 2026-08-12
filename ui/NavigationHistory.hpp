// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * NavigationHistory.hpp — JUCE-free navigation history for the editor.
 *
 * Maintains a stack of visited file positions (file URI + line + column)
 * with back/forward navigation semantics. This is purely a data structure;
 * the JUCE-dependent EditorArea or UI panels call into it and then perform
 * the actual file open + cursor move.
 *
 * Requirement references: L-2 §2
 */

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace hathor::ui {

/**
 * A single entry in the navigation history.
 */
struct NavigationEntry {
    std::string uri;          ///< file:// URI of the source file
    int             line     = 0;     ///< 0-based line number
    int             column   = 0;     ///< 0-based column (UTF-16 code units)
};

/**
 * NavigationHistory
 *
 * A bidirectional history stack. When the user navigates (goto definition,
 * goto reference, etc.), the current position is pushed onto the back stack
 * and the new position becomes the current entry. `goBack` pops from the
 * back stack to the forward stack, and `goForward` does the reverse.
 *
 * The history is bounded to avoid unbounded growth (default 100 entries).
 */
class NavigationHistory
{
public:
    explicit NavigationHistory(std::size_t maxSize = 100);

    /** True if there is a previous entry to navigate back to. */
    bool canGoBack() const noexcept;

    /** True if there is a forward entry to navigate forward to. */
    bool canGoForward() const noexcept;

    /**
     * Record the current position and navigate to a new position.
     * The current position (if any) is saved to the back stack, and the
     * new position becomes the current entry. The forward stack is cleared.
     */
    void navigateTo(const NavigationEntry& entry);

    /**
     * Navigate to a new position without affecting the back stack.
     * Used for the initial navigation or when the user explicitly requests
     * a position (e.g. via quick-open) without wanting back-history.
     */
    void setCurrent(const NavigationEntry& entry);

    /**
     * Move back in history. Returns the entry to navigate to, or
     * std::nullopt if there is nothing to go back to.
     */
    std::optional<NavigationEntry> goBack();

    /**
     * Move forward in history. Returns the entry to navigate to, or
     * std::nullopt if there is nothing to go forward to.
     */
    std::optional<NavigationEntry> goForward();

    /** The current navigation entry, if any. */
    std::optional<NavigationEntry> current() const noexcept;

    /** Clear all history. */
    void clear() noexcept;

    /** Number of entries in the back stack. */
    std::size_t backCount() const noexcept;

    /** Number of entries in the forward stack. */
    std::size_t forwardCount() const noexcept;

private:
    std::vector<NavigationEntry> backStack_;
    std::vector<NavigationEntry> forwardStack_;
    std::optional<NavigationEntry> current_;
    std::size_t maxSize_;
};

} // namespace hathor::ui
