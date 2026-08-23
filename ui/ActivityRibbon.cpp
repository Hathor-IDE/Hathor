// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ActivityRibbon.cpp — implementation of the 48 px activity ribbon.
 *
 * Requirements: 21.1, 21.2, 21.5, 21.6
 */

#include "ActivityRibbon.hpp"
#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ActivityRibbon::ActivityRibbon()
{
    setSize(kRibbonWidth, 400); // default height; resized() will correct it
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void ActivityRibbon::setActivePanel(Panel p)
{
    if (activePanel_ != p)
    {
        activePanel_ = p;
        repaint();
    }
}

// ---------------------------------------------------------------------------
// juce::Component — layout
// ---------------------------------------------------------------------------

void ActivityRibbon::resized()
{
    const int cx = (getWidth() - kButtonSize) / 2; // centre button horizontally

    // Lay out the four navigation buttons from the top
    int y = kTopPadding;
    for (auto& btn : navButtons_)
    {
        btn.bounds = { cx, y, kButtonSize, kButtonSize };
        y += kButtonSize + kButtonGap;
    }

    // Settings/Profile button pinned at the bottom
    const int settingsY = getHeight() - kBottomPadding - kButtonSize;
    settingsButton_.bounds = { cx, settingsY, kButtonSize, kButtonSize };

    // 1 px horizontal rule just above the settings button
    separatorBounds_ = { 0, settingsY - kButtonGap - 1, getWidth(), 1 };
}

// ---------------------------------------------------------------------------
// juce::Component — painting
// ---------------------------------------------------------------------------

void ActivityRibbon::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Background — deepest surface level (surface-container-lowest)
    g.fillAll(palette.background);

    // Navigation buttons
    for (const auto& btn : navButtons_)
        paintButton(g, btn);

    // 1 px separator rule
    g.setColour(palette.surfaceHighest);
    g.fillRect(separatorBounds_);

    // Settings button
    paintButton(g, settingsButton_);
}

void ActivityRibbon::paintButton(juce::Graphics& g, const RibbonButton& btn) const
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    const bool isActive = (btn.panel != Panel::None) && (btn.panel == activePanel_);

    // Accent highlight bar for active button
    if (isActive)
    {
        // 3 px left-edge accent bar (VS Code style, green accent from mockup)
        g.setColour(palette.accent);
        g.fillRect(juce::Rectangle<int>(0, btn.bounds.getY(), 3, kButtonSize));

        // Subtle tinted background over the entire button
        g.setColour(palette.accent.withAlpha(0.15f));
        g.fillRect(btn.bounds);
    }

    // Icon centred in the 32×32 box (tinted from theme, Agent 0.6)
    const juce::Colour iconCol = isActive
        ? palette.accent
        : palette.textSecondary;

    constexpr int kIconSize = 18;
    IconLibrary::drawIcon(g, btn.icon,
                          btn.bounds.withSizeKeepingCentre(kIconSize, kIconSize).toFloat(),
                          iconCol);
}

// ---------------------------------------------------------------------------
// juce::Component — mouse input
// ---------------------------------------------------------------------------

void ActivityRibbon::mouseDown(const juce::MouseEvent& e)
{
    const juce::Point<int> pos = e.getPosition();

    // 0.2: right-click anywhere on the ribbon opens the workspace
    // context menu (owned by MainWindow via onContextMenu).
    if (e.mods.isRightButtonDown())
    {
        if (onContextMenu)
            onContextMenu(pos);
        return;
    }

    // Check navigation buttons
    for (const auto& btn : navButtons_)
    {
        if (btn.bounds.contains(pos))
        {
            if (onPanelToggled)
                onPanelToggled(btn.panel);
            return;
        }
    }

    // Settings button — treated as Panel::None toggle (no panel opens)
    if (settingsButton_.bounds.contains(pos))
    {
        if (onPanelToggled)
            onPanelToggled(Panel::None);
    }
}

} // namespace hathor::ui
