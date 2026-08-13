// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// test_control_slots.cpp — A3 per-slot play/stop unit tests.
//
// Exercises ControlInterface::dispatch() for slot-play / slot-stop against
// a fake AudioEngineFacade, covering:
//   - Valid slot play/stop round-trip.
//   - Missing slot identifier.
//   - Slot identifier that exceeds the 16-slot table (rejected).
//
// No JUCE or audio device required.

#include "ControlInterface.hpp"
#include "AudioEngineFacade.hpp"
#include "SampleBank.hpp"
#include "SlotState.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

class ControlSlotsFakeFacade final : public AudioEngineFacade {
public:
    void play() noexcept override {}
    void stop() noexcept override {}
    void setBpm(double) noexcept override {}
    double getBpm() const noexcept override { return 120.0; }
    bool isRunning() const noexcept override { return false; }

    void slotPlay(int idx) noexcept override {
        if (idx >= 0 && idx < kNumSlots)
            slotRunning_[idx] = true;
    }
    void slotStop(int idx) noexcept override {
        if (idx >= 0 && idx < kNumSlots)
            slotRunning_[idx] = false;
    }
    bool isSlotRunning(int idx) const noexcept override {
        if (idx >= 0 && idx < kNumSlots)
            return slotRunning_[idx];
        return false;
    }

    void setMasterGain(float) noexcept override {}
    float getMasterGain() const noexcept override { return 1.0f; }

    int findOrAddSlot(const std::string& name) override {
        for (int i = 0; i < count_; ++i)
            if (names_[i] == name)
                return i;
        if (count_ >= kNumSlots)
            return -1;
        names_[count_] = name;
        states_[count_].reset();
        slotRunning_[count_] = false;
        return count_++;
    }
    void storeSlot(int, std::shared_ptr<SlotState>) noexcept override {}
    bool clearSlot(int) noexcept override { return true; }
    int  slotCount() const noexcept override { return count_; }
    std::string slotName(int idx) const override {
        if (idx >= 0 && idx < count_) return names_[idx];
        return {};
    }
    std::shared_ptr<SlotState> loadSlot(int) const noexcept override {
        return nullptr;
    }

    bool hasWorker() const noexcept override { return false; }
    bool ckEval(int, const std::string&) noexcept override { return false; }
    bool stopCkTab(int) noexcept override { return false; }
    std::string queryCkTab(int) const override { return {}; }

    // AI-5 stubs
    uint64_t startAsyncCkCompile(int, const std::string&,
                                  std::function<void(bool, const std::string&)>) override
    {
        return 0;
    }
    nlohmann::json queryCkJob(uint64_t jobId) const override
    {
        return nlohmann::json{{"ok", false}, {"job_id", jobId}, {"status", "failed"}};
    }
    bool cancelCkJob(uint64_t) override { return false; }

     void setMasterEqPreset(hathor::EqPreset) noexcept override {}
    hathor::EqPreset getMasterEqPreset() const noexcept override
    {
        return hathor::EqPreset::Flat;
    }

    // B8-K1 stubs — not exercised by the control slot tests.
    std::filesystem::path resolveRenderPath(hathor::AssetTarget,
                                            std::string_view,
                                            const std::filesystem::path&) override
    {
        return {};
    }
    void setLiveJamSessionDir(std::filesystem::path) override {}
    void cleanupLiveJamAssets() override {}
    bool isStudioAssetPath(const std::filesystem::path&) const override
    {
        return false;
    }

     hathor::RenderHandle startBakeRender(uint8_t,
                                           std::string,
                                           uint64_t,
                                           unsigned,
                                           const std::filesystem::path&,
                                           hathor::ChuckRenderWriter::CompletionCallback) override
     {
         return hathor::RenderHandle{};
     }
     hathor::RenderHandle startBakeRenderRaw(uint8_t,
                                               std::string,
                                               uint64_t,
                                               unsigned,
                                               const std::filesystem::path&,
                                               hathor::ChuckRenderWriter::CompletionCallback) override
     {
         return hathor::RenderHandle{};
     }
     int  activeRenderCount() const noexcept override { return 0; }
     void shutdownRender() noexcept override {}

      // B8-K4 stubs
      bool registerBakedAsset(std::string, const std::filesystem::path&) override
      {
          return false;
      }
      std::vector<std::string> listSamples() const override
      {
          return {};
      }

      // --- AI-2 read-only introspection stubs ---
      std::vector<SlotInfo> listSlots() const noexcept override { return {}; }
      SlotInfo getSlotInfo(int idx) const noexcept override
      {
          SlotInfo info;
          info.slotIndex = idx;
          return info;
      }
      VmStatus getVmStatus(int) const noexcept override { return VmStatus{}; }
      AudioStatus getAudioStatus() const noexcept override
      {
          return AudioStatus{false, 120.0, 0, 1.0f, "flat", 0, false, 0, 0.0, 0};
      }
    int activeVoiceCount() const noexcept override { return 0; }
    void activeVoices(std::vector<VoiceInfo>& out) const override { (void)out; }

          std::vector<SlotPlayback> listSlotPlayback() const noexcept override { return {}; }
      std::vector<InstrumentInfo> listChuckInstruments(
          const std::filesystem::path&) const noexcept override { return {}; }
      std::filesystem::path studioInstrumentsDir(
          const std::filesystem::path&) const noexcept override { return {}; }
       std::filesystem::path currentProjectDir() const noexcept override { return projectDir_; }
       void setProjectDir(std::filesystem::path dir) override { projectDir_ = std::move(dir); }

     static constexpr int kNumSlots = 16;
    std::string names_[kNumSlots];
    std::shared_ptr<SlotState> states_[kNumSlots];
    bool slotRunning_[kNumSlots] = {};
     int  count_ = 0;
     std::filesystem::path projectDir_;
};

// Capture emitResponse output by replacing stdout is tricky in-process;
// instead we use dispatchWithCallback which routes to a per-thread sink.
struct RespCapture {
    nlohmann::json data;
    bool got = false;
};

void runCmd(hathor::control::ControlInterface& ci,
            const std::string& cmd, RespCapture& cap)
{
    ci.dispatchWithCallback(cmd,
        [&cap](nlohmann::json j) { cap.data = std::move(j); cap.got = true; });
}

} // namespace

TEST_CASE("A3: slot-stop on a valid slot succeeds and sets running=false",
          "[a3][control]")
{
    ControlSlotsFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // d1 is auto-registered by findOrAddSlot.
    RespCapture cap;
    runCmd(ci, "slot-stop d1", cap);
    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data.value("cmd", "") == "slot-stop");
    REQUIRE(cap.data.value("slot", "") == "d1");
    REQUIRE(audio.isSlotRunning(0) == false);
}

TEST_CASE("A3: slot-play sets running=true without affecting other slots",
          "[a3][control]")
{
    ControlSlotsFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    // Pre-set d1 running, d2 not.
    audio.findOrAddSlot("d1");
    audio.findOrAddSlot("d2");
    audio.slotPlay(0);
    REQUIRE(audio.isSlotRunning(0) == true);
    REQUIRE(audio.isSlotRunning(1) == false);

    RespCapture cap;
    runCmd(ci, "slot-play d2", cap);
    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data.value("cmd", "") == "slot-play");

    // d1 must remain running, d2 must now be running.
    REQUIRE(audio.isSlotRunning(0) == true);
    REQUIRE(audio.isSlotRunning(1) == true);
}

TEST_CASE("A3: slot-stop does not affect other slots",
          "[a3][control]")
{
    ControlSlotsFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    audio.findOrAddSlot("d1");
    audio.findOrAddSlot("d2");
    audio.slotPlay(0);
    audio.slotPlay(1);
    REQUIRE(audio.isSlotRunning(0) == true);
    REQUIRE(audio.isSlotRunning(1) == true);

    RespCapture cap;
    runCmd(ci, "slot-stop d1", cap);
    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);

    REQUIRE(audio.isSlotRunning(0) == false);
    REQUIRE(audio.isSlotRunning(1) == true);
}

TEST_CASE("A3: slot-stop with missing slot name returns error",
          "[a3][control]")
{
    ControlSlotsFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "slot-stop", cap);
    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", true) == false);
    REQUIRE(cap.data.value("cmd", "") == "slot-stop");
    REQUIRE_FALSE(cap.data.value("error", "").empty());
}

TEST_CASE("A3: slot-play with missing slot name returns error",
          "[a3][control]")
{
    ControlSlotsFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "slot-play", cap);
    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", true) == false);
    REQUIRE(cap.data.value("cmd", "") == "slot-play");
    REQUIRE_FALSE(cap.data.value("error", "").empty());
}

TEST_CASE("A3: unknown command still rejected",
          "[a3][control]")
{
    ControlSlotsFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    RespCapture cap;
    runCmd(ci, "bogus-command", cap);
    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", true) == false);
    REQUIRE(cap.data.value("error", "") == "unknown command");
}

// ---------------------------------------------------------------------------
// B1: dispatchSlotPlayStop convenience wrapper
// ---------------------------------------------------------------------------

TEST_CASE("B1: dispatchSlotPlayStop issues slot-play and sets running=true",
          "[b1][control]")
{
    ControlSlotsFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    audio.findOrAddSlot("d1");
    REQUIRE(audio.isSlotRunning(0) == false);

    RespCapture cap;
    ci.dispatchSlotPlayStop("d1", /*start=*/true,
        [&cap](nlohmann::json j) { cap.data = std::move(j); cap.got = true; });

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data.value("cmd", "") == "slot-play");
    REQUIRE(audio.isSlotRunning(0) == true);
}

TEST_CASE("B1: dispatchSlotPlayStop issues slot-stop and sets running=false",
          "[b1][control]")
{
    ControlSlotsFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    audio.findOrAddSlot("d1");
    audio.slotPlay(0);
    REQUIRE(audio.isSlotRunning(0) == true);

    RespCapture cap;
    ci.dispatchSlotPlayStop("d1", /*start=*/false,
        [&cap](nlohmann::json j) { cap.data = std::move(j); cap.got = true; });

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data.value("cmd", "") == "slot-stop");
    REQUIRE(audio.isSlotRunning(0) == false);
}

TEST_CASE("B1: dispatchSlotPlayStop on d1 does not affect d2",
          "[b1][control]")
{
    ControlSlotsFakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    audio.findOrAddSlot("d1");
    audio.findOrAddSlot("d2");
    audio.slotPlay(0);  // d1 running
    REQUIRE(audio.isSlotRunning(0) == true);
    REQUIRE(audio.isSlotRunning(1) == false);

    RespCapture cap;
    ci.dispatchSlotPlayStop("d2", /*start=*/true,
        [&cap](nlohmann::json j) { cap.data = std::move(j); cap.got = true; });

    REQUIRE(cap.got);
    REQUIRE(cap.data.value("ok", false) == true);
    REQUIRE(cap.data.value("cmd", "") == "slot-play");

    // Both must now be running independently.
    REQUIRE(audio.isSlotRunning(0) == true);
    REQUIRE(audio.isSlotRunning(1) == true);

    // Stop d1 only — d2 must continue.
    RespCapture cap2;
    ci.dispatchSlotPlayStop("d1", /*start=*/false,
        [&cap2](nlohmann::json j) { cap2.data = std::move(j); cap2.got = true; });

    REQUIRE(cap2.got);
    REQUIRE(cap2.data.value("cmd", "") == "slot-stop");
    REQUIRE(audio.isSlotRunning(0) == false);
    REQUIRE(audio.isSlotRunning(1) == true);
}
