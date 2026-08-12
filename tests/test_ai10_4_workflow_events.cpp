// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_ai10_4_workflow_events.cpp — AI-10.4: Observable progress/explanation
 * event stream through the AgenticWorkflow orchestration layer.
 *
 * Verifies:
 *   1. A workflow_start emits an initial WorkflowStarted event whose state
 *      snapshot reports "queued" (and the details carry queued=true).
 *   2. Every progress event carries a stable, non-zero workflow identity.
 *   3. The stream is ordered: WorkflowStarted precedes WorkflowCompleted.
 *   4. A completed run emits a WorkflowCompleted event with a completed-state
 *      snapshot.
 *   5. eventTypeName() maps every EventType to a stable string.
 *   6. start() is gated: a second start while a workflow is active fails.
 *   7. The workflow's change-set (AI-10.3) is exposed via getChangeSet() and
 *      reflects the completed run's intent.
 *
 * Architecture: builds the REAL AgenticWorkflow against a fake
 * AudioEngineFacade (FakePlanFacade4) + real ProjectReadFacade,
 * ChuckSessionService, RenderService, and SongMutationService — the same
 * JUCE-free pattern as test_ai10_1.  A pattern-mode DRY-RUN workflow is used
 * so the run completes deterministically (no confirmation pause, no file
 * writes) while still exercising the full event pipeline.
 *
 * Requirement references: AI-10.4, AI-10.3, AI-1 capability model
 */

#include "AgenticWorkflow.hpp"
#include "IntentPlanner.hpp"
#include "ProjectReadFacade.hpp"
#include "ChuckSessionService.hpp"
#include "RenderService.hpp"
#include "SongMutationService.hpp"

#include "AudioEngineFacade.hpp"
#include "SampleBank.hpp"
#include "SlotState.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using hathor::control::AgenticWorkflow;

namespace fs = std::filesystem;

// ===========================================================================
// FakePlanFacade4 — JUCE-free AudioEngineFacade (mirrors test_ai10_1)
// ===========================================================================

class FakePlanFacade4 final : public AudioEngineFacade {
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
    nlohmann::json queryCkJob(uint64_t) const override
    { return {{"ok", true}, {"status", "succeeded"}}; }
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
        return AudioStatus{true, bpm_, 44100, 1.0f, "flat", 0, true, 0, 0.0, 0};
    }
    int activeVoiceCount() const noexcept override { return 0; }
    void activeVoices(std::vector<VoiceInfo>& out) const override { (void)out; }

        std::vector<SlotPlayback> listSlotPlayback() const noexcept override { return {}; }
    std::vector<InstrumentInfo> listChuckInstruments(const fs::path&) const noexcept override
    { return {}; }
    fs::path studioInstrumentsDir(const fs::path& p) const noexcept override {
        return p / ".hathor_assets" / "chuck_instruments";
    }

    double bpm_ = 120.0;
};

// ===========================================================================
// Test helpers
// ===========================================================================

namespace {

/// Thread-safe accumulator for the progress event stream.
struct EventCollector {
    std::mutex mtx;
    std::vector<AgenticWorkflow::ProgressEvent> events;

    AgenticWorkflow::ProgressCallback cb() {
        return [this](const AgenticWorkflow::ProgressEvent& ev) {
            std::lock_guard<std::mutex> lock(mtx);
            events.push_back(ev);
        };
    }
    std::vector<AgenticWorkflow::ProgressEvent> snapshot() {
        std::lock_guard<std::mutex> lock(mtx);
        return events;
    }
    size_t count() {
        std::lock_guard<std::mutex> lock(mtx);
        return events.size();
    }
};

/// Poll getState() until it reaches a terminal state (completed/failed/cancelled)
/// or @p deadline elapses.  Returns the final state string.
std::string waitForTerminal(AgenticWorkflow& wf,
                            std::chrono::steady_clock::time_point deadline)
{
    for (;;) {
        const std::string s = wf.getState().value("state", std::string{});
        if (s == "completed" || s == "failed" || s == "cancelled")
            return s;
        if (std::chrono::steady_clock::now() > deadline)
            return wf.getState().value("state", std::string{});
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // anonymous namespace

// ===========================================================================
// 1 + 2 + 4. WorkflowStarted is queued; events carry a stable workflow id;
// a completed run emits WorkflowCompleted with a completed snapshot.
// ===========================================================================

TEST_CASE("AI-10.4: WorkflowStarted reports queued and stream ends Completed",
          "[ai10][ai10_4][events][queued][completed]")
{
    FakePlanFacade4 audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_4_test";

    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    hathor::control::ChuckSessionService chuckService(audio);
    hathor::control::RenderService renderService(audio, bank, chuckService);
    hathor::control::SongMutationService songService(audio, bank);
    AgenticWorkflow wf(audio, bank, readFacade, chuckService, renderService, songService);

    EventCollector events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    AgenticWorkflow::Request req;
    req.intent = "bd sn hh acid groove";
    req.targetSlot = "d1";
    req.notation = "bd sn hh cp";
    req.dryRun = true;

    REQUIRE(wf.start(req, events.cb(), [](AgenticWorkflow::ConfirmationRequest) {}));

    const std::string terminal = waitForTerminal(wf, deadline);
    REQUIRE(terminal == "completed");

    const auto stream = events.snapshot();
    REQUIRE_FALSE(stream.empty());

    // 1. The first event is the initial WorkflowStarted with a queued snapshot.
    REQUIRE(stream.front().type == AgenticWorkflow::EventType::WorkflowStarted);
    REQUIRE(stream.front().state.value("state", std::string{}) == "queued");
    REQUIRE(stream.front().details.value("queued", false) == true);
    REQUIRE(stream.front().message.find("workflow for") != std::string::npos);

    // 2. Every event carries the same, non-zero workflow identity.
    REQUIRE(stream.front().workflowId != 0);
    for (const auto& ev : stream)
        REQUIRE(ev.workflowId == stream.front().workflowId);

    // 3+4. The stream is ordered and ends with WorkflowCompleted.
    REQUIRE(stream.back().type == AgenticWorkflow::EventType::WorkflowCompleted);
    REQUIRE(stream.back().ok == true);
    REQUIRE(stream.back().state.value("state", std::string{}) == "completed");
    REQUIRE(stream.back().details.contains("completed_steps"));
    REQUIRE(stream.back().details["completed_steps"].is_array());
    REQUIRE_FALSE(stream.back().details["completed_steps"].empty());

    // The queued event must precede the completed event.
    const auto it = std::find_if(stream.begin(), stream.end(),
        [](const auto& ev) {
            return ev.type == AgenticWorkflow::EventType::WorkflowStarted;
        });
    const auto end = stream.end() - 1;  // last element is WorkflowCompleted
    REQUIRE(it < end);
}

// ===========================================================================
// 5. eventTypeName() maps every EventType to a stable non-empty string.
// ===========================================================================

TEST_CASE("AI-10.4: every EventType has a stable string name",
          "[ai10][ai10_4][events][event_type_name]")
{
    using E = AgenticWorkflow::EventType;
    const E all[] = {
        E::WorkflowStarted, E::PlanCreated, E::StepStarted, E::StepProgress,
        E::StepCompleted, E::StepFailed, E::DiagnosticsDiscovered, E::RepairStarted,
        E::RepairCompleted, E::ConfirmationRequired, E::RenderStarted,
        E::RenderCompleted, E::AssetCommitted, E::SongMutationApplied,
        E::WorkflowCancelled, E::WorkflowCompleted, E::LifecycleTransition,
    };
    for (const auto e : all) {
        const char* name = AgenticWorkflow::eventTypeName(e);
        REQUIRE(name != nullptr);
        REQUIRE(std::string(name).size() > 0);
        // Stable snake_case (no spaces) so the UI can key on it.
        REQUIRE(std::string(name).find(' ') == std::string::npos);
    }
    REQUIRE(std::string(AgenticWorkflow::eventTypeName(E::WorkflowStarted)) == "workflow_started");
    REQUIRE(std::string(AgenticWorkflow::eventTypeName(E::WorkflowCompleted)) == "workflow_completed");
}

// ===========================================================================
// 6. start() is gated — a second start while active fails.
// ===========================================================================

TEST_CASE("AI-10.4: start() refuses a second concurrent workflow",
          "[ai10][ai10_4][events][gating]")
{
    FakePlanFacade4 audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_4_gate_test";

    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    hathor::control::ChuckSessionService chuckService(audio);
    hathor::control::RenderService renderService(audio, bank, chuckService);
    hathor::control::SongMutationService songService(audio, bank);
    AgenticWorkflow wf(audio, bank, readFacade, chuckService, renderService, songService);

    EventCollector events;

    AgenticWorkflow::Request req;
    req.intent = "quick pattern";
    req.targetSlot = "d1";
    req.notation = "bd sn";
    req.dryRun = true;

    REQUIRE(wf.start(req, events.cb(), [](AgenticWorkflow::ConfirmationRequest) {}));

    // A second, identical start must be rejected while the first is active.
    REQUIRE_FALSE(wf.start(req, events.cb(), [](AgenticWorkflow::ConfirmationRequest) {}));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    REQUIRE(waitForTerminal(wf, deadline) == "completed");
}

// ===========================================================================
// 7. Change-set integration (AI-10.3) — getChangeSet() reflects the run.
// ===========================================================================

TEST_CASE("AI-10.4: a completed run exposes its change-set (AI-10.3)",
          "[ai10][ai10_4][events][changeset]")
{
    FakePlanFacade4 audio;
    audio.projectDir = fs::temp_directory_path() / "hathor_ai10_4_cs_test";

    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    hathor::control::ChuckSessionService chuckService(audio);
    hathor::control::RenderService renderService(audio, bank, chuckService);
    hathor::control::SongMutationService songService(audio, bank);
    AgenticWorkflow wf(audio, bank, readFacade, chuckService, renderService, songService);

    EventCollector events;

    AgenticWorkflow::Request req;
    req.intent = "my reviewed change";
    req.targetSlot = "d1";
    req.notation = "bd sn hh";
    req.dryRun = true;

    REQUIRE(wf.start(req, events.cb(), [](AgenticWorkflow::ConfirmationRequest) {}));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    REQUIRE(waitForTerminal(wf, deadline) == "completed");

    // The change-set is exposed and tied to this run's intent.
    const nlohmann::json cs = wf.getChangeSet();
    REQUIRE(cs.value("ok", false) == true);
    REQUIRE(cs["intent"] == "my reviewed change");
    REQUIRE(cs["status"] == "pending");  // dry-run never auto-accepts
    REQUIRE(cs["change_set_id"].is_number_integer());

    // It is coherently reviewable / previewable.
    const nlohmann::json preview = wf.previewChangeSet();
    REQUIRE(preview.value("ok", false) == true);
    REQUIRE(preview["intent"] == "my reviewed change");
    REQUIRE(preview["status"] == "pending");
}
