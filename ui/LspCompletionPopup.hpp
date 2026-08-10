// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * LspCompletionPopup.hpp — JUCE popup component for displaying LSP completions.
 *
 * Shows a filtered list of CompletionCandidate items as a popup menu over
 * the editor. Navigation via keyboard (Up/Down/Enter/Escape) and mouse.
 *
 * Requirement references: AI-4, Req 25.1–25.3 (editor language features)
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "LspProtocol.hpp"

#include <functional>
#include <string>
#include <vector>

namespace hathor::ui {

/**
 * LspCompletionPopup
 *
 * A light-weight popup component (no menu manager) that displays a list of
 * completion candidates. It is positioned by the parent editor at the
 * cursor location.
 *
 * Key behaviours:
 *   - Filters candidates by the current prefix as the user types.
 *   - Arrow keys navigate, Enter / Tab / mouse selects.
 *   - Escape or clicking away dismisses.
 *   - Selection fires onSelect callback with the chosen item.
 */
class LspCompletionPopup : public juce::Component,
                             private juce::ListBoxModel
{
public:
    using SelectCallback = std::function<void(const lsp::CompletionCandidate&)>;
    using DismissCallback = std::function<void()>;

    /**
     * Construct the popup.
     * @param onSelect   Called when a candidate is chosen or keyboard-navigated.
     * @onDismiss        Called when the popup is dismissed (Escape / click-away).
     */
    LspCompletionPopup(SelectCallback onSelect,
                       DismissCallback onDismiss);

    ~LspCompletionPopup() override = default;

    // -----------------------------------------------------------------------
    // juce::Component
    // -----------------------------------------------------------------------

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyDown(const juce::KeyPress& key) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    // -----------------------------------------------------------------------
    // juce::ListBoxModel interface
    // -----------------------------------------------------------------------

    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g,
                          int width, int height, bool rowIsSelected) override;
    juce::Component* createSnapshot() override;

    // -----------------------------------------------------------------------
    // Public API — called by HathorTab / HathorLspClient
    // -----------------------------------------------------------------------

    /**
     * Set the full list of completion candidates and show the popup.
     * The list is stored and re-filtered on subsequent filterPrefix() calls.
     */
    void setCandidates(const std::vector<lsp::CompletionCandidate>& candidates);

    /**
     * Append additional candidates (e.g. metadata fallback) to the current list.
     */
    void addCandidates(const std::vector<lsp::CompletionCandidate>& candidates);

    /**
     * Re-filter the stored candidates by a new (or empty) prefix.
     * Repaint the list. If the filter results in zero items and the previous
     * list was non-empty, the popup is dismissed.
     */
    void filterPrefix(std::string_view prefix);

    /** Select the first match (default selection). */
    void selectFirst() noexcept;

    /** Select the last match. */
    void selectLast() noexcept;

    /** Move selection up by one. */
    void selectPrevious() noexcept;

    /** Move selection down by one. */
    void selectNext() noexcept;

    /** Confirm the current selection (Enter / Tab). */
    void confirmSelection();

    /** Dismiss the popup. */
    void dismiss();

    /** True if the popup currently has visible candidates. */
    bool hasCandidates() const noexcept { return !displayedItems_.empty(); }

    /** Return the currently selected candidate (for preview, etc.). */
    const lsp::CompletionCandidate* selectedCandidate() const noexcept;

    /** Number of items currently shown after filtering. */
    int visibleCount() const noexcept { return static_cast<int>(displayedItems_.size()); }

    /** Maximum number of items to show before scrolling. */
    static constexpr int kMaxVisibleRows = 12;

    /** Row height in pixels. */
    static constexpr int kRowHeight = 22;

    /** Popup width in pixels. */
    static constexpr int kPopupWidth = 280;

private:
    /**
     * Recompute displayedItems_ from allItems_ using the current prefix_.
     * Called by setCandidates(), addCandidates(), and filterPrefix().
     */
    void rebuildDisplay();

    /**
     * Case-insensitive prefix match.
     */
    static bool matchesPrefix(std::string_view label, std::string_view prefix) noexcept;

    void updateListBoxSize();

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------

    std::vector<lsp::CompletionCandidate> allItems_;
    std::vector<lsp::CompletionCandidate> displayedItems_;
    std::string                              prefix_;
    int                                      selectedIndex_ = 0;

    SelectCallback    onSelect_;
    DismissCallback   onDismiss_;

    juce::ListBox     listBox_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LspCompletionPopup)
};

} // namespace hathor::ui
