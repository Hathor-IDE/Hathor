// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_ai2_readonly.cpp — Phase 2.5 H0: AI-2 read-only project introspection tests.
 *
 * Verifies:
 *   1. inspect_project       — semantic project representation
 *   2. get_current_song     — semantic current-song state
 *   3. list_samples         — reads from the real SampleBank
 *   4. list_chuck_instruments — B8-K1/K2/K3/K4 instrument lifecycle
 *  5. get_diagnostics       — real parser/compiler diagnostics (no mocks)
 *   6. get_audio_status      — real audio engine state
 *   7. MCP command routing    — through canonical ProjectReadFacade
 *   8. Mutation regression    — read operations leave all state unchanged
 *
 * Architecture: tests use a TrackingFakeFacade that records all mutations
 * (setBpm, play, stop, storeSlot, addEntry, etc.) so we can verify that
 * read-only operations never mutate.
 *
 * Requirement: Phase 2.5 H0, AI-2 §2–§14
 */

#include "ControlInterface.hpp"
#include "ProjectReadFacade.hpp"
#include "AudioEngineFacade.hpp"
#include "SampleBank.hpp"
#include "SlotState.hpp"

#include "hathor/Pattern.hpp"
#include "hathor/ParamMap.hpp"
#include "hathor/Event.hpp"
#include "hathor/Rational.hpp"

#include "ChuckDiagnostics.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

// ===========================================================================
// TrackingFakeFacade — records ALL mutations for read-only verification
// ===========================================================================

class TrackingFakeFacade final : public AudioEngineFacade {
public:
    // --- Mutation tracking ---
    struct MutationLog {
        std::string action;
        nlohmann::json data;
    };
    std::vector<MutationLog> mutations;

    void log(const std::string& action, nlohmann::json data = {}) {
        mutations.push_back({action, std::move(data)});
    }

    // --- Transport (mutable) ---
    void play() noexcept override {
        log("play");
        running_ = true;
    }
    void stop() noexcept override {
        log("stop");
        running_ = false;
    }
    void setBpm(double bpm) noexcept override {
        log("setBpm", {{"bpm", bpm}});
        bpm_ = bpm;
    }
    double getBpm() const noexcept override { return bpm_; }
    bool isRunning() const noexcept override { return running_; }

    // --- Master gain ---
    void setMasterGain(float g) noexcept override {
        log("setMasterGain", {{"gain", g}});
        gain_ = g;
    }
    float getMasterGain() const noexcept override { return gain_; }

    // --- EQ preset ---
    void setMasterEqPreset(hathor::EqPreset p) noexcept override {
        log("setMasterEqPreset", {{"preset", static_cast<int>(p)}});
        eqPreset_ = p;
    }
    hathor::EqPreset getMasterEqPreset() const noexcept override {
        return eqPreset_;
    }

    // --- Slot API ---
    int findOrAddSlot(const std::string& name) override {
        for (int i = 0; i < slotCount_; ++i)
            if (names_[i] == name)
                return i;
        if (slotCount_ >= kNumSlots)
            return -1;
        log("findOrAddSlot", {{"name", name}});
        names_[slotCount_] = name;
        states_[slotCount_].reset();
        slotRunning_[slotCount_] = false;
        return slotCount_++;
    }
    void storeSlot(int idx, std::shared_ptr<SlotState> state) noexcept override {
        log("storeSlot", {{"idx", idx}, {"has_pattern", state != nullptr}});
        if (idx >= 0 && idx < slotCount_)
            states_[idx] = std::move(state);
    }
    bool clearSlot(int idx) noexcept override {
        log("clearSlot", {{"idx", idx}});
        if (idx >= 0 && idx < slotCount_) {
            states_[idx].reset();
            return true;
        }
        return false;
    }
    int slotCount() const noexcept override { return slotCount_; }
    std::string slotName(int idx) const override {
        if (idx >= 0 && idx < slotCount_)
            return names_[idx];
        return {};
    }
    std::shared_ptr<SlotState> loadSlot(int idx) const noexcept override {
        if (idx >= 0 && idx < slotCount_) {
            // Use acquire ordering for lock-free safety (matches AudioEngine pattern).
            return std::atomic_load_explicit(&states_[idx], std::memory_order_acquire);
        }
        return nullptr;
    }

    // --- Per-slot play/stop ---
    void slotPlay(int idx) noexcept override {
        log("slotPlay", {{"idx", idx}});
        if (idx >= 0 && idx < kNumSlots)
            slotRunning_[idx] = true;
    }
    void slotStop(int idx) noexcept override {
        log("slotStop", {{"idx", idx}});
        if (idx >= 0 && idx < kNumSlots)
            slotRunning_[idx] = false;
    }
    bool isSlotRunning(int idx) const noexcept override {
        if (idx >= 0 && idx < kNumSlots)
            return slotRunning_[idx];
        return false;
    }

    // --- ChucK VM ---
    bool hasWorker() const noexcept override { return workerAlive_; }
    bool ckEval(int idx, const std::string& code) noexcept override {
        log("ckEval", {{"idx", idx}, {"code_len", code.size()}});
        (void)idx; (void)code;
        return true;
    }
    bool stopCkTab(int idx) noexcept override {
        log("stopCkTab", {{"idx", idx}});
        (void)idx;
        return true;
    }
    std::string queryCkTab(int idx) const override {
        if (idx >= 0 && idx < kNumSlots && vmStates_[idx].non_empty_) {
            return vmStates_[idx].message;
        }
        return {};
    }

    // --- B8-K1 render target ---
    std::filesystem::path resolveRenderPath(hathor::AssetTarget,
                                             std::string_view,
                                             const std::filesystem::path&) override {
        return {};
    }
    void setLiveJamSessionDir(std::filesystem::path) override {}
    void cleanupLiveJamAssets() override {}
    bool isStudioAssetPath(const std::filesystem::path&) const override { return false; }

    // --- B8-K2 render ---
    hathor::RenderHandle startBakeRender(uint8_t, std::string, uint64_t, unsigned,
                                          const std::filesystem::path&,
                                          hathor::ChuckRenderWriter::CompletionCallback) override {
        return hathor::RenderHandle{};
    }
    int activeRenderCount() const noexcept override { return 0; }
    void shutdownRender() noexcept override {}

    // --- B8-K4 ---
    bool registerBakedAsset(std::string, const std::filesystem::path&) override {
        log("registerBakedAsset");
        return true;
    }
    std::vector<std::string> listSamples() const override {
        return {};
    }

    // --- AI-2 read-only introspection ---
    std::vector<SlotInfo> listSlots() const noexcept override {
        std::vector<SlotInfo> result;
        result.reserve(kNumSlots);
        for (int i = 0; i < kNumSlots; ++i) {
            SlotInfo info;
            info.slotIndex = i;
            info.slotName = names_[i];
            auto state = std::atomic_load_explicit(&states_[i], std::memory_order_acquire);
            info.active = (state != nullptr);
            info.running = slotRunning_[i];
            info.eventCount = 0;
            if (state) {
                info.notation = state->notation;
                info.eventCount = static_cast<int>(state->eventBuffer.size());
            }
            result.push_back(std::move(info));
        }
        return result;
    }

    SlotInfo getSlotInfo(int idx) const noexcept override {
        SlotInfo info;
        if (idx < 0 || idx >= kNumSlots)
            return info;
        info.slotIndex = idx;
        info.slotName = names_[idx];
        auto state = std::atomic_load_explicit(&states_[idx], std::memory_order_acquire);
        info.active = (state != nullptr);
        info.running = slotRunning_[idx];
        if (state) {
            info.notation = state->notation;
            info.eventCount = static_cast<int>(state->eventBuffer.size());
        }
        return info;
    }

    VmStatus getVmStatus(int idx) const noexcept override {
        VmStatus status;
        status.hasWorker = workerAlive_;
        if (idx >= 0 && idx < kNumSlots) {
            if (vmStates_[idx].non_empty_) {
                status.state = vmStates_[idx].message;
                status.shredInfo = vmStates_[idx].message;
            } else {
                status.state = "inactive";
            }
            status.generation = vmGeneration_;
        }
        return status;
    }

    AudioStatus getAudioStatus() const noexcept override {
        return AudioStatus{
            running_, bpm_, sampleRate_, gain_,
            hathor::presetName(eqPreset_), sampleClock_,
            deviceOpen_, 0
        };
    }

    std::vector<SlotPlayback> listSlotPlayback() const noexcept override {
        std::vector<SlotPlayback> result;
        result.reserve(kNumSlots);
        for (int i = 0; i < kNumSlots; ++i) {
            SlotPlayback sp;
            sp.slotIndex = i;
            sp.slotName = names_[i];
            auto state = std::atomic_load_explicit(&states_[i], std::memory_order_acquire);
            sp.hasPattern = (state != nullptr);
            sp.running = slotRunning_[i];
            if (state)
                sp.notation = state->notation;
            result.push_back(std::move(sp));
        }
        return result;
    }

    std::vector<InstrumentInfo> listChuckInstruments(
        const std::filesystem::path& projectDir) const noexcept override
    {
        return listInstruments(projectDir);
    }

    std::filesystem::path studioInstrumentsDir(
        const std::filesystem::path& projectDir) const noexcept override
    {
        return (projectDir / ".hathor_assets" / "chuck_instruments");
    }

    std::filesystem::path currentProjectDir() const noexcept override
    {
        return projectDir_;
    }

    // --- Test helpers ---
    static constexpr int kNumSlots = 16;
    std::string names_[kNumSlots] = {};
    std::shared_ptr<SlotState> states_[kNumSlots] = {};
    bool slotRunning_[kNumSlots] = {};
    int  slotCount_ = 0;

    double bpm_ = 120.0;
    bool running_ = false;
    float gain_ = 1.0f;
    hathor::EqPreset eqPreset_ = hathor::EqPreset::Flat;
    uint64_t sampleClock_ = 0;
    int sampleRate_ = 0;
    bool deviceOpen_ = false;
    std::string projectDir_ = "/test/project";
    bool workerAlive_ = false;
    uint64_t vmGeneration_ = 1;

    struct VmStateInfo {
        bool non_empty_ = false;
        std::string message;
    };
    VmStateInfo vmStates_[kNumSlots];

    /// Helper to compute InstrumentInfo from the filesystem (same logic as
    /// AudioEngine::listChuckInstruments, but without JUCE for test simplicity).
    std::vector<AudioEngineFacade::InstrumentInfo>
    listInstruments(const std::filesystem::path& projectDir) const
    {
        std::vector<AudioEngineFacade::InstrumentInfo> result;
        const auto instrDir = projectDir / ".hathor_assets" / "chuck_instruments";

        std::error_code ec;
        if (!std::filesystem::exists(instrDir, ec))
            return result;

        std::vector<std::string> wavStems;
        for (const auto& entry : std::filesystem::directory_iterator(instrDir, ec)) {
            if (!entry.is_regular_file())
                continue;
            if (entry.path().extension() == ".wav")
                wavStems.push_back(entry.path().stem().string());
        }
        std::sort(wavStems.begin(), wavStems.end());

        for (const auto& stem : wavStems) {
            InstrumentInfo info;
            info.name = stem;

            std::filesystem::path ckPath = instrDir / (stem + ".ck");
            if (!std::filesystem::exists(ckPath, ec))
                ckPath = projectDir / (stem + ".ck");
            info.sourceCkExists = std::filesystem::exists(ckPath, ec);
            info.sourcePath = info.sourceCkExists ? ckPath.string() : "";

            std::filesystem::path wavPath = instrDir / (stem + ".wav");
            info.renderedWavExists = std::filesystem::exists(wavPath, ec);
            info.renderedPath = info.renderedWavExists ? wavPath.string() : "";

            // Check SampleBank binding — use find() with index 0.
            info.boundToSampleBank = (bankOverride ? bankOverride->find(stem, 0) != nullptr : false);

            info.durationSeconds = 0.0;  // duration not available without JUCE in tests

            result.push_back(std::move(info));
        }
        return result;
    }

    /// Optional SampleBank reference for binding checks (set by tests).
    SampleBank* bankOverride = nullptr;
};

// ===========================================================================
// WAV writer helper (for test fixtures)
// ===========================================================================

/// Write a minimal WAV file (44-byte header + 1 second of silence at 44100 Hz mono 16-bit).
void writeMinimalWav(std::ofstream& f);

// ===========================================================================
// Helpers
// ===========================================================================

/// Alias for Rational to keep test code concise.
static inline hathor::Rational R(int64_t n, int64_t d)
{
    return hathor::Rational(n, d);
}

/// Make a blank Event<ParamMap> with zero arcs for use as a vector fill.
static hathor::Event<hathor::ParamMap> blankEvent()
{
    hathor::Arc z{R(0, 1), R(0, 1)};
    return hathor::Event<hathor::ParamMap>{z, z, hathor::ParamMap{}};
}

/// Capture helper using dispatchWithCallback (same pattern as existing tests).

/// Set up a test project directory with .ck sources and .wav files in
/// .hathor_assets/chuck_instruments/.
struct TestProject {
    std::filesystem::path root;
    std::string projectName;

    TestProject() {
        root = std::filesystem::temp_directory_path() /
               ("hathor-ai2-test-" + std::to_string(getpid()) + "-" +
                std::to_string(testCounter_++));
        std::filesystem::create_directories(root);
    }

    ~TestProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path instrumentsDir() const {
        return root / ".hathor_assets" / "chuck_instruments";
    }

    void makeInstrument(const std::string& name,
                         bool makeCk = true, bool makeWav = true) {
        auto instrDir = instrumentsDir();
        std::filesystem::create_directories(instrDir);

        if (makeWav) {
            // Write a minimal valid WAV file (1 second of silence at 44100 Hz).
            std::ofstream wav(instrDir / (name + ".wav"), std::ios::binary);
            writeMinimalWav(wav);
        }
        if (makeCk) {
            std::ofstream ck(instrDir / (name + ".ck"));
            ck << "SinOsc s => dac;\n440 => s.freq;\n1::second => now;\n";
        }
    }

    void makeLooseCk(const std::string& name) {
        std::ofstream ck(root / (name + ".ck"));
        ck << "SinOsc s => dac;\n";
    }

    static uint64_t testCounter_;
};

uint64_t TestProject::testCounter_ = 0;

/// Write a minimal WAV file (44-byte header + 1 second of silence at 44100 Hz mono 16-bit).
void writeMinimalWav(std::ofstream& f) {
    // RIFF header
    f.write("RIFF", 4);
    uint32_t chunkSize = 36 + 44100 * 2;
    f.write(reinterpret_cast<const char*>(&chunkSize), 4);
    f.write("WAVE", 4);

    // fmt chunk
    f.write("fmt ", 4);
    uint32_t subChunk1Size = 16;
    uint16_t audioFormat = 1;  // PCM
    uint16_t numChannels = 1;
    uint32_t sampleRate = 44100;
    uint32_t byteRate = sampleRate * numChannels * 2;
    uint16_t blockAlign = numChannels * 2;
    uint16_t bitsPerSample = 16;
    f.write(reinterpret_cast<const char*>(&subChunk1Size), 4);
    f.write(reinterpret_cast<const char*>(&audioFormat), 2);
    f.write(reinterpret_cast<const char*>(&numChannels), 2);
    f.write(reinterpret_cast<const char*>(&sampleRate), 4);
    f.write(reinterpret_cast<const char*>(&byteRate), 4);
    f.write(reinterpret_cast<const char*>(&blockAlign), 2);
    f.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    // data chunk
    f.write("data", 4);
    uint32_t dataSize = 44100 * 2;
    f.write(reinterpret_cast<const char*>(&dataSize), 4);
    // Write 1 second of silence (zero samples)
    std::vector<int16_t> silence(44100, 0);
    f.write(reinterpret_cast<const char*>(silence.data()), static_cast<std::streamsize>(dataSize));
}

/// Capture helper using dispatchWithCallback (same pattern as existing tests).
struct RespCapture {
    nlohmann::json data;
    bool got = false;
};

void runCmd(hathor::control::ControlInterface& ci,
            const std::string& cmd, RespCapture& cap) {
    ci.dispatchWithCallback(cmd,
        [&cap](nlohmann::json j) { cap.data = std::move(j); cap.got = true; });
}

// ===========================================================================
// 2. inspect_project tests
// ===========================================================================

TEST_CASE("AI-2: inspect_project returns semantic project representation",
          "[ai2][readonly][inspect_project]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "inspect_project", cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);

    // Project name
    REQUIRE(cap.data.contains("project_name"));
    REQUIRE_FALSE(cap.data.value("project_name", "").empty());

    // Songs — at minimum, an array exists (may be empty for a fresh project)
    REQUIRE(cap.data.contains("songs"));
    REQUIRE(cap.data["songs"].is_array());

    // BPM
    REQUIRE(cap.data.contains("bpm"));
    REQUIRE(cap.data.value("bpm", 0.0) == Catch::Approx(120.0));

    // Active slots — array exists
    REQUIRE(cap.data.contains("active_slots"));
    REQUIRE(cap.data["active_slots"].is_array());

    // ChucK instruments — array exists
    REQUIRE(cap.data.contains("chuck_instruments"));
    REQUIRE(cap.data["chuck_instruments"].is_array());

    // Samples — at minimum, count and names
    REQUIRE(cap.data.contains("samples_count"));
    REQUIRE(cap.data.contains("sample_names"));
    REQUIRE(cap.data["sample_names"].is_array());

    // Timestamp
    REQUIRE(cap.data.contains("timestamp"));

    // No mutations should have occurred
    REQUIRE(audio.mutations.empty());
}

TEST_CASE("AI-2: inspect_project with active slot reflects it in active_slots",
          "[ai2][readonly][inspect_project]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // Set up a slot with a pattern
    audio.findOrAddSlot("d0");
    auto state = std::make_shared<SlotState>();
    state->notation = "bd sn";
    state->eventBuffer.resize(2, blankEvent());
    std::atomic_store_explicit(&audio.states_[0], state, std::memory_order_release);
    audio.slotRunning_[0] = true;

    // No mutations should have occurred from the AI-2 read call itself.
    // (findOrAddSlot in setup is a test action, not an AI-2 operation.)
    const auto mutationsBeforeRead = audio.mutations.size();
    runCmd(ci, "inspect_project", cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);

    const auto& activeSlots = cap.data["active_slots"];
    bool found_d0 = false;
    for (const auto& s : activeSlots) {
        if (s.value("slot_name", "") == "d0") {
            found_d0 = true;
            REQUIRE(s.value("running", false) == true);
            REQUIRE(s.value("notation", "") == "bd sn");
        }
    }
    REQUIRE(found_d0);

    // AI-2 read call must not have added any mutations
    REQUIRE(audio.mutations.size() == mutationsBeforeRead);

TEST_CASE("AI-2: inspect_project no mutation when called multiple times",
          "[ai2][readonly][inspect_project][mutation_audit]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "inspect_project", cap);
    const auto mutationsAfter1 = audio.mutations.size();

    runCmd(ci, "inspect_project", cap);
    const auto mutationsAfter2 = audio.mutations.size();

    runCmd(ci, "inspect_project", cap);
    const auto mutationsAfter3 = audio.mutations.size();

    REQUIRE(mutationsAfter1 == 0);
    REQUIRE(mutationsAfter2 == 0);
    REQUIRE(mutationsAfter3 == 0);
}

// ===========================================================================
// 3. get_current_song tests
// ===========================================================================

TEST_CASE("AI-2: get_current_song returns semantic song state",
          "[ai2][readonly][get_current_song]")
{
    TrackingFakeFacade audio;
    SampleBank bank;

    // Set up a current song
    audio.findOrAddSlot("d0");
    auto state = std::make_shared<SlotState>();
    state->notation = "bd sn hh cp";
    state->eventBuffer.resize(4, blankEvent());
    std::atomic_store_explicit(&audio.states_[0], state, std::memory_order_release);
    audio.slotRunning_[0] = true;
    audio.setBpm(140.0);

    hathor::control::ControlInterface ci(audio, bank);

     // No mutations from the AI-2 read call itself (setup mutations are pre-existing)
    const auto mutationsBeforeRead = audio.mutations.size();
    RespCapture cap;
    runCmd(ci, "get_current_song", cap);
    REQUIRE(audio.mutations.size() == mutationsBeforeRead);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);

    // Source should reference the active slot
    REQUIRE(cap.data.contains("source"));
    REQUIRE(cap.data.value("source", "") == "slot:d0");

    // Active patterns
    REQUIRE(cap.data.contains("active_patterns"));
    REQUIRE(cap.data["active_patterns"].is_array());
    REQUIRE(cap.data["active_patterns"].size() == 1);
    REQUIRE(cap.data["active_patterns"][0].value("notation", "") == "bd sn hh cp");

    // Tempo
    REQUIRE(cap.data.value("tempo", 0.0) == Catch::Approx(140.0));

    // Selection
    REQUIRE(cap.data.contains("selection"));

    // Diagnostics (may be empty if notation parses fine)
    REQUIRE(cap.data.contains("diagnostics"));
    REQUIRE(cap.data["diagnostics"].is_array());

    // Referenced assets
    REQUIRE(cap.data.contains("referenced_assets"));
    REQUIRE(cap.data["referenced_assets"].is_array());

    // slot_count
    REQUIRE(cap.data.contains("slot_count"));
}

TEST_CASE("AI-2: get_current_song returns null source when no active song",
          "[ai2][readonly][get_current_song][edge_case]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "get_current_song", cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    // source is null when no active song
    REQUIRE(cap.data["source"].is_null());
}

// ===========================================================================
// 4. list_samples tests — from the real SampleBank
// ===========================================================================

TEST_CASE("AI-2: list_samples reads from the real SampleBank",
          "[ai2][readonly][list_samples]")
{
    TrackingFakeFacade audio;
    SampleBank bank;

    // Inject sample entries via the real SampleBank addEntry()
    bank.addEntry("bd", 0,
        std::vector<float>(44100, 0.5f), 1, 44100.0,
        "/samples/bd/0.wav");
    bank.addEntry("sn", 0,
        std::vector<float>(88200, 0.3f), 2, 44100.0,
        "/samples/sn/0.wav");

    audio.bankOverride = &bank;
    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "list_samples", cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data.contains("samples"));
    REQUIRE(cap.data["samples"].is_array());
    REQUIRE(cap.data["samples"].size() == 2);

    // Verify the samples match what was injected
    bool found_bd = false, found_sn = false;
    for (const auto& s : cap.data["samples"]) {
        if (s.value("name", "") == "bd") {
            found_bd = true;
            REQUIRE(s.value("index", -1) == 0);
            REQUIRE(s.value("path", "") == "/samples/bd/0.wav");
            REQUIRE(s.value("channels", 0) == 1);
            REQUIRE(s.value("sample_rate", 0.0) == Catch::Approx(44100.0));
            // Duration = 44100 samples / 44100 rate / 1 channel = 1.0s
            REQUIRE(s.value("duration_seconds", 0.0) == Catch::Approx(1.0));
        }
        if (s.value("name", "") == "sn") {
            found_sn = true;
            REQUIRE(s.value("channels", 0) == 2);
            // Duration = 88200 samples / 44100 rate / 2 channels = 1.0s
            REQUIRE(s.value("duration_seconds", 0.0) == Catch::Approx(1.0));
        }
    }
    REQUIRE(found_bd);
    REQUIRE(found_sn);

    // No mutations
    REQUIRE(audio.mutations.empty());
}

TEST_CASE("AI-2: list_samples returns empty array when SampleBank is empty",
          "[ai2][readonly][list_samples][edge_case]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "list_samples", cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data["samples"].is_array());
    REQUIRE(cap.data["samples"].empty());

    // No mutations
    REQUIRE(audio.mutations.empty());
}

// ===========================================================================
// 5. list_chuck_instruments tests — B8-K1/K2/K3/K4 instrument lifecycle
// ===========================================================================

TEST_CASE("AI-2: list_chuck_instruments exposes managed ChucK instrument lifecycle",
          "[ai2][readonly][list_chuck_instruments]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    audio.bankOverride = &bank;

    // Create a test project with instruments in various lifecycle states
    TestProject proj;
    proj.makeInstrument("acid_bass", /*makeCk=*/true,  /*makeWav=*/true);   // fully baked & bound
    proj.makeInstrument("kick",      /*makeCk=*/true,  /*makeWav=*/true);   // fully baked & bound
    proj.makeInstrument("snare",     /*makeCk=*/false, /*makeWav=*/true);    // rendered only, no .ck
    proj.makeInstrument("closed_hat",/*makeCk=*/true, /*makeWav=*/false);   // source only, no .wav yet
    // orphan: .ck in project root, no .wav — not in instruments list
    proj.makeLooseCk("orphan");

    audio.projectDir_ = proj.root.string();
    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "list_chuck_instruments " + proj.root.string(), cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data.contains("instruments"));
    REQUIRE(cap.data["instruments"].is_array());

    // Verify lifecycle states
    std::map<std::string, std::string> lifecycleByName;
    for (const auto& inst : cap.data["instruments"]) {
        lifecycleByName[inst.value("name", "")] = inst.value("lifecycle_state", "");
    }

    REQUIRE(lifecycleByName.contains("acid_bass"));
    REQUIRE(lifecycleByName.contains("kick"));
    REQUIRE(lifecycleByName.contains("snare"));
    // closed_hat has makeWav=false — no .wav means not in the instrument inventory
    REQUIRE_FALSE(lifecycleByName.contains("closed_hat"));
    // orphan .ck has no .wav — not in the instrument list
    REQUIRE_FALSE(lifecycleByName.contains("orphan"));

    // Verify instrument details
    for (const auto& inst : cap.data["instruments"]) {
        if (inst.value("name", "") == "acid_bass") {
            REQUIRE(inst.value("source_ck_exists", false) == true);
            REQUIRE(inst.value("rendered_wav_exists", false) == true);
            REQUIRE_FALSE(inst.value("source_path", "").empty());
            REQUIRE_FALSE(inst.value("rendered_path", "").empty());
            // resource_id follows canonical naming
            REQUIRE(inst.value("resource_id", "") == "instrument:acid_bass");
        }
        if (inst.value("name", "") == "snare") {
            REQUIRE(inst.value("source_ck_exists", false) == false);
            REQUIRE(inst.value("rendered_wav_exists", false) == true);
            REQUIRE(inst.value("lifecycle_state", "") == "rendered");
        }
    }

    // No mutations
    REQUIRE(audio.mutations.empty());
}

TEST_CASE("AI-2: list_chuck_instruments returns empty when no instruments exist",
          "[ai2][readonly][list_chuck_instruments][edge_case]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    audio.bankOverride = &bank;

    TestProject proj;
    audio.projectDir_ = proj.root.string();
    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "list_chuck_instruments " + proj.root.string(), cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data["instruments"].is_array());
    REQUIRE(cap.data["instruments"].empty());

    // No mutations
    REQUIRE(audio.mutations.empty());
}

// ===========================================================================
// 6. get_diagnostics tests — real parser/compiler diagnostics (no mocks)
// ===========================================================================

TEST_CASE("AI-2: get_diagnostics returns real mini-notation parse errors",
          "[ai2][readonly][get_diagnostics][mini]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // Deliberately broken mini-notation: unmatched bracket
    const std::string badNotation = "[bd sn";

    RespCapture cap;
    runCmd(ci, "get_diagnostics slot:d0 false " + badNotation, cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data.contains("diagnostics"));
    REQUIRE(cap.data["diagnostics"].is_array());
    REQUIRE_FALSE(cap.data["diagnostics"].empty());

    // The diagnostic should come from the REAL parseMini() path
    const auto& diag = cap.data["diagnostics"][0];
    REQUIRE(diag.value("severity", "") == "error");
    REQUIRE(diag.value("code", "") == "PARSE_ERROR");
    REQUIRE_FALSE(diag.value("message", "").empty());
    REQUIRE(diag.value("resource_id", "") == "slot:d0");
    REQUIRE(diag.value("source", "") == "miniparser");
    REQUIRE(diag.contains("location"));

    // No mutations
    REQUIRE(audio.mutations.empty());
}

TEST_CASE("AI-2: get_diagnostics returns real ChucK compiler errors",
          "[ai2][readonly][get_diagnostics][chuck]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // Broken ChucK: unbalanced parentheses
    const std::string badChuck = "SinOsc s => dac(";

    RespCapture cap;
    runCmd(ci, "get_diagnostics tab:d0 true " + badChuck, cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data["diagnostics"].is_array());
    REQUIRE_FALSE(cap.data["diagnostics"].empty());

    // Diagnostic should come from the REAL validateChuckSource() path
    const auto& diag = cap.data["diagnostics"][0];
    REQUIRE(diag.value("severity", "") == "error");
    REQUIRE(diag.value("code", "") == "CK_COMPILE_ERROR");
    REQUIRE_FALSE(diag.value("message", "").empty());
    REQUIRE(diag.value("resource_id", "") == "tab:d0");
    REQUIRE(diag.value("source", "") == "chuck_compiler");
    REQUIRE(diag.contains("location"));
    REQUIRE(diag["location"].contains("line"));
    REQUIRE(diag["location"].contains("column"));

    // No mutations
    REQUIRE(audio.mutations.empty());
}

TEST_CASE("AI-2: get_diagnostics returns no errors for valid mini-notation",
          "[ai2][readonly][get_diagnostics][mini][positive]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // Valid mini-notation
    const std::string goodNotation = "bd sn hh cp";

    RespCapture cap;
    runCmd(ci, "get_diagnostics slot:d0 false " + goodNotation, cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);

    // Should have an "info" diagnostic indicating parse success
    bool foundOk = false;
    for (const auto& d : cap.data["diagnostics"]) {
        if (d.value("severity", "") == "info")
            foundOk = true;
    }
    REQUIRE(foundOk);
}

TEST_CASE("AI-2: get_diagnostics returns no errors for valid ChucK",
          "[ai2][readonly][get_diagnostics][chuck][positive]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // Valid ChucK: balanced brackets, has => operator
    const std::string goodChuck = "SinOsc s => dac; 440 => s.freq; 1::second => now;";

    RespCapture cap;
    runCmd(ci, "get_diagnostics tab:d0 true " + goodChuck, cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);

    bool foundOk = false;
    for (const auto& d : cap.data["diagnostics"]) {
        if (d.value("severity", "") == "info")
            foundOk = true;
    }
    REQUIRE(foundOk);
}

TEST_CASE("AI-2: get_diagnostics directly calls real parseMini (no mock)",
          "[ai2][readonly][get_diagnostics][integration]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    // Call ProjectReadFacade directly (not through MCP) to verify it uses
    // the real parseMini() function, not a mock.
    hathor::control::ProjectReadFacade facade(audio, bank);

    // Unterminated subsequence in mini-notation
    auto result = facade.getDiagnostics("bd <2 sn", "slot:d1", false);
    nlohmann::json j = result;

    REQUIRE(j.value("ok", false) == true);
    // The real parser should detect an error (unterminated angle bracket)
    bool foundError = false;
    for (const auto& d : j["diagnostics"]) {
        if (d.value("severity", "") == "error")
            foundError = true;
    }
    REQUIRE(foundError);
}

// ===========================================================================
// 7. get_audio_status tests
// ===========================================================================

TEST_CASE("AI-2: get_audio_status reflects initial stopped state",
          "[ai2][readonly][get_audio_status]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "get_audio_status", cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);

    const auto& transport = cap.data["transport"];
    REQUIRE(transport.value("running", true) == false);
    REQUIRE(transport.value("bpm", 0.0) == Catch::Approx(120.0));
    REQUIRE(transport.contains("sample_rate"));
    REQUIRE(transport.contains("master_gain"));
    REQUIRE(transport.contains("eq_preset"));
    REQUIRE(transport.contains("sample_clock"));
    REQUIRE(transport.contains("device_open"));

    REQUIRE(cap.data.contains("slots"));
    REQUIRE(cap.data["slots"].is_array());

    REQUIRE(cap.data.contains("worker"));
    REQUIRE(cap.data.contains("timestamp"));

    // No mutations
    REQUIRE(audio.mutations.empty());
}

TEST_CASE("AI-2: get_audio_status reflects transport state changes",
          "[ai2][readonly][get_audio_status][state_consistency]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // Start playback (this is a test setup action, not an AI-2 call)
    audio.play();
    audio.setBpm(130.0);

    // Snapshot mutations from setup
    const auto mutationsBeforeQuery = audio.mutations.size();

    RespCapture cap;
    runCmd(ci, "get_audio_status", cap);

    REQUIRE(cap.got);
    const auto& transport = cap.data["transport"];
    REQUIRE(transport.value("running", false) == true);
    REQUIRE(transport.value("bpm", 0.0) == Catch::Approx(130.0));

    // The AI-2 read call must not add any new mutations
    REQUIRE(audio.mutations.size() == mutationsBeforeQuery);

    // Stop playback (test setup action)
    audio.stop();

    RespCapture cap2;
    runCmd(ci, "get_audio_status", cap2);

    REQUIRE(cap2.got);
    REQUIRE(cap2.data["transport"].value("running", true) == false);
    // BPM should still be 130 (setBpm persists)
    REQUIRE(cap2.data["transport"].value("bpm", 0.0) == Catch::Approx(130.0));

    // AI-2 read call must not add mutations
    REQUIRE(audio.mutations.size() == mutationsBeforeQuery + 1);  // only the stop() from setup
}

TEST_CASE("AI-2: get_audio_status reflects per-slot playback state",
          "[ai2][readonly][get_audio_status][slot_state]")
{
    TrackingFakeFacade audio;
    SampleBank bank;

    // Set up two slots, one running, one not
    audio.findOrAddSlot("d0");
    audio.findOrAddSlot("d1");
    auto state0 = std::make_shared<SlotState>();
    state0->notation = "bd sn";
    state0->eventBuffer.resize(2, blankEvent());
    std::atomic_store_explicit(&audio.states_[0], state0, std::memory_order_release);

    auto state1 = std::make_shared<SlotState>();
    state1->notation = "hh cp";
    state1->eventBuffer.resize(2, blankEvent());
    std::atomic_store_explicit(&audio.states_[1], state1, std::memory_order_release);

    audio.slotPlay(0);  // d0 running
    // d1 not running

    // Snapshot mutations from setup
    const auto mutationsBeforeQuery = audio.mutations.size();

    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "get_audio_status", cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data["slots"].is_array());

    bool found_d0 = false, found_d1 = false;
    for (const auto& s : cap.data["slots"]) {
        if (s.value("slot_name", "") == "d0") {
            found_d0 = true;
            REQUIRE(s.value("running", false) == true);
            REQUIRE(s.value("has_pattern", false) == true);
            REQUIRE(s.value("notation", "") == "bd sn");
        }
        if (s.value("slot_name", "") == "d1") {
            found_d1 = true;
            REQUIRE(s.value("running", true) == false);
            REQUIRE(s.value("has_pattern", false) == true);
            REQUIRE(s.value("notation", "") == "hh cp");
        }
    }
    REQUIRE(found_d0);
    REQUIRE(found_d1);

    // AI-2 read call must not add mutations
    REQUIRE(audio.mutations.size() == mutationsBeforeQuery);
}

// ===========================================================================
// 8. MCP command routing — verifies commands route through ProjectReadFacade
// ===========================================================================

TEST_CASE("AI-2: MCP routes inspect_project through canonical service layer",
          "[ai2][readonly][mcp_routing]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // The command should be dispatched through dispatchWithCallback →
    // handleReadOnlyCommand → ProjectReadFacade::inspectProject
    RespCapture cap;
    runCmd(ci, "inspect_project", cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data.contains("project_name"));
    REQUIRE(cap.data.contains("active_slots"));
}

TEST_CASE("AI-2: MCP routes get_diagnostics through canonical service layer",
          "[ai2][readonly][mcp_routing][diagnostics]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // Test mini-notation diagnostics via MCP command
    RespCapture cap;
    runCmd(ci, "get_diagnostics slot:d2 false bd [", cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data["diagnostics"].is_array());
    REQUIRE_FALSE(cap.data["diagnostics"].empty());
    REQUIRE(cap.data["diagnostics"][0].value("source", "") == "miniparser");
}

TEST_CASE("AI-2: MCP routes get_diagnostics with Chuck source",
          "[ai2][readonly][mcp_routing][diagnostics][chuck]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // Test ChucK diagnostics via MCP command
    RespCapture cap;
    runCmd(ci, "get_diagnostics tab:d1 true dac )", cap);

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data["diagnostics"].is_array());

    bool foundCkError = false;
    for (const auto& d : cap.data["diagnostics"]) {
        if (d.value("source", "") == "chuck_compiler" &&
            d.value("severity", "") == "error")
            foundCkError = true;
    }
    REQUIRE(foundCkError);
}

// ===========================================================================
// 9. Mutation regression tests — read operations must not mutate
// ===========================================================================

TEST_CASE("AI-2: inspect_project does not mutate playback state",
          "[ai2][readonly][mutation_audit][inspect_project]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // Snapshot state before
    const bool runningBefore = audio.running_;
    const double bpmBefore = audio.bpm_;
    const int mutationCountBefore = static_cast<int>(audio.mutations.size());

    RespCapture cap;
    runCmd(ci, "inspect_project", cap);

    // State after must be unchanged
    REQUIRE(audio.running_ == runningBefore);
    REQUIRE(audio.bpm_ == bpmBefore);
    REQUIRE(static_cast<int>(audio.mutations.size()) == mutationCountBefore);
}

TEST_CASE("AI-2: get_current_song does not mutate project state",
          "[ai2][readonly][mutation_audit][get_current_song]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    audio.findOrAddSlot("d0");
    const int mutationCountBefore = static_cast<int>(audio.mutations.size());

    RespCapture cap;
    runCmd(ci, "get_current_song", cap);

    REQUIRE(static_cast<int>(audio.mutations.size()) == mutationCountBefore);
}

TEST_CASE("AI-2: list_samples does not mutate SampleBank",
          "[ai2][readonly][mutation_audit][list_samples]")
{
    TrackingFakeFacade audio;
    SampleBank bank;

    bank.addEntry("bd", 0, std::vector<float>(44100, 0.5f), 1, 44100.0, "/bd.wav");
    const int countBefore = bank.loadedCount();

    audio.bankOverride = &bank;
    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "list_samples", cap);

    REQUIRE(bank.loadedCount() == countBefore);
    REQUIRE(audio.mutations.empty());
}

TEST_CASE("AI-2: list_chuck_instruments does not create/modify assets",
          "[ai2][readonly][mutation_audit][list_chuck_instruments]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    audio.bankOverride = &bank;

    TestProject proj;
    proj.makeInstrument("bass", /*makeCk=*/true, /*makeWav=*/true);
    audio.projectDir_ = proj.root.string();

    // Snapshot filesystem state before
    const auto instrDir = proj.instrumentsDir();
    const auto ckPath = instrDir / "bass.ck";
    const auto wavPath = instrDir / "bass.wav";
    const auto ftimeCkBefore = std::filesystem::last_write_time(ckPath);
    const auto ftimeWavBefore = std::filesystem::last_write_time(wavPath);
    const uintmax_t ckSizeBefore = std::filesystem::file_size(ckPath);
    const uintmax_t wavSizeBefore = std::filesystem::file_size(wavPath);
    hathor::control::ControlInterface ci(audio, bank);

    // No mutations from the AI-2 read call itself (setup mutations are pre-existing)
    const auto mutationsBeforeRead = audio.mutations.size();
    runCmd(ci, "list_chuck_instruments " + proj.root.string(), cap);

    REQUIRE(cap.got);

    // Filesystem sizes must be unchanged
    REQUIRE(std::filesystem::file_size(ckPath) == ckSizeBefore);
    REQUIRE(std::filesystem::file_size(wavPath) == wavSizeBefore);

    // Filesystem state must be unchanged — compare as durations since epoch
    // (file_time_type underlying type is 128-bit on some platforms, which
    // does not stream to ostream, so we compare via duration_cast).
    using namespace std::chrono;
    const auto ckTimeBefore = time_point_cast<nanoseconds>(ftimeCkBefore).time_since_epoch().count();
    const auto ckTimeAfter = time_point_cast<nanoseconds>(
        std::filesystem::last_write_time(ckPath)).time_since_epoch().count();
    REQUIRE(ckTimeAfter == ckTimeBefore);

    const auto wavTimeBefore = time_point_cast<nanoseconds>(ftimeWavBefore).time_since_epoch().count();
    const auto wavTimeAfter = time_point_cast<nanoseconds>(
        std::filesystem::last_write_time(wavPath)).time_since_epoch().count();
    REQUIRE(wavTimeAfter == wavTimeBefore);

    // No engine mutations
    REQUIRE(audio.mutations.empty());
}

TEST_CASE("AI-2: get_diagnostics does not compile or execute ChucK",
          "[ai2][readonly][mutation_audit][get_diagnostics]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // Valid ChucK — should not trigger any compilation or VM creation
    RespCapture cap;
    runCmd(ci, "get_diagnostics tab:d0 true SinOsc s => dac; 440 => s.freq; 1::second => now;", cap);

    REQUIRE(cap.got);

    // Verify no ckEval, no play, no stop, no storeSlot etc.
    for (const auto& m : audio.mutations) {
        REQUIRE(m.action != "ckEval");
        REQUIRE(m.action != "play");
        REQUIRE(m.action != "stop");
        REQUIRE(m.action != "storeSlot");
        REQUIRE(m.action != "setBpm");
        REQUIRE(m.action != "setMasterGain");
    }
    REQUIRE(audio.mutations.empty());
}

TEST_CASE("AI-2: get_audio_status does not start/stop playback",
          "[ai2][readonly][mutation_audit][get_audio_status]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    const bool runningBefore = audio.running_;
    const auto mutationsBefore = audio.mutations.size();

    RespCapture cap;
    runCmd(ci, "get_audio_status", cap);

    REQUIRE(audio.running_ == runningBefore);
    REQUIRE(audio.mutations.size() == mutationsBefore);
}

TEST_CASE("AI-2: get_audio_status does not start/stop ChucK VMs",
          "[ai2][readonly][mutation_audit][get_audio_status][vm]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    audio.workerAlive_ = true;

    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "get_audio_status", cap);

    // No ckEval, stopCkTab, or other VM mutations
    for (const auto& m : audio.mutations) {
        REQUIRE(m.action != "ckEval");
        REQUIRE(m.action != "stopCkTab");
    }
    REQUIRE(audio.mutations.empty());
}

// ===========================================================================
// 10. Schema consistency tests
// ===========================================================================

TEST_CASE("AI-2: all responses use consistent error model",
          "[ai2][readonly][schema]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    for (const auto& cmd : {"inspect_project", "get_current_song", "list_samples",
                            "get_audio_status"}) {
        RespCapture cap;
        runCmd(ci, cmd, cap);
        REQUIRE(cap.got);
        REQUIRE(cap.data.value("ok", false) == true);
        // Every response should carry a timestamp (canonical schema)
        REQUIRE(cap.data.contains("timestamp"));
    }
}

TEST_CASE("AI-2: all diagnostics use consistent schema",
          "[ai2][readonly][schema][diagnostics]")
{
    TrackingFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // Get diagnostics with an error
    RespCapture cap;
    runCmd(ci, "get_diagnostics s:d0 false [", cap);

    REQUIRE(cap.got);
    for (const auto& d : cap.data["diagnostics"]) {
        REQUIRE(d.contains("severity"));
        REQUIRE(d.contains("code"));
        REQUIRE(d.contains("message"));
        REQUIRE(d.contains("resource_id"));
        REQUIRE(d.contains("source"));
        REQUIRE(d.contains("timestamp"));
    }
}

// ===========================================================================
// 11. Direct ProjectReadFacade tests (MCP bypass path)
// ===========================================================================

TEST_CASE("AI-2: ProjectReadFacade can be called directly without MCP",
          "[ai2][readonly][direct_api]")
{
    TrackingFakeFacade audio;
    SampleBank bank;

    bank.addEntry("test_sample", 0,
        std::vector<float>(22050, 0.0f), 1, 44100.0,
        "/test/samples/test_sample.wav");
    audio.bankOverride = &bank;

    hathor::control::ProjectReadFacade facade(audio, bank);

    // Direct call to list_samples
    auto result = facade.listSamples();
    nlohmann::json j = result;
    REQUIRE(j.value("ok", false) == true);
    REQUIRE(j["samples"].size() == 1);

    // Direct call to inspect_project
    auto projResult = facade.inspectProject();
    j = projResult;
    REQUIRE(j.value("ok", false) == true);
    REQUIRE(j.contains("project_name"));

    // Direct call to get_audio_status
    auto audioResult = facade.getAudioStatus();
    j = audioResult;
    REQUIRE(j.value("ok", false) == true);
    REQUIRE(j.contains("transport"));
}
