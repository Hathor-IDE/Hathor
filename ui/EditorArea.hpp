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
#include "hathor/Event.hpp"
#include "hathor/LanguageMetadata.hpp"

// Control
#include "../control/ControlInterface.hpp"

// UI
#include "HathorTab.hpp"
#include "HathorLookAndFeel.hpp"
#include "SettingsComponent.hpp"
#include "PetdexManifestService.hpp"
#include "BakeOrchestrator.hpp"
#include "EditorContextBridge.hpp"
#include "LspContextBridge.hpp"
#include "GhostLlmClient.hpp"
// L-1: Editor ergonomics
#include "ActionRegistry.hpp"
#include "FindReplaceModel.hpp"
#include "TabReorderModel.hpp"
#include "RecentlyClosedTabs.hpp"
#include "FindReplacePanel.hpp"
#include "CommandPalette.hpp"
#include "BreadcrumbsBar.hpp"
#include "EditorSplitSurface.hpp"
// L-2: Navigation & workspace search
#include "NavigationHistory.hpp"
#include "WorkspaceSearchModel.hpp"
#include "SymbolSearchModel.hpp"
#include "QuickOpenDialog.hpp"
#include "WorkspaceSearchPanel.hpp"
#include "SymbolSearchPanel.hpp"
// L-3: Problems panel
#include "ProblemsPanel.hpp"
#include "control/Diagnostic.hpp"
#include "control/DiagnosticRegistry.hpp"

// L-4: Terminal panel
#include "TerminalPanel.hpp"

// L-5: Git source control panel
#include "SourceControlPanel.hpp"

// L-6: Debug & Runtime Inspector panel
#include "DebugPanel.hpp"

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

/**
 * TabInfo — lightweight descriptor for a single tab in the Tab_Bar.
 * Used by TabBarComponent::rebuild() so the bar doesn't need to know about
 * HathorTab vs SettingsComponent internals.
 */
struct TabInfo
{
    juce::String label;
    bool         unsavedDot{ false };
};

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
    void rebuild(const std::vector<TabInfo>& tabs,
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

    /**
      * Open or focus the Settings tab (A2).
      * If the Settings tab is already open, focuses it; otherwise creates it
      * and activates it.  The caller should install onSettingsApplied on the
      * returned SettingsComponent if it wants to react to Apply.
      *
     * @param props  ApplicationProperties for persistence (passed to SettingsComponent).
     * @return Pointer to the SettingsComponent, or nullptr if the tab could
     *         not be created (e.g. all slots full — never, but kept for symmetry).
     */
    SettingsComponent* openSettingsTab(juce::ApplicationProperties* props);

    /// Number of open tabs.
    int tabCount() const noexcept { return static_cast<int>(tabs_.size()); }

    /// The app-lifetime Petdex manifest service (non-owning; may be null in
    /// stripped-down builds). MainWindow uses it to resolve the selected pet's
    /// manifest entry when a selection is applied (Phase G / D2–D4).
    PetdexManifestService* petdexManifestService() const noexcept
    {
        return petdexService_.get();
    }

    /// The currently active tab, or nullptr if no tabs are open.
    HathorTab* activeTab() noexcept;

    /// Returns true if the Strudel LSP server process is running.
    bool isLspConnected() const noexcept;

    // -----------------------------------------------------------------------
    // AI-8: Editor/LSP context bridges
    // -----------------------------------------------------------------------
    // The MainWindow installs EditorContextBridge and LspContextBridge
    // instances (non-owning pointers) that are refreshed whenever the editor
    // state or LSP diagnostics change.  These bridges implement the JUCE-free
    // abstract interfaces consumed by the control-layer AuthoringContext.
    //
    // Requirement references: AI-8 §2, §9

    /// Install the editor context bridge (non-owning, may be null).
    void setEditorContextBridge(class EditorContextBridge* bridge) noexcept;

    /// Install the LSP context bridge (non-owning, may be null).
    void setLspContextBridge(class LspContextBridge* bridge) noexcept;

    /// Expose the LSP client for context bridge wiring (AI-8).
    class HathorLspClient* lspClient() const noexcept;

    /// Expose the language metadata for AI-8 context assembly (AI-3).
    const hathor::language::LanguageMetadata& metadata() const noexcept;
    const hathor::language::MetadataCompatibility& metadataCompatibility() const noexcept;

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

    // -----------------------------------------------------------------------
    // L-1: Editor ergonomics — find/replace, split, breadcrumbs
    // -----------------------------------------------------------------------

    /// Show the find/replace panel and focus the find field.
    void showFindReplace();

    /// Hide the find/replace panel.
    void hideFindReplace();

    /// Find next match in the active tab.
    void findNextInActiveTab();

    /// Find previous match in the active tab.
    void findPrevInActiveTab();

    /// Replace current match in the active tab.
    void replaceInActiveTab();

    /// Replace all matches in the active tab.
    void replaceAllInActiveTab();

    /// Toggle a vertical split of the active editor group.
    void toggleSplit();

    // -----------------------------------------------------------------------
    // L-2: Navigation & Workspace Search
    // -----------------------------------------------------------------------

    /// Show the quick-open file dialog (Ctrl/Cmd+P).
    void showQuickOpen();

    /// Navigate to the definition of the symbol at the cursor.
    void gotoDefinition();

    /// Navigate to references of the symbol at the cursor.
    void gotoReferences();

    /// Peek at the definition (show in a small overlay, don't navigate).
    void peekDefinition();

    /// Navigate back in history.
    void navigateBack();

    /// Navigate forward in history.
    void navigateForward();

    /// Show the workspace search panel for multi-file text search.
    void showWorkspaceSearch();

    /// Show the symbol search panel.
    void showSymbolSearch();

    /// Show document symbols for the active tab.
    void showDocumentSymbols();

    /// Navigate to a specific diagnostic (from the error panel / next/prev).
    void navigateToNextDiagnostic();
    void navigateToPrevDiagnostic();

    /// Get the navigation history (non-owning).
    NavigationHistory* navigationHistory() noexcept
    {
        return navigationHistory_.get();
    }

    /// Get the workspace search model (non-owning).
    WorkspaceSearchModel* workspaceSearchModel() noexcept
    {
        return workspaceSearchModel_.get();
    }

    /// Get the symbol search model (non-owning).
    SymbolSearchModel* symbolSearchModel() noexcept
    {
        return symbolSearchModel_.get();
    }

    // -----------------------------------------------------------------------
    // L-3: Unified Problems / Diagnostics surface
    // -----------------------------------------------------------------------

    /// The shared diagnostic registry (non-owning to callers).
    hathor::control::DiagnosticRegistry* diagnosticRegistry() noexcept
    {
        return diagnosticRegistry_.get();
    }

    /// Show or hide the Problems panel.
    void showProblemsPanel();

    /// Hide the Problems panel.
    void hideProblemsPanel();

    /// Get the problems panel (non-owning, for MainWindow ribbon wiring).
    ProblemsPanel* problemsPanel() noexcept
    {
        return problemsPanel_.get();
    }

    // -----------------------------------------------------------------------
    // L-4: Simple integrated terminal panel
    // -----------------------------------------------------------------------

    /// Show or hide the terminal panel.
    void showTerminalPanel();
    void hideTerminalPanel();

    /// Get the terminal panel (non-owning, for MainWindow ribbon wiring).
    hathor::ui::TerminalPanel* terminalPanel() noexcept
    {
        return terminalPanel_.get();
    }

    // -----------------------------------------------------------------------
    // L-5: Git source control panel
    // -----------------------------------------------------------------------

    /// Show or hide the source control panel.
    void showSourceControlPanel();
    void hideSourceControlPanel();

    /// Get the source control panel (non-owning, for MainWindow ribbon wiring).
    SourceControlPanel* sourceControlPanel() noexcept
    {
        return sourceControlPanel_.get();
    }

    // -----------------------------------------------------------------------
    // L-6: Debug & Runtime Inspector panel
    // -----------------------------------------------------------------------

    /// Show or hide the Debug & Runtime Inspector panel.
    void showDebugPanel();
    void hideDebugPanel();

    /// Get the debug panel (non-owning, for MainWindow ribbon wiring).
    DebugPanel* debugPanel() noexcept
    {
        return debugPanel_.get();
    }

    /**
     * Set the workspace root directory for search and quick-open.
     * Called by MainWindow after project loading.
     */
    void setWorkspaceRoot(const std::filesystem::path& root);

    /// Get the action registry (non-owning).
    hathor::ui::ActionRegistry* actionRegistry() noexcept
    {
        return actionRegistry_.get();
    }

    /// Get the find/replace panel (non-owning).
    hathor::ui::FindReplacePanel* findReplacePanel() noexcept
    {
        return findReplacePanel_.get();
    }

    /// Get the breadcrumbs bar (non-owning).
    hathor::ui::BreadcrumbsBar* breadcrumbsBar() noexcept
    {
        return breadcrumbsBar_.get();
    }

    /// Get the command palette (non-owning).
    hathor::ui::CommandPalette* commandPalette() noexcept
    {
        return commandPalette_.get();
    }

    /// Register all L-1 editor actions with their key bindings.
    void registerEditorActions();

    /**
        * Sync all tabs' Play/Stop button visuals to the engine's slot state (B1).
        * Called from UITimer at 60 Hz so the UI reflects engine state changes
        * from any path, not only button clicks.
        */
       void syncSlotButtonStates();

       /**
        * Tick all open tabs' ghost-text logic. Called from UITimer at 60 Hz
        * (via onGhostTick callback) so that debounce timers and latency
        * timeouts fire on schedule.
        */
       void ghostTick();

#ifdef HATHOR_ENABLE_GHOST_TELEMETRY
    /**
       * Save ghost completion telemetry from all tabs to a single JSON file (J-6).
       * Called on application shutdown to persist quality metrics across sessions.
       *
       * @param filePath  Path to the telemetry JSON file.
       * @return true if all tabs were serialized successfully.
       */
    bool saveTelemetry(const std::string& filePath) const;

    /**
       * Load ghost completion telemetry and distribute it to all tabs.
       * Called on application startup to restore quality metrics.
       * Merges per-tab telemetry by matching the URI stored in each event.
       *
       * @param filePath  Path to the telemetry JSON file.
       */
    void loadTelemetry(const std::string& filePath);
#endif

     // -----------------------------------------------------------------------
     // C1: Now-playing highlight update path
     // -----------------------------------------------------------------------
     // UITimer calls this once per tick with the latest playback events.
     // It resolves sourceOffset → glyph bounds per-event, routes by slotId
     // to the correct HathorTab, and applies/clears the highlight overlay.
     // Must run on the JUCE message thread only.
     // -----------------------------------------------------------------------
      void updateNowPlayingHighlight(
          const std::vector<hathor::Event<hathor::ParamMap>>& events);

    private:
      /// Resolve a sourceOffset in a tab's document to the pixel rectangle
      /// of the glyph at that position. Returns empty rect if unresolvable.
      juce::Rectangle<int> resolveGlyphBounds(HathorTab& tab,
                                              std::size_t sourceOffset);

    // -----------------------------------------------------------------------
    // B8-K6: Bake to Song — triggered by Ctrl+Shift+B on an active .ck tab
    // -----------------------------------------------------------------------
    /**
     * Initiate a "Bake to Song" operation on the currently active tab.
     * Delegates to BakeOrchestrator which shows the target selection dialog
     * (B8-K1) and drives the background render pipeline (B8-K2→K4, B8-K5).
     *
     * Only .ck tabs can be baked — mini-notation (.hathor) tabs show an
     * error status message.
     */
    void bakeActiveTab();

private:
    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

     /// Switch to the tab at index.  Does NOT audio-interrupt the old slot
     /// (the old slot continues playing — Req 22.3).
     void activateTab(int index);

    /// Remove tab at index from the vectors and update the tab bar.
    void removeTabAt(int index);

    /// Show a status-bar message for a few seconds, then clear it.
    void showStatus(const juce::String& msg);

    /// Wire up the onUnsavedDotChanged callback for a tab.
    void wireUnsavedCallback(HathorTab& tab);

    /// Wire up the onPlayStopClicked callback for a tab (B1).
     /// Wire up the onPlayStopClicked callback for a tab (B1).
    void wirePlayStopCallback(HathorTab& tab);

    /// L-1 §5: Wire context menu callbacks for a tab (find, replace, eval, etc.).
    void wireContextMenuCallbacks(HathorTab& tab);

    /// Build a pointer list of HathorTab only (excludes Settings tab).
    std::vector<HathorTab*> buildHathorTabPointers() const;

    /// Build a TabInfo list for the tab bar (HathorTab entries + optional Settings tab).
    std::vector<TabInfo> tabInfos() const;

     /// Rebuild the tab bar geometry and show the active content.
    void refreshTabBar();

    // -----------------------------------------------------------------------
    // Layout constants
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
      * Dispatch `set-pattern <slotName> <text>` on the worker thread (Req 23.7)
      * ...existing doc...
      *
      * @param tab      The source tab (must outlive the lambda).
      * @param slotName AudioEngine slot name string (e.g. "d0").
      * @param text     Mini-notation text to compile.
      */
     void evalOnWorkerThread(HathorTab* tab,
                             const juce::String& slotName,
                             const juce::String& text);

     // -----------------------------------------------------------------------
     // B4-K7: .ck tab evaluation — compile→load→execute path
     // -----------------------------------------------------------------------

     /**
      * Evaluate ChucK source for a .ck tab via AudioEngine::ckEval().
      * Runs on a detached worker thread so the JUCE message thread is
      * never blocked.  On completion, marshals the result to the message
      * thread to update the tab's eval state and status bar.
      *
      * @param tab   The .ck source tab.
      * @param code  Full ChucK source code (entire buffer for both
      *              Ctrl+Enter and Ctrl+Alt+Enter on .ck tabs).
      */
     void evalCkOnWorkerThread(HathorTab* tab, const juce::String& code);

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

    /// Settings tab (A2) — managed alongside HathorTab tabs.
    /// When active, it occupies the same content area as HathorTab tabs.
    std::unique_ptr<SettingsComponent>       settingsTab_;
    bool                                     settingsActive_{ false };

    /// Petdex manifest service (Phase G / D1) — app-lifetime so the Settings
    /// tab can be opened/closed without losing catalog state. Does no network
    /// work until the Settings tab requests a load (opt-in).
    std::unique_ptr<PetdexManifestService>   petdexService_;

    /// One key-listener per tab (parallel to tabs_); owns the listener objects.
    std::vector<std::unique_ptr<TabKeyListener>> keyListeners_;

    /// References (not owned)
    // -----------------------------------------------------------------------
    AudioEngine&                       audio_;
    hathor::control::ControlInterface& ci_;

    // -----------------------------------------------------------------------
    // B8-K6: Bake-to-Song orchestrator
    // -----------------------------------------------------------------------
    std::unique_ptr<BakeOrchestrator> bakeOrchestrator_;

    // -----------------------------------------------------------------------
    // AI-4: LSP language server integration
    // -----------------------------------------------------------------------
    // The LSP client manages the strudel-lsp-server process and provides
    // completions, hover, and diagnostics for .hathor tabs.
    // -----------------------------------------------------------------------
    std::unique_ptr<HathorLspClient> lspClient_;
     std::unique_ptr<GhostLlmClient> ghostClient_;
     hathor::language::LanguageMetadata metadata_;
    hathor::language::MetadataCompatibility metadataCompat_;

    // AI-8: Non-owning bridges for dynamic context assembly.
    // Refreshed when tabs activate, documents change, or LSP diagnostics
    // are published.  May be null if AI-8 context is not wired up.
    class EditorContextBridge* editorContextBridge_{nullptr};
    class LspContextBridge*    lspContextBridge_{nullptr};

    // Timer for clearing the status bar message
    juce::Timer* statusClearTimer_{ nullptr };

    // -----------------------------------------------------------------------
    // L-1: Editor ergonomics components (non-owning access via accessors)
    // -----------------------------------------------------------------------
    std::unique_ptr<hathor::ui::ActionRegistry>   actionRegistry_;
    std::unique_ptr<hathor::ui::FindReplacePanel> findReplacePanel_;
    std::unique_ptr<hathor::ui::CommandPalette>   commandPalette_;
    std::unique_ptr<hathor::ui::BreadcrumbsBar>   breadcrumbsBar_;
    std::unique_ptr<hathor::ui::EditorSplitSurface> editorSplitSurface_;

    // -----------------------------------------------------------------------
    // L-2: Navigation & workspace search components
    // -----------------------------------------------------------------------
    std::unique_ptr<hathor::ui::NavigationHistory>      navigationHistory_;
    std::unique_ptr<hathor::ui::WorkspaceSearchModel>   workspaceSearchModel_;
    std::unique_ptr<hathor::ui::SymbolSearchModel>      symbolSearchModel_;
    std::unique_ptr<hathor::ui::QuickOpenDialog>        quickOpenDialog_;
    std::unique_ptr<hathor::ui::WorkspaceSearchPanel>   workspaceSearchPanel_;
    std::unique_ptr<hathor::ui::SymbolSearchPanel>      symbolSearchPanel_;

    // -----------------------------------------------------------------------
    // L-3: Unified Problems / Diagnostics surface
    // -----------------------------------------------------------------------
    std::unique_ptr<hathor::control::DiagnosticRegistry> diagnosticRegistry_;
    std::unique_ptr<hathor::ui::ProblemsPanel>           problemsPanel_;

    // L-4: Simple integrated terminal panel (bottom-docked, like ProblemsPanel)
    std::unique_ptr<hathor::ui::TerminalPanel>           terminalPanel_;

    // L-5: Git source control panel (bottom-docked)
    std::unique_ptr<SourceControlPanel>                  sourceControlPanel_;

    // L-6: Debug & Runtime Inspector panel (bottom-docked, two tabs)
    std::unique_ptr<DebugPanel>                          debugPanel_;

    std::filesystem::path workspaceRoot_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorArea)
};

} // namespace hathor::ui
