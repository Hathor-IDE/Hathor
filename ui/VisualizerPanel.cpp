// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * VisualizerPanel.cpp — procedural visualizer (four modes + idle state).
 *
 * Requirements: 29.1, 29.2, 29.3, 29.4, 29.5, 29.6
 *
 * Data flow (Req 29.2):
 *   UITimer::timerCallback() -> VisualizerPanel::updateFrame()
 *                           -> VisualizerPanel::updateSamples()
 *                           -> repaint()
 *                           -> paint()
 *
 * No other code path writes cyclePos_ / cellBrightness_ / pcmHistory_.
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
    cellBrightness_.fill(0.0f);
    pcmHistory_.reserve(kPcmHistoryMax);
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

    // --- 1. Decay all cell brightnesses (step grid fade) ------------------
    // At 60 Hz, multiplying by 0.80 per tick gives a ~250 ms fade-out.
    for (auto& b : cellBrightness_)
    {
        b *= 0.80f;
        if (b < 0.01f)
            b = 0.0f;
    }

    // --- 2. Flash cells for fired events (mapped to real positions) ------
    const bool hasEvents = !events.empty();
    if (hasEvents)
    {
        for (const auto& ev : events)
        {
            // Map the event's active arc to a cell based on its time position
            // within the current cycle.
            //
            // Event::active.start is a Rational representing cycle-relative
            // position [0, 1).  We map it to a column:
            //   column = floor(active.start.toDouble() * kGridCols)
            // And the slotId determines the row:
            //   row = slotId % kGridRows
            //
            // This gives musical-order lighting: events that fire earlier
            // in the beat light up earlier columns, and slots are stacked
            // by row.

            const double activeStart = ev.active.start.toDouble();
            const int col = static_cast<int>(std::floor(activeStart * kGridCols)) % kGridCols;
            const int row = (ev.slotId >= 0) ? (ev.slotId % kGridRows) : 0;
            const int idx = row * kGridCols + col;

            if (idx >= 0 && idx < kNumCells)
                cellBrightness_[idx] = 1.0f;
        }

        lastActiveMs_ = juce::Time::currentTimeMillis();
        idle_ = false;
    }

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
            pcmHistory_.push_back(samples[i]);

        // Cap history size to prevent unbounded growth.
        while (static_cast<int>(pcmHistory_.size()) > kPcmHistoryMax)
            pcmHistory_.erase(pcmHistory_.begin());

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
// mouseUp() — cycle through visual modes
// ==========================================================================

void VisualizerPanel::mouseUp(const juce::MouseEvent& /*e*/)
{
    const auto next = static_cast<uint8_t>(mode_) + 1u;
    mode_ = static_cast<Mode>(next % static_cast<uint8_t>(Mode::kCount));
    repaint();
}

// ==========================================================================
// paint() — dispatch to active mode renderer
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

    switch (mode_)
    {
        case Mode::Pulse:
            paintPulse(g, bounds, idle_, idlePhase, palette);
            break;
        case Mode::StepGrid:
            paintStepGrid(g, bounds, idle_, idlePhase, palette);
            break;
        case Mode::Waveform:
            paintWaveform(g, bounds, idle_, idlePhase, palette);
            break;
        case Mode::Spectrum:
            paintSpectrum(g, bounds, idle_, idlePhase, palette);
            break;
        default:
            paintPulse(g, bounds, idle_, idlePhase, palette);
            break;
    }
}

// ==========================================================================
// paintPulse() — filled ellipse driven by PCM audio energy
// ==========================================================================

void VisualizerPanel::paintPulse(juce::Graphics& g,
                                   const juce::Rectangle<float>& bounds,
                                   bool idle, float idlePhase,
                                   const Palette& palette) const
{
    if (idle)
    {
        paintIdleRing(g, bounds, idlePhase, palette);
        return;
    }

    // Compute peak amplitude from recent PCM history.
    float peak = 0.0f;
    if (!pcmHistory_.empty())
    {
        const int windowSize = std::min(static_cast<int>(pcmHistory_.size()), 64);
        const int offset = static_cast<int>(pcmHistory_.size()) - windowSize;
        for (int i = 0; i < windowSize; ++i)
            peak = std::max(peak, std::abs(pcmHistory_[offset + i]));
    }

    // Scale from 0.2x to 1.0x panel height based on audio energy.
    const float panelH  = bounds.getHeight();
    const float scale   = 0.2f + 0.8f * peak;
    const float diameter = scale * panelH;
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();

    const juce::Colour pulseColour = palette.accent.withAlpha(0.8f);

    g.setColour(pulseColour);
    g.fillEllipse(cx - diameter * 0.5f,
                   cy - diameter * 0.5f,
                   diameter,
                   diameter);
}

// ==========================================================================
// paintStepGrid() — 8x4 grid with flash-and-fade mapped to real positions
// ==========================================================================

void VisualizerPanel::paintStepGrid(juce::Graphics& g,
                                      const juce::Rectangle<float>& bounds,
                                      bool idle, float idlePhase,
                                      const Palette& palette) const
{
    const float cellW = bounds.getWidth()  / static_cast<float>(kGridCols);
    const float cellH = bounds.getHeight() / static_cast<float>(kGridRows);
    const float pad   = 2.0f;

    for (int row = 0; row < kGridRows; ++row)
    {
        for (int col = 0; col < kGridCols; ++col)
        {
            const int idx = row * kGridCols + col;
            const float brightness = cellBrightness_[idx];

            const juce::Colour dimC  = palette.surfaceHigh;
            const juce::Colour flashC = palette.accent;
            const juce::Colour cellC  = dimC.interpolatedWith(flashC, brightness);

            const juce::Rectangle<float> cell(
                bounds.getX() + col * cellW + pad,
                bounds.getY() + row * cellH + pad,
                cellW - pad * 2.0f,
                cellH - pad * 2.0f);

            g.setColour(cellC);
            g.fillRect(cell);
        }
    }

    if (idle)
        paintIdleRing(g, bounds, idlePhase, palette);
}

// ==========================================================================
// paintWaveform() — polyline of actual incoming PCM samples
// ==========================================================================

void VisualizerPanel::paintWaveform(juce::Graphics& g,
                                      const juce::Rectangle<float>& bounds,
                                      bool idle, float idlePhase,
                                      const Palette& palette) const
{
    if (idle)
    {
        paintIdleRing(g, bounds, idlePhase, palette);
        return;
    }

    if (pcmHistory_.empty())
        return;

    const float w = bounds.getWidth();
    const float h = bounds.getHeight();
    const int   n = static_cast<int>(pcmHistory_.size());

    juce::Path path;
    bool       started = false;

    // Decimate PCM history to panel width and plot as a waveform.
    const int panelW = static_cast<int>(w);
    for (int px = 0; px < panelW; ++px)
    {
        const int srcIdx = (n > 1)
            ? static_cast<int>(static_cast<double>(px) / static_cast<double>(panelW) * static_cast<double>(n - 1))
            : 0;

        if (srcIdx < 0 || srcIdx >= n)
            continue;

        const float sample = pcmHistory_[srcIdx];
        const float x = bounds.getX() + static_cast<float>(px);

        // Map amplitude [-1, 1] to panel height, centered at middle.
        const float y = bounds.getCentreY() - sample * (h * 0.5f);

        if (!started)
        {
            path.startNewSubPath(x, y);
            started = true;
        }
        else
        {
            path.lineTo(x, y);
        }
    }

    // Draw center line.
    g.setColour(palette.surfaceHighest.withAlpha(0.3f));
    g.drawLine(bounds.getX(), bounds.getCentreY(),
               bounds.getRight(), bounds.getCentreY(), 1.0f);

    g.setColour(palette.accent.withAlpha(0.9f));
    g.strokePath(path, juce::PathStrokeType(1.5f));
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
        return;
    }

    if (pcmHistory_.size() < 2)
        return;

    // Build input: take the most recent kFftSize samples, zero-pad if fewer.
    float input[kFftSize];
    std::memset(input, 0, sizeof(input));

    const int available = static_cast<int>(pcmHistory_.size());
    const int toCopy = std::min(available, kFftSize);
    const int srcOffset = available - toCopy;

    for (int i = 0; i < toCopy; ++i)
        input[i] = pcmHistory_[srcOffset + i];

    // Apply Hann window to reduce spectral leakage.
    for (int i = 0; i < kFftSize; ++i)
    {
        const float hann = 0.5f - 0.5f * std::cos(
            juce::MathConstants<float>::twoPi * static_cast<float>(i) / static_cast<float>(kFftSize - 1));
        input[i] *= hann;
    }

    // Compute FFT magnitude spectrum.
    float mag[kFftSize / 2 + 1];
    computeFFTMagnitude(
        std::vector<float>(input, input + kFftSize),
        mag, kFftSize);

    // Draw the spectrum as a bar graph.
    const float barW = bounds.getWidth() / static_cast<float>(kFftSize / 2);
    const float maxMag = 0.1f;

    for (int i = 0; i < kFftSize / 2; ++i)
    {
        const float magnitude = mag[i];
        const float normalized = std::min(1.0f, magnitude / maxMag);
        const float barH = normalized * bounds.getHeight() * 0.8f;

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

void VisualizerPanel::computeFFTMagnitude(const std::vector<float>& input,
                                            float* outMag, int n)
{
    // Bit-reverse reordering.
    std::vector<std::complex<float>> data(static_cast<std::size_t>(n));
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
