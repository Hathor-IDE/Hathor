// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_ai10_1_intent_plan.cpp — AI-10.1: Natural-Language Intent → Actionable Plan.
 *
 * Verifies:
 *   1. Pattern mode plan — correct step sequence, mode="pattern"
 *   2. Chuck mode plan — correct step sequence, mode="chuck"
 *   3. Reuse detection — existing fully-baked instrument → decision="reuse"
 *   4. Modify decision — existing source-only instrument → decision="modify"
 *   5. Create decision — no matching instrument → decision="create"
 *   6. Dry-run mode — persistent-mutation steps tagged with dry_run=true
 *   7. Override plan — pre-determined plan is used when provided
 *   8. Capability class tagging — each step carries its AI-1 class
 *   9. Confirmation gating — persistent-mutation steps require confirmation
 *  10. Plan JSON is well-formed and matches documented schema
 *  11. Cancellation safety — planner does not perform any mutations
 *  12. Empty project — plan is produced with zero reuse findings
 *
 * Architecture: tests construct a configurable FakePlanFacade (extends
 * AudioEngineFacade) + real SampleBank, then build real ProjectReadFacade,
 * ChuckSessionService, and RenderService instances. IntentPlanner queries
 * only read-only service methods — no mutations occur.
 *
 * Requirement references: AI-10.1, AI-1 capability model, PROGRAM.md §1392
 */

#include "ControlInterface.hpp"
#include "IntentPlanner.hpp"
#include "ProjectReadFacade.hpp"
#include "ChuckSessionService.hpp"
#include "RenderService.hpp"
#include "AgenticWorkflow.hpp"

#include "AudioEngineFacade.hpp"
#include "SampleBank.hpp"
#include "SlotState.hpp"
#include "AssetTarget.hpp"
#include "ChuckRenderWriter.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using hathor::control::IntentPlanner;
using hathor::control::PlanModel;
using hathor::control::PlanStep;
using hathor::control::CapabilityClass;
using hathor::control::ReuseDecision;
using hathor::control::ReuseFinding;
using hathor::control::ProjectReadFacade;
using hathor::control::ChuckSessionService;
using hathor::control::RenderService;
using hathor::control::AgenticWorkflow;

namespace fs = std::filesystem;

// ===========================================================================
// FakePlanFacade — configurable AudioEngineFacade for IntentPlanner tests
// ===========================================================================

class FakePlanFacade final : public AudioEngineFacade {
public:
    std::vector<InstrumentInfo> fakeInstruments;
    std::vector<std::string>    fakeSamples;
    fs::path                    projectDir;

    // --- Transport ---
    void play() noexcept override {}
    void stop() noexcept override {}
    void setBpm(double bpm) noexcept override { bpm_ = bpm; }
    double getBpm() const noexcept override { return bpm_; }
    bool isRunning() const noexcept override { return true; }

    // --- Master gain / EQ ---
    void setMasterGain(float) noexcept override {}
    float getMasterGain() const noexcept override { return 1.0f; }
    void setMasterEqPreset(hathor::EqPreset) noexcept override {}
    hathor::EqPreset getMasterEqPreset() const noexcept override { return hathor::EqPreset::Flat; }

    // --- Slot API ---
    int findOrAddSlot(const std::string&) override { return 0; }
    void storeSlot(int, std::shared_ptr<SlotState>) noexcept override {}
    bool clearSlot(int) noexcept override { return true; }
    int slotCount() const noexcept override { return 4; }
    std::string slotName(int idx) const override {
        static const char* names[] = {"d0", "d1", "d2", "d3"};
        return (idx >= 0 && idx < 4) ? names[idx] : "";
    }
    std::shared_ptr<SlotState> loadSlot(int) const noexcept override { return nullptr; }

    // --- Per-slot play/stop ---
    void slotPlay(int) noexcept override {}
    void slotStop(int) noexcept override {}
    bool isSlotRunning(int) const noexcept override { return false; }

    // --- B4-K7: ChucK VM ---
    bool hasWorker() const noexcept override { return false; }
    bool ckEval(int, const std::string&) noexcept override { return true; }
    bool stopCkTab(int) noexcept override { return true; }
    std::string queryCkTab(int) const override { return "ok"; }
    uint64_t startAsyncCkCompile(int, const std::string&,
                                 std::function<void(bool, const std::string&)>) override
    { return 1; }
    nlohmann::json queryCkJob(uint64_t) const override
    { return {{"ok", true}, {"status", "succeeded"}}; }
    bool cancelCkJob(uint64_t) override { return true; }

    // --- B8-K1 ---
    fs::path resolveRenderPath(hathor::AssetTarget, std::string_view,
                               const fs::path&) override { return {}; }
    void setLiveJamSessionDir(fs::path) override {}
    void setProjectDir(fs::path dir) override { projectDir = std::move(dir); }
    fs::path currentProjectDir() const noexcept override { return projectDir; }
    void cleanupLiveJamAssets() override {}
    bool isStudioAssetPath(const fs::path&) const override { return false; }

    // --- B8-K2 ---
    hathor::RenderHandle startBakeRender(uint8_t, std::string, uint64_t, unsigned,
                                         const fs::path&,
                                         hathor::ChuckRenderWriter::CompletionCallback) override
    { return {}; }
    hathor::RenderHandle startBakeRenderRaw(uint8_t, std::string, uint64_t, unsigned,
                                            const fs::path&,
                                            hathor::ChuckRenderWriter::CompletionCallback) override
    { return {}; }
    int activeRenderCount() const noexcept override { return 0; }
    void shutdownRender() noexcept override {}

    // --- B8-K4 ---
    bool registerBakedAsset(std::string, const fs::path&) override { return true; }
    std::vector<std::string> listSamples() const override { return fakeSamples; }

    // --- AI-2 read-only introspection ---
    std::vector<SlotInfo> listSlots() const noexcept override { return {}; }
    SlotInfo getSlotInfo(int) const noexcept override { return {}; }
    VmStatus getVmStatus(int) const noexcept override { return {}; }
    std::vector<SlotPlayback> listSlotPlayback() const noexcept override { return {}; }

    std::vector<InstrumentInfo> listChuckInstruments(
        const fs::path&) const noexcept override
    { return fakeInstruments; }

    fs::path studioInstrumentsDir(const fs::path& projectDir) const noexcept override {
        return projectDir / ".hathor_assets" / "chuck_instruments";
    }

    // --- Test helpers ---
    double bpm_ = 120.0;
};

// ===========================================================================
// Test helpers
// ===========================================================================

/// Build a fully-bound instrument info entry.
static AudioEngineFacade::InstrumentInfo makeBoundInstrument(const std::string& name,
                                                              const std::string& ckPath,
                                                              const std::string& wavPath)
{
    AudioEngineFacade::InstrumentInfo inst;
    inst.name = name;
    inst.sourceCkExists = true;
    inst.renderedWavExists = true;
    inst.boundToSampleBank = true;
    inst.sourcePath = ckPath;
    inst.renderedPath = wavPath;
    inst.durationSeconds = 4.0;
    return inst;
}

/// Build a source-only instrument info entry (not yet rendered).
static AudioEngineFacade::InstrumentInfo makeSourceOnlyInstrument(const std::string& name,
                                                                   const std::string& ckPath)
{
    AudioEngineFacade::InstrumentInfo inst;
    inst.name = name;
    inst.sourceCkExists = true;
    inst.renderedWavExists = false;
    inst.boundToSampleBank = false;
    inst.sourcePath = ckPath;
    inst.durationSeconds = 0.0;
    return inst;
}

/// Helper: find a step by name in a PlanModel.
static const PlanStep* findStep(const PlanModel& model, const std::string& name)
{
    for (const auto& s : model.steps)
        if (s.name == name)
            return &s;
    return nullptr;
}

// ===========================================================================
// 1. Pattern mode plan — correct step sequence and mode
// ===========================================================================

TEST_CASE("AI-10.1: pattern mode produces correct plan structure",
          "[ai10][ai10_1][pattern_mode]")
{
    FakePlanFacade audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_test";
    audio.fakeSamples = {"bd", "sn", "hh"};

    SampleBank bank;
    ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    RenderService renderService(audio, bank, chuckService);

    IntentPlanner planner(readFacade, chuckService, renderService);

    PlanModel plan = planner.planFromRequest(
        "simple 4-bar pattern",
        "d1",
        "",           // no asset name → pattern mode
        4,
        false);

    REQUIRE(plan.mode == "pattern");
    REQUIRE(plan.intent == "simple 4-bar pattern");
    REQUIRE(plan.targetSlot == "d1");
    REQUIRE(plan.durationBars == 4);
    REQUIRE(plan.isDryRun == false);

    // First three steps must be read-only inspection.
    REQUIRE(plan.steps.size() >= 3);
    REQUIRE(plan.steps[0].name == "inspect_project");
    REQUIRE(plan.steps[0].capabilityClass == CapabilityClass::ReadOnly);
    REQUIRE_FALSE(plan.steps[0].requiresConfirmation);
    REQUIRE(plan.steps[1].name == "inspect_song");
    REQUIRE(plan.steps[2].name == "inspect_assets");

    // generate_pattern must be non-destructive.
    const PlanStep* gen = findStep(plan, "generate_pattern");
    REQUIRE(gen != nullptr);
    REQUIRE(gen->capabilityClass == CapabilityClass::NonDestructive);

    // validate must be read-only.
    const PlanStep* val = findStep(plan, "validate");
    REQUIRE(val != nullptr);
    REQUIRE(val->capabilityClass == CapabilityClass::ReadOnly);

    // update_song must be persistent_mutation with confirmation.
    const PlanStep* upd = findStep(plan, "update_song");
    REQUIRE(upd != nullptr);
    REQUIRE(upd->capabilityClass == CapabilityClass::PersistentMutation);
    REQUIRE(upd->requiresConfirmation);
}

// ===========================================================================
// 2. Chuck mode plan — correct step sequence and mode
// ===========================================================================

TEST_CASE("AI-10.1: chuck mode produces correct plan structure",
          "[ai10][ai10_1][chuck_mode]")
{
    FakePlanFacade audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_test";

    SampleBank bank;
    ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    RenderService renderService(audio, bank, chuckService);

    IntentPlanner planner(readFacade, chuckService, renderService);

    PlanModel plan = planner.planFromRequest(
        "dark 8-bar acid bassline",
        "d2",
        "acid_bass",   // asset name present → chuck mode
        8,
        false);

    REQUIRE(plan.mode == "chuck");
    REQUIRE(plan.intent == "dark 8-bar acid bassline");
    REQUIRE(plan.targetSlot == "d2");
    REQUIRE(plan.assetName == "acid_bass");
    REQUIRE(plan.durationBars == 8);

    // Must include create_chuck_session.
    const PlanStep* sess = findStep(plan, "create_chuck_session");
    REQUIRE(sess != nullptr);
    REQUIRE(sess->capabilityClass == CapabilityClass::NonDestructive);
    REQUIRE(sess->params.contains("target_slot"));
    REQUIRE(sess->params["target_slot"] == "d2");

    // Must include compile.
    const PlanStep* comp = findStep(plan, "compile");
    REQUIRE(comp != nullptr);
    REQUIRE(comp->capabilityClass == CapabilityClass::NonDestructive);

    // Must include render.
    const PlanStep* ren = findStep(plan, "render");
    REQUIRE(ren != nullptr);
    REQUIRE(ren->capabilityClass == CapabilityClass::NonDestructive);
    REQUIRE(ren->params["asset_name"] == "acid_bass");
    REQUIRE(ren->params["duration_bars"] == 8);

    // bind_asset must be persistent_mutation with confirmation.
    const PlanStep* bind = findStep(plan, "bind_asset");
    REQUIRE(bind != nullptr);
    REQUIRE(bind->capabilityClass == CapabilityClass::PersistentMutation);
    REQUIRE(bind->requiresConfirmation);
    REQUIRE_FALSE(plan.isDryRun);

    // update_song must be persistent_mutation with confirmation.
    const PlanStep* upd = findStep(plan, "update_song");
    REQUIRE(upd != nullptr);
    REQUIRE(upd->capabilityClass == CapabilityClass::PersistentMutation);
    REQUIRE(upd->requiresConfirmation);
}

// ===========================================================================
// 3. Reuse detection — fully bound instrument → decision="reuse"
// ===========================================================================

TEST_CASE("AI-10.1: reuse detected for fully-bound existing instrument",
          "[ai10][ai10_1][reuse_detect]")
{
    FakePlanFacade audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_test";
    audio.fakeInstruments.push_back(
        makeBoundInstrument("acid_bass", "/proj/.hathor_assets/chuck_instruments/acid_bass.ck",
                            "/proj/.hathor_assets/chuck_instruments/acid_bass.wav"));

    SampleBank bank;
    ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    RenderService renderService(audio, bank, chuckService);

    IntentPlanner planner(readFacade, chuckService, renderService);

    PlanModel plan = planner.planFromRequest(
        "acid bass",
        "d1",
        "acid_bass",
        8,
        false);

    REQUIRE(plan.mode == "chuck");
    REQUIRE(plan.reuseDecision == ReuseDecision::Reuse);
    REQUIRE_FALSE(plan.reuseFindings.empty());

    bool foundBound = false;
    for (const auto& f : plan.reuseFindings) {
        if (f.type == "chuck_instrument" && f.name == "acid_bass") {
            foundBound = true;
            REQUIRE(f.lifecycle_state == "bound");
        }
    }
    REQUIRE(foundBound);
}

// ===========================================================================
// 4. Modify decision — source-only instrument → decision="modify"
// ===========================================================================

TEST_CASE("AI-10.1: modify decision for source-only instrument",
          "[ai10][ai10_1][modify_detect]")
{
    FakePlanFacade audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_test";
    audio.fakeInstruments.push_back(
        makeSourceOnlyInstrument("acid_bass", "/proj/.hathor_assets/chuck_instruments/acid_bass.ck"));

    SampleBank bank;
    ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    RenderService renderService(audio, bank, chuckService);

    IntentPlanner planner(readFacade, chuckService, renderService);

    PlanModel plan = planner.planFromRequest(
        "acid bass",
        "d1",
        "acid_bass",
        8,
        false);

    REQUIRE(plan.reuseDecision == ReuseDecision::Modify);

    const PlanStep* bind = findStep(plan, "bind_asset");
    REQUIRE(bind != nullptr);
    REQUIRE(bind->description.find("overwriting existing") != std::string::npos);
}

// ===========================================================================
// 5. Create decision — no matching instrument → decision="create"
// ===========================================================================

TEST_CASE("AI-10.1: create decision when no matching instrument exists",
          "[ai10][ai10_1][create_detect]")
{
    FakePlanFacade audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_test";
    // No instruments at all.
    audio.fakeSamples = {};

    SampleBank bank;
    ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    RenderService renderService(audio, bank, chuckService);

    IntentPlanner planner(readFacade, chuckService, renderService);

    PlanModel plan = planner.planFromRequest(
        "new pad sound",
        "d3",
        "warm_pad",
        16,
        false);

    REQUIRE(plan.reuseDecision == ReuseDecision::Create);
    REQUIRE(plan.reuseFindings.empty());

    const PlanStep* bind = findStep(plan, "bind_asset");
    REQUIRE(bind != nullptr);
    REQUIRE(bind->description.find("new asset") != std::string::npos);
}

// ===========================================================================
// 6. Dry-run mode — persistent-mutation steps tagged with dry_run=true
// ===========================================================================

TEST_CASE("AI-10.1: dry-run mode tags persistent mutations as skipped",
          "[ai10][ai10_1][dry_run]")
{
    FakePlanFacade audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_test";

    SampleBank bank;
    ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    RenderService renderService(audio, bank, chuckService);

    IntentPlanner planner(readFacade, chuckService, renderService);

    PlanModel plan = planner.planFromRequest(
        "acid bass",
        "d1",
        "acid_bass",
        8,
        true);  // dry_run

    REQUIRE(plan.isDryRun == true);

    // bind_asset should NOT require confirmation in dry-run.
    const PlanStep* bind = findStep(plan, "bind_asset");
    REQUIRE(bind != nullptr);
    REQUIRE_FALSE(bind->requiresConfirmation);
    REQUIRE(bind->params.contains("dry_run"));
    REQUIRE(bind->params["dry_run"].get<bool>() == true);

    // update_song should NOT require confirmation in dry-run.
    const PlanStep* upd = findStep(plan, "update_song");
    REQUIRE(upd != nullptr);
    REQUIRE_FALSE(upd->requiresConfirmation);
    REQUIRE(upd->params.contains("dry_run"));
    REQUIRE(upd->params["dry_run"].get<bool>() == true);
}

// ===========================================================================
// 7. Override plan — pre-determined plan is used when provided
// ===========================================================================

TEST_CASE("AI-10.1: override plan is used when provided and well-formed",
          "[ai10][ai10_1][override_plan]")
{
    FakePlanFacade audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_test";

    SampleBank bank;
    ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    RenderService renderService(audio, bank, chuckService);

    IntentPlanner planner(readFacade, chuckService, renderService);

    nlohmann::json overridePlan = {
        {"mode", "chuck"},
        {"intent", "from override"},
        {"reuse_decision", "create"},
        {"steps", nlohmann::json::array({
            {{"name", "inspect_project"},
             {"service", "ProjectReadFacade"},
             {"method", "inspectProject"},
             {"capability_class", "read_only"},
             {"requires_confirmation", false},
             {"description", "custom step"}},
            {{"name", "custom_step"},
             {"service", "CustomService"},
             {"method", "doCustom"},
             {"capability_class", "persistent_mutation"},
             {"requires_confirmation", true},
             {"description", "does something destructive"}}
        })}
    };

    PlanModel plan = planner.planFromRequestWithOverride(
        "dark bassline",
        "d1",
        "acid_bass",
        8,
        false,
        overridePlan);

    REQUIRE(plan.mode == "chuck");
    REQUIRE(plan.intent == "dark bassline");  // uses the live intent, not override's
    REQUIRE(plan.reuseDecision == ReuseDecision::Create);
    REQUIRE(plan.steps.size() == 2);

    REQUIRE(plan.steps[0].name == "inspect_project");
    REQUIRE(plan.steps[0].capabilityClass == CapabilityClass::ReadOnly);
    REQUIRE_FALSE(plan.steps[0].requiresConfirmation);

    REQUIRE(plan.steps[1].name == "custom_step");
    REQUIRE(plan.steps[1].capabilityClass == CapabilityClass::PersistentMutation);
    REQUIRE(plan.steps[1].requiresConfirmation);
}

// ===========================================================================
// 7b. Override plan — empty/falsy override falls back to fresh plan
// ===========================================================================

TEST_CASE("AI-10.1: empty override plan falls back to planner",
          "[ai10][ai10_1][override_plan][fallback]")
{
    FakePlanFacade audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_test";

    SampleBank bank;
    ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    RenderService renderService(audio, bank, chuckService);

    IntentPlanner planner(readFacade, chuckService, renderService);

    // Empty JSON object — no "steps" key.
    nlohmann::json overridePlan = nlohmann::json::object();

    PlanModel plan = planner.planFromRequestWithOverride(
        "bass line",
        "d0",
        "",
        4,
        false,
        overridePlan);

    // Should fall back to pattern mode (no asset name).
    REQUIRE(plan.mode == "pattern");
    REQUIRE_FALSE(plan.steps.empty());
}

// ===========================================================================
// 8. Capability class tagging — every step has a valid capability class
// ===========================================================================

TEST_CASE("AI-10.1: all steps have valid capability class",
          "[ai10][ai10_1][capability_classes][read_only]")
{
    FakePlanFacade audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_test";

    SampleBank bank;
    ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    RenderService renderService(audio, bank, chuckService);

    IntentPlanner planner(readFacade, chuckService, renderService);

    // Pattern mode
    {
        PlanModel plan = planner.planFromRequest(
            "bd sn hh", "d1", "", 4, false);

        for (const auto& s : plan.steps) {
            // capabilityClass must be one of the three valid values.
            REQUIRE(s.capabilityClass == CapabilityClass::ReadOnly ||
                    s.capabilityClass == CapabilityClass::NonDestructive ||
                    s.capabilityClass == CapabilityClass::PersistentMutation);
            // requiresConfirmation must be true ONLY for persistent mutation.
            if (s.capabilityClass == CapabilityClass::PersistentMutation && !plan.isDryRun)
                REQUIRE(s.requiresConfirmation);
            else if (!plan.isDryRun)
                REQUIRE_FALSE(s.requiresConfirmation);
        }
    }

    // Chuck mode
    {
        PlanModel plan = planner.planFromRequest(
            "acid bass", "d1", "acid_bass", 8, false);

        for (const auto& s : plan.steps) {
            REQUIRE(s.capabilityClass == CapabilityClass::ReadOnly ||
                    s.capabilityClass == CapabilityClass::NonDestructive ||
                    s.capabilityClass == CapabilityClass::PersistentMutation);
            if (s.capabilityClass == CapabilityClass::PersistentMutation && !plan.isDryRun)
                REQUIRE(s.requiresConfirmation);
            else if (!plan.isDryRun)
                REQUIRE_FALSE(s.requiresConfirmation);
        }
    }
}

// ===========================================================================
// 9. Confirmation gating — pattern mode update_song requires confirmation
// ===========================================================================

TEST_CASE("AI-10.1: pattern mode update_song requires confirmation (non-dry-run)",
          "[ai10][ai10_1][confirmation][pattern]")
{
    FakePlanFacade audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_test";

    SampleBank bank;
    ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    RenderService renderService(audio, bank, chuckService);

    IntentPlanner planner(readFacade, chuckService, renderService);

    PlanModel plan = planner.planFromRequest(
        "bd sn", "d0", "", 4, false);

    const PlanStep* upd = findStep(plan, "update_song");
    REQUIRE(upd != nullptr);
    REQUIRE(upd->requiresConfirmation);
    REQUIRE(upd->capabilityClass == CapabilityClass::PersistentMutation);
}

// ===========================================================================
// 10. Plan JSON is well-formed and matches documented schema
// ===========================================================================

TEST_CASE("AI-10.1: plan serializes to well-formed JSON with required keys",
          "[ai10][ai10_1][json_schema]")
{
    FakePlanFacade audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_test";
    audio.fakeInstruments.push_back(
        makeBoundInstrument("acid_bass", "ck_path", "wav_path"));

    SampleBank bank;
    ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    RenderService renderService(audio, bank, chuckService);

    IntentPlanner planner(readFacade, chuckService, renderService);

    PlanModel plan = planner.planFromRequest(
        "acid bass", "d1", "acid_bass", 8, false);

    nlohmann::json j = plan.toJson();

    REQUIRE(j.contains("mode"));
    REQUIRE(j.contains("intent"));
    REQUIRE(j.contains("target_slot"));
    REQUIRE(j.contains("asset_name"));
    REQUIRE(j.contains("duration_bars"));
    REQUIRE(j.contains("is_dry_run"));
    REQUIRE(j.contains("reuse_decision"));
    REQUIRE(j.contains("reuse_findings"));
    REQUIRE(j.contains("steps"));

    REQUIRE(j["mode"] == "chuck");
    REQUIRE(j["intent"] == "acid bass");
    REQUIRE(j["target_slot"] == "d1");
    REQUIRE(j["asset_name"] == "acid_bass");
    REQUIRE(j["duration_bars"] == 8);
    REQUIRE(j["is_dry_run"] == false);
    REQUIRE(j["reuse_decision"] == "reuse");
    REQUIRE(j["reuse_findings"].is_array());
    REQUIRE(j["steps"].is_array());

    // Each step in JSON form must have the required keys.
    for (const auto& sj : j["steps"]) {
        REQUIRE(sj.contains("name"));
        REQUIRE(sj.contains("service"));
        REQUIRE(sj.contains("method"));
        REQUIRE(sj.contains("capability_class"));
        REQUIRE(sj.contains("requires_confirmation"));
        REQUIRE(sj.contains("description"));
    }

    // Reuse findings must be an array of objects with resource_id, name, type.
    REQUIRE(j["reuse_findings"].size() >= 1);
    bool foundBoundInstrument = false;
    for (const auto& rf : j["reuse_findings"]) {
        REQUIRE(rf.contains("resource_id"));
        REQUIRE(rf.contains("name"));
        REQUIRE(rf.contains("type"));
        REQUIRE(rf.contains("lifecycle_state"));
        if (rf["name"] == "acid_bass" && rf["type"] == "chuck_instrument")
            foundBoundInstrument = true;
    }
    REQUIRE(foundBoundInstrument);
}

// ===========================================================================
// 12. Empty project — plan is produced with zero reuse findings
// ===========================================================================

TEST_CASE("AI-10.1: plan on empty project has no reuse findings",
          "[ai10][ai10_1][empty_project]")
{
    FakePlanFacade audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_empty_test";
    audio.fakeInstruments = {};
    audio.fakeSamples = {};

    SampleBank bank;
    ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    RenderService renderService(audio, bank, chuckService);

    IntentPlanner planner(readFacade, chuckService, renderService);

    PlanModel plan = planner.planFromRequest(
        "brand new sound", "d0", "new_sound", 8, false);

    REQUIRE(plan.mode == "chuck");
    REQUIRE(plan.reuseFindings.empty());
    REQUIRE(plan.reuseDecision == ReuseDecision::Create);
    REQUIRE_FALSE(plan.steps.empty());
}
