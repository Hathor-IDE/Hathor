// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-Later

/**
 * test_ai10_3_change_set.cpp — AI-10.3: First-class Diff / Preview / Undo.
 *
 * Verifies the 10 acceptance requirements:
 *   1. An agentic mutation produces a change-set.
 *   2. The composer can inspect a human-readable summary/diff.
 *   3. A multi-step workflow is represented as one coherent change-set.
 *   4. Reject restores the complete pre-change state.
 *   5. Undo restores an accepted reversible change-set.
 *   6. Accept does not accidentally reapply operations.
 *   7. Irreversible operations are clearly identified.
 *   8. AI-1 confirmation boundaries remain intact (destructive flags; preview
 *      does not grant authorization to execute destructive operations).
 *   9. AI-7 transactional semantics remain the underlying safety mechanism
 *      (the change-set models restore_song via AI-7, never a parallel rollback).
 *  10. Failed workflows do not produce misleading accepted change-sets.
 *
 * Architecture: the ChangeSetManager is a pure in-memory model and is tested
 * directly.  The song-restore path is verified end-to-end against the real
 * SongMutationService (AI-7 atomic write) using a fake AudioEngineFacade +
 * a temporary project directory — the same pattern as test_ai7_song_mutation.
 *
 * Requirement references: AI-10.3, AI-10, AI-7, PROGRAM.md §1421
 */

#include "ChangeSet.hpp"
#include "SongMutationService.hpp"
#include "AudioEngineFacade.hpp"
#include "SampleBank.hpp"
#include "SlotState.hpp"

#include "HathorFileParser.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

using hathor::control::ChangeSetManager;
using hathor::control::ChangeSetOperation;
using hathor::control::ChangeSet;
using hathor::control::ChangeSetStatus;
using hathor::control::SongMutationService;

namespace fs = std::filesystem;

// ===========================================================================
// Helpers — build ChangeSetOperations
// ===========================================================================

namespace {

/// Build a song (edit_song) change-set operation with the given slot and
/// before/after bodies (and optional before/after BPM).
ChangeSetOperation makeSongOp(const std::string& slot,
                              const std::string& beforeBody,
                              const std::string& afterBody,
                              double beforeBpm = 120.0,
                              double afterBpm = 124.0)
{
    ChangeSetOperation op;
    op.op = "edit_song";
    op.resourceId = "song:" + slot + ".hathor";
    op.slotName = slot;
    op.songFile = slot + ".hathor";
    op.originalContent = "[hathor]\nslot = " + slot + "\nbpm = " +
                         std::to_string(static_cast<int>(beforeBpm)) +
                         "\n\n" + beforeBody;
    op.newContent = "[hathor]\nslot = " + slot + "\nbpm = " +
                    std::to_string(static_cast<int>(afterBpm)) +
                    "\n\n" + afterBody;
    op.before = nlohmann::json{
        {"bpm", beforeBpm}, {"label", nullptr}, {"color", nullptr},
        {"slot", slot}, {"bank", nullptr}, {"body", beforeBody}
    };
    op.after = nlohmann::json{
        {"bpm", afterBpm}, {"label", nullptr}, {"color", nullptr},
        {"slot", slot}, {"bank", nullptr}, {"body", afterBody}
    };
    op.reversible = true;
    op.revertAction = "restore song '" + op.songFile + "' to pre-change content";
    return op;
}

/// Build a commit_rendered_asset change-set operation.
ChangeSetOperation makeAssetOp(const std::string& name, bool existedBefore = false)
{
    ChangeSetOperation op;
    op.op = "commit_rendered_asset";
    op.resourceId = "instrument:" + name;
    op.slotName = "d1";
    op.assetName = name;
    op.assetExistedBefore = existedBefore;
    op.before = nlohmann::json{{"existed", existedBefore}};
    op.after = nlohmann::json{{"asset_name", name}, {"ok", true}};
    op.reversible = true;
    op.revertAction = "remove baked asset '" + name + "'";
    return op;
}

} // anonymous namespace

// ===========================================================================
// 1. An agentic mutation produces a change-set
// ===========================================================================

TEST_CASE("AI-10.3: an agentic mutation produces a change-set",
          "[ai10][ai10_3][changeset][produce]")
{
    ChangeSetManager mgr;

    REQUIRE_FALSE(mgr.hasPending());
    REQUIRE(mgr.currentChangeSetId() == 0);

    const int id = mgr.beginChangeSet("dark 8-bar acid bassline");
    REQUIRE(id > 0);
    REQUIRE(mgr.hasPending());
    REQUIRE(mgr.currentChangeSetId() == id);

    // A single mutation becomes one operation in the active change-set.
    mgr.addOperation(makeSongOp("d1", "bd sn", "bd sn hh cp"));
    auto cs = mgr.currentChangeSet();
    REQUIRE(cs.has_value());
    REQUIRE(cs->id == id);
    REQUIRE(cs->operations.size() == 1);
    REQUIRE(cs->operations[0].op == "edit_song");
}

// ===========================================================================
// 2. Human-readable summary / diff
// ===========================================================================

TEST_CASE("AI-10.3: composer can inspect a human-readable summary/diff",
          "[ai10][ai10_3][changeset][preview]")
{
    ChangeSetManager mgr;
    mgr.beginChangeSet("dark 8-bar acid bassline");

    mgr.addOperation(makeSongOp("d1", "bd sn", "bd sn hh cp", 120.0, 124.0));
    mgr.addOperation(makeAssetOp("acid_bass", /*existedBefore=*/false));

    const nlohmann::json preview = mgr.previewCurrent();
    REQUIRE(preview.value("ok", false) == true);
    REQUIRE(preview["change_set_id"] > 0);
    REQUIRE(preview["intent"] == "dark 8-bar acid bassline");
    REQUIRE(preview["status"] == "pending");

    // Per-operation human summaries.
    const auto& ops = preview["operations"];
    REQUIRE(ops.size() == 2);
    REQUIRE(ops[0]["op"] == "edit_song");
    REQUIRE(ops[0]["summary"].get<std::string>().find(
        "Pattern changed on `d1`") != std::string::npos);
    REQUIRE(ops[0]["summary"].get<std::string>().find(
        "BPM changed from 120 → 124") != std::string::npos);
    REQUIRE(ops[1]["op"] == "commit_rendered_asset");
    REQUIRE(ops[1]["summary"] == "New `acid_bass.wav` rendered and committed.");

    // A whole-change-set summary string is present.
    REQUIRE(preview["summary"].is_string());
}

// ===========================================================================
// 2b. Existing-asset modification preview
// ===========================================================================

TEST_CASE("AI-10.3: overwrite of an existing asset is shown as a modification",
          "[ai10][ai10_3][changeset][preview][overwrite]")
{
    ChangeSetManager mgr;
    mgr.beginChangeSet("re-bake the pad");

    mgr.addOperation(makeAssetOp("warm_pad", /*existedBefore=*/true));

    const nlohmann::json preview = mgr.previewCurrent();
    REQUIRE(preview["operations"][0]["summary"] ==
            "Existing `warm_pad` modified (re-rendered and committed).");
}

// ===========================================================================
// 3. Multi-step workflow → one coherent change-set
// ===========================================================================

TEST_CASE("AI-10.3: multi-step workflow is one coherent change-set",
          "[ai10][ai10_3][changeset][coherent]")
{
    ChangeSetManager mgr;
    const int id = mgr.beginChangeSet("build a full groove");

    // Simulate a full workflow: bind an asset AND update the song — both
    // persistent mutations from one run must group into ONE change-set.
    mgr.addOperation(makeAssetOp("acid_bass"));
    mgr.addOperation(makeSongOp("d1", "", "s \"acid_bass\""));
    mgr.addOperation(makeSongOp("d2", "bd", "bd sn"));

    auto cs = mgr.currentChangeSet();
    REQUIRE(cs.has_value());
    REQUIRE(cs->id == id);
    REQUIRE(cs->operations.size() == 3);

    // One change-set carries all of them; toJson exposes every operation.
    const nlohmann::json js = mgr.toJson(*cs);
    REQUIRE(js["change_set_id"] == id);
    REQUIRE(js["operations"].size() == 3);
    REQUIRE(js["reversible"] == true);
}

// ===========================================================================
// 4. Reject restores the complete pre-change state
// ===========================================================================

TEST_CASE("AI-10.3: reject builds a full revert to pre-change state",
          "[ai10][ai10_3][changeset][reject][revert]")
{
    ChangeSetManager mgr;
    mgr.beginChangeSet("edit the groove");

    const ChangeSetOperation songOp = makeSongOp("d1", "bd sn", "bd sn hh cp");
    mgr.addOperation(songOp);
    mgr.addOperation(makeAssetOp("acid_bass"));

    // The revert plan reverts EVERY operation in reverse order.
    auto plan = mgr.rejectCurrent();
    REQUIRE(plan.has_value());
    REQUIRE(plan->size() == 2);

    // Asset created is removed first (reverse order), then song restored.
    REQUIRE((*plan)[0].kind == "remove_asset");
    REQUIRE((*plan)[0].assetName == "acid_bass");
    REQUIRE((*plan)[0].destructive == true);

    REQUIRE((*plan)[1].kind == "restore_song");
    REQUIRE((*plan)[1].songFile == "d1.hathor");
    // The restore carries the ORIGINAL content so the full pre-change state
    // is restored through the AI-7 atomic write path.
    REQUIRE((*plan)[1].content == songOp.originalContent);
    REQUIRE((*plan)[1].destructive == true);
}

// ===========================================================================
// 4b. Reject restores the complete pre-change state (end-to-end via AI-7)
// ===========================================================================

namespace {
struct Ai103TempDir {
    fs::path path;
    Ai103TempDir() {
        path = fs::temp_directory_path() /
               ("hathor_ai103_test_" + std::to_string(counter_++));
        fs::create_directories(path);
    }
    ~Ai103TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    void writeSong(const std::string& name, const std::string& content) {
        std::ofstream(path / name) << content;
    }
    static uint64_t counter_;
};
uint64_t Ai103TempDir::counter_ = 0;
} // anonymous namespace

// A minimal AudioEngineFacade fake (JUCE-free), mirroring the AI-7 test.
class Ai103FakeFacade final : public AudioEngineFacade {
public:
    void play() noexcept override {}
    void stop() noexcept override {}
    void setBpm(double b) noexcept override { bpm_ = b; }
    double getBpm() const noexcept override { return bpm_; }
    bool isRunning() const noexcept override { return true; }
    void slotPlay(int) noexcept override {}
    void slotStop(int) noexcept override {}
    bool isSlotRunning(int) const noexcept override { return false; }
    void setMasterGain(float) noexcept override {}
    float getMasterGain() const noexcept override { return 0.8f; }
    void setMasterEqPreset(hathor::EqPreset) noexcept override {}
    hathor::EqPreset getMasterEqPreset() const noexcept override { return hathor::EqPreset::Flat; }

    int findOrAddSlot(const std::string& name) override {
        for (size_t i = 0; i < names_.size(); ++i)
            if (names_[i] == name) return static_cast<int>(i);
        if (names_.size() < 16) {
            names_.push_back(name);
            states_.emplace_back();
            return static_cast<int>(names_.size()) - 1;
        }
        return -1;
    }
    void storeSlot(int idx, std::shared_ptr<SlotState> s) noexcept override {
        if (idx >= 0 && static_cast<size_t>(idx) < states_.size())
            states_[idx] = std::move(s);
    }
    bool clearSlot(int idx) noexcept override {
        if (idx >= 0 && static_cast<size_t>(idx) < states_.size()) {
            states_[idx].reset();
            return true;
        }
        return false;
    }
    int slotCount() const noexcept override { return static_cast<int>(names_.size()); }
    std::string slotName(int idx) const override {
        return (idx >= 0 && static_cast<size_t>(idx) < names_.size()) ? names_[idx] : "";
    }
    std::shared_ptr<SlotState> loadSlot(int idx) const noexcept override {
        return (idx >= 0 && static_cast<size_t>(idx) < states_.size())
            ? states_[idx] : nullptr;
    }
    bool hasWorker() const noexcept override { return false; }
    bool ckEval(int, const std::string&) noexcept override { return false; }
    bool stopCkTab(int) noexcept override { return false; }
    std::string queryCkTab(int) const override { return {}; }
    uint64_t startAsyncCkCompile(int, const std::string&,
        std::function<void(bool, const std::string&)>) override { return 0; }
    nlohmann::json queryCkJob(uint64_t) const override { return {{"ok", false}}; }
    bool cancelCkJob(uint64_t) override { return false; }
    std::filesystem::path resolveRenderPath(hathor::AssetTarget, std::string_view,
        const std::filesystem::path&) override { return {}; }
    void setLiveJamSessionDir(std::filesystem::path) override {}
    void cleanupLiveJamAssets() noexcept override {}
    bool isStudioAssetPath(const std::filesystem::path&) const override { return false; }
    hathor::RenderHandle startBakeRender(uint8_t, std::string, uint64_t, unsigned,
        const std::filesystem::path&, hathor::ChuckRenderWriter::CompletionCallback) override { return {}; }
    hathor::RenderHandle startBakeRenderRaw(uint8_t, std::string, uint64_t, unsigned,
        const std::filesystem::path&, hathor::ChuckRenderWriter::CompletionCallback) override { return {}; }
    int activeRenderCount() const noexcept override { return 0; }
    void shutdownRender() noexcept override {}
    bool registerBakedAsset(std::string, const std::filesystem::path&) override { return false; }
    std::vector<std::string> listSamples() const override { return {}; }
    std::vector<SlotInfo> listSlots() const noexcept override { return {}; }
    SlotInfo getSlotInfo(int) const noexcept override { return SlotInfo{}; }
    VmStatus getVmStatus(int) const noexcept override { return VmStatus{}; }
    AudioStatus getAudioStatus() const noexcept override { return AudioStatus{}; }
    std::vector<SlotPlayback> listSlotPlayback() const noexcept override { return {}; }
    std::vector<InstrumentInfo> listChuckInstruments(const std::filesystem::path&) const noexcept override { return {}; }
    std::filesystem::path studioInstrumentsDir(const std::filesystem::path&) const noexcept override { return {}; }
    std::filesystem::path currentProjectDir() const noexcept override { return projectDir_; }
    void setProjectDir(std::filesystem::path d) override { projectDir_ = std::move(d); }

private:
    double bpm_ = 120.0;
    std::vector<std::string> names_;
    std::vector<std::shared_ptr<SlotState>> states_;
    std::filesystem::path projectDir_;
};

TEST_CASE("AI-10.3: reject restores the complete pre-change song state (AI-7)",
          "[ai10][ai10_3][changeset][reject][restore][ai7]")
{
    Ai103TempDir dir;
    const std::string original =
        "[hathor]\nslot = d1\nbpm = 120\n\nbd sn";
    dir.writeSong("d1.hathor", original);

    Ai103FakeFacade audio;
    SampleBank bank;
    audio.setProjectDir(dir.path);
    SongMutationService svc(audio, bank);

    // Capture the pre-change content (this is what a change-set records).
    auto read = svc.readSongContent("d1.hathor");
    REQUIRE(read.value("ok", false) == true);
    REQUIRE(read["content"] == original);

    // Mutate the song (the agent's change).
    nlohmann::json ops = nlohmann::json::array({
        {{"op", "replace_pattern"}, {"slot", "d1"},
         {"notation", "bd sn hh cp"}, {"confirm", true}},
        {{"op", "set_meta"}, {"bpm", 124}, {"confirm", true}}
    });
    auto edit = svc.editSong("d1.hathor", ops);
    REQUIRE(edit.value("ok", false) == true);

    // Verify it changed.
    auto after = svc.readSongContent("d1.hathor");
    REQUIRE(after.value("ok", false) == true);
    REQUIRE(after["content"] != original);

    // REJECT = restore the original content through the AI-7 atomic write
    // path (SongMutationService::restoreSongFile) — the same call the
    // change-set revert plan executes.
    auto restore = svc.restoreSongFile("d1.hathor", original);
    REQUIRE(restore.value("ok", false) == true);

    // The complete pre-change state is restored on disk.
    auto final = svc.readSongContent("d1.hathor");
    REQUIRE(final.value("ok", false) == true);
    REQUIRE(final["content"] == original);

    // And the runtime BPM was restored (AI-7 updates runtime state).
    REQUIRE(audio.getBpm() == Catch::Approx(120.0));
}

// ===========================================================================
// 5. Undo restores an accepted reversible change-set
// ===========================================================================

TEST_CASE("AI-10.3: undo restores an accepted change-set",
          "[ai10][ai10_3][changeset][undo]")
{
    ChangeSetManager mgr;
    const int id = mgr.beginChangeSet("edit the groove");
    mgr.addOperation(makeSongOp("d1", "bd sn", "bd sn hh cp"));

    // Accept the change-set.
    REQUIRE(mgr.acceptCurrent() == true);
    auto cs = mgr.getChangeSet(id);
    REQUIRE(cs.has_value());
    REQUIRE(cs->status == ChangeSetStatus::Accepted);

    // Undo the accepted change-set → produces a full revert plan.
    auto plan = mgr.undoAccepted(id);
    REQUIRE(plan.has_value());
    REQUIRE(plan->size() == 1);
    REQUIRE((*plan)[0].kind == "restore_song");
    REQUIRE((*plan)[0].content.find("bd sn") != std::string::npos);

    mgr.markUndone();
    auto undone = mgr.getChangeSet(id);
    REQUIRE(undone.has_value());
    REQUIRE(undone->status == ChangeSetStatus::Undone);
}

// ===========================================================================
// 6. Accept does not accidentally reapply operations
// ===========================================================================

TEST_CASE("AI-10.3: accept does not reapply operations",
          "[ai10][ai10_3][changeset][accept]")
{
    ChangeSetManager mgr;
    mgr.beginChangeSet("edit the groove");
    mgr.addOperation(makeSongOp("d1", "bd sn", "bd sn hh cp"));
    mgr.addOperation(makeAssetOp("acid_bass"));

    auto before = mgr.toJsonActive();
    REQUIRE(before["operations"].size() == 2);

    // Accept merely finalises status — it must NOT mutate or reapply.
    REQUIRE(mgr.acceptCurrent() == true);

    auto cs = mgr.currentChangeSet();
    REQUIRE(cs.has_value());
    REQUIRE(cs->status == ChangeSetStatus::Accepted);
    // Operations are untouched — nothing was re-run or re-added.
    REQUIRE(cs->operations.size() == 2);
    REQUIRE(cs->operations[0].op == "edit_song");
    REQUIRE(cs->operations[1].op == "commit_rendered_asset");

    // No plan is generated by accept (no reapplication / no revert).
    // The manager still exposes the same data.
    auto after = mgr.toJsonActive();
    REQUIRE(after["operations"].size() == 2);
    REQUIRE(after["status"] == "accepted");
}

// ===========================================================================
// 7. Irreversible operations are clearly identified
// ===========================================================================

TEST_CASE("AI-10.3: irreversible operations are clearly identified",
          "[ai10][ai10_3][changeset][irreversible]")
{
    ChangeSetManager mgr;
    mgr.beginChangeSet("irreversible change");

    auto op = makeSongOp("d1", "bd sn", "bd sn hh cp");
    op.reversible = false;
    op.revertAction = "NOT reversible";
    mgr.addOperation(op);

    // The change-set reports itself as NOT reversible.
    auto cs = mgr.currentChangeSet();
    REQUIRE(cs.has_value());
    REQUIRE(cs->operations[0].reversible == false);

    const nlohmann::json js = mgr.toJson(*cs);
    REQUIRE(js["reversible"] == false);
    REQUIRE(js["operations"][0]["reversible"] == false);
    REQUIRE(js["operations"][0]["revert_action"] == "NOT reversible");

    // A whole change-set that is not fully reversible yields NO revert plan
    // (we never pretend a partially-reversible change-set can be coherently
    // reverted).
    auto plan = mgr.buildRevertPlan(*cs);
    REQUIRE_FALSE(plan.has_value());
}

// ===========================================================================
// 8. AI-1 confirmation boundaries remain intact
// ===========================================================================

TEST_CASE("AI-10.3: destructive revert actions are flagged for AI-1 confirmation",
          "[ai10][ai10_3][changeset][ai1][confirmation]")
{
    ChangeSetManager mgr;
    mgr.beginChangeSet("destructive change");

    mgr.addOperation(makeSongOp("d1", "bd sn", "bd sn hh cp"));
    mgr.addOperation(makeAssetOp("acid_bass"));

    // Every revert action that mutates persistent state is flagged destructive
    // so it must pass AI-1 confirmation before execution.
    auto plan = mgr.rejectCurrent();
    REQUIRE(plan.has_value());
    for (const auto& action : *plan) {
        REQUIRE(action.destructive == true);
        REQUIRE((action.kind == "restore_song" || action.kind == "remove_asset"));
    }
}

TEST_CASE("AI-10.3: preview does not grant authorization to execute",
          "[ai10][ai10_3][changeset][ai1][preview_no_authorization]")
{
    ChangeSetManager mgr;
    mgr.beginChangeSet("inspect only");
    mgr.addOperation(makeSongOp("d1", "bd sn", "bd sn hh cp"));

    // Previewing is read-only: it produces a plan for review but performs no
    // mutation and returns nothing executable on its own.
    const nlohmann::json preview = mgr.previewCurrent();
    REQUIRE(preview["operations"].size() == 1);
    REQUIRE(preview["summary"].is_string());

    // The change-set remains pending (not accepted) — the composer must still
    // explicitly Accept, and a destructive Reject requires confirmation.
    auto cs = mgr.currentChangeSet();
    REQUIRE(cs.has_value());
    REQUIRE(cs->status == ChangeSetStatus::Pending);
}

// ===========================================================================
// 9. AI-7 transactional semantics remain the underlying safety mechanism
// ===========================================================================

TEST_CASE("AI-10.3: song revert is modelled via AI-7 restore, not a parallel rollback",
          "[ai10][ai10_3][changeset][ai7]")
{
    ChangeSetManager mgr;
    mgr.beginChangeSet("model AI-7 reuse");
    mgr.addOperation(makeSongOp("d1", "bd sn", "bd sn hh cp"));

    // The revert plan's song action is a restore_song that routes through
    // SongMutationService::restoreSongFile (AI-7 atomic write) — the change-set
    // model itself performs NO filesystem mutation.
    auto plan = mgr.rejectCurrent();
    REQUIRE(plan.has_value());
    REQUIRE((*plan)[0].kind == "restore_song");

    // The change-set manager has no mutation side effects — reject just builds
    // a plan; the model stays pending until the caller executes + marks it.
    auto cs = mgr.currentChangeSet();
    REQUIRE(cs.has_value());
    REQUIRE(cs->status == ChangeSetStatus::Pending);
    REQUIRE(cs->operations.size() == 1);
}

// ===========================================================================
// 10. Failed workflows do not produce misleading accepted change-sets
// ===========================================================================

TEST_CASE("AI-10.3: a cancelled/failed workflow never looks accepted",
          "[ai10][ai10_3][changeset][failed]")
{
    ChangeSetManager mgr;
    mgr.beginChangeSet("a run that fails");
    mgr.addOperation(makeSongOp("d1", "bd sn", "bd sn hh cp"));

    // The workflow fails → the pending change-set is cancelled.
    mgr.cancelCurrent();

    auto cs = mgr.currentChangeSet();
    REQUIRE(cs.has_value());
    REQUIRE(cs->status == ChangeSetStatus::Cancelled);

    // A cancelled change-set cannot be accepted (no misleading accept).
    REQUIRE(mgr.acceptCurrent() == false);

    // And it is not reported as accepted.
    const nlohmann::json js = mgr.toJson(*cs);
    REQUIRE(js["status"] == "cancelled");
    REQUIRE_FALSE(js["status"] == "accepted");
}

TEST_CASE("AI-10.3: a change-set is pending until explicitly accepted",
          "[ai10][ai10_3][changeset][status]")
{
    ChangeSetManager mgr;
    mgr.beginChangeSet("review me");
    mgr.addOperation(makeSongOp("d1", "bd sn", "bd sn hh cp"));

    // Not accepted by default — the composer must review and accept.
    auto cs = mgr.currentChangeSet();
    REQUIRE(cs.has_value());
    REQUIRE(cs->status == ChangeSetStatus::Pending);

    const nlohmann::json js = mgr.toJson(*cs);
    REQUIRE(js["status"] == "pending");
}
