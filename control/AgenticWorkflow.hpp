// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * AgenticWorkflow.hpp — AI-10: Canonical agentic musical workflow orchestration.
 *
 * Provides the orchestration layer that executes the canonical musical workflow:
 *
 *   inspect_project → inspect_song → inspect_assets → generate/modify pattern
 *   → validate → compile → audition → inspect diagnostics → repair
 *   → validate again → render → bind asset → update song
 *
 * The agentic workflow operates ENTIRELY through the canonical application
 * contract established by AI-1 and the services implemented by AI-2…AI-9.
 * It creates NO second model of Hathor, bypasses NO authorization boundary,
 * and writes to NO persistent state directly — every operation routes through
 * the canonical service layer (ProjectReadFacade, ChuckSessionService,
 * RenderService, SongMutationService).
 *
 * Architecture boundary (AI-10):
 *
 *   AI Agent (MCP)
 *        ↓
 *   ControlInterface::dispatch("workflow_start …")
 *        ↓
 *   AgenticWorkflow  ← this layer (state machine + step orchestration)
 *        ↓
 *   ProjectReadFacade   (AI-2: read-only inspection)
 *         ↓
 *   ChuckSessionService  (AI-5: ChucK lifecycle / compile / audition)
 *         ↓
 *   RenderService        (AI-6: render → commit boundary)
 *         ↓
 *   SongMutationService  (AI-7: transactional song mutation)
 *
 * Safety / authorization (AI-1 capability model carried through):
 *   - Read-only operations (inspect_project, get_current_song, list_assets,
 *     get_diagnostics, get_audio_status) execute automatically.
 *   - Non-destructive execution (compile_chuck, audition_chuck, play, stop)
 *     executes automatically.
 *   - Persistent mutation (commit_rendered_asset, edit_song) CROSSES the
 *     confirmation boundary — the workflow pauses at WAITING_FOR_APPROVAL
 *     and emits a ConfirmationRequest via callback.  No persistent mutation
 *     occurs without explicit approval.
 *
 * Requirement references: AI-1 §1, AI-2, AI-5, AI-6, AI-7, AI-10 §1–§6,
 *                         PROGRAM.md Phase K
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace hathor::control {

// Forward declarations — full headers are only needed in the .cpp.
class ProjectReadFacade;
class ChuckSessionService;
class RenderService;
class SongMutationService;

} // namespace hathor::control

// Forward declarations — JUCE-free abstract interface.
class AudioEngineFacade;
class SampleBank;

namespace hathor::control {

/**
 * AgenticWorkflow — canonical agentic musical workflow orchestration.
 *
 * Constructed by ControlInterface with references to the canonical services.
 * Runs on its own background thread; communicates via callbacks.
 *
 * Thread model:
 *   - start()        : called from the control/worker thread (non-blocking)
 *   - runWorkflow()  : runs on workflowThread_ (the orchestration thread)
 *   - cancel()       : called from any thread (atomic flag + CV notify)
 *   - respondToConfirmation() : called from any thread (atomic flag + CV notify)
 *   - getState()     : called from any thread (mutex-guarded read)
 *   - callbacks      : invoked on workflowThread_
 */
class AgenticWorkflow {
public:
    // -----------------------------------------------------------------------
    // AI-10.6: Workflow lifecycle states
    // -----------------------------------------------------------------------

    enum class State {
        Idle,            ///< No workflow active.
        Queued,          ///< Workflow request received, about to start.
        Planning,        ///< Analysing request and assembling a plan.
        Inspecting,      ///< Running inspect_project / inspect_song / inspect_assets.
        Editing,         ///< Applying generated content (set_pattern / create session).
        Validating,      ///< Validating notation / diagnostics.
        Compiling,       ///< Compiling ChucK source (async job).
        Auditioning,     ///< Playing pattern / auditioning ChucK instrument.
        InspectingDiagnostics, ///< Checking diagnostics from compile/audition.
        Repairing,       ///< Attempting repair (re-generate / re-compile).
        Rendering,       ///< Rendering ChucK instrument to WAV (async job).
        Binding,         ///< Committing rendered asset (requires confirmation).
        UpdatingSong,    ///< Updating song file (requires confirmation).
        WaitingForApproval, ///< Paused pending user confirmation of a destructive op.
        WaitingForUser,  ///< Paused pending user input (e.g. creative feedback).
        Completed,       ///< Workflow finished successfully.
        Failed,          ///< Workflow ended in a non-recoverable error.
        Cancelled,       ///< Workflow was cancelled by the user.
    };

    // -----------------------------------------------------------------------
    // Canonical workflow steps (PROGRAM.md Phase K)
    // -----------------------------------------------------------------------

    enum class Step {
        InspectProject,
        InspectSong,
        InspectAssets,
        GeneratePattern,
        Validate,
        Compile,
        Audition,
        InspectDiagnostics,
        Repair,
        Render,
        BindAsset,
        UpdateSong,
        None,  ///< No active step (initial / cleanup)
    };

    // -----------------------------------------------------------------------
    // Request — the high-level intent the workflow executes
    // -----------------------------------------------------------------------

    struct Request {
        std::string intent;           ///< Natural-language description (e.g. "dark 8-bar bassline")
        std::string targetSlot;       ///< Target pattern slot (e.g. "d1")
        std::string notation;         ///< Generated mini-notation (empty if ChucK workflow)
        std::string ckSource;         ///< ChucK source code (empty if pattern workflow)
        std::string assetName;        ///< Asset name for rendering (e.g. "acid_bass")
        int durationBars = 8;         ///< Render duration in bars
        nlohmann::json plan;          ///< Optional pre-determined plan steps
        bool dryRun = false;          ///< If true, skip all persistent mutations
    };

    // -----------------------------------------------------------------------
    // Step result — observable outcome of each canonical step
    // -----------------------------------------------------------------------

    struct StepResult {
        bool        ok          = false;
        std::string message;
        std::string stepName;
        nlohmann::json data;    ///< Step-specific result data
    };

    // -----------------------------------------------------------------------
    // Confirmation request — emitted for persistent-mutation operations
    // -----------------------------------------------------------------------

    struct ConfirmationRequest {
        int         requestId;       ///< Unique ID for matching respondToConfirmation()
        std::string action;          ///< Short action name (e.g. "commit_rendered_asset")
        std::string description;     ///< Human-readable description
        nlohmann::json details;      ///< Operation details (asset name, paths, etc.)
        std::string capabilityClass; ///< AI-1 capability class (e.g. "persistent_mutation")
    };

    // -----------------------------------------------------------------------
    // Callback types
    // -----------------------------------------------------------------------

    /// Called on every state/step transition so the UI can report progress.
    using ProgressCallback       = std::function<void(nlohmann::json state)>;

    /// Called when a destructive operation requires user confirmation.
    using ConfirmationCallback   = std::function<void(ConfirmationRequest req)>;

    // -----------------------------------------------------------------------
    // Construction / destruction
    // -----------------------------------------------------------------------

    /**
     * Construct the workflow with references to the canonical services.
     *
     * The workflow does NOT own these services — they are owned by
     * ControlInterface.  All services must outlive the workflow.
     *
     * @param audio          AudioEngineFacade — transport, slots, ChucK VM access.
     * @param bank           SampleBank — for sample lookup after binding.
     * @param readFacade     ProjectReadFacade — canonical read-only inspection (AI-2).
     * @param chuckService   ChuckSessionService — ChucK lifecycle (AI-5).
     * @param renderService  RenderService — render → commit boundary (AI-6).
     * @param songService    SongMutationService — structured song mutation (AI-7).
     */
    AgenticWorkflow(AudioEngineFacade&       audio,
                    SampleBank&              bank,
                    ProjectReadFacade&     readFacade,
                    ChuckSessionService&     chuckService,
                    RenderService&           renderService,
                    SongMutationService&     songService);

    ~AgenticWorkflow();

    AgenticWorkflow(const AgenticWorkflow&)            = delete;
    AgenticWorkflow& operator=(const AgenticWorkflow&) = delete;

    // -----------------------------------------------------------------------
    // Public interface
    // -----------------------------------------------------------------------

    /**
     * Start a new workflow.
     *
     * The workflow runs on a background thread.  Progress events are delivered
     * via @p onProgress; confirmation requests via @p onConfirmation.
     *
     * If a workflow is already running, this call fails and returns false.
     *
     * @return true if the workflow was queued successfully.
     */
    bool start(Request request,
               ProgressCallback     onProgress,
               ConfirmationCallback onConfirmation);

    /**
     * Cancel the currently running workflow.
     *
     * Sets the stop flag; the workflow thread checks it between steps and
     * exits cleanly.  Active async jobs (compile, render) are cancelled
     * through their canonical cancellation paths.
     *
     * @return true if a running workflow was signalled to stop.
     */
    bool cancel();

    /**
     * Respond to a pending confirmation request.
     *
     * @param approved  true to approve the destructive operation,
     *                  false to reject it (workflow fails the step).
     * @return true if the confirmation was found and the response delivered.
     */
    bool respondToConfirmation(bool approved);

    /**
     * Get the current workflow state as a JSON snapshot (thread-safe).
     *
     * Returns the full observable state: current state, current step,
     * completed steps, diagnostics, render status, pending confirmation,
     * applied changes, and any error.
     */
    nlohmann::json getState() const;

    /**
     * Check if a workflow is currently active (not Idle).
     */
    bool isRunning() const noexcept;

    /**
     * Reset to Idle state.  Called after the caller has observed a terminal
     * state (Completed, Failed, Cancelled).  Allows a new workflow to start.
     */
    void reset();

private:
    // -----------------------------------------------------------------------
    // Workflow execution (runs on workflowThread_)
    // -----------------------------------------------------------------------

    void runWorkflow();

    // --- Canonical step implementations ---

    StepResult stepInspectProject();
    StepResult stepInspectSong();
    StepResult stepInspectAssets();
    StepResult stepGeneratePattern();
    StepResult stepValidate();
    StepResult stepCompile();
    StepResult stepAudition();
    StepResult stepInspectDiagnostics();
    StepResult stepRepair();
    StepResult stepRender();
    StepResult stepBindAsset();
    StepResult stepUpdateSong();

    // --- Orchestration helpers ---

    /// Set the internal state and emit a progress event.
    void setState(State s);

    /// Set the current active step and emit a progress event.
    void setCurrentStep(Step s);

    /// Record a completed step and emit a progress event.
    void completeStep(const std::string& name, const StepResult& result);

    /// Check if cancellation was requested.  Returns true if the workflow
    /// should stop.  Called at each step boundary.
    bool checkCancellation();

    /// Emit the current state as a progress event.
    void emitProgress();

    /// Pause execution and wait for user confirmation of a destructive op.
    /// Returns true if approved, false if rejected.  Emits a ConfirmationRequest
    /// via the confirmation callback before blocking.
    bool waitForConfirmation(const std::string& action,
                             const std::string& capabilityClass,
                             const std::string& description,
                             nlohmann::json     details);

    /// Poll an async job (compile/render) until it completes or is cancelled.
    /// @param jobTrackerId  The job ID from compileChuck() / renderChuck().
    /// @param serviceType   "chuck" or "render" — determines which service to poll.
    /// @param result        Output: the JSON job status.
    /// @return true if the job succeeded, false if failed/cancelled/timed out.
    bool waitForAsyncJob(uint64_t jobTrackerId,
                         const std::string& serviceType,
                         nlohmann::json& result);

    // -----------------------------------------------------------------------
    // Internal state (protected by stateMtx_)
    // -----------------------------------------------------------------------

    AudioEngineFacade&             audio_;
    SampleBank&                    bank_;
    ProjectReadFacade&             readFacade_;
    ChuckSessionService&           chuckService_;
    RenderService&                 renderService_;
    SongMutationService&           songService_;

    // Thread management
    std::thread      workflowThread_;
    mutable std::mutex    stateMtx_;
    std::condition_variable cv_;
    std::atomic<bool>  stopRequested_{false};

    // Workflow state machine
    State              state_           = State::Idle;
    Step               currentStep_     = Step::None;
    std::vector<std::string> completedSteps_;
    nlohmann::json    currentStepResult_;
    nlohmann::json    diagnostics_;
    nlohmann::json    renderStatus_;
    std::optional<ConfirmationRequest> pendingConfirmation_;
    std::vector<nlohmann::json> appliedChanges_;
    std::optional<std::string> error_;
    Request           currentRequest_;

    // Inspection results (cached for use across steps)
    nlohmann::json  projectInfo_;
    nlohmann::json  songInfo_;
    nlohmann::json  assetsInfo_;

    // Generated content (from the generate step)
    std::string       generatedNotation_;   ///< canonical mini-notation
    std::string       generatedCkSource_;   ///< canonical ChucK source
    std::string       sessionId_;           ///< ChucK session ID (for ChucK workflows)

    // Render job tracking
    uint64_t        renderJobId_ = 0;
    bool            renderCompleted_ = false;
    nlohmann::json  renderResult_;

    // Repair loop tracking
    int             repairAttempts_ = 0;
    static constexpr int kMaxRepairAttempts = 3;

    // Confirmation tracking
    std::atomic<int>         nextConfirmationId_{1};
    std::atomic<bool>        confirmationApproved_{false};
    std::atomic<bool>        confirmationResponded_{false};
    std::mutex               confirmMtx_;
    std::condition_variable  confirmCv_;

    // Callbacks (set per-workflow run, invoked on workflowThread_)
    ProgressCallback       progressCallback_;
    ConfirmationCallback   confirmationCallback_;
};

} // namespace hathor::control
