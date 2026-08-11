// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * AgenticWorkflow.cpp — AI-10: Agentic musical workflow orchestration.
 *
 * Requirement references: AI-1 §1, AI-2, AI-5, AI-6, AI-7, AI-10 §1–§6,
 *                         PROGRAM.md Phase K
 */

#include "AgenticWorkflow.hpp"

#include "ProjectReadFacade.hpp"
#include "ChuckSessionService.hpp"
#include "RenderService.hpp"
#include "SongMutationService.hpp"
#include "ChuckSession.hpp"
#include "JobTracker.hpp"
#include "Commands.hpp"

#include "../app/AudioEngineFacade.hpp"
#include "../app/SampleBank.hpp"
#include "../app/AssetTarget.hpp"
#include "../app/AssetPathResolver.hpp"
#include "../app/ChuckRenderWriter.hpp"

#include "hathor/MiniParser.hpp"
#include "hathor/PrettyPrinter.hpp"
#include "hathor/PatternCompiler.hpp"
#include "hathor/MiniTokeniser.hpp"
#include "hathor/Event.hpp"
#include "hathor/ParamMap.hpp"
#include "hathor/Arc.hpp"
#include "ChuckDiagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <thread>

namespace hathor::control {

// ---------------------------------------------------------------------------
// Step name helpers
// ---------------------------------------------------------------------------

static const char* stepName(AgenticWorkflow::Step s) noexcept
{
    switch (s) {
        case AgenticWorkflow::Step::InspectProject:        return "inspect_project";
        case AgenticWorkflow::Step::InspectSong:          return "inspect_song";
        case AgenticWorkflow::Step::InspectAssets:        return "inspect_assets";
        case AgenticWorkflow::Step::GeneratePattern:      return "generate_pattern";
        case AgenticWorkflow::Step::Validate:             return "validate";
        case AgenticWorkflow::Step::Compile:               return "compile";
        case AgenticWorkflow::Step::Audition:             return "audition";
        case AgenticWorkflow::Step::InspectDiagnostics:   return "inspect_diagnostics";
        case AgenticWorkflow::Step::Repair:               return "repair";
        case AgenticWorkflow::Step::CreativeRepair:       return "creative_repair";
        case AgenticWorkflow::Step::Render:               return "render";
        case AgenticWorkflow::Step::BindAsset:            return "bind_asset";
        case AgenticWorkflow::Step::UpdateSong:           return "update_song";
        case AgenticWorkflow::Step::None:                 return "none";
    }
    return "unknown";
}

static const char* stateName(AgenticWorkflow::State s) noexcept
{
    switch (s) {
        case AgenticWorkflow::State::Idle:                   return "idle";
        case AgenticWorkflow::State::Queued:                 return "queued";
        case AgenticWorkflow::State::Planning:               return "planning";
        case AgenticWorkflow::State::Inspecting:             return "inspecting";
        case AgenticWorkflow::State::Editing:                return "editing";
        case AgenticWorkflow::State::Validating:             return "validating";
        case AgenticWorkflow::State::Compiling:              return "compiling";
        case AgenticWorkflow::State::Auditioning:            return "auditioning";
        case AgenticWorkflow::State::InspectingDiagnostics:  return "inspecting_diagnostics";
         case AgenticWorkflow::State::Repairing:              return "repairing";
         case AgenticWorkflow::State::CreativeRepairing:      return "creative_repairing";
        case AgenticWorkflow::State::Rendering:              return "rendering";
        case AgenticWorkflow::State::Binding:                return "binding";
        case AgenticWorkflow::State::UpdatingSong:           return "updating_song";
        case AgenticWorkflow::State::WaitingForApproval:   return "waiting_for_approval";
        case AgenticWorkflow::State::WaitingForUser:         return "waiting_for_user";
        case AgenticWorkflow::State::Completed:              return "completed";
        case AgenticWorkflow::State::Failed:                 return "failed";
        case AgenticWorkflow::State::Cancelled:              return "cancelled";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// AI-10.4: Event type names
// ---------------------------------------------------------------------------

const char* AgenticWorkflow::eventTypeName(EventType t) noexcept
{
    switch (t) {
        case EventType::WorkflowStarted:       return "workflow_started";
        case EventType::PlanCreated:           return "plan_created";
        case EventType::StepStarted:           return "step_started";
        case EventType::StepProgress:          return "step_progress";
        case EventType::StepCompleted:         return "step_completed";
        case EventType::StepFailed:            return "step_failed";
        case EventType::DiagnosticsDiscovered: return "diagnostics_discovered";
        case EventType::RepairStarted:         return "repair_started";
        case EventType::RepairCompleted:       return "repair_completed";
        case EventType::ConfirmationRequired:  return "confirmation_required";
        case EventType::RenderStarted:         return "render_started";
        case EventType::RenderCompleted:       return "render_completed";
        case EventType::AssetCommitted:        return "asset_committed";
        case EventType::SongMutationApplied:   return "song_mutation_applied";
        case EventType::WorkflowCancelled:     return "workflow_cancelled";
        case EventType::WorkflowCompleted:     return "workflow_completed";
    }
    return "unknown";
}

std::atomic<uint64_t> AgenticWorkflow::s_nextWorkflowId_{1};

/// AI-10.4: Concise, musical/application-level explanation of a canonical step
/// (used in StepStarted / StepCompleted events — no implementation noise).
static const char* stepExplain(AgenticWorkflow::Step s) noexcept
{
    switch (s) {
        case AgenticWorkflow::Step::InspectProject:       return "Inspecting the project";
        case AgenticWorkflow::Step::InspectSong:          return "Inspecting the current song";
        case AgenticWorkflow::Step::InspectAssets:        return "Checking the available instruments";
        case AgenticWorkflow::Step::GeneratePattern:      return "Generating the pattern";
        case AgenticWorkflow::Step::Validate:             return "Validating the notation";
        case AgenticWorkflow::Step::Compile:              return "Compiling the instrument";
        case AgenticWorkflow::Step::Audition:             return "Auditioning the result";
        case AgenticWorkflow::Step::InspectDiagnostics:   return "Checking for errors";
        case AgenticWorkflow::Step::Repair:               return "Repairing the issue";
        case AgenticWorkflow::Step::CreativeRepair:         return "Applying creative repair from feedback";
        case AgenticWorkflow::Step::Render:               return "Rendering the instrument to audio";
        case AgenticWorkflow::Step::BindAsset:            return "Committing the rendered asset";
        case AgenticWorkflow::Step::UpdateSong:           return "Updating the song file";
        case AgenticWorkflow::Step::None:                 return "";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Internal audit helper (stderr — canonical pattern from AI-6/AI-7)
// ---------------------------------------------------------------------------

static void auditLog(std::string_view action,
                     std::string_view target,
                     bool success,
                     std::string_view detail = "") noexcept
{
    std::ostringstream log;
    log << "[AI-10 AUDIT] action=" << action
        << " target=" << target
        << " success=" << (success ? "true" : "false");
    if (!detail.empty())
        log << " detail=" << detail;
    std::fprintf(stderr, "%s\n", log.str().c_str());
}

/// Serialise a change-set revert plan to a human-readable JSON preview
/// (does NOT grant authorization to execute destructive actions).
static nlohmann::json revertPlanToJson(
    const std::vector<ChangeSetManager::RevertAction>& plan)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& a : plan) {
        nlohmann::json j;
        j["action"] = a.kind;
        j["resource_id"] = a.resourceId;
        j["destructive"] = a.destructive;
        if (a.kind == "restore_song")
            j["song_file"] = a.songFile;
        if (a.kind == "remove_asset")
            j["asset_name"] = a.assetName;
        arr.push_back(std::move(j));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AgenticWorkflow::AgenticWorkflow(AudioEngineFacade& audio,
                                  SampleBank& bank,
                                  ProjectReadFacade& readFacade,
                                  ChuckSessionService& chuckService,
                                  RenderService& renderService,
                                  SongMutationService& songService)
    : audio_(audio)
    , bank_(bank)
    , readFacade_(readFacade)
    , chuckService_(chuckService)
    , renderService_(renderService)
    , songService_(songService)
     , planner_(readFacade, chuckService, renderService)
     , creativeRepairEngine_(workingSet_, songService, chuckService)
{}

AgenticWorkflow::~AgenticWorkflow()
{
    if (workflowThread_.joinable()) {
        stopRequested_.store(true, std::memory_order_release);
        cv_.notify_all();
        confirmCv_.notify_all();
        workflowThread_.join();
    }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool AgenticWorkflow::start(Request request,
                            ProgressCallback onProgress,
                            ConfirmationCallback onConfirmation)
{
    // The initial state + request are set under stateMtx_, but the
    // WorkflowStarted event is emitted AFTER the lock is released:
    // emitEvent() calls getState() which itself acquires stateMtx_, so it
    // must never be invoked while the caller holds that lock (self-deadlock).
    std::string startMessage;

    {
        std::lock_guard<std::mutex> lock(stateMtx_);

        if (state_ != State::Idle) {
            return false;
        }

        state_ = State::Queued;
        currentRequest_ = std::move(request);
        progressCallback_ = std::move(onProgress);
        confirmationCallback_ = std::move(onConfirmation);
        stopRequested_.store(false, std::memory_order_release);
        confirmationResponded_.store(false, std::memory_order_release);
        completedSteps_.clear();
        appliedChanges_.clear();
        error_.reset();
        repairAttempts_ = 0;
        renderCompleted_ = false;

        // AI-10.4: Assign a fresh workflow identity for event scoping.
        workflowId_ = s_nextWorkflowId_.fetch_add(1, std::memory_order_acq_rel);

        // AI-10.2: Record the user intent in the working set so that aliases
        // (e.g. "the bass") can be derived from intent keywords.
        workingSet_.setLastIntent(currentRequest_.intent);

        // AI-10.3: Begin a fresh, pending change-set for this workflow run so
        // that every persistent mutation is grouped into one coherent,
        // reviewable unit.
        changeSetManager_.beginChangeSet(currentRequest_.intent);

        // Build the start message while we still hold the current request.
        startMessage = std::string("Starting the ") +
                       (currentRequest_.dryRun ? "dry-run " : "") +
                       "workflow for \"" + currentRequest_.intent + "\"";
    }

    // Emit the initial queued state (AI-10.4: workflow started), lock-free.
    if (progressCallback_) {
        emitEvent(EventType::WorkflowStarted, startMessage, true, {},
                  {{"queued", true}});
    }

    // Launch the workflow thread.
    workflowThread_ = std::thread([this] { runWorkflow(); });

    return true;
}

// ---------------------------------------------------------------------------
// AI-10.5: Creative repair entry point
// ---------------------------------------------------------------------------

bool AgenticWorkflow::startCreativeRepair(std::string_view feedback,
                                          std::string_view intentContext,
                                          ProgressCallback onProgress,
                                          ConfirmationCallback onConfirmation)
{
    if (feedback.empty()) {
        return false;
    }

    Request req;
    req.feedback = std::string(feedback);
    // Build a synthetic intent for plan/change-set labeling.
    req.intent = "creative repair: " + std::string(feedback);
    if (!intentContext.empty())
        req.intent += " (" + std::string(intentContext) + ")";

    return start(std::move(req),
                 std::move(onProgress),
                 std::move(onConfirmation));
}

bool AgenticWorkflow::cancel()
{
    bool wasRunning = false;
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        if (state_ == State::Idle || state_ == State::Queued)
            return false;
        wasRunning = true;
        stopRequested_.store(true, std::memory_order_release);
    }

    cv_.notify_all();
    confirmCv_.notify_all();

    if (wasRunning && workflowThread_.joinable())
        workflowThread_.join();

    return wasRunning;
}

bool AgenticWorkflow::respondToConfirmation(bool approved)
{
    {
        std::lock_guard<std::mutex> lock(confirmMtx_);
        if (!pendingConfirmation_.has_value())
            return false;
        pendingConfirmation_.reset();
    }

    confirmationApproved_.store(approved, std::memory_order_release);
    confirmationResponded_.store(true, std::memory_order_release);
    confirmCv_.notify_all();
    return true;
}

nlohmann::json AgenticWorkflow::getState() const
{
    std::lock_guard<std::mutex> lock(stateMtx_);

    nlohmann::json j;
    j["state"] = stateName(state_);
    j["current_step"] = stepName(currentStep_);
    j["completed_steps"] = completedSteps_;

    if (!currentStepResult_.is_null())
        j["current_step_result"] = currentStepResult_;

    if (!diagnostics_.is_null())
        j["diagnostics"] = diagnostics_;

    if (!renderStatus_.is_null())
        j["render_status"] = renderStatus_;

    if (pendingConfirmation_.has_value()) {
        const auto& cr = *pendingConfirmation_;
        j["pending_confirmation"] = {
            {"request_id",       cr.requestId},
            {"action",           cr.action},
            {"description",      cr.description},
            {"details",          cr.details},
            {"capability_class", cr.capabilityClass}
        };
    } else {
        j["pending_confirmation"] = nullptr;
    }

    j["applied_changes"] = appliedChanges_;
    j["repair_attempts"] = repairAttempts_;
    j["dry_run"] = currentRequest_.dryRun;

    if (error_.has_value())
        j["error"] = *error_;
    else
        j["error"] = nullptr;

    return j;
}

bool AgenticWorkflow::isRunning() const noexcept
{
    std::lock_guard<std::mutex> lock(stateMtx_);
    return state_ != State::Idle && state_ != State::Completed
           && state_ != State::Failed && state_ != State::Cancelled;
}

void AgenticWorkflow::reset()
{
    std::lock_guard<std::mutex> lock(stateMtx_);
    state_ = State::Idle;
    currentStep_ = Step::None;
    completedSteps_.clear();
    currentStepResult_.clear();
    diagnostics_.clear();
    renderStatus_.clear();
    pendingConfirmation_.reset();
    appliedChanges_.clear();
    error_.reset();
    repairAttempts_ = 0;
    renderCompleted_ = false;
    renderJobId_ = 0;
    sessionId_.clear();
    currentRepairPlan_.reset();
    // Note: projectInfo_/songInfo_/assetsInfo_ are cleared on the next start().

    // AI-10.2: The working set is NOT cleared here — it persists across
    // workflow runs for multi-turn conversational continuity.  Use
    // clearWorkingSet() to explicitly reset session-scoped memory.
    stopRequested_.store(false, std::memory_order_release);
    confirmationResponded_.store(false, std::memory_order_release);

    // If a thread is joinable from a previous run (shouldn't happen if
    // properly waited), join it.
    if (workflowThread_.joinable())
        workflowThread_.join();
}

// ---------------------------------------------------------------------------
// Workflow execution
// ---------------------------------------------------------------------------

void AgenticWorkflow::runWorkflow()
{
    // AI-10.2: Set the active slot from the request so that pronoun
    // resolution ("it", "that pattern") targets the correct slot.
    if (!currentRequest_.targetSlot.empty())
        workingSet_.setActiveSlot(currentRequest_.targetSlot);

    // AI-10.5: If the request carries creative feedback, branch to the
    // creative repair path instead of the full generation workflow.
    if (!currentRequest_.feedback.empty()) {
        runCreativeRepair();
        return;
    }

    // Phase 1: PLANNING — analyse the request and assemble a plan.
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        state_ = State::Planning;
    }
    emitEvent(EventType::StepStarted, "Planning the approach", true, "planning");

    if (checkCancellation()) {
        setState(State::Cancelled);
        return;
    }

    // Determine workflow mode: ChucK instrument vs. mini-notation pattern.
    const bool isChuckWorkflow = !currentRequest_.ckSource.empty();
    const bool isPatternWorkflow = !currentRequest_.notation.empty();

    if (!isChuckWorkflow && !isPatternWorkflow) {
        error_ = "request must specify either 'notation' or 'ck_source'";
        setState(State::Failed);
        auditLog("workflow_start", "agentic", false, *error_);
        return;
    }

    // Emit the plan for observability (AI-10.1, AI-10.4).
    {
        std::lock_guard<std::mutex> lock(stateMtx_);

        // Use IntentPlanner to produce a structured, inspectable plan.
        // If the request carries a pre-determined plan, it is validated
        // and used; otherwise the planner derives one from the intent.
        PlanModel plan = planner_.planFromRequestWithOverride(
            currentRequest_.intent,
            currentRequest_.targetSlot,
            currentRequest_.assetName,
            currentRequest_.durationBars,
            currentRequest_.dryRun,
            currentRequest_.plan);

        currentStepResult_ = plan.toJson();
    }
    // AI-10.4: The plan is now observable as a dedicated event.
    // (Emitted lock-free — emitEvent() calls getState(), which acquires
    // stateMtx_; it must not be called while that lock is held.)
    emitEvent(EventType::PlanCreated,
              currentStepResult_.value("intent",
                  std::string("Assembled a plan")) + " — planned " +
                  std::to_string(currentStepResult_.value("steps",
                      nlohmann::json::array()).size()) + " steps",
              true, "planning", currentStepResult_);

    // Phase 2: INSPECTION — inspect_project, inspect_song, inspect_assets.
    setState(State::Inspecting);

    for (const auto step : { Step::InspectProject, Step::InspectSong,
                             Step::InspectAssets }) {
        setCurrentStep(step);

        if (checkCancellation()) {
            setState(State::Cancelled);
            return;
        }

        StepResult result;
        switch (step) {
            case Step::InspectProject: result = stepInspectProject(); break;
            case Step::InspectSong:    result = stepInspectSong();    break;
            case Step::InspectAssets:  result = stepInspectAssets();  break;
            default: result.ok = false; result.message = "unexpected step"; break;
        }

        if (!result.ok) {
            error_ = std::string(stepName(step)) + ": " + result.message;
            setState(State::Failed);
            auditLog(std::string("step_") + stepName(step), "workflow", false, result.message);
            return;
        }

        completeStep(stepName(step), result);

        if (checkCancellation()) {
            setState(State::Cancelled);
            return;
        }
    }

    // Phase 3: EDITING — generate/modify pattern.
    setCurrentStep(Step::GeneratePattern);
    setState(State::Editing);

    if (checkCancellation()) {
        setState(State::Cancelled);
        return;
    }

    {
        StepResult result = stepGeneratePattern();
        if (!result.ok) {
            error_ = "generate_pattern: " + result.message;
            setState(State::Failed);
            auditLog("step_generate_pattern", "workflow", false, result.message);
            return;
        }
        completeStep("generate_pattern", result);
    }

    // Phase 4: VALIDATION — validate the generated content.
    setCurrentStep(Step::Validate);
    setState(State::Validating);

    if (checkCancellation()) {
        setState(State::Cancelled);
        return;
    }

    {
        StepResult result = stepValidate();
        diagnostics_ = result.data;
        if (!result.ok) {
            error_ = "validate: " + result.message;
            setState(State::Failed);
            auditLog("step_validate", "workflow", false, result.message);
            return;
        }
        completeStep("validate", result);
    }

    // Phase 5: COMPILE (ChucK workflows only).
    if (isChuckWorkflow) {
        setCurrentStep(Step::Compile);
        setState(State::Compiling);

        if (checkCancellation()) {
            setState(State::Cancelled);
            return;
        }

        {
            StepResult result = stepCompile();
            diagnostics_ = result.data;
            if (!result.ok) {
                // Compile failure — enter repair loop.
                goto repair_loop;
            }
            completeStep("compile", result);
        }
    }

    // Phase 6: AUDITION.
    setCurrentStep(Step::Audition);
    setState(State::Auditioning);

    if (checkCancellation()) {
        setState(State::Cancelled);
        return;
    }

    {
        StepResult result = stepAudition();
        if (!result.ok) {
            error_ = "audition: " + result.message;
            setState(State::Failed);
            auditLog("step_audition", "workflow", false, result.message);
            return;
        }
        completeStep("audition", result);
    }

    // Phase 7: INSPECT DIAGNOSTICS.
    setCurrentStep(Step::InspectDiagnostics);
    setState(State::InspectingDiagnostics);

    if (checkCancellation()) {
        setState(State::Cancelled);
        return;
    }

    {
        StepResult result = stepInspectDiagnostics();
        diagnostics_ = result.data;
        if (!result.ok) {
            // Diagnostics show errors — enter repair loop.
            goto repair_loop;
        }
        completeStep("inspect_diagnostics", result);
    }

    // Phase 8: REPAIR LOOP (if needed).
    {
    repair_loop:
        if (checkCancellation()) {
            setState(State::Cancelled);
            return;
        }

        // Determine whether repair is possible and needed.
        // The repair loop handles both compile errors and diagnostic errors.
        const int maxAttempts = isChuckWorkflow ? kMaxRepairAttempts : kMaxRepairAttempts;

        while (repairAttempts_ < maxAttempts) {
            repairAttempts_++;

            setCurrentStep(Step::Repair);
            setState(State::Repairing);
            emitEvent(EventType::RepairStarted,
                      "Repair attempt " + std::to_string(repairAttempts_) +
                          " of " + std::to_string(maxAttempts) +
                          " (attempting to fix the last failure)");

            if (checkCancellation()) {
                setState(State::Cancelled);
                return;
            }

            {
                StepResult result = stepRepair();
                if (!result.ok) {
                    error_ = "repair: " + result.message;
                    setState(State::Failed);
                    auditLog("step_repair", "workflow", false, result.message);
                    return;
                }
                completeStep("repair", result);
                emitEvent(EventType::RepairCompleted,
                          "Repair attempt " + std::to_string(repairAttempts_) +
                              " succeeded",
                          true, "repair", result.data);
            }

            // Re-validate.
            setCurrentStep(Step::Validate);
            setState(State::Validating);

            {
                StepResult result = stepValidate();
                diagnostics_ = result.data;
                if (result.ok) {
                    completeStep("validate", result);
                    break;  // Validation passed, exit repair loop.
                }
                completeStep("validate", result);
            }

            // Re-compile (for ChucK workflows) and re-audition.
            if (isChuckWorkflow) {
                setCurrentStep(Step::Compile);
                setState(State::Compiling);

                StepResult compileResult = stepCompile();
                diagnostics_ = compileResult.data;
                if (!compileResult.ok) {
                    completeStep("compile", compileResult);
                    // Loop again for another repair attempt.
                    continue;
                }
                completeStep("compile", compileResult);
            }

            setCurrentStep(Step::Audition);
            setState(State::Auditioning);

            StepResult auditionResult = stepAudition();
            if (!auditionResult.ok) {
                completeStep("audition", auditionResult);
                continue;  // Try another repair.
            }
            completeStep("audition", auditionResult);

            // Re-check diagnostics.
            setCurrentStep(Step::InspectDiagnostics);
            setState(State::InspectingDiagnostics);
            StepResult diagResult = stepInspectDiagnostics();
            diagnostics_ = diagResult.data;
            if (diagResult.ok)
                break;  // Diagnostics clean, exit repair loop.
        }

        if (repairAttempts_ >= maxAttempts) {
            error_ = "repair exceeded maximum attempts (" +
                     std::to_string(maxAttempts) + ")";
            setState(State::Failed);
            auditLog("workflow_repair", "agentic", false, *error_);
            return;
        }
    }

    // Phase 9: RENDER (ChucK workflows only).
    if (isChuckWorkflow) {
        setCurrentStep(Step::Render);
        setState(State::Rendering);

        if (checkCancellation()) {
            setState(State::Cancelled);
            return;
        }

        {
            StepResult result = stepRender();
            renderStatus_ = result.data;
            if (!result.ok) {
                error_ = "render: " + result.message;
                setState(State::Failed);
                auditLog("step_render", "workflow", false, result.message);
                return;
            }
            completeStep("render", result);
        }
    }

    // Phase 10: BIND ASSET (ChucK workflows only, requires confirmation).
    if (isChuckWorkflow) {
        setCurrentStep(Step::BindAsset);
        setState(State::Binding);

        if (checkCancellation()) {
            setState(State::Cancelled);
            return;
        }

        // Determine if the asset already exists (collision check).
        const auto projectDir = audio_.currentProjectDir();
        hathor::AssetPathResolver resolver(projectDir);
        auto resolveResult = resolver.resolveStudio(
            hathor::sanitizeAssetName(currentRequest_.assetName));

        std::error_code ec;
        const bool assetExists =
            std::filesystem::exists(resolveResult.path, ec) ||
            []() {
                // Also check .ck path existence (handled below)
                return false;
            }();

        auto ckPath = resolveResult.path;
        ckPath.replace_extension(".ck");
        const bool ckExists = std::filesystem::exists(ckPath, ec);

        if (assetExists || ckExists) {
            nlohmann::json details;
            details["asset_name"] = currentRequest_.assetName;
            details["wav_path"] = resolveResult.path.string();
            details["ck_path"] = ckPath.string();
            details["wav_exists"] = assetExists;
            details["ck_exists"] = ckExists;

            if (!waitForConfirmation("commit_rendered_asset",
                                     "persistent_mutation",
                                     "An instrument with this name already exists. Overwrite?",
                                     details)) {
                error_ = "bind_asset: user rejected overwrite confirmation";
                setState(State::Failed);
                auditLog("step_bind_asset", "workflow", false, "user rejected overwrite");
                return;
            }
        }

        {
            StepResult result = stepBindAsset();
            if (!result.ok) {
                error_ = "bind_asset: " + result.message;
                setState(State::Failed);
                auditLog("step_bind_asset", "workflow", false, result.message);
                return;
            }
            completeStep("bind_asset", result);
            appliedChanges_.push_back(result.data);
        }
    }

    // Phase 11: UPDATE SONG (requires confirmation in non-dry-run mode).
    {
        setCurrentStep(Step::UpdateSong);
        setState(State::UpdatingSong);

        if (checkCancellation()) {
            setState(State::Cancelled);
            return;
        }

        // The final song mutation must go through the canonical
        // SongMutationService (AI-7) — never direct file writes (AI-10 requirement).
        // Requires confirmation because it is a persistent mutation.

        nlohmann::json details;
        details["target_slot"] = currentRequest_.targetSlot;
        if (isChuckWorkflow) {
            details["asset_reference"] = currentRequest_.assetName;
        } else {
            details["notation"] = generatedNotation_;
        }
        details["dry_run"] = currentRequest_.dryRun;

        bool needsConfirm = !currentRequest_.dryRun;

        if (needsConfirm) {
            if (!waitForConfirmation("edit_song",
                                     "persistent_mutation",
                                     "Apply this change to the song file (persists to disk)?",
                                     details)) {
                error_ = "update_song: user rejected confirmation";
                setState(State::Failed);
                auditLog("step_update_song", "workflow", false, "user rejected confirmation");
                return;
            }
        }

        {
            StepResult result = stepUpdateSong();
            if (!result.ok) {
                error_ = "update_song: " + result.message;
                setState(State::Failed);
                auditLog("step_update_song", "workflow", false, result.message);
                return;
            }
            completeStep("update_song", result);
            appliedChanges_.push_back(result.data);
        }
    }

    // Phase 12: COMPLETED.
    setState(State::Completed);
    auditLog("workflow_complete", "agentic", true,
             "steps=" + std::to_string(completedSteps_.size()));
}

// ---------------------------------------------------------------------------
// Step implementations
// ---------------------------------------------------------------------------

AgenticWorkflow::StepResult AgenticWorkflow::stepInspectProject()
{
    auto result = readFacade_.inspectProject();

    StepResult sr;
    sr.ok = result.value("ok", false);
    if (!sr.ok) {
        sr.message = result.value("error", std::string("inspect_project failed"));
        return sr;
    }

    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        projectInfo_ = result;
    }

    sr.message = "project inspected";
    sr.data = result;
    return sr;
}

AgenticWorkflow::StepResult AgenticWorkflow::stepInspectSong()
{
    auto result = readFacade_.getCurrentSong();

    StepResult sr;
    sr.ok = result.value("ok", false);
    if (!sr.ok) {
        sr.message = result.value("error", std::string("get_current_song failed"));
        return sr;
    }

    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        songInfo_ = result;
    }

    sr.message = "song inspected";
    sr.data = result;
    return sr;
}

AgenticWorkflow::StepResult AgenticWorkflow::stepInspectAssets()
{
    auto result = readFacade_.listAssets();

    StepResult sr;
    sr.ok = result.value("ok", false);
    if (!sr.ok) {
        sr.message = result.value("error", std::string("list_assets failed"));
        return sr;
    }

    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        assetsInfo_ = result;
    }

    sr.message = "assets inspected";
    sr.data = result;
    return sr;
}

AgenticWorkflow::StepResult AgenticWorkflow::stepGeneratePattern()
{
    StepResult sr;
    sr.stepName = "generate_pattern";

    const bool isChuckWorkflow = !currentRequest_.ckSource.empty();

    if (isChuckWorkflow) {
        // Generate ChucK: create a session for the target slot.
        // We do NOT create a live VM here (AI-5 §3: createSession is Open only).
        // The VM is activated in the audition step.
        if (currentRequest_.targetSlot.empty()) {
            sr.ok = false;
            sr.message = "ck workflow requires target_slot (e.g. 'd1')";
            return sr;
        }

        const int slotIdx = [&] {
            // Map slot name to index via AudioEngineFacade.
            return audio_.findOrAddSlot(currentRequest_.targetSlot);
        }();

        if (slotIdx < 0) {
            sr.ok = false;
            sr.message = "no free slot available for ChucK session";
            return sr;
        }

        // Create the ChucK session (non-destructive — just metadata).
        auto session = chuckService_.createSession(
            static_cast<uint8_t>(slotIdx),
            currentRequest_.ckSource);

        {
            std::lock_guard<std::mutex> lock(stateMtx_);
            sessionId_ = session.sessionId;
            generatedCkSource_ = session.source;
        }

        nlohmann::json data;
        data["session_id"] = session.sessionId;
        data["slot_index"] = slotIdx;

        sr.ok = true;
        sr.message = "ChucK session created (not yet compiled)";
        sr.data = data;
        return sr;
    } else {
        // Generate pattern: validate the notation via the real parseMini().
        const auto parseResult = hathor::parseMini(currentRequest_.notation);

        if (std::holds_alternative<hathor::ParseError>(parseResult)) {
            const auto& err = std::get<hathor::ParseError>(parseResult);
            sr.ok = false;
            sr.message = "pattern parse error: " + err.message;
            sr.data["position"] = static_cast<int>(err.position);
            sr.data["step"] = "generate_pattern";
            return sr;
        }

        const auto& compiled = std::get<hathor::CompiledPattern>(parseResult);
        const std::string canonical = hathor::printMini(compiled);

        // Lower Pattern<std::string> → Pattern<ParamMap> and store in the slot
        // so stepAudition can play it (mirrors WorkerThread::workerLoop).
        auto paramPattern = hathor::lowerToParamMap(compiled.pattern);
        const std::size_t maxEvents = paramPattern.maxEventsPerCycle();

        const int slotIdx = audio_.findOrAddSlot(currentRequest_.targetSlot);
        if (slotIdx < 0) {
            sr.ok = false;
            sr.message = "no free slot available for pattern";
            return sr;
        }

        auto slotState = std::make_shared<SlotState>();
        slotState->pattern = std::make_shared<hathor::Pattern<hathor::ParamMap>>(
            std::move(paramPattern));

        // Pre-allocate the event buffer (same as WorkerThread).
        {
            const hathor::Rational zero{0, 1};
            const hathor::Arc      zeroArc{zero, zero};
            const hathor::Event<hathor::ParamMap> dummy{zeroArc, zeroArc, {}};
            slotState->eventBuffer.assign(maxEvents, dummy);
        }

        slotState->notation = canonical;
        audio_.storeSlot(slotIdx, std::move(slotState));

        {
            std::lock_guard<std::mutex> lock(stateMtx_);
            generatedNotation_ = canonical;
        }

        nlohmann::json data;
        data["canonical_notation"] = canonical;
        data["slot"] = currentRequest_.targetSlot;
        data["slot_index"] = slotIdx;
        data["event_count_per_cycle"] = static_cast<int>(maxEvents);
        data["source"] = "slot:" + currentRequest_.targetSlot;

        sr.ok = true;
        sr.message = "pattern generated and stored on slot";
        sr.data = std::move(data);
        return sr;
    }
}

AgenticWorkflow::StepResult AgenticWorkflow::stepValidate()
{
    StepResult sr;
    sr.stepName = "validate";

    // AI-10.5: In creative repair mode, validate the repaired content
    // (from the repair plan) rather than the original request content.
    std::string validateNotation;
    std::string validateCkSource;
    bool isCreativeRepair = false;
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        if (currentRepairPlan_.has_value()) {
            isCreativeRepair = true;
            if (currentRepairPlan_->targetDomain ==
                CreativeRepairEngine::TargetDomain::Pattern) {
                validateNotation = currentRepairPlan_->targetNotation;
            } else if (currentRepairPlan_->targetDomain ==
                       CreativeRepairEngine::TargetDomain::Instrument) {
                validateCkSource = currentRepairPlan_->targetSource;
            }
        }
    }

    bool isChuckWorkflow = !currentRequest_.ckSource.empty() || isCreativeRepair;
    // For creative repair on patterns, isChuckWorkflow should be false.
    if (isCreativeRepair) {
        isChuckWorkflow = !validateCkSource.empty();
    }

    if (isChuckWorkflow) {
        // Validate ChucK source via the real compiler diagnostics path.
        // This is the same validateChuckSource() called by ChuckCompiler
        // on B4-K4 — never an approximate parser (AI-5/AI-18).
        const auto diag = hathor::audio_worker::validateChuckSource(
            isCreativeRepair ? validateCkSource : currentRequest_.ckSource);

        nlohmann::json diags = nlohmann::json::array();
        if (!diag.ok) {
            diags.push_back({
                {"severity", "error"},
                {"code",     "CK_COMPILE_ERROR"},
                {"message",  diag.message},
                {"line",     diag.errorLine},
                {"column",   diag.errorColumn}
            });
            sr.ok = false;
            sr.message = "ChucK source failed validation";
        } else {
            diags.push_back({
                {"severity", "info"},
                {"code",     "CK_OK"},
                {"message",  "ChucK source passed validation"}
            });
            sr.ok = true;
            sr.message = "ChucK source validated";
        }
        const bool ckHasErrors = !sr.ok;
        sr.data["diagnostics"] = std::move(diags);
        sr.data["source"] = "chuck_compiler";
        emitEvent(EventType::DiagnosticsDiscovered,
                  ckHasErrors ? "The instrument source has problems to resolve"
                              : "The instrument source validates cleanly",
                  !ckHasErrors, "validate", {{"diagnostics", sr.data["diagnostics"]}});
        return sr;
    } else {
        // Validate mini-notation via the real parseMini().
        const auto parseResult = hathor::parseMini(
            isCreativeRepair ? validateNotation : currentRequest_.notation);

        nlohmann::json diags = nlohmann::json::array();
        if (std::holds_alternative<hathor::ParseError>(parseResult)) {
            const auto& err = std::get<hathor::ParseError>(parseResult);
            diags.push_back({
                {"severity", "error"},
                {"code",     "PARSE_ERROR"},
                {"message",  err.message},
                {"position", static_cast<int>(err.position)}
            });
            sr.ok = false;
            sr.message = "mini-notation parse error";
        } else {
            // Check for TK_ERROR tokens (unrecognised characters).
            const auto tokens = hathor::tokenise(
                isCreativeRepair ? validateNotation : currentRequest_.notation);
            bool hasError = false;
            for (const auto& tok : tokens) {
                if (tok.kind == hathor::TokenKind::TK_ERROR) {
                    diags.push_back({
                        {"severity", "error"},
                        {"code",     "TOKENIZER_ERROR"},
                        {"message",  std::string("unrecognised character '") + std::string(tok.text) + "'"}
                    });
                    hasError = true;
                }
            }
            if (!hasError) {
                diags.push_back({
                    {"severity", "info"},
                    {"code",     "MINI_PARSE_OK"},
                    {"message",  "mini-notation parsed successfully"}
                });
            }
            sr.ok = !hasError;
            sr.message = hasError ? "tokeniser errors found" : "notation validated";
        }
        const bool mnHasErrors = !sr.ok;
        sr.data["diagnostics"] = std::move(diags);
        sr.data["source"] = "miniparser";
        emitEvent(EventType::DiagnosticsDiscovered,
                  mnHasErrors ? "The pattern has syntax problems to resolve"
                              : "The pattern notation validates cleanly",
                  !mnHasErrors, "validate", {{"diagnostics", sr.data["diagnostics"]}});
        return sr;
    }
}

AgenticWorkflow::StepResult AgenticWorkflow::stepCompile()
{
    StepResult sr;
    sr.stepName = "compile";

    std::string sessionId;
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        sessionId = sessionId_;
    }

    if (sessionId.empty()) {
        sr.ok = false;
        sr.message = "no ChucK session to compile";
        return sr;
    }

    // Compile via the canonical ChuckSessionService (AI-5).
    // compileChuck() is asynchronous — returns immediately with a job ID for status polling.
    // It routes through the real libchuck compiler (validateChuckSource in
    // this JUCE-free build, or ck.compileCode in the worker process).

    // AI-10.5: In creative repair mode, compile the repaired source.
    std::string compileSource = currentRequest_.ckSource;
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        if (currentRepairPlan_.has_value() &&
            currentRepairPlan_->targetDomain ==
                CreativeRepairEngine::TargetDomain::Instrument) {
            compileSource = currentRepairPlan_->targetSource;
        }
    }

    // Use a promise to bridge the async callback and capture the CompileResult.
    auto promise = std::make_shared<std::promise<hathor::CompileResult>>();
    auto future = promise->get_future();

    auto jobHandle = chuckService_.compileChuck(
        sessionId,
        compileSource,
        [promise](CompileResult cr) {
            promise->set_value(std::move(cr));
        });

    const uint64_t jobId = jobHandle.id();

    // Wait for the compile job to complete (with cancellation checking).
    nlohmann::json jobResult;

    if (!waitForAsyncJob(jobId, "chuck", jobResult)) {
        sr.data = jobResult;
        sr.data["job_id"] = jobId;

        const std::string status = jobResult.value("status", std::string("unknown"));
        if (status == "cancelled") {
            sr.ok = false;
            sr.message = "compile cancelled";
        } else {
            sr.ok = false;
            sr.message = "compile failed or timed out";
        }
        return sr;
    }

    sr.data = jobResult;
    sr.data["job_id"] = jobId;

    if (jobResult.value("status", std::string()) == "succeeded") {
        sr.ok = true;
        sr.message = "ChucK source compiled successfully";

        // Extract diagnostics from the compile result delivered via the callback.
        CompileResult cr;
        try {
            cr = future.get();
        } catch (...) {
            // Future may have been consumed already — fall through without
            // diagnostics if we can't retrieve the result.
        }

        nlohmann::json diags = nlohmann::json::array();
        for (const auto& d : cr.diagnostics) {
            diags.push_back({
                {"severity", d.severity},
                {"code",     d.code},
                {"message",  d.message},
                {"line",     d.line},
                {"column",   d.column}
            });
        }
        sr.data["diagnostics"] = std::move(diags);
    } else if (jobResult.value("status", std::string()) == "cancelled") {
        sr.ok = false;
        sr.message = "compile cancelled";
    } else {
        sr.ok = false;
        sr.message = "compile failed: " +
                     jobResult.value("error",
                                     jobResult.value("result", nlohmann::json{})
                                         .value("error", std::string("unknown error")));
    }

    return sr;
}

AgenticWorkflow::StepResult AgenticWorkflow::stepAudition()
{
    StepResult sr;
    sr.stepName = "audition";

    const bool isChuckWorkflow = !currentRequest_.ckSource.empty();

    if (isChuckWorkflow) {
        // Audition the ChucK session (AI-5 §11: audition_chuck).
        // This activates the per-tab VM (B4-K3 vm_activate).
        std::string sessionId;
        {
            std::lock_guard<std::mutex> lock(stateMtx_);
            sessionId = sessionId_;
        }

        if (sessionId.empty()) {
            sr.ok = false;
            sr.message = "no ChucK session to audition";
            return sr;
        }

        auto session = chuckService_.auditionSession(sessionId);

        sr.data["session_id"] = session.sessionId;
        sr.data["state"] = hathor::toString(session.state);

        if (session.state == hathor::SessionState::Live) {
            sr.ok = true;
            sr.message = "ChucK session is live (auditioning)";
        } else {
            sr.ok = false;
            sr.message = session.lastError.empty()
                ? "ChucK session not live (state: " + std::string(hathor::toString(session.state)) + ")"
                : "ChucK session audition failed: " + session.lastError;
        }
        return sr;
    } else {
        // Audition a mini-notation pattern: set the pattern and play the slot.
        // This goes through the canonical set-pattern path (WorkerThread)
        // and then slot-play (non-destructive runtime mutation).
        // We use the slot API directly since the pattern was already validated.

        const std::string slotName = currentRequest_.targetSlot;
        if (slotName.empty()) {
            sr.ok = false;
            sr.message = "audition requires target_slot for pattern workflow";
            return sr;
        }

        // Play the slot (non-destructive runtime state mutation).
        // The pattern was already set during the generate step via set-pattern.
        // Here we just start playback.
        int slotIdx = audio_.findOrAddSlot(slotName);
        if (slotIdx < 0) {
            sr.ok = false;
            sr.message = "no free slot for audition";
            return sr;
        }

        audio_.slotPlay(slotIdx);

        sr.ok = true;
        sr.message = "pattern playing on slot " + slotName;
        sr.data["slot"] = slotName;
        sr.data["slot_index"] = slotIdx;
        return sr;
    }
}

AgenticWorkflow::StepResult AgenticWorkflow::stepInspectDiagnostics()
{
    StepResult sr;
    sr.stepName = "inspect_diagnostics";

    // Check diagnostics from the last compile/validate step.
    // For ChucK: use the real compiler diagnostics (already captured in compile step).
    // For patterns: use the real mini-notation diagnostics (already captured in validate step).
    //
    // We also check runtime diagnostics via get_diagnostics on the source content.

    const bool isChuckWorkflow = !currentRequest_.ckSource.empty();

    nlohmann::json diags = nlohmann::json::array();

    if (isChuckWorkflow) {
        // Re-check ChucK diagnostics via the canonical path.
        auto diag = readFacade_.getDiagnostics(
            currentRequest_.ckSource,
            "ck:" + currentRequest_.targetSlot,
            true);

        if (diag.contains("diagnostics")) {
            for (const auto& d : diag["diagnostics"]) {
                if (d.value("severity", std::string()) == "error") {
                    diags.push_back(d);
                }
            }
        }
    } else {
        // Re-check mini-notation diagnostics via the canonical path.
        auto diag = readFacade_.getDiagnostics(
            currentRequest_.notation,
            "slot:" + currentRequest_.targetSlot,
            false);

        if (diag.contains("diagnostics")) {
            for (const auto& d : diag["diagnostics"]) {
                if (d.value("severity", std::string()) == "error") {
                    diags.push_back(d);
                }
            }
        }
    }

    sr.data["diagnostics"] = diags;
    sr.data["source"] = isChuckWorkflow ? "chuck_compiler" : "miniparser";
    sr.data["error_count"] = diags.size();

    if (!diags.empty()) {
        sr.ok = false;
        std::string msg;
        for (const auto& d : diags) {
            if (!msg.empty()) msg += "; ";
            msg += d.value("message", std::string("unknown diagnostic"));
        }
        sr.message = "diagnostics found " + std::to_string(diags.size()) + " error(s): " + msg;
    } else {
        sr.ok = true;
        sr.message = "no diagnostics errors found";
    }

    emitEvent(EventType::DiagnosticsDiscovered,
              sr.ok ? "No remaining errors after checking"
                    : "Diagnostics revealed " + std::to_string(diags.size()) +
                          " error(s): " + sr.message,
              sr.ok, "inspect_diagnostics",
              {{"diagnostics", diags}, {"error_count", diags.size()}});

    return sr;
}

AgenticWorkflow::StepResult AgenticWorkflow::stepRepair()
{
    StepResult sr;
    sr.stepName = "repair";

    const bool isChuckWorkflow = !currentRequest_.ckSource.empty();

    if (isChuckWorkflow) {
        // Repair ChucK source: re-validate the source.
        // The actual repair (fixing the source) is done by the AI agent
        // — this step just re-runs validation after the agent has provided
        // an updated source. For an autonomous workflow, we re-validate
        // the same source and report whether repair is possible.
        //
        // In a full implementation, the AI agent would modify ckSource
        // based on the diagnostics. Here we re-check the source.
        const auto diag = hathor::audio_worker::validateChuckSource(
            currentRequest_.ckSource);

        if (diag.ok) {
            sr.ok = true;
            sr.message = "ChucK source repaired (re-validated successfully)";
            sr.data["source"] = "chuck_compiler";
            sr.data["repair_attempt"] = repairAttempts_;
        } else {
            sr.ok = false;
            sr.message = "ChucK source still has errors after repair attempt " +
                         std::to_string(repairAttempts_);
            sr.data["source"] = "chuck_compiler";
            sr.data["repair_attempt"] = repairAttempts_;
            sr.data["error"] = diag.message;
            sr.data["line"] = diag.errorLine;
            sr.data["column"] = diag.errorColumn;
        }
        return sr;
    } else {
        // Repair mini-notation: re-parse the notation.
        const auto parseResult = hathor::parseMini(currentRequest_.notation);

        if (std::holds_alternative<hathor::CompiledPattern>(parseResult)) {
            sr.ok = true;
            sr.message = "notation repaired (re-validated successfully)";
            sr.data["source"] = "miniparser";
            sr.data["repair_attempt"] = repairAttempts_;
        } else {
            const auto& err = std::get<hathor::ParseError>(parseResult);
            sr.ok = false;
            sr.message = "notation still has errors after repair attempt " +
                         std::to_string(repairAttempts_);
            sr.data["source"] = "miniparser";
            sr.data["repair_attempt"] = repairAttempts_;
            sr.data["error"] = err.message;
            sr.data["position"] = static_cast<int>(err.position);
        }
        return sr;
    }
}

// ---------------------------------------------------------------------------
// AI-10.5: Creative repair workflow
// ---------------------------------------------------------------------------

void AgenticWorkflow::runCreativeRepair()
{
    // AI-10.5: Conversational creative repair workflow.
    //
    // This is a condensed workflow that:
    //   1. Plans the repair via CreativeRepairEngine (classify + resolve + plan)
    //   2. Executes the plan ops through canonical services
    //   3. Re-validates / re-auditions
    //   4. Records changes in the AI-10.3 change-set

    // Phase 1: Planning
    setState(State::Planning);
    emitEvent(EventType::StepStarted, "Planning creative repair", true, "planning");

    if (checkCancellation()) {
        setState(State::Cancelled);
        return;
    }

    // Use CreativeRepairEngine to classify feedback, resolve target, and plan.
    const std::string intentContext = currentRequest_.targetSlot.empty()
        ? std::string(currentRequest_.feedback)
        : currentRequest_.targetSlot;

    CreativeRepairEngine::RepairPlan plan =
        creativeRepairEngine_.planRepair(currentRequest_.feedback, intentContext);

    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        currentRepairPlan_ = plan;
    }

    // Emit the plan for observability (AI-10.1, AI-10.4, AI-10.5).
    emitEvent(EventType::PlanCreated,
              "Planned creative repair: " + (plan.explanation.empty()
                  ? std::string("no targeted change identified")
                  : plan.explanation),
              !plan.ops.empty(), "creative_repair", plan.toJson());

    if (plan.ops.empty()) {
        error_ = "creative repair: no actionable plan for feedback \"" +
                 std::string(currentRequest_.feedback) + "\"";
        setState(State::Failed);
        auditLog("workflow_repair", "agentic", false, *error_);
        return;
    }

    // AI-10.3: Begin a change-set for this repair.
    changeSetManager_.beginChangeSet(currentRequest_.feedback);

    // Phase 2: Execute the repair
    if (checkCancellation()) {
        setState(State::Cancelled);
        changeSetManager_.cancelCurrent();
        return;
    }

    setCurrentStep(Step::CreativeRepair);
    setState(State::CreativeRepairing);

    emitEvent(EventType::RepairStarted,
              "Applying creative repair from feedback: \"" +
                  currentRequest_.feedback + "\"",
              true, "creative_repair");

    if (checkCancellation()) {
        setState(State::Cancelled);
        changeSetManager_.cancelCurrent();
        return;
    }

    {
        StepResult result = stepCreativeRepair();
        if (!result.ok) {
            error_ = "creative repair: " + result.message;
            setState(State::Failed);
            changeSetManager_.cancelCurrent();
            auditLog("step_creative_repair", "workflow", false, result.message);
            emitEvent(EventType::StepFailed,
                      "Creative repair failed: " + result.message,
                      false, "creative_repair", result.data);
            return;
        }
        completeStep("creative_repair", result);
        emitEvent(EventType::RepairCompleted,
                  "Creative repair applied successfully",
                  true, "creative_repair", result.data);
    }

    // Phase 3: Re-validate (pattern mode) or re-compile + re-audition (ChucK mode)
    if (plan.targetDomain == CreativeRepairEngine::TargetDomain::Pattern) {
        setCurrentStep(Step::Validate);
        setState(State::Validating);

        StepResult valResult = stepValidate();
        diagnostics_ = valResult.data;
        if (!valResult.ok) {
            completeStep("validate", valResult);
            error_ = "creative repair re-validation failed: " + valResult.message;
            setState(State::Failed);
            auditLog("step_validate", "workflow", false, valResult.message);
            return;
        }
        completeStep("validate", valResult);

        // Re-audition the repaired pattern.
        setCurrentStep(Step::Audition);
        setState(State::Auditioning);

        StepResult audResult = stepAudition();
        if (!audResult.ok) {
            completeStep("audition", audResult);
            error_ = "creative repair re-audition failed: " + audResult.message;
            setState(State::Failed);
            auditLog("step_audition", "workflow", false, audResult.message);
            return;
        }
        completeStep("audition", audResult);
    } else if (plan.targetDomain == CreativeRepairEngine::TargetDomain::Instrument) {
        // For ChucK repairs, compile + audition is handled in stepCreativeRepair.
        // Re-validate the repaired source.
        setCurrentStep(Step::Validate);
        setState(State::Validating);

        StepResult valResult = stepValidate();
        diagnostics_ = valResult.data;
        if (!valResult.ok) {
            completeStep("validate", valResult);
            error_ = "creative repair re-validation failed: " + valResult.message;
            setState(State::Failed);
            return;
        }
        completeStep("validate", valResult);
    }

    // Phase 4: Complete
    setState(State::Completed);
}

AgenticWorkflow::StepResult AgenticWorkflow::stepCreativeRepair()
{
    StepResult sr;
    sr.stepName = "creative_repair";

    if (!currentRepairPlan_.has_value()) {
        sr.ok = false;
        sr.message = "no creative repair plan loaded";
        return sr;
    }

    const auto& plan = *currentRepairPlan_;
    nlohmann::json details;
    details["feedback"] = plan.feedback;
    details["property"] = hathor::control::propertyToString(plan.property);
    details["target_domain"] = hathor::control::domainToString(plan.targetDomain);
    details["slot_name"] = plan.slotName;

    bool anyPersistent = false;

    // Execute each operation in the plan through the canonical services.
    for (const auto& op : plan.ops) {
        if (op.capabilityClass == CreativeRepairEngine::CapabilityClass::PersistentMutation) {
            if (op.requiresConfirmation && !currentRequest_.dryRun) {
                // AI-1: persistent mutation crosses the confirmation boundary.
                // Pause and wait for approval.
                ConfirmationRequest req;
                req.requestId = nextConfirmationId_.fetch_add(1, std::memory_order_relaxed);
                req.action = op.op;
                req.description = op.description;
                req.details = op.params;
                req.capabilityClass = "persistent_mutation";

                pendingConfirmation_ = req;
                confirmationCallback_(req);

                // Wait for approval.
                std::unique_lock<std::mutex> lk(confirmMtx_);
                confirmCv_.wait(lk, [this] {
                    return confirmationResponded_.load(std::memory_order_acquire);
                });

                if (checkCancellation()) {
                    sr.ok = false;
                    sr.message = "creative repair cancelled during confirmation";
                    return sr;
                }

                if (!confirmationApproved_.load(std::memory_order_acquire)) {
                    sr.ok = false;
                    sr.message = "creative repair rejected by user";
                    sr.data = details;
                    return sr;
                }

                pendingConfirmation_.reset();
            }
            anyPersistent = true;
        }
    }

    // Execute the actual mutations through canonical services.
    nlohmann::json appliedOps = nlohmann::json::array();

    for (const auto& op : plan.ops) {
        if (op.service == "SongMutationService") {
            // Pattern repair via SongMutationService::editSong
            std::string songFile = op.params.value("song_file", std::string{});
            if (songFile.empty())
                songFile = plan.slotName + ".hathor";

            // Capture before-content for AI-10.3 change-set.
            std::string beforeContent;
            if (!currentRequest_.dryRun) {
                auto beforeResult = songService_.readSongContent(songFile);
                if (beforeResult.value("ok", false))
                    beforeContent = beforeResult.value("content", std::string{});
            }

            nlohmann::json editOps = nlohmann::json::array();
            editOps.push_back(nlohmann::json{
                {"op", op.op},
                {"notation", op.params.value("notation", plan.targetNotation)},
                {"position", op.params.value("position", "replace")},
                {"confirm", true}
            });

            nlohmann::json result;
            if (currentRequest_.dryRun) {
                result = {{"ok", true}, {"cmd", "edit_song"},
                          {"dry_run", true}, {"message", "dry-run: song edit simulated"}};
            } else {
                result = songService_.editSong(songFile, editOps);
            }

            appliedOps.push_back({
                {"op", op.op},
                {"service", "SongMutationService"},
                {"result", result}
            });

            if (!result.value("ok", false)) {
                sr.ok = false;
                sr.message = "song mutation failed: " +
                             result.value("error", std::string("unknown error"));
                sr.data = {{"applied_ops", appliedOps}, {"details", details}};
                return sr;
            }

            // AI-10.3: Record in change-set.
            if (!currentRequest_.dryRun) {
                std::string afterContent;
                auto afterResult = songService_.readSongContent(songFile);
                if (afterResult.value("ok", false))
                    afterContent = afterResult.value("content", std::string{});

                auto snapshotBody = [](const std::string& content) -> std::string {
                    const auto parsed = hathor::ui::parseHathorFile(content);
                    if (const auto* hf = std::get_if<hathor::ui::HathorFile>(&parsed))
                        return hf->body;
                    return content;
                };

                ChangeSetOperation csOp;
                csOp.op = "edit_song";
                csOp.resourceId = "song:" + songFile;
                csOp.slotName = plan.slotName;
                csOp.songFile = songFile;
                csOp.originalContent = beforeContent;
                csOp.newContent = afterContent;
                csOp.before = nlohmann::json{{"body", snapshotBody(beforeContent)}};
                csOp.after = nlohmann::json{{"body", plan.targetNotation}};
                csOp.reversible = true;
                csOp.revertAction = "restore song '" + songFile + "' to pre-repair content";
                changeSetManager_.addOperation(std::move(csOp));
            }

        } else if (op.service == "ChuckSessionService") {
            // ChucK instrument repair via compileChuck + audition
            const std::string sessionId = op.params.value("session_id", std::string{});
            const std::string newSource = op.params.value("source", std::string{});

            if (sessionId.empty()) {
                sr.ok = false;
                sr.message = "no session_id for ChucK repair";
                sr.data = {{"applied_ops", appliedOps}, {"details", details}};
                return sr;
            }

            if (currentRequest_.dryRun) {
                appliedOps.push_back({
                    {"op", op.op},
                    {"service", "ChuckSessionService"},
                    {"session_id", sessionId},
                    {"dry_run", true}
                });
            } else {
                // Compile the repaired source.
                uint64_t jobId = 0;
                {
                    auto source = op.params.value("source", std::string{});
                    auto jobHandle = chuckService_.compileChuck(
                        sessionId, source,
                        [](hathor::CompileResult cr) {
                            (void)cr;
                        });
                    jobId = jobHandle.id();

                if (jobId == 0) {
                    sr.ok = false;
                    sr.message = "ChucK compile failed to start";
                    sr.data = {{"applied_ops", appliedOps}, {"details", details}};
                    return sr;
                }

                // Wait for compile to complete.
                nlohmann::json compileResult;
                if (!waitForAsyncJob(jobId, "chuck", compileResult)) {
                    sr.ok = false;
                    sr.message = "ChucK compile did not complete";
                    sr.data = {{"applied_ops", appliedOps}, {"details", details}};
                    return sr;
                }

                // Audition the repaired instrument.
                chuckService_.auditionSession(sessionId);

                appliedOps.push_back({
                    {"op", op.op},
                    {"service", "ChuckSessionService"},
                    {"session_id", sessionId},
                    {"job_id", jobId},
                    {"compile_result", compileResult}
                });
            }

            // AI-10.2: Update working set with the repaired session source.
            if (!currentRequest_.dryRun) {
                workingSet_.recordItem({
                    .id = sessionId,
                    .name = sessionId,
                    .type = WorkingSet::ItemType::Session,
                    .slotName = plan.slotName,
                    .state = {{"source", plan.targetSource}, {"repaired", true}},
                });
            }
        }
    } // end for each op

    // AI-10.5: Update runtime state for re-audition.
    // For pattern repairs, store the repaired notation on the slot so
    // stepAudition() can play it.  For ChucK repairs, ensure the session
    // is current.
    if (plan.targetDomain == CreativeRepairEngine::TargetDomain::Pattern &&
        !plan.slotName.empty()) {
        {
            std::lock_guard<std::mutex> lock(stateMtx_);
            generatedNotation_ = plan.targetNotation;
        }

        if (!currentRequest_.dryRun) {
            // Validate + lower the repaired notation and store on the slot.
            const auto parseResult = hathor::parseMini(plan.targetNotation);
            if (std::holds_alternative<hathor::CompiledPattern>(parseResult)) {
                const auto& compiled = std::get<hathor::CompiledPattern>(parseResult);
                const std::string canonical = hathor::printMini(compiled);
                auto paramPattern = hathor::lowerToParamMap(compiled.pattern);
                const std::size_t maxEvents = paramPattern.maxEventsPerCycle();

                int slotIdx = audio_.findOrAddSlot(plan.slotName);
                if (slotIdx >= 0) {
                    auto slotState = std::make_shared<SlotState>();
                    slotState->pattern = std::make_shared<hathor::Pattern<hathor::ParamMap>>(
                        std::move(paramPattern));
                    {
                        const hathor::Rational zero{0, 1};
                        const hathor::Arc zeroArc{zero, zero};
                        const hathor::Event<hathor::ParamMap> dummy{zeroArc, zeroArc, {}};
                        slotState->eventBuffer.assign(maxEvents, dummy);
                    }
                    slotState->notation = canonical;
                    audio_.storeSlot(slotIdx, std::move(slotState));
                }
            }
        }
    } else if (plan.targetDomain == CreativeRepairEngine::TargetDomain::Instrument) {
        {
            std::lock_guard<std::mutex> lock(stateMtx_);
            generatedCkSource_ = plan.targetSource;
            if (!plan.sessionId.empty())
                sessionId_ = plan.sessionId;
        }
    }

    sr.ok = true;
    sr.message = anyPersistent
        ? "creative repair applied (persistent mutation — requires confirmation)"
        : "creative repair applied (non-destructive, auditioned)";
    sr.data = {
        {"feedback", plan.feedback},
        {"property", hathor::control::propertyToString(plan.property)},
        {"target_domain", hathor::control::domainToString(plan.targetDomain)},
        {"slot_name", plan.slotName},
        {"target_notation", plan.targetNotation},
        {"target_source", plan.targetSource},
        {"needs_confirmation", plan.needsConfirmation},
        {"dry_run", currentRequest_.dryRun},
        {"applied_ops", appliedOps},
        {"explanation", plan.explanation}
    };

    return sr;
}

AgenticWorkflow::StepResult AgenticWorkflow::stepRender()
{
    StepResult sr;
    sr.stepName = "render";

    std::string sessionId;
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        sessionId = sessionId_;
    }

    if (sessionId.empty()) {
        sr.ok = false;
        sr.message = "no ChucK session to render";
        return sr;
    }

    if (currentRequest_.assetName.empty()) {
        sr.ok = false;
        sr.message = "render requires an asset_name";
        return sr;
    }

    // Start a background render via the canonical RenderService (AI-6).
    // renderChuck() is non-blocking — returns immediately with a job ID.
    // The render writes to a temp file (non-destructive); only commit
    // (which requires confirmation) touches the persistent project tree.
    uint64_t jobId = renderService_.renderChuck(
        sessionId,
        currentRequest_.durationBars,
        currentRequest_.assetName,
        hathor::AssetTarget::Studio);

    if (jobId == 0) {
        sr.ok = false;
        sr.message = "render failed to start (no job ID)";
        return sr;
    }

    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        renderJobId_ = jobId;
    }

    // AI-10.4: Render job submitted — surface it with its job identity.
    emitEvent(EventType::RenderStarted,
              "Rendering \"" + currentRequest_.assetName +
                  "\" to audio in the background",
              true, "render", {{"job_id", jobId}, {"asset_name", currentRequest_.assetName}},
              false, jobId, currentRequest_.assetName);

    // Wait for the render job to complete (with cancellation checking).
    nlohmann::json jobResult;

    if (!waitForAsyncJob(jobId, "render", jobResult)) {
        {
            std::lock_guard<std::mutex> lock(stateMtx_);
            renderStatus_ = jobResult;
            renderResult_ = jobResult;
        }

        sr.data = jobResult;
        const std::string status = jobResult.value("status", std::string("cancelled"));
        emitEvent(EventType::RenderCompleted,
                  status == "cancelled" ? "Render cancelled before completion"
                                        : "Render failed or timed out",
                  false, "render", jobResult, false, jobId, currentRequest_.assetName);
        if (status == "cancelled") {
            sr.ok = false;
            sr.message = "render cancelled";
        } else {
            sr.ok = false;
            sr.message = "render failed or timed out";
        }
        return sr;
    }

    // Check the final job status.
    const std::string finalStatus = jobResult.value("status", std::string("unknown"));

    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        renderStatus_ = jobResult;
        renderResult_ = jobResult;
    }

    sr.data = jobResult;

    if (finalStatus == "succeeded" || finalStatus == "completed") {
        sr.ok = true;
        sr.message = "render completed successfully";
        renderCompleted_ = true;
        emitEvent(EventType::RenderCompleted,
                  "Finished rendering \"" + currentRequest_.assetName + "\"",
                  true, "render", jobResult, false, jobId, currentRequest_.assetName);
    } else if (finalStatus == "cancelled") {
        sr.ok = false;
        sr.message = "render cancelled";
        emitEvent(EventType::RenderCompleted, "Render cancelled before completion",
                  false, "render", jobResult, false, jobId, currentRequest_.assetName);
    } else {
        sr.ok = false;
        sr.message = "render failed: " +
                     jobResult.value("error",
                                     jobResult.value("result", nlohmann::json{})
                                         .value("error", std::string("unknown error")));
    }

    return sr;
}

AgenticWorkflow::StepResult AgenticWorkflow::stepBindAsset()
{
    StepResult sr;
    sr.stepName = "bind_asset";

    uint64_t jobId;
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        jobId = renderJobId_;
    }

    if (jobId == 0) {
        sr.ok = false;
        sr.message = "no render job to commit";
        return sr;
    }

    if (!renderCompleted_) {
        sr.ok = false;
        sr.message = "render not yet complete";
        return sr;
    }

    // Commit the rendered asset via the canonical RenderService (AI-6).
    // commitRenderedAsset() performs collision detection and requires
    // explicit confirmation for overwrites. The confirmation was already
    // handled by the workflow's waitForConfirmation() call before reaching
    // this step.
    nlohmann::json result = renderService_.commitRenderedAsset(
        jobId,
        currentRequest_.assetName,
        true /* confirmOverwrite — handled by the workflow's own confirmation */);

    sr.data = result;

    if (result.value("ok", false)) {
        sr.ok = true;
        sr.message = "asset committed: " + result.value("asset_name", std::string(""));

        // AI-10.4: The rendered asset is now committed to the project.
        emitEvent(EventType::AssetCommitted,
                  "Baked \"" + currentRequest_.assetName +
                      "\" into the project as an instrument",
                  true, "bind_asset", result, false, jobId,
                  "instrument:" + currentRequest_.assetName);

        // Verify the asset is bound to the SampleBank.
        const auto assetName = result.value("asset_name", std::string{});
        if (!assetName.empty()) {
            auto* entry = bank_.find(assetName, 0);
            sr.data["bound_to_sample_bank"] = (entry != nullptr);

            // AI-10.3: record this persistent mutation in the change-set.
            // Determine whether the asset existed before this commit.
            const auto projectDir = audio_.currentProjectDir();
            hathor::AssetPathResolver resolver(projectDir);
            auto wavResolve = resolver.resolveStudio(
                hathor::sanitizeAssetName(assetName));
            auto ckPath = wavResolve.path;
            ckPath.replace_extension(".ck");
            std::error_code ec;
            const bool existed = std::filesystem::exists(wavResolve.path, ec)
                || std::filesystem::exists(ckPath, ec);

            ChangeSetOperation csOp;
            csOp.op = "commit_rendered_asset";
            csOp.resourceId = "instrument:" + assetName;
            csOp.slotName = currentRequest_.targetSlot;
            csOp.assetName = assetName;
            csOp.assetExistedBefore = existed;
            csOp.before = nlohmann::json{{"existed", existed}};
            csOp.after = result;
            csOp.reversible = true;
            csOp.revertAction = "remove baked asset '" + assetName + "'";
            changeSetManager_.addOperation(std::move(csOp));
        }
    } else {
        sr.ok = false;
        sr.message = result.value("error", std::string("commit failed"));
    }

    return sr;
}

AgenticWorkflow::StepResult AgenticWorkflow::stepUpdateSong()
{
    StepResult sr;
    sr.stepName = "update_song";

    const bool isChuckWorkflow = !currentRequest_.ckSource.empty();

    // Use the canonical structured mutation service (AI-7).
    // The mutation is transactional (all-or-nothing) and validated.
    // We do NOT write to the filesystem directly — the SongMutationService
    // handles atomic writes with rollback (AI-7/AI-10 requirement).

    nlohmann::json ops = nlohmann::json::array();

    if (isChuckWorkflow) {
        // For ChucK workflows, the song references the baked instrument.
        // We update the song file to use `s "assetName"` for the target slot.
        std::string notation;
        if (currentRequest_.targetSlot.empty()) {
            sr.ok = false;
            sr.message = "update_song requires target_slot";
            return sr;
        }
        notation = "s \"" + currentRequest_.assetName + "\"";

        ops.push_back({
            {"op", "replace_pattern"},
            {"slot", currentRequest_.targetSlot},
            {"notation", notation},
            {"confirm", true}
        });
    } else {
        // For pattern workflows, replace the slot's pattern with the generated notation.
        if (currentRequest_.targetSlot.empty()) {
            sr.ok = false;
            sr.message = "update_song requires target_slot for pattern workflow";
            return sr;
        }

        std::string notation;
        {
            std::lock_guard<std::mutex> lock(stateMtx_);
            notation = generatedNotation_;
        }

        if (notation.empty())
            notation = currentRequest_.notation;

        // Write to a .hathor file named after the slot.
        const std::string songFile = currentRequest_.targetSlot + ".hathor";

        ops.push_back({
            {"op", "replace_pattern"},
            {"slot", currentRequest_.targetSlot},
            {"notation", notation},
            {"confirm", true}
        });
    }

    // In dry-run mode, simulate the mutation without writing.
    if (currentRequest_.dryRun) {
        sr.ok = true;
        sr.message = "dry-run: song update simulated (no file written)";
        sr.data["dry_run"] = true;
        sr.data["operations"] = ops;
        sr.data["target_slot"] = currentRequest_.targetSlot;
        return sr;
    }

    // Determine the song file to edit.
    const std::string songFile = currentRequest_.targetSlot + ".hathor";

    // AI-10.3: capture the before-content for the change-set (canonical read).
    std::string beforeContent;
    {
        auto beforeResult = songService_.readSongContent(songFile);
        if (beforeResult.value("ok", false))
            beforeContent = beforeResult.value("content", std::string{});
    }

    // Apply the mutation through the canonical SongMutationService (AI-7).
    nlohmann::json result = songService_.editSong(songFile, ops);

    sr.data = result;

    if (result.value("ok", false)) {
        sr.ok = true;
        sr.message = "song updated successfully";
        sr.data["song"] = songFile;

        // AI-10.4: The song file mutation has been applied.
        emitEvent(EventType::SongMutationApplied,
                  "Wrote the new " + currentRequest_.targetSlot +
                      " pattern into the song file",
                  true, "update_song", {{"song", songFile}, {"result", result}});

        // AI-10.3: capture the after-content and record the change-set op.
        std::string afterContent;
        {
            auto afterResult = songService_.readSongContent(songFile);
            if (afterResult.value("ok", false))
                afterContent = afterResult.value("content", std::string{});
        }

        // Build a structured before/after snapshot for the human diff by
        // parsing the captured file content (pre- and post-mutation).
        auto snapshotOf = [](const std::string& content) {
            nlohmann::json snap = nlohmann::json{
                {"bpm", nullptr}, {"label", nullptr}, {"color", nullptr},
                {"slot", nullptr}, {"bank", nullptr}, {"body", ""}
            };
            const auto parsed = hathor::ui::parseHathorFile(content);
            if (const auto* hf = std::get_if<hathor::ui::HathorFile>(&parsed)) {
                if (hf->front.bpm)   snap["bpm"]   = *hf->front.bpm;
                if (hf->front.label) snap["label"] = *hf->front.label;
                if (hf->front.color) snap["color"] = *hf->front.color;
                if (hf->front.slot)  snap["slot"]  = *hf->front.slot;
                if (hf->front.bank)  snap["bank"]  = *hf->front.bank;
                snap["body"] = hf->body;
            } else {
                snap["body"] = content;
            }
            return snap;
        };

        ChangeSetOperation csOp;
        csOp.op = "edit_song";
        csOp.resourceId = "song:" + songFile;
        csOp.slotName = currentRequest_.targetSlot;
        csOp.songFile = songFile;
        csOp.originalContent = beforeContent;
        csOp.newContent = afterContent;
        csOp.before = snapshotOf(beforeContent);
        csOp.after = snapshotOf(afterContent);
        csOp.reversible = true;
        csOp.revertAction = "restore song '" + songFile + "' to pre-change content";
        changeSetManager_.addOperation(std::move(csOp));
    } else {
        sr.ok = false;
        sr.message = result.value("error", std::string("song mutation failed"));
    }

    return sr;
}

// ---------------------------------------------------------------------------
// Orchestration helpers
// ---------------------------------------------------------------------------

void AgenticWorkflow::setState(State s)
{
    // AI-10.4: A requested cancellation wins over a coincident failure so the
    // UI reports "cancelled", not "failed", when the user stopped the work.
    // This matters for async steps (compile/render) where the underlying job
    // is cancelled mid-flight and surfaces as a step failure.
    if (s == State::Failed && stopRequested_.load(std::memory_order_acquire))
        s = State::Cancelled;

    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        state_ = s;
    }
    // AI-10.3: A failed or cancelled workflow never presents its (possibly
    // partial) change-set as accepted.  Mark any pending change-set cancelled
    // so the composer cannot mistake a failed run for a clean, reviewable one.
    if (s == State::Failed || s == State::Cancelled)
        changeSetManager_.cancelCurrent();
    auditLog("state_change", "workflow", true, stateName(s));

    // AI-10.4: Emit a typed event for terminal states.
    switch (s) {
        case State::Completed: {
            const int steps = static_cast<int>(completedSteps_.size());
            emitEvent(EventType::WorkflowCompleted,
                      "Workflow complete — applied " +
                          std::to_string(appliedChanges_.size()) + " change(s) across " +
                          std::to_string(steps) + " step(s)",
                      true, {}, {{"completed_steps", completedSteps_}});
            break;
        }
        case State::Cancelled: {
            emitEvent(EventType::WorkflowCancelled,
                      "Workflow cancelled — background work has stopped",
                      false, {}, {});
            break;
        }
        case State::Failed: {
            std::string step;
            std::string err;
            {
                std::lock_guard<std::mutex> lock(stateMtx_);
                step = stepName(currentStep_);
                if (error_.has_value())
                    err = *error_;
            }
            // Indicate whether the agent would attempt repair: only when the
            // failure is repairable and repair budget remains.
            const bool canRepair = (repairAttempts_ < kMaxRepairAttempts)
                                   && step != "bind_asset" && step != "update_song"
                                   && step != "inspect_project" && step != "inspect_song"
                                   && step != "inspect_assets";
            emitEvent(EventType::StepFailed,
                      err.empty() ? ("Step '" + step + "' failed") : ("Could not " + step +
                          " — " + err),
                      false, step, {}, canRepair);
            break;
        }
        default:
            break;  // Non-terminal state — no standalone event.
    }
}

void AgenticWorkflow::setCurrentStep(Step s)
{
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        currentStep_ = s;
        currentStepResult_.clear();
    }
    emitEvent(EventType::StepStarted, stepExplain(s), true, stepName(s));
}

void AgenticWorkflow::completeStep(const std::string& name, const StepResult& result)
{
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        completedSteps_.push_back(name);
        currentStepResult_ = nlohmann::json{
            {"step", name},
            {"ok", result.ok},
            {"message", result.message},
            {"data", result.data}
        };
    }
    auditLog("step_complete", name, result.ok, result.message);

    // AI-10.2: Update the conversational working set after every completed step.
    // The working set tracks items, changes, and aliases for multi-turn
    // reference resolution ("make it darker", "revert that", etc.).
    // Only successful steps contribute state; failures leave the working
    // set unchanged (testing requirement #8).
    workingSet_.updateAfterStep(name, result.data, result.ok);

    // AI-10.4: Emit the outcome for this step.
    if (result.ok)
        emitEvent(EventType::StepCompleted, result.message, true, name, result.data);
    else
        emitEvent(EventType::StepFailed, result.message, false, name, result.data);
}

bool AgenticWorkflow::checkCancellation()
{
    return stopRequested_.load(std::memory_order_acquire);
}

void AgenticWorkflow::emitProgress()
{
    if (!progressCallback_)
        return;

    // Legacy fallback: emit a generic progress event carrying the state snapshot.
    ProgressEvent ev;
    ev.workflowId = workflowId_;
    ev.type = EventType::StepProgress;
    ev.step = currentStep_;
    ev.stepName = stepName(currentStep_);
    ev.ok = true;
    ev.message = "…";
    ev.state = getState();
    progressCallback_(ev);
}

void AgenticWorkflow::emitEvent(EventType type,
                                std::string message,
                                bool ok,
                                std::string stepNameIn,
                                nlohmann::json details,
                                bool repairPlanned,
                                uint64_t jobId,
                                std::string resource)
{
    if (!progressCallback_)
        return;

    ProgressEvent ev;
    ev.workflowId = workflowId_;
    ev.type = type;
    ev.step = currentStep_;
    ev.stepName = stepNameIn.empty() ? stepName(currentStep_) : std::move(stepNameIn);
    ev.message = std::move(message);
    ev.ok = ok;
    ev.repairPlanned = repairPlanned;
    ev.jobId = jobId;
    ev.resource = std::move(resource);
    ev.details = std::move(details);
    ev.state = getState();

    auditLog(eventTypeName(type), ev.stepName, ok, ev.message);
    progressCallback_(ev);
}

bool AgenticWorkflow::waitForConfirmation(const std::string& action,
                                          const std::string& capabilityClass,
                                          const std::string& description,
                                          nlohmann::json details)
{
    const int reqId = nextConfirmationId_.fetch_add(1, std::memory_order_acq_rel);

    {
        std::lock_guard<std::mutex> lock(confirmMtx_);
        pendingConfirmation_ = ConfirmationRequest{
            reqId, action, description, details, capabilityClass
        };
    }

    // Emit the confirmation request via callback.
    if (confirmationCallback_) {
        ConfirmationRequest req{
            reqId, action, description, details, capabilityClass
        };
        confirmationCallback_(req);
    }

    // Update workflow state.
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        state_ = State::WaitingForApproval;
    }
    // AI-10.4: Surface the authorization boundary as an observable event.
    emitEvent(EventType::ConfirmationRequired,
              description,
              true, action, details);

    // Wait for the response (with a generous timeout).
    constexpr auto kTimeout = std::chrono::minutes(5);
    auto deadline = std::chrono::steady_clock::now() + kTimeout;

    {
        std::unique_lock<std::mutex> lock(confirmMtx_);
        while (!confirmationResponded_.load(std::memory_order_acquire)) {
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                // Also check stopRequested.
                if (stopRequested_.load(std::memory_order_acquire)) {
                    {
                        std::lock_guard<std::mutex> l2(stateMtx_);
                        pendingConfirmation_.reset();
                    }
                    confirmationResponded_.store(false, std::memory_order_release);
                    setState(State::Cancelled);
                    return false;
                }
                // Timeout waiting for confirmation.
                {
                    std::lock_guard<std::mutex> l2(stateMtx_);
                    pendingConfirmation_.reset();
                    error_ = "confirmation timed out";
                }
                confirmationResponded_.store(false, std::memory_order_release);
                setState(State::Failed);
                return false;
            }
        }
    }

    // Clean up pending confirmation.
    {
        std::lock_guard<std::mutex> lock(confirmMtx_);
        pendingConfirmation_.reset();
    }

    const bool approved = confirmationApproved_.load(std::memory_order_acquire);
    confirmationResponded_.store(false, std::memory_order_release);

    auditLog("confirmation", action, approved, "");
    return approved;
}

// ---------------------------------------------------------------------------
// waitForAsyncJob
// ---------------------------------------------------------------------------

bool AgenticWorkflow::waitForAsyncJob(uint64_t jobTrackerId,
                                      const std::string& serviceType,
                                      nlohmann::json& result)
{
    constexpr auto kPollInterval = std::chrono::milliseconds(50);
    constexpr auto kMaxWait      = std::chrono::seconds(60);
    auto deadline = std::chrono::steady_clock::now() + kMaxWait;

    while (std::chrono::steady_clock::now() < deadline) {
        if (stopRequested_.load(std::memory_order_acquire)) {
            if (serviceType == "chuck") {
                chuckService_.cancelJob(jobTrackerId);
            } else if (serviceType == "render") {
                renderService_.cancelJob(jobTrackerId);
            }
            result = {{"job_id", jobTrackerId}, {"status", "cancelled"}};
            auditLog("async_job_cancel", serviceType, true, "job_id=" + std::to_string(jobTrackerId));
            return false;
        }

        if (serviceType == "chuck") {
            result = chuckService_.getJobStatus(jobTrackerId);
        } else if (serviceType == "render") {
            result = renderService_.getJobStatus(jobTrackerId);
        } else {
            result = {{"job_id", jobTrackerId}, {"status", "unknown"},
                      {"error", "unknown service type: " + serviceType}};
            return false;
        }

        const std::string status = result.value("status", std::string("unknown"));
        if (status == "succeeded" || status == "failed" ||
            status == "cancelled" || status == "completed") {
            return status == "succeeded" || status == "completed";
        }

        // AI-10.4: Surface meaningful progress while the async job runs so the
        // user sees it is still actively working (not hung).
        if (serviceType == "chuck")
            emitEvent(EventType::StepProgress,
                      "Still compiling — waiting on the instrument to build",
                      true, "compile", result, false, jobTrackerId);
        else if (serviceType == "render")
            emitEvent(EventType::StepProgress,
                      "Still rendering audio in the background",
                      true, "render", result, false, jobTrackerId);

        std::this_thread::sleep_for(kPollInterval);
    }

    // Timeout.
    result = {{"job_id", jobTrackerId}, {"status", "timeout"},
              {"error", "async job timed out after " +
                         std::to_string(kMaxWait.count()) + "s"}};
    auditLog("async_job_timeout", serviceType, false, "job_id=" + std::to_string(jobTrackerId));
    return false;
}

// ---------------------------------------------------------------------------
// AI-10.2: Conversational working set access
// ---------------------------------------------------------------------------

nlohmann::json AgenticWorkflow::getWorkingSet() const
{
    // AI-10.2: Return a thread-safe snapshot of the conversational working set.
    // The working set is owned by the workflow but is a separate object with
    // its own mutex, so we can safely call it without holding stateMtx_.
    nlohmann::json j = workingSet_.toJson();
    j["cmd"] = "working_set";
    return j;
}

nlohmann::json AgenticWorkflow::resolveReference(
    std::string_view phrase,
    std::string_view intentContext) const
{
    WorkingSet::ResolveResult resolved = workingSet_.resolveReference(phrase, intentContext);

    nlohmann::json j;
    j["cmd"] = "resolve_reference";
    j["ok"] = resolved.found;
    j["ambiguous"] = resolved.ambiguous;

    if (resolved.found && !resolved.ambiguous) {
        j["resolved"] = resolved.resolved;
    }

    if (resolved.ambiguous) {
        j["candidates"] = resolved.candidates;
        j["error"] = resolved.errorMessage;
    }

    if (!resolved.found && !resolved.ambiguous) {
        j["error"] = resolved.errorMessage.empty()
            ? "no matching item in working set"
            : resolved.errorMessage;
    }

    return j;
}

nlohmann::json AgenticWorkflow::getRevertInfo() const
{
    nlohmann::json j = workingSet_.getRevertInfo();
    j["cmd"] = "revert_info";
    return j;
}

void AgenticWorkflow::clearWorkingSet()
{
    std::lock_guard<std::mutex> lock(stateMtx_);
    workingSet_.clear();
    auditLog("working_set_cleared", "agentic", true, "session-scoped state cleared");
}

void AgenticWorkflow::reconcileWorkingSet(const nlohmann::json& projectState)
{
    workingSet_.reconcile(projectState);
}

// ---------------------------------------------------------------------------
// AI-10.3: First-class diff / preview / undo for AI changes
// ---------------------------------------------------------------------------

nlohmann::json AgenticWorkflow::getChangeSet() const
{
    nlohmann::json j = changeSetManager_.toJsonActive();
    if (!j.value("ok", false))
        return j;  // {ok:false, error:"no change-set"}
    j["cmd"] = "changeset_status";
    return j;
}

nlohmann::json AgenticWorkflow::previewChangeSet() const
{
    nlohmann::json j = changeSetManager_.previewCurrent();
    return j;
}

nlohmann::json AgenticWorkflow::acceptChangeSet()
{
    // A change-set may only be accepted when the workflow actually completed.
    // A failed/cancelled run is never presented as accepted (test #10).
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        if (state_ != State::Completed) {
            return {
                {"ok", false},
                {"cmd", "changeset_accept"},
                {"error", "workflow has not completed; cannot accept change-set"}
            };
        }
    }

    if (!changeSetManager_.hasPending()) {
        return {
            {"ok", false},
            {"cmd", "changeset_accept"},
            {"error", "no pending change-set to accept"}
        };
    }

    const int id = changeSetManager_.currentChangeSetId();
    if (!changeSetManager_.acceptCurrent()) {
        return {
            {"ok", false},
            {"cmd", "changeset_accept"},
            {"error", "failed to accept change-set"}
        };
    }

    auditLog("changeset_accept", "agentic", true,
             "change_set_id=" + std::to_string(id));
    return {
        {"ok", true},
        {"cmd", "changeset_accept"},
        {"change_set_id", id},
        {"status", "accepted"}
    };
}

nlohmann::json AgenticWorkflow::rejectChangeSet(bool confirm)
{
    auto plan = changeSetManager_.rejectCurrent();
    if (!plan) {
        return {
            {"ok", false},
            {"cmd", "changeset_reject"},
            {"error", "no reversible pending change-set to reject"}
        };
    }

    const int id = changeSetManager_.currentChangeSetId();

    // Preview what would be reverted.  A preview does not grant authorization.
    if (!confirm) {
        return {
            {"ok", false},
            {"cmd", "changeset_reject"},
            {"requires_confirmation", true},
            {"change_set_id", id},
            {"preview", revertPlanToJson(*plan)},
            {"message", "rejecting reverts the ENTIRE change-set; pass confirm=true to authorize"}
        };
    }

    nlohmann::json executed = executeRevertPlan(*plan, confirm);

    if (executed.value("ok", false))
        changeSetManager_.markRejected();

    return {
        {"ok", executed.value("ok", false)},
        {"cmd", "changeset_reject"},
        {"change_set_id", id},
        {"status", executed.value("ok", false) ? "rejected" : "reject_failed"},
        {"executed", executed.value("executed", nlohmann::json::array())},
        {"error", executed.value("error", nlohmann::json(nullptr))}
    };
}

nlohmann::json AgenticWorkflow::undoChangeSet(int changeSetId, bool confirm)
{
    auto plan = changeSetManager_.undoAccepted(changeSetId);
    if (!plan) {
        return {
            {"ok", false},
            {"cmd", "changeset_undo"},
            {"error", "no accepted, reversible change-set with that id"}
        };
    }

    if (!confirm) {
        return {
            {"ok", false},
            {"cmd", "changeset_undo"},
            {"requires_confirmation", true},
            {"change_set_id", changeSetId},
            {"preview", revertPlanToJson(*plan)},
            {"message", "undoing reverts the ENTIRE change-set; pass confirm=true to authorize"}
        };
    }

    nlohmann::json executed = executeRevertPlan(*plan, confirm);

    if (executed.value("ok", false))
        changeSetManager_.markUndone();

    return {
        {"ok", executed.value("ok", false)},
        {"cmd", "changeset_undo"},
        {"change_set_id", changeSetId},
        {"status", executed.value("ok", false) ? "undone" : "undo_failed"},
        {"executed", executed.value("executed", nlohmann::json::array())},
        {"error", executed.value("error", nlohmann::json(nullptr))}
    };
}

nlohmann::json AgenticWorkflow::executeRevertPlan(
    const std::vector<ChangeSetManager::RevertAction>& plan,
    bool confirm)
{
    nlohmann::json executed = nlohmann::json::array();
    nlohmann::json failure;

    for (const auto& action : plan) {
        // Destructive actions require authorization (AI-1).  The caller must
        // have passed confirm=true — a preview never grants authorization.
        if (action.destructive && !confirm) {
            failure = {
                {"action", action.kind},
                {"resource_id", action.resourceId},
                {"error", "destructive revert requires confirmation"}
            };
            break;
        }

        if (action.kind == "restore_song") {
            nlohmann::json r = songService_.restoreSongFile(
                action.songFile, action.content);
            r["action"] = "restore_song";
            r["resource_id"] = action.resourceId;
            executed.push_back(std::move(r));
            if (!r.value("ok", false)) {
                failure = r;
                break;
            }
        } else if (action.kind == "remove_asset") {
            nlohmann::json r = renderService_.removeRenderedAsset(
                action.assetName);
            r["action"] = "remove_asset";
            r["resource_id"] = action.resourceId;
            executed.push_back(std::move(r));
            if (!r.value("ok", false)) {
                failure = r;
                break;
            }
        } else {
            failure = {
                {"action", action.kind},
                {"resource_id", action.resourceId},
                {"error", "unknown revert action"}
            };
            break;
        }
    }

    if (!failure.is_null())
        return {{"ok", false}, {"executed", executed}, {"error", failure}};
    return {{"ok", true}, {"executed", executed}};
}

} // namespace hathor::control
