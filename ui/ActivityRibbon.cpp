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
    // Background — deepest surface level (surface-container-lowest)
    g.fillAll(juce::Colour(kBgColour));

    // Navigation buttons
    for (const auto& btn : navButtons_)
        paintButton(g, btn);

    // 1 px separator rule
    g.setColour(juce::Colour(kSepColour));
    g.fillRect(separatorBounds_);

    // Settings button
    paintButton(g, settingsButton_);
}

void ActivityRibbon::paintButton(juce::Graphics& g, const RibbonButton& btn) const
{
    const bool isActive = (btn.panel != Panel::None) && (btn.panel == activePanel_);

    // Accent highlight bar for active button
    if (isActive)
    {
        // 3 px left-edge accent bar (VS Code style, green accent from mockup)
        g.setColour(juce::Colour(kAccentColour));
        g.fillRect(juce::Rectangle<int>(0, btn.bounds.getY(), 3, kButtonSize));

        // Subtle tinted background over the entire button
        g.setColour(juce::Colour(kAccentColour).withAlpha(0.15f));
        g.fillRect(btn.bounds);
    }

    // Icon label centred in the 32×32 box
    const juce::Colour textCol = isActive
        ? juce::Colour(kAccentColour)
        : juce::Colour(kTextColour);

    g.setColour(textCol);
    g.setFont(HathorLookAndFeel::fontBold(14.0f));
    g.drawText(btn.label, btn.bounds, juce::Justification::centred, false);
}

// ---------------------------------------------------------------------------
// juce::Component — mouse input
// ---------------------------------------------------------------------------

void ActivityRibbon::mouseDown(const juce::MouseEvent& e)
{
    const juce::Point<int> pos = e.getPosition();

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
