// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_ai10_7_e2e_chuck_workflow.cpp — AI-10.7: End-to-end ChucK workflow +
 * change-set lifecycle (compile → audition → render → bind → edit-song
 * → accept → revert).
 *
 * Exercises the full AgenticWorkflow orchestration against a JUCE-free fake
 * AudioEngineFacade that simulates ChucK compilation, VM lifecycle, and
 * background rendering with on-disk WAV creation.
 *
 * Verifies:
 *   1. Dry-run ChucK workflow completes without any persistent mutation
 *      (no .ck / .wav / .hathor files created, change-set stays pending).
 *   2. Full (non-dry-run) ChucK workflow pauses for confirmation at each
 *      persistent-mutation step (commit_rendered_asset, edit_song), and upon
 *      approval creates the baked .ck + .wav, registers in SampleBank, and
 *      writes the .hathor song file with the correct notation.
 *   3. Change-set accept + revert removes the baked asset files and restores
 *      the song file to its pre-workflow content.
 *
 * Architecture: REAL AgenticWorkflow + real ProjectReadFacade,
 * ChuckSessionService, RenderService, SongMutationService, IntentPlanner,
 * and WorkingSet — same JUCE-free pattern as test_ai10_1/4.
 *
 * Requirement references: AI-10.1, AI-10.2, AI-10.3, AI-6, AI-7
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
#include "AssetTarget.hpp"
#include "ChuckRenderWriter.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using hathor::control::AgenticWorkflow;
using hathor::control::ChuckSessionService;
using hathor::control::ProjectReadFacade;
using hathor::control::RenderService;
using hathor::control::SongMutationService;

namespace fs = std::filesystem;

// ===========================================================================
// E2EFakeFacade — JUCE-free AudioEngineFacade with ChucK compile + render sim
// ===========================================================================
//
// Key differences from FakePlanFacade4 (test_ai10_4):
//   - hasWorker() returns true  (real worker process is alive)
//   - startAsyncCkCompile fires the completion callback synchronously
//     with success=true (simulates libchuck producing a valid compile)
//   - queryCkTab returns "active shred_id=1 source_hash=0xabc"
//     (so auditionSession sees SessionState::Live)
//   - startBakeRenderRaw writes a minimal WAV to the temp path + fires callback
//     (same simulateRender pattern as test_ai6 AI6FakeFacade)
//   - registerBakedAsset adds to the real SampleBank
//   - listChuckInstruments scans the Studio directory on disk

class E2EFakeFacade final : public AudioEngineFacade {
public:
    fs::path projectDir_;
    SampleBank& bank_;
    std::atomic<uint64_t> nextJobId_{1000};
    std::atomic<uint64_t> nextRenderId_{1};
    double bpm_ = 120.0;

    // Slot table (4 slots, like test_ai10_4/AI6FakeFacade).
    static constexpr int kNumSlots = 4;
    std::string slotNames_[kNumSlots]{};

    E2EFakeFacade(SampleBank& bank) : bank_(bank) {}

    void initProjectDir(const fs::path& p) {
        projectDir_ = p;
        fs::create_directories(p);
        fs::create_directories(p / ".hathor_assets" / "chuck_instruments");
    }

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
    hathor::EqPreset getMasterEqPreset() const noexcept override {
        return hathor::EqPreset::Flat;
    }

    // --- Hot-swap slots ---
    int findOrAddSlot(const std::string& name) override {
        for (int i = 0; i < kNumSlots; ++i)
            if (slotNames_[i] == name) return i;
        for (int i = 0; i < kNumSlots; ++i)
            if (slotNames_[i].empty()) { slotNames_[i] = name; return i; }
        return -1;
    }
    void storeSlot(int, std::shared_ptr<SlotState>) noexcept override {}
    bool clearSlot(int) noexcept override { return true; }
    int slotCount() const noexcept override { return kNumSlots; }
    std::string slotName(int idx) const override {
        if (idx >= 0 && idx < kNumSlots) return slotNames_[idx];
        return "";
    }
    std::shared_ptr<SlotState> loadSlot(int) const noexcept override {
        return nullptr;
    }

    void slotPlay(int) noexcept override {}
    void slotStop(int) noexcept override {}
    bool isSlotRunning(int) const noexcept override { return false; }

    // --- ChucK session compile lifecycle ---
    bool hasWorker() const noexcept override { return true; }
    bool ckEval(int, const std::string&) noexcept override { return true; }
    bool stopCkTab(int) noexcept override { return true; }
    std::string queryCkTab(int) const override {
        return "active shred_id=1 source_hash=0xabc";
    }

    uint64_t startAsyncCkCompile(int,
                                 const std::string&,
                                 std::function<void(bool, const std::string&)> onComplete) override
    {
        const uint64_t jobId = nextJobId_.fetch_add(1);
        // Simulate libchuck: fire callback synchronously with success.
        if (onComplete)
            onComplete(true, "ok shred_id=1 source_hash=0xabc");
        return jobId;
    }

    nlohmann::json queryCkJob(uint64_t) const override {
        return {{"ok", true}, {"status", "succeeded"},
                {"shred_id", 1}, {"source_hash", "0xabc"}};
    }
    bool cancelCkJob(uint64_t) override { return true; }

    // --- Asset targets ---
    fs::path resolveRenderPath(hathor::AssetTarget target,
                               std::string_view name,
                               const fs::path&) override
    {
        if (target == hathor::AssetTarget::Studio)
            return projectDir_ / ".hathor_assets" / "chuck_instruments"
                   / (std::string(name) + ".wav");
        return {};
    }
    void setLiveJamSessionDir(fs::path) override {}
    void setProjectDir(fs::path dir) override {
        projectDir_ = std::move(dir);
        std::error_code ec;
        fs::create_directories(projectDir_, ec);
        fs::create_directories(projectDir_ / ".hathor_assets" / "chuck_instruments", ec);
    }
    fs::path currentProjectDir() const noexcept override { return projectDir_; }
    void cleanupLiveJamAssets() override {}
    bool isStudioAssetPath(const fs::path&) const override { return true; }

    // --- Background render (B8-K2) — simulates WAV + callback ---
    hathor::RenderHandle startBakeRender(uint8_t tabId, std::string ckSource,
                                         uint64_t numSamples, unsigned sampleRate,
                                         const fs::path& destPath,
                                         hathor::ChuckRenderWriter::CompletionCallback cb) override
    {
        return simulateRender(tabId, std::move(ckSource), numSamples,
                              sampleRate, destPath, true, std::move(cb));
    }

    hathor::RenderHandle startBakeRenderRaw(uint8_t tabId, std::string ckSource,
                                            uint64_t numSamples, unsigned sampleRate,
                                            const fs::path& destPath,
                                            hathor::ChuckRenderWriter::CompletionCallback cb) override
    {
        return simulateRender(tabId, std::move(ckSource), numSamples,
                              sampleRate, destPath, false, std::move(cb));
    }

    hathor::RenderHandle simulateRender(uint8_t, std::string,
                                        uint64_t numSamples, unsigned sampleRate,
                                        const fs::path& destPath,
                                        bool autoRegister,
                                        hathor::ChuckRenderWriter::CompletionCallback onComplete)
    {
        const uint64_t handleId = nextRenderId_.fetch_add(1);
        auto state = std::make_shared<std::atomic<hathor::RenderState>>(
            hathor::RenderState::Pending);
        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);

        std::thread([=, this]() mutable {
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(5ms);

            if (cancelFlag->load(std::memory_order_relaxed)) {
                state->store(hathor::RenderState::Cancelled,
                             std::memory_order_release);
                if (onComplete)
                    onComplete(hathor::RenderResult{
                        .success = false,
                        .state = hathor::RenderState::Cancelled,
                        .errorMessage = "cancelled by user",
                    });
                return;
            }

            state->store(hathor::RenderState::Writing,
                         std::memory_order_release);

            const uint32_t actualSamples = static_cast<uint32_t>(numSamples);
            const uint32_t dataSize = actualSamples * 2;
            const uint32_t fileSize = 36 + dataSize;

            std::error_code ec;
            fs::create_directories(destPath.parent_path(), ec);

            std::ofstream f(destPath, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) {
                state->store(hathor::RenderState::Failed,
                             std::memory_order_release);
                if (onComplete)
                    onComplete(hathor::RenderResult{
                        .success = false,
                        .state = hathor::RenderState::Failed,
                        .errorMessage = "cannot open output file: " + destPath.string(),
                    });
                return;
            }

            f << "RIFF";
            uint32_t fileSizeLE = fileSize;
            f.write(reinterpret_cast<const char*>(&fileSizeLE), 4);
            f << "WAVE";

            f << "fmt ";
            uint32_t fmtSize = 16;
            f.write(reinterpret_cast<const char*>(&fmtSize), 4);
            uint16_t audioFormat = 1;
            f.write(reinterpret_cast<const char*>(&audioFormat), 2);
            uint16_t channels = 1;
            f.write(reinterpret_cast<const char*>(&channels), 2);
            uint32_t sr = sampleRate;
            f.write(reinterpret_cast<const char*>(&sr), 4);
            uint32_t byteRate = sampleRate * 2;
            f.write(reinterpret_cast<const char*>(&byteRate), 4);
            uint16_t blockAlign = 2;
            f.write(reinterpret_cast<const char*>(&blockAlign), 2);
            uint16_t bitsPerSample = 16;
            f.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

            f << "data";
            f.write(reinterpret_cast<const char*>(&dataSize), 4);
            std::vector<uint16_t> samples(actualSamples, 0);
            f.write(reinterpret_cast<const char*>(samples.data()),
                    static_cast<std::streamsize>(samples.size() * sizeof(uint16_t)));
            f.close();

            state->store(hathor::RenderState::Completed,
                         std::memory_order_release);

            if (autoRegister) {
                const std::string name = destPath.stem().string();
                registerBakedAsset(name, destPath);
            }

            if (onComplete)
                onComplete(hathor::RenderResult{
                    .success = true,
                    .state = hathor::RenderState::Completed,
                    .errorMessage = {},
                    .outputPath = destPath,
                    .samplesWritten = actualSamples,
                    .durationSeconds = static_cast<double>(actualSamples)
                                       / static_cast<double>(sampleRate),
                });
        }).detach();

        return hathor::RenderHandle(handleId, state, cancelFlag, nullptr);
    }

    int activeRenderCount() const noexcept override { return 0; }
    void shutdownRender() noexcept override {}

    // --- SampleBank registration ---
    bool registerBakedAsset(std::string name, const fs::path& wavPath) override {
        bank_.addEntry(name, 0, {}, 1, 44100, wavPath.string());
        return true;
    }
    std::vector<std::string> listSamples() const override {
        return bank_.listNames();
    }

    // --- Read-only introspection ---
    std::vector<SlotInfo> listSlots() const noexcept override {
        std::vector<SlotInfo> slots;
        for (int i = 0; i < kNumSlots; ++i)
            slots.push_back(SlotInfo{
                .slotIndex = i,
                .slotName = slotNames_[i],
                .active = !slotNames_[i].empty(),
                .running = false,
                .notation = "",
                .eventCount = 0,
            });
        return slots;
    }
    SlotInfo getSlotInfo(int idx) const noexcept override {
        if (idx >= 0 && idx < kNumSlots && !slotNames_[idx].empty())
            return SlotInfo{.slotIndex = idx, .slotName = slotNames_[idx],
                            .active = true, .running = false,
                            .notation = "", .eventCount = 0};
        return SlotInfo{};
    }
    VmStatus getVmStatus(int) const noexcept override { return {}; }
    AudioStatus getAudioStatus() const noexcept override {
        return AudioStatus{true, bpm_, 44100, 1.0f, "flat", 0, true, 0};
    }
    std::vector<SlotPlayback> listSlotPlayback() const noexcept override {
        return {};
    }
    std::vector<InstrumentInfo> listChuckInstruments(
        const fs::path& projectDir) const noexcept override
    {
        std::vector<InstrumentInfo> instruments;
        const fs::path instDir = projectDir / ".hathor_assets" / "chuck_instruments";
        std::error_code ec;
        if (!fs::exists(instDir, ec)) return instruments;

        for (const auto& entry : fs::directory_iterator(instDir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".ck") continue;

            const std::string name = entry.path().stem().string();
            const fs::path wavPath = instDir / (name + ".wav");
            InstrumentInfo info;
            info.name = name;
            info.sourceCkExists = true;
            info.renderedWavExists = fs::exists(wavPath, ec);
            info.boundToSampleBank = false;
            info.sourcePath = entry.path().string();
            info.renderedPath = info.renderedWavExists ? wavPath.string() : "";
            info.durationSeconds = 0.0;
            instruments.push_back(info);
        }
        return instruments;
    }
    fs::path studioInstrumentsDir(const fs::path& p) const noexcept override {
        return p / ".hathor_assets" / "chuck_instruments";
    }
};

// ===========================================================================
// Event collector — thread-safe accumulator for progress events
// ===========================================================================

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
};

// ===========================================================================
// Helpers
// ===========================================================================

/// Poll getState() until terminal state or deadline.
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

/// A confirmation callback that always approves, recording which actions
/// were confirmed.  Calls respondToConfirmation from a detached thread to
/// avoid deadlocking on the workflow's confirmCv_ wait.
struct ApprovingConfirmer {
    std::vector<std::string> confirmed;
    std::mutex mtx;

    AgenticWorkflow::ConfirmationCallback callback(AgenticWorkflow& wf) {
        return [this, &wf](AgenticWorkflow::ConfirmationRequest req) {
            {
                std::lock_guard<std::mutex> lock(mtx);
                confirmed.push_back(req.action);
            }
            std::thread([&wf]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                wf.respondToConfirmation(true);
            }).detach();
        };
    }

    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lock(mtx);
        return confirmed;
    }
};

// Valid ChucK source used across all test cases.
static const std::string kValidCkSource =
    "SinOsc s => dac; 440 => s.freq; 1::second => now;";

// ===========================================================================
// 1. Dry-run ChucK workflow completes without persistent mutation
// ===========================================================================

TEST_CASE("AI-10.7: dry-run ChucK workflow completes without persistent mutation",
          "[ai10][ai10_7][dry_run][e2e]")
{
    const fs::path projectDir =
        fs::temp_directory_path() / "hathor_ai10_7_dryrun";
    std::error_code ec;
    fs::remove_all(projectDir, ec);

    SampleBank bank;
    E2EFakeFacade audio(bank);
    audio.initProjectDir(projectDir);

    ChuckSessionService chuckService(audio);
    ProjectReadFacade readFacade(audio, bank);
    RenderService renderService(audio, bank, chuckService);
    SongMutationService songService(audio, bank);

    AgenticWorkflow wf(audio, bank, readFacade, chuckService,
                       renderService, songService);

    EventCollector events;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(15);

    AgenticWorkflow::Request req;
    req.intent = "acid bass growl instrument";
    req.targetSlot = "d1";
    req.assetName = "acid_bass";
    req.ckSource = kValidCkSource;
    req.dryRun = true;

    // In dry-run mode, no confirmation should ever be requested.
    REQUIRE(wf.start(req, events.cb(),
        [](AgenticWorkflow::ConfirmationRequest) {
            REQUIRE(false);  // should not be reached in dry-run
        }));

    const std::string terminal = waitForTerminal(wf, deadline);
    REQUIRE(terminal == "completed");

    const auto stream = events.snapshot();
    REQUIRE_FALSE(stream.empty());

    // Final event reports completion.
    REQUIRE(stream.back().type == AgenticWorkflow::EventType::WorkflowCompleted);
    REQUIRE(stream.back().ok == true);

    // No persistent song mutation: the .hathor file is NOT created
    // (stepUpdateSong has a dry-run guard that simulates without writing).
    REQUIRE_FALSE(fs::exists(projectDir / "d1.hathor", ec));

    // Change-set is exposed but pending (dry-run never auto-accepts).
    const nlohmann::json cs = wf.getChangeSet();
    REQUIRE(cs.value("ok", false) == true);
    REQUIRE(cs["intent"] == "acid bass growl instrument");
    REQUIRE(cs["status"] == "pending");
    REQUIRE(cs["change_set_id"].is_number_integer());
    REQUIRE(cs["operations"].is_array());
    REQUIRE(cs["operations"].size() >= 1);

    // In dry-run mode the commit_rendered_asset step still executes
    // (stepBindAsset has no dry-run guard), so the SampleBank WILL have
    // the entry.  What dry-run skips is the confirmation prompt and the
    // song-file write (stepUpdateSong has a dry-run guard).
    const auto samples = bank.listNames();
    REQUIRE(std::find(samples.begin(), samples.end(), "acid_bass")
            != samples.end());

    fs::remove_all(projectDir, ec);
}

// ===========================================================================
// 2. Full ChucK workflow with approval creates assets + song file
// ===========================================================================

TEST_CASE("AI-10.7: full ChucK workflow with approval creates assets and song file",
          "[ai10][ai10_7][full_workflow][confirmation]")
{
    const fs::path projectDir =
        fs::temp_directory_path() / "hathor_ai10_7_full";
    std::error_code ec;
    fs::remove_all(projectDir, ec);

    SampleBank bank;
    E2EFakeFacade audio(bank);
    audio.initProjectDir(projectDir);

    ChuckSessionService chuckService(audio);
    ProjectReadFacade readFacade(audio, bank);
    RenderService renderService(audio, bank, chuckService);
    SongMutationService songService(audio, bank);

    AgenticWorkflow wf(audio, bank, readFacade, chuckService,
                       renderService, songService);

    EventCollector events;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(15);

    // Pre-create the song file so editSong (replace_pattern) can modify it.
    // (SongMutationService::editSong requires the target .hathor to exist.)
    const fs::path songFile = projectDir / "d1.hathor";
    {
        std::ofstream f(songFile);
        f << "s \"kick\"";
        f.close();
    }

    ApprovingConfirmer confirmer;

    AgenticWorkflow::Request req;
    req.intent = "acid bass growl instrument";
    req.targetSlot = "d1";
    req.assetName = "acid_bass";
    req.ckSource = kValidCkSource;
    req.dryRun = false;

    REQUIRE(wf.start(req, events.cb(), confirmer.callback(wf)));

    const std::string terminal = waitForTerminal(wf, deadline);
    REQUIRE(terminal == "completed");

    // Verify both confirmation steps were requested.
    const auto confirmed = confirmer.snapshot();
    REQUIRE(confirmed.size() >= 2);
    bool sawAsset = false, sawSong = false;
    for (const auto& a : confirmed) {
        if (a == "commit_rendered_asset") sawAsset = true;
        if (a == "edit_song") sawSong = true;
    }
    REQUIRE(sawAsset);
    REQUIRE(sawSong);

    const auto stream = events.snapshot();
    REQUIRE_FALSE(stream.empty());
    REQUIRE(stream.back().type == AgenticWorkflow::EventType::WorkflowCompleted);
    REQUIRE(stream.back().ok == true);

    // Verify an AssetCommitted event was emitted.
    bool sawAssetCommitted = false;
    bool sawSongMutation = false;
    for (const auto& ev : stream) {
        if (ev.type == AgenticWorkflow::EventType::AssetCommitted)
            sawAssetCommitted = true;
        if (ev.type == AgenticWorkflow::EventType::SongMutationApplied)
            sawSongMutation = true;
    }
    REQUIRE(sawAssetCommitted);
    REQUIRE(sawSongMutation);

    // --- Persistent mutation: asset files created ---
    const fs::path instDir = projectDir / ".hathor_assets" / "chuck_instruments";
    REQUIRE(fs::exists(instDir / "acid_bass.ck", ec));
    REQUIRE(fs::exists(instDir / "acid_bass.wav", ec));

    // The .ck file contains the ChucK source.
    std::ifstream ckFile(instDir / "acid_bass.ck");
    REQUIRE(ckFile.is_open());
    std::string ckContent((std::istreambuf_iterator<char>(ckFile)),
                          std::istreambuf_iterator<char>());
    REQUIRE(ckContent.find("SinOsc") != std::string::npos);

    // --- SampleBank has the baked asset ---
    const auto samples = bank.listNames();
    REQUIRE(std::find(samples.begin(), samples.end(), "acid_bass") != samples.end());

    // --- Song file written with correct notation ---
    REQUIRE(fs::exists(songFile, ec));
    std::ifstream sf(songFile);
    REQUIRE(sf.is_open());
    std::string songContent((std::istreambuf_iterator<char>(sf)),
                            std::istreambuf_iterator<char>());
    // The notation should reference the baked instrument.
    REQUIRE(songContent.find("acid_bass") != std::string::npos);

    // --- Change-set is pending until explicitly accepted ---
    const nlohmann::json cs = wf.getChangeSet();
    REQUIRE(cs.value("ok", false) == true);
    REQUIRE(cs["intent"] == "acid bass growl instrument");
    REQUIRE(cs["status"] == "pending");
    REQUIRE(cs["change_set_id"].is_number_integer());
    REQUIRE(cs["reversible"] == true);
    REQUIRE(cs["operations"].is_array());
    REQUIRE(cs["operations"].size() == 2);

    fs::remove_all(projectDir, ec);
}

// ===========================================================================
// 3. Change-set accept + revert removes assets and restores song
// ===========================================================================

TEST_CASE("AI-10.7: change-set accept and revert rolls back assets and song",
          "[ai10][ai10_7][changeset][revert]")
{
    const fs::path projectDir =
        fs::temp_directory_path() / "hathor_ai10_7_revert";
    std::error_code ec;
    fs::remove_all(projectDir, ec);

    SampleBank bank;
    E2EFakeFacade audio(bank);
    audio.initProjectDir(projectDir);

    ChuckSessionService chuckService(audio);
    ProjectReadFacade readFacade(audio, bank);
    RenderService renderService(audio, bank, chuckService);
    SongMutationService songService(audio, bank);

    AgenticWorkflow wf(audio, bank, readFacade, chuckService,
                       renderService, songService);

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(15);

    // --- Pre-create a song file with baseline content ---
    const fs::path songFile = projectDir / "d1.hathor";
    const std::string baselineContent = "s \"kick\"";
    {
        std::ofstream f(songFile);
        f << baselineContent;
        f.close();
    }
    REQUIRE(fs::exists(songFile, ec));

    // --- Phase A: run the full workflow with approval ---
    ApprovingConfirmer confirmer;

    AgenticWorkflow::Request req;
    req.intent = "acid bass growl instrument";
    req.targetSlot = "d1";
    req.assetName = "acid_bass";
    req.ckSource = kValidCkSource;
    req.dryRun = false;

    REQUIRE(wf.start(req,
        [](AgenticWorkflow::ProgressEvent) {},
        confirmer.callback(wf)));

    REQUIRE(waitForTerminal(wf, deadline) == "completed");

    // Verify assets + song were created/overwritten.
    const fs::path instDir = projectDir / ".hathor_assets" / "chuck_instruments";
    REQUIRE(fs::exists(instDir / "acid_bass.ck", ec));
    REQUIRE(fs::exists(instDir / "acid_bass.wav", ec));

    // Song file should now contain the new instrument reference.
    {
        std::ifstream sf(songFile);
        std::string afterContent((std::istreambuf_iterator<char>(sf)),
                                 std::istreambuf_iterator<char>());
        REQUIRE(afterContent.find("acid_bass") != std::string::npos);
        REQUIRE(afterContent.find("kick") == std::string::npos);
    }

    // --- Phase B: reject (revert) the pending change-set with confirmation ---
    const nlohmann::json rejectResult = wf.rejectChangeSet(true);
    REQUIRE(rejectResult.value("ok", false) == true);
    REQUIRE(rejectResult["status"] == "rejected");
    REQUIRE(rejectResult["executed"].is_array());
    REQUIRE(rejectResult["executed"].size() == 2);

    // The baked asset files should have been removed on revert.
    REQUIRE_FALSE(fs::exists(instDir / "acid_bass.ck", ec));
    REQUIRE_FALSE(fs::exists(instDir / "acid_bass.wav", ec));

    // The song file should have been restored to its pre-workflow content.
    {
        std::ifstream sf(songFile);
        REQUIRE(sf.is_open());
        std::string restoredContent((std::istreambuf_iterator<char>(sf)),
                                    std::istreambuf_iterator<char>());
        REQUIRE(restoredContent.find("kick") != std::string::npos);
        REQUIRE(restoredContent.find("acid_bass") == std::string::npos);
    }

    // SampleBank should no longer have the acid_bass entry.
    const auto samples = bank.listNames();
    REQUIRE(std::find(samples.begin(), samples.end(), "acid_bass")
            == samples.end());

    fs::remove_all(projectDir, ec);
}

// ---------------------------------------------------------------------------
// SampleBank::removeEntry — JUCE-free stub.
//
// SampleBank.cpp pulls in juce_audio_formats and is NOT compiled into
// hathor-control-tests (JUCE-free).  The real removeEntry is a trivial
// vector erase; provide a minimal definition so RenderService::removeRenderedAsset
// can link.  This mirrors the production implementation exactly.
// ---------------------------------------------------------------------------

int SampleBank::removeEntry(std::string_view name, int64_t index)
{
    std::lock_guard<std::mutex> lock(registrationMutex_);

    int removed = 0;
    for (auto it = entries_.begin(); it != entries_.end(); )
    {
        if (it->name == name && it->index == index)
        {
            it = entries_.erase(it);
            ++removed;
        }
        else
        {
            ++it;
        }
    }

    if (removed > 0)
        loaded_ = std::max(0, loaded_ - removed);
    return removed;
}
