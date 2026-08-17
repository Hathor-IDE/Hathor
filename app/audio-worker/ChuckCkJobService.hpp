// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckCkJobService.hpp — JUCE-free async ChucK compile job tracker (AI-5 Phase 2A/2B/2C).
 *
 * Replaces the in-class CkJobEntry registry that previously lived inline in
 * AudioEngine.hpp.  That inline implementation had three defects that prevented
 * correct job-status querying:
 *
 *   1. Non-canonical schema — queryCkJob() returned {job_id, status, ok,
 *      cancel_requested, response} instead of the canonical JobTracker::queryJob()
 *      shape ({ok, job_id, status, success, result.diagnostics, error}).
 *   2. Data race — the worker thread wrote entry->response (a std::string) with
 *      no lock while queryCkJob() read it; the only mutex guarded the map, not
 *      the per-entry result fields.
 *   3. "queued" was unreachable — the entry was stamped Running immediately at
 *      construction, before the worker thread ever began.
 *
 * This class fixes all three and exposes real compiler diagnostics for the
 * Failed state by parsing the worker's ck_compile reply (error= / line= /
 * col=), which is produced by the SAME ChuckCompiler::dispatcherLoop() that
 * compileChuK already relies on (B4-K4).  No new diagnostic-code vocabulary is
 * invented — CK_OK, CK_COMPILE_ERROR and CK_CANCELLED are reused verbatim from
 * control/ChuckSessionService.cpp's compile job callback.
 *
 * JUCE-free: depends only on audio_ipc.h (VMResult/Publisher), nlohmann/json, and
 * the C++ standard library.  Built into hathor-audio-worker-lib so it is linked by
 * both the JUCE app target and the JUCE-free test binaries.
 *
 * Requirement references: AI-5 §5, AI-5 §6, AI-5 §7, B4-K4, B4-K7
 */

#pragma once

#include "audio_ipc.h" // VMResult

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace hathor::audio_worker {

/**
 * Compile job lifecycle — mirrors hathor::JobState (AI-1 canonical job model).
 * Kept as a distinct type so the JUCE-free audio-worker layer does not depend on
 * the control layer's headers (would create a CMake cycle via hathor-chuck-diagnostics).
 */
enum class CompileJobState : int {
    Queued    = 0, ///< Job accepted, worker thread not yet running.
    Running   = 1, ///< Worker thread is live; IPC to the ChucK worker in flight.
    Succeeded = 2, ///< ck_compile completed and the worker acknowledged the shred.
    Failed    = 3, ///< ck_compile rejected the source (real compiler diagnostics).
    Cancelled = 4, ///< Cooperative cancellation was requested before completion.
};

/// @return Canonical string form matching JobTracker::toString(JobState).
const char* compileJobStateStr(CompileJobState s) noexcept;

/// A single structured diagnostic entry — schema matches ChuckDiagnosticInfo.
struct CompileJobDiag {
    std::string severity; ///< "error" | "warning" | "info"
    std::string code;     ///< e.g. "CK_OK", "CK_COMPILE_ERROR", "CK_CANCELLED"
    std::string message;  ///< human-readable message
    int         line   = 0;    ///< 1-based line (0 if not determinable)
    int         column = 0;    ///< 1-based column (0 if not determinable)
};

/// Per-job tracking entry, owned by ChuckCkJobService.  The workerThread is held
/// here so shutdown() can join all live compile threads before the owning
/// AudioEngine tears down the AudioWorkerManager (otherwise worker IPC could
/// race with worker process destruction).
struct CompileJobEntry {
    uint64_t jobId = 0;
    uint8_t  tabId = 0;  ///< slot index for worker-side cancel signal

    std::atomic<int>    state{static_cast<int>(CompileJobState::Queued)};
    std::atomic<bool>   cancelRequested{false};
    std::unique_ptr<std::thread> workerThread;

    struct Result {
        bool        success       = false;
        std::string response;          ///< raw worker response (for the onComplete callback)
        std::string sourceHash      = "compiled"; ///< matches compileChuK default
        int         shredId         = -1;
        std::string errorMessage;       ///< populated on failure (parsed error text)
        std::vector<CompileJobDiag> diagnostics;
    };
    Result result;

    /// Guards result writes (worker thread) vs result reads (queryJob).  The
    /// map lookup itself is guarded by ChuckCkJobService::jobsMtx_.
    mutable std::mutex resultMtx;
};

/**
 * ChuckCkJobService — tracks async ChucK compile jobs on the JUCE-free side.
 *
 * AudioEngine owns one instance (created after the worker process is live).
 * startAsyncCkCompile() / queryCkJob() / cancelCkJob() become thin delegates,
 * so the testable state machine + schema logic lives here and is covered by
 * tests/test_b4_k4_ckpt_compile_job.cpp without pulling in JUCE.
 *
 * Thread model:
 *   - startCompile() : any thread (control / AI), non-blocking
 *   - worker thread  : publishes to ChucK via the injected Publisher, updates state
 *   - queryJob()     : any thread, non-blocking (brief jobsMtx_ + per-entry resultMtx)
 *   - cancelJob()    : any thread — sets the cooperative flag AND sends a
 *                      ck_cancel control-plane command via the injected Canceller
 *                      so the worker dispatcher can suppress the handoff shred
 */
class ChuckCkJobService {
public:
    /// Publishes a .ck tab to the ChucK worker.  Returns the raw VMResult (ok +
    /// ck_compile reply).  In production this binds AudioWorkerManager::evaluateCkTab;
    /// in tests it is a fake that returns canned replies.
    using Publisher = std::function<VMResult(uint8_t tabId, const std::string& code)>;

    /// Worker-side cancellation callback (AI-5 Phase 2C).  Invoked by
    /// cancelJob() to send a ck_cancel command through the existing control
    /// plane.  In production this binds AudioWorkerManager::cancelCkCompile.
    using Canceller = std::function<void(uint8_t tabId)>;

    /// Completion callback fired exactly once, on the worker thread, when the
    /// job reaches a terminal state.  Contract matches startAsyncCkCompile():
    /// (success, workerResponse).
    using Completion = std::function<void(bool success, const std::string& response)>;

    explicit ChuckCkJobService(Publisher publish, Canceller cancel = {});
    ~ChuckCkJobService();
    ChuckCkJobService(const ChuckCkJobService&)            = delete;
    ChuckCkJobService& operator=(const ChuckCkJobService&) = delete;

    /// Submit a compile job (mirrors AudioEngine::startAsyncCkCompile).
    /// Validates nothing about tabId range — the caller (AudioEngine) owns the
    /// [0, kNumSlots) invariant before calling.  Returns a non-zero jobId.
    uint64_t startCompile(uint8_t tabId, std::string code, Completion onComplete);

    /// Query job status — canonical JobTracker::queryJob() schema.
    ///   known + terminal : {ok:true, job_id, status, success, result{…}, …}
    ///   known + nonterminal: {ok:true, job_id, status}
    ///   unknown          : {ok:false, error:"unknown job id", job_id}
    nlohmann::json queryJob(uint64_t jobId) const;

    /// Cancel an async ChucK compile job (AI-5 Phase 2C).
    /// Sets the cooperative cancelRequested flag AND sends a ck_cancel
    /// control-plane command to the worker process via the Canceller callback.
    /// The worker dispatcher checks the flag before publishing the handoff
    /// shred, preventing a cancelled job's result from being consumed.
    ///
    /// Return value semantics:
    ///   false — unknown job, or job already in a terminal state (Succeeded,
    ///           Failed, Cancelled). The caller should treat this as "cannot
    ///           cancel"; queryJob() will reveal the actual terminal state.
    ///   true  — cancellation was accepted. The job will transition to
    ///           Cancelled when the worker thread observes cancelRequested.
    bool cancelJob(uint64_t jobId);

    /// Cancel + join every live compile thread.  Called by AudioEngine::shutdownWorker()
    /// before tearing down the AudioWorkerManager so no IPC races the teardown.
    void shutdown();

private:
    void runCompile(std::shared_ptr<CompileJobEntry> entry,
                    uint8_t tabId,
                    std::string code,
                    Completion onComplete);

    /// Parse the worker ck_compile reply into a structured Result, reusing the
    /// compileChuK diagnostic vocabulary (CK_OK / CK_COMPILE_ERROR).  On success
    /// extracts hash= / shred=; on failure extracts error= / line= / col=.
    static void parseResponse(const std::string& response, bool success,
                              CompileJobEntry::Result& out);

    static constexpr int kQueued    = static_cast<int>(CompileJobState::Queued);
    static constexpr int kRunning   = static_cast<int>(CompileJobState::Running);
    static constexpr int kSucceeded = static_cast<int>(CompileJobState::Succeeded);
    static constexpr int kFailed    = static_cast<int>(CompileJobState::Failed);
    static constexpr int kCancelled = static_cast<int>(CompileJobState::Cancelled);

    std::atomic<uint64_t> nextJobId_{1};

    mutable std::mutex                                    jobsMtx_;
    std::unordered_map<uint64_t, std::shared_ptr<CompileJobEntry>> jobs_;

    Publisher publish_;
    Canceller canceller_;
};

} // namespace hathor::audio_worker
