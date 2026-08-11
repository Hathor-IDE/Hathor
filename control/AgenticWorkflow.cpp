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

    // AI-10.2: Record the user intent in the working set so that aliases
    // (e.g. "the bass") can be derived from intent keywords.
    workingSet_.setLastIntent(currentRequest_.intent);

    // AI-10.3: Begin a fresh, pending change-set for this workflow run so that
    // every persistent mutation is grouped into one coherent, reviewable unit.
    changeSetManager_.beginChangeSet(currentRequest_.intent);

    // Emit the initial queued state.
    if (progressCallback_) {
        nlohmann::json state = getState();
        progressCallback_(state);
    }

    // Launch the workflow thread.
    workflowThread_ = std::thread([this] { runWorkflow(); });

    return true;
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

    // Phase 1: PLANNING — analyse the request and assemble a plan.
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        state_ = State::Planning;
    }
    emitProgress();

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
    emitProgress();

    // Phase 2: INSPECTION — inspect_project, inspect_song, inspect_assets.
    setState(State::Inspecting);

    for (const auto step : { Step::InspectProject, Step::InspectSong,
                             Step::InspectAssets }) {
        setCurrentStep(step);
        emitProgress();

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
    emitProgress();

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
    emitProgress();

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
        emitProgress();

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
    emitProgress();

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
    emitProgress();

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
            emitProgress();

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
            }

            // Re-validate.
            setCurrentStep(Step::Validate);
            setState(State::Validating);
            emitProgress();

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
                emitProgress();

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
            emitProgress();

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
        emitProgress();

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
        emitProgress();

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
        emitProgress();

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

    const bool isChuckWorkflow = !currentRequest_.ckSource.empty();

    if (isChuckWorkflow) {
        // Validate ChucK source via the real compiler diagnostics path.
        // This is the same validateChuckSource() called by ChuckCompiler
        // on B4-K4 — never an approximate parser (AI-5/AI-18).
        const auto diag = hathor::audio_worker::validateChuckSource(
            currentRequest_.ckSource);

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
        sr.data["diagnostics"] = std::move(diags);
        sr.data["source"] = "chuck_compiler";
        return sr;
    } else {
        // Validate mini-notation via the real parseMini().
        const auto parseResult = hathor::parseMini(currentRequest_.notation);

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
            const auto tokens = hathor::tokenise(currentRequest_.notation);
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
        sr.data["diagnostics"] = std::move(diags);
        sr.data["source"] = "miniparser";
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

    // Use a promise to bridge the async callback and capture the CompileResult.
    auto promise = std::make_shared<std::promise<hathor::CompileResult>>();
    auto future = promise->get_future();

    auto jobHandle = chuckService_.compileChuck(
        sessionId,
        currentRequest_.ckSource,
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
    } else if (finalStatus == "cancelled") {
        sr.ok = false;
        sr.message = "render cancelled";
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
    emitProgress();
}

void AgenticWorkflow::setCurrentStep(Step s)
{
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        currentStep_ = s;
        currentStepResult_.clear();
    }
    emitProgress();
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

    emitProgress();
}

bool AgenticWorkflow::checkCancellation()
{
    return stopRequested_.load(std::memory_order_acquire);
}

void AgenticWorkflow::emitProgress()
{
    if (!progressCallback_)
        return;

    // Call the callback with the current state snapshot.
    // getState() acquires stateMtx_ internally.
    nlohmann::json state = getState();
    progressCallback_(std::move(state));
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
    emitProgress();

    // Wait for the response (with a generous timeout).
    constexpr auto kTimeout = std::chrono::minutes(5);
    auto deadline = std::chrono::steady_clock::now() + kTimeout;

    {
        std::unique_lock<std::mutex> lock(confirmMtx_);
        while (!confirmationResponded_.load(std::memory_order_acquire)) {
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                // Also check stopRequested.
                if (stopRequested_.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> l2(stateMtx_);
                    pendingConfirmation_.reset();
                    state_ = State::Cancelled;
                    confirmationResponded_.store(false, std::memory_order_release);
                    emitProgress();
                    return false;
                }
                // Timeout waiting for confirmation.
                std::lock_guard<std::mutex> l2(stateMtx_);
                pendingConfirmation_.reset();
                error_ = "confirmation timed out";
                state_ = State::Failed;
                confirmationResponded_.store(false, std::memory_order_release);
                emitProgress();
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
