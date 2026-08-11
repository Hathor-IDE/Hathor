// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * GhostTextOverlay.hpp — JUCE overlay component for ghost-text display.
 *
 * Renders semi-transparent "ghost" completion text overlaid on the editor,
 * following the existing DiagnosticsOverlay/HighlightOverlay pattern.
 * The overlay is a child of HathorTab, positioned to cover the editor area,
 * with mouse events intercepted (false, false) so the underlying editor
 * receives all input.
 *
 * Ghost text is rendered at the cursor position, appearing as a natural
 * continuation of the current line. It uses the editor's font and
 * palette-derived muted colour (e.g. 40% opacity textSecondary).
 *
 * Requirement references: AI-4
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "LspProtocol.hpp"

#include <string>

namespace hathor::ui {

/**
 * GhostTextOverlay
 *
 * A lightweight juce::Component that paints ghost completion text at the
 * cursor position. It does NOT handle input — the parent editor (HathorTab)
 * handles keystrokes; the overlay only renders.
 *
 * Usage:
 *   - setGhostText(): set the text to display (cleared on any edit/cursor move)
 *   - setCursorPosition(): update the cursor position so text is re-laid-out
 *   - clearGhost(): hide the ghost text
 *   - acceptGhost(): retrieve the text before clearing (for insertion)
 */
class GhostTextOverlay : public juce::Component
{
public:
    GhostTextOverlay();
    ~GhostTextOverlay() override = default;

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------

    void paint(juce::Graphics& g) override;
    void resized() override;

    // -----------------------------------------------------------------------
    // Ghost text management
    // -----------------------------------------------------------------------

    /**
     * Set the ghost completion text to display.
     * @param text  The completion text (appears after the cursor on the current line).
     * @param cursorLine  0-based line number in the document.
     * @param cursorChar  0-based character offset on the line.
     * @param insertionLen  Number of characters at the cursor position already
     *                      present in the document that the ghost text extends.
     *                      This is used to dim the ghost text that matches
     *                      the existing text (so the user sees only the new part).
     */
    void setGhostText(const std::string& text,
                      int cursorLine,
                      int cursorChar,
                      int insertionLen = 0);

    /** Clear the ghost text. */
    void clearGhost() noexcept;

    /** Hide the overlay component visually without clearing the stored ghost text. */
    void hideGhost() noexcept;

    /** Show the overlay component if ghost text is present. */
    void showGhost() noexcept;

    /** Accept the ghost text — returns the text to insert, then clears. */
    std::string acceptGhost();

    /** True if ghost text is currently visible. */
    bool hasGhost() const noexcept { return !ghostText_.empty(); }

    /** Get the current ghost text (for display/preview). */
    std::string getGhostText() const noexcept { return ghostText_; }

    /** Number of characters to dim (existing text covered by the ghost). */
    int insertionLen() const noexcept { return insertionLen_; }

private:
    friend class HathorTab;

    std::string ghostText_;
    int         cursorLine_    = 0;
    int         cursorChar_    = 0;
    int         insertionLen_  = 0;
    bool        visible_       = false;

    juce::Font ghostFont_;
    juce::Colour ghostColour_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GhostTextOverlay)
};

} // namespace hathor::ui
