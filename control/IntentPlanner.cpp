// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * IntentPlanner.cpp — AI-10.1 implementation.
 *
 * See IntentPlanner.hpp for the full architecture documentation.
 */

#include "IntentPlanner.hpp"

#include "ProjectReadFacade.hpp"
#include "ChuckSessionService.hpp"
#include "RenderService.hpp"

#include "../app/AssetTarget.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace hathor::control {

// ---------------------------------------------------------------------------
// PlanStep::toJson
// ---------------------------------------------------------------------------

nlohmann::json PlanStep::toJson() const
{
    nlohmann::json j;
    j["name"]              = name;
    j["service"]           = service;
    j["method"]            = method;
    j["capability_class"]  = toString(capabilityClass);
    j["requires_confirmation"] = requiresConfirmation;
    j["description"]       = description;
    if (!params.is_null())
        j["params"] = params;
    return j;
}

// ---------------------------------------------------------------------------
// ReuseFinding::toJson
// ---------------------------------------------------------------------------

nlohmann::json ReuseFinding::toJson() const
{
    nlohmann::json j;
    j["resource_id"]      = resource_id;
    j["name"]             = name;
    j["type"]             = type;
    j["lifecycle_state"]  = lifecycle_state;
    j["details"]          = details;
    return j;
}

// ---------------------------------------------------------------------------
// PlanModel::toJson
// ---------------------------------------------------------------------------

nlohmann::json PlanModel::toJson() const
{
    nlohmann::json j;
    j["mode"]            = mode;
    j["intent"]          = intent;
    j["target_slot"]     = targetSlot;
    j["asset_name"]      = assetName;
    j["duration_bars"]   = durationBars;
    j["is_dry_run"]      = isDryRun;
    j["reuse_decision"]  = toString(reuseDecision);

    // Include generated content when available (AI-10.1).
    if (!notation.empty())
        j["notation"] = notation;
    if (!ckSource.empty())
        j["ck_source"] = ckSource;

    nlohmann::json findings = nlohmann::json::array();
    for (const auto& f : reuseFindings)
        findings.push_back(f.toJson());
    j["reuse_findings"]  = findings;

    nlohmann::json stepsArr = nlohmann::json::array();
    for (const auto& s : steps)
        stepsArr.push_back(s.toJson());
    j["steps"] = stepsArr;

    return j;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

IntentPlanner::IntentPlanner(ProjectReadFacade& readFacade,
                             ChuckSessionService& chuckService,
                             RenderService& renderService) noexcept
    : readFacade_(readFacade)
    , chuckService_(chuckService)
    , renderService_(renderService)
{}

// ---------------------------------------------------------------------------
// discoverReuseCandidates
// ---------------------------------------------------------------------------

std::vector<ReuseFinding> IntentPlanner::discoverReuseCandidates(
    const std::string& intentKeywords,
    const std::string& assetName)
{
    std::vector<ReuseFinding> findings;

    // --- Query the project for existing ChucK instruments ---
    // inspectProject() gives us the project_dir; we pass it to
    // listChuckInstruments() for the full asset inventory.
    auto projectInfo = readFacade_.inspectProject();
    if (!projectInfo.value("ok", false))
        return findings;

    std::filesystem::path projectDir = projectInfo.value("project_dir", std::string{});
    if (projectDir.empty())
        return findings;

    auto instruments = readFacade_.listChuckInstruments(projectDir);
    if (instruments.value("ok", false)) {
        for (const auto& inst : instruments.value("instruments", nlohmann::json::array())) {
            const std::string name = inst.value("name", std::string{});
            if (name.empty())
                continue;

            ReuseFinding finding;
            finding.resource_id = "instrument:" + name;
            finding.name = name;
            finding.type = "chuck_instrument";
            finding.lifecycle_state = inst.value("lifecycle_state", std::string{"unknown"});

            finding.details["source_ck_exists"] = inst.value("source_ck_exists", false);
            finding.details["rendered_wav_exists"] = inst.value("rendered_wav_exists", false);
            finding.details["bound_to_sample_bank"] = inst.value("bound_to_sample_bank", false);
            if (inst.contains("source_path"))
                finding.details["source_path"] = inst["source_path"];
            if (inst.contains("rendered_path"))
                finding.details["rendered_path"] = inst["rendered_path"];
            if (inst.contains("duration_seconds"))
                finding.details["duration_seconds"] = inst["duration_seconds"];

            findings.push_back(std::move(finding));
        }
    }

    // --- Check SampleBank for existing samples ---
    auto samples = readFacade_.listSamples();
    if (samples.value("ok", false)) {
        for (const auto& s : samples.value("samples", nlohmann::json::array())) {
            const std::string name = s.value("name", std::string{});
            if (name.empty())
                continue;

            ReuseFinding finding;
            finding.resource_id = "sample:" + name;
            finding.name = name;
            finding.type = "sample";
            finding.lifecycle_state = "bound";

            if (s.contains("path"))
                finding.details["path"] = s["path"];
            if (s.contains("duration_seconds"))
                finding.details["duration_seconds"] = s["duration_seconds"];

            findings.push_back(std::move(finding));
        }
    }

    // --- Check existing session state for the asset name ---
    // Look for a session whose ID matches the asset name pattern.
    // Sessions are "ck:N" so we check by source content matching.
    // We query listRenderJobs to see if a previous render exists.
    auto jobs = renderService_.listRenderJobs();
    if (jobs.is_array()) {
        for (const auto& job : jobs) {
            const std::string assetNameVal = job.value("asset_name", std::string{});
            if (!assetNameVal.empty() && assetNameVal == assetName) {
                ReuseFinding finding;
                finding.resource_id = "render_job:" + assetNameVal;
                finding.name = assetNameVal;
                finding.type = "render_job";
                finding.lifecycle_state = job.value("status", std::string{"unknown"});
                finding.details["job_id"] = job.value("job_id", 0);
                findings.push_back(std::move(finding));
            }
        }
    }

    (void)intentKeywords; // reserved for future keyword-based matching

    return findings;
}

// ---------------------------------------------------------------------------
// decideReuse
// ---------------------------------------------------------------------------

ReuseDecision IntentPlanner::decideReuse(
    const std::vector<ReuseFinding>& findings,
    const std::string& assetName)
{
    // Look for a fully-baked ChucK instrument matching the asset name.
    for (const auto& f : findings) {
        if (f.type == "chuck_instrument" && f.name == assetName) {
            const bool rendered = f.details.value("rendered_wav_exists", false);
            const bool bound = f.details.value("bound_to_sample_bank", false);
            if (rendered && bound) {
                return ReuseDecision::Reuse;  // fully baked and registered
            }
            if (rendered || bound) {
                return ReuseDecision::Modify;  // partially complete, needs work
            }
        }
    }

    // Look for a sample with the same name.
    for (const auto& f : findings) {
        if (f.type == "sample" && f.name == assetName) {
            return ReuseDecision::Reuse;  // sample already registered
        }
    }

    // No matching asset found — need to create one.
    return ReuseDecision::Create;
}

// ---------------------------------------------------------------------------
// buildPatternSteps
// ---------------------------------------------------------------------------

std::vector<PlanStep> IntentPlanner::buildPatternSteps(
    const PlanModel& model,
    const std::vector<ReuseFinding>& findings,
    const std::string& intentKeywords)
{
    std::vector<PlanStep> steps;
    (void)intentKeywords;

    // 1. inspect_project — read-only, auto-execute
    steps.push_back({
        "inspect_project",
        "ProjectReadFacade",
        "inspectProject",
        CapabilityClass::ReadOnly,
        false,
        {},
        "Inspect the current project structure, slots, and tempo."
    });

    // 2. inspect_song — read-only, auto-execute
    steps.push_back({
        "inspect_song",
        "ProjectReadFacade",
        "getCurrentSong",
        CapabilityClass::ReadOnly,
        false,
        {},
        "Read the active song's patterns, tempo, and diagnostics."
    });

    // 3. inspect_assets — read-only, auto-execute
    steps.push_back({
        "inspect_assets",
        "ProjectReadFacade",
        "listAssets",
        CapabilityClass::ReadOnly,
        false,
        {},
        "Inventory existing samples and instruments."
    });

    // 4. generate_pattern — non-destructive, auto-execute
    //    The generated notation is held in memory, not yet persisted.
    nlohmann::json genParams;
    genParams["slot"] = model.targetSlot;
    genParams["duration_bars"] = model.durationBars;
    genParams["intent"] = model.intent;

    // If the intent mentions reusing an existing slot, note it.
    if (!findings.empty()) {
        nlohmann::json reusing = nlohmann::json::array();
        for (const auto& f : findings) {
            if (f.type == "pattern_slot")
                reusing.push_back(f.toJson());
        }
        if (!reusing.empty())
            genParams["reuse_candidates"] = reusing;
    }

    steps.push_back({
        "generate_pattern",
        "AgenticWorkflow",
        "stepGeneratePattern",
        CapabilityClass::NonDestructive,
        false,
        genParams,
        "Generate mini-notation for slot '" + model.targetSlot + "' from the intent."
    });

    // 5. validate — read-only, auto-execute
    steps.push_back({
        "validate",
        "ProjectReadFacade",
        "getDiagnostics",
        CapabilityClass::ReadOnly,
        false,
        {{"is_chuck", false}},
        "Validate the generated notation against the mini-notation parser."
    });

    // 6. audition — non-destructive (play pattern), auto-execute
    nlohmann::json auditionParams;
    auditionParams["slot"] = model.targetSlot;
    steps.push_back({
        "audition",
        "AgenticWorkflow",
        "stepAudition",
        CapabilityClass::NonDestructive,
        false,
        auditionParams,
        "Play the generated pattern so the composer can hear it."
    });

    // 7. inspect_diagnostics — read-only, auto-execute
    steps.push_back({
        "inspect_diagnostics",
        "ProjectReadFacade",
        "getDiagnostics",
        CapabilityClass::ReadOnly,
        false,
        {{"source", "pattern"}},
        "Check diagnostics from the audition."
    });

    // 8. repair_loop — non-destructive, auto-execute (conditional)
    steps.push_back({
        "repair_loop",
        "AgenticWorkflow",
        "stepRepair",
        CapabilityClass::NonDestructive,
        false,
        {{"max_attempts", 3}},
        "If diagnostics show issues, regenerate or simplify the pattern."
    });

    // 9. update_song — persistent mutation (requires confirmation)
    //     Skipped entirely in dry_run mode.
    if (model.isDryRun) {
        steps.push_back({
            "update_song",
            "SongMutationService",
            "editSong",
            CapabilityClass::PersistentMutation,
            false,  // dry-run: planned but never executed
            {{"dry_run", true}},
            "Persist the new pattern to the song file (dry-run: skipped)."
        });
    } else {
        nlohmann::json updateParams;
        updateParams["slot"] = model.targetSlot;
        updateParams["duration_bars"] = model.durationBars;
        steps.push_back({
            "update_song",
            "SongMutationService",
            "editSong",
            CapabilityClass::PersistentMutation,
            true,  // requires user confirmation
            updateParams,
            "Persist the new pattern to the song file (requires confirmation)."
        });
    }

    return steps;
}

// ---------------------------------------------------------------------------
// buildChuckSteps
// ---------------------------------------------------------------------------

std::vector<PlanStep> IntentPlanner::buildChuckSteps(
    const PlanModel& model,
    const std::vector<ReuseFinding>& findings,
    const ReuseDecision& reuse)
{
    (void)findings;
    std::vector<PlanStep> steps;

    // 1. inspect_project — read-only, auto-execute
    steps.push_back({
        "inspect_project",
        "ProjectReadFacade",
        "inspectProject",
        CapabilityClass::ReadOnly,
        false,
        {},
        "Inspect the current project structure, slots, and tempo."
    });

    // 2. inspect_song — read-only, auto-execute
    steps.push_back({
        "inspect_song",
        "ProjectReadFacade",
        "getCurrentSong",
        CapabilityClass::ReadOnly,
        false,
        {},
        "Read the active song's patterns, tempo, and diagnostics."
    });

    // 3. inspect_assets — read-only, auto-execute
    steps.push_back({
        "inspect_assets",
        "ProjectReadFacade",
        "listAssets",
        CapabilityClass::ReadOnly,
        false,
        {},
        "Inventory existing samples and instruments."
    });

    // 4. create_chuck_session — non-destructive (session metadata only, no VM),
    //    auto-execute.  The session is created without activating a live VM
    //    per AI-5 §3.
    nlohmann::json sessionParams;
    sessionParams["target_slot"] = model.targetSlot;
    sessionParams["asset_name"] = model.assetName;
    steps.push_back({
        "create_chuck_session",
        "ChuckSessionService",
        "createSession",
        CapabilityClass::NonDestructive,
        false,
        sessionParams,
        "Create a ChucK session for slot '" + model.targetSlot + "' (no VM activated yet)."
    });

    // 5. compile_chuck — non-destructive (compiles to session, no persistent file),
    //    auto-execute.  Uses async job infrastructure.
    steps.push_back({
        "compile",
        "ChuckSessionService",
        "compileChuck",
        CapabilityClass::NonDestructive,
        false,
        {{"session_id", "derived"}, {"source", "provided"}},
        "Compile the ChucK source in the session (async job, no persistent file)."
    });

    // 6. audition_chuck — non-destructive (activates VM for live audition),
    //    auto-execute.
    nlohmann::json auditionParams;
    auditionParams["session_id"] = "derived";
    steps.push_back({
        "audition",
        "ChuckSessionService",
        "auditionSession",
        CapabilityClass::NonDestructive,
        false,
        auditionParams,
        "Activate the per-tab VM and play the instrument for audition."
    });

    // 7. inspect_diagnostics — read-only, auto-execute
    steps.push_back({
        "inspect_diagnostics",
        "ChuckSessionService",
        "getDiagnostics",
        CapabilityClass::ReadOnly,
        false,
        {{"source", "chuck"}},
        "Check ChucK compiler/runtime diagnostics."
    });

    // 8. repair_loop — non-destructive, auto-execute (conditional)
    steps.push_back({
        "repair_loop",
        "AgenticWorkflow",
        "stepRepair",
        CapabilityClass::NonDestructive,
        false,
        {{"max_attempts", 3}},
        "If diagnostics show compile errors, attempt repair (re-compile)."
    });

    // 9. render_chuck — non-destructive (writes to TEMP dir only, not project tree),
    //    auto-execute.  This is the render→commit boundary from AI-6.
    nlohmann::json renderParams;
    renderParams["session_id"] = "derived";
    renderParams["duration_bars"] = model.durationBars;
    renderParams["asset_name"] = model.assetName;
    renderParams["target"] = toString(hathor::AssetTarget::Studio);
    steps.push_back({
        "render",
        "RenderService",
        "renderChuck",
        CapabilityClass::NonDestructive,
        false,
        renderParams,
        "Render the ChucK instrument to a temporary WAV (non-persistent)."
    });

    // 10. bind_asset / commit_rendered_asset — persistent mutation
    //     (writes .ck + .wav to the project asset tree + SampleBank registration).
    //     Requires confirmation unless dry-run.
    if (model.isDryRun) {
        steps.push_back({
            "bind_asset",
            "RenderService",
            "commitRenderedAsset",
            CapabilityClass::PersistentMutation,
            false,
            {{"dry_run", true}, {"asset_name", model.assetName}},
            "Commit the rendered WAV to the project asset tree (dry-run: skipped)."
        });
    } else {
        std::string desc = "Persist the rendered instrument to the project asset tree";
        if (reuse == ReuseDecision::Create)
            desc += " (new asset '" + model.assetName + "')";
        else if (reuse == ReuseDecision::Modify)
            desc += " (overwriting existing '" + model.assetName + "')";
        else
            desc += " (reusing existing '" + model.assetName + "')";
        desc += " — requires confirmation.";

        steps.push_back({
            "bind_asset",
            "RenderService",
            "commitRenderedAsset",
            CapabilityClass::PersistentMutation,
            true,  // requires user confirmation
            {{"asset_name", model.assetName},
             {"reuse_decision", toString(reuse)}},
            desc
        });
    }

    // 11. update_song — persistent mutation (binds instrument name to song slot)
    //     Requires confirmation unless dry-run.
    if (model.isDryRun) {
        steps.push_back({
            "update_song",
            "SongMutationService",
            "editSong",
            CapabilityClass::PersistentMutation,
            false,
            {{"dry_run", true}},
            "Update the song file to reference the new instrument (dry-run: skipped)."
        });
    } else {
        steps.push_back({
            "update_song",
            "SongMutationService",
            "editSong",
            CapabilityClass::PersistentMutation,
            true,  // requires user confirmation
            {{"slot", model.targetSlot},
             {"instrument", model.assetName}},
            "Update the song file to reference instrument '" + model.assetName +
            "' — requires confirmation."
        });
    }

    return steps;
}

// ---------------------------------------------------------------------------
// planFromRequest
// ---------------------------------------------------------------------------

PlanModel IntentPlanner::planFromRequest(
    std::string_view intent,
    std::string_view targetSlot,
    std::string_view assetName,
    int              durationBars,
    bool             dryRun)
{
    PlanModel model;
    model.intent = std::string(intent);
    model.targetSlot = std::string(targetSlot);
    model.assetName = std::string(assetName);
    model.durationBars = durationBars;
    model.isDryRun = dryRun;

    // Inspect existing project state to populate reuse findings.
    model.reuseFindings = discoverReuseCandidates(
        model.intent, model.assetName);
    model.reuseDecision = decideReuse(
        model.reuseFindings, model.assetName);

    // Determine mode: ChucK if an asset name is provided; pattern otherwise.
    std::string intentKeywords = model.intent;
    std::transform(intentKeywords.begin(), intentKeywords.end(),
                   intentKeywords.begin(), ::tolower);

    const bool isChuckWorkflow = !model.assetName.empty();
    model.mode = isChuckWorkflow ? "chuck" : "pattern";

    // Generate starter content from the intent (AI-10.1).
    if (isChuckWorkflow) {
        model.ckSource = generateChuckSource(intentKeywords, model.assetName);
    } else {
        model.notation = generateMiniNotation(intentKeywords, model.targetSlot,
                                              model.durationBars, model.assetName);
    }

    if (isChuckWorkflow) {
        model.steps = buildChuckSteps(model, model.reuseFindings, model.reuseDecision);
    } else {
        model.steps = buildPatternSteps(model, model.reuseFindings, intentKeywords);
    }

    return model;
}
    }

    return model;
}

// ---------------------------------------------------------------------------
// planFromRequestWithOverride
// ---------------------------------------------------------------------------

PlanModel IntentPlanner::planFromRequestWithOverride(
    std::string_view intent,
    std::string_view targetSlot,
    std::string_view assetName,
    int              durationBars,
    bool             dryRun,
    const nlohmann::json& overridePlan)
{
    // If the caller provided a pre-determined plan, validate it is well-formed.
    if (overridePlan.contains("steps") && overridePlan["steps"].is_array()) {
        PlanModel model;
        model.intent = std::string(intent);
        model.targetSlot = std::string(targetSlot);
        model.assetName = std::string(assetName);
        model.durationBars = durationBars;
        model.isDryRun = dryRun;
        model.mode = overridePlan.value("mode", std::string{"pattern"});

        if (overridePlan.contains("reuse_decision")) {
            const std::string rd = overridePlan["reuse_decision"];
            if (rd == "reuse") model.reuseDecision = ReuseDecision::Reuse;
            else if (rd == "modify") model.reuseDecision = ReuseDecision::Modify;
            else model.reuseDecision = ReuseDecision::Create;
        }

        for (const auto& stepJson : overridePlan["steps"]) {
            PlanStep step;
            step.name = stepJson.value("name", std::string{});
            step.service = stepJson.value("service", std::string{});
            step.method = stepJson.value("method", std::string{});
            step.description = stepJson.value("description", std::string{});
            step.params = stepJson.value("params", nlohmann::json::object());
            step.requiresConfirmation = stepJson.value("requires_confirmation", false);

            const std::string cc = stepJson.value("capability_class", std::string{"read_only"});
            if (cc == "read_only") step.capabilityClass = CapabilityClass::ReadOnly;
            else if (cc == "non_destructive") step.capabilityClass = CapabilityClass::NonDestructive;
            else step.capabilityClass = CapabilityClass::PersistentMutation;

            if (step.requiresConfirmation)
                step.capabilityClass = CapabilityClass::PersistentMutation;

            model.steps.push_back(std::move(step));
        }

        return model;
    }

    // No usable override — fall back to a fresh plan.
    return planFromRequest(intent, targetSlot, assetName, durationBars, dryRun);
}

// ---------------------------------------------------------------------------
// Content generation (AI-10.1)
// ---------------------------------------------------------------------------

std::string IntentPlanner::generateChuckSource(
    const std::string& intentKeywords,
    const std::string& assetName)
{
    (void)assetName; // reserved for future per-instrument templates

    // Acid bass: TB-303-style resonant low-pass filtered saw/square.
    if (intentKeywords.find("acid") != std::string::npos &&
        (intentKeywords.find("bass") != std::string::npos ||
         intentKeywords.find("sub") != std::string::npos)) {
        return "SqrOsc osc => LPF lpf => Gain g => dac;\n"
               "0.4 => g.gain;\n"
               "Std.mtof(36) => osc.freq;\n"
               "200 => lpf.freq;\n"
               "0.8 => lpf.Q;\n"
               "1::second => now;";
    }

    // House bass: warm sine/sub.
    if (intentKeywords.find("bass") != std::string::npos &&
        intentKeywords.find("house") != std::string::npos) {
        return "SinOsc osc => Gain g => dac;\n"
               "0.5 => g.gain;\n"
               "Std.mtof(36) => osc.freq;\n"
               "1::second => now;";
    }

    // Kick/BD: sine sub with pitch envelope.
    if (intentKeywords.find("kick") != std::string::npos ||
        intentKeywords.find("bd") != std::string::npos) {
        return "SinOsc osc => Gain g => dac;\n"
               "0.8 => g.gain;\n"
               "40 => osc.freq;\n"
               "500::ms => now;\n"
               "0 => osc.freq;\n"
               "500::ms => now;";
    }

    // Snare: noise burst.
    if (intentKeywords.find("snare") != std::string::npos ||
        intentKeywords.find("sn") != std::string::npos) {
        return "NoiseOsc osc => LPF lpf => Gain g => dac;\n"
               "0.3 => g.gain;\n"
               "2000 => lpf.freq;\n"
               "100::ms => now;";
    }

    // Hi-hat: filtered noise.
    if (intentKeywords.find("hi-hat") != std::string::npos ||
        intentKeywords.find("hihat") != std::string::npos ||
        intentKeywords.find("hh") != std::string::npos) {
        return "NoiseOsc osc => HPF hpf => Gain g => dac;\n"
               "0.15 => g.gain;\n"
               "8000 => hpf.freq;\n"
               "50::ms => now;";
    }

    // Lead: saw/square lead.
    if (intentKeywords.find("lead") != std::string::npos ||
        intentKeywords.find("saw") != std::string::npos) {
        return "SawOsc osc => LPF lpf => Gain g => dac;\n"
               "0.3 => g.gain;\n"
               "1500 => lpf.freq;\n"
               "Std.mtof(60) => osc.freq;\n"
               "1::second => now;";
    }

    // Pad/chord: soft saw pad.
    if (intentKeywords.find("pad") != std::string::npos ||
        intentKeywords.find("chord") != std::string::npos) {
        return "SawOsc osc => LPF lpf => Gain g => dac;\n"
               "0.2 => g.gain;\n"
               "1000 => lpf.freq;\n"
               "Std.mtof(48) => osc.freq;\n"
               "2::second => now;";
 }

    // Generic default.
    return "SinOsc osc => Gain g => dac;\n"
           "0.3 => g.gain;\n"
           "Std.mtof(48) => osc.freq;\n"
           "1::second => now;";
}

std::string IntentPlanner::generateMiniNotation(
    const std::string& intentKeywords,
    const std::string& targetSlot,
    int durationBars,
    const std::string& assetName)
{
    // If the user already provided a slot reference, use s "name" on that slot.
    if (!assetName.empty()) {
        return targetSlot + "(s \"" + assetName + "\")";
    }

    // Kick/BD.
    if (intentKeywords.find("kick") != std::string::npos ||
        intentKeywords.find("bd") != std::string::npos) {
        return targetSlot + "([bd ~ bd ~] * " + std::to_string(durationBars) + ")";
    }

    // Snare.
    if (intentKeywords.find("snare") != std::string::npos ||
        intentKeywords.find("sn") != std::string::npos) {
        return targetSlot + "([sn ~] * " + std::to_string(durationBars * 2) + ")";
    }

    // Hi-hat.
    if (intentKeywords.find("hi-hat") != std::string::npos ||
        intentKeywords.find("hihat") != std::string::npos ||
        intentKeywords.find("hh") != std::string::npos) {
        return targetSlot + "([hh hh ~ hh ~] * " + std::to_string(durationBars * 2) + ")";
    }

    // Chord/pad.
    if (intentKeywords.find("chord") != std::string::npos ||
        intentKeywords.find("pad") != std::string::npos) {
        if (intentKeywords.find("dark") != std::string::npos)
            return targetSlot + "(m1 * " + std::to_string(durationBars) + ")";
        return targetSlot + "(c1 * " + std::to_string(durationBars) + ")";
    }

    // Default: bassline pattern using the target slot.
    if (intentKeywords.find("acid") != std::string::npos) {
        return targetSlot + "([c1 e1 g1 b1] * " + std::to_string(durationBars) + ")";
    }
    return targetSlot + "([c1 g1 c1 g1] * " + std::to_string(durationBars) + ")";
}

// Also populate notation/ckSource for override plans.
PlanModel IntentPlanner::planFromRequestWithOverrideImpl(
    std::string_view intent,
    std::string_view targetSlot,
    std::string_view assetName,
    int              durationBars,
    bool             dryRun,
    const nlohmann::json& overridePlan)
{
    // If the caller provided a pre-determined plan, validate it is well-formed.
    if (overridePlan.contains("steps") && overridePlan["steps"].is_array()) {
        PlanModel model;
        model.intent = std::string(intent);
        model.targetSlot = std::string(targetSlot);
        model.assetName = std::string(assetName);
        model.durationBars = durationBars;
        model.isDryRun = dryRun;
        model.mode = overridePlan.value("mode", std::string{"pattern"});

        if (overridePlan.contains("reuse_decision")) {
            const std::string rd = overridePlan["reuse_decision"];
            if (rd == "reuse") model.reuseDecision = ReuseDecision::Reuse;
            else if (rd == "modify") model.reuseDecision = ReuseDecision::Modify;
            else model.reuseDecision = ReuseDecision::Create;
        }

        // Carry through generated content if present in the override.
        model.notation = overridePlan.value("notation", std::string{});
        model.ckSource = overridePlan.value("ck_source", std::string{});

        for (const auto& stepJson : overridePlan["steps"]) {
            PlanStep step;
            step.name = stepJson.value("name", std::string{});
            step.service = stepJson.value("service", std::string{});
            step.method = stepJson.value("method", std::string{});
            step.description = stepJson.value("description", std::string{});
            step.params = stepJson.value("params", nlohmann::json::object());
            step.requiresConfirmation = stepJson.value("requires_confirmation", false);

            const std::string cc = stepJson.value("capability_class", std::string{"read_only"});
            if (cc == "read_only") step.capabilityClass = CapabilityClass::ReadOnly;
            else if (cc == "non_destructive") step.capabilityClass = CapabilityClass::NonDestructive;
            else step.capabilityClass = CapabilityClass::PersistentMutation;

            if (step.requiresConfirmation)
                step.capabilityClass = CapabilityClass::PersistentMutation;

            model.steps.push_back(std::move(step));
        }

        // If override didn't include generated content, generate from intent.
        if (model.notation.empty() && model.ckSource.empty()) {
            std::string kw = model.intent;
            std::transform(kw.begin(), kw.end(), kw.begin(), ::tolower);
            if (model.mode == "chuck") {
                model.ckSource = generateChuckSource(kw, model.assetName);
            } else {
                model.notation = generateMiniNotation(kw, model.targetSlot,
                                                      model.durationBars, model.assetName);
            }
        }

        return model;
    }

    // No usable override — fall back to a fresh plan.
    return planFromRequest(intent, targetSlot, assetName, durationBars, dryRun);
}

} // namespace hathor::control
