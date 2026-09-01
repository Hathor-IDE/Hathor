// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * VisualizerPanel.cpp — procedural visualizer (three modes + idle state).
 *
 * Requirements: 29.1, 29.2, 29.3, 29.4, 29.5, 29.6
 *
 * Data flow (Req 29.2):
 *   UITimer::timerCallback() → VisualizerPanel::updateFrame()
 *                           → repaint()
 *                           → paint()
 *
 * No other code path writes cyclePos_ / cellBrightness_ / waveHistory_.
 * No other code path calls repaint() on this component.
 */

// Ensure the real class definition is seen before any stub guard fires.
// (UITimer.cpp and MainWindow.cpp define stubs behind the same guard;
//  including the real header first wins because the guard is already set.)
#include "VisualizerPanel.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include "../app/AudioEngine.hpp"   // Forward-declared parameter type
#include "../engine/include/hathor/ParamMap.hpp"  // hathor::keys::kS

#include <cmath>
#include <functional>  // std::hash
#include <string>

namespace hathor::ui {

// ==========================================================================
// Construction
// ==========================================================================

VisualizerPanel::VisualizerPanel(AudioEngine& /*audio*/)
{
    // Initialise cell brightness array to zero.
    cellBrightness_.fill(0.0f);

    // Mark idle from the start — no events yet.
    lastActiveMs_ = 0;
    idle_         = true;
}

// ==========================================================================
// updateFrame() — called exclusively by UITimer on the JUCE message thread
// ==========================================================================

void VisualizerPanel::updateFrame(
    double latestCyclePos,
    const std::vector<hathor::Event<hathor::ParamMap>>& events)
{
    // --- 1. Store latest cycle position ------------------------------------
    cyclePos_ = latestCyclePos;

    // --- 2. Push cyclePos into waveform history ----------------------------
    waveHistory_.push_back(latestCyclePos);
    while (static_cast<int>(waveHistory_.size()) > kWaveformSamples)
        waveHistory_.pop_front();

    // --- 3. Decay all cell brightnesses (step grid fade) ------------------
    //
    // At 60 Hz, multiplying by 0.85 per tick gives a ~200 ms fade-out
    // (0.85^12 ≈ 0.14, i.e. nearly gone after 12 ticks ≈ 200 ms).
    for (auto& b : cellBrightness_)
    {
        b *= 0.85f;
        if (b < 0.01f)
            b = 0.0f;
    }

    // --- 4. Flash cells for fired events ----------------------------------
    const bool hasEvents = !events.empty();
    if (hasEvents)
    {
        for (const auto& ev : events)
        {
            // Extract sample name from the "s" parameter (hathor::keys::kS).
            // hathor::Value is std::variant<double, std::string, int64_t>.
            std::string sampleName;
            if (const hathor::Value* sv = ev.value.get(hathor::keys::kS))
            {
                if (const auto* sp = std::get_if<std::string>(sv))
                    sampleName = *sp;
            }

            // Hash sample name to a cell index (Req 29.3b).
            const std::size_t cellIdx =
                std::hash<std::string>{}(sampleName) %
                static_cast<std::size_t>(kNumCells);

            cellBrightness_[cellIdx] = 1.0f;
        }

        // Record the time of the last frame with events.
        lastActiveMs_ = juce::Time::currentTimeMillis();
        idle_         = false;
    }

    // --- 5. Check idle threshold ------------------------------------------
    //
    // If no frames with eventCount > 0 have arrived in the last 500 ms,
    // enter idle state (Req 29.4).  UITimer still calls updateFrame() with
    // empty events during idle — so this check runs every tick.
    if (!idle_)
    {
        const int64_t nowMs  = juce::Time::currentTimeMillis();
        const int64_t deltaMs = nowMs - lastActiveMs_;
        if (deltaMs >= kIdleThresholdMs)
            idle_ = true;
    }

     // --- 6. Trigger repaint (Req 29.5) ------------------------------------
    //
    // repaint() is called ONLY here — never from a self-owned timer.
    repaint();
}

// ==========================================================================
// updateSamples() — receive raw PCM from SpscSampleRing drain (V1)
// ==========================================================================

void VisualizerPanel::updateSamples(const float* samples, std::size_t count) noexcept
{
    if (count == 0 || samples == nullptr)
        return;

    // Append incoming PCM samples into the ring buffer, wrapping as needed.
    // No allocation — writes directly into the inline std::array.
    for (std::size_t i = 0; i < count; ++i) {
        pcmHistory_[pcmWritePos_] = samples[i];
        pcmWritePos_ = (pcmWritePos_ + 1) % kPcmHistoryMax;
        if (pcmCount_ < kPcmHistoryMax)
            ++pcmCount_;
    }

    // Trigger repaint so the waveform mode can read the new PCM data.
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

    // Fill background.
    g.fillAll(palette.surface);

    const auto bounds = getLocalBounds().toFloat();
    if (bounds.isEmpty())
        return;

    // Compute idle animation phase [0.0, 1.0) for the slowly-breathing ring.
    // Uses a 3-second period so one breath cycle = 3 s.
    // Since repaint() is only called by UITimer at 60 Hz, this only advances
    // during active UITimer ticks — which is fine (Req 29.4 satisfied).
    const int64_t nowMs   = juce::Time::currentTimeMillis();
    const float   idlePhase =
        static_cast<float>((nowMs % 3000) / 3000.0);  // [0, 1)

    // Render the active mode.
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
        default:
            paintPulse(g, bounds, idle_, idlePhase, palette);
            break;
    }
}

// ==========================================================================
// paintPulse() — filled ellipse driven by cyclePos mod 1.0 (Req 29.3a)
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

    // fractional beat phase in [0.0, 1.0)
    const double frac = std::fmod(cyclePos_, 1.0);

    // Scale from 0.2× to 1.0× panel height.
    const float panelH  = bounds.getHeight();
    const float scale   = 0.2f + 0.8f * static_cast<float>(frac);
    const float diameter = scale * panelH;
    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();

    // Use a slightly desaturated accent colour for the pulse fill.
    const juce::Colour pulseColour =
        palette.accent.withAlpha(0.8f);

    g.setColour(pulseColour);
    g.fillEllipse(cx - diameter * 0.5f,
                   cy - diameter * 0.5f,
                   diameter,
                   diameter);
}

// ==========================================================================
// paintStepGrid() — N×M grid with flash-and-fade (Req 29.3b)
// ==========================================================================

void VisualizerPanel::paintStepGrid(juce::Graphics& g,
                                      const juce::Rectangle<float>& bounds,
                                      bool idle, float idlePhase,
                                      const Palette& palette) const
{
    if (idle)
    {
        // Draw dim grid structure plus idle ring overlay.
        // Grid cells are all dim.
        const float cellW = bounds.getWidth()  / static_cast<float>(kGridCols);
        const float cellH = bounds.getHeight() / static_cast<float>(kGridRows);
        const float pad   = 2.0f;

        g.setColour(palette.surfaceHigh);
        for (int row = 0; row < kGridRows; ++row)
        {
            for (int col = 0; col < kGridCols; ++col)
            {
                const juce::Rectangle<float> cell(
                    bounds.getX() + col * cellW + pad,
                    bounds.getY() + row * cellH + pad,
                    cellW - pad * 2.0f,
                    cellH - pad * 2.0f);
                g.fillRect(cell);
            }
        }

        paintIdleRing(g, bounds, idlePhase, palette);
        return;
    }

    const float cellW = bounds.getWidth()  / static_cast<float>(kGridCols);
    const float cellH = bounds.getHeight() / static_cast<float>(kGridRows);
    const float pad   = 2.0f;

    for (int row = 0; row < kGridRows; ++row)
    {
        for (int col = 0; col < kGridCols; ++col)
        {
            const int idx = row * kGridCols + col;
            const float brightness = cellBrightness_[idx];

            // Interpolate from dim background to accent.
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
}

// ==========================================================================
// paintWaveform() — polyline of last 128 cyclePos samples (Req 29.3c)
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

    if (waveHistory_.size() < 2)
        return;

    const float w = bounds.getWidth();
    const float h = bounds.getHeight();
    const int   n = static_cast<int>(waveHistory_.size());

    juce::Path path;
    bool       started = false;

    for (int i = 0; i < n; ++i)
    {
        // x: time index — left to right across panel width.
        const float x = bounds.getX() + (static_cast<float>(i) / static_cast<float>(n - 1)) * w;

        // y: fractional cycle pos (mod 1.0) mapped to panel height.
        // 0.0 at bottom, 1.0 at top (invert y for screen coords).
        const double frac = std::fmod(waveHistory_[i], 1.0);
        const float  y    = bounds.getBottom() - static_cast<float>(frac) * h;

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

    g.setColour(palette.accent.withAlpha(0.9f));
    g.strokePath(path, juce::PathStrokeType(1.5f));
}

// ==========================================================================
// paintIdleRing() — dim slowly-breathing placeholder ring (Req 29.4)
// ==========================================================================

void VisualizerPanel::paintIdleRing(juce::Graphics& g,
                                      const juce::Rectangle<float>& bounds,
                                      float phase,
                                      const Palette& palette) const
{
    // Slowly breathing alpha: sine wave between 0.15 and 0.45.
    // phase is in [0.0, 1.0), mapped to [0, 2π).
    const float alpha =
        0.15f + 0.15f * (0.5f + 0.5f * std::sin(phase * juce::MathConstants<float>::twoPi));

    const float cx = bounds.getCentreX();
    const float cy = bounds.getCentreY();

    // Ring radius: 30% of the smaller dimension.
    const float radius = std::min(bounds.getWidth(), bounds.getHeight()) * 0.30f;
    const float strokeW = std::max(2.0f, radius * 0.08f);

    const juce::Colour ringColour = palette.surfaceHighest.withAlpha(alpha);

    juce::Path ring;
    ring.addEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

    g.setColour(ringColour);
    g.strokePath(ring, juce::PathStrokeType(strokeW));
}

} // namespace hathor::ui
