// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_runtime_inspector_model.cpp — L-6 unit tests for the JUCE-free
 * RuntimeInspectorModel.
 *
 * Uses a controllable fake AudioEngineFacade (no JUCE, no real audio) to
 * verify that deterministic runtime state — playback/BPM/cycle, slots,
 * voices, worker liveness, per-tab ChucK VM state, diagnostics counts —
 * flows correctly into RuntimeSnapshot, and that inspection never mutates
 * the engine.
 *
 * JUCE-free. Uses Catch2.
 */

#include <catch2/catch_test_macros.hpp>

#include "RuntimeInspectorModel.hpp"
#include "DiagnosticRegistry.hpp"

#include <chrono>
#include <map>
#include <thread>

using hathor::ui::RuntimeInspectorModel;
using hathor::ui::RuntimeSnapshot;

// ---------------------------------------------------------------------------
// Fake AudioEngineFacade — controllable L-6 introspection state
// ---------------------------------------------------------------------------

class FakeInspectorFacade final : public AudioEngineFacade
{
public:
    // --- Transport ---
    void play() noexcept override { running_ = true; }
    void stop() noexcept override { running_ = false; }
    void setBpm(double b) noexcept override { bpm_ = b; }
    double getBpm() const noexcept override { return bpm_; }
    bool isRunning() const noexcept override { return running_; }

    // --- Per-slot ---
    void slotPlay(int) noexcept override {}
    void slotStop(int) noexcept override {}
    bool isSlotRunning(int) const noexcept override { return false; }

    // --- Gain / EQ ---
    void setMasterGain(float g) noexcept override { gain_ = g; }
    float getMasterGain() const noexcept override { return gain_; }
    void setMasterEqPreset(hathor::EqPreset) noexcept override {}
    hathor::EqPreset getMasterEqPreset() const noexcept override { return hathor::EqPreset::Flat; }

    // --- Slots ---
    int findOrAddSlot(const std::string& name) override
    {
        if (auto it = names_.find(name); it != names_.end()) return it->second;
        const int idx = static_cast<int>(slots_.size());
        names_[name] = idx;
        slots_.emplace_back();
        return idx;
    }
    void storeSlot(int idx, std::shared_ptr<SlotState> state) noexcept override
    {
        if (idx >= 0 && static_cast<std::size_t>(idx) < slots_.size())
            slots_[static_cast<std::size_t>(idx)] = std::move(state);
    }
    bool clearSlot(int idx) noexcept override
    {
        if (idx >= 0 && static_cast<std::size_t>(idx) < slots_.size())
        {
            slots_[static_cast<std::size_t>(idx)].reset();
            return true;
        }
        return false;
    }
    int slotCount() const noexcept override { return static_cast<int>(slots_.size()); }
    std::string slotName(int idx) const override
    {
        for (const auto& [name, i] : names_)
            if (i == idx) return name;
        return {};
    }
    std::shared_ptr<SlotState> loadSlot(int idx) const noexcept override
    {
        if (idx >= 0 && static_cast<std::size_t>(idx) < slots_.size())
            return slots_[static_cast<std::size_t>(idx)];
        return nullptr;
    }

    // --- Worker / ChucK ---
    bool hasWorker() const noexcept override { return workerAlive_; }
    bool ckEval(int, const std::string&) noexcept override { return false; }
    bool stopCkTab(int) noexcept override { return false; }
    std::string queryCkTab(int) const override { return {}; }
    uint64_t startAsyncCkCompile(int, const std::string&,
                                 std::function<void(bool, const std::string&)>) override { return 0; }
    nlohmann::json queryCkJob(uint64_t) const override { return {{"ok", false}}; }
    bool cancelCkJob(uint64_t) override { return false; }

    // --- Bake (unused in tests) ---
    std::filesystem::path resolveRenderPath(hathor::AssetTarget, std::string_view,
                                            const std::filesystem::path&) override { return {}; }
    void setLiveJamSessionDir(std::filesystem::path) override {}
    void setProjectDir(std::filesystem::path) override {}
    std::filesystem::path currentProjectDir() const noexcept override { return projectDir_; }
    void cleanupLiveJamAssets() override {}
    bool isStudioAssetPath(const std::filesystem::path&) const override { return false; }
    hathor::RenderHandle startBakeRender(uint8_t, std::string, uint64_t, unsigned,
                                         const std::filesystem::path&,
                                         hathor::ChuckRenderWriter::CompletionCallback) override { return {}; }
    hathor::RenderHandle startBakeRenderRaw(uint8_t, std::string, uint64_t, unsigned,
                                            const std::filesystem::path&,
                                            hathor::ChuckRenderWriter::CompletionCallback) override { return {}; }
    int activeRenderCount() const noexcept override { return activeRenders_; }
    void shutdownRender() noexcept override {}
    bool registerBakedAsset(std::string, const std::filesystem::path&) override { return false; }
    std::vector<std::string> listSamples() const override { return {}; }

    // --- AI-2 / L-6 read-only introspection ---
    std::vector<SlotInfo> listSlots() const noexcept override
    {
        std::vector<SlotInfo> out;
        for (const auto& [name, i] : names_)
        {
            SlotInfo si;
            si.slotIndex = i;
            si.slotName = name;
            si.active = slots_[static_cast<std::size_t>(i)] != nullptr;
            out.push_back(std::move(si));
        }
        return out;
    }
    SlotInfo getSlotInfo(int idx) const noexcept override
    {
        SlotInfo si;
        si.slotIndex = idx;
        if (idx >= 0 && static_cast<std::size_t>(idx) < slots_.size())
            si.active = slots_[static_cast<std::size_t>(idx)] != nullptr;
        return si;
    }
    VmStatus getVmStatus(int slotIndex) const noexcept override
    {
        if (auto it = vmStatuses_.find(slotIndex); it != vmStatuses_.end())
            return it->second;
        VmStatus st;
        st.hasWorker = workerAlive_;
        st.workerStatus = workerStatus_;
        st.state = workerAlive_ ? "inactive" : "not_started";
        st.generation = 0;
        return st;
    }
    AudioStatus getAudioStatus() const noexcept override
    {
        AudioStatus s;
        s.running = running_;
        s.bpm = bpm_;
        s.sampleRate = sampleRate_;
        s.masterGain = gain_;
        s.eqPreset = "flat";
        s.sampleClock = sampleClock_;
        s.deviceOpen = (sampleRate_ > 0);
        s.activeRenders = activeRenders_;
        if (s.sampleRate > 0 && s.bpm > 0.0)
        {
            const double cyclePos = (static_cast<double>(s.sampleClock) * s.bpm) /
                                    (static_cast<double>(s.sampleRate) * 60.0);
            s.cyclePos = cyclePos - std::floor(cyclePos);
            s.currentBeat = static_cast<int>(std::floor(s.cyclePos * 4.0)) + 1;
        }
        return s;
    }
    int activeVoiceCount() const noexcept override { return static_cast<int>(voices_.size()); }
    void activeVoices(std::vector<VoiceInfo>& out) const override { out = voices_; }
    std::vector<SlotPlayback> listSlotPlayback() const noexcept override
    {
        std::vector<SlotPlayback> out;
        for (const auto& [name, i] : names_)
        {
            SlotPlayback sp;
            sp.slotIndex = i;
            sp.slotName = name;
            sp.running = false;
            sp.hasPattern = slots_[static_cast<std::size_t>(i)] != nullptr;
            out.push_back(std::move(sp));
        }
        return out;
    }
    std::vector<InstrumentInfo> listChuckInstruments(
        const std::filesystem::path&) const noexcept override { return {}; }
    std::filesystem::path studioInstrumentsDir(
        const std::filesystem::path&) const noexcept override { return {}; }

    // --- Test configuration ---
    void setTransport(bool running, double bpm, uint64_t clock, int sampleRate)
    {
        running_ = running;
        bpm_ = bpm;
        sampleClock_ = clock;
        sampleRate_ = sampleRate;
    }
    void addSlot(const std::string& name, bool hasPattern)
    {
        const int idx = findOrAddSlot(name);
        if (hasPattern)
            slots_[static_cast<std::size_t>(idx)] = std::make_shared<SlotState>();
    }
    void setWorker(bool alive, const std::string& status)
    {
        workerAlive_ = alive;
        workerStatus_ = status;
    }
    void setVm(int slot, VmStatus st) { vmStatuses_[slot] = std::move(st); }
    void setVoices(std::vector<VoiceInfo> voices) { voices_ = std::move(voices); }

private:
    bool running_ = true;
    double bpm_ = 120.0;
    uint64_t sampleClock_ = 0;
    int sampleRate_ = 44100;
    float gain_ = 0.8f;
    int activeRenders_ = 1;

    bool workerAlive_ = false;
    std::string workerStatus_ = "not_started";
    std::map<int, VmStatus> vmStatuses_;

    std::map<std::string, int> names_;
    std::vector<std::shared_ptr<SlotState>> slots_;
    std::vector<VoiceInfo> voices_;
    std::filesystem::path projectDir_ = std::filesystem::current_path();
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("RuntimeInspectorModel quick capture reflects playback + cycle", "[runtime-inspect]")
{
    FakeInspectorFacade fake;
    // 1.5 beats at 120 BPM = 33075 samples → cyclePos 1.5 → beat 3/4.
    fake.setTransport(true, 120.0, 33075, 44100);

    hathor::control::DiagnosticRegistry registry;
    hathor::control::Diagnostic errDiag;
    errDiag.severity = hathor::control::DiagSeverity::Error;
    errDiag.source = hathor::control::DiagSource::ChuckCompiler;
    errDiag.message = "syntax error";
    registry.addDiagnostic(errDiag);

    RuntimeInspectorModel model;
    model.setSources(&fake, &registry);
    model.refreshQuick();

    const auto snap = model.snapshot();
    REQUIRE(snap.audio.running);
    REQUIRE(snap.audio.bpm == 120.0);
    REQUIRE(snap.audio.sampleClock == 33075);
    // Half a bar → beat 3/4 (floor(0.5 * 4) + 1).
    REQUIRE(snap.audio.currentBeat == 3);
    REQUIRE(snap.audio.cyclePos > 0.49);
    REQUIRE(snap.audio.cyclePos < 0.51);

    REQUIRE(snap.diagErrors == 1);
    REQUIRE(snap.diagWarnings == 0);
}

TEST_CASE("RuntimeInspectorModel quick capture reflects slots and voices", "[runtime-inspect]")
{
    FakeInspectorFacade fake;
    fake.addSlot("d0", true);
    fake.addSlot("d1", false);
    fake.addSlot("d2", true);

    AudioEngineFacade::VoiceInfo v;
    v.slotId = 0;
    v.gain = 0.7f;
    v.pan = 0.5f;
    v.speed = 1.0;
    fake.setVoices({v});

    RuntimeInspectorModel model;
    model.setSources(&fake, nullptr);
    model.refreshQuick();

    const auto snap = model.snapshot();
    REQUIRE(snap.slots.size() == 3);
    REQUIRE(snap.voices.size() == 1);
    REQUIRE(snap.voices[0].slotId == 0);
    REQUIRE(snap.voices[0].gain == 0.7f);
    REQUIRE(snap.audio.activeRenders == 1);
}

TEST_CASE("RuntimeInspectorModel sync VM capture reads per-tab VM state", "[runtime-inspect]")
{
    FakeInspectorFacade fake;
    fake.addSlot("d0", true);
    fake.addSlot("d1", true);

    fake.setWorker(true, "healthy");
    AudioEngineFacade::VmStatus vm0;
    vm0.hasWorker = true;
    vm0.state = "active";
    vm0.shredInfo = "shred_id=5";
    vm0.generation = 3;
    vm0.workerStatus = "healthy";
    fake.setVm(0, vm0);

    AudioEngineFacade::VmStatus vm1;
    vm1.hasWorker = true;
    vm1.state = "suspended";
    vm1.generation = 0;
    vm1.workerStatus = "healthy";
    fake.setVm(1, vm1);

    RuntimeInspectorModel model;
    model.setSources(&fake, nullptr);
    model.refreshQuick();     // sets workerAlive/hasWorker (no IPC)
    model.captureVmsSync();

    const auto snap = model.snapshot();
    REQUIRE(snap.workerStatus == "healthy");
    REQUIRE(snap.workerGeneration == 3);
    REQUIRE(snap.workerAlive);

    REQUIRE(snap.vmStates.size() == 2);
    // Both active slots were queried (d0 and d1).
    bool sawActive = false, sawSuspended = false;
    for (const auto& vm : snap.vmStates)
    {
        if (vm.state == "active") sawActive = true;
        if (vm.state == "suspended") sawSuspended = true;
    }
    REQUIRE(sawActive);
    REQUIRE(sawSuspended);
}

TEST_CASE("RuntimeInspectorModel reports worker death", "[runtime-inspect]")
{
    FakeInspectorFacade fake;
    fake.addSlot("d0", true);
    fake.setWorker(false, "dead");

    AudioEngineFacade::VmStatus vm;
    vm.hasWorker = false;
    vm.state = "error";
    vm.lastError = "worker crashed";
    vm.generation = 0;
    vm.workerStatus = "dead";
    fake.setVm(0, vm);

    RuntimeInspectorModel model;
    model.setSources(&fake, nullptr);
    model.captureVmsSync();

    const auto snap = model.snapshot();
    REQUIRE_FALSE(snap.workerAlive);
    REQUIRE(snap.workerStatus == "dead");
}

TEST_CASE("RuntimeInspectorModel async VM capture completes without blocking", "[runtime-inspect]")
{
    FakeInspectorFacade fake;
    fake.addSlot("d0", true);
    fake.setWorker(true, "healthy");

    AudioEngineFacade::VmStatus vm;
    vm.hasWorker = true;
    vm.state = "active";
    vm.shredInfo = "shred_id=9";
    vm.generation = 0;
    vm.workerStatus = "healthy";
    fake.setVm(0, vm);

    RuntimeInspectorModel model;
    model.setSources(&fake, nullptr);
    model.refreshQuick();
    model.requestVmCapture();

    // Wait for the background capture to finish (healthy fake → fast).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (model.vmCaptureInFlight() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    REQUIRE_FALSE(model.vmCaptureInFlight());

    const auto snap = model.snapshot();
    REQUIRE(snap.workerStatus == "healthy");
    REQUIRE_FALSE(snap.vmStates.empty());
    REQUIRE(snap.vmStates[0].state == "active");
}

TEST_CASE("RuntimeInspectorModel inspection does not mutate engine state", "[runtime-inspect]")
{
    FakeInspectorFacade fake;
    fake.addSlot("d0", true);
    fake.setTransport(true, 140.0, 0, 44100);
    fake.setWorker(true, "healthy");

    const double bpmBefore = fake.getBpm();
    const bool runningBefore = fake.isRunning();
    const int slotsBefore = fake.slotCount();

    RuntimeInspectorModel model;
    model.setSources(&fake, nullptr);
    for (int i = 0; i < 5; ++i)
    {
        model.refreshQuick();
        model.captureVmsSync();
    }

    REQUIRE(fake.getBpm() == bpmBefore);
    REQUIRE(fake.isRunning() == runningBefore);
    REQUIRE(fake.slotCount() == slotsBefore);
}

TEST_CASE("RuntimeInspectorModel shutdown stops captures", "[runtime-inspect]")
{
    FakeInspectorFacade fake;
    fake.addSlot("d0", true);

    RuntimeInspectorModel model;
    model.setSources(&fake, nullptr);
    model.requestVmCapture();
    model.shutdown();

    // After shutdown, no capture may start.
    model.requestVmCapture();
    model.refreshQuick();   // no-op after shutdown (no crash)
}
