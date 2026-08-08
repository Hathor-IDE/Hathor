// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * UITimer.hpp — 60 Hz JUCE timer: drains the SPSC ring buffer and syncs
 * slider display values from AudioEngine state.
 *
 * Requirements: 26.4, 26.9, 28.5, 28.6
 *
 * Thread safety:
 *   timerCallback() runs on the JUCE message thread (timer callback thread).
 *   It calls buf_.read() which is the sole consumer of the SPSC ring buffer —
 *   never called from any other thread.
 *   Slider accessors (bpmDisplayValue, gainDisplayValue, setBpmDisplay,
 *   setGainDisplay) are JUCE-component operations and also run on the
 *   message thread.
 */

#include <juce_core/juce_core.h>   // juce::Timer, juce::int64
#include <vector>
#include <cmath>
#include <cstdint>

// Engine types
#include "../app/VisualizerFrame.hpp"   // hathor::SpscRingBuffer<128>
#include "../app/AudioEngine.hpp"       // AudioEngine

// Forward-declare the two panel types that may not yet be implemented.
// When VisualizerPanel.hpp (task 3.8, now done) and SliderPanel.hpp (task 3.9) exist,
// MainWindow.cpp will include them before including UITimer.hpp; the
// definitions below (in UITimer.cpp) are protected by include-guards so they
// will not conflict.
namespace hathor::ui {
    class VisualizerPanel;
    class SliderPanel;
} // namespace hathor::ui

namespace hathor::ui {

/**
 * UITimer — juce::Timer subclass running at 60 Hz (Req 28.5).
 *
 * Each tick:
 *   (a) Drains the SPSC ring buffer, accumulating the latest cyclePos and
 *       all fired events; calls VisualizerPanel::updateFrame() if any frames
 *       were read (Req 28.6).
 *   (b) Syncs the BPM slider display without dispatching (Req 26.4).
 *   (c) Syncs the master-gain slider display without dispatching (Req 26.9).
 *
 * Started with startTimerHz(60) from MainWindow constructor (Req 28.5).
 */
class UITimer : public juce::Timer
{
public:
    /**
      * @param buf      SPSC ring buffer owned by AudioEngine.
      * @param vis      VisualizerPanel to push frames to.
      * @param sliders  SliderPanel whose display values are kept in sync.
      * @param audio    AudioEngine for BPM / gain / slot queries.
      */
    UITimer(hathor::SpscRingBuffer<128>& buf,
            hathor::ui::VisualizerPanel& vis,
            hathor::ui::SliderPanel&     sliders,
            AudioEngine&                 audio);

    ~UITimer() override = default;

    // Non-copyable / non-movable.
    UITimer(const UITimer&)            = delete;
    UITimer& operator=(const UITimer&) = delete;
    UITimer(UITimer&&)                 = delete;
    UITimer& operator=(UITimer&&)      = delete;

    // -----------------------------------------------------------------------
    // Slot-play/stop sync (B1)
    // -----------------------------------------------------------------------
    // Installed by MainWindow after construction. Called at 60 Hz to sync
    // each tab's Play/Stop button visual state to the engine's SlotState::running
    // atomic, so the UI reflects slot state changes from any path (not just
    // button clicks).
    std::function<void()> onSyncSlotButtons;

    // juce::Timer override — three-step drain/sync logic.
    void timerCallback() override;

private:
    hathor::SpscRingBuffer<128>&            buf_;
    hathor::ui::VisualizerPanel&            vis_;
    hathor::ui::SliderPanel&                sliders_;
    AudioEngine&                            audio_;

    /// Reused per tick — no heap allocation in steady state (Req 28.6).
    std::vector<hathor::Event<hathor::ParamMap>> firedEvents_;
};

} // namespace hathor::ui
