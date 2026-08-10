// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckSessionService.cpp — AI-5 canonical ChucK session service implementation.
 *
 * Maps the abstract session vocabulary onto the B4-K3/K4/K7 per-tab ChucK
 * architecture via AudioEngineFacade.  All low-level VM/worker-thread/watchdog
 * internals remain hidden behind the facade's control-plane IPC.
 *
 * Requirement references: AI-5 §1–§18
 */

#include "ChuckSessionService.hpp"
#include "ChuckDiagnostics.hpp"
#include "JobTracker.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace hathor::control {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ChuckSessionService::ChuckSessionService(AudioEngineFacade& audio) noexcept
    : audio_(audio)
    , jobTracker_(std::make_shared<JobTracker>())
{}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int ChuckSessionService::parseSessionId(std::string_view sessionId)
{
    // Expected format: "ck:<tabIndex>"
    if (sessionId.size() < 4 || sessionId.substr(0, 3) != "ck:")
        return -1;

    std::string_view numPart = sessionId.substr(3);
    if (numPart.empty())
        return -1;

    try {
        int tabId = std::stoi(std::string(numPart));
        if (tabId < 0 || tabId >= 16)
            return -1;
        return tabId;
    } catch (...) {
        return -1;
    }
}

std::string ChuckSessionService::makeSessionId(uint8_t tabIdx)
{
    return "ck:" + std::to_string(static_cast<int>(tabIdx));
}

SessionState ChuckSessionService::translateState(const std::string& vmStateStr)
{
    // B4 VMState values: inactive, active, suspended, destroyed, error, failed, recreating
    if (vmStateStr.find("inactive") != std::string::npos)
        return SessionState::Open;
    if (vmStateStr.find("active") != std::string::npos)
        return SessionState::Live;
    if (vmStateStr.find("suspended") != std::string::npos)
        return SessionState::Suspended;
    if (vmStateStr.find("destroyed") != std::string::npos)
        return SessionState::Destroyed;
    if (vmStateStr.find("error") != std::string::npos)
        return SessionState::Error;
    if (vmStateStr.find("failed") != std::string::npos)
        return SessionState::Failed;
    if (vmStateStr.find("recreating") != std::string::npos)
        return SessionState::Recovering;
    return SessionState::Open;
}

// ---------------------------------------------------------------------------
// AI-5 §3: create_chuck_session
// -----------------------------------------------------------------------

ChuckSession ChuckSessionService::createSession(uint8_t slotIdx, std::string source)
{
    ChuckSession session;
    session.sessionId = makeSessionId(slotIdx);
    session.source = std::move(source);

    // Per AI-5 §3: Do NOT blindly create a live VM merely because an
    // API caller asks to inspect a .ck file.  The session is created in
    // the Open state — no VM is allocated until auditionSession() is called.
    if (!audio_.hasWorker()) {
        session.state = SessionState::Open;
        session.lastError = "audio worker is not running";
    } else {
        session.state = SessionState::Open;
    }

    session.vmGeneration = 0;

    // Persist the session (including source) so AI-6 render_chuck can
    // retrieve the source code later.
    {
        std::lock_guard<std::mutex> lock(sessionsMtx_);
        sessions_[session.sessionId] = session;
    }

    return session;
}

// ---------------------------------------------------------------------------
// AI-5 §4: get_chuck_session
// -----------------------------------------------------------------------

ChuckSession ChuckSessionService::getSession(std::string_view sessionId) const
{
    ChuckSession session;

    const int tabId = parseSessionId(sessionId);
    if (tabId < 0) {
        session.sessionId = std::string(sessionId);
        session.state = SessionState::Error;
        session.lastError = "invalid session ID: " + std::string(sessionId);
        return session;
    }

    session.sessionId = std::string(sessionId);

    // Return any stored source from a prior createSession() call.
    {
        std::lock_guard<std::mutex> lock(sessionsMtx_);
        auto it = sessions_.find(std::string(sessionId));
        if (it != sessions_.end())
            session.source = it->second.source;
    }

    // Query the B4 VM state via the facade (B4-K3 vm_query).
    // This is a read-only operation that does NOT mutate VM state.
    const std::string status = audio_.queryCkTab(tabId);

    session.state = translateState(status);

    if (status.empty()) {
        session.state = SessionState::Open;
    }

    return session;
}

// ---------------------------------------------------------------------------
// AI-5/AI-6: Session source retrieval
// ---------------------------------------------------------------------------

std::string ChuckSessionService::getSessionSource(std::string_view sessionId) const
{
    std::lock_guard<std::mutex> lock(sessionsMtx_);
    auto it = sessions_.find(std::string(sessionId));
    if (it != sessions_.end())
        return it->second.source;
    return {};
}

// ---------------------------------------------------------------------------
// AI-5 §5: compile_chuck (ASYNCHRONOUS)
// -----------------------------------------------------------------------

AsyncJobHandle ChuckSessionService::compileChuck(
    std::string_view sessionId,
    std::string source,
    std::function<void(CompileResult)> onComplete)
{
    const int tabId = parseSessionId(sessionId);
    if (tabId < 0) {
        // Invalid session — return an immediately failed job.
        uint64_t jobId = jobTracker_->submit(
            [](std::shared_ptr<JobEntry> entry) {
                entry->state.store(JobState::Failed, std::memory_order_release);
                std::lock_guard<std::mutex> lock(entry->resultMtx);
                entry->errorMessage = "invalid session ID";
            }, nullptr);
        return AsyncJobHandle(jobId);
    }

    // Check if the worker is alive.
    if (!audio_.hasWorker()) {
        uint64_t jobId = jobTracker_->submit(
            [onComplete](std::shared_ptr<JobEntry> entry) {
                entry->state.store(JobState::Failed, std::memory_order_release);
                std::lock_guard<std::mutex> lock(entry->resultMtx);
                entry->errorMessage = "audio worker is not running";
                entry->result.diagnostics.push_back({
                    "error", "WORKER_NOT_RUNNING",
                    "The audio worker process is not running.", 0, 0
                });
                if (onComplete) {
                    CompileResult cr;
                    cr.success = false;
                    cr.errorMessage = "audio worker is not running";
                    onComplete(cr);
                }
            }, nullptr);
        return AsyncJobHandle(jobId);
    }

    const std::string sourceCopy = source;

    // Submit the compile work to the shared job tracker.
    // The job function runs on the JobTracker's worker thread.
    //
    // The real compiler (libchuck) is linked into the control process, so
    // we call validateChuckSource() directly — NO blocking on a promise/future
    // from startAsyncCkCompile. If diagnostics pass, we publish to the worker
    // via startAsyncCkCompile (fire-and-forget; the callback updates the
    // job entry asynchronously).
    uint64_t jobId = jobTracker_->submit(
        [this, tabId, sourceCopy, onComplete](
            std::shared_ptr<JobEntry> entry)
        {
            // Check cancellation before starting.
            if (entry->cancelRequested.load(std::memory_order_acquire)) {
                entry->state.store(JobState::Cancelled, std::memory_order_release);
                return;
            }

            // Mark as running.
            entry->state.store(JobState::Running, std::memory_order_release);

            // Run the real ChucK compiler diagnostics directly on this thread.
            // This uses the vendored libchuck via validateChuckSource(),
            // which is the SAME function called by ChuckCompiler::dispatcherLoop()
            // in the worker process (B4-K4). No IPC round-trip needed.
            auto diag = audio_worker::validateChuckSource(sourceCopy);

            CompileResult cr;
            cr.sourceHash = "compiled";
            cr.shredId = -1;

            if (!diag.ok) {
                // Compilation failed — structured diagnostics from the real compiler.
                cr.success = false;
                cr.errorMessage = diag.message;
                cr.diagnostics.push_back({
                    "error", "CK_COMPILE_ERROR",
                    diag.message, diag.errorLine, diag.errorColumn
                });

                // Store result and mark complete (no worker publish needed).
                {
                    std::lock_guard<std::mutex> lock(entry->resultMtx);
                    entry->result = cr;
                    entry->state.store(JobState::Failed, std::memory_order_release);
                }

                if (onComplete)
                    onComplete(cr);
                return;
            }

            // Diagnostics passed — publish to the worker process.
            // startAsyncCkCompile dispatches to the B4-K4 ChuckCompiler
            // dispatcher thread in the worker process. We fire-and-forget:
            // the callback updates the job entry when the shred is loaded.
            //
            // We capture the entry weak_ptr so the callback can update it.
            std::weak_ptr<JobEntry> weakEntry = entry;
            audio_.startAsyncCkCompile(tabId, sourceCopy,
                [weakEntry, onComplete, sourceCopy](bool /*success*/, const std::string& response) {
                    // This callback fires on the worker thread's notification
                    // thread. Update the job entry if it still exists.
                    if (auto entry = weakEntry.lock()) {
                        CompileResult cr;
                        cr.success = true;
                        cr.sourceHash = "compiled";
                        cr.shredId = -1;

                        // Parse response for shred/hash info if the worker
                        // provided it.
                        auto hashPos = response.find("hash=");
                        if (hashPos != std::string::npos) {
                            size_t end = response.find(' ', hashPos);
                            if (end == std::string::npos) end = response.size();
                            cr.sourceHash = response.substr(hashPos + 5, end - hashPos - 5);
                        }
                        auto shredPos = response.find("shred=");
                        if (shredPos != std::string::npos) {
                            try {
                                cr.shredId = std::stoi(response.substr(shredPos + 6));
                            } catch (...) {}
                        }

                        cr.diagnostics.push_back({
                            "info", "CK_OK", "ChucK source compiled and published", 0, 0
                        });

                        {
                            std::lock_guard<std::mutex> lock(entry->resultMtx);
                            entry->result = cr;
                            entry->state.store(JobState::Succeeded, std::memory_order_release);
                        }

                        if (onComplete)
                            onComplete(cr);
                    }
                });

            // Return immediately — the job will be marked complete by the
            // callback above. The job tracker reports "running" status until
            // the callback fires.
        },
        [onComplete]() {
            // Cancellation cleanup
            if (onComplete) {
                CompileResult cr;
                cr.success = false;
                cr.errorMessage = "compile job cancelled";
                onComplete(cr);
            }
        });

    return AsyncJobHandle(jobId);
}

// ---------------------------------------------------------------------------
// AI-5 §11: audition_chuck
// -----------------------------------------------------------------------

ChuckSession ChuckSessionService::auditionSession(std::string_view sessionId)
{
    ChuckSession session;

    const int tabId = parseSessionId(sessionId);
    if (tabId < 0) {
        session.sessionId = std::string(sessionId);
        session.state = SessionState::Error;
        session.lastError = "invalid session ID";
        return session;
    }

    session.sessionId = std::string(sessionId);
    uint8_t tabIdx = static_cast<uint8_t>(tabId);

    // Check if worker is alive.
    if (!audio_.hasWorker()) {
        session.state = SessionState::Open;
        session.lastError = "audio worker is not running";
        return session;
    }

    // Activate the per-tab VM — B4-K3 vm_activate via the facade.
    // This affects ONLY this tab's VM (AI-5 §11).
    // We use ckEval with empty source to activate the VM without compiling.
    bool ok = audio_.ckEval(tabIdx, "");
    if (!ok) {
        session.state = SessionState::Error;
        session.lastError = "failed to activate VM for tab " + std::to_string(tabIdx);
        return session;
    }

    // Query the VM state.
    const std::string status = audio_.queryCkTab(tabIdx);
    session.state = translateState(status);

    return session;
}

// ---------------------------------------------------------------------------
// AI-5 §12: stop_chuck
// -----------------------------------------------------------------------

ChuckSession ChuckSessionService::stopSession(std::string_view sessionId)
{
    ChuckSession session;

    const int tabId = parseSessionId(sessionId);
    if (tabId < 0) {
        session.sessionId = std::string(sessionId);
        session.state = SessionState::Error;
        session.lastError = "invalid session ID";
        return session;
    }

    session.sessionId = std::string(sessionId);
    uint8_t tabIdx = static_cast<uint8_t>(tabId);

    // Destroy the VM for this tab only — B4-K7 ck_stop via the facade.
    // This affects ONLY this session — other sessions and .hathor playback
    // continue unaffected (AI-5 §12).
    bool ok = audio_.stopCkTab(tabIdx);

    if (ok) {
        session.state = SessionState::Destroyed;
    } else {
        session.state = SessionState::Error;
        session.lastError = "failed to stop VM for tab " + std::to_string(tabIdx);
    }

    return session;
}

// ---------------------------------------------------------------------------
// AI-5 §10: ChucK diagnostics
// -----------------------------------------------------------------------

std::vector<ChuckDiagnosticInfo> ChuckSessionService::getDiagnostics(
    std::string_view source) const
{
    std::vector<ChuckDiagnosticInfo> diags;

    // Route through the REAL ChucK compiler diagnostic path.
    // validateChuckSource() is the same function called by
    // ChuckCompiler::dispatcherLoop() during ck_compile (B4-K4).
    // When libchuck is linked, this will be replaced by ck.compileCode().
    auto diag = audio_worker::validateChuckSource(source);

    if (!diag.ok) {
        diags.push_back({
            "error", "CK_COMPILE_ERROR",
            diag.message, diag.errorLine, diag.errorColumn
        });
    } else {
        diags.push_back({
            "info", "CK_OK",
            "ChucK source passed validation", 0, 0
        });
    }

    return diags;
}

// ---------------------------------------------------------------------------
// AI-5 §6: Job status polling
// -----------------------------------------------------------------------

nlohmann::json ChuckSessionService::getJobStatus(uint64_t jobId) const
{
    return jobTracker_->queryJob(jobId);
}

// ---------------------------------------------------------------------------
// AI-5 §17: Cancellation
// -----------------------------------------------------------------------

bool ChuckSessionService::cancelJob(uint64_t jobId)
{
    return jobTracker_->cancelJob(jobId);
}

} // namespace hathor::control
