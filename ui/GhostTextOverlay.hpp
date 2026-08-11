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
 * AI-G6 (Ghost Text Is UI State, Not Document State):
 *   - The overlay ONLY renders temporary visual state. It does NOT modify the
 *     juce::CodeDocument, never enters the undo history, and is never seen by
 *     compilation or diagnostics.
 *   - setGhostText() stores ghost text + resolved pixel bounds — it does NOT
 *     touch the document.
 *   - clearGhost() / hideGhost() only clear the overlay's internal visual
 *     state; the document is never touched.
 *   - acceptGhost() returns the stored text; the INSERTION into the document
 *     is performed entirely by HathorTab via document_::insertText(), which is
 *     a normal, undoable edit.
 *
 * Rendering approach (AI-G6 rendering constraint):
 *   The overlay follows the same pattern as HighlightOverlay and
 *   DiagnosticsOverlay: it is a child component of HathorTab with identical
 *   bounds to the editor_ (set in HathorTab::resized()). Because the overlay
 *   and editor share the same origin within HathorTab, editor-local
 *   coordinates (from CodeEditorComponent::getCaretRectangleForCharIndex or
 *   getCharacterBounds) map directly to overlay-local coordinates. The
 *   paint() method draws the ghost text at the stored caret pixel position
 *   within its own coordinate space — NO setTopLeftPosition() is used to move
 *   the whole component, which would break alignment with the editor's
 *   scroll offset and button-area Y offset.
 *
 * Requirement references: AI-4, AI-G6
 */

#include <juce_gui_basics/juce_gui_basics.h>

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
 *   - setGhostText(): set the text to display + resolved pixel caret bounds
 *   - clearGhost(): hide the ghost text and clear all internal state
 *   - acceptGhost(): retrieve the text for insertion, then clears
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
      *
      * @param text           The completion text to show at the cursor.
      * @param caretBounds    Resolved pixel rectangle of the caret cursor in
      *                       editor-local coordinates (== overlay-local since
      *                       they share the same bounds in HathorTab::resized).
      *                       The ghost text is drawn starting at caretBounds.getRight().
      * @param insertionLen   Number of characters at the cursor position that
      *                       are already present in the document and are covered
      *                       (dimmed) by the ghost text. This is used to visually
      *                       distinguish the new completion from the existing
      *                       text it extends.
      *
      * This method ONLY stores rendering state. It does NOT modify the
      * CodeDocument, undo history, compiler input, or diagnostics.
      */
    void setGhostText(const std::string& text,
                      const juce::Rectangle<int>& caretBounds,
                      int insertionLen = 0);

    /**
      * Set the J-2 candidate indicator (e.g. "2/3").
      * When count > 1, a small badge is rendered next to the ghost text
      * showing the currently selected candidate index (1-based) out of the
      * total count.
      *
      * @param count  Total number of cached candidates.
      * @param selectedIndex  0-based index of the currently selected candidate.
      */
    void setCandidateIndicator(size_t count, size_t selectedIndex) noexcept;

    /** Clear the candidate indicator (no badge rendered). */
    void clearCandidateIndicator() noexcept;

    /**
     * Clear the ghost text and hide the overlay.
     * This does NOT modify the document, undo history, compiler, or diagnostics.
     * Does NOT create an undo entry.
     */
    void clearGhost() noexcept;

    /**
     * Hide the overlay visually without clearing the stored ghost text.
     * The stored state (text, caretBounds, insertionLen) is preserved so the
     * overlay can be shown again without re-setting.
     */
    void hideGhost() noexcept;

    /**
     * Show the overlay if ghost text is present.
     */
    void showGhost() noexcept;

    /**
     * Accept the ghost text — returns the text to insert, then clears.
     * The caller (HathorTab) is responsible for inserting this text into
     * the document via the normal document edit mechanism (which creates
     * a proper undo entry).
     */
    std::string acceptGhost();

    /** True if ghost text is currently stored (regardless of visibility). */
    bool hasGhost() const noexcept { return !ghostText_.empty(); }

    /** True if the overlay is currently visible (ghost shown). */
    bool isGhostVisible() const noexcept { return visible_; }

    /** Get the current ghost text (for display/preview). */
    std::string getGhostText() const noexcept { return ghostText_; }

    /** Number of characters to dim (existing text covered by the ghost). */
    int insertionLen() const noexcept { return insertionLen_; }

    /** J-2: Current candidate count and selected index for the indicator badge. */
    size_t candidateCount() const noexcept { return candidateCount_; }
    size_t selectedCandidate() const noexcept { return selectedCandidate_; }

private:
    friend class HathorTab;

    std::string ghostText_;
    juce::Rectangle<int> caretBounds_;    ///< caret pixel position (editor-local = overlay-local)
    int insertionLen_       = 0;
    bool visible_          = false;

    /// J-2: candidate count + selected index for the "n/M" indicator badge.
    size_t candidateCount_     = 0;
    size_t selectedCandidate_  = 0;

    juce::Font ghostFont_;
    juce::Colour ghostColour_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GhostTextOverlay)
};

} // namespace hathor::ui
