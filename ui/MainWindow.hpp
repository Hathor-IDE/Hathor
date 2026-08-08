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
// Child component headers that already exist (implemented in other tasks).
// Headers not yet available are forward-declared below.
// ---------------------------------------------------------------------------
#include "ActivityRibbon.hpp"   // task 3.2 — implemented
#include "ExplorerPanel.hpp"    // task 3.2 — implemented

// Task 3.4: EditorArea is now implemented — include the real header.
#include "EditorArea.hpp"

// Task 3.9: SliderPanel is now implemented — include the real header.
#include "SliderPanel.hpp"

// ChatSidebar and AcpAgentSession are now fully implemented (task 5.1).
#include "ChatSidebar.hpp"
#include "AcpAgentSession.hpp"

// ---------------------------------------------------------------------------
// Forward declarations — child components not yet implemented
// ---------------------------------------------------------------------------
namespace hathor::ui {

class VisualizerPanel;
class UITimer;

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

    /// Real SliderPanel — created in the constructor with ci_.
    /// Passed to UITimer for bidirectional BPM/gain sync (Req 26.4, 26.9).
    std::unique_ptr<hathor::ui::SliderPanel>       sliderPanel_;

    /// ACP agent session — created in the constructor and wired to ChatSidebar.
    /// Owned by MainWindow; stopped in the destructor before child destruction.
    /// Requirements: 32.1, 32.3
    std::unique_ptr<hathor::ui::AcpAgentSession>   agentSession_;

    // -----------------------------------------------------------------------
    // Engine references
    // -----------------------------------------------------------------------
    AudioEngine&                       audio_;
    hathor::control::ControlInterface& ci_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};
