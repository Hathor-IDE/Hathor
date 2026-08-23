// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * SliderPanel.hpp — BPM and master-gain juce::Slider controls.
 *
 * Renders two horizontal sliders in the bottom area of ChatSidebar:
 *   - BPM slider:  range [20, 400], step 1, initial display 120
 *   - Gain slider: range [0.0, 2.0], continuous, initial display 1.0
 *
 * Dispatches commands on the JUCE message thread via juce::AsyncUpdater
 * (0.5/C2 — coalesces to the latest value; no threads, no dangling refs):
 *   - BPM:  "bpm <value>"      (integer, only when changed)
 *   - Gain: "set-gain <value>" (2 decimal places, clamped [0.0, 2.0])
 *
 * setBpmDisplay / setGainDisplay update the slider display WITHOUT
 * dispatching — called by UITimer for bidirectional sync (Req 26.4, 26.9).
 *
 * Requirements: 26.1, 26.2, 26.3, 26.4, 26.9
 */

// This guard suppresses the stub definitions in UITimer.cpp and MainWindow.hpp.
#define HATHOR_SLIDER_PANEL_DEFINED

#include <juce_gui_basics/juce_gui_basics.h>

// Control
#include "../control/ControlInterface.hpp"

namespace hathor::ui {

/**
 * SliderPanel — two-slider component for BPM and master gain control.
 *
 * Layout (within the assigned bounds):
 *   - Top half:    BPM label + BPM slider
 *   - Bottom half: Gain label + Gain slider
 *
 * Requirements: 26.1, 26.2, 26.3, 26.4, 26.9
 */
class SliderPanel : public juce::Component,
                    private juce::AsyncUpdater
{
public:
    /**
     * Construct with a ControlInterface reference for dispatching commands.
     *
     * @param ci  ControlInterface used to dispatch "bpm" and "set-gain" commands.
     */
    explicit SliderPanel(hathor::control::ControlInterface& ci);

    ~SliderPanel() override;

    // Non-copyable / non-movable (holds a reference and JUCE components).
    SliderPanel(SliderPanel&&)                 = delete;
    SliderPanel& operator=(SliderPanel&&)      = delete;

    // -----------------------------------------------------------------------
    // Bidirectional sync accessors (Req 26.4, 26.9)
    // Called by UITimer on the JUCE message thread — do NOT dispatch.
    // -----------------------------------------------------------------------

    /// Returns the integer BPM currently displayed in the BPM slider.
    int   bpmDisplayValue() const;

    /// Returns the gain value currently displayed in the gain slider.
    float gainDisplayValue() const;

    /// Update the BPM slider display without dispatching a command (Req 26.4).
    void setBpmDisplay(int bpm);

    /// Update the gain slider display without dispatching a command (Req 26.9).
    void setGainDisplay(float g);

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    // -----------------------------------------------------------------------
    // Slider setup helpers
    // -----------------------------------------------------------------------
    void setupBpmSlider();
    void setupGainSlider();

    // -----------------------------------------------------------------------
    // Async dispatch (0.5/C2) — coalesces latest values onto the message
    // thread; nothing outlives the panel.
    // -----------------------------------------------------------------------
    void handleAsyncUpdate() override;
    void postDispatch();

    /// Pending flags/values set by onValueChange, drained in handleAsyncUpdate.
    bool  bpmPending_   = false;
    int   pendingBpm_   = 120;
    bool  gainPending_  = false;
    float pendingGain_  = 1.0f;

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------
    hathor::control::ControlInterface& ci_;

    juce::Label  bpmLabel_;
    juce::Slider bpmSlider_;

    juce::Label  gainLabel_;
    juce::Slider gainSlider_;

    /// Guard flag: when true, onValueChange callbacks skip dispatch.
    /// Set during setBpmDisplay / setGainDisplay to prevent re-entry.
    bool suppressDispatch_ = false;

    /// Last BPM dispatched via "bpm <value>" — used to suppress duplicate
    /// dispatches when the integer value has not changed (Req 26.2).
    int lastDispatchedBpm_ = 120;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SliderPanel)
};

} // namespace hathor::ui
