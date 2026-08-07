// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ActivityRibbon.hpp — 48 px wide vertical icon ribbon on the far-left edge.
 *
 * Contains icon-only 32×32 px buttons for Explorer, Search, Version Control,
 * and AI Agent, plus a Profile/Settings button pinned at the bottom separated
 * by a 1 px horizontal rule.
 *
 * The active-panel button is highlighted with an accent colour; inactive
 * buttons have no highlight.
 *
 * Requirements: 21.1, 21.2, 21.5, 21.6
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace hathor::ui {

/// Identifies which side panel (if any) is currently active.
enum class Panel
{
    None,
    Explorer,
    Search,
    VersionControl,
    AIAgent
};

/**
 * ActivityRibbon
 *
 * A 48 px wide dark vertical strip holding icon-only navigation buttons.
 * Owners should:
 *   1. Call setActivePanel() to reflect external panel state changes.
 *   2. Install onPanelToggled to react to button clicks.
 */
class ActivityRibbon : public juce::Component
{
public:
    ActivityRibbon();
    ~ActivityRibbon() override = default;

    //==========================================================================
    // Callback — installed by MainWindow.
    // Called on the JUCE message thread when a navigation button is clicked.
    // The panel argument is the panel the button represents; callers toggle
    // open/closed logic themselves and call setActivePanel() with the result.
    std::function<void(Panel)> onPanelToggled;

    //==========================================================================
    // State

    /// Reflect the currently open panel (or Panel::None if all closed).
    /// Triggers a repaint.
    void setActivePanel(Panel p);

    Panel activePanel() const noexcept { return activePanel_; }

    //==========================================================================
    // juce::Component overrides
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    //==========================================================================
    // Colours
    static constexpr juce::uint32 kBgColour     = 0xff252526u; ///< ribbon background
    static constexpr juce::uint32 kAccentColour  = 0xff0078d4u; ///< VS Code blue
    static constexpr juce::uint32 kTextColour    = 0xffccccccu; ///< inactive icon text
    static constexpr juce::uint32 kSepColour     = 0xff3c3c3cu; ///< 1 px rule colour

    //==========================================================================
    // Button geometry constants
    static constexpr int kRibbonWidth   = 48;
    static constexpr int kButtonSize    = 32;
    static constexpr int kTopPadding    = 8;
    static constexpr int kButtonGap     = 4; ///< vertical gap between nav buttons
    static constexpr int kBottomPadding = 8;

    //==========================================================================
    // Internal button description
    struct RibbonButton
    {
        Panel       panel;   ///< which panel this button controls (None for settings)
        juce::String label;  ///< single-char label rendered in the 32×32 box
        juce::Rectangle<int> bounds; ///< set in resized()
    };

    std::array<RibbonButton, 4> navButtons_
    {{
        { Panel::Explorer,       "E", {} },
        { Panel::Search,         "S", {} },
        { Panel::VersionControl, "V", {} },
        { Panel::AIAgent,        "A", {} },
    }};

    RibbonButton settingsButton_ { Panel::None, "P", {} };

    Panel activePanel_ { Panel::None };

    //==========================================================================
    // Layout helpers
    juce::Rectangle<int> separatorBounds_;

    //==========================================================================
    // Input
    void mouseDown(const juce::MouseEvent& e) override;

    //==========================================================================
    // Drawing helpers
    void paintButton(juce::Graphics& g, const RibbonButton& btn) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ActivityRibbon)
};

} // namespace hathor::ui
