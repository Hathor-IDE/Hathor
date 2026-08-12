// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * RuntimeInspectorModel.hpp — L-6: JUCE-free Hathor runtime-inspection model.
 *
 * Collects deterministic runtime state for the Hathor Runtime Inspector from
 * the EXISTING authoritative services (no duplicate runtime model):
 *
 *   - AudioEngineFacade  — playback state, BPM, cycle/beat, active slots,
 *                          active voices, per-tab ChucK VM state, worker
 *                          liveness/generation (AudioStatus, SlotPlayback,
 *                          VoiceInfo, VmStatus)
 *   - DiagnosticRegistry — error/warning/info counts (L-3 diagnostics stay
 *                          the single authority — this model only reads counts)
 *
 * Threading / audio-safety contract (L-6 §AUDIO / THREAD SAFETY):
 *   - captureQuick()  — message thread.  Uses only noexcept, lock-free
 *     snapshot APIs (atomics) plus the registry's mutex-protected counts().
 *     Never blocks, never mutates engine state.
 *   - requestVmCapture() — spawns a detached capture thread that queries
 *     per-tab VM state over the control-plane socket.  This IPC can block
 *     (bounded ~5 s when the worker is hung), so it NEVER runs on the
 *     message thread or the audio thread.  The capture only calls read-only
 *     facade methods; opening the inspector never starts/stops/restarts
 *     audio or ChucK state.
 *   - shutdown() — marks the model stopped and joins the capture thread with
 *     a bounded grace period (it may block inside an in-flight control-plane
 *     query for at most the IPC timeout).  After shutdown() returns, no
 *     capture code is running.
 *
 * The audio thread never touches this class.
 *
 * Requirement references: L-6 §Hathor Runtime Inspection, L-6 §Audio/Thread Safety
 */

#include "../app/AudioEngineFacade.hpp"
#include "../control/DiagnosticRegistry.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hathor::ui {

/**
 * One snapshot of deterministic runtime state, grouped by the L-6 surfaces:
 * Playback / ChucK / Worker / Audio / Diagnostics.
 */
struct RuntimeSnapshot
{
    // --- Playback + Audio (AudioEngineFacade::AudioStatus) ---
    AudioEngineFacade::AudioStatus audio;

    // --- Active pattern slots ---
    std::vector<AudioEngineFacade::SlotPlayback> slots;

    // --- Active voices ---
    std::vector<AudioEngineFacade::VoiceInfo> voices;

    // --- Worker liveness (from the latest VM capture) ---
    bool        workerAlive = false;
    std::string workerStatus;         ///< "healthy" | "dead" | ... (VmStatus.workerStatus)
    uint64_t    workerGeneration = 0; ///< worker generation/session identity

    // --- Per-tab ChucK VM state (async capture) ---
    std::vector<AudioEngineFacade::VmStatus> vmStates;
    std::vector<int>                         vmSlotIndices;  ///< parallel to vmStates

    // --- Diagnostics (read from the L-3 registry — not duplicated authority) ---
    int diagErrors   = 0;
    int diagWarnings = 0;
    int diagInfo     = 0;
};

/**
 * RuntimeInspectorModel — collects and caches RuntimeSnapshot data.
 *
 * Owned by the RuntimeInspectorPanel (a JUCE component).  The model itself is
 * JUCE-free so it can be unit-tested headlessly.
 */
class RuntimeInspectorModel
{
public:
    /// Shared state: lives as long as the model AND any in-flight capture
    /// thread (both hold shared_ptr copies) — never dangles.
    struct Shared
    {
        std::atomic<bool> stopped{false};
        std::atomic<bool> vmCaptureInFlight{false};

        mutable std::mutex mtx;
        RuntimeSnapshot    snap;
    };

    RuntimeInspectorModel() = default;
    ~RuntimeInspectorModel();

    RuntimeInspectorModel(const RuntimeInspectorModel&) = delete;
    RuntimeInspectorModel& operator=(const RuntimeInspectorModel&) = delete;

    // -----------------------------------------------------------------------
    // Sources (must be set before capture; both outlive the model)
    // -----------------------------------------------------------------------
    void setSources(AudioEngineFacade* audio,
                    hathor::control::DiagnosticRegistry* registry) noexcept;

    // -----------------------------------------------------------------------
    // Quick capture — message thread, non-blocking
    // -----------------------------------------------------------------------
    /// Refresh the fast fields (audio status, slots, voices, diag counts).
    void refreshQuick();

    /// Latest merged snapshot (const; thread-safe).
    RuntimeSnapshot snapshot() const;

    // -----------------------------------------------------------------------
    // VM capture — background thread (never the message/audio thread)
    // -----------------------------------------------------------------------
    /// Synchronously query per-tab VM state.  May block on control-plane IPC.
    /// Used by the background thread and by tests.
    void captureVmsSync();

    /// Start an asynchronous VM capture on a detached thread if none is in
    /// flight.  Non-blocking.
    void requestVmCapture();

    /// True while a VM capture is running in the background.
    bool vmCaptureInFlight() const noexcept;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
    /// Stop the model and join any in-flight capture (bounded wait).
    /// Called from the panel destructor on the message thread.
    void shutdown();

private:
    AudioEngineFacade*              audio_{nullptr};
    hathor::control::DiagnosticRegistry* registry_{nullptr};
    std::shared_ptr<Shared>         shared_{std::make_shared<Shared>()};
    std::thread                     captureThread_;  ///< joinable only while a capture runs
};

} // namespace hathor::ui
