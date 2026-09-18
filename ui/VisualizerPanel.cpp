// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * VisualizerPanel.cpp — procedural visualizer (spectrum equalizer only).
 *
 * Requirements: 29.1, 29.2, 29.3, 29.4, 29.5, 29.6
 *
 * Data flow (Req 29.2):
 *   UITimer::timerCallback() -> VisualizerPanel::updateFrame()
 *                           -> VisualizerPanel::updateSamples()
 *                           -> repaint()
 *                           -> paint()
 *
 * No other code path writes cyclePos_ / pcmHistory_.
 * repaint() is called ONLY from updateSamples() — no self-owned timer (Req 29.5).
 */

#include "VisualizerPanel.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include "../app/AudioEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hathor::ui {

// ==========================================================================
// Construction
// ==========================================================================

VisualizerPanel::VisualizerPanel(AudioEngine& /*audio*/)
{
    setTitle("Visualizer");
    setDescription("Live audio spectrum of the playing session.");
    pcmCount_       = 0;
    pcmWriteCursor_ = 0;
    lastActiveMs_ = 0;
    idle_         = true;
}

// ==========================================================================
// updateFrame() — called exclusively by UITimer on the JUCE message thread
// ==========================================================================

void VisualizerPanel::updateFrame(
    double latestCyclePos,
    const std::vector<hathor::Event<hathor::ParamMap>>& events,
    int sampleRate,
    double bpmValue,
    bool running)
{
    cyclePos_ = latestCyclePos;
    sampleRate_ = sampleRate;
    bpm_ = bpmValue;

    // If transport is not running, we are idle regardless of event activity.
    if (!running)
    {
        idle_ = true;
        return;
    }

    // Check for musical activity from events (used for idle detection).
    const bool hasEvents = !events.empty();
    if (hasEvents)
    {
        lastActiveMs_ = juce::Time::currentTimeMillis();
        idle_ = false;
    }

    // PCM arrival in updateSamples() is what drives the spectrum animation.

    // --- 3. Check idle threshold (Req 29.4) --------------------------------
    if (!idle_)
    {
        const int64_t nowMs  = juce::Time::currentTimeMillis();
        const int64_t deltaMs = nowMs - lastActiveMs_;
        if (deltaMs >= kIdleThresholdMs)
            idle_ = true;
    }

    // Note: repaint() is called from updateSamples(), which is always
    // called after updateFrame() by UITimer (V4: continuous repaint).
}

// ==========================================================================
// updateSamples() — receive raw PCM from SpscSampleRing drain (V1)
// ==========================================================================

void VisualizerPanel::updateSamples(const float* samples, std::size_t count, bool running)
{
    if (samples != nullptr && count > 0)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            pcmHistory_[pcmWriteCursor_] = samples[i];
            pcmWriteCursor_ = (pcmWriteCursor_ + 1) % kPcmHistoryMax;
            if (pcmCount_ < kPcmHistoryMax)
                ++pcmCount_;
            else
                pcmCount_ = kPcmHistoryMax;  // full — cursor wrap handles eviction
        }

        // PCM arrival means we're not idle.
        idle_ = false;
        lastActiveMs_ = juce::Time::currentTimeMillis();
    }
    else if (!running)
    {
        // No PCM data and transport stopped -> idle.
        idle_ = true;
    }

    // Continuous repaint regardless of frame arrival (V4: timer calls
    // this unconditionally every 60 Hz, ensuring the idle breathing
    // animation never stalls even without frame arrival).
    repaint();
}

// ==========================================================================
// paint() — render the spectrum equalizer
// ==========================================================================

void VisualizerPanel::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    g.fillAll(palette.surface);

    const auto bounds = getLocalBounds().toFloat();
    if (bounds.isEmpty())
        return;

    // Compute idle animation phase [0.0, 1.0) for the slowly-breathing ring.
    const int64_t nowMs   = juce::Time::currentTimeMillis();
    const float   idlePhase =
        static_cast<float>((nowMs % 3000) / 3000.0);

    // Spectrum is the only rendering mode (linear FFT bar graph,
    // bass-left → treble-right). All other modes (Pulse, StepGrid,
    // Waveform) were removed to simplify the visualizer.
    paintSpectrum(g, bounds, idle_, idlePhase, palette);
}

// ==========================================================================
// paintSpectrum() — magnitude spectrum from small dependency-free FFT
// ==========================================================================

void VisualizerPanel::paintSpectrum(juce::Graphics& g,
                                      const juce::Rectangle<float>& bounds,
                                      bool idle, float idlePhase,
                                      const Palette& palette) const
{
    constexpr int kFftSize = 256;

    if (idle)
    {
        paintIdleRing(g, bounds, idlePhase, palette);
        // Text second for the idle state (not color-only): the ring alone
        // doesn't say why nothing is moving.
        g.setColour(palette.textSecondary);
        g.setFont(HathorLookAndFeel::uiFontRegular(11.0f));
        g.drawText("Idle — press play",
                   bounds.toNearestInt(),
                   juce::Justification::centred, false);
        return;
    }

    if (pcmCount_ < 2)
    {
        g.setColour(palette.textSecondary);
        g.setFont(HathorLookAndFeel::uiFontRegular(11.0f));
        g.drawText("Waiting for audio…",
                   bounds.toNearestInt(),
                   juce::Justification::centred, false);
        return;
    }

    // Build input: take the most recent kFftSize samples (newest at the end),
    // reading oldest-first from the ring buffer.
    float input[kFftSize];
    std::memset(input, 0, sizeof(input));

    const int toCopy = std::min(pcmCount_, kFftSize);
    const int oldestIdx = (pcmWriteCursor_ - pcmCount_ + kPcmHistoryMax) % kPcmHistoryMax;
    const int srcStart = oldestIdx;  // begin reading oldest valid sample

    for (int i = 0; i < toCopy; ++i)
        input[i] = pcmHistory_[(srcStart + i) % kPcmHistoryMax];

    // Apply Hann window to reduce spectral leakage.
    for (int i = 0; i < kFftSize; ++i)
    {
        const float hann = 0.5f - 0.5f * std::cos(
            juce::MathConstants<float>::twoPi * static_cast<float>(i) / static_cast<float>(kFftSize - 1));
        input[i] *= hann;
    }

    // Compute FFT magnitude spectrum (no heap allocation — input is a stack array).
    float mag[kFftSize / 2 + 1];
    computeFFTMagnitude(input, mag, kFftSize);

    // Draw the spectrum as a bar graph.
    // Use dynamic normalization: find the peak magnitude this frame, then
    // scale all bars relative to it (with a floor so near-silence still
    // shows something).  This avoids the hard-coded maxMag=0.1f which
    // saturated every bar to full height for any non-zero PCM.
    float peakMag = 0.0f;
    for (int i = 0; i < kFftSize / 2; ++i)
        peakMag = std::max(peakMag, mag[i]);

    // Floor: 1% of the DC bin for a 256-sample windowed signal ≈ 2.0.
    // This ensures the spectrum is visible even at low volumes but
    // never clips the scale so peaks reach ~full bar height.
    peakMag = std::max(peakMag, 2.0f);

    const float barW = bounds.getWidth() / static_cast<float>(kFftSize / 2);
    const float barMax = bounds.getHeight() * 0.85f;

    for (int i = 0; i < kFftSize / 2; ++i)
    {
        const float magnitude   = mag[i];
        const float normalized  = std::min(1.0f, magnitude / peakMag);
        const float barH        = normalized * barMax;

        const juce::Rectangle<float> bar(
            bounds.getX() + static_cast<float>(i) * barW,
            bounds.getBottom() - barH,
            barW - 1.0f,
            barH);

        const float t = static_cast<float>(i) / static_cast<float>(kFftSize / 2);
        const juce::Colour barColor = palette.accent.withAlpha(0.5f + 0.5f * t);

        g.setColour(barColor);
        g.fillRect(bar);
    }
}

// ==========================================================================
// paintIdleRing() — dim slowly-breathing placeholder ring
// ==========================================================================

void VisualizerPanel::paintIdleRing(juce::Graphics& g,
                                      const juce::Rectangle<float>& bounds,
                                      float phase,
                                      const Palette& palette) const
{
    const float alpha =
        0.15f + 0.15f * (0.5f + 0.5f * std::sin(phase * juce::MathConstants<float>::twoPi));

    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();

    const float radius = std::min(bounds.getWidth(), bounds.getHeight()) * 0.30f;
    const float strokeW = std::max(2.0f, radius * 0.08f);

    const juce::Colour ringColour = palette.surfaceHighest.withAlpha(alpha);

    juce::Path ring;
    ring.addEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

    g.setColour(ringColour);
    g.strokePath(ring, juce::PathStrokeType(strokeW));
}

// ==========================================================================
// computeFFTMagnitude() — dependency-free radix-2 DIT FFT
// ==========================================================================

void VisualizerPanel::computeFFTMagnitude(const float* input,
                                            float* outMag, int n)
{
    // Use a stack buffer for the complex working data — no heap allocation
    // in this 60 Hz paint path.  kFftSize is always 256, so 256 complex
    // floats = 2 KiB on the stack, which is safe.
    constexpr int kMaxFft = 256;
    jassert(n <= kMaxFft);  // paintSpectrum uses kFftSize = 256
    std::complex<float> data[kMaxFft];
    for (int i = 0; i < n; ++i)
        data[i] = std::complex<float>(input[i], 0.0f);

    {
        int j = 0;
        for (int i = 1; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;

            if (i < j)
                std::swap(data[i], data[j]);
        }
    }

    // Radix-2 DIT FFT.
    for (int len = 2; len <= n; len <<= 1)
    {
        const float angle = -juce::MathConstants<float>::twoPi / static_cast<float>(len);
        const std::complex<float> wlen(
            std::cos(angle), std::sin(angle));

        for (int i = 0; i < n; i += len)
        {
            std::complex<float> w(1.0f, 0.0f);
            for (int k = 0; k < len / 2; ++k)
            {
                const std::complex<float> u = data[i + k];
                const std::complex<float> v = data[i + k + len / 2] * w;
                data[i + k] = u + v;
                data[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }

    // Extract magnitudes.
    for (int i = 0; i <= n / 2; ++i)
        outMag[i] = std::abs(data[i]);
}

} // namespace hathor::ui
