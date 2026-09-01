// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * VisualizerPanel.hpp — real-time procedural visualizer (bottom panel).
 *
 * Receives VisualizerFrame data exclusively from UITimer::timerCallback()
 * via updateFrame().  It NEVER reads AudioEngine state directly (Req 29.2).
 * repaint() is called ONLY from updateFrame() — no self-owned timer (Req 29.5).
 *
 * Three rendering modes (Req 29.3), cycled by clicking the panel:
 *   Pulse     — filled ellipse scales 0.2×–1.0× panel height with cyclePos.
 *   StepGrid  — N×M cell grid; cells flash white on fired event, fade to dim.
 *   Waveform  — polyline of last 128 cyclePos samples.
 *
 * Idle state (Req 29.4): when no frames with eventCount > 0 are received for
 * 500 ms, all three modes show a slowly-breathing dim placeholder ring.
 *
 * paint() budget: ≤ 8 ms on target hardware (Req 29.6).
 *
 * Requirements: 29.1, 29.2, 29.3, 29.4, 29.5, 29.6
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <deque>
#include <vector>
#include <cstdint>
#include <functional>

// Engine types
#include "../app/VisualizerFrame.hpp"  // hathor::kMaxFrameEvents
#include "../app/SpscSampleRing.hpp"    // hathor::SpscSampleRing
#include "../engine/include/hathor/Event.hpp"
#include "../engine/include/hathor/ParamMap.hpp"

// Design system
#include "HathorLookAndFeel.hpp"

// Forward-declare AudioEngine — VisualizerPanel only accepts it as a constructor
// parameter for API uniformity; it MUST NOT store or dereference the reference
// (Req 29.2).  The full header is only needed in the .cpp.
class AudioEngine;

// Include-guard so MainWindow.cpp stub doesn't conflict if included before us.
#ifndef HATHOR_VISUALIZER_PANEL_DEFINED
#define HATHOR_VISUALIZER_PANEL_DEFINED

namespace hathor::ui {

/**
 * VisualizerPanel — procedural visualizer component.
 *
 * Occupies the bottom panel spanning the full width between the
 * ActivityRibbon right edge and the ChatSidebar right edge (≥ 120 px
 * height).  All sizing is handled by MainWindow::resized() (Req 29.1).
 */
class VisualizerPanel : public juce::Component
{
public:
    /// Visual rendering modes, cycled on mouseUp.
    enum class Mode : uint8_t
    {
        Pulse    = 0,  ///< pulsing filled ellipse driven by cyclePos
        StepGrid = 1,  ///< N×M cell grid with flash-and-fade
        Waveform = 2,  ///< polyline of last 128 cyclePos samples
        kCount   = 3
    };

    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    /// @param audio  Not stored — VisualizerPanel MUST NOT read AudioEngine
    ///               state.  Parameter exists so MainWindow::VisualizerPanel
    ///               construction is uniform.  Pass audio_ for
    ///               forward-compatibility.
    explicit VisualizerPanel(AudioEngine& /*audio*/);

    ~VisualizerPanel() override = default;

    // -----------------------------------------------------------------------
    // Data push — called exclusively by UITimer on the JUCE message thread.
    // Stores the latest frame data and calls repaint().
    // repaint() is NEVER called from anywhere else (Req 29.5).
    // -----------------------------------------------------------------------

    /**
     * Receive a new frame from UITimer.
     *
     * @param latestCyclePos  Cycle position from the most recent ring-buffer
     *                        frame (0.0 ≤ value, fractional part is beat phase).
     * @param events          All fired events accumulated across frames drained
     *                        this tick.
     */
    void updateFrame(double latestCyclePos,
                     const std::vector<hathor::Event<hathor::ParamMap>>& events);

    /**
     * Receive a batch of raw PCM samples drained from SpscSampleRing (V1).
     *
     * @param samples   Pointer to @p count float samples (mono, post-gain).
     * @param count     Number of valid samples (may be 0).
     *
     * Appends samples to the internal PCM ring buffer and triggers a repaint
     * so the waveform mode can render actual audio content (Agent 3.2 will
     * consume pcmHistory_ in paintWaveform).
     */
    void updateSamples(const float* samples, std::size_t count) noexcept;

    // -----------------------------------------------------------------------
    // Mode query (mostly for tests / external observers)
    // -----------------------------------------------------------------------
    Mode currentMode() const noexcept { return mode_; }

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------
    void paint(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    // -----------------------------------------------------------------------
    // Rendering helpers — one per mode
    // -----------------------------------------------------------------------
    void paintPulse(juce::Graphics& g, const juce::Rectangle<float>& bounds,
                    bool idle, float idlePhase, const Palette& palette) const;

    void paintStepGrid(juce::Graphics& g, const juce::Rectangle<float>& bounds,
                       bool idle, float idlePhase, const Palette& palette) const;

    void paintWaveform(juce::Graphics& g, const juce::Rectangle<float>& bounds,
                       bool idle, float idlePhase, const Palette& palette) const;

    /// Draw the dim placeholder ring (shown in idle state over any mode).
    void paintIdleRing(juce::Graphics& g, const juce::Rectangle<float>& bounds,
                       float phase, const Palette& palette) const;

    // -----------------------------------------------------------------------
    // Step grid dimensions
    static constexpr int kGridCols = 8;
    static constexpr int kGridRows = 4;
    static constexpr int kNumCells = kGridCols * kGridRows;  // 32

    // -----------------------------------------------------------------------
    // Waveform history
    // -----------------------------------------------------------------------
    static constexpr int kWaveformSamples = 128;

    // -----------------------------------------------------------------------
    // Idle timeout
    // -----------------------------------------------------------------------
    static constexpr int64_t kIdleThresholdMs = 500;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    Mode mode_       { Mode::Pulse };  ///< currently active visual mode
    double cyclePos_ { 0.0 };         ///< latest cycle position (fractional beat phase)

    /// Per-cell brightness [0.0, 1.0]; 1.0 = just flashed, decays toward 0.
    std::array<float, kNumCells> cellBrightness_ {};

    /// Rolling history of the last 128 cyclePos values.
    std::deque<double> waveHistory_;

    // -----------------------------------------------------------------------
    // PCM sample history (V1: raw float samples for waveform rendering)
    // -----------------------------------------------------------------------
    /// Maximum number of PCM samples retained for waveform display.
    static constexpr std::size_t kPcmHistoryMax = 512;

    /// Rolling PCM samples (newest at back); decimated to panel width at paint.
    std::array<float, kPcmHistoryMax> pcmHistory_ {};
    /// Current write position in pcmHistory_ (ring wrap index).
    std::size_t pcmWritePos_ { 0 };
    /// Number of valid samples in pcmHistory_ (≤ kPcmHistoryMax).
    std::size_t pcmCount_ { 0 };

    /// Timestamp (ms) of the last updateFrame() call that had eventCount > 0.
    int64_t lastActiveMs_ { 0 };

    /// Whether we are currently in the idle state.
    bool idle_ { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VisualizerPanel)
};

} // namespace hathor::ui

#endif  // HATHOR_VISUALIZER_PANEL_DEFINED
