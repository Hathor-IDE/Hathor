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
#include "MasterEq.hpp"
#include "AssetTarget.hpp"

#include <memory>
#include <string>
#include <filesystem>

// Bring AssetTarget into the global namespace so AudioEngineFacade's method
// signatures stay unqualified (matching the existing SlotState/EqPreset pattern).
using hathor::AssetTarget;

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

    // --- B7-K2: Master-bus preset EQ ---

    /// Select the master-bus EQ preset (worker/control thread).
    /// Publishes a complete replacement filter state atomically.
    virtual void setMasterEqPreset(hathor::EqPreset preset) noexcept = 0;

    /// Returns the currently active EQ preset.
    virtual hathor::EqPreset getMasterEqPreset() const noexcept = 0;

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

    // --- B4-K7: Per-tab ChucK VM evaluation ---

    /// Check if the audio worker process is running.
    virtual bool hasWorker() const noexcept = 0;

    /// Evaluate ChucK source code for a tab (compile→load→execute path).
    /// This is the .ck equivalent of set-pattern for mini-notation.
    /// @param slotIdx  Pattern slot index (maps to tab slot).
    /// @param code     ChucK source code.
    /// @return true on successful compile+publish; false on error.
    virtual bool ckEval(int slotIdx, const std::string& code) noexcept = 0;

    /// Stop a .ck tab: destroy the per-tab VM and clear any pending handoff.
    /// The previously running shred is not affected (the VM is destroyed).
    /// @param slotIdx  Pattern slot index.
    /// @return true on success; false if no VM was running.
    virtual bool stopCkTab(int slotIdx) noexcept = 0;

    /// Query the .ck VM state for a tab slot.  Returns a human-readable
    /// status string (e.g. "active shred_id=5 source_hash=0x1234").
    /// @param slotIdx  Pattern slot index.
    /// @return Status string (empty if no worker or VM).
    virtual std::string queryCkTab(int slotIdx) const = 0;

    // --- B8-K1: Bake-to-Song render target (Studio vs Live Jam) ---
    //
    // The bake pipeline receives the selected AssetTarget explicitly.
    // B8-K1 owns target selection, representation, and path resolution;
    // B8-K2 owns rendering the ChucK instrument and writing PCM data.
    //
    // resolveRenderPath() turns (target, name, projectDir) into a concrete
    // .wav path.  The caller (B8-K2 renderer) then writes audio to that path.
    // Studio is ALWAYS the default; LiveJam is an explicit opt-in.

    /// Resolve the render destination path for a bake operation (B8-K1 §5).
    ///
    /// @param target      Studio (default) or LiveJam.
    /// @param name        Instrument name (sanitised — see sanitizeAssetName).
    /// @param projectDir  Current Hathor project directory.
    /// @return Absolute path to the .wav destination.  Empty string on error.
    ///
    /// For Studio:  <projectDir>/.hathor_assets/chuck_instruments/<name>.wav
    /// For LiveJam: <session-temp>/hathor_live_jam_<pid>_<seq>/<name>.wav
    ///
    /// The parent directory is created if it does not exist.
    /// Path construction is centralised here — never duplicated in the renderer.
    virtual std::filesystem::path resolveRenderPath(AssetTarget target,
                                                     std::string_view name,
                                                     const std::filesystem::path& projectDir) = 0;

    /// Set the LiveJam session directory (for the current Hathor session).
    /// Called once at session startup; required before resolveRenderPath(LiveJam, ...).
    virtual void setLiveJamSessionDir(std::filesystem::path dir) = 0;

    /// Trigger cleanup of all LiveJam assets for the current session.
    /// Called from application shutdown (HathorApplication::shutdown).
    /// Removes only LiveJam temp files — NEVER Studio assets.
    virtual void cleanupLiveJamAssets() = 0;

    /// Returns true if the given path is inside the Studio permanent asset area.
    virtual bool isStudioAssetPath(const std::filesystem::path& path) const = 0;
};
