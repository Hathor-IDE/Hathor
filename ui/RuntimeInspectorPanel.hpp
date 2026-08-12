// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * RuntimeInspectorPanel.hpp — L-6: Hathor runtime-inspection UI.
 *
 * A dockable panel (bottom-docked within EditorArea, like ProblemsPanel)
 * that answers "what is the engine doing right now?" with deterministic,
 * read-only runtime state:
 *
 *   - Playback: transport, BPM, cycle/beat, sample clock
 *   - Active pattern slots and active voices
 *   - ChucK: per-tab VM state (live/suspended/destroyed), shred info
 *   - Worker: liveness, status, generation (restart/crash visibility)
 *   - Audio: sample rate, master gain, EQ preset, device open, renders
 *   - Diagnostics: error/warning/info counts from the L-3 registry, with an
 *     "Open Problems" action (problems remain the single diagnostic authority)
 *
 * Threading / audio-safety:
 *   - The model's quick capture runs on the JUCE message thread (noexcept,
 *     atomic snapshot APIs only — never blocks).
 *   - Per-tab VM queries run on the model's background capture thread
 *     (control-plane IPC is bounded but must never touch the message or
 *     audio threads).
 *   - Opening/refreshing this panel never mutates audio or ChucK state.
 *
 * AI restriction (L-6 §AI RESTRICTION): no AI repair affordance here —
 * diagnostics and runtime inspection remain deterministic IDE tools.
 *
 * Requirement references: L-6 §Hathor Runtime Inspection, L-6 §Workspace Integration
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "HathorLookAndFeel.hpp"
#include "RuntimeInspectorModel.hpp"

namespace hathor::ui {

/**
 * RuntimeInspectorPanel — native JUCE runtime inspector.
 */
class RuntimeInspectorPanel : public juce::Component,
                              private juce::Timer
{
public:
    static constexpr int kPanelHeight = 250;

    /**
     * @param audio    The AudioEngineFacade (read-only inspection).
     * @param registry The L-3 DiagnosticRegistry (non-owning; may be null).
     */
    RuntimeInspectorPanel(AudioEngineFacade& audio,
                          hathor::control::DiagnosticRegistry* registry);
    ~RuntimeInspectorPanel() override;

    // -----------------------------------------------------------------------
    // juce::Component
    // -----------------------------------------------------------------------
    void resized() override;
    void paint(juce::Graphics& g) override;
    void setVisible(bool visible) override;

    // -----------------------------------------------------------------------
    // Callbacks — installed by DebugPanel / EditorArea
    // -----------------------------------------------------------------------
    std::function<void()> onOpenProblems;  ///< open the L-3 Problems panel
    std::function<void()> onClosePanel;

    /// Force an immediate refresh (e.g. when the panel is opened).
    void refreshNow();

private:
    // -----------------------------------------------------------------------
    // Display rows
    // -----------------------------------------------------------------------
    struct Row
    {
        enum class Kind { Section, Item };
        Kind        kind = Kind::Item;
        std::string label;             ///< left label (sections have none)
        std::string value;             ///< right-aligned value text
        juce::Colour colour;           ///< value colour (default textSecondary)
    };

    std::vector<Row> buildRows() const;

    // -----------------------------------------------------------------------
    // juce::Timer (message thread)
    // -----------------------------------------------------------------------
    static constexpr int kPollIntervalHz = 10;
    static constexpr int kVmCaptureIntervalMs = 1000;
    void timerCallback() override;

    void applySnapshot();

    // -----------------------------------------------------------------------
    // Child components
    // -----------------------------------------------------------------------
    std::unique_ptr<juce::Viewport>  viewport_;
    std::unique_ptr<juce::Component> content_;
    std::unique_ptr<juce::Label>     titleLabel_;
    std::unique_ptr<juce::TextButton> problemsBtn_;
    std::unique_ptr<juce::TextButton> refreshBtn_;
    std::unique_ptr<juce::TextButton> closeBtn_;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    RuntimeInspectorModel model_;
    RuntimeSnapshot      snap_;
    std::vector<Row>     rows_;
    juce::int64          lastVmCaptureMs_ = 0;

    // Layout constants
    static constexpr int kHeaderHeight = 28;
    static constexpr int kRowHeight    = 18;
    static constexpr int kSectionHeight = 22;
    static constexpr int kMargin       = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RuntimeInspectorPanel)
};

} // namespace hathor::ui
