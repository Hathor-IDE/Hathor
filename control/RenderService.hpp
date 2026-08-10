// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * RenderService.hpp — AI-6 canonical rendering service.
 *
 * Provides the canonical application-layer service for background ChucK
 * instrument rendering with an explicit render→commit boundary.
 *
 * Architecture boundary (AI-6):
 *
 *   MCP / AI / UI
 *         ↓
 *   RenderService  ← this layer (canonical render + commit contract)
 *         ↓
 *   ChuckRenderWriter (B8-K2) + JobTracker (AI-1) + SampleBank (B8-K4)
 *
 * Design:
 *   - Rendering is NON-DESTRUCTIVE.  A render job writes to a temporary path
 *     in the system temp directory and does NOT touch the persistent project
 *     asset tree until commit is explicitly called.
 *   - Registration in the SampleBank happens ONLY at commit time, not at render
 *     completion (unlike B8's BakeOrchestrator which auto-registers on success).
 *   - Collision detection at commit time: existing assets require explicit
 *     confirmation to overwrite (no silent replacement).
 *   - Rollback on commit failure: partial file writes and SampleBank entries
 *     are undone.
 *
 * Requirement references: AI-1 §1, AI-5 §16 (shared job infrastructure),
 *                         B8-K1, B8-K2, B8-K4, PROGRAM.md B8
 */

#include "ChuckSession.hpp"

#include "../app/AudioEngineFacade.hpp"
#include "../app/ChuckRenderWriter.hpp"
#include "../app/AssetTarget.hpp"
#include "../app/SampleBank.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace hathor::control {

class JobTracker;
class ChuckSessionService;

/**
 * RenderService — canonical rendering service with explicit render→commit
 * boundary.
 *
 * Thread model:
 *   - All public methods may be called from the control/worker thread.
 *   - renderChuck() submits to the shared JobTracker and returns immediately
 *     with a job_id.  The actual render happens on a background thread
 *     (ChuckRenderWriter's render thread).
 *   - The completion callback fires on the render thread and updates the
 *     JobEntry's state + stores the RenderResult.
 *   - getJobStatus() queries the JobTracker for state and augments with
 *     render-specific metadata.
 *   - commitRenderedAsset() performs the persistent mutation (file write +
 *     SampleBank registration) synchronously on the calling thread.
 */
class RenderService {
public:
    /**
     * Construct the render service.
     *
     * @param audio     AudioEngineFacade — provides startBakeRenderRaw,
     *                  getBpm(), getAudioStatus(), currentProjectDir().
     * @param bank      SampleBank — for asset registration (B8-K4).
     * @param sessions  ChuckSessionService — for session source retrieval (AI-5).
     */
    RenderService(AudioEngineFacade& audio,
                  SampleBank&        bank,
                  ChuckSessionService& sessions) noexcept;

    ~RenderService() = default;
    RenderService(const RenderService&)            = delete;
    RenderService& operator=(const RenderService&) = delete;

    // -----------------------------------------------------------------------
    // AI-6 §2: render_chuck
    // -----------------------------------------------------------------------

    /**
     * Start a background render of the ChucK instrument associated with the
     * given session.
     *
     * This call is non-blocking — it submits a job to the JobTracker and
     * returns immediately with a job_id.  The actual render happens on a
     * background thread via ChuckRenderWriter (B8-K2).
     *
     * The render writes to a TEMPORARY path (system temp directory).  The
     * persistent project tree is NOT touched until commitRenderedAsset() is
     * called explicitly.
     *
     * @param sessionId     The ChucK session ID (e.g. "ck:3").  The session
     *                       must exist and have source code associated.
     * @param durationBars  Duration in bars (4/4 time).  Converted to samples
     *                       using current BPM and sample rate.
     * @param assetName     The asset name for the final output.  Must be a
     *                       safe filename component — path separators and ".."
     *                       are REJECTED (not silently rewritten).
     * @param target        Asset target (Studio or LiveJam).  Defaults to Studio.
     * @return The job ID for polling via getJobStatus().
     */
    uint64_t renderChuck(std::string_view sessionId,
                         int durationBars,
                         std::string_view assetName,
                         hathor::AssetTarget target = hathor::AssetTarget::Studio);

    // -----------------------------------------------------------------------
    // AI-6 §5: get_job_status
    // -----------------------------------------------------------------------

    /**
     * Query the status of a render job.
     *
     * @param jobId  The job ID returned by renderChuck().
     * @return JSON with job_id, status, and (if complete) render result details.
     */
    nlohmann::json getJobStatus(uint64_t jobId) const;

    // -----------------------------------------------------------------------
    // AI-6 §7: commit_rendered_asset
    // -----------------------------------------------------------------------

    /**
     * Commit a completed render to the persistent asset tree.
     *
     * This is the explicit commit boundary — the only operation that mutates
     * the persistent project.  Performs collision detection, writes the .ck
     * source and .wav file, and registers the asset in the SampleBank.
     *
     * @param jobId           The render job ID (must have succeeded).
     * @param assetName       The final asset name.  Must match the name used
     *                         in renderChuck() (for Studio target).  Must be a
     *                         safe filename component.
     * @param confirmOverwrite If true, overwrite an existing asset at the
     *                         final path.  If false and an asset exists, a
     *                         conflict is returned and nothing is written.
     * @return JSON result with ok=true on success, ok=false with error on
     *         failure (including collision conflicts).
     */
    nlohmann::json commitRenderedAsset(uint64_t jobId,
                                        std::string_view assetName,
                                        bool confirmOverwrite = false);

    // -----------------------------------------------------------------------
    // AI-6 §21: cancellation
    // -----------------------------------------------------------------------

    /**
     * Cancel an in-flight render job.
     *
     * @param jobId  The job ID returned by renderChuck().
     * @return true if the job was cancelled, false if it was already complete
     *         or not found.
     */
    bool cancelJob(uint64_t jobId);

    // -----------------------------------------------------------------------
    // AI-6 §9: List render jobs
    // -----------------------------------------------------------------------

    /**
     * List all render jobs known to this service.
     * @return JSON array of job status objects.
     */
    nlohmann::json listRenderJobs() const;

private:
    // -----------------------------------------------------------------------
    // Internal: render job state (maintained alongside JobTracker)
    // -----------------------------------------------------------------------

    struct RenderJobData {
        uint64_t                         jobId       = 0;
        std::string                      sessionId;
        uint8_t                          tabId       = 0;
        std::string                      sanitizedName;      // validated asset name
        std::string                      ckSource;           // captured from session
        hathor::AssetTarget              target        = hathor::AssetTarget::Studio;
        int                              durationBars  = 0;
        uint64_t                         numSamples    = 0;
        unsigned                         sampleRate    = 44100;
        double                           bpm           = 120.0;
        std::filesystem::path            tempPath;         // temp .wav (render output)
        std::filesystem::path            finalWavPath;     // final Studio .wav
        std::filesystem::path            finalCkPath;      // final Studio .ck
        std::filesystem::path            projectDir;
        hathor::RenderResult             renderResult;    // filled by callback
        std::atomic<bool>                renderComplete{false};
        hathor::RenderHandle             renderHandle;    // for cancellation
    };

    /**
     * Clean up the temp file associated with a completed/failed/cancelled
     * render job.
     */
    void cleanupTempFile(uint64_t jobId) noexcept;

    /**
     * Emit an audit log entry for a commit operation.
     */
    void auditCommit(uint64_t jobId,
                     const std::string& assetName,
                     const std::string& action,
                     bool success) const noexcept;

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    AudioEngineFacade&           audio_;
    SampleBank&                  bank_;
    ChuckSessionService&         sessions_;
    std::shared_ptr<JobTracker>  jobTracker_;

    mutable std::mutex           jobsMtx_;
    std::unordered_map<uint64_t, std::shared_ptr<RenderJobData>> renderJobs_;
};

} // namespace hathor::control
