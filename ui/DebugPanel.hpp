// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * DebugPanel.hpp — L-6: combined "Debug & Inspect" dockable panel.
 *
 * Hosts the two complementary L-6 surfaces as tabs (mirroring the
 * SourceControlPanel Changes/History pattern):
 *
 *   - "Debugger" — native C++ debugging (DebuggerPanel / DebugSession):
 *     breakpoints, continue/pause/step, call stack, locals, watches.
 *   - "Runtime" — Hathor runtime inspection (RuntimeInspectorPanel /
 *     RuntimeInspectorModel): playback, BPM/cycle, slots, voices, ChucK
 *     VMs/shreds, worker liveness/restart, audio state, diagnostics.
 *
 * The Runtime tab is the default — musician-first: useful musical/runtime
 * state is one click away; the developer-facing C++ debugger is one more.
 *
 * No second application-wide dashboard is introduced: this is a docked
 * panel within the existing EditorArea workspace, next to Problems,
 * Terminal, and Source Control.  Clicking the worker indicator in the
 * bottom StatusRibbon opens this panel on the Runtime tab.
 *
 * Requirement references: L-6 §Native/C++ Debugging, L-6 §Hathor Runtime
 * Inspection, L-6 §Workspace Integration
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

#include "DebuggerPanel.hpp"
#include "RuntimeInspectorPanel.hpp"

namespace hathor::ui {

/**
 * DebugPanel — two-tab container for native debugging + runtime inspection.
 */
class DebugPanel : public juce::Component
{
public:
    static constexpr int kPanelHeight = 300;

    /**
     * @param audio    The AudioEngineFacade (read-only inspection).
     * @param registry The L-3 DiagnosticRegistry (non-owning; may be null).
     */
    DebugPanel(AudioEngineFacade& audio,
               hathor::control::DiagnosticRegistry* registry);
    ~DebugPanel() override = default;

    // -----------------------------------------------------------------------
    // juce::Component
    // -----------------------------------------------------------------------
    void resized() override;
    void paint(juce::Graphics& g) override;
    void setVisible(bool visible) override;

    // -----------------------------------------------------------------------
    // Tab selection
    // -----------------------------------------------------------------------
    /// Switch to the Runtime inspector tab (e.g. from the StatusRibbon worker
    /// indicator).
    void showRuntimeTab();

    /// Switch to the native Debugger tab.
    void showDebuggerTab();

    /// The underlying debugger pane (for programmatic control).
    DebuggerPanel& debuggerPane() noexcept { return debuggerPane_; }

    /// The underlying runtime inspector pane.
    RuntimeInspectorPanel& runtimePane() noexcept { return runtimePane_; }

    // -----------------------------------------------------------------------
    // Callbacks — installed by EditorArea
    // -----------------------------------------------------------------------
    std::function<void()> onClosePanel;
    std::function<void()> onOpenProblems;

private:
    enum class Tab { Debugger, Runtime };

    struct TabButton
    {
        Tab                    tab;
        juce::String           label;
        juce::Rectangle<int>   bounds;
        bool                   active = false;
    };

    void setActiveTab(Tab tab);
    void layoutTabButtons();

    // -----------------------------------------------------------------------
    // Child components
    // -----------------------------------------------------------------------
    DebuggerPanel       debuggerPane_;
    RuntimeInspectorPanel runtimePane_;

    std::array<TabButton, 2> tabButtons_
    {{
        { Tab::Debugger, "Debugger", {} },
        { Tab::Runtime,  "Runtime",  {} },
    }};

    Tab activeTab_ = Tab::Runtime;   ///< musician-first default (L-6 §Musician-first)

    // Layout constants
    static constexpr int kTabBarHeight = 26;
    static constexpr int kTabWidth     = 110;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DebugPanel)
};

} // namespace hathor::ui
