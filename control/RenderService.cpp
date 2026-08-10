// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * RenderService.cpp — AI-6 canonical rendering service implementation.
 *
 * Requirement references: AI-1 §1, AI-5 §16, B8-K1, B8-K2, B8-K4
 */

#include "RenderService.hpp"
#include "JobTracker.hpp"
#include "ChuckSessionService.hpp"
#include "../app/AudioEngineFacade.hpp"
#include "../app/AssetPathResolver.hpp"
#include "../app/ChuckRenderWriter.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace hathor::control {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RenderService::RenderService(AudioEngineFacade&       audio,
                             SampleBank&              bank,
                             ChuckSessionService&     sessions) noexcept
    : audio_(audio)
    , bank_(bank)
    , sessions_(sessions)
    , jobTracker_(std::make_shared<JobTracker>())
{}

// ---------------------------------------------------------------------------
// Helper: asset name validation (reject unsafe, do NOT sanitize)
// ---------------------------------------------------------------------------

static bool isAssetNameSafe(std::string_view name) noexcept
{
    if (name.empty())
        return false;

    for (char c : name) {
        if (c == '/' || c == '\\' || c == '\0')
            return false;
    }

    if (name == "." || name == "..")
        return false;

    bool hasNonDot = false;
    for (char c : name) {
        if (c != '.') { hasNonDot = true; break; }
    }
    if (!hasNonDot)
        return false;

    if (name.front() == '.')
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// AI-6 §2: render_chuck
// ---------------------------------------------------------------------------

uint64_t RenderService::renderChuck(std::string_view sessionId,
                                    int durationBars,
                                    std::string_view assetName,
                                    hathor::AssetTarget target)
{
    // -----------------------------------------------------------------------
    // Step 1: Validate asset name (reject unsafe, do not sanitize)
    // -----------------------------------------------------------------------
    if (!isAssetNameSafe(assetName)) {
        auto jobId = jobTracker_->submit(
            [](std::shared_ptr<JobEntry> entry) {
                entry->state.store(JobState::Failed, std::memory_order_release);
                std::lock_guard<std::mutex> lock(entry->resultMtx);
                entry->errorMessage = "asset name contains path separators or is unsafe";
            }, nullptr);
        return jobId;
    }

    // -----------------------------------------------------------------------
    // Step 2: Validate duration
    // -----------------------------------------------------------------------
    if (durationBars <= 0) {
        auto jobId = jobTracker_->submit(
            [](std::shared_ptr<JobEntry> entry) {
                entry->state.store(JobState::Failed, std::memory_order_release);
                std::lock_guard<std::mutex> lock(entry->resultMtx);
                entry->errorMessage = "duration_bars must be positive";
            }, nullptr);
        return jobId;
    }

    // -----------------------------------------------------------------------
    // Step 3: Resolve session source
    // -----------------------------------------------------------------------
    const std::string source = sessions_.getSessionSource(sessionId);

    // Parse tabId from session ID ("ck:<N>")
    int tabId = -1;
    if (sessionId.size() >= 4 && sessionId.substr(0, 3) == "ck:") {
        try {
            tabId = std::stoi(std::string(sessionId.substr(3)));
            if (tabId < 0 || tabId >= 16) tabId = -1;
        } catch (...) { tabId = -1; }
    }

    if (tabId < 0) {
        auto jobId = jobTracker_->submit(
            [](std::shared_ptr<JobEntry> entry) {
                entry->state.store(JobState::Failed, std::memory_order_release);
                std::lock_guard<std::mutex> lock(entry->resultMtx);
                entry->errorMessage = "invalid session ID";
            }, nullptr);
        return jobId;
    }

    if (source.empty()) {
        auto jobId = jobTracker_->submit(
            [](std::shared_ptr<JobEntry> entry) {
                entry->state.store(JobState::Failed, std::memory_order_release);
                std::lock_guard<std::mutex> lock(entry->resultMtx);
                entry->errorMessage = "session has no source code";
            }, nullptr);
        return jobId;
    }

    // -----------------------------------------------------------------------
    // Step 4: Compute render parameters
    // -----------------------------------------------------------------------
    const double bpm = audio_.getBpm();
    const auto audioStatus = audio_.getAudioStatus();
    const unsigned sampleRate = audioStatus.sampleRate > 0
        ? audioStatus.sampleRate : 44100u;
    const uint64_t numSamples = static_cast<uint64_t>(
        static_cast<uint64_t>(durationBars) * (60.0 / bpm) * 4.0
        * static_cast<double>(sampleRate));

    // -----------------------------------------------------------------------
    // Step 5: Resolve paths
    // -----------------------------------------------------------------------
    const std::string safeName = hathor::sanitizeAssetName(assetName);
    const auto projectDir = audio_.currentProjectDir();

    // Temp path — system temp directory, NOT in the project asset tree.
    // This ensures the render is NON-DESTRUCTIVE (AI-6 §3).
    std::ostringstream tempStream;
    tempStream << "hathor_render_"
               << std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count()
               << ".wav";
    const std::filesystem::path tempPath =
        std::filesystem::temp_directory_path() / tempStream.str();

    std::error_code ec;
    std::filesystem::create_directories(tempPath.parent_path(), ec);

    // Final paths (resolved but NOT written until commit).
    std::filesystem::path finalWavPath, finalCkPath;
    {
        hathor::AssetPathResolver resolver(projectDir);
        auto wavResult = resolver.resolveStudio(safeName);
        if (!wavResult.ok) {
            auto jobId = jobTracker_->submit(
                [msg = wavResult.error](std::shared_ptr<JobEntry> entry) {
                    entry->state.store(JobState::Failed, std::memory_order_release);
                    std::lock_guard<std::mutex> lock(entry->resultMtx);
                    entry->errorMessage = msg;
                }, nullptr);
            return jobId;
        }
        finalWavPath = wavResult.path;
        finalCkPath  = finalWavPath;
        finalCkPath.replace_extension(".ck");
    }

    // -----------------------------------------------------------------------
    // Step 6: Populate RenderJobData and submit
    // -----------------------------------------------------------------------
    auto data = std::make_shared<RenderJobData>();
    data->sessionId     = std::string(sessionId);
    data->tabId         = static_cast<uint8_t>(tabId);
    data->sanitizedName = safeName;
    data->ckSource      = source;
    data->target        = target;
    data->durationBars  = durationBars;
    data->numSamples    = numSamples;
    data->sampleRate    = sampleRate;
    data->bpm           = bpm;
    data->tempPath      = tempPath;
    data->finalWavPath  = finalWavPath;
    data->finalCkPath   = finalCkPath;
    data->projectDir    = projectDir;

    const uint64_t jobId = jobTracker_->submit(
        [this, data, numSamples, sampleRate](std::shared_ptr<JobEntry> entry)
        {
            // Set the job ID on the render data.
            data->jobId = entry->jobId;

            // Register render job data immediately so cancelJob() can find it.
            {
                std::lock_guard<std::mutex> lock(jobsMtx_);
                renderJobs_[entry->jobId] = data;
            }

            // Check cancellation before starting.
            if (entry->cancelRequested.load(std::memory_order_acquire)) {
                entry->state.store(JobState::Cancelled, std::memory_order_release);
                return;
            }

            // Mark as running.
            entry->state.store(JobState::Running, std::memory_order_release);

            // Check if the worker is available.
            if (!audio_.hasWorker()) {
                std::lock_guard<std::mutex> lock(entry->resultMtx);
                entry->errorMessage = "audio worker is not running";
                entry->state.store(JobState::Failed, std::memory_order_release);
                return;
            }

            // Capture weak references for the completion callback.
            std::weak_ptr<JobEntry> weakEntry = entry;
            std::weak_ptr<RenderJobData> weakData = data;

            // Start the render via the facade's non-registering entry point (B8-K2).
            // The render writes to the temp path and calls the callback
            // when complete (on the ChuckRenderWriter's background thread).
            hathor::ChuckRenderWriter::CompletionCallback onComplete =
                [weakEntry, weakData](const hathor::RenderResult& result)
                {
                    // Fires on the ChuckRenderWriter background thread.

                    // Update the job entry state.
                    if (auto locked = weakEntry.lock()) {
                        if (result.success) {
                            locked->state.store(JobState::Succeeded,
                                                  std::memory_order_release);
                        } else if (result.state == hathor::RenderState::Cancelled) {
                            locked->state.store(JobState::Cancelled,
                                                  std::memory_order_release);
                        } else {
                            locked->state.store(JobState::Failed,
                                                  std::memory_order_release);
                        }

                        std::lock_guard<std::mutex> lock(locked->resultMtx);
                        if (!result.success)
                            locked->errorMessage = result.errorMessage;
                    }

                    // Store the render result in RenderJobData.
                    if (auto locked = weakData.lock()) {
                        locked->renderResult = result;
                        locked->renderComplete.store(true,
                                                     std::memory_order_release);

                        // Clean up temp file on failure or cancellation.
                        if (!result.success ||
                            result.state == hathor::RenderState::Cancelled) {
                            std::error_code ec;
                            std::filesystem::remove(locked->tempPath, ec);
                        }
                    }
                };

            data->renderHandle = audio_.startBakeRenderRaw(
                data->tabId,
                data->ckSource,
                numSamples,
                sampleRate,
                data->tempPath,
                std::move(onComplete));

            // If startBakeRenderRaw returned an empty handle (worker failed),
            // the callback should have already fired with a failed result.
            // But as a safety net, check here.
            if (data->renderHandle.id() == 0 && !data->renderComplete.load()) {
                std::lock_guard<std::mutex> lock(entry->resultMtx);
                entry->errorMessage = "failed to start render (worker not ready)";
                entry->state.store(JobState::Failed, std::memory_order_release);
            }
        },
        nullptr);

    data->jobId = jobId;
    return jobId;
}

// ---------------------------------------------------------------------------
// AI-6 §5: get_job_status
// ---------------------------------------------------------------------------

nlohmann::json RenderService::getJobStatus(uint64_t jobId) const
{
    nlohmann::json status = jobTracker_->queryJob(jobId);

    if (!status.contains("ok") || !status["ok"].get<bool>())
        return status;

    std::shared_ptr<RenderJobData> data;
    {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        auto it = renderJobs_.find(jobId);
        if (it != renderJobs_.end())
            data = it->second;
    }

    if (data) {
        status["type"] = "render";
        status["session_id"]      = data->sessionId;
        status["asset_name"]      = data->sanitizedName;
        status["target"]          = hathor::toString(data->target);
        status["duration_bars"]   = data->durationBars;

        if (data->renderComplete.load(std::memory_order_acquire)) {
            const auto& r = data->renderResult;
            status["render_result"] = {
                {"success",          r.success},
                {"state",            hathor::toString(r.state)},
                {"samples_written",  r.samplesWritten},
                {"duration_seconds", r.durationSeconds},
                {"output_path",      r.outputPath.string()}
            };
            if (!r.errorMessage.empty())
                status["render_result"]["error"] = r.errorMessage;

            status["status"] = r.success
                ? hathor::toString(JobState::Succeeded)
                : (r.state == hathor::RenderState::Cancelled
                    ? hathor::toString(JobState::Cancelled)
                    : hathor::toString(JobState::Failed));
        } else {
            status["status"] = hathor::toString(JobState::Running);
        }

        status["final_wav_path"] = data->finalWavPath.string();
        status["final_ck_path"]  = data->finalCkPath.string();
        status["temp_path"]      = data->tempPath.string();
        status["committable"]    = data->renderResult.success
                                   && data->renderComplete.load(std::memory_order_acquire);
    }

    return status;
}

// ---------------------------------------------------------------------------
// AI-6 §7: commit_rendered_asset
// ---------------------------------------------------------------------------

nlohmann::json RenderService::commitRenderedAsset(uint64_t jobId,
                                                   std::string_view assetName,
                                                   bool confirmOverwrite)
{
    // -----------------------------------------------------------------------
    // Step 1: Look up the render job
    // -----------------------------------------------------------------------
    std::shared_ptr<RenderJobData> data;
    {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        auto it = renderJobs_.find(jobId);
        if (it == renderJobs_.end()) {
            return {
                {"ok",     false},
                {"cmd",    "commit_rendered_asset"},
                {"job_id", jobId},
                {"error",  "unknown job id"}
            };
        }
        data = it->second;
    }

    // -----------------------------------------------------------------------
    // Step 2: Validate the render succeeded
    // -----------------------------------------------------------------------
    if (!data->renderComplete.load(std::memory_order_acquire)) {
        return {
            {"ok",     false},
            {"cmd",    "commit_rendered_asset"},
            {"job_id", jobId},
            {"error",  "render not yet complete"}
        };
    }

    if (!data->renderResult.success) {
        return {
            {"ok",     false},
            {"cmd",    "commit_rendered_asset"},
            {"job_id", jobId},
            {"error",  data->renderResult.errorMessage.empty()
                        ? "render failed"
                        : data->renderResult.errorMessage},
            {"render_state", hathor::toString(data->renderResult.state)}
        };
    }

    // -----------------------------------------------------------------------
    // Step 3: Validate asset name (reject unsafe, do not sanitize)
    // -----------------------------------------------------------------------
    if (!isAssetNameSafe(assetName)) {
        return {
            {"ok",    false},
            {"cmd",   "commit_rendered_asset"},
            {"job_id", jobId},
            {"error", "asset name contains path separators or is unsafe"}
        };
    }

    // -----------------------------------------------------------------------
    // Step 4: Resolve final paths
    // -----------------------------------------------------------------------
    const std::string safeName = hathor::sanitizeAssetName(assetName);
    hathor::AssetPathResolver resolver(data->projectDir);

    auto wavResolve = resolver.resolveStudio(safeName);
    if (!wavResolve.ok) {
        return {
            {"ok",    false},
            {"cmd",   "commit_rendered_asset"},
            {"job_id", jobId},
            {"error", "path resolution failed: " + wavResolve.error}
        };
    }

    const auto finalWavPath = wavResolve.path;
    auto finalCkPath = finalWavPath;
    finalCkPath.replace_extension(".ck");

    // -----------------------------------------------------------------------
    // Step 5: Collision detection
    // -----------------------------------------------------------------------
    std::error_code ec;
    const bool ckExists  = std::filesystem::exists(finalCkPath,  ec);
    const bool wavExists = std::filesystem::exists(finalWavPath, ec);

    if ((ckExists || wavExists) && !confirmOverwrite) {
        nlohmann::json conflicts = nlohmann::json::array();
        if (ckExists)  conflicts.push_back(finalCkPath.string());
        if (wavExists) conflicts.push_back(finalWavPath.string());

        return {
            {"ok",              false},
            {"cmd",             "commit_rendered_asset"},
            {"job_id",          jobId},
            {"status",          "conflict"},
            {"conflict",        true},
            {"existing_files",  conflicts},
            {"error",           "asset already exists; pass confirm_overwrite=true to replace"}
        };
    }

    // -----------------------------------------------------------------------
    // Step 6: Backup existing files (if overwriting)
    // -----------------------------------------------------------------------
    std::filesystem::path ckBackup;
    std::filesystem::path wavBackup;

    if (ckExists) {
        ckBackup = finalCkPath;
        ckBackup.replace_extension(".bak");
        std::filesystem::copy_file(finalCkPath, ckBackup,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return {
                {"ok",    false},
                {"cmd",   "commit_rendered_asset"},
                {"job_id", jobId},
                {"error", "failed to back up existing .ck: " + ec.message()}
            };
        }
    }

    if (wavExists) {
        wavBackup = finalWavPath;
        wavBackup.replace_extension(".bak");
        std::filesystem::copy_file(finalWavPath, wavBackup,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            if (!ckBackup.empty()) std::filesystem::remove(ckBackup, ec);
            return {
                {"ok",    false},
                {"cmd",   "commit_rendered_asset"},
                {"job_id", jobId},
                {"error", "failed to back up existing .wav: " + ec.message()}
            };
        }
    }

    // -----------------------------------------------------------------------
    // Step 7: Prepare destination directory
    // -----------------------------------------------------------------------
    auto instrDir = resolver.studioInstrumentsDir();
    std::filesystem::create_directories(instrDir, ec);
    if (ec) {
        if (!ckBackup.empty())  std::filesystem::remove(ckBackup, ec);
        if (!wavBackup.empty()) std::filesystem::remove(wavBackup, ec);
        return {
            {"ok",    false},
            {"cmd",   "commit_rendered_asset"},
            {"job_id", jobId},
            {"error", "cannot create instruments directory: " + ec.message()}
        };
    }

    // -----------------------------------------------------------------------
    // Step 8: Verify temp file exists
    // -----------------------------------------------------------------------
    if (!std::filesystem::exists(data->tempPath, ec)) {
        if (!ckBackup.empty())  std::filesystem::remove(ckBackup, ec);
        if (!wavBackup.empty()) std::filesystem::remove(wavBackup, ec);
        cleanupTempFile(jobId);
        return {
            {"ok",    false},
            {"cmd",   "commit_rendered_asset"},
            {"job_id", jobId},
            {"error", "temp render file not found"}
        };
    }

    // -----------------------------------------------------------------------
    // Step 9: Write .ck source (temp file + atomic rename)
    // -----------------------------------------------------------------------
    auto ckTemp = finalCkPath;
    ckTemp.replace_filename(finalCkPath.stem().string() + ".tmp.ck");

    {
        std::ofstream f(ckTemp, std::ios::binary | std::ios::trunc);
        if (!f.is_open() ||
            !f.write(data->ckSource.data(),
                     static_cast<std::streamsize>(data->ckSource.size()))) {
            f.close();
            std::filesystem::remove(ckTemp, ec);
            if (!ckBackup.empty())  std::filesystem::remove(ckBackup, ec);
            if (!wavBackup.empty()) std::filesystem::remove(wavBackup, ec);
            cleanupTempFile(jobId);
            return {
                {"ok",    false},
                {"cmd",   "commit_rendered_asset"},
                {"job_id", jobId},
                {"error", "failed to write .ck source file"}
            };
        }
        f.close();
    }

    std::error_code renameEc;
    std::filesystem::rename(ckTemp, finalCkPath, renameEc);
    if (renameEc) {
        std::filesystem::remove(ckTemp, ec);
        if (!ckBackup.empty())  std::filesystem::remove(ckBackup, ec);
        if (!wavBackup.empty()) std::filesystem::remove(wavBackup, ec);
        cleanupTempFile(jobId);
        return {
            {"ok",    false},
            {"cmd",   "commit_rendered_asset"},
            {"job_id", jobId},
            {"error", "failed to write .ck file: " + renameEc.message()}
        };
    }

    // -----------------------------------------------------------------------
    // Step 10: Move temp .wav → final .wav (atomic rename + copy fallback)
    // -----------------------------------------------------------------------
    std::filesystem::rename(data->tempPath, finalWavPath, renameEc);
    if (renameEc) {
        ec.clear();
        if (!std::filesystem::copy_file(data->tempPath, finalWavPath,
                                        std::filesystem::copy_options::overwrite_existing, ec)) {
            // Rollback: remove .ck, restore backups.
            std::error_code rbc;
            std::filesystem::remove(finalCkPath, rbc);
            if (!ckBackup.empty())  std::filesystem::rename(ckBackup, finalCkPath, rbc);
            if (!wavBackup.empty()) std::filesystem::rename(wavBackup, finalWavPath, rbc);
            return {
                {"ok",    false},
                {"cmd",   "commit_rendered_asset"},
                {"job_id", jobId},
                {"error", "failed to write .wav file: " + ec.message()}
            };
        }
        std::filesystem::remove(data->tempPath, ec);
    }

    // -----------------------------------------------------------------------
    // Step 11: Register in SampleBank (decode + addEntry)
    // -----------------------------------------------------------------------
    bool registered = audio_.registerBakedAsset(safeName, finalWavPath);

    if (!registered) {
        // Rollback: remove .ck and .wav, restore backups.
        std::error_code rb;
        std::filesystem::remove(finalCkPath, rb);
        std::filesystem::remove(finalWavPath, rb);
        if (!ckBackup.empty())  std::filesystem::rename(ckBackup, finalCkPath, rb);
        if (!wavBackup.empty()) std::filesystem::rename(wavBackup, finalWavPath, rb);
        auditCommit(jobId, safeName, "register_failed", false);
        return {
            {"ok",    false},
            {"cmd",   "commit_rendered_asset"},
            {"job_id", jobId},
            {"error", "SampleBank registration failed"}
        };
    }

    // -----------------------------------------------------------------------
    // Step 12: Verify registration
    // -----------------------------------------------------------------------
    auto* entry = bank_.find(safeName, 0);
    if (!entry) {
        // Registration returned true but entry not findable — inconsistency.
        bank_.removeEntry(safeName, 0);
        std::error_code rb;
        std::filesystem::remove(finalCkPath, rb);
        std::filesystem::remove(finalWavPath, rb);
        if (!ckBackup.empty())  std::filesystem::rename(ckBackup, finalCkPath, rb);
        if (!wavBackup.empty()) std::filesystem::rename(wavBackup, finalWavPath, rb);
        auditCommit(jobId, safeName, "verify_failed", false);
        return {
            {"ok",    false},
            {"cmd",   "commit_rendered_asset"},
            {"job_id", jobId},
            {"error", "SampleBank registration verification failed"}
        };
    }

    // -----------------------------------------------------------------------
    // Step 13: Clean up backups
    // -----------------------------------------------------------------------
    if (!ckBackup.empty())  std::filesystem::remove(ckBackup, ec);
    if (!wavBackup.empty()) std::filesystem::remove(wavBackup, ec);

    // -----------------------------------------------------------------------
    // Step 14: Success — audit log
    // -----------------------------------------------------------------------
    const bool wasNew = !(ckExists || wavExists);
    auditCommit(jobId, safeName,
                wasNew ? "commit_new" : "commit_overwrite", true);

    // -----------------------------------------------------------------------
    // Step 15: Return success
    // -----------------------------------------------------------------------
    nlohmann::json result = {
        {"ok",         true},
        {"cmd",        "commit_rendered_asset"},
        {"job_id",     jobId},
        {"asset_name", safeName},
        {"path",       finalWavPath.string()},
        {"ck_path",    finalCkPath.string()},
        {"new_asset",  wasNew},
        {"overwritten", !wasNew},
        {"samples",    static_cast<int>(entry->data.size() / entry->numChannels)},
        {"sample_rate", static_cast<int>(entry->sampleRate)},
        {"channels",   entry->numChannels}
    };

    return result;
}

// ---------------------------------------------------------------------------
// AI-6 §21: cancellation
// ---------------------------------------------------------------------------

bool RenderService::cancelJob(uint64_t jobId)
{
    // First, cancel via the JobTracker (sets cancelRequested flag, handles
    // queued/running state transitions).
    const bool cancelled = jobTracker_->cancelJob(jobId);

    // Also cancel the underlying render handle if the render has started.
    {
        std::shared_ptr<RenderJobData> data;
        {
            std::lock_guard<std::mutex> lock(jobsMtx_);
            auto it = renderJobs_.find(jobId);
            if (it != renderJobs_.end())
                data = it->second;
        }

        if (data) {
            data->renderHandle.cancel();
            cleanupTempFile(jobId);
        }
    }

    return cancelled;
}

// ---------------------------------------------------------------------------
// AI-6 §9: list render jobs
// ---------------------------------------------------------------------------

nlohmann::json RenderService::listRenderJobs() const
{
    nlohmann::json arr = nlohmann::json::array();

    std::lock_guard<std::mutex> lock(jobsMtx_);
    for (const auto& [jobId, data] : renderJobs_) {
        nlohmann::json entry = {
            {"job_id",         jobId},
            {"session_id",     data->sessionId},
            {"asset_name",     data->sanitizedName},
            {"target",         hathor::toString(data->target)},
            {"duration_bars",  data->durationBars},
            {"complete",       data->renderComplete.load(std::memory_order_acquire)}
        };

        if (data->renderComplete.load(std::memory_order_acquire)) {
            const auto& r = data->renderResult;
            entry["render_success"] = r.success;
            entry["render_state"]   = hathor::toString(r.state);
            if (!r.errorMessage.empty())
                entry["error"] = r.errorMessage;
        } else {
            entry["render_state"] = "in_progress";
        }

        arr.push_back(std::move(entry));
    }

    return arr;
}

// ---------------------------------------------------------------------------
// Helper: cleanup temp file
// ---------------------------------------------------------------------------

void RenderService::cleanupTempFile(uint64_t jobId) noexcept
{
    std::shared_ptr<RenderJobData> data;
    {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        auto it = renderJobs_.find(jobId);
        if (it != renderJobs_.end())
            data = it->second;
    }

    if (data && !data->tempPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(data->tempPath, ec);
    }
}

// ---------------------------------------------------------------------------
// Helper: audit log
// ---------------------------------------------------------------------------

void RenderService::auditCommit(uint64_t jobId,
                                  const std::string& assetName,
                                  const std::string& action,
                                  bool success) const noexcept
{
    std::ostringstream log;
    log << "[AI-6 AUDIT] job=" << jobId
        << " asset=" << assetName
        << " action=" << action
        << " success=" << (success ? "true" : "false")
        << " timestamp=" << std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();

    std::cerr << log.str() << '\n';
}

} // namespace hathor::control
