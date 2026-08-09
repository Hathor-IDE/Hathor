// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * UITimer.cpp — 60 Hz drain loop and slider sync implementation.
 *
 * Requirements: 26.4, 26.9, 28.5, 28.6
 *
 * Stub panel classes are defined here (under include-guards) for tasks that
 * are not yet implemented:
 *
 *   HATHOR_SLIDER_PANEL_DEFINED      — SliderPanel     (task 3.9)
 *
 * When those headers land, the include-guard prevents the stubs from being
 * re-defined, and MainWindow.cpp (and any other TU) can include the real
 * headers before including UITimer.hpp.
 *
 * TODO (task 3.9): replace stub with  #include "SliderPanel.hpp"
 */

#include "UITimer.hpp"

// Task 3.8: VisualizerPanel is now implemented — include real header.
// Must be included before UITimer.hpp's forward declaration resolves.
#include "VisualizerPanel.hpp"

// Task 3.9: SliderPanel is now implemented — include real header.
// HATHOR_SLIDER_PANEL_DEFINED is set by SliderPanel.hpp, suppressing the stub below.
#include "SliderPanel.hpp"

#include <functional>

// ---------------------------------------------------------------------------
// SliderPanel stub — suppressed by HATHOR_SLIDER_PANEL_DEFINED (task 3.9 done)
// ---------------------------------------------------------------------------
#ifndef HATHOR_SLIDER_PANEL_DEFINED
#define HATHOR_SLIDER_PANEL_DEFINED
#include <juce_gui_basics/juce_gui_basics.h>

namespace hathor::ui {
/// Stub SliderPanel — replaced by SliderPanel.hpp (task 3.9).
class SliderPanel : public juce::Component {
public:
    SliderPanel() = default;

    /// Returns the integer BPM currently displayed in the BPM slider.
    int   bpmDisplayValue() const  { return bpmDisplay_; }

    /// Returns the gain value currently displayed in the gain slider.
    float gainDisplayValue() const { return gainDisplay_; }

    /// Update the displayed BPM without dispatching a command.
    void setBpmDisplay(int bpm)    { bpmDisplay_  = bpm; }

    /// Update the displayed gain without dispatching a command.
    void setGainDisplay(float g)   { gainDisplay_ = g; }

private:
    int   bpmDisplay_  = 120;
    float gainDisplay_ = 1.0f;
};
} // namespace hathor::ui
#endif  // HATHOR_SLIDER_PANEL_DEFINED

// ==========================================================================
// UITimer implementation
// ==========================================================================

namespace hathor::ui {

UITimer::UITimer(hathor::SpscRingBuffer<128>& buf,
                 hathor::ui::VisualizerPanel& vis,
                 hathor::ui::SliderPanel&     sliders,
                 AudioEngine&                 audio)
    : buf_(buf)
    , vis_(vis)
    , sliders_(sliders)
    , audio_(audio)
{
    // Pre-reserve so steady-state tick never allocates (Req 28.6).
    // 64 events * up to 128 ring-buffer slots is a reasonable worst-case
    // burst; in practice most ticks will see 1–4 frames with 0–4 events each.
    firedEvents_.reserve(hathor::kMaxFrameEvents * 4);
}

// ---------------------------------------------------------------------------
// timerCallback() — called on the JUCE message thread at ≈60 Hz (Req 28.5)
// ---------------------------------------------------------------------------
void UITimer::timerCallback()
{
    // -----------------------------------------------------------------------
    // (a) Drain the SPSC ring buffer (Req 28.5, 28.6)
    //
    // Loop until read() returns false. Accumulate:
    //   - latestCyclePos: the cyclePos from the most recently read frame
    //   - firedEvents_  : all events from every drained frame (appended)
    //
    // readStorage is aligned raw storage for kMaxFrameEvents Event<ParamMap>
    // objects. We avoid a plain array because Event<ParamMap> has no default
    // constructor (Arc::Rational has none). The buf_.read() call assigns into
    // this storage via copy assignment on previously placement-new'd objects
    // inside the ring buffer — so the memory just needs to be properly aligned.
    // -----------------------------------------------------------------------
    alignas(hathor::Event<hathor::ParamMap>)
        std::byte readStorage[hathor::kMaxFrameEvents * sizeof(hathor::Event<hathor::ParamMap>)];
    auto* readBuf = reinterpret_cast<hathor::Event<hathor::ParamMap>*>(readStorage);

    double   latestCyclePos = -1.0;
    uint32_t eventCount     = 0;

    firedEvents_.clear();

    while (buf_.read(latestCyclePos, eventCount, readBuf))
    {
        for (uint32_t i = 0; i < eventCount; ++i)
            firedEvents_.push_back(readBuf[i]);
    }

    // Only push to the visualizer if at least one frame was actually read.
    if (latestCyclePos >= 0.0)
        vis_.updateFrame(latestCyclePos, firedEvents_);

    // -----------------------------------------------------------------------
    // (b) Sync BPM slider display (Req 26.4)
    //
    // Read the engine BPM (atomic double, relaxed-equivalent via getBpm()),
    // round to nearest integer, compare with the slider's displayed value,
    // and update the display if they differ.  No dispatch() is called.
    // -----------------------------------------------------------------------
    const int bpmNow = static_cast<int>(std::round(audio_.getBpm()));
    if (bpmNow != sliders_.bpmDisplayValue())
        sliders_.setBpmDisplay(bpmNow);

    // -----------------------------------------------------------------------
    // (c) Sync master-gain slider display (Req 26.9)
    //
    // Read gain with memory_order_relaxed (continuous fader — no sync
    // dependency); compare with slider display using a 0.001 f epsilon to
    // avoid unnecessary repaints from floating-point noise.  No dispatch().
    // -----------------------------------------------------------------------
    const float gainNow = audio_.getMasterGain();   // memory_order_relaxed
    if (std::abs(gainNow - sliders_.gainDisplayValue()) > 0.001f)
        sliders_.setGainDisplay(gainNow);

    // -----------------------------------------------------------------------
    // (d) Sync per-tab Play/Stop button visuals to the engine's slot state (B1)
    //
    // The engine's SlotState::running atomic is the single source of truth.
    // This callback (installed by MainWindow) iterates all tabs and calls
    // setSlotRunningVisual() on each, so the button icon reflects the actual
    // armed/running state regardless of which path changed it.
    // -----------------------------------------------------------------------
     if (onSyncSlotButtons)
         onSyncSlotButtons();

     // -----------------------------------------------------------------------
     // (e) C1: Now-playing highlight update
     //
     // Pass the drained events to the EditorArea so it can resolve
     // sourceOffset → glyph bounds per-event, route by slotId, and apply
     // the highlight overlay on the correct tab.
     // -----------------------------------------------------------------------
     if (onUpdateNowPlaying && !firedEvents_.empty())
         onUpdateNowPlaying(firedEvents_);
 }

} // namespace hathor::ui
