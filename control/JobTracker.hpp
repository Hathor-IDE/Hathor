// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * JobTracker.hpp — canonical asynchronous job infrastructure (AI-1/AI-5/AI-6).
 *
 * Provides a shared async job pool that:
 *   - Accepts jobs for background execution
 *   - Tracks job state (queued, running, succeeded, failed, cancelled)
 *   - Provides job result storage and retrieval
 *   - Supports cancellation via cooperative polling
 *
 * This is NOT AI-specific — it is the same infrastructure that B8-K2 rendering
 * uses (ChuckRenderWriter) and that AI-6 baking will use. The job model is
 * deliberately minimal: submit work → get a job ID → poll or callback for result.
 *
 * Requirement references: AI-1 §1 (canonical job model), AI-5 §5–§6, AI-5 §7,
 *                         AI-5 §16 (shared infrastructure)
 */

#include "ChuckSession.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hathor::control {

/**
 * Job entry — tracks state and result of a single async job.
 */
struct JobEntry {
    uint64_t              jobId;
    std::atomic<JobState> state{JobState::Queued};
    std::atomic<bool>     cancelRequested{false};
    CompileResult         result{};
    std::string           errorMessage;
    std::function<void()> onCancel;  ///< optional cancellation cleanup callback

    JobEntry(uint64_t id) : jobId(id) {}

    /// Mutex to protect result/errorMessage writes from concurrent access
    /// (the worker thread writes; the querying thread may read).
    mutable std::mutex    resultMtx;
};

/**
 * Job entry with embedded work function.
 * The work function is invoked on the worker thread.
 */
struct JobWithWork : public JobEntry {
    std::function<void(std::shared_ptr<JobEntry>)> work;

    template<typename F>
    JobWithWork(uint64_t id, F&& f) : JobEntry(id), work(std::forward<F>(f)) {}
};

/**
 * JobTracker — shared async job pool with a single worker thread.
 *
 * Thread model:
 *   - submit()       : called from any thread (non-blocking)
 *   - worker thread  : runs jobs sequentially, invokes completion callbacks
 *   - queryJob()     : called from any thread (non-blocking, lock-free-ish)
 *   - cancelJob()     : called from any thread (sets flag, checks if already running)
 *
 * The worker thread uses a mutex + condition_variable to pick up jobs.
 * Jobs are stored in an unordered_map for O(1) lookup by ID.
 */
class JobTracker {
public:
    JobTracker();
    ~JobTracker();

    JobTracker(const JobTracker&) = delete;
    JobTracker& operator=(const JobTracker&) = delete;

    /**
     * Submit a job for background execution.
     *
     * @param work  The function to execute on the worker thread.
     *              Must check jobEntry->cancelRequested periodically and
     *              return early if set.
     * @param onCancel  Optional callback invoked if the job is cancelled
     *                  (on the worker thread).  Used for cleanup.
     * @return The unique job ID.
     */
    uint64_t submit(std::function<void(std::shared_ptr<JobEntry>)> work,
                    std::function<void()> onCancel = nullptr);

    /**
     * Query the status and result of a job.
     *
     * @param jobId  The job ID returned by submit().
     * @return JSON with job_id, status, and (if complete) the result.
     */
    nlohmann::json queryJob(uint64_t jobId) const;

    /**
     * Cancel a job.
     *
     * If the job is queued: it will be removed from the queue and marked
     * Cancelled.  If the job is running: the cancelRequested flag is set;
     * the job implementation must poll this flag and exit early.  If the job
     * is already complete, this is a no-op.
     *
     * @return true if the cancellation was accepted (queued or running),
     *         false if the job was already complete or not found.
     */
    bool cancelJob(uint64_t jobId);

    /// Number of jobs currently tracked (any state).
    int jobCount() const noexcept;

private:
    void workerLoop();

    std::atomic<uint64_t> nextJobId_{1};

    mutable std::mutex    jobsMtx_;
    std::unordered_map<uint64_t, std::shared_ptr<JobEntry>> jobs_;
    std::queue<std::shared_ptr<JobEntry>> pendingQueue_;

    std::mutex              queueMtx_;
    std::condition_variable cv_;
    std::thread             workerThread_;
    std::atomic<bool>       shutdown_{false};
};

} // namespace hathor::control
