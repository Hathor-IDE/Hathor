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
 * Key behaviours (requirements 22.2, 22.3, 22.5, 22.6, 22.7, 23.1–23.7, 24.4):
 *   - Tab labels: front-matter `label` → filename stem → "untitled-<slot>"
 *   - Tab switching: immediately swaps visible HathorTab component; the
 *     previously-evaluated slot continues playing (no AudioEngine call here)
 *   - Unsaved dot: small filled circle rendered on tabs with unsavedDot==true
 *   - New untitled buffer: calls nextFreeSlot(); declines + shows status error
 *     if all 16 slots are occupied
 *   - Tab close with unsaved changes: shows Save / Discard / Cancel modal;
 *     Cancel keeps the tab open
 *   - Ctrl+Enter: evaluate Eval_Block on worker thread (Req 23.1, 23.2)
 *   - Ctrl+Alt+Enter: evaluate entire buffer on worker thread (Req 23.3)
 *
 * Requirements: 22.1–22.3, 22.5–22.7, 23.1–23.7, 24.4
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
#include "HathorLookAndFeel.hpp"

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

    // Colours — sourced from HathorLookAndFeel design tokens
    static constexpr juce::uint32 kBgColour      = HathorLookAndFeel::Colours::background;
    static constexpr juce::uint32 kActiveBg      = HathorLookAndFeel::Colours::surface;
    static constexpr juce::uint32 kInactiveBg    = HathorLookAndFeel::Colours::background;
    static constexpr juce::uint32 kTextColour    = HathorLookAndFeel::Colours::textSecondary;
    static constexpr juce::uint32 kActiveText    = HathorLookAndFeel::Colours::textPrimary;
    static constexpr juce::uint32 kDotColour     = HathorLookAndFeel::Colours::warning; ///< amber unsaved dot
    static constexpr juce::uint32 kCloseColour   = HathorLookAndFeel::Colours::textSecondary;
    static constexpr juce::uint32 kSepColour     = HathorLookAndFeel::Colours::surfaceHighest;

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

    // -----------------------------------------------------------------------
    // Key handler — routes Ctrl+Enter and Ctrl+Alt+Enter (Req 23.1–23.7)
    // -----------------------------------------------------------------------
    /**
     * Handle a key press for the active editor.
     *
     * Called by the active HathorTab's CodeEditorComponent via a custom
     * KeyListener installed at construction. This component intercepts
     * Ctrl+Enter and Ctrl+Alt+Enter before JUCE's default editor handling.
     *
     * Returns true if the key was consumed (eval triggered or blank-line
     * warning shown), false otherwise.
     *
     * Req 23.6: no other keystroke triggers pattern evaluation.
     */
    bool handleKeyPress(const juce::KeyPress& key, HathorTab* tab);

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
    // Eval helpers (Req 23.1–23.7)
    // -----------------------------------------------------------------------

    /**
     * Extract the Eval_Block — maximal contiguous run of non-blank lines
     * containing the cursor's line — and return it as a single string.
     * Returns nullopt if the cursor is on a blank line (Req 23.2).
     */
    static std::optional<juce::String> extractEvalBlock(
        const juce::CodeDocument& doc,
        int cursorLine) noexcept;

    /**
     * Dispatch `set-pattern <slotName> <text>` on the worker thread.
     * On completion, if ok==true clears the tab's unsaved dot and repaints
     * the tab bar; if ok==false shows the error field in the status bar.
     * (Req 23.4, 23.5, 23.7)
     *
     * @param tab      The source tab (must outlive the lambda — it is
     *                 ref-counted via weak ownership check on JUCE message
     *                 thread via callAsync).
     * @param slotName AudioEngine slot name string (e.g. "d0").
     * @param text     Mini-notation text to compile.
     */
    void evalOnWorkerThread(HathorTab* tab,
                            const juce::String& slotName,
                            const juce::String& text);

    // -----------------------------------------------------------------------
    // Per-tab KeyListener — bridges CodeEditorComponent key events into
    // EditorArea::handleKeyPress (Req 23.1–23.7)
    // -----------------------------------------------------------------------

    /**
     * TabKeyListener
     *
     * Installed on a HathorTab's CodeEditorComponent so that Ctrl+Enter /
     * Ctrl+Alt+Enter are intercepted before the editor's built-in handling.
     *
     * Forwards only to EditorArea::handleKeyPress(); all other keys return
     * false immediately (Req 23.6).
     */
    class TabKeyListener : public juce::KeyListener
    {
    public:
        TabKeyListener(EditorArea& owner, HathorTab* tab)
            : owner_(owner), tab_(tab) {}

        bool keyPressed(const juce::KeyPress& key,
                        juce::Component* /*source*/) override
        {
            return owner_.handleKeyPress(key, tab_);
        }

        bool keyStateChanged(bool /*isKeyDown*/,
                             juce::Component* /*source*/) override
        {
            return false;
        }

    private:
        EditorArea& owner_;
        HathorTab*  tab_;
    };

    /// Install a TabKeyListener on a newly created tab's editor.
    void installKeyListenerForTab(HathorTab& tab);

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

    /// One key-listener per tab (parallel to tabs_); owns the listener objects.
    std::vector<std::unique_ptr<TabKeyListener>> keyListeners_;

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
