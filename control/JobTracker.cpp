// ---------------------------------------------------------------------------
// JobTracker — canonical asynchronous job infrastructure (AI-1/AI-5/AI-6)
// ---------------------------------------------------------------------------

#include "JobTracker.hpp"

namespace hathor::control {

JobTracker::JobTracker()
    : workerThread_([this] { workerLoop(); })
{}

JobTracker::~JobTracker()
{
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        shutdown_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    if (workerThread_.joinable())
        workerThread_.join();
}

uint64_t JobTracker::submit(
    std::function<void(std::shared_ptr<JobEntry>)> work,
    std::function<void()> onCancel)
{
    const uint64_t id = nextJobId_.fetch_add(1, std::memory_order_acq_rel);

    auto entry = std::make_shared<JobWithWork>(id, std::move(work));
    entry->onCancel = std::move(onCancel);

    {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        jobs_[id] = entry;
        pendingQueue_.push(entry);
    }

    cv_.notify_one();
    return id;
}

nlohmann::json JobTracker::queryJob(uint64_t jobId) const
{
    std::shared_ptr<JobEntry> entry;
    {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        auto it = jobs_.find(jobId);
        if (it == jobs_.end()) {
            return nlohmann::json{
                {"ok",     false},
                {"error",  "unknown job id"},
                {"job_id", jobId}
            };
        }
        entry = it->second;
    }

    nlohmann::json result = {
        {"ok",     true},
        {"job_id", jobId},
        {"status", toString(entry->state)}
    };

    if (entry->externJobId > 0)
        result["extern_job_id"] = entry->externJobId;

    if (entry->state == JobState::Succeeded) {
        result["success"] = true;
        nlohmann::json diags = nlohmann::json::array();
        for (const auto& d : entry->result.diagnostics) {
            diags.push_back(nlohmann::json{
                {"severity", d.severity},
                {"code",     d.code},
                {"message",  d.message},
                {"line",     d.line},
                {"column",   d.column}
            });
        }
        result["result"] = {
            {"success",        true},
            {"diagnostics",    std::move(diags)},
            {"source_hash",    entry->result.sourceHash},
            {"shred_id",       entry->result.shredId}
        };
    } else if (entry->state == JobState::Failed) {
        result["success"] = false;
        result["error"] = entry->errorMessage.empty()
                          ? "compile failed"
                          : entry->errorMessage;
        nlohmann::json diags = nlohmann::json::array();
        for (const auto& d : entry->result.diagnostics) {
            diags.push_back(nlohmann::json{
                {"severity", d.severity},
                {"code",     d.code},
                {"message",  d.message},
                {"line",     d.line},
                {"column",   d.column}
            });
        }
        result["result"] = {
            {"success",     false},
            {"diagnostics", std::move(diags)},
            {"error",       entry->errorMessage.empty()
                            ? "compile failed"
                            : entry->errorMessage}
        };
    } else if (entry->state == JobState::Cancelled) {
        result["success"] = false;
        result["error"] = "job cancelled";
    }

    return result;
}

bool JobTracker::cancelJob(uint64_t jobId)
{
    std::shared_ptr<JobEntry> entry;
    {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        auto it = jobs_.find(jobId);
        if (it == jobs_.end())
            return false;
        entry = it->second;
    }

    const JobState currentState = entry->state;
    if (currentState == JobState::Succeeded ||
        currentState == JobState::Failed ||
        currentState == JobState::Cancelled)
        return false;

    entry->cancelRequested.store(true, std::memory_order_release);

    if (currentState == JobState::Queued) {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        entry->state = JobState::Cancelled;
        return true;
    }

    // Currently running — set the flag; the worker thread will observe it.
    return true;
}

int JobTracker::jobCount() const noexcept
{
    std::lock_guard<std::mutex> lock(jobsMtx_);
    return static_cast<int>(jobs_.size());
}

void JobTracker::workerLoop()
{
    while (true) {
        std::shared_ptr<JobEntry> entry;

        {
            std::unique_lock<std::mutex> lock(queueMtx_);
            cv_.wait(lock, [this] {
                return shutdown_.load(std::memory_order_acquire) || !pendingQueue_.empty();
            });

            if (shutdown_.load(std::memory_order_acquire) && pendingQueue_.empty())
                return;

            // Pop the next pending job
            entry = pendingQueue_.front();
            pendingQueue_.pop();
        }

        if (!entry)
            continue;

        // Check if it was cancelled while queued
        if (entry->state == JobState::Cancelled)
            continue;

        // Check cancellation flag
        if (entry->cancelRequested.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(jobsMtx_);
            entry->state = JobState::Cancelled;
            continue;
        }

        // Mark as running
        {
            std::lock_guard<std::mutex> lock(jobsMtx_);
            auto it = jobs_.find(entry->jobId);
            if (it == jobs_.end())
                continue;
            entry->state = JobState::Running;
        }

        // Downcast to access the work function
        auto workEntry = std::static_pointer_cast<JobWithWork>(entry);
        if (!workEntry)
            continue;

        try {
            workEntry->work(entry);
        } catch (...) {
            std::lock_guard<std::mutex> lock(jobsMtx_);
            entry->state = JobState::Failed;
            entry->errorMessage = "unhandled exception in job";
        }
    }
}

} // namespace hathor::control
