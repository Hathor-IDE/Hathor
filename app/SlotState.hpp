// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * SlotState.hpp — the SlotState aggregate used by both AudioEngine and
 * WorkerThread.
 *
 * Intentionally free of JUCE headers so that control/ translation units can
 * include this without pulling in the full JUCE SDK.
 *
 * Requirements: 11.1–11.4, 13.2
 */

#include "hathor/Event.hpp"
#include "hathor/ParamMap.hpp"
#include "hathor/Pattern.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

/**
 * SlotState — bundles a compiled Pattern<ParamMap> with its pre-allocated
 * event buffer so the audio thread never needs to resize anything.
 *
 * The eventBuffer is sized exactly once on the worker thread via
 *   eventBuffer.resize(pattern->maxEventsPerCycle())
 * and is never resized again.
 */
struct SlotState {
    std::shared_ptr<hathor::Pattern<hathor::ParamMap>> pattern;
    std::vector<hathor::Event<hathor::ParamMap>>       eventBuffer;
    /// Canonical mini-notation string (from PrettyPrinter). Used by
    /// list-patterns. Written once on the worker thread; read on the main
    /// thread only after the atomic store/load fence.
    std::string notation;

    /// Per-slot running flag (A3 — true independent per-slot Play/Stop).
    /// Set by the control thread (slot-play/slot-stop), read lock-free on the
    /// audio thread.  When false the audio callback skips voice triggering for
    /// this slot and instead silences any voices that were previously armed by
    /// this slot.  The flag is atomic so no mutex is needed in the callback.
    std::atomic<bool> running{false};
};
