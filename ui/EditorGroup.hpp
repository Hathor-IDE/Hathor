// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * EditorGroup.hpp — single group of editor tabs (extracted from EditorArea).
 *
 * An EditorGroup owns a set of HathorTab instances and renders them with a
 * custom tab bar that supports:
 *   - Click to switch
 *   - Drag to reorder (within the same group)
 *   - Pin tabs (lock to the left, immovable, non-closable)
 *   - Close button with unsaved-change protection
 *   - Recently-closed tab history (undo via reopen)
 *
 * Layout (top-to-bottom):
 *   ┌──────────────────────────────────┐
 *   │  TabBar (custom-drawn)           │
 *   ├──────────────────────────────────┤
 *   │  Active HathorTab or content     │
 *   └──────────────────────────────────┘
 *
 * This component is used by EditorSplitSurface for multi-group layouts,
 * but can also stand alone (single-group EditorArea replacement).
 *
 * Requirement references: L-1 §1, §2, §3
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

#include "HathorTab.hpp"
#include "TabReorderModel.hpp"
#include "RecentlyClosedTabs.hpp"
#include "../app/AudioEngine.hpp"
#include "../control/ControlInterface.hpp"
#include "hathor/Event.hpp"

namespace hathor::ui {

/**
 * TabBarComponent (enhanced) — custom tab bar with drag, pin, close.
 *
 * Extended from the original to support drag-reorder and pin icons.
 * The JUCE-free TabReorderModel handles drop-index computation.
 */
class EnhancedTabBar : public juce::Component
{
public:
    static constexpr int kTabHeight  = 32;
    static constexpr int kPinIconSize = 12;
    static constexpr int kCloseBoxSize = 14;
    static constexpr int kUnsavedDotRadius = 4;

    EnhancedTabBar();
    ~EnhancedTabBar() override = default;

    // Callbacks
    std::function<void(int)> onTabClicked;        // tab index
    std::function<void(int)> onTabCloseClicked;  // tab index
    std::function<void(int)> onTabPinClicked;    // tab index (toggle pin)
    std::function<void()>    onReorderRequested; // drag ended, reorder tabs (local)
    std::function<void(int, int)> onLocalReorderRequested; // (fromIndex, toIndex) for local reorder
    std::function<void(int)> onTabDragStarted;   // tab drag began (cross-pane)
    std::function<bool(const juce::MouseEvent&)> onTabDragEnded; // drag ended, returns true if consumed

    struct TabDisplayInfo
    {
        juce::String label;
        bool unsavedDot{false};
        bool pinned{false};
    };

    void rebuild(const std::vector<TabDisplayInfo>& tabs,
                 int activeIndex,
                 const TabReorderModel* reorderModel);

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;

private:
    struct TabGeometry
    {
        juce::Rectangle<int> bounds;
        juce::Rectangle<int> closeBtnBounds;
        juce::Rectangle<int> pinBtnBounds;
        juce::String label;
        bool unsavedDot{false};
        bool pinned{false};
    };

    std::vector<TabGeometry> geom_;
    int activeIndex_{ -1 };
    const TabReorderModel* reorderModel_{ nullptr };

    // Drag state
    bool isDragging_{false};
    int  draggedTabIndex_{ -1 };
    juce::Point<int> dragStartPoint_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnhancedTabBar)
};

/**
 * EditorGroup — manages a collection of HathorTab instances with drag/pin/reopen.
 *
 * Delegates tab-management logic to:
 *   - TabReorderModel (JUCE-free) for drag reorder + pin computation
 *   - RecentlyClosedTabs (JUCE-free) for undo-close history
 *
 * The group fires callbacks for:
 *   - onActiveTabChanged(HathorTab*)
 *   - onTabClosedWithUnsavedChanges(TabSnapshot) — for window-level safety
 *   - onFindReplaceRequested()
 *   - onCommandPaletteRequested()
 */
class EditorGroup : public juce::Component
{
public:
    explicit EditorGroup(AudioEngine& audio,
                         hathor::control::ControlInterface& ci);
    ~EditorGroup() override;

    // Non-movable (owns JUCE components)
    EditorGroup(EditorGroup&&) = delete;
    EditorGroup& operator=(EditorGroup&&) = delete;

    // -----------------------------------------------------------------------
    // Tab management (delegated from EditorArea)
    // -----------------------------------------------------------------------

    /** Open a new untitled tab. Returns nullptr if all slots are full. */
    HathorTab* openUntitledTab();

    /** Open a file in a new tab or focus existing tab for same file. */
    HathorTab* openFile(const juce::File& file);

    /** Close the tab at index. Returns true unless cancelled by user. */
    bool closeTab(int index);

    /** Reopen the most recently closed tab. */
    void reopenLastClosedTab();

    /** Number of open tabs. */
    int tabCount() const noexcept { return static_cast<int>(tabs_.size()); }

    /** Currently active tab, or nullptr. */
    HathorTab* activeTab() noexcept;
    const HathorTab* activeTab() const noexcept;

    /** Activate tab at index. */
    void activateTab(int index);

    /** Set the tab at index as pinned or unpinned. */
    void setTabPinned(int index, bool pinned);

    /** True if the tab at index is pinned. */
    bool isTabPinned(int index) const noexcept;

    // -----------------------------------------------------------------------
    // Cross-pane tab transfer (delegated from EditorSplitSurface)
    // -----------------------------------------------------------------------

    /** Remove and return the tab at index (detach from component tree). */
    std::unique_ptr<HathorTab> takeTab(int index);

    /** Insert a tab at index. Returns raw pointer for convenience. */
    HathorTab* insertTab(std::unique_ptr<HathorTab> tab, int index);

    /** Reorder tab from one index to another (local reorder). */
    void reorderTab(int fromIndex, int toIndex);

    // -----------------------------------------------------------------------
    // Drag callbacks (installed by EditorSplitSurface)
    // -----------------------------------------------------------------------
    std::function<void(int)> onTabDragStarted;
    std::function<bool(const juce::MouseEvent&)> onTabDragEnded;

    // -----------------------------------------------------------------------
    // Callbacks (installed by EditorArea or parent)
    // -----------------------------------------------------------------------
    std::function<void(HathorTab*)> onActiveTabChanged;
    std::function<void(const TabSnapshot&)> onTabClosedWithUnsavedChanges;
    std::function<void()> onTabCountChanged;

    // -----------------------------------------------------------------------
    // Editor feature access (delegated to active tab)
    // -----------------------------------------------------------------------
    void handleKeyPress(const juce::KeyPress& key);

    // -----------------------------------------------------------------------
    // Eval helpers (Req 23.1–23.7)
    // -----------------------------------------------------------------------
    // Mini-notation eval routes through ControlInterface::enqueueSetPattern()
    // — the same canonical path used by EditorArea and the console/MCP.
    // .ck eval routes through AudioEngine::ckEval() — the canonical .ck
    // control implementation (no equivalent ControlInterface command exists).
    // -----------------------------------------------------------------------

    /**
     * Dispatch `set-pattern <slotName> <text>` on the worker thread (Req 23.7).
     *
     * @param tab      The source tab (must outlive the lambda).
     * @param slotName AudioEngine slot name string (e.g. "d0").
     * @param text     Mini-notation text to compile.
     */
    void evalOnWorkerThread(HathorTab* tab,
                            const juce::String& slotName,
                            const juce::String& text);

    /**
     * Evaluate ChucK source for a .ck tab via AudioEngine::ckEval().
     *
     * @param tab   The .ck source tab.
     * @param code  Full ChucK source code.
     */
    void evalCkOnWorkerThread(HathorTab* tab, const juce::String& code);

    /**
     * Show a status-bar message for a few seconds, then clear it.
     */
    void showStatus(const juce::String& msg);

    /** Enable editor ergonomics on newly created tabs. */
    void setEditorErgonomicsEnabled(bool enabled) noexcept;

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------
    void resized() override;
    void paint(juce::Graphics& g) override;

    // -----------------------------------------------------------------------
    // LSP / Ghost wiring (delegates to tabs that already have clients installed)
    // -----------------------------------------------------------------------
    void setLspClient(class HathorLspClient* client) noexcept;
    void setGhostClient(class GhostLlmClient* client) noexcept;

    void ghostTick();
    void syncSlotButtonStates();

    void updateNowPlayingHighlight(
        const std::vector<hathor::Event<hathor::ParamMap>>& events);

    juce::Rectangle<int> resolveGlyphBounds(HathorTab& tab,
                                             std::size_t sourceOffset);

private:
    void refreshTabBar();
    void wireTabCallbacks(HathorTab* tab);
    std::vector<EnhancedTabBar::TabDisplayInfo> buildTabDisplayInfos() const;
    std::vector<HathorTab*> buildHathorTabPointers() const;

    // -----------------------------------------------------------------------
    // Child components
    // -----------------------------------------------------------------------
    EnhancedTabBar tabBar_;
    juce::Label statusBar_;

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    std::vector<std::unique_ptr<HathorTab>> tabs_;
    int activeIndex_{ -1 };
    int draggedTabIndex_{ -1 };

    // JUCE-free models
    TabReorderModel reorderModel_;
    RecentlyClosedTabs closedTabsHistory_;

    // References (not owned)
    AudioEngine& audio_;
    hathor::control::ControlInterface& ci_;

    // LSP / Ghost clients (non-owning)
    class HathorLspClient* lspClient_{ nullptr };
    class GhostLlmClient* ghostClient_{ nullptr };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorGroup)
};

} // namespace hathor::ui
