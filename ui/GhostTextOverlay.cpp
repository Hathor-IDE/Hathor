// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * GhostTextOverlay.cpp — implementation of the ghost-text overlay.
 *
 * Renders semi-transparent completion text at the cursor position,
 * following the existing HighlightOverlay / DiagnosticsOverlay pattern.
 *
 * AI-G6 (Ghost Text Is UI State, Not Document State):
 *   - paint() only reads ghostText_ / caretBounds_ / insertionLen_ — it never
 *     writes to juce::CodeDocument. The document model is untouched.
 *   - The overlay shares the same bounds as the editor (set by
 *     HathorTab::resized), so editor-local caret coordinates map directly
 *     to overlay-local coordinates. This is the same mechanism used by
 *     HighlightOverlay — no setTopLeftPosition() repositioning.
 *
 * Requirement references: AI-4, AI-G6
 */

#include "GhostTextOverlay.hpp"
#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GhostTextOverlay::GhostTextOverlay()
{
    setInterceptsMouseClicks(false, false);
    setVisible(false);

    // Default font + colour — updated in paint() from the palette
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    ghostFont_ = HathorLookAndFeel::fontRegular(
        HathorLookAndFeel::Typography::codeDefault);
    ghostColour_ = palette.textSecondary.withAlpha(0.4f);
}

// ---------------------------------------------------------------------------
// juce::Component overrides
// ---------------------------------------------------------------------------

void GhostTextOverlay::resized()
{
    // Size follows the editor content area (set by HathorTab::resized).
    // No internal layout needed — paint() positions the ghost text at
    // the stored caretBounds_ which is in editor-local (== overlay-local)
    // coordinates.
}

void GhostTextOverlay::paint(juce::Graphics& g)
{
    if (!visible_ || ghostText_.empty())
        return;

    // Refresh palette colours on theme switch
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    ghostColour_ = palette.textSecondary.withAlpha(0.4f);

    g.setFont(ghostFont_);
    g.setColour(ghostColour_);

    // Draw the ghost text starting at the caret's right edge, same baseline.
    // caretBounds_ is in editor-local coordinates, which map directly to
    // overlay-local coordinates because both share the same bounds in
    // HathorTab::resized (both at origin (0, editorTop) within HathorTab
    // with identical width/height).
    const int startX = caretBounds_.getRight();
    const int startY = caretBounds_.getY();
    const int rowHeight = caretBounds_.getHeight();

    if (rowHeight <= 0)
        return;

    // -----------------------------------------------------------------------
    // AI-G6: Render the ghost text, dimming characters that overlap with
    // already-present document text (insertionLen).
    // -----------------------------------------------------------------------

    juce::String ghostStr(ghostText_);
    const juce::Font& font = ghostFont_;
    int textEndX = startX;

    if (insertionLen_ > 0 && insertionLen_ < static_cast<int>(ghostText_.length()))
    {
        juce::String dimmedPart = ghostStr.substring(0, insertionLen_);
        juce::String normalPart = ghostStr.substring(insertionLen_);

        // Dimmed part — lower opacity to indicate it's existing text
        g.setColour(ghostColour_.withAlpha(0.2f));
        float dimmedWidth = font.getStringWidthFloat(dimmedPart);
        g.drawText(dimmedPart,
                   startX, startY,
                   static_cast<int>(dimmedWidth), rowHeight,
                   juce::Justification::topLeft,
                   false);
        textEndX = startX + static_cast<int>(dimmedWidth);

        // Normal part — standard ghost opacity
        g.setColour(ghostColour_);
        float normalWidth = font.getStringWidthFloat(normalPart);
        g.drawText(normalPart,
                   textEndX, startY,
                   static_cast<int>(normalWidth), rowHeight,
                   juce::Justification::top_left,
                   false);
        textEndX += static_cast<int>(normalWidth);
    }
    else
    {
        // No dimming needed — draw the entire ghost text at standard opacity
        float fullWidth = font.getStringWidthFloat(ghostStr);
        g.drawText(ghostStr,
                   startX, startY,
                   static_cast<int>(fullWidth), rowHeight,
                   juce::Justification::top_left,
                   false);
        textEndX = startX + static_cast<int>(fullWidth);
    }

    // -----------------------------------------------------------------------
    // J-2: Draw candidate indicator badge (e.g. "2/3") after the ghost text
    // -----------------------------------------------------------------------
    if (candidateCount_ > 1)
    {
        juce::String badge = juce::String(selectedCandidate_ + 1) + "/" + juce::String(candidateCount_);
        float badgeWidth = font.getStringWidthFloat(badge);
        g.setColour(ghostColour_.withAlpha(0.6f));
        g.drawText(badge,
                   textEndX + 4, startY,
                   static_cast<int>(badgeWidth), rowHeight,
                   juce::Justification::top_left,
                   false);
    }
}
    else
    {
        // No dimming needed — draw the entire ghost text at standard opacity
        g.drawText(ghostStr,
                   startX, startY,
                   ghostStr.length() * 8, rowHeight,
                   juce::Justification::topLeft,
                   false);
    }
}

    // -----------------------------------------------------------------------
    // Ghost text management
    // -----------------------------------------------------------------------

    void GhostTextOverlay::setGhostText(const std::string& text,
                                         const juce::Rectangle<int>& caretBounds,
                                         int insertionLen)
    {
        ghostText_ = text;
        caretBounds_ = caretBounds;
        insertionLen_ = insertionLen;
        visible_ = !text.empty();
        setVisible(visible_);
        if (visible_)
            repaint();
    }

    void GhostTextOverlay::setCandidateIndicator(size_t count, size_t selectedIndex) noexcept
    {
        candidateCount_ = count;
        selectedCandidate_ = selectedIndex;
        if (visible_)
        {
            setVisible(true);
            repaint();
        }
    }

    void GhostTextOverlay::clearCandidateIndicator() noexcept
    {
        candidateCount_ = 0;
        selectedCandidate_ = 0;
        if (visible_)
            repaint();
    }

void GhostTextOverlay::clearGhost() noexcept
{
    if (visible_ || !ghostText_.empty() || candidateCount_ > 0)
    {
        visible_ = false;
        ghostText_.clear();
        caretBounds_ = {};
        insertionLen_ = 0;
        candidateCount_ = 0;
        selectedCandidate_ = 0;
        setVisible(false);
        repaint();
    }
}

void GhostTextOverlay::hideGhost() noexcept
{
    if (visible_)
    {
        visible_ = false;
        setVisible(false);
        repaint();
    }
}

void GhostTextOverlay::showGhost() noexcept
{
    if (!ghostText_.empty() && !visible_)
    {
        visible_ = true;
        setVisible(true);
        repaint();
    }
}

std::string GhostTextOverlay::acceptGhost()
{
    std::string text = ghostText_;
    clearGhost();
    return text;
}

} // namespace hathor::ui
