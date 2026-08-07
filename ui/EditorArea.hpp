// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * EditorArea.hpp — multi-tab code editor region with custom tab bar.
 *
 * Owns:
 *   - std::vector<std::unique_ptr<HathorTab>>   — the tab data / editor widgets
 *   - Custom tab bar (drawn manually) showing label + unsaved-dot indicator
 *   - A juce::Label status bar at the bottom for error messages
 *
 * Key behaviours (requirements 22.2, 22.3, 22.5, 22.6, 22.7, 24.4):
 *   - Tab labels: front-matter `label` → filename stem → "untitled-<slot>"
 *   - Tab switching: immediately swaps visible HathorTab component; the
 *     previously-evaluated slot continues playing (no AudioEngine call here)
 *   - Unsaved dot: small filled circle rendered on tabs with unsavedDot==true
 *   - New untitled buffer: calls nextFreeSlot(); declines + shows status error
 *     if all 16 slots are occupied
 *   - Tab close with unsaved changes: shows Save / Discard / Cancel modal;
 *     Cancel keeps the tab open
 *
 * Requirements: 22.1–22.3, 22.5–22.7, 24.4
 */

// Guard so MainWindow.cpp stub is replaced when this header is included.
#define HATHOR_EDITOR_AREA_DEFINED

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>
#include <optional>
#include <vector>

// App
#include "../app/AudioEngine.hpp"

// Control
#include "../control/ControlInterface.hpp"

// UI
#include "HathorTab.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// nextFreeSlot — free function (Req 22.6, 24.4)
// ---------------------------------------------------------------------------

/**
 * Returns the lowest slot index in [0, AudioEngine::kNumSlots) not currently
 * bound to any open tab, or -1 if all slots are occupied.
 *
 * Req 22.6, 24.4
 */
int nextFreeSlot(const std::vector<HathorTab*>& openTabs) noexcept;

// ---------------------------------------------------------------------------
// TabBarComponent — custom tab bar with unsaved-dot rendering
// ---------------------------------------------------------------------------

/**
 * TabBarComponent
 *
 * Renders the row of tabs above the editor area. Each tab shows:
 *   - The tab label
 *   - A small filled circle (unsaved dot) when unsavedDot == true (Req 22.5)
 *   - A close button (×)
 *
 * Fires callbacks into the owning EditorArea on tab click and close-click.
 */
class TabBarComponent : public juce::Component
{
public:
    static constexpr int kTabHeight     = 32;
    static constexpr int kMinTabWidth   = 80;
    static constexpr int kMaxTabWidth   = 200;
    static constexpr int kCloseBoxSize  = 14;
    static constexpr int kDotRadius     = 4;

    TabBarComponent();
    ~TabBarComponent() override = default;

    // Callbacks installed by EditorArea
    std::function<void(int)> onTabClicked;   ///< argument: tab index
    std::function<void(int)> onTabCloseClicked; ///< argument: tab index

    /// Rebuild tab geometry from the given tab list and repaint.
    void rebuild(const std::vector<std::unique_ptr<HathorTab>>& tabs,
                 int activeIndex);

    // juce::Component
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    struct TabGeometry
    {
        juce::Rectangle<int> bounds;
        juce::Rectangle<int> closeBtnBounds;
        juce::String         label;
        bool                 unsavedDot{ false };
    };

    std::vector<TabGeometry> geom_;
    int activeIndex_{ -1 };

    // Colours
    static constexpr juce::uint32 kBgColour      = 0xff252526u;
    static constexpr juce::uint32 kActiveBg      = 0xff1e1e1eu;
    static constexpr juce::uint32 kInactiveBg    = 0xff2d2d2du;
    static constexpr juce::uint32 kTextColour    = 0xffd4d4d4u;
    static constexpr juce::uint32 kActiveText    = 0xffffffffu;
    static constexpr juce::uint32 kDotColour     = 0xffe8a835u; ///< amber unsaved dot
    static constexpr juce::uint32 kCloseColour   = 0xff858585u;
    static constexpr juce::uint32 kSepColour     = 0xff3c3c3cu;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TabBarComponent)
};

// ---------------------------------------------------------------------------
// EditorArea
// ---------------------------------------------------------------------------

/**
 * EditorArea
 *
 * The main editor region: tab bar + stacked editor content + status bar.
 *
 * Layout (top-to-bottom):
 *   ┌──────────────────────────────────────────┐
 *   │  TabBarComponent (kTabHeight px)         │
 *   ├──────────────────────────────────────────┤
 *   │  Active HathorTab (fills remaining area) │
 *   ├──────────────────────────────────────────┤
 *   │  Status bar label (22 px)                │
 *   └──────────────────────────────────────────┘
 *
 * Requirements: 22.1–22.3, 22.5–22.7, 24.4
 */
class EditorArea : public juce::Component
{
public:
    explicit EditorArea(AudioEngine& audio,
                        hathor::control::ControlInterface& ci);
    ~EditorArea() override;

    // Non-copyable / non-movable
    // JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (at the end of the class)
    // handles copy deletion; delete move explicitly here.
    EditorArea(EditorArea&&)                 = delete;
    EditorArea& operator=(EditorArea&&)      = delete;

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    /**
     * Open a new untitled buffer.
     * Calls nextFreeSlot(); if all 16 slots are taken, shows an error in the
     * status bar and returns false without opening a tab.  (Req 22.6)
     *
     * @return true if the tab was created, false if all slots are occupied.
     */
    bool openUntitledTab();

    /**
     * Open a file in a new tab (or focus an existing tab for the same file).
     * If the file has no slot front-matter, calls nextFreeSlot() (Req 24.4).
     *
     * @param file  The .hathor file to open.
     * @return true if the tab was created/focused, false if slots are full.
     */
    bool openFile(const juce::File& file);

    /**
     * Close the tab at the given index.
     * If the buffer has unsaved changes, shows Save / Discard / Cancel modal
     * (Req 22.7).  Cancel leaves the tab open and returns false.
     *
     * @param index  Index of the tab to close.
     * @return true if the tab was closed, false if Cancel was chosen.
     */
    bool closeTab(int index);

    /// Number of open tabs.
    int tabCount() const noexcept { return static_cast<int>(tabs_.size()); }

    /// The currently active tab, or nullptr if no tabs are open.
    HathorTab* activeTab() noexcept;

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /// Build a flat pointer list for nextFreeSlot().
    std::vector<HathorTab*> tabPointers() const;

    /// Switch to the tab at index.  Does NOT audio-interrupt the old slot
    /// (the old slot continues playing — Req 22.3).
    void activateTab(int index);

    /// Remove tab at index from the vectors and update the tab bar.
    void removeTabAt(int index);

    /// Show a status-bar message for a few seconds, then clear it.
    void showStatus(const juce::String& msg);

    /// Wire up the onUnsavedDotChanged callback for a tab.
    void wireUnsavedCallback(HathorTab& tab);

    /// Rebuild the tab bar geometry and show the active content.
    void refreshTabBar();

    // -----------------------------------------------------------------------
    // Layout constants
    // -----------------------------------------------------------------------
    static constexpr int kStatusBarHeight = 22;
    static constexpr int kTabBarHeight    = TabBarComponent::kTabHeight;

    // -----------------------------------------------------------------------
    // Child components
    // -----------------------------------------------------------------------
    TabBarComponent                          tabBar_;
    juce::Label                              statusBar_;

    // -----------------------------------------------------------------------
    // Tab data
    // -----------------------------------------------------------------------
    std::vector<std::unique_ptr<HathorTab>>  tabs_;
    int                                      activeIndex_{ -1 };

    // -----------------------------------------------------------------------
    // References (not owned)
    // -----------------------------------------------------------------------
    AudioEngine&                       audio_;
    hathor::control::ControlInterface& ci_;

    // Timer for clearing the status bar message
    juce::Timer* statusClearTimer_{ nullptr };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorArea)
};

} // namespace hathor::ui
