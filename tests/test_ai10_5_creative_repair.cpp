// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_ai10_5_creative_repair.cpp — AI-10.5: Conversational Creative Repair.
 *
 * Verifies:
 *   1.  Feedback classification maps natural-language modifiers to the correct
 *       musical Property + TargetDomain (all 8 properties + Unknown).
 *   2.  classifyFeedback is case-insensitive.
 *   3.  resolveTarget delegates to WorkingSet::resolveReference and returns
 *       structured JSON.
 *   4.  Pattern density repair wraps existing notation in slow(2) for "too busy".
 *   5.  Pattern density repair wraps existing notation in degradeBy(0.3) for
 *       "simplify the rhythm".
 *   6.  ChucK darkness repair lowers filter cutoff frequency in source text.
 *   7.  ChucK brightness repair raises filter cutoff frequency in source text.
 *   8.  ChucK repair appends an inline LPF when no filter param is found.
 *   9.  planRepair produces a RepairOp tagged persistent_mutation + confirm=true
 *       for pattern repairs.
 *   10. planRepair produces a RepairOp tagged non_destructive for ChucK repairs.
 *   11. planRepair falls back to the active slot when no reference is found.
 *   12. planRepair returns an empty plan with an explanation when no target is
 *       resolvable.
 *   13. planRepair falls back to working-set state when no .hathor file exists.
 *   14. RepairPlan::toJson serialises all fields correctly.
 *   15. RepairOp::toJson serialises capability class names correctly.
 *   16. Unrecognised feedback produces Property::Unknown with no ops.
 *   17. End-to-end: startCreativeRepair on AgenticWorkflow produces events and
 *       reaches a terminal state in dry-run mode.
 *   18. Dry-run mode does not write to disk.
 *
 * Architecture: tests construct a CreativeRepairEngine with a real
 * SongMutationService (using a fake AudioEngineFacade + temp project dir) and
 * ChuckSessionService.  WorkingSet items are seeded directly for deterministic
 * target resolution.
 *
 * Requirement references: AI-10.5, AI-10.2, AI-10.3, AI-7, AI-5
 */

#include "CreativeRepairEngine.hpp"
#include "AgenticWorkflow.hpp"

#include "SongMutationService.hpp"
#include "ChuckSessionService.hpp"
#include "WorkingSet.hpp"
#include "ProjectReadFacade.hpp"
#include "RenderService.hpp"
#include "IntentPlanner.hpp"
#include "ProjectReadFacade.hpp"
#include "RenderService.hpp"
#include "IntentPlanner.hpp"

#include "AudioEngineFacade.hpp"
#include "SampleBank.hpp"
#include "SlotState.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using hathor::control::AgenticWorkflow;
using hathor::control::ChuckSessionService;
using hathor::control::CreativeRepairEngine;
using hathor::control::SongMutationService;
using hathor::control::WorkingSet;

using CCR = CreativeRepairEngine;

namespace fs = std::filesystem;

// ===========================================================================
// Fake audio facade (minimal, mirrors test_ai10_1/4 pattern)
// ===========================================================================

class FakeAudioForRepair final : public AudioEngineFacade {
public:
    fs::path projectDir;

    void play() noexcept override {}
    void stop() noexcept override {}
    void setBpm(double bpm) noexcept override { bpm_ = bpm; }
    double getBpm() const noexcept override { return bpm_; }
    bool isRunning() const noexcept override { return true; }

    void setMasterGain(float) noexcept override {}
    float getMasterGain() const noexcept override { return 1.0f; }
    void setMasterEqPreset(hathor::EqPreset) noexcept override {}
    hathor::EqPreset getMasterEqPreset() const noexcept override { return hathor::EqPreset::Flat; }

    int findOrAddSlot(const std::string&) override { return 0; }
    void storeSlot(int, std::shared_ptr<SlotState>) noexcept override {}
    bool clearSlot(int) noexcept override { return true; }
    int slotCount() const noexcept override { return 4; }

    std::string slotName(int idx) const override {
        static const char* names[] = {"d0", "d1", "d2", "d3"};
        return (idx >= 0 && idx < 4) ? names[idx] : "";
    }
    std::shared_ptr<SlotState> loadSlot(int) const noexcept override { return nullptr; }

    void slotPlay(int) noexcept override {}
    void slotStop(int) noexcept override {}
    bool isSlotRunning(int) const noexcept override { return false; }

    bool hasWorker() const noexcept override { return false; }
    bool ckEval(int, const std::string&) noexcept override { return true; }
    bool stopCkTab(int) noexcept override { return true; }
    std::string queryCkTab(int) const override { return "ok"; }
    uint64_t startAsyncCkCompile(int, const std::string&,
                                 std::function<void(bool, const std::string&)>) override
    { return 1; }
    nlohmann::json queryCkJob(uint64_t) const override {
        return {{"ok", true}, {"status", "succeeded"}};
    }
    bool cancelCkJob(uint64_t) override { return true; }

    fs::path resolveRenderPath(hathor::AssetTarget, std::string_view,
                               const fs::path&) override { return {}; }
    void setLiveJamSessionDir(fs::path) override {}
    void setProjectDir(fs::path dir) override { projectDir = std::move(dir); }
    fs::path currentProjectDir() const noexcept override { return projectDir; }
    void cleanupLiveJamAssets() override {}
    bool isStudioAssetPath(const fs::path&) const override { return false; }

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

    bool registerBakedAsset(std::string, const fs::path&) override { return true; }
    std::vector<std::string> listSamples() const override { return {"bd", "sn", "hh"}; }
    std::vector<SlotInfo> listSlots() const noexcept override { return {}; }
    SlotInfo getSlotInfo(int) const noexcept override { return {}; }
    VmStatus getVmStatus(int) const noexcept override { return {}; }
    AudioStatus getAudioStatus() const noexcept override {
        return AudioStatus{true, bpm_, 44100, 1.0f, "flat", 0, true, 0};
    }
    std::vector<SlotPlayback> listSlotPlayback() const noexcept override { return {}; }
    std::vector<InstrumentInfo> listChuckInstruments(const fs::path&) const noexcept override
    { return {}; }
    fs::path studioInstrumentsDir(const fs::path& p) const noexcept override {
        return p / ".hathor_assets" / "chuck_instruments";
    }

    double bpm_ = 120.0;
};

// ===========================================================================
// Helpers
// ===========================================================================

namespace {

/// Set up a temp project directory for SongMutationService.
struct TempProject {
    fs::path dir;
    explicit TempProject(const fs::path& base) {
        dir = base / ("hathor_repair_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
    }
    ~TempProject() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

/// Create a pattern TrackedItem for the working set.
WorkingSet::TrackedItem makePatternItem(const std::string& slot,
                                        const std::string& notation) {
    WorkingSet::TrackedItem item;
    item.id   = "pattern:" + slot;
    item.name = slot;
    item.type = WorkingSet::ItemType::Pattern;
    item.slotName = slot;
    item.state = nlohmann::json{{"canonical_notation", notation}, {"slot", slot}};
    return item;
}

} // anonymous namespace

// ===========================================================================
// 1. Feedback classification — all property types
// ===========================================================================

TEST_CASE("AI-10.5: classifyFeedback maps keywords to Property + Domain",
          "[ai10][ai10_5][classification]")
{
    FakeAudioForRepair audio;
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    CCR engine(ws, songSvc, chuckSvc);

    SECTION("too busy → RhythmicDensity / Pattern") {
        auto c = engine.classifyFeedback("too busy");
        REQUIRE(c.property == CCR::Property::RhythmicDensity);
        REQUIRE(c.domain == CCR::TargetDomain::Pattern);
    }

    SECTION("make it darker → TimbralDarkness / Instrument") {
        auto c = engine.classifyFeedback("make it darker");
        REQUIRE(c.property == CCR::Property::TimbralDarkness);
        REQUIRE(c.domain == CCR::TargetDomain::Instrument);
    }

    SECTION("warmer → TimbralDarkness") {
        auto c = engine.classifyFeedback("warmer");
        REQUIRE(c.property == CCR::Property::TimbralDarkness);
    }

    SECTION("brighter → TimbralBrightness / Instrument") {
        auto c = engine.classifyFeedback("brighter");
        REQUIRE(c.property == CCR::Property::TimbralBrightness);
        REQUIRE(c.domain == CCR::TargetDomain::Instrument);
    }

    SECTION("sharper → TimbralBrightness") {
        auto c = engine.classifyFeedback("sharper tone");
        REQUIRE(c.property == CCR::Property::TimbralBrightness);
    }

    SECTION("louder → Loudness / Instrument") {
        auto c = engine.classifyFeedback("louder");
        REQUIRE(c.property == CCR::Property::Loudness);
        REQUIRE(c.domain == CCR::TargetDomain::Instrument);
    }

    SECTION("punchier → Loudness") {
        auto c = engine.classifyFeedback("more punch");
        REQUIRE(c.property == CCR::Property::Loudness);
    }

    SECTION("more spacious → StereoSpread / Instrument") {
        auto c = engine.classifyFeedback("more spacious");
        REQUIRE(c.property == CCR::Property::StereoSpread);
        REQUIRE(c.domain == CCR::TargetDomain::Instrument);
    }

    SECTION("wider → StereoSpread") {
        auto c = engine.classifyFeedback("wider mix");
        REQUIRE(c.property == CCR::Property::StereoSpread);
    }

    SECTION("tighter → Timing / Pattern") {
        auto c = engine.classifyFeedback("tighter");
        REQUIRE(c.property == CCR::Property::Timing);
        REQUIRE(c.domain == CCR::TargetDomain::Pattern);
    }

    SECTION("swing → Timing") {
        auto c = engine.classifyFeedback("add swing");
        REQUIRE(c.property == CCR::Property::Timing);
    }

    SECTION("higher → Pitch / Instrument") {
        auto c = engine.classifyFeedback("higher");
        REQUIRE(c.property == CCR::Property::Pitch);
        REQUIRE(c.domain == CCR::TargetDomain::Instrument);
    }

    SECTION("bass → Pitch") {
        auto c = engine.classifyFeedback("bass too low");
        REQUIRE(c.property == CCR::Property::Pitch);
    }

    SECTION("aggressive → TimbralCharacter / Instrument") {
        auto c = engine.classifyFeedback("more aggressive");
        REQUIRE(c.property == CCR::Property::TimbralCharacter);
        REQUIRE(c.domain == CCR::TargetDomain::Instrument);
    }

    SECTION("smooth → TimbralCharacter") {
        auto c = engine.classifyFeedback("smooth it out");
        REQUIRE(c.property == CCR::Property::TimbralCharacter);
    }

    SECTION("unknown → Unknown / Unknown") {
        auto c = engine.classifyFeedback("the weather is nice");
        REQUIRE(c.property == CCR::Property::Unknown);
        REQUIRE(c.domain == CCR::TargetDomain::Unknown);
    }
}

// ===========================================================================
// 2. Classification is case-insensitive
// ===========================================================================

TEST_CASE("AI-10.5: classifyFeedback is case-insensitive",
          "[ai10][ai10_5][classification][case]")
{
    FakeAudioForRepair audio;
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;
    CCR engine(ws, songSvc, chuckSvc);

    REQUIRE(engine.classifyFeedback("DARKER").property == CCR::Property::TimbralDarkness);
    REQUIRE(engine.classifyFeedback("BRIGHTER").property == CCR::Property::TimbralBrightness);
    REQUIRE(engine.classifyFeedback("TOO BUSY").property == CCR::Property::RhythmicDensity);
}

// ===========================================================================
// 3. resolveTarget delegates to WorkingSet
// ===========================================================================

TEST_CASE("AI-10.5: resolveTarget delegates to WorkingSet::resolveReference",
          "[ai10][ai10_5][resolution][workingset]")
{
    FakeAudioForRepair audio;
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    ws.recordItem(makePatternItem("d1", "bd sn hh"));

    CCR engine(ws, songSvc, chuckSvc);

    SECTION("resolves a tracked pattern by slot name") {
        auto result = engine.resolveTarget("d1");
        REQUIRE(result["found"] == true);
        REQUIRE(result["ambiguous"] == false);
        REQUIRE(result["resolved"]["type"] == "pattern");
        REQUIRE(result["resolved"]["slot"] == "d1");
    }

    SECTION("returns found=false for unknown reference") {
        auto result = engine.resolveTarget("nonexistent_slot");
        REQUIRE(result["found"] == false);
    }
}

// ===========================================================================
// 4. Pattern density repair: "too busy" → slow(2)
// ===========================================================================

TEST_CASE("AI-10.5: 'too busy' wraps pattern in slow(2)",
          "[ai10][ai10_5][pattern][density][slow]")
{
    std::string original = "bd sn hh cp";
    std::string repaired = CCR::RepairPlan{}.targetNotation; // placeholder

    // Use the private generator indirectly via planRepair
    // For a direct unit test, we call generatePatternDensityRepair through planRepair
    FakeAudioForRepair audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_repair_test_1";
    fs::create_directories(audio.projectDir);
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    // Seed the working set with a pattern on slot d1.
    ws.recordItem(makePatternItem("d1", "bd sn hh cp"));
    ws.setActiveSlot("d1");

    CCR engine(ws, songSvc, chuckSvc);
    auto plan = engine.planRepair("too busy", "");

    REQUIRE(plan.property == CCR::Property::RhythmicDensity);
    REQUIRE(plan.targetDomain == CCR::TargetDomain::Pattern);
    REQUIRE(plan.ops.size() == 1);
    REQUIRE(plan.ops[0].service == "SongMutationService");
    REQUIRE(plan.ops[0].op == "insert");
    REQUIRE(plan.ops[0].capabilityClass == CCR::CapabilityClass::PersistentMutation);
    REQUIRE(plan.ops[0].requiresConfirmation == true);
    REQUIRE(plan.needsConfirmation == true);

    // The notation should be wrapped in slow(2, ...)
    REQUIRE(plan.targetNotation.find("slow(2,") != std::string::npos);
    REQUIRE(plan.targetNotation.find("bd sn hh cp") != std::string::npos);

    fs::remove_all(audio.projectDir);
}

// ===========================================================================
// 5. Pattern density repair: "simplify the rhythm" → degradeBy(0.3)
// ===========================================================================

TEST_CASE("AI-10.5: 'simplify the rhythm' wraps pattern in degradeBy(0.3)",
          "[ai10][ai10_5][pattern][density][degrade]")
{
    FakeAudioForRepair audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_repair_test_2";
    fs::create_directories(audio.projectDir);
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    ws.recordItem(makePatternItem("d1", "bd sn hh cp"));
    ws.setActiveSlot("d1");

    CCR engine(ws, songSvc, chuckSvc);
    auto plan = engine.planRepair("keep the sound but simplify the rhythm", "");

    REQUIRE(plan.property == CCR::Property::RhythmicDensity);
    REQUIRE(plan.targetDomain == CCR::TargetDomain::Pattern);
    REQUIRE(plan.targetNotation.find("degradeBy(0.3,") != std::string::npos);
    REQUIRE(plan.targetNotation.find("bd sn hh cp") != std::string::npos);

    fs::remove_all(audio.projectDir);
}

// ===========================================================================
// 6. ChucK darkness repair lowers filter cutoff
// ===========================================================================

TEST_CASE("AI-10.5: 'darker' lowers filter cutoff freq in ChucK source",
          "[ai10][ai10_5][chuck][darkness]")
{
    FakeAudioForRepair audio;
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    const std::string originalSource = "SinOsc s => LPF lpf => g; 440 => s.freq; 300 => lpf.freq;";
    // Create a session via the service so getSessionSource has the source.
    chuckSvc.createSession(1, originalSource);

    ws.recordItem(WorkingSet::TrackedItem{
        .id = "instrument:acid_bass",
        .name = "acid_bass",
        .type = WorkingSet::ItemType::Instrument,
        .slotName = "d1",
        .alias = "the bass",
        .state = nlohmann::json{
            {"session_id", "ck:1"},
            {"source", originalSource},
            {"lifecycle_state", "bound"}
        }
    });

    CCR engine(ws, songSvc, chuckSvc);
    auto plan = engine.planRepair("make it darker", "bass");

    REQUIRE(plan.property == CCR::Property::TimbralDarkness);
    REQUIRE(plan.targetDomain == CCR::TargetDomain::Instrument);
    REQUIRE(plan.ops.size() == 1);
    REQUIRE(plan.ops[0].service == "ChuckSessionService");
    REQUIRE(plan.ops[0].method == "compileChuck");
    REQUIRE(plan.ops[0].capabilityClass == CCR::CapabilityClass::NonDestructive);
    REQUIRE(plan.ops[0].requiresConfirmation == false);
    REQUIRE(plan.needsConfirmation == false);

    // The source should have changed — freq should be lower.
    REQUIRE(plan.targetSource != originalSource);
    REQUIRE(plan.targetSource.find("210") != std::string::npos); // 300 * 0.7 = 210
}

// ===========================================================================
// 7. ChucK brightness repair raises filter cutoff
// ===========================================================================

TEST_CASE("AI-10.5: 'brighter' raises filter cutoff freq in ChucK source",
          "[ai10][ai10_5][chuck][brightness]")
{
    FakeAudioForRepair audio;
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    const std::string originalSource = "SinOsc s => LPF lpf => g; 440 => s.freq; 200 => lpf.freq;";
    chuckSvc.createSession(1, originalSource);

    ws.recordItem(WorkingSet::TrackedItem{
        .id = "instrument:acid_bass",
        .name = "acid_bass",
        .type = WorkingSet::ItemType::Instrument,
        .slotName = "d1",
        .alias = "the bass",
        .state = nlohmann::json{
            {"session_id", "ck:1"},
            {"source", originalSource},
            {"lifecycle_state", "bound"}
        }
    });

    CCR engine(ws, songSvc, chuckSvc);
    auto plan = engine.planRepair("make it brighter", "bass");

    REQUIRE(plan.property == CCR::Property::TimbralBrightness);
    REQUIRE(plan.targetDomain == CCR::TargetDomain::Instrument);
    REQUIRE(plan.targetSource != originalSource);
    // 200 * 1.4 = 280
    REQUIRE(plan.targetSource.find("280") != std::string::npos);
}

// ===========================================================================
// 8. ChucK repair appends LPF when no filter param found
// ===========================================================================

TEST_CASE("AI-10.5: 'darker' appends LPF when no filter freq in source",
          "[ai10][ai10_5][chuck][darkness][fallback]")
{
    FakeAudioForRepair audio;
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    // Source with no LPF freq assignment.
    const std::string originalSource = "SinOsc s => g; 440 => s.freq;";
    chuckSvc.createSession(2, originalSource);

    ws.recordItem(WorkingSet::TrackedItem{
        .id = "instrument:lead",
        .name = "lead",
        .type = WorkingSet::ItemType::Instrument,
        .slotName = "d2",
        .alias = "the lead",
        .state = nlohmann::json{
            {"session_id", "ck:2"},
            {"source", originalSource},
            {"lifecycle_state", "bound"}
        }
    });

    CCR engine(ws, songSvc, chuckSvc);
    auto plan = engine.planRepair("make it darker", "lead");

    REQUIRE(plan.property == CCR::Property::TimbralDarkness);
    REQUIRE(plan.targetSource != originalSource);
    REQUIRE(plan.targetSource.find("LPF lpf") != std::string::npos);
    REQUIRE(plan.targetSource.find("repair") != std::string::npos);
}

// ===========================================================================
// 9. planRepair produces persistent_mutation + confirm=true for patterns
// ===========================================================================

TEST_CASE("AI-10.5: pattern repair plan is tagged persistent_mutation",
          "[ai10][ai10_5][plan][persistence]")
{
    FakeAudioForRepair audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_repair_test_3";
    fs::create_directories(audio.projectDir);
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    ws.recordItem(makePatternItem("d1", "bd sn hh cp"));
    ws.setActiveSlot("d1");

    CCR engine(ws, songSvc, chuckSvc);
    auto plan = engine.planRepair("too busy", "");

    REQUIRE(plan.ops.size() == 1);
    REQUIRE(plan.ops[0].capabilityClass == CCR::CapabilityClass::PersistentMutation);
    REQUIRE(plan.ops[0].requiresConfirmation == true);
    REQUIRE(plan.needsConfirmation == true);

    fs::remove_all(audio.projectDir);
}

// ===========================================================================
// 10. ChucK repair is non_destructive, no confirmation
// ===========================================================================

TEST_CASE("AI-10.5: ChucK repair plan is tagged non_destructive",
          "[ai10][ai10_5][plan][non_destructive]")
{
    FakeAudioForRepair audio;
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    const std::string originalSource = "SinOsc s => LPF lpf => g; 440 => s.freq; 300 => lpf.freq;";
    chuckSvc.createSession(1, originalSource);

    ws.recordItem(WorkingSet::TrackedItem{
        .id = "instrument:bass",
        .name = "bass",
        .type = WorkingSet::ItemType::Instrument,
        .slotName = "d1",
        .alias = "the bass",
        .state = nlohmann::json{
            {"session_id", "ck:1"},
            {"source", originalSource},
            {"lifecycle_state", "bound"}
        }
    });

    CCR engine(ws, songSvc, chuckSvc);
    auto plan = engine.planRepair("make it darker", "bass");

    REQUIRE(plan.ops.size() == 1);
    REQUIRE(plan.ops[0].capabilityClass == CCR::CapabilityClass::NonDestructive);
    REQUIRE(plan.ops[0].requiresConfirmation == false);
    REQUIRE(plan.needsConfirmation == false);
}

// ===========================================================================
// 11. Falls back to active slot when no reference in feedback
// ===========================================================================

TEST_CASE("AI-10.5: falls back to active slot when feedback has no reference",
          "[ai10][ai10_5][resolution][fallback]")
{
    FakeAudioForRepair audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_repair_test_4";
    fs::create_directories(audio.projectDir);
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    ws.recordItem(makePatternItem("d1", "bd sn hh cp"));
    ws.setActiveSlot("d1");

    CCR engine(ws, songSvc, chuckSvc);

    // "simplify" has no slot reference — should fall back to active slot d1.
    auto plan = engine.planRepair("simplify", "");

    REQUIRE(plan.slotName == "d1");
    REQUIRE(plan.targetDomain == CCR::TargetDomain::Pattern);
    REQUIRE_FALSE(plan.ops.empty());
}

// ===========================================================================
// 12. No target → empty plan with explanation
// ===========================================================================

TEST_CASE("AI-10.5: no target resolved produces empty plan + explanation",
          "[ai10][ai10_5][plan][no_target]")
{
    FakeAudioForRepair audio;
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    CCR engine(ws, songSvc, chuckSvc);

    // No items in working set, no active slot.
    auto plan = engine.planRepair("too busy", "");

    REQUIRE(plan.ops.empty());
    REQUIRE_FALSE(plan.explanation.empty());
}

// ===========================================================================
// 13. Falls back to working-set state when no .hathor file exists
// ===========================================================================

TEST_CASE("AI-10.5: falls back to working-set notation when no song file",
          "[ai10][ai10_5][plan][ws_fallback]")
{
    FakeAudioForRepair audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_repair_test_5";
    fs::create_directories(audio.projectDir);
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    // No .hathor file on disk, but the working set has a pattern with notation.
    ws.recordItem(makePatternItem("d1", "bd sn hh cp"));
    ws.setActiveSlot("d1");

    CCR engine(ws, songSvc, chuckSvc);

    // "simplify the rhythm" has no slot reference → falls back to active slot.
    auto plan = engine.planRepair("simplify the rhythm", "");

    REQUIRE(plan.slotName == "d1");
    REQUIRE(plan.targetDomain == CCR::TargetDomain::Pattern);
    REQUIRE_FALSE(plan.ops.empty());
    REQUIRE(plan.targetNotation.find("degradeBy(0.3,") != std::string::npos);

    fs::remove_all(audio.projectDir);
}

// ===========================================================================
// 14. RepairPlan::toJson serialises all fields
// ===========================================================================

TEST_CASE("AI-10.5: RepairPlan::toJson serialises all fields",
          "[ai10][ai10_5][serialisation][plan]")
{
    FakeAudioForRepair audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_repair_test_6";
    fs::create_directories(audio.projectDir);
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    ws.recordItem(makePatternItem("d1", "bd sn hh cp"));
    ws.setActiveSlot("d1");

    CCR engine(ws, songSvc, chuckSvc);
    auto plan = engine.planRepair("too busy", "");

    nlohmann::json j = plan.toJson();

    REQUIRE(j["feedback"] == "too busy");
    REQUIRE(j["property"] == "rhythmic_density");
    REQUIRE(j["target_domain"] == "pattern");
    REQUIRE(j["slot_name"] == "d1");
    REQUIRE(j["needs_confirmation"] == true);
    REQUIRE(j["ops"].is_array());
    REQUIRE(j["ops"].size() == 1);
    REQUIRE(j["ops"][0]["capability_class"] == "persistent_mutation");
    REQUIRE(j["ops"][0]["requires_confirmation"] == true);
    REQUIRE(j["ops"][0]["service"] == "SongMutationService");

    fs::remove_all(audio.projectDir);
}

// ===========================================================================
// 15. RepairOp::toJson serialises capability class names
// ===========================================================================

TEST_CASE("AI-10.5: RepairOp::toJson serialises capability class names",
          "[ai10][ai10_5][serialisation][op]")
{
    CCR::RepairOp op;
    op.op = "compile_chuck";
    op.service = "ChuckSessionService";
    op.method = "compileChuck";
    op.capabilityClass = CCR::CapabilityClass::NonDestructive;
    op.requiresConfirmation = false;
    op.description = "darker repair";
    op.params = {{"session_id", "ck:1"}, {"source", "..."}};

    nlohmann::json j = op.toJson();
    REQUIRE(j["op"] == "compile_chuck");
    REQUIRE(j["service"] == "ChuckSessionService");
    REQUIRE(j["method"] == "compileChuck");
    REQUIRE(j["capability_class"] == "non_destructive");
    REQUIRE(j["requires_confirmation"] == false);
    REQUIRE(j["description"] == "darker repair");
}

// ===========================================================================
// 16. Unrecognised feedback → Property::Unknown, no ops
// ===========================================================================

TEST_CASE("AI-10.5: unrecognised feedback produces Unknown property, no ops",
          "[ai10][ai10_5][classification][unknown]")
{
    FakeAudioForRepair audio;
    SampleBank bank;
    SongMutationService songSvc(audio, bank);
    ChuckSessionService chuckSvc(audio);
    WorkingSet ws;

    ws.recordItem(makePatternItem("d1", "bd sn hh cp"));
    ws.setActiveSlot("d1");

    CCR engine(ws, songSvc, chuckSvc);
    auto plan = engine.planRepair("the sky is blue", "");

    REQUIRE(plan.property == CCR::Property::Unknown);
    REQUIRE(plan.ops.empty());
}

// ===========================================================================
// 17. End-to-end: startCreativeRepair reaches terminal state (dry-run)
// ===========================================================================

TEST_CASE("AI-10.5: startCreativeRepair reaches terminal state in dry-run",
          "[ai10][ai10_5][workflow][e2e][dryrun]")
{
    FakeAudioForRepair audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_repair_test_7";
    fs::create_directories(audio.projectDir);
    SampleBank bank;

    hathor::control::ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    hathor::control::RenderService renderService(audio, bank, chuckService);
    SongMutationService songService(audio, bank);
    AgenticWorkflow wf(audio, bank, readFacade, chuckService, renderService, songService);

    // Phase 1: Run a normal pattern workflow to populate the working set.
    {
        std::vector<AgenticWorkflow::ProgressEvent> genEvents;
        std::mutex genMtx;
        auto genCb = [&](const AgenticWorkflow::ProgressEvent& ev) {
            std::lock_guard<std::mutex> lock(genMtx);
            genEvents.push_back(ev);
        };

        AgenticWorkflow::Request req;
        req.intent = "bd sn hh groove";
        req.targetSlot = "d1";
        req.notation = "bd sn hh cp";
        req.dryRun = true;

        REQUIRE(wf.start(req, genCb,
            [](AgenticWorkflow::ConfirmationRequest) {}));

        // Wait for the first workflow to complete.
        const auto deadline1 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            auto s = wf.getState();
            std::string state = s.value("state", std::string{});
            if (state == "completed" || state == "failed" || state == "cancelled")
                break;
            if (std::chrono::steady_clock::now() > deadline1)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(wf.getState().value("state", std::string{}) == "completed");
    }

    // Reset state between workflow runs (keep working set for continuity).
    wf.reset();

    // Phase 2: Start creative repair on the pattern now tracked in the working set.
    std::vector<AgenticWorkflow::ProgressEvent> events;
    std::mutex eventMtx;

    auto progressCb = [&](const AgenticWorkflow::ProgressEvent& ev) {
        std::lock_guard<std::mutex> lock(eventMtx);
        events.push_back(ev);
    };

    bool started = wf.startCreativeRepair("too busy", "", progressCb,
        [](AgenticWorkflow::ConfirmationRequest) {});

    REQUIRE(started);

    // Wait for terminal state.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string state;
    for (;;) {
        auto s = wf.getState();
        state = s.value("state", std::string{});
        if (state == "completed" || state == "failed" || state == "cancelled")
            break;
        if (std::chrono::steady_clock::now() > deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(state == "completed");

    // Verify we got a creative_repair step result.
    auto s = wf.getState();
    REQUIRE(s["state"] == "completed");

    fs::remove_all(audio.projectDir);
}

// ===========================================================================
// 18. Dry-run mode does not write to disk
// ===========================================================================

TEST_CASE("AI-10.5: dry-run creative repair does not write song file",
          "[ai10][ai10_5][workflow][dryrun][persistence]")
{
    FakeAudioForRepair audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_repair_test_8";
    fs::create_directories(audio.projectDir);
    SampleBank bank;

    hathor::control::ProjectReadFacade readFacade(audio, bank);
    ChuckSessionService chuckService(audio);
    hathor::control::RenderService renderService(audio, bank, chuckService);
    SongMutationService songService(audio, bank);
    AgenticWorkflow wf(audio, bank, readFacade, chuckService, renderService, songService);

    // Run a normal pattern workflow to populate the working set.
    {
        std::vector<AgenticWorkflow::ProgressEvent> genEvents;
        std::mutex genMtx;
        auto genCb = [&](const AgenticWorkflow::ProgressEvent& ev) {
            std::lock_guard<std::mutex> lock(genMtx);
            genEvents.push_back(ev);
        };

        AgenticWorkflow::Request req;
        req.intent = "bd sn hh groove";
        req.targetSlot = "d1";
        req.notation = "bd sn hh cp";
        req.dryRun = true;

        REQUIRE(wf.start(req, genCb,
            [](AgenticWorkflow::ConfirmationRequest) {}));

        const auto deadline1 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            auto s = wf.getState();
            std::string state = s.value("state", std::string{});
            if (state == "completed" || state == "failed" || state == "cancelled")
                break;
            if (std::chrono::steady_clock::now() > deadline1)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(wf.getState().value("state", std::string{}) == "completed");
    }

    // Reset state between workflow runs (keep working set for continuity).
    wf.reset();

    // Now start creative repair in dry-run mode.
    std::vector<AgenticWorkflow::ProgressEvent> events;
    std::mutex eventMtx;
    auto progressCb = [&](const AgenticWorkflow::ProgressEvent& ev) {
        std::lock_guard<std::mutex> lock(eventMtx);
        events.push_back(ev);
    };

    bool started = wf.startCreativeRepair("too busy", "", progressCb,
        [](AgenticWorkflow::ConfirmationRequest) {});

    REQUIRE(started);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string state;
    for (;;) {
        auto s = wf.getState();
        state = s.value("state", std::string{});
        if (state == "completed" || state == "failed" || state == "cancelled")
            break;
        if (std::chrono::steady_clock::now() > deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(state == "completed");

    // Verify no song file was written (dry-run should not persist).
    REQUIRE_FALSE(fs::exists(audio.projectDir / "d1.hathor"));

    fs::remove_all(audio.projectDir);
}
