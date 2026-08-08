// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * AudioEngineFacade.hpp — a JUCE-free interface exposing the AudioEngine
 * methods that the control/ layer needs.
 *
 * control/ translation units include this header instead of AudioEngine.hpp
 * so they are not dragged into the JUCE module compilation graph.
 *
 * AudioEngine (defined in app/AudioEngine.hpp) inherits from this class
 * so that the same object can be passed through both interfaces.
 *
 * Requirements: 11.1–11.5, 13.1–13.4, 14.1–14.6, 15.1–15.3
 */

#include "SlotState.hpp"

#include <memory>
#include <string>

/**
 * Abstract facade for the methods AudioEngine exposes to the control layer.
 *
 * Using an abstract base class keeps control/ completely free of JUCE headers
 * while still providing virtual-dispatch access to the real AudioEngine at
 * runtime.
 */
class AudioEngineFacade {
public:
    virtual ~AudioEngineFacade() = default;

     // --- Transport ---
    virtual void   play()    noexcept = 0;
    virtual void   stop()    noexcept = 0;
    virtual void   setBpm(double bpm) noexcept = 0;
    virtual double getBpm()  const noexcept = 0;
    virtual bool   isRunning() const noexcept = 0;

    // --- Per-slot play/stop (A3) ---
    // slotPlay(slotIdx) — resume one slot independently (others unchanged).
    // slotStop(slotIdx) — stop one slot independently (others continue).
    virtual void slotPlay(int slotIdx) noexcept = 0;
    virtual void slotStop(int slotIdx) noexcept = 0;
    virtual bool isSlotRunning(int slotIdx) const noexcept = 0;

    // --- Master gain (Req 26.5, 26.6) ---

    /// Set the master output gain (clamped to [0.0, 2.0], relaxed ordering).
    virtual void  setMasterGain(float g) noexcept = 0;

    /// Get the current master output gain (relaxed ordering).
    virtual float getMasterGain() const noexcept = 0;

    // --- Hot-swap slot API ---

    /// Map a slot name to a 0-based index. Returns -1 if not found and the
    /// table is full (16 slots already registered).
    virtual int findOrAddSlot(const std::string& name) = 0;

    /// Store a new SlotState into slot @p idx (release ordering).
    virtual void storeSlot(int idx, std::shared_ptr<SlotState> state) noexcept = 0;

    /// Clear slot @p idx. Returns false if idx is out of range.
    virtual bool clearSlot(int idx) noexcept = 0;

    /// Number of registered slot names.
    virtual int slotCount() const noexcept = 0;

    /// Name of slot @p idx (empty string if unregistered).
    virtual std::string slotName(int idx) const = 0;

    /// Load the current SlotState for slot @p idx (acquire ordering).
    virtual std::shared_ptr<SlotState> loadSlot(int idx) const noexcept = 0;
};
