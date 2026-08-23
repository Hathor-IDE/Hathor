// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * MainWindow.hpp — top-level JUCE DocumentWindow with four-zone IDE layout.
 *
 * Layout (Req 20.1, 20.3):
 *   ┌────────────────────────────────────────────────────────────┐
 *   │ ActivityRibbon (48 px) │    EditorArea     │ ChatSidebar  │
 *   │                        │                   │   (320 px)   │
 *   │                        │───────────────────┤              │
 *   │                        │  VisualizerPanel  │              │
 *   │                        │  (max(h/4,120)px) │              │
 *   └────────────────────────────────────────────────────────────┘
 *
 * Dark theme enforced by HathorLookAndFeel (Req 20.2):
 *   - Background luminance ≤ 15% of white  (#0e0e0e ≈ 0.2%)
 *   - All text contrast ratio ≥ 4.5:1 WCAG AA (#e5e2e1 on #0e0e0e ≈ 12.6:1)
 *
 * Window management (Req 20.5):
 *   - Minimum size 1024×768 (setResizeLimits)
 *   - Restores last bounds from juce::PropertiesFile ("windowBounds" key)
 *   - Falls back to centred 1024×768 if stored bounds are off-screen
 *
 * UITimer (Req 28.5): started at 60 Hz in constructor after audio device opens.
 *
 * Requirements: 20.1, 20.2, 20.3, 20.4, 20.5
 */

// JUCE
#include <juce_gui_basics/juce_gui_basics.h>

// App
#include "../app/AudioEngine.hpp"

// Control
#include "../control/ControlInterface.hpp"

// Design system — single source of truth for colours, fonts, spacing (Req 20.2, 5)
#include "HathorLookAndFeel.hpp"

// ---------------------------------------------------------------------------
// Child component headers (all implemented — included directly here or in .cpp).
// Forward declarations for pointer-only types follow below.
// ---------------------------------------------------------------------------
#include "ActivityRibbon.hpp"   // task 3.2 — implemented
#include "ExplorerPanel.hpp"    // task 3.2 — implemented

// Task 3.4: EditorArea is now implemented — include the real header.
#include "EditorArea.hpp"

// Task 3.9: SliderPanel is now implemented — include the real header.
#include "SliderPanel.hpp"

// L-3: StatusRibbon for unified diagnostic/status display
#include "StatusRibbon.hpp"

// Phase G — Petdex mascot (D2–D4): sprite acquisition + D4-gated display
#include "PetWidget.hpp"
#include "PetdexResourceService.hpp"

// ChatSidebar and AcpAgentSession are now fully implemented (task 5.1).
#include "ChatSidebar.hpp"
#include "AcpAgentSession.hpp"

// AI-8: Context bridges
#include "EditorContextBridge.hpp"
#include "LspContextBridge.hpp"

// L-1: Workspace session persistence
#include "WorkspaceSession.hpp"

// ---------------------------------------------------------------------------
// Forward declarations — concrete types defined in their own headers,
// included only in MainWindow.cpp.  Forward-declared here because MainWindow's
// own interface only holds pointers/references to them.
// ---------------------------------------------------------------------------
namespace hathor::ui {

class VisualizerPanel;
class UITimer;
class WelcomeScreen;

} // namespace hathor::ui

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------

class MainWindow : public juce::DocumentWindow
{
public:
    /**
     * Construct the main application window.
     *
     * @param audio          Reference to the fully-initialised AudioEngine (device
     *                       must already be open so UITimer can start immediately).
     * @param ci             Reference to the ControlInterface for dispatching commands.
     * @param agentExePath   Absolute path to the ACP agent executable (empty = no agent).
     * @param hathorMcpPath  Absolute path to the hathor-mcp sidecar (empty = no MCP).
     *
     * Requirements: 32.1
     */
    MainWindow(AudioEngine& audio,
               hathor::control::ControlInterface& ci,
               std::string agentExePath  = {},
               std::string hathorMcpPath = {});

    ~MainWindow() override;

    // -----------------------------------------------------------------------
    // juce::DocumentWindow overrides
    // -----------------------------------------------------------------------

    /// Lay out the four child-component zones (Req 20.1, 20.3).
    void resized() override;

    /// Persist window bounds before closing (Req 20.5).
    void closeButtonPressed() override;

    /// Handle global keyboard shortcuts (L-1 §5).
    bool keyPressed(const juce::KeyPress& key) override;

private:

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    /// Returns a PropertiesFile for persisting window state (Req 20.5).
    static juce::PropertiesFile::Options makePropertiesOptions();

    /// Restore or compute the initial window bounds (Req 20.5).
    /// Falls back to centred 1024×768 if stored bounds are off-screen.
    juce::Rectangle<int> resolveInitialBounds();

     /// Returns true if @p bounds is at least partially visible on a connected
     /// display — used to decide whether to use stored bounds (Req 20.5).
     static bool boundsIntersectsDisplays(const juce::Rectangle<int>& bounds);

     // -----------------------------------------------------------------------
     // 20.7: Workspace session persistence
     // -----------------------------------------------------------------------

     /// Persist the current editor workspace (tabs, cursors, slot state) into
     /// the existing PropertiesFile. Called on quit / window close.
     void saveWorkspace();

     /// Restore the previously saved workspace from the PropertiesFile.
     /// Called during construction after EditorArea is fully initialised.
     /// Malformed or version-mismatched data is silently ignored.
     void restoreWorkspace();

     // -----------------------------------------------------------------------
     // Agent 0.1: Welcome screen (shown when no workspace was persisted)
     // -----------------------------------------------------------------------

     /// Create + show the welcome overlay, seeded with the persisted MRU.
     void showWelcomeScreen();

     // -----------------------------------------------------------------------
     // Phase G (D2–D4) — Petdex selection lifecycle
     // -----------------------------------------------------------------------

    /// React to an applied Petdex selection from Settings (fires on Apply with
    /// the committed slug; empty string = explicit "no mascot"). Runs the D4
    /// attribution gate before anything can be displayed.
    void applySelectedPet(const std::string& slug);

    /// Restore a persisted pet selection at startup (offline-safe: disk cache
    /// when present, D4 snapshot gate re-run from disk, never a manifest fetch).
    void restorePetSelection();

    // -----------------------------------------------------------------------
    // 0.2 — Workspace lifecycle (Open Folder + recent-projects MRU)
    // -----------------------------------------------------------------------

    /// Show a native directory chooser and switch workspace on confirmation.
    void openFolderChooser();

    /// Switch the workspace root at runtime: closes tabs under the old root
    /// (with save prompts), re-roots the Explorer and the EditorArea.
    void switchWorkspace(const juce::File& dir);

    /// Load the persisted recent-projects list (most-recent-first).
    std::vector<std::string> loadRecentProjects();

    /// Insert @p path at the front of the MRU (dedup) and persist (max 10).
    void pushRecentProject(const std::string& path);

    /// Re-register "Open Recent: <path>" palette actions from the MRU.
    void refreshRecentActions();

    // =========================================================================
    // Layout constants
    // =========================================================================
    /// Width of the ExplorerPanel when open (H1).
    static constexpr int kExplorerWidth = 240;

    // -----------------------------------------------------------------------
    // Dark theme
    // -----------------------------------------------------------------------
    HathorLookAndFeel lookAndFeel_;

    // -----------------------------------------------------------------------
    // Application properties (window bounds persistence — Req 20.5)
    // -----------------------------------------------------------------------
    juce::ApplicationProperties appProperties_;

    // -----------------------------------------------------------------------
    // Child components owned as unique_ptr so lifetime is tied to MainWindow.
    //
    // ActivityRibbon, ExplorerPanel, EditorArea, ChatSidebar,
    // VisualizerPanel, and UITimer are concrete types (headers available).
    // -----------------------------------------------------------------------
    std::unique_ptr<hathor::ui::ActivityRibbon>   activityRibbon_;
    std::unique_ptr<hathor::ui::ExplorerPanel>   explorerPanel_;
    std::unique_ptr<hathor::ui::EditorArea>        editorArea_;
    std::unique_ptr<hathor::ui::ChatSidebar>       chatSidebar_;
    std::unique_ptr<hathor::ui::VisualizerPanel>   visualizerPanel_;
    std::unique_ptr<hathor::ui::UITimer>            uiTimer_;

    // L-3: Unified Problems / Diagnostics status ribbon (bottom of window)
    std::unique_ptr<hathor::ui::StatusRibbon>        statusRibbon_;

    // 0.5/S4: orphan SliderPanel removed — ChatSidebar's instance is the
    // single BPM/gain surface (synced via chatSidebar_->getSliderPanel()).

    // Phase G — Petdex (D2–D4): sprite acquisition + D4-gated mascot display.
    // Declared service-first so destruction order stops the widget's timer
    // before the service joins its worker thread.
    std::unique_ptr<hathor::ui::PetdexResourceService> petdexResourceService_;
    std::unique_ptr<hathor::ui::PetWidget>              petWidget_;

    // Agent 0.1: startup welcome overlay (no persisted workspace).
    std::unique_ptr<hathor::ui::WelcomeScreen>       welcomeScreen_;

    // -----------------------------------------------------------------------
    // L-1: Editor ergonomics — owned by EditorArea, accessed via accessors.
    // MainWindow just triggers actions and adds the components to its layout.
    // -----------------------------------------------------------------------

    /// ACP agent session config — used to start chat threads (B6).
    /// The agentExePath_ can be updated via Settings (A2).
    /// The hathorMcpPath_ is resolved at startup and reused for all threads.
    std::string                                    agentExePath_;
    std::string                                    hathorMcpPath_;

    /// A2: Known-agent registry — powers the ChatSidebar header's agent
    /// selector combo. Loaded from the platform config dir at startup so the
    /// picker is live, not inert.
    std::unique_ptr<hathor::ui::AgentRegistry>     agentRegistry_;

    // 0.2: current workspace root (initialised in ctor; changed via Open Folder)
    std::string                                    workspaceDir_;

    // 0.2: kept alive for the duration of an async directory chooser
    std::unique_ptr<juce::FileChooser>             folderChooser_;

    // -----------------------------------------------------------------------
    // AI-8: Dynamic authoring context bridges (JUCE-dependent providers)
    // -----------------------------------------------------------------------
    std::unique_ptr<hathor::ui::EditorContextBridge> editorContextBridge_;
    std::unique_ptr<hathor::ui::LspContextBridge>    lspContextBridge_;

    // -----------------------------------------------------------------------
    // Engine references
    // -----------------------------------------------------------------------
    AudioEngine&                       audio_;
    hathor::control::ControlInterface& ci_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};
