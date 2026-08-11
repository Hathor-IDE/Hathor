// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * IntentPlanner.hpp — AI-10.1: Natural-Language Intent → Actionable Plan.
 *
 * Transforms a high-level natural-language request ("make me a dark 8-bar
 * acid bassline") into a structured, inspectable, executable Plan by consulting
 * the canonical read-only services (ProjectReadFacade, ChuckSessionService,
 * RenderService).  The plan is shown before any heavy or destructive step runs.
 *
 * Key design principles (AI-10.1, PROGRAM.md Phase K):
 *   - Reuse-first: existing instruments/samples are discovered and reported
 *     *before* any creation step is added to the plan.
 *   - Capability tagging: every plan step carries its AI-1 capability class
 *     ("read_only", "non_destructive", "persistent_mutation") so the executor
 *     knows which steps auto-run and which require confirmation.
 *   - No mutation: IntentPlanner is purely read-only; it never writes files,
 *     creates sessions, or renders audio.
 *   - Inspectable: the entire plan serializes to JSON for the UI / chat layer.
 *
 * Architecture boundary (AI-10):
 *
 *   AgenticWorkflow.runWorkflow()  (Planning state)
 *         ↓
 *   IntentPlanner::planFromRequest()  ← this layer
 *         ↓
 *   ProjectReadFacade       (AI-2: read-only inspection)
 *   ChuckSessionService     (AI-5: session state — read-only queries)
 *   RenderService           (AI-6: job status — read-only queries)
 *
 * Requirement references: AI-10.1, AI-1 capability model, PROGRAM.md §1392
 */

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace hathor::control {

// Forward declarations — full headers only needed in the .cpp.
class ProjectReadFacade;
class ChuckSessionService;
class RenderService;

/**
 * CapabilityClass — the AI-1 capability classification for a plan step.
 *
 * Mirrors the AI-1 capability model (PROGRAM.md §1037–§1053):
 *   - Read-only: safe, no mutation, auto-executes.
 *   - Non-destructive execution: mutates runtime/audio state only, never
 *     persistent files; auto-executes.
 *   - Persistent mutation: writes to .hathor files, creates/overwrites
 *     assets; crosses the confirmation boundary — does NOT auto-execute.
 */
enum class CapabilityClass {
    ReadOnly,          ///< inspect_project, inspect_song, get_diagnostics, etc.
    NonDestructive,    ///< compile_chuck, audition_chuck, play, stop
    PersistentMutation, ///< edit_song, commit_rendered_asset, create instrument
};

inline const char* toString(CapabilityClass c) noexcept
{
    switch (c) {
        case CapabilityClass::ReadOnly:           return "read_only";
        case CapabilityClass::NonDestructive:   return "non_destructive";
        case CapabilityClass::PersistentMutation: return "persistent_mutation";
    }
    return "unknown";
}

/**
 * PlanStep — one actionable step in the plan.
 *
 * Each step references a canonical service method that the AgenticWorkflow
 * will call.  The `capabilityClass` field tells the executor whether the step
 * can run automatically or must pause for user confirmation.
 */
struct PlanStep {
    std::string  name;            ///< canonical step name (e.g. "inspect_project")
    std::string  service;         ///< canonical service (e.g. "ProjectReadFacade")
    std::string  method;          ///< method to invoke (e.g. "inspectProject")
    CapabilityClass capabilityClass = CapabilityClass::ReadOnly;
    bool         requiresConfirmation = false; ///< true if persistent_mutation
    nlohmann::json params;       ///< step-specific parameters
    std::string  description;     ///< human-readable explanation of what/why

    nlohmann::json toJson() const;
};

/**
 * ReuseFinding — a discovered existing asset that might be reused.
 */
struct ReuseFinding {
    std::string  resource_id;    ///< e.g. "instrument:acid_bass"
    std::string  name;           ///< human-readable name
    std::string  type;           ///< "chuck_instrument" | "sample" | "pattern_slot"
    std::string  lifecycle_state; ///< e.g. "compiled", "rendered", "bound"
    nlohmann::json details;     ///< source_path, rendered_path, duration, etc.

    nlohmann::json toJson() const;
};

/**
 * ReuseDecision — the outcome of checking existing assets before planning.
 */
enum class ReuseDecision {
    Reuse,    ///< existing asset is suitable, no new creation needed
    Modify,   ///< existing asset exists but needs adaptation
    Create,   ///< no suitable existing asset, must create new one
};

inline const char* toString(ReuseDecision d) noexcept
{
    switch (d) {
        case ReuseDecision::Reuse:   return "reuse";
        case ReuseDecision::Modify:  return "modify";
        case ReuseDecision::Create:  return "create";
    }
    return "unknown";
}

/**
 * PlanModel — a complete, inspectable plan produced by IntentPlanner.
 *
 * Contains:
 *   - mode: "pattern" (mini-notation) or "chuck" (ChucK instrument)
 *   - intent: the original natural-language request
 *   - reuse: discovered existing assets + the reuse decision
 *   - steps: ordered list of actionable steps with capability tags
 *   - parameters: target slot, asset name, duration, etc.
 *   - is_dry_run: whether this plan avoids all persistent mutations
 *   - notation: generated mini-notation (pattern mode only)
 *   - ckSource: generated ChucK source (chuck mode only)
 */
struct PlanModel {
    std::string  mode;              ///< "pattern" or "chuck"
    std::string  intent;            ///< original natural-language intent
    std::string  targetSlot;        ///< e.g. "d1"
    std::string  assetName;         ///< e.g. "acid_bass" (if applicable)
    int          durationBars = 8;  ///< render/audition duration
    bool         isDryRun = false;  ///< skip all persistent mutations
    std::string  notation;          ///< generated mini-notation (pattern mode)
    std::string  ckSource;          ///< generated ChucK source (chuck mode)
    ReuseDecision reuseDecision = ReuseDecision::Create;
    std::vector<ReuseFinding> reuseFindings;
    std::vector<PlanStep> steps;

    nlohmann::json toJson() const;
};

/**
 * IntentPlanner — produces executable plans from natural-language intents.
 *
 * Constructed with references to the canonical read-only services.  The
 * planner is stateless with respect to the project — it queries the real
 * services on every call and does not maintain shadowing caches.
 *
 * Thread safety: planFromRequest() is safe to call from the workflow thread
 * (it only reads via the locked service accessors).  It is NOT safe to call
 * concurrently from multiple threads on the same instance, but the
 * AgenticWorkflow only calls it from its single orchestration thread.
 */
class IntentPlanner {
public:
    /**
     * Construct the planner with references to the canonical services.
     *
     * @param readFacade     ProjectReadFacade — AI-2 read-only inspection.
     * @param chuckService   ChuckSessionService — AI-5 session state (read).
     * @param renderService  RenderService — AI-6 job status (read).
     */
    IntentPlanner(ProjectReadFacade&    readFacade,
                  ChuckSessionService&  chuckService,
                  RenderService&        renderService) noexcept;

    ~IntentPlanner() = default;
    IntentPlanner(const IntentPlanner&)            = delete;
    IntentPlanner& operator=(const IntentPlanner&) = delete;

    /**
     * Inspect the project state and produce a structured, executable plan
     * from the given intent + request parameters.
     *
     * This method is purely read-only — it queries ProjectReadFacade to
     * understand what instruments, samples, and slots already exist, then
     * assembles a step sequence where:
     *   - Read-only steps are marked auto-execute
     *   - Persistent-mutation steps are marked requiresConfirmation
     *   - Reuse is decided before any creation step appears
     *
     * @param intent      Natural-language request (e.g. "dark 8-bar bassline").
     * @param targetSlot  Target slot name (e.g. "d1").
     * @param assetName   Asset name for ChUcK instrument rendering (may be empty).
     * @param durationBars Duration in bars for audition/render.
     * @param dryRun      If true, mark all persistent-mutation steps as
     *                    skipped (plan them but tag them as dry_run).
     * @return A fully populated PlanModel.
     */
    PlanModel planFromRequest(std::string_view intent,
                              std::string_view targetSlot,
                              std::string_view assetName,
                              int              durationBars,
                              bool             dryRun);

    /**
     * If the request already carries a pre-determined plan (Request::plan is
     * non-null), validate that it is well-formed and return it as a PlanModel.
     * Otherwise, fall through to planFromRequest().
     */
    PlanModel planFromRequestWithOverride(
        std::string_view intent,
        std::string_view targetSlot,
        std::string_view assetName,
        int              durationBars,
        bool             dryRun,
        const nlohmann::json& overridePlan);

private:
    /**
     * Discover existing assets that could be reused for the given intent.
     *
     * Queries inspectProject(), listChuckInstruments(), listSamples(), and
     * the current song's active patterns to build a reuse candidate list.
     */
    std::vector<ReuseFinding> discoverReuseCandidates(
        const std::string& intentKeywords,
        const std::string& assetName);

    /**
     * Decide whether to reuse, modify, or create based on findings.
     *
     * Heuristic: if an instrument/sample with a matching name exists and is
     * fully baked (rendered + bound), the decision is "reuse".  If a session
     * with the same name exists but is only compiled (not rendered), the
     * decision is "modify" (re-render).  Otherwise "create".
     */
    ReuseDecision decideReuse(
        const std::vector<ReuseFinding>& findings,
        const std::string& assetName);

    /**
     * Assemble the ordered step list for a mini-notation pattern workflow.
     */
    std::vector<PlanStep> buildPatternSteps(
        const PlanModel& model,
        const std::vector<ReuseFinding>& findings,
        const std::string& intentKeywords);

    /**
     * Assemble the ordered step list for a ChucK instrument workflow.
     */
    std::vector<PlanStep> buildChuckSteps(
        const PlanModel& model,
        const std::vector<ReuseFinding>& findings,
        const ReuseDecision& reuse);

    ProjectReadFacade&   readFacade_;
    ChuckSessionService& chuckService_;
    RenderService&       renderService_;
};

} // namespace hathor::control
