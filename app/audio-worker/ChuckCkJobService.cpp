// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ChuckCkJobService.hpp"

#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hathor::audio_worker {

const char* compileJobStateStr(CompileJobState s) noexcept
{
    switch (s) {
        case CompileJobState::Queued:     return "queued";
        case CompileJobState::Running:    return "running";
        case CompileJobState::Succeeded:  return "succeeded";
        case CompileJobState::Failed:     return "failed";
        case CompileJobState::Cancelled:  return "cancelled";
    }
    return "unknown";
}

static int parseTokenInt(const std::string& s, const std::string& key) noexcept
{
    const auto pos = s.find(key);
    if (pos == std::string::npos)
        return 0; // AI-5 §9: 0 when not determinable, never fabricated.
    try {
        return std::stoi(s.substr(pos + key.size()));
    } catch (...) {
        return 0;
    }
}

ChuckCkJobService::ChuckCkJobService(Publisher publish)
    : publish_(std::move(publish))
{
}

ChuckCkJobService::~ChuckCkJobService()
{
    shutdown();
}

uint64_t ChuckCkJobService::startCompile(uint8_t tabId, std::string code,
                                         Completion onComplete)
{
    // Monotonic, non-zero job ID (same pattern as ChuckRenderWriter::nextJobId_).
    const uint64_t jobId = nextJobId_.fetch_add(1, std::memory_order_relaxed);

    auto entry = std::make_shared<CompileJobEntry>();
    entry->jobId = jobId;
    entry->state.store(kQueued, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        jobs_[jobId] = entry;
    }

    // Spawn the worker thread. It lives in the entry so shutdown() can join it.
    entry->workerThread = std::make_unique<std::thread>(
        [this, entry, tabId, code = std::move(code),
         onComplete = std::move(onComplete)]() mutable {
            runCompile(entry, tabId, std::move(code), onComplete);
        });

    return jobId;
}

void ChuckCkJobService::runCompile(std::shared_ptr<CompileJobEntry> entry,
                                  uint8_t tabId,
                                  std::string code,
                                  Completion onComplete)
{
    // Honour a cancellation requested before the thread began.
    if (entry->cancelRequested.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(entry->resultMtx);
            entry->result.success = false;
            entry->result.response = "job cancelled";
            entry->result.errorMessage = "job cancelled";
            entry->result.diagnostics.push_back(
                {"warning", "CK_CANCELLED", "Compilation job was cancelled", 0, 0});
        }
        entry->state.store(kCancelled, std::memory_order_release);
        if (onComplete)
            onComplete(false, "job cancelled");
        return;
    }

    // Observe the Running state BEFORE the (potentially long) IPC so poll
    // results report "running" rather than skipping straight to terminal.
    entry->state.store(kRunning, std::memory_order_release);

    VMResult vm{false, 1, "audio worker not available"};
    if (publish_)
        vm = publish_(tabId, code);

    // Observe cancellation requested while blocked on the IPC round-trip.
    if (entry->cancelRequested.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(entry->resultMtx);
            entry->result.success = false;
            entry->result.response = "job cancelled";
            entry->result.errorMessage = "job cancelled";
            entry->result.diagnostics.push_back(
                {"warning", "CK_CANCELLED", "Compilation job was cancelled", 0, 0});
        }
        entry->state.store(kCancelled, std::memory_order_release);
        if (onComplete)
            onComplete(false, "job cancelled");
        return;
    }

    // Build the structured result under the per-entry lock (fixes the data race
    // that the old inline implementation had on entry->response).
    {
        std::lock_guard<std::mutex> lock(entry->resultMtx);
        entry->result.response = vm.message;
        parseResponse(vm.message, vm.ok, entry->result);
    }

    // Status is published AFTER the result is fully written; queryJob() loads
    // status (acquire) and then takes resultMtx, so it never observes a terminal
    // status with a half-written result.
    entry->state.store(vm.ok ? kSucceeded : kFailed, std::memory_order_release);

    if (onComplete)
        onComplete(vm.ok, vm.message);
}

void ChuckCkJobService::parseResponse(const std::string& response, bool success,
                                      CompileJobEntry::Result& out)
{
    out.success = success;
    out.sourceHash = "compiled"; // compileChuK default; overridden by worker "hash="
    out.shredId = -1;
    out.errorMessage.clear();
    out.diagnostics.clear();

    if (success) {
        // "ok ck_compile tab=<id> version=<v> hash=<h> [shred=<n>]"
        const auto hashPos = response.find("hash=");
        if (hashPos != std::string::npos) {
            const auto end = response.find(' ', hashPos);
            out.sourceHash = (end == std::string::npos)
                ? response.substr(hashPos + 5)
                : response.substr(hashPos + 5, end - (hashPos + 5));
        }

        const auto shredPos = response.find("shred=");
        if (shredPos != std::string::npos) {
            try {
                out.shredId = std::stoi(response.substr(shredPos + 6));
            } catch (...) {
                out.shredId = -1;
            }
        }

        // Reuse compileChuK's exact success diagnostic.
        out.diagnostics.push_back(
            {"info", "CK_OK", "ChucK source compiled and published", 0, 0});
    } else {
        // "err ck_compile tab=<id> version=<v> error=<msg> [line=<l> col=<c>]"
        // or a bare transport error such as "audio worker not available".
        std::string message = response;
        if (const auto errPos = response.find("error="); errPos != std::string::npos) {
            const auto linePos = response.find(" line=", errPos);
            const size_t msgStart = errPos + 6;
            message = (linePos == std::string::npos)
                ? response.substr(msgStart)
                : response.substr(msgStart, linePos - msgStart);
        }

        const int line = parseTokenInt(response, "line=");
        const int col  = parseTokenInt(response, "col=");

        out.errorMessage = message;
        out.diagnostics.push_back(
            {"error", "CK_COMPILE_ERROR", message, line, col});
    }
}

nlohmann::json ChuckCkJobService::queryJob(uint64_t jobId) const
{
    std::shared_ptr<CompileJobEntry> entry;
    {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        const auto it = jobs_.find(jobId);
        if (it == jobs_.end()) {
            return nlohmann::json{
                {"ok", false},
                {"error", "unknown job id"},
                {"job_id", jobId}
            };
        }
        entry = it->second;
    }

    const int state = entry->state.load(std::memory_order_acquire);

    nlohmann::json result = {
        {"ok", true},
        {"job_id", jobId},
        {"status", compileJobStateStr(static_cast<CompileJobState>(state))}
    };

    // Snapshot the result under the per-entry lock so diagnostics +
    // errorMessage are coherent with the reported status.
    CompileJobEntry::Result snap;
    {
        std::lock_guard<std::mutex> lock(entry->resultMtx);
        snap = entry->result;
    }

    switch (state) {
        case kSucceeded: {
            nlohmann::json diags = nlohmann::json::array();
            for (const auto& d : snap.diagnostics)
                diags.push_back({
                    {"severity", d.severity},
                    {"code", d.code},
                    {"message", d.message},
                    {"line", d.line},
                    {"column", d.column}
                });
            result["success"] = true;
            result["result"] = {
                {"success", true},
                {"diagnostics", std::move(diags)},
                {"source_hash", snap.sourceHash},
                {"shred_id", snap.shredId}
            };
            break;
        }
        case kFailed: {
            nlohmann::json diags = nlohmann::json::array();
            for (const auto& d : snap.diagnostics)
                diags.push_back({
                    {"severity", d.severity},
                    {"code", d.code},
                    {"message", d.message},
                    {"line", d.line},
                    {"column", d.column}
                });
            const std::string err = snap.errorMessage.empty()
                ? std::string("compile failed")
                : snap.errorMessage;
            result["success"] = false;
            result["error"] = err;
            result["result"] = {
                {"success", false},
                {"diagnostics", std::move(diags)},
                {"error", err}
            };
            break;
        }
        case kCancelled:
            result["success"] = false;
            result["error"] = "job cancelled";
            break;
        default:
            // Queued / Running: only the base {ok, job_id, status} are emitted,
            // exactly like JobTracker::queryJob for non-terminal states.
            break;
    }

    return result;
}

bool ChuckCkJobService::cancelJob(uint64_t jobId)
{
    std::shared_ptr<CompileJobEntry> entry;
    {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        const auto it = jobs_.find(jobId);
        if (it == jobs_.end())
            return false;
        entry = it->second;
    }

    const int state = entry->state.load(std::memory_order_acquire);
    if (state == kSucceeded || state == kFailed || state == kCancelled)
        return false; // already terminal

    entry->cancelRequested.store(true, std::memory_order_release);
    return true;
}

void ChuckCkJobService::shutdown()
{
    // Snapshot the live entries, then cancel + join each thread without holding
    // jobsMtx_ during the join (a join may block on worker IPC).
    std::vector<std::shared_ptr<CompileJobEntry>> live;
    {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        live.reserve(jobs_.size());
        for (const auto& [_, e] : jobs_)
            live.push_back(e);
    }

    for (const auto& e : live) {
        e->cancelRequested.store(true, std::memory_order_release);
        if (e->workerThread && e->workerThread->joinable())
            e->workerThread->join();
    }
}

} // namespace hathor::audio_worker
