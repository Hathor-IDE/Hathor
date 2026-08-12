// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetdexAnimation.hpp — deterministic animation driver (D3).
 *
 * JUCE-free. Drives the pet's frame selection from the verified Petdex state
 * convention (per-state frame counts + sequence durations). It is purely a UI
 * concern: it never runs on the audio thread and never allocates in steady
 * state.
 *
 * Deterministic timing: the current frame is a pure function of the elapsed
 * time accumulated inside the current state
 *     perFrameMs = state.durationMs / state.frames
 *     frame      = (elapsedInStateMs / perFrameMs) % state.frames
 * so the same elapsed timeline always produces the same frames. Callers
 * (typically a juce::Timer on the message thread) call advance() with the
 * elapsed milliseconds since the last call.
 *
 * Safety/fallback (requirement: a safe fallback state if an expected animation
 * is unavailable):
 *   - an unknown state id, a state whose row exceeds the sheet, or a state
 *     with zero frames all fall back to the "idle" row;
 *   - if even idle is unavailable (empty grid), the animation stays at frame 0.
 */

#include "PetdexFrameGrid.hpp"

#include <string>

namespace hathor::ui {

class PetdexAnimation
{
public:
    /// Attach the analyzed sheet grid (a copy). Configuring an invalid grid
    /// leaves the animation inert (frame 0, "idle").
    void configure(const PetdexFrameGrid& grid) noexcept;

    /// Switch state (a transition). Resets the in-state clock to frame 0.
    /// Unknown/unavailable ids fall back to "idle" (or stay inert).
    void setState(const std::string& id) noexcept;

    /// The current (possibly fallback) state id.
    std::string state() const noexcept { return stateId_; }

    /// Advance the in-state clock by @p elapsedMs (clamped to >= 0) and return
    /// the frame index to draw. Deterministic for a given elapsed timeline.
    int advance(int elapsedMs) noexcept;

    /// The frame index for the current state (valid after configure/advance).
    int currentFrame() const noexcept { return frame_; }

    /// Number of frames in the current state (0 if unavailable/inert).
    int frameCount() const noexcept;

    /// The sheet row of the current (resolved) state, for frame lookups
    /// (0 when inert — matches the idle row's geometry).
    int currentStateRow() const noexcept;

private:
    /// Resolve @p id to a usable state, falling back to idle/inert.
    const PetdexAnimationState* resolveState(const std::string& id) const noexcept;

    PetdexFrameGrid grid_;
    std::string stateId_;
    int frame_ = 0;
    int elapsedInStateMs_ = 0;
};

} // namespace hathor::ui
