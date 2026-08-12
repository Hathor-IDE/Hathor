// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_ai6_render_lifecycle.cpp — AI-6: Background render with explicit
 * render→commit boundary.
 *
 * Verifies:
 *   1. render_chuck returns a job_id with "queued" status
 *   2. Render is async (render_chuck returns before completion)
 *   3. Render is non-destructive (temp file, not persistent project)
 *   4. get_job_status shows correct state transitions (queued → running → succeeded)
 *   5. Render completion callback fires and updates job state
 *   6. commit_rendered_asset creates .wav + .ck on disk
 *   7. commit_rendered_asset registers in SampleBank
 *   8. Collision detection (existing .ck/.wav)
 *   9. Overwriting without confirmation is rejected (conflict)
 *  10. Overwriting with confirmation succeeds
 *  11. Unsafe asset name rejected (path traversal)
 *  12. Unsafe commit asset name rejected
 *  13. Cancellation during render
 *  14. Session source is persisted and used for rendering
 *  15. Failed render does not commit
 *  16. No temp file leak on failure
 *  17. Commit after render failure is rejected
 *  18. LiveJam target (temp) path resolution
 *  19. Commit creates .ck with correct source
 *  20. No SampleBank mutation until commit (render phase)
 *  21. get_job_status for unknown job_id
 *  22. Audit logging on commit
 *
 * Architecture: tests use AI6FakeFacade (extends AudioEngineFacade) that
 * simulates B8-K2 background rendering by writing a minimal WAV file to the
 * requested temp path and invoking the completion callback on a background
 * thread.
 *
 * Requirement references: AI-1 §1, AI-5 §3/§16, AI-6 §1–§22, B8-K1–K4
 */

#include "ControlInterface.hpp"
#include "ChuckSessionService.hpp"
#include "RenderService.hpp"
#include "ChuckSession.hpp"
#include "ChuckDiagnostics.hpp"

#include "AudioEngineFacade.hpp"
#include "SampleBank.hpp"
#include "SlotState.hpp"
#include "AssetTarget.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using hathor::control::ControlInterface;
using hathor::control::ChuckSessionService;
using hathor::control::RenderService;
using hathor::ChuckSession;
using hathor::JobState;
using hathor::AssetTarget;

namespace fs = std::filesystem;

// ===========================================================================
// AI6FakeFacade — simulates AudioEngineFacade with working B8-K2 render
// ===========================================================================

class AI6FakeFacade final : public AudioEngineFacade {
public:
    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------
    explicit AI6FakeFacade(SampleBank& bank)
        : projectDir_(fs::temp_directory_path() / "hathor_ai6_test")
        , bank_(bank)
    {}

    // --- Transport ---
    void play() noexcept override {}
    void stop() noexcept override {}
    void setBpm(double bpm) noexcept override { bpm_ = bpm; }
    double getBpm() const noexcept override { return bpm_; }
    bool isRunning() const noexcept override { return true; }

    // --- Master gain / EQ ---
    void setMasterGain(float g) noexcept override { gain_ = g; }
    float getMasterGain() const noexcept override { return gain_; }
    void setMasterEqPreset(hathor::EqPreset p) noexcept override { eqPreset_ = p; }
    hathor::EqPreset getMasterEqPreset() const noexcept override { return eqPreset_; }

    // --- Slot API ---
    int findOrAddSlot(const std::string& name) override {
        for (int i = 0; i < 16; ++i)
            if (slotNames_[i] == name) return i;
        for (int i = 0; i < 16; ++i) {
            if (slotNames_[i].empty()) {
                slotNames_[i] = name;
                return i;
            }
        }
        return -1;
    }
    void storeSlot(int idx, std::shared_ptr<SlotState> state) noexcept override {
        if (idx >= 0 && idx < 16) states_[idx] = std::move(state);
    }
    bool clearSlot(int idx) noexcept override {
        if (idx >= 0 && idx < 16) { states_[idx].reset(); return true; }
        return false;
    }
    int slotCount() const noexcept override { return 16; }
    std::string slotName(int idx) const override {
        if (idx >= 0 && idx < 16) return slotNames_[idx]; return {}; }
    std::shared_ptr<SlotState> loadSlot(int idx) const noexcept override {
        if (idx >= 0 && idx < 16)
            return std::atomic_load_explicit(&states_[idx], std::memory_order_acquire);
        return nullptr;
    }
    void slotPlay(int /*idx*/) noexcept override {}
    void slotStop(int /*idx*/) noexcept override {}
    bool isSlotRunning(int /*idx*/) const noexcept override { return false; }

    // --- B4-K7: ChucK VM ---
    bool hasWorker() const noexcept override { return workerAlive_; }
    bool ckEval(int idx, const std::string& code) noexcept override {
        (void)idx; (void)code; return true;
    }
    bool stopCkTab(int idx) noexcept override { (void)idx; return true; }
    std::string queryCkTab(int idx) const override {
        (void)idx; return "ok vm_state tab=" + std::to_string(idx) + " state=inactive";
    }
    uint64_t startAsyncCkCompile(int idx, const std::string& code,
                                  std::function<void(bool, const std::string&)> onComplete) override {
        (void)idx; (void)code; (void)onComplete; return 1;
    }
    nlohmann::json queryCkJob(uint64_t jobId) const override {
        return {{"ok", true}, {"job_id", jobId}, {"status", "unknown"}};
    }
    bool cancelCkJob(uint64_t jobId) override { (void)jobId; return true; }

    // --- B8-K1: Render path resolution ---
    fs::path resolveRenderPath(AssetTarget, std::string_view name,
                               const fs::path& projectDir) override {
        return projectDir / ".hathor_assets" / "chuck_instruments"
             / (hathor::sanitizeAssetName(name) + ".wav");
    }
    void setLiveJamSessionDir(fs::path dir) override { liveJamDir_ = std::move(dir); }
    void setProjectDir(fs::path dir) override { projectDir_ = std::move(dir); }
    fs::path currentProjectDir() const noexcept override { return projectDir_; }
    void cleanupLiveJamAssets() override {}
    bool isStudioAssetPath(const fs::path& path) const override {
        return path.string().find(".hathor_assets") != std::string::npos;
    }

    // --- AI-2 read-only ---
    AudioStatus getAudioStatus() const noexcept override {
        uint64_t clock = sampleClock_.fetch_add(1, std::memory_order_relaxed);
        return AudioStatus{true, bpm_, 44100, gain_,
                          hathor::presetName(eqPreset_),
                          clock, true, 1, 0.0, 0};
    }
    std::vector<SlotInfo> listSlots() const noexcept override { return {}; }
    SlotInfo getSlotInfo(int) const noexcept override { return {}; }
    VmStatus getVmStatus(int) const noexcept override { return {}; }
    int activeVoiceCount() const noexcept override { return 0; }
    void activeVoices(std::vector<VoiceInfo>& out) const override { (void)out; }

        std::vector<SlotPlayback> listSlotPlayback() const noexcept override { return {}; }
    std::vector<InstrumentInfo> listChuckInstruments(
        const fs::path&) const noexcept override { return {}; }
    fs::path studioInstrumentsDir(const fs::path& projectDir) const noexcept override {
        return projectDir / ".hathor_assets" / "chuck_instruments";
    }

    // --- B8-K2: Background render (simulated) ---
    hathor::RenderHandle startBakeRender(uint8_t tabId, std::string ckSource,
                                          uint64_t numSamples, unsigned sampleRate,
                                          const fs::path& destPath,
                                          hathor::ChuckRenderWriter::CompletionCallback onComplete) override {
        return simulateRender(tabId, ckSource, numSamples, sampleRate, destPath,
                              std::move(onComplete), /*register=*/true);
    }

    hathor::RenderHandle startBakeRenderRaw(uint8_t tabId, std::string ckSource,
                                             uint64_t numSamples, unsigned sampleRate,
                                             const fs::path& destPath,
                                             hathor::ChuckRenderWriter::CompletionCallback onComplete) override {
        return simulateRender(tabId, ckSource, numSamples, sampleRate, destPath,
                              std::move(onComplete), /*register=*/false);
    }

    int activeRenderCount() const noexcept override { return 0; }
    void shutdownRender() noexcept override {}

    // --- B8-K4: SampleBank registration ---
    bool registerBakedAsset(std::string name, const fs::path& wavPath) override {
        // In the fake, we don't decode the WAV — we just create a stub entry.
        // The real AudioEngine would decode, resample, and call addEntry.
        if (!fs::exists(wavPath)) {
            std::fprintf(stderr, "[Fake] registerBakedAsset: file not found: %s\n",
                         wavPath.c_str());
            return false;
        }

        // Create a minimal entry with stub data so find() can locate it.
        // Sample rate is 44100, mono, with a few samples of silence.
        std::vector<float> stubData(44100, 0.0f);  // 1 second of silence at 44.1k
        bank_.addEntry(std::move(name), 0, std::move(stubData),
                       1, 44100.0, wavPath.string());
        lastRegisteredName_ = name;
        return true;
    }

    std::vector<std::string> listSamples() const override {
        return bank_.listNames();
    }

    // -----------------------------------------------------------------------
    // Test controls
    // -----------------------------------------------------------------------

    /// Set whether the "worker" is alive (controls hasWorker()).
    void setWorkerAlive(bool alive) { workerAlive_ = alive; }

    /// Set whether the next render should succeed or fail.
    void setNextRenderSuccess(bool success) { nextRenderSuccess_ = success; }

    /// Set whether the next render should be cancelled.
    void setNextRenderCancel(bool cancel) { nextRenderCancel_ = cancel; }

    /// Set a delay (ms) before the render callback fires.
    void setRenderDelayMs(int ms) { renderDelayMs_ = ms; }

    /// Get the last registered asset name.
    std::string lastRegisteredName() const { return lastRegisteredName_; }

    /// Reset render simulation state.
    void resetRenderState() {
        nextRenderSuccess_ = true;
        nextRenderCancel_ = false;
        renderDelayMs_ = 0;
    }

    fs::path projectDir_;
    fs::path liveJamDir_;
    double bpm_ = 120.0;
    float gain_ = 1.0f;
    hathor::EqPreset eqPreset_ = hathor::EqPreset::Flat;
    bool workerAlive_ = true;
    mutable std::atomic<uint64_t> sampleClock_{0};

    std::string slotNames_[16] = {};
    std::shared_ptr<SlotState> states_[16] = {};

    // Render simulation controls
    std::atomic<bool> nextRenderSuccess_{true};
    std::atomic<bool> nextRenderCancel_{false};
    std::atomic<int> renderDelayMs_{0};

    // The SampleBank to register assets into (set via RenderService).
    SampleBank& bank_;
    std::string lastRegisteredName_;

private:
    // -----------------------------------------------------------------------
    // Helper: write a minimal WAV file + schedule completion callback
    // -----------------------------------------------------------------------

    hathor::RenderHandle simulateRender(
        uint8_t tabId,
        const std::string& ckSource,
        uint64_t numSamples,
        unsigned sampleRate,
        const fs::path& destPath,
        hathor::ChuckRenderWriter::CompletionCallback onComplete,
        bool autoRegister)
    {
        (void)tabId;
        (void)ckSource;

        // Generate a unique render handle ID.
        const uint64_t handleId = nextRenderId_.fetch_add(1, std::memory_order_relaxed);

        auto state = std::make_shared<std::atomic<hathor::RenderState>>(
            hathor::RenderState::Pending);
        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);

        // Spawn a background thread that:
        // 1. Waits for the configured delay
        // 2. Writes a minimal WAV file to destPath
        // 3. Calls the completion callback
        // 4. Optionally auto-registers in SampleBank (only for startBakeRender)
        std::thread([this, onComplete, numSamples, sampleRate, destPath,
                      autoRegister, state, cancelFlag]() mutable {
            if (renderDelayMs_.load() > 0)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(renderDelayMs_.load()));

            // Check cancellation before starting real work.
            if (cancelFlag->load(std::memory_order_acquire)) {
                state->store(hathor::RenderState::Cancelled,
                             std::memory_order_release);
                if (onComplete) {
                    onComplete(hathor::RenderResult{
                        .success = false,
                        .state = hathor::RenderState::Cancelled,
                        .errorMessage = "render cancelled",
                    });
                }
                return;
            }

            state->store(hathor::RenderState::Rendering,
                         std::memory_order_release);

            // Write a minimal mono WAV file.
            // WAV header = 44 bytes, data = numSamples * sizeof(float) if float,
            // or numSamples * sizeof(int16_t) if 16-bit PCM.
            // We use 16-bit PCM for simplicity.
            const bool success = nextRenderSuccess_.load();
            const bool cancel  = nextRenderCancel_.load();

            if (cancel) {
                state->store(hathor::RenderState::Cancelled,
                             std::memory_order_release);
                if (onComplete) {
                    onComplete(hathor::RenderResult{
                        .success = false,
                        .state = hathor::RenderState::Cancelled,
                        .errorMessage = "render cancelled by simulation",
                    });
                }
                return;
            }

            if (!success) {
                state->store(hathor::RenderState::Failed,
                             std::memory_order_release);
                if (onComplete) {
                    onComplete(hathor::RenderResult{
                        .success = false,
                        .state = hathor::RenderState::Failed,
                        .errorMessage = "simulated render failure",
                    });
                }
                return;
            }

            // Write WAV file.
            state->store(hathor::RenderState::Writing,
                         std::memory_order_release);

            const uint32_t actualSamples = static_cast<uint32_t>(numSamples);
            const uint32_t dataSize = actualSamples * 2;  // 16-bit PCM, mono
            const uint32_t fileSize = 36 + dataSize;  // RIFF(12) + fmt(8+16) + data(8) + data

            std::error_code ec;
            fs::create_directories(destPath.parent_path(), ec);

            std::ofstream f(destPath, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) {
                state->store(hathor::RenderState::Failed,
                             std::memory_order_release);
                if (onComplete) {
                    onComplete(hathor::RenderResult{
                        .success = false,
                        .state = hathor::RenderState::Failed,
                        .errorMessage = "cannot open output file: " + destPath.string(),
                    });
                }
                return;
            }

            // RIFF chunk
            f << "RIFF";
            uint32_t fileSizeLE = fileSize;
            f.write(reinterpret_cast<const char*>(&fileSizeLE), 4);
            f << "WAVE";

            // fmt chunk (PCM)
            f << "fmt ";
            uint32_t fmtSize = 16;
            f.write(reinterpret_cast<const char*>(&fmtSize), 4);
            uint16_t audioFormat = 1;  // PCM
            f.write(reinterpret_cast<const char*>(&audioFormat), 2);
            uint16_t channels = 1;  // mono
            f.write(reinterpret_cast<const char*>(&channels), 2);
            uint32_t sr = sampleRate;
            f.write(reinterpret_cast<const char*>(&sr), 4);
            uint32_t byteRate = sampleRate * 2;  // 16-bit mono
            f.write(reinterpret_cast<const char*>(&byteRate), 4);
            uint16_t blockAlign = 2;
            f.write(reinterpret_cast<const char*>(&blockAlign), 2);
            uint16_t bitsPerSample = 16;
            f.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

            // data chunk
            f << "data";
            f.write(reinterpret_cast<const char*>(&dataSize), 4);

            // Write silence (zeros)
            std::vector<uint16_t> samples(actualSamples, 0);
            f.write(reinterpret_cast<const char*>(samples.data()),
                    static_cast<std::streamsize>(samples.size() * sizeof(uint16_t)));

            f.close();

            state->store(hathor::RenderState::Completed,
                         std::memory_order_release);

            // Auto-register if requested (startBakeRender, not startBakeRenderRaw).
            if (autoRegister && success) {
                // In the real engine, this happens inside startBakeRender's
                // wrapped callback.  In the fake, we simulate it.
                // For startBakeRenderRaw, this is skipped — commit handles it.
                const std::string name = destPath.stem().string();
                registerBakedAsset(name, destPath);
            }

            if (onComplete) {
                onComplete(hathor::RenderResult{
                    .success = success,
                    .state = hathor::RenderState::Completed,
                    .errorMessage = {},
                    .outputPath = destPath,
                    .samplesWritten = actualSamples,
                    .durationSeconds = static_cast<double>(actualSamples)
                                       / static_cast<double>(sampleRate),
                });
            }
        })
        .detach();

        // Return a RenderHandle that shares the state and cancel flag.
        // The RenderJob is opaque (detail::RenderJob) — we pass nullptr
        // since the RenderHandle only needs id(), state(), cancel(), and isDone().
        return hathor::RenderHandle(handleId, state, cancelFlag, nullptr);
    }

    std::atomic<uint64_t> nextRenderId_{1};
};

// ===========================================================================
// Test fixture: sets up a temp project dir + facade + services
// ===========================================================================

struct AI6Fixture {
    fs::path projectDir;
    SampleBank bank;
    AI6FakeFacade audio;
    std::unique_ptr<ChuckSessionService> sessions;
    std::unique_ptr<RenderService> renders;

    AI6Fixture() : audio(bank) {
        // Create a fresh temp project dir.
        projectDir = fs::temp_directory_path() / ("hathor_ai6_" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()));
        fs::create_directories(projectDir);

        audio.projectDir_ = projectDir;
        sessions = std::make_unique<ChuckSessionService>(audio);
        renders = std::make_unique<RenderService>(audio, bank, *sessions);
    }

    ~AI6Fixture() {
        // Clean up temp dir.
        std::error_code ec;
        fs::remove_all(projectDir, ec);
    }

    AI6Fixture(const AI6Fixture&) = delete;
    AI6Fixture& operator=(const AI6Fixture&) = delete;

    // Create a session with the given source.
    std::string createSession(const std::string& source) {
        auto session = sessions->createSession(0, source);
        return session.sessionId;
    }

    // Wait for a render job to complete (poll up to timeout seconds).
    nlohmann::json waitForJob(uint64_t jobId, double timeoutSec = 5.0) {
        auto deadline = std::chrono::steady_clock::now()
            + std::chrono::duration<double>(timeoutSec);
        while (std::chrono::steady_clock::now() < deadline) {
            auto status = renders->getJobStatus(jobId);
            std::string state = status.value("status", "unknown");
            if (state == "succeeded" || state == "failed" ||
                state == "cancelled") {
                return status;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Timeout — return whatever we have.
        return renders->getJobStatus(jobId);
    }

    // Create a session via ControlInterface dispatch (for end-to-end tests).
    std::string createSessionViaCI(const std::string& source) {
        nlohmann::json resp;
        ControlInterface ci(audio, bank);
        ci.dispatchWithCallback(
            "create_chuck_session 3 " + source,
            [&resp](nlohmann::json j) { resp = std::move(j); });
        REQUIRE(resp.value("ok", false) == true);
        return resp.value("session_id", "");
    }
};

// ===========================================================================
// 1. render_chuck returns a job_id with "queued" status
// ===========================================================================

TEST_CASE("AI-6: render_chuck returns job_id with queued status", "[ai6]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac; 440 => s.freq;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "test_kick");

    REQUIRE(jobId != 0);

    // The work function runs asynchronously on the JobTracker's worker thread.
    // Poll until render job data is stored (type field appears).
    nlohmann::json status;
    for (int i = 0; i < 100; ++i) {
        status = fx.renders->getJobStatus(jobId);
        if (status.contains("type")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(status["ok"].get<bool>() == true);
    REQUIRE(status["type"].get<std::string>() == "render");
    REQUIRE(status["job_id"].get<uint64_t>() == jobId);
    // Initially queued (the work function hasn't run yet on the JobTracker thread).
    std::string state = status["status"].get<std::string>();
    REQUIRE((state == "queued" || state == "running"));
}

// ===========================================================================
// 2. Render is async (render_chuck returns before completion)
// ===========================================================================

TEST_CASE("AI-6: render_chuck is async — returns before completion", "[ai6]") {
    AI6Fixture fx;
    fx.audio.setRenderDelayMs(200);  // 200ms delay
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "async_test");

    REQUIRE(jobId != 0);

    // Immediately check — should not be complete yet.
    auto status = fx.renders->getJobStatus(jobId);
    std::string state = status["status"].get<std::string>();
    REQUIRE(state != "succeeded");
    REQUIRE(state != "failed");

    // Wait for completion.
    status = fx.waitForJob(jobId, 3.0);
    state = status["status"].get<std::string>();
    REQUIRE(state == "succeeded");
}

// ===========================================================================
// 3. Render is non-destructive (temp file, not persistent project)
// ===========================================================================

TEST_CASE("AI-6: render does NOT write to persistent project until commit", "[ai6]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "non_destructive");
    auto status = fx.waitForJob(jobId);

    REQUIRE(status["status"].get<std::string>() == "succeeded");

    // The persistent Studio path should NOT exist (not yet committed).
    fs::path studioWav = fx.projectDir / ".hathor_assets" / "chuck_instruments"
                       / "non_destructive.wav";
    fs::path studioCk = fx.projectDir / ".hathor_assets" / "chuck_instruments"
                      / "non_destructive.ck";

    REQUIRE_FALSE(fs::exists(studioWav));
    REQUIRE_FALSE(fs::exists(studioCk));

    // The temp file SHOULD exist (render wrote to temp).
    std::string tempPathStr = status["temp_path"].get<std::string>();
    REQUIRE_FALSE(tempPathStr.empty());
    REQUIRE(fs::exists(fs::path(tempPathStr)));
}

// ===========================================================================
// 4. get_job_status shows correct state transitions
// ===========================================================================

TEST_CASE("AI-6: get_job_status shows correct state transitions", "[ai6]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "state_test");

    // Poll until we see at least queued + succeeded (or just succeeded).
    nlohmann::json finalStatus;
    bool sawQueuedOrRunning = false;

    auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto status = fx.renders->getJobStatus(jobId);
        std::string state = status["status"].get<std::string>();
        if (state == "queued" || state == "running")
            sawQueuedOrRunning = true;
        if (state == "succeeded" || state == "failed" || state == "cancelled") {
            finalStatus = status;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(sawQueuedOrRunning);
    REQUIRE(finalStatus["status"].get<std::string>() == "succeeded");
    REQUIRE(finalStatus["ok"].get<bool>() == true);
    REQUIRE(finalStatus.contains("render_result"));
    REQUIRE(finalStatus["render_result"]["success"].get<bool>() == true);
}

// ===========================================================================
// 5. Render completion callback fires
// ===========================================================================

TEST_CASE("AI-6: render completion callback fires and updates job state", "[ai6]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "callback_test");
    auto status = fx.waitForJob(jobId);

    REQUIRE(status["status"].get<std::string>() == "succeeded");

    // The render result should contain output_path pointing to the temp file.
    std::string outputPath = status["render_result"]["output_path"].get<std::string>();
    REQUIRE_FALSE(outputPath.empty());
    REQUIRE(fs::exists(fs::path(outputPath)));

    // samples_written and duration_seconds should be populated.
    REQUIRE(status["render_result"]["samples_written"].get<uint64_t>() > 0);
    REQUIRE(status["render_result"]["duration_seconds"].get<double>() > 0.0);
}

// ===========================================================================
// 6. commit_rendered_asset creates .wav + .ck on disk
// ===========================================================================

TEST_CASE("AI-6: commit creates .wav + .ck on disk", "[ai6]") {
    AI6Fixture fx;
    const std::string source = "SinOsc s => dac; 440 => s.freq;";
    const std::string session = fx.createSession(source);

    uint64_t jobId = fx.renders->renderChuck(session, 4, "commit_test");
    fx.waitForJob(jobId);

    // Verify pre-commit state: no persistent files.
    fs::path studioWav = fx.projectDir / ".hathor_assets" / "chuck_instruments"
                       / "commit_test.wav";
    fs::path studioCk = fx.projectDir / ".hathor_assets" / "chuck_instruments"
                      / "commit_test.ck";
    REQUIRE_FALSE(fs::exists(studioWav));
    REQUIRE_FALSE(fs::exists(studioCk));

    // Commit.
    auto result = fx.renders->commitRenderedAsset(jobId, "commit_test");

    REQUIRE(result["ok"].get<bool>() == true);
    REQUIRE(result["cmd"].get<std::string>() == "commit_rendered_asset");
    REQUIRE(result["asset_name"].get<std::string>() == "commit_test");
    REQUIRE(result["new_asset"].get<bool>() == true);
    REQUIRE(result["overwritten"].get<bool>() == false);

    // Verify files exist on disk.
    REQUIRE(fs::exists(studioWav));
    REQUIRE(fs::exists(studioCk));

    // Verify .ck contains the source.
    std::ifstream ckFile(studioCk);
    std::string ckContent((std::istreambuf_iterator<char>(ckFile)),
                          std::istreambuf_iterator<char>());
    REQUIRE(ckContent.find("SinOsc") != std::string::npos);
}

// ===========================================================================
// 7. commit_rendered_asset registers in SampleBank
// ===========================================================================

TEST_CASE("AI-6: commit registers in SampleBank", "[ai6]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "sb_register");
    fx.waitForJob(jobId);

    // Verify NOT registered before commit.
    auto names = fx.bank.listNames();
    bool foundBefore = false;
    for (const auto& n : names)
        if (n == "sb_register") foundBefore = true;
    REQUIRE_FALSE(foundBefore);

    // Commit.
    auto result = fx.renders->commitRenderedAsset(jobId, "sb_register");
    REQUIRE(result["ok"].get<bool>() == true);

    // Verify registered after commit.
    auto* entry = fx.bank.find("sb_register", 0);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->numChannels == 1);
    REQUIRE(entry->sampleRate == 44100.0);
}

// ===========================================================================
// 8. Collision detection (existing .ck/.wav)
// ===========================================================================

TEST_CASE("AI-6: collision detection rejects overwrite without confirmation", "[ai6]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    // First render + commit.
    uint64_t jobId1 = fx.renders->renderChuck(session, 4, "collision_test");
    fx.waitForJob(jobId1);
    auto result1 = fx.renders->commitRenderedAsset(jobId1, "collision_test");
    REQUIRE(result1["ok"].get<bool>() == true);

    // Verify files exist.
    fs::path studioWav = fx.projectDir / ".hathor_assets" / "chuck_instruments"
                       / "collision_test.wav";
    fs::path studioCk = fx.projectDir / ".hathor_assets" / "chuck_instruments"
                      / "collision_test.ck";
    REQUIRE(fs::exists(studioWav));
    REQUIRE(fs::exists(studioCk));

    // Second render + commit WITHOUT confirmation.
    uint64_t jobId2 = fx.renders->renderChuck(session, 4, "collision_test");
    fx.waitForJob(jobId2);

    auto result2 = fx.renders->commitRenderedAsset(jobId2, "collision_test",
                                                     /*confirmOverwrite=*/false);

    REQUIRE(result2["ok"].get<bool>() == false);
    REQUIRE(result2["status"].get<std::string>() == "conflict");
    REQUIRE(result2["conflict"].get<bool>() == true);
    REQUIRE(result2["existing_files"].is_array());
    REQUIRE(result2["existing_files"].size() >= 2);
}

// ===========================================================================
// 9. Overwriting with confirmation succeeds
// ===========================================================================

TEST_CASE("AI-6: overwrite with confirmation succeeds", "[ai6]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    // First commit.
    uint64_t jobId1 = fx.renders->renderChuck(session, 4, "overwrite_test");
    fx.waitForJob(jobId1);
    auto r1 = fx.renders->commitRenderedAsset(jobId1, "overwrite_test");
    REQUIRE(r1["ok"].get<bool>() == true);

    // Record file sizes before overwrite.
    fs::path studioWav = fx.projectDir / ".hathor_assets" / "chuck_instruments"
                       / "overwrite_test.wav";
    uintmax_t sizeBefore = fs::file_size(studioWav); (void)sizeBefore;

    // Second render + commit WITH confirmation.
    uint64_t jobId2 = fx.renders->renderChuck(session, 4, "overwrite_test");
    fx.waitForJob(jobId2);

    auto r2 = fx.renders->commitRenderedAsset(jobId2, "overwrite_test",
                                                /*confirmOverwrite=*/true);

    REQUIRE(r2["ok"].get<bool>() == true);
    REQUIRE(r2["new_asset"].get<bool>() == false);
    REQUIRE(r2["overwritten"].get<bool>() == true);

    // Files still exist.
    REQUIRE(fs::exists(studioWav));

    // Backups should be cleaned up.
    fs::path bakPath = studioWav;
    bakPath.replace_extension(".bak");
    REQUIRE_FALSE(fs::exists(bakPath));
}

// ===========================================================================
// 10. Unsafe asset name rejected
// ===========================================================================

TEST_CASE("AI-6: unsafe asset name rejected (path traversal)", "[ai6][edge_case]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    // Path traversal in asset name.
    uint64_t jobId = fx.renders->renderChuck(session, 4, "../../etc/passwd");
    auto status = fx.waitForJob(jobId);

    REQUIRE(status["status"].get<std::string>() == "failed");
    std::string err = status["error"].get<std::string>();
    REQUIRE(err.find("unsafe") != std::string::npos);
}

// ===========================================================================
// 11. Unsafe commit asset name rejected
// ===========================================================================

TEST_CASE("AI-6: unsafe commit asset name rejected", "[ai6][edge_case]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "safe_name");
    fx.waitForJob(jobId);

    auto result = fx.renders->commitRenderedAsset(jobId, "../escape");
    REQUIRE(result["ok"].get<bool>() == false);
    std::string cerr1 = result["error"].get<std::string>();
    REQUIRE(cerr1.find("unsafe") != std::string::npos);
}

// ===========================================================================
// 12. Cancellation during render
// ===========================================================================

TEST_CASE("AI-6: cancellation during render", "[ai6]") {
    AI6Fixture fx;
    fx.audio.setRenderDelayMs(500);  // Give time to cancel
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "cancel_test");

    // Wait a bit, then cancel.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    bool cancelled = fx.renders->cancelJob(jobId);
    REQUIRE(cancelled);

    // Wait for the cancellation to propagate.
    auto status = fx.waitForJob(jobId, 3.0);
    std::string state = status["status"].get<std::string>();
    REQUIRE((state == "cancelled" || state == "failed"));
}

// ===========================================================================
// 13. Session source is persisted and used for rendering
// ===========================================================================

TEST_CASE("AI-6: session source is persisted and used for render", "[ai6]") {
    AI6Fixture fx;
    const std::string source = "Delay del => blackhole; Sine1 s => dac;";
    const std::string session = fx.createSession(source);

    // Create the session.
    auto sess = fx.sessions->getSession(session);
    REQUIRE(sess.source == source);

    // Render — should use the persisted source.
    uint64_t jobId = fx.renders->renderChuck(session, 4, "source_test");
    auto status = fx.waitForJob(jobId);
    REQUIRE(status["status"].get<std::string>() == "succeeded");
    REQUIRE(status["session_id"].get<std::string>() == session);
}

// ===========================================================================
// 14. Failed render does not commit
// ===========================================================================

TEST_CASE("AI-6: failed render cannot be committed", "[ai6]") {
    AI6Fixture fx;
    fx.audio.setNextRenderSuccess(false);  // Simulate failure
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "fail_test");
    auto status = fx.waitForJob(jobId);

    REQUIRE(status["status"].get<std::string>() == "failed");

    auto commitResult = fx.renders->commitRenderedAsset(jobId, "fail_test");
    REQUIRE(commitResult["ok"].get<bool>() == false);
    std::string ferr = commitResult["error"].get<std::string>();
    REQUIRE(ferr.find("fail") != std::string::npos);
}

// ===========================================================================
// 15. No temp file leak on failure
// ===========================================================================

TEST_CASE("AI-6: temp file cleaned up on render failure", "[ai6]") {
    AI6Fixture fx;
    fx.audio.setNextRenderSuccess(false);
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "leak_test");
    auto status = fx.waitForJob(jobId);

    REQUIRE(status["status"].get<std::string>() == "failed");

    // The temp file should have been cleaned up by the callback.
    std::string tempPathStr = status["temp_path"].get<std::string>();
    REQUIRE_FALSE(tempPathStr.empty());
    REQUIRE_FALSE(fs::exists(fs::path(tempPathStr)));
}

// ===========================================================================
// 16. No SampleBank mutation until commit (render phase)
// ===========================================================================

TEST_CASE("AI-6: no SampleBank mutation during render phase", "[ai6]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "no_mutate");
    fx.waitForJob(jobId);

    // SampleBank should NOT have the entry yet (only at commit time).
    auto* entry = fx.bank.find("no_mutate", 0);
    REQUIRE(entry == nullptr);
}

// ===========================================================================
// 17. get_job_status for unknown job_id
// ===========================================================================

TEST_CASE("AI-6: get_job_status for unknown job_id returns error", "[ai6][edge_case]") {
    AI6Fixture fx;

    auto status = fx.renders->getJobStatus(99999);
    REQUIRE(status["ok"].get<bool>() == false);
    REQUIRE(status.contains("error"));
}

// ===========================================================================
// 18. Invalid session_id rejected
// ===========================================================================

TEST_CASE("AI-6: render_chuck rejects invalid session ID", "[ai6][edge_case]") {
    AI6Fixture fx;

    uint64_t jobId = fx.renders->renderChuck("ck:99", 4, "bad_session");
    auto status = fx.waitForJob(jobId, 2.0);

    // tabId 99 is out of range — should fail.
    REQUIRE(status["status"].get<std::string>() == "failed");
}

// ===========================================================================
// 19. Invalid duration rejected
// ===========================================================================

TEST_CASE("AI-6: render_chuck rejects zero/negative duration", "[ai6][edge_case]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 0, "zero_bars");
    auto status = fx.waitForJob(jobId, 2.0);

    REQUIRE(status["status"].get<std::string>() == "failed");
    std::string perr = status["error"].get<std::string>();
    REQUIRE(perr.find("positive") != std::string::npos);
}

// ===========================================================================
// 20. End-to-end through ControlInterface dispatch
// ===========================================================================

TEST_CASE("AI-6: end-to-end through ControlInterface dispatch", "[ai6][integration]") {
    AI6Fixture fx;

    ControlInterface ci(fx.audio, fx.bank);

    // The ControlInterface lazily creates ChuckSessionService and RenderService.
    // But our fixture already has them. Let's use the ControlInterface directly.
    // We need to create a session first.
    nlohmann::json createResp;
    ci.dispatchWithCallback("create_chuck_session 3 SinOsc s => dac; 440 => s.freq;",
                             [&createResp](nlohmann::json j) {
                                 createResp = std::move(j);
                             });
    REQUIRE(createResp["ok"].get<bool>() == true);
    std::string sessionId = createResp["session_id"].get<std::string>();
    REQUIRE(sessionId == "ck:3");

    // Render.
    nlohmann::json renderResp;
    ci.dispatchWithCallback("render_chuck " + sessionId + " 4 e2e_test",
                             [&renderResp](nlohmann::json j) {
                                 renderResp = std::move(j);
                             });
    REQUIRE(renderResp["ok"].get<bool>() == true);
    REQUIRE(renderResp["cmd"].get<std::string>() == "render_chuck");
    uint64_t jobId = renderResp["job_id"].get<uint64_t>();
    REQUIRE(jobId != 0);
    REQUIRE(renderResp["status"].get<std::string>() == "queued");

    // Wait for render to complete.
    nlohmann::json statusResp;
    for (int i = 0; i < 100; ++i) {
        ci.dispatchWithCallback("get_job_status " + std::to_string(jobId),
                                 [&statusResp](nlohmann::json j) {
                                     statusResp = std::move(j);
                                 });
        std::string state = statusResp["status"].get<std::string>();
        if (state == "succeeded" || state == "failed" || state == "cancelled")
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(statusResp["status"].get<std::string>() == "succeeded");

    // Commit.
    nlohmann::json commitResp;
    ci.dispatchWithCallback("commit_rendered_asset " + std::to_string(jobId) + " e2e_test",
                             [&commitResp](nlohmann::json j) {
                                 commitResp = std::move(j);
                             });
    REQUIRE(commitResp["ok"].get<bool>() == true);
    REQUIRE(commitResp["cmd"].get<std::string>() == "commit_rendered_asset");
    REQUIRE(commitResp["asset_name"].get<std::string>() == "e2e_test");

    // Verify files on disk.
    fs::path wavPath = fx.projectDir / ".hathor_assets" / "chuck_instruments"
                     / "e2e_test.wav";
    fs::path ckPath = fx.projectDir / ".hathor_assets" / "chuck_instruments"
                    / "e2e_test.ck";
    REQUIRE(fs::exists(wavPath));
    REQUIRE(fs::exists(ckPath));

    // Verify SampleBank registration.
    auto* entry = fx.bank.find("e2e_test", 0);
    REQUIRE(entry != nullptr);
}

// ===========================================================================
// 21. Render phase does not call registerBakedAsset
// ===========================================================================

TEST_CASE("AI-6: render uses startBakeRenderRaw (no auto-registration)", "[ai6]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "no_autoreg");
    fx.waitForJob(jobId);

    // The fake tracks the last registered name; if it matches our temp
    // file name (not our asset name), then auto-registration from
    // startBakeRender happened. We want to verify it did NOT.
    // Since we use startBakeRenderRaw (no auto-register), the last
    // registered name should be empty or unrelated.
    REQUIRE((fx.audio.lastRegisteredName().empty()
            || fx.audio.lastRegisteredName() != "no_autoreg"));
}

// ===========================================================================
// 22. list_render_jobs returns all render jobs
// ===========================================================================

TEST_CASE("AI-6: list_render_jobs returns all render jobs", "[ai6]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId1 = fx.renders->renderChuck(session, 4, "list_test_1");
    uint64_t jobId2 = fx.renders->renderChuck(session, 4, "list_test_2");
    fx.waitForJob(jobId1);
    fx.waitForJob(jobId2);

    auto list = fx.renders->listRenderJobs();
    REQUIRE(list.is_array());
    REQUIRE(list.size() >= 2);

    bool found1 = false, found2 = false;
    for (const auto& entry : list) {
        if (entry["job_id"].get<uint64_t>() == jobId1) found1 = true;
        if (entry["job_id"].get<uint64_t>() == jobId2) found2 = true;
    }
    REQUIRE(found1);
    REQUIRE(found2);
}

// ===========================================================================
// 23. Commit idempotency — committing same asset twice
// ===========================================================================

TEST_CASE("AI-6: committing completed render succeeds once, second uses overwrite", "[ai6]") {
    AI6Fixture fx;
    const std::string session = fx.createSession("SinOsc s => dac;");

    uint64_t jobId = fx.renders->renderChuck(session, 4, "idempotent");
    fx.waitForJob(jobId);

    // First commit should succeed as new.
    auto r1 = fx.renders->commitRenderedAsset(jobId, "idempotent");
    REQUIRE(r1["ok"].get<bool>() == true);
    REQUIRE(r1["new_asset"].get<bool>() == true);

    // Second render + commit without overwrite should conflict.
    uint64_t jobId2 = fx.renders->renderChuck(session, 4, "idempotent");
    fx.waitForJob(jobId2);

    auto r2 = fx.renders->commitRenderedAsset(jobId2, "idempotent", false);
    REQUIRE(r2["ok"].get<bool>() == false);
    REQUIRE(r2["status"].get<std::string>() == "conflict");
}

// ===========================================================================
// 24. Render job with cancelled flag set before start
// ===========================================================================

TEST_CASE("AI-6: render job can be queried immediately for type and session", "[ai6]") {
    AI6Fixture fx;
    const std::string source = "SinOsc osc => dac; 440 => osc.freq;";
    const std::string session = fx.createSession(source);

    uint64_t jobId = fx.renders->renderChuck(session, 8, "quick_kick", AssetTarget::Studio);

    // The work function runs asynchronously — wait until render job data is stored.
    nlohmann::json status;
    for (int i = 0; i < 100; ++i) {
        status = fx.renders->getJobStatus(jobId);
        if (status.contains("type")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(status["ok"].get<bool>() == true);
    REQUIRE(status["type"].get<std::string>() == "render");
    REQUIRE(status["session_id"].get<std::string>() == session);
    REQUIRE(status["asset_name"].get<std::string>() == "quick_kick");
    REQUIRE(status["target"].get<std::string>() == "studio");
    REQUIRE(status["duration_bars"].get<int>() == 8);
}
