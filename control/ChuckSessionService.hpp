// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ChuckSessionService.hpp — AI-5 canonical ChucK session service.
 *
 * This is the canonical application-layer service for ChucK lifecycle
 * operations.  It exposes session-based operations that map onto the
 * isolated per-tab ChucK architecture (B4-K3) while hiding all low-level
 * VM/worker thread/watchdog internals.
 *
 * Architecture boundary (AI-5):
 *
 *   MCP / AI / UI
 *         ↓
 *   ChuckSessionService  ← this layer
 *         ↓
 *   AudioWorkerManager + VmLifecycle + ChuckCompiler  (B4-K3/K4/K7)
 *
 * Requirement references: AI-5 §1–§12, §14, §16
 */

#include "ChuckSession.hpp"

#include "../app/AudioEngineFacade.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <mutex>

namespace hathor::control {

// Forward declaration — JobTracker is defined in JobTracker.hpp.
class JobTracker;

/**
 * ChuckSessionService — canonical service layer for ChucK session lifecycle.
 *
 * Constructed with a reference to AudioEngineFacade (which AudioEngine
 * satisfies).  All operations translate between the abstract session
 * vocabulary and the concrete B4-K3 tab-slot architecture via the facade.
 *
 * Thread model:
 *   - All public methods may be called from the control/worker thread.
 *   - compileChuck() enqueues onto the shared job infrastructure and
 *     returns immediately.
 *   - Async job completion callbacks are invoked on the job tracker's
 *     worker thread; the service forwards results to the caller-provided
 *     completion callback.
 */
class ChuckSessionService {
public:
    /**
     * Construct the service bound to an AudioEngineFacade.
     *
     * @param audio  The AudioEngineFacade that provides access to the
     *               B4-K3 per-tab VM architecture.
     */
    explicit ChuckSessionService(AudioEngineFacade& audio) noexcept;

    ~ChuckSessionService() = default;

    ChuckSessionService(const ChuckSessionService&) = delete;
    ChuckSessionService& operator=(const ChuckSessionService&) = delete;

    // -----------------------------------------------------------------------
    // AI-5 §3: create_chuck_session
    // -----------------------------------------------------------------------

    /**
     * Create or establish a ChucK session.
     *
     * Does NOT activate a live VM — creates the session metadata only.
     * The VM is allocated (B4-K3 vm_activate) only when auditionSession()
     * is called.  Per AI-5 §3: "Do not blindly create a live VM merely
     * because an API caller asks to inspect a .ck file."
     *
     * @param slotIdx  The tab/slot index [0, 15) this session maps to.
     * @param source   The .ck source text to associate with this session.
     * @return The created session with its canonical session ID.
     */
    ChuckSession createSession(uint8_t slotIdx, std::string source);

    // -----------------------------------------------------------------------
    // AI-5 §4: get_chuck_session
    // -----------------------------------------------------------------------

    /**
     * Get the current state of a ChucK session.
     *
     * Queries the B4-K3 VM state via the control plane (vm_query) and
     * translates the internal state into the canonical SessionState enum.
     *
     * @param sessionId  The session ID (e.g. "ck:3").
     * @return The session with current state, diagnostics, and runtime info.
     *         If the session does not exist, state is Open with an error.
     */
    ChuckSession getSession(std::string_view sessionId) const;

    // -----------------------------------------------------------------------
    // AI-5 §5: compile_chuck (ASYNCHRONOUS)
    // -----------------------------------------------------------------------

    /**
     * Start an asynchronous ChucK compilation job.
     *
     * This call returns IMMEDIATELY with a job handle.  Compilation happens
     * on a background thread (B4-K4 ChuckCompiler dispatcher) and the result
     * is delivered via the completion callback.
     *
     * Uses the shared job infrastructure (JobTracker) — the same infrastructure
     * that B8-K2 rendering uses (AI-5 §16).
     *
     * @param sessionId     The session to compile into.
     * @param source        ChucK source code to compile.
     * @param onComplete    Called on the job tracker's worker thread when the
     *                      job completes (success or failure).  May be null.
     * @return AsyncJobHandle for polling/cancellation.
     */
    AsyncJobHandle compileChuck(std::string_view sessionId,
                                 std::string source,
                                 std::function<void(CompileResult)> onComplete);

    // -----------------------------------------------------------------------
    // AI-5 §11: audition_chuck
    // -----------------------------------------------------------------------

    /**
     * Start (audition) a ChucK session — activate the VM for the given tab.
     *
     * Maps to B4-K3 vm_activate.  Affects ONLY this session — other sessions
     * continue running (AI-5 §11).
     *
     * Unlike compileChuck, this does not compile new source.  It simply
     * activates (or reactivates) the per-tab VM so it can play pre-compiled
     * shreds.
     *
     * @param sessionId  The session to audition.
     * @return The session after audition (state should be Live).
     */
    ChuckSession auditionSession(std::string_view sessionId);

    // -----------------------------------------------------------------------
    // AI-5 §12: stop_chuck
    // -----------------------------------------------------------------------

    /**
     * Stop a ChucK session — destroy the VM and clear any pending handoff.
     *
     * Maps to B4-K7 ck_stop.  Affects ONLY this session — other sessions,
     * including .hathor pattern playback, are not stopped (AI-5 §12).
     *
     * @param sessionId  The session to stop.
     * @return The session after stop (state should be Destroyed).
     */
    ChuckSession stopSession(std::string_view sessionId);

    // -----------------------------------------------------------------------
    // AI-5 §10: ChucK diagnostics
    // -----------------------------------------------------------------------

    /**
     * Get structured diagnostics for ChucK source text.
     *
     * Routes through the REAL vendored ChucK compiler diagnostic path
     * (validateChuckSource — the same function called by ChuckCompiler::
     * dispatcherLoop on B4-K4).  When libchuck is linked, this will use
     * ck.compileCode() directly.
     *
     * This is a read-only operation — it does NOT start a VM (AI-5 §10).
     *
     * @param source  The .ck source text to diagnose.
     * @return Structured diagnostics from the real compiler.
     */
    std::vector<ChuckDiagnosticInfo> getDiagnostics(std::string_view source) const;

    // -----------------------------------------------------------------------
    // AI-5 §6: Job status polling
    // -----------------------------------------------------------------------

    /**
     * Query the status of an async job.
     *
     * @param jobId  The job ID returned by compileChuck().
     * @return JSON with job_id, status, and (if complete) the result.
     */
    nlohmann::json getJobStatus(uint64_t jobId) const;

    /**
     * Cancel an in-flight async job.
     *
     * @param jobId  The job ID returned by compileChuck().
     * @return true if the job was cancelled, false if it could not be
     *         (already complete, unknown, or cancellation not supported
     *         for this job type).
     */
    bool cancelJob(uint64_t jobId);

private:
    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    /// Parse a session ID like "ck:3" → tab index 3.  Returns -1 on invalid.
    static int parseSessionId(std::string_view sessionId);

    /// Build a session ID from a tab index: "ck:" + tabId.
    static std::string makeSessionId(uint8_t tabIdx);

    /// Translate B4 VMState string to canonical SessionState.
    static SessionState translateState(const std::string& vmStateStr);

    AudioEngineFacade&        audio_;
    std::shared_ptr<JobTracker>  jobTracker_;
};

} // namespace hathor::control
