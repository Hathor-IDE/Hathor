// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * GhostTextOverlay.cpp — implementation of the ghost-text overlay.
 *
 * Renders semi-transparent completion text at the cursor position,
 * matching the existing DiagnosticsOverlay/HighlightOverlay pattern.
 *
 * Requirement references: AI-4
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
    // No internal layout needed — paint() positions the ghost text.
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

    // The parent (HathorTab) will set our bounds to cover the editor area.
    // We need the CodeEditorComponent to map (cursorLine, cursorChar) to
    // pixel coordinates. Since we're a sibling of the editor in HathorTab,
    // we need the editor to provide us with the pixel position.
    //
    // Actually, the parent (HathorTab) calls setGhostText with the
    // pixel coordinates already resolved via CodeEditorComponent.
    // The cursorScreenPos_ field stores the resolved pixel position.
    //
    // But wait — we don't have cursorScreenPos_. Let me use a different
    // approach: the parent will call setBounds on us and we'll compute
    // the position from cursorLine/cursorChar using the editor's metrics.
    //
    // Since we don't have a reference to the editor, the parent (HathorTab)
    // will set our position via setTopLeftPosition() before calling repaint().
    // We just need to paint at offset (0, 0) relative to ourselves.

    // Paint the ghost text at (0, 0) — the parent positions us at the cursor.
    g.drawText(ghostText_,
               0, 0,
               ghostText_.empty() ? 1 : static_cast<int>(ghostText_.length()) * 8,
               ghostFont_.getHeight(),
               juce::Justification::topLeft,
               false);
}

// ---------------------------------------------------------------------------
// Ghost text management
// ---------------------------------------------------------------------------

void GhostTextOverlay::setGhostText(const std::string& text,
                                     int cursorLine,
                                     int cursorChar,
                                     int insertionLen)
{
    ghostText_ = text;
    cursorLine_ = cursorLine;
    cursorChar_ = cursorChar;
    insertionLen_ = insertionLen;
    visible_ = !text.empty();
    setVisible(visible_);
    repaint();
}

void GhostTextOverlay::clearGhost() noexcept
{
    if (visible_)
    {
        visible_ = false;
        ghostText_.clear();
        setVisible(false);
        repaint();
    }
}

void GhostTextOverlay::hideGhost() noexcept
{
    if (isVisible())
    {
        setVisible(false);
        repaint();
    }
}

void GhostTextOverlay::showGhost() noexcept
{
    if (!ghostText_.empty() && !isVisible())
    {
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
