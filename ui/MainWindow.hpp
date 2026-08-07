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
 *   - Background luminance ≤ 15% of white  (0xff1e1e1e ≈ 7%)
 *   - All text contrast ratio ≥ 4.5:1 WCAG AA (0xffd4d4d4 on 0xff1e1e1e ≈ 9.6:1)
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

// ---------------------------------------------------------------------------
// Child component headers that already exist (implemented in other tasks).
// Headers not yet available are forward-declared below.
// ---------------------------------------------------------------------------
#include "ActivityRibbon.hpp"   // task 3.2 — implemented
#include "ExplorerPanel.hpp"    // task 3.2 — implemented

// ---------------------------------------------------------------------------
// Forward declarations — child components not yet implemented
// (ChatSidebar: task 5.1, EditorArea: task 3.4,
//  VisualizerPanel: task 3.8, UITimer: task 3.7)
// ---------------------------------------------------------------------------
namespace hathor::ui {

class EditorArea;
class ChatSidebar;
class VisualizerPanel;
class UITimer;

} // namespace hathor::ui

// ---------------------------------------------------------------------------
// HathorLookAndFeel — dark theme (Req 20.2, 20.4)
//
// Background: 0xff1e1e1e  (relative luminance ≈ 1.4% — well under 15%)
// Text:       0xffd4d4d4  (contrast ratio vs. background ≈ 9.6:1 — WCAG AA ✓)
//
// WCAG relative-luminance formula (IEC 61966-2-1 sRGB):
//   L = 0.2126·R + 0.7152·G + 0.0722·B  (values linearised)
//   Background: R=G=B=0x1e/255=0.118  → linearised ≈ 0.013 → L ≈ 0.013
//   "15% of white" means L ≤ 0.15  ✓
//   Text 0xffd4d4d4: R=G=B=0xd4/255=0.831 → L ≈ 0.655
//   Contrast = (0.655+0.05)/(0.013+0.05) ≈ 11.2:1 ≥ 4.5:1 ✓
// ---------------------------------------------------------------------------

class HathorLookAndFeel : public juce::LookAndFeel_V4
{
public:
    /// Background colour for all panels — luminance ≈ 1.4% (Req 20.2).
    static constexpr juce::uint32 kColourBackground = 0xff1e1e1e;

    /// Primary text colour — contrast ratio ≈ 11:1 on kColourBackground (Req 20.2).
    static constexpr juce::uint32 kColourText       = 0xffd4d4d4;

    /// Slightly lighter surface for panels (e.g. sidebar, ribbon).
    static constexpr juce::uint32 kColourSurface    = 0xff252526;

    /// Accent colour used for highlighted/active elements.
    static constexpr juce::uint32 kColourAccent     = 0xff569cd6;

    HathorLookAndFeel();

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HathorLookAndFeel)
};

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------

class MainWindow : public juce::DocumentWindow
{
public:
    /**
     * Construct the main application window.
     *
     * @param audio  Reference to the fully-initialised AudioEngine (device
     *               must already be open so UITimer can start immediately).
     * @param ci     Reference to the ControlInterface for dispatching commands.
     */
    explicit MainWindow(AudioEngine& audio,
                        hathor::control::ControlInterface& ci);

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
    // ActivityRibbon and ExplorerPanel are concrete types (headers available).
    // EditorArea, ChatSidebar, VisualizerPanel, UITimer are forward-declared
    // and will be wired up as their tasks are completed.
    // -----------------------------------------------------------------------
    std::unique_ptr<hathor::ui::ActivityRibbon>   activityRibbon_;
    std::unique_ptr<hathor::ui::EditorArea>        editorArea_;
    std::unique_ptr<hathor::ui::ChatSidebar>       chatSidebar_;
    std::unique_ptr<hathor::ui::VisualizerPanel>   visualizerPanel_;
    std::unique_ptr<hathor::ui::UITimer>            uiTimer_;

    // -----------------------------------------------------------------------
    // Engine references
    // -----------------------------------------------------------------------
    AudioEngine&                       audio_;
    hathor::control::ControlInterface& ci_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};
