// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * VisualizerPanel.hpp — real-time procedural visualizer (bottom panel).
 *
 * Receives visualizer frame data exclusively from UITimer::timerCallback()
 * via updateFrame() and updateSamples().  It NEVER reads AudioEngine state
 * directly (Req 29.2).
 * repaint() is called ONLY from updateSamples() — no self-owned timer (Req 29.5).
 *
 * Three rendering modes (Req 29.3), with Spectrum as the only active mode:
 *   Spectrum  — magnitude spectrum from small FFT over PCM ring (vertical
 *               bars, bass left → treble right). This is the sole mode.
 *
 * Idle state (Req 29.4): when transport is stopped or no PCM samples are
 * received for 500 ms, a slowly-breathing dim placeholder ring is shown.
 *
 * paint() budget: ≤ 8 ms on target hardware (Req 29.6).
 *
 * Requirements: 29.1, 29.2, 29.3, 29.4, 29.5, 29.6
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <complex>
#include <cstdint>
#include <vector>

// Engine types
#include "../app/VisualizerFrame.hpp"  // hathor::kMaxFrameEvents
#include "../app/SpscSampleRing.hpp"   // hathor::SpscSampleRing
#include "../engine/include/hathor/Event.hpp"
#include "../engine/include/hathor/ParamMap.hpp"
#include "../engine/include/hathor/Arc.hpp"

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
    /// Visual rendering mode. Only Spectrum is retained; Pulse, StepGrid,
    /// and Waveform were removed to simplify the visualizer to a single
    /// linear spectrum equalizer (vertical bars, bass-left → treble-right).
    enum class Mode : uint8_t
    {
        Spectrum = 3,  ///< magnitude spectrum from small FFT over PCM ring
    };

    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    /// @param audio  Not stored — VisualizerPanel MUST NOT read AudioEngine
    ///               state.  Parameter exists so MainWindow::VisualizerPanel
    ///               construction is uniform.
    explicit VisualizerPanel(AudioEngine& /*audio*/);

    ~VisualizerPanel() override = default;

    // -----------------------------------------------------------------------
    // Data push — called exclusively by UITimer on the JUCE message thread.
    // repaint() is called from updateSamples() (V4: continuous repaint).
    // -----------------------------------------------------------------------

    /**
     * Receive a new frame from UITimer (musical event data).
     *
     * @param latestCyclePos  Cycle position from the most recent ring-buffer
     *                        frame (0.0 <= value, fractional part is beat phase).
     * @param events          All fired events accumulated across frames drained
     *                        this tick.
     * @param sampleRate      Current audio device sample rate (Hz, 0 if no device).
     * @param bpmValue    Current tempo in BPM.
     * @param running         Whether the transport is currently running.
     */
    void updateFrame(double latestCyclePos,
                     const std::vector<hathor::Event<hathor::ParamMap>>& events,
                     int sampleRate,
                     double bpmValue,
                     bool running);

    /**
     * Receive a batch of raw PCM samples drained from SpscSampleRing (V1).
     *
     * @param samples   Pointer to @p count float samples (mono, post-gain).
     * @param count     Number of valid samples (may be 0).
     * @param running   Whether the transport is currently running.
     *
     * Appends samples to the internal PCM history and triggers a repaint
     * so the waveform and spectrum modes can render actual audio content.
     */
    void updateSamples(const float* samples, std::size_t count, bool running);

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------
    void paint(juce::Graphics& g) override;

private:
    // -----------------------------------------------------------------------
    // Rendering helpers
    // -----------------------------------------------------------------------
    void paintSpectrum(juce::Graphics& g, const juce::Rectangle<float>& bounds,
                       bool idle, float idlePhase, const Palette& palette) const;

    /// Draw the dim placeholder ring (shown in idle state over any mode).
    void paintIdleRing(juce::Graphics& g, const juce::Rectangle<float>& bounds,
                       float phase, const Palette& palette) const;

    // -----------------------------------------------------------------------
    // FFT (dependency-free radix-2 DIT)
    // -----------------------------------------------------------------------
    /// Compute magnitude spectrum from input samples.  @p n must be a power
    /// of two.  Output magnitudes written to @p outMag (size n/2 + 1).
    /// Reads from raw pointer input (no heap allocation in the FFT itself).
    static void computeFFTMagnitude(const float* input,
                                    float* outMag, int n);

    // -----------------------------------------------------------------------
    // PCM history
    // -----------------------------------------------------------------------
    static constexpr int kPcmHistoryMax = 512;

    // -----------------------------------------------------------------------
    // Idle timeout
    // -----------------------------------------------------------------------
    static constexpr int64_t kIdleThresholdMs = 500;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    double cyclePos_ { 0.0 };         ///< latest cycle position (fractional beat phase)
    int    sampleRate_ { 44100 };     ///< current device sample rate (Hz)
    double bpm_       { 120.0 };      ///< current tempo (BPM)

    /// Ring-buffer of recent PCm samples (writeCursor_ is the next-write slot).
    /// Using a fixed array + cursor avoids the O(n) erase(begin()) that a
    /// growing-and-shrunk std::vector caused on every 60 Hz tick — the old
    /// code did up to kPcmHistoryMax element shifts per tick = O(n) per tick,
    /// which dropped frames whenever the audio callback was producing samples.
    std::array<float, kPcmHistoryMax> pcmHistory_ {};

    /// Number of valid entries currently in pcmHistory_ (≤ kPcmHistoryMax).
    int pcmCount_      { 0 };

    /// Next write slot in pcmHistory_ (wraps modulo kPcmHistoryMax).
    /// Oldest valid sample is at pcmHistory_[(writeCursor_ - pcmCount_ + kPcmHistoryMax) % kPcmHistoryMax].
    int pcmWriteCursor_ { 0 };

    /// Timestamp (ms) of the last updateFrame() call that had eventCount > 0.
    int64_t lastActiveMs_ { 0 };

    /// Whether we are currently in the idle state.
    bool idle_ { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VisualizerPanel)
};

} // namespace hathor::ui

#endif  // HATHOR_VISUALIZER_PANEL_DEFINED
