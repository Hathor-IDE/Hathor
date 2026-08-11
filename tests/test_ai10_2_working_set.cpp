// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-Later

/**
 * test_ai10_2_working_set.cpp — AI-10.2: Conversational Memory / Working Set.
 *
 * Verifies:
 *   1. "Make it darker" resolves to the most recent relevant musical object.
 *   2. "Use that instrument" resolves to the relevant working-set instrument.
 *   3. Multi-turn pattern edits maintain the correct target.
 *   4. A reference can resolve after several intermediate safe operations.
 *   5. Ambiguous references do not trigger an unsafe mutation silently.
 *   6. "Revert the last change" identifies the correct recent mutation.
 *   7. Working-set state updates after successful agent operations.
 *   8. Failed operations do not incorrectly enter the working set as successful state.
 *   9. Stale working-set information is reconciled against authoritative project state.
 *  10. Working memory does not become a second project database.
 *  11. Transient conversational state is correctly scoped and cleared.
 *  12. The working set remains distinct from AI-8's per-request context injection.
 *
 * Architecture: tests construct a WorkingSet directly and exercise its
 * public API with controlled inputs — no project state or services required.
 *
 * Requirement references: AI-10.2, PROGRAM.md §1416, AI-10 §1
 */

#include "WorkingSet.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

using hathor::control::WorkingSet;

namespace fs = std::filesystem;

// ===========================================================================
// Helpers
// ===========================================================================

namespace {

/// Create a minimal instrument TrackedItem for testing.
WorkingSet::TrackedItem makeInstrument(const std::string& name,
                                        const std::string& slot = "d1",
                                        const nlohmann::json& extraState = {})
{
    WorkingSet::TrackedItem item;
    item.id   = "instrument:" + name;
    item.name = name;
    item.type = WorkingSet::ItemType::Instrument;
    item.slotName = slot;
    item.state = extraState;
    if (item.state.is_null())
        item.state = nlohmann::json::object();
    item.state["name"] = name;
    item.state["lifecycle_state"] = "bound";
    return item;
}

/// Create a minimal pattern TrackedItem for testing.
WorkingSet::TrackedItem makePattern(const std::string& slot,
                                     const std::string& notation = "[bd sn]*4")
{
    WorkingSet::TrackedItem item;
    item.id   = "pattern:" + slot;
    item.name = slot;
    item.type = WorkingSet::ItemType::Pattern;
    item.slotName = slot;
    item.state = nlohmann::json{{"canonical_notation", notation}, {"slot", slot}};
    return item;
}

/// Create a recorded change for testing.
WorkingSet::RecordedChange makeChange(
    int id,
    const std::string& operation,
    const std::string& resourceId,
    const std::string& revertAction,
    bool reversible = true)
{
    WorkingSet::RecordedChange change;
    change.changeId = id;
    change.operation = operation;
    change.resourceId = resourceId;
    change.slotName = "d1";
    change.before = nlohmann::json{{"existed", false}};
    change.after = nlohmann::json{{"ok", true}, {"resource_id", resourceId}};
    change.reversible = reversible;
    change.revertAction = revertAction;
    change.timestamp = std::chrono::steady_clock::now();
    return change;
}

} // anonymous namespace

// ===========================================================================
// 1. "Make it darker" correctly resolves to the most recent relevant musical object
// ===========================================================================

TEST_CASE("AI-10.2: 'make it darker' resolves to most recent musical object",
          "[ai10][ai10_2][resolution][pronoun]")
{
    WorkingSet ws;
    ws.setLastIntent("dark acid bassline");

    // Create and track an instrument (the bass).
    auto bass = makeInstrument("acid_bass", "d1");
    bass.alias = "the bass";
    ws.recordItem(bass);

    // Also record a sample and a pattern on a different slot to ensure
    // the resolution picks the most recent *relevant* object, not just any.
    auto sample = makeInstrument("bd", "d2");
    sample.type = WorkingSet::ItemType::Instrument;
    sample.alias = "the kick";
    ws.recordItem(sample);

    // Small delay so "the bass" has an earlier lastTouched than "the kick".
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Now record a pattern for the kick (more recent).
    ws.recordItem(makePattern("d2", "[bd]*4"));

    // "make it darker" should resolve "it" to the most recent musical object.
    // Since "darker" is an instrument/audio parameter context, it should
    // resolve to the most recently touched instrument (the kick sample),
    // not the pattern.
    auto result = ws.resolveReference("make it darker", "darker");

    REQUIRE(result.found == true);
    REQUIRE(result.ambiguous == false);
    REQUIRE(result.resolved["type"] == "instrument");
    REQUIRE(result.resolved["name"] == "bd");
}

// ===========================================================================
// 2. "Use that instrument" resolves to the relevant working-set instrument
// ===========================================================================

TEST_CASE("AI-10.2: 'use that instrument' resolves to relevant working-set instrument",
          "[ai10][ai10_2][resolution][named_ref]")
{
    WorkingSet ws;
    ws.setLastIntent("warm pad sound");

    // Track an instrument on slot d3.
    auto pad = makeInstrument("warm_pad", "d3");
    pad.state["rendered_wav_exists"] = true;
    pad.state["bound_to_sample_bank"] = true;
    ws.recordItem(pad);

    // "use that instrument" should resolve to the pad.
    auto result = ws.resolveReference("use that instrument", "use");

    REQUIRE(result.found == true);
    REQUIRE(result.ambiguous == false);
    REQUIRE(result.resolved["type"] == "instrument");
    REQUIRE(result.resolved["name"] == "warm_pad");
    REQUIRE(result.resolved["slot"] == "d3");
}

// ===========================================================================
// 2b. "Use that instrument" when an instrument was created by a workflow
// ===========================================================================

TEST_CASE("AI-10.2: named reference 'that instrument' after workflow creation",
          "[ai10][ai10_2][resolution][named_ref][workflow]")
{
    WorkingSet ws;
    ws.setLastIntent("acid bass");

    // Simulate what updateAfterStep does after bind_asset step.
    nlohmann::json bindResult = {
        {"step", "bind_asset"},
        {"ok", true},
        {"message", "asset committed: acid_bass"},
        {"asset_name", "acid_bass"},
        {"wav_path", "/proj/.hathor_assets/chuck_instruments/acid_bass.wav"},
        {"ck_path", "/proj/.hathor_assets/chuck_instruments/acid_bass.ck"},
        {"bound_to_sample_bank", true}
    };

    ws.updateAfterStep("bind_asset", bindResult, true);

    // Resolve "use that instrument" — should find the acid_bass instrument.
    auto result = ws.resolveReference("use that instrument", "use");

    REQUIRE(result.found == true);
    REQUIRE_FALSE(result.ambiguous);
    REQUIRE(result.resolved["name"] == "acid_bass");
    REQUIRE(result.resolved["type"] == "instrument");
}

// ===========================================================================
// 3. Multi-turn pattern edits maintain the correct target
// ===========================================================================

TEST_CASE("AI-10.2: multi-turn pattern edits maintain correct target",
          "[ai10][ai10_2][multi_turn][pattern]")
{
    WorkingSet ws;
    ws.setLastIntent("simple 4-bar pattern");

    // Turn 1: create a pattern on d1.
    auto pattern = makePattern("d1", "[c1 g1]*4");
    ws.recordItem(pattern);

    // "make it simpler" → should resolve to the pattern on d1.
    auto r1 = ws.resolveReference("make it simpler", "simpler");
    REQUIRE(r1.found == true);
    REQUIRE(r1.resolved["type"] == "pattern");
    REQUIRE(r1.resolved["slot"] == "d1");

    // Simulate updating the pattern's notation.
    ws.updateAfterStep("update_song",
        nlohmann::json{
            {"step", "update_song"},
            {"ok", true},
            {"song", "d1.hathor"},
            {"target_slot", "d1"},
        }, true);

    // Turn 2: "make it even simpler" → should still resolve to d1.
    auto r2 = ws.resolveReference("make it even simpler", "simpler");
    REQUIRE(r2.found == true);
    REQUIRE(r2.resolved["slot"] == "d1");
}

// ===========================================================================
// 4. A reference can resolve after several intermediate safe operations
// ===========================================================================

TEST_CASE("AI-10.2: reference resolves after several intermediate safe operations",
          "[ai10][ai10_2][resolution][safe_ops]")
{
    WorkingSet ws;
    ws.setLastIntent("acid bass");

    // Create the bass instrument.
    ws.recordItem(makeInstrument("acid_bass", "d1"));

    // Run several safe (non-destructive) operations that should NOT
    // invalidate the working set's knowledge of the instrument.
    ws.updateAfterStep("inspect_project",
        nlohmann::json{{"ok", true}, {"project_name", "test"}, {"project_dir", "/proj"}}, true);
    ws.updateAfterStep("inspect_song",
        nlohmann::json{{"ok", true}, {"bpm", 126}}, true);
    ws.updateAfterStep("inspect_assets",
        nlohmann::json{{"ok", true}, {"samples", nlohmann::json::array()}}, true);

    // "make it darker" should still resolve to the bass.
    auto result = ws.resolveReference("make it darker", "darker");

    REQUIRE(result.found == true);
    REQUIRE(result.ambiguous == false);
    REQUIRE(result.resolved["name"] == "acid_bass");
}

// ===========================================================================
// 5. Ambiguous references do not trigger an unsafe mutation silently
// ===========================================================================

TEST_CASE("AI-10.2: ambiguous references surface ambiguity (no silent mutation)",
          "[ai10][ai10_2][resolution][ambiguous]")
{
    WorkingSet ws;

    // Record two instruments without aliases that both could match "it".
    // Both are recent and both are instruments.
    ws.recordItem(makeInstrument("acid_bass", "d1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ws.recordItem(makeInstrument("warm_pad", "d3"));

    // "make it darker" — "it" could refer to either since both are
    // the most recent type (Instrument).  Since the pad is the most recent,
    // it should resolve to the pad, not be ambiguous.
    // To test true ambiguity, we need two items of the same type that
    // both match a named reference.

    auto bass = makeInstrument("acid_bass", "d1");
    bass.alias = "the bass";
    auto pad = makeInstrument("warm_pad", "d3");
    pad.alias = "the bass";  // both aliased as "the bass" — truly ambiguous

    WorkingSet ws2;
    ws2.recordItem(bass);
    ws2.recordItem(pad);

    auto result = ws2.resolveReference("use that bass", "use");

    REQUIRE(result.found == false);
    REQUIRE(result.ambiguous == true);
    REQUIRE(result.candidates.size() == 2);
    REQUIRE(result.errorMessage.find("ambiguous") != std::string::npos);
}

// ===========================================================================
// 6. "Revert the last change" identifies the correct recent mutation
// ===========================================================================

TEST_CASE("AI-10.2: 'revert the last change' identifies correct recent mutation",
          "[ai10][ai10_2][revert]")
{
    WorkingSet ws;

    // Record two changes — the first is non-reversible, the second is.
    ws.recordChange(makeChange(1, "inspect_project", "project:/proj", "no revert", false));

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ws.recordChange(makeChange(2, "edit_song", "song:d1.hathor",
        "restore song to previous pattern", true));

    // The last reversible change should be change #2, not #1.
    auto last = ws.getLastReversibleChange();
    REQUIRE(last.has_value());
    REQUIRE(last->changeId == 2);
    REQUIRE(last->operation == "edit_song");

    // getRevertInfo should include the revert command.
    nlohmann::json info = ws.getRevertInfo();
    REQUIRE(info["has_revertable"] == true);
    REQUIRE(info["last_change"]["change_id"] == 2);
    REQUIRE(info["revert_command"]["cmd"] == "edit_song");
}

// ===========================================================================
// 6b. "Revert the last change" via resolveReference
// ===========================================================================

TEST_CASE("AI-10.2: resolveReference('revert that') returns last reversible change",
          "[ai10][ai10_2][revert][resolve]")
{
    WorkingSet ws;

    // No changes yet.
    auto r0 = ws.resolveReference("revert that");
    REQUIRE(r0.found == false);

    ws.recordChange(makeChange(1, "edit_song", "song:d1.hathor", "restore", true));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ws.recordChange(makeChange(2, "edit_song", "song:d2.hathor", "restore", true));

    // "revert that" should find change #2.
    auto r1 = ws.resolveReference("revert that");
    REQUIRE(r1.found == true);
    REQUIRE_FALSE(r1.ambiguous);
    REQUIRE(r1.resolved["change_id"] == 2);
    REQUIRE(r1.resolved["operation"] == "edit_song");
}

// ===========================================================================
// 7. Working-set state updates after successful agent operations
// ===========================================================================

TEST_CASE("AI-10.2: working-set updates after successful operations",
          "[ai10][ai10_2][updates][success]")
{
    WorkingSet ws;
    ws.setLastIntent("acid bass");

    // Simulate the generate_pattern step (pattern workflow).
    nlohmann::json genResult = {
        {"step", "generate_pattern"},
        {"ok", true},
        {"canonical_notation", "[c1 e1 g1 b1]*8"},
        {"slot", "d1"},
        {"slot_index", 1},
        {"event_count_per_cycle", 4},
        {"source", "slot:d1"}
    };
    ws.updateAfterStep("generate_pattern", genResult, true);

    // Verify the pattern was tracked.
    nlohmann::json wsJson = ws.toJson();
    bool foundPattern = false;
    for (const auto& item : wsJson["items"]) {
        if (item["id"] == "pattern:d1" && item["type"] == "pattern") {
            foundPattern = true;
            REQUIRE(item["state"]["canonical_notation"] == "[c1 e1 g1 b1]*8");
            break;
        }
    }
    REQUIRE(foundPattern);

    // Simulate the bind_asset step (ChucK workflow).
    nlohmann::json bindResult = {
        {"step", "bind_asset"},
        {"ok", true},
        {"asset_name", "acid_bass"},
        {"wav_path", "/proj/.hathor_assets/chuck_instruments/acid_bass.wav"},
        {"bound_to_sample_bank", true}
    };
    ws.updateAfterStep("bind_asset", bindResult, true);

    // Verify the instrument was tracked AND a change was recorded.
    wsJson = ws.toJson();
    bool foundInstrument = false;
    for (const auto& item : wsJson["items"]) {
        if (item["id"] == "instrument:acid_bass") {
            foundInstrument = true;
            break;
        }
    }
    REQUIRE(foundInstrument);

    // Verify a change was recorded.
    REQUIRE(wsJson["changes"].size() >= 1);
    bool foundChange = false;
    for (const auto& change : wsJson["changes"]) {
        if (change["operation"] == "commit_rendered_asset" &&
            change["resource_id"] == "instrument:acid_bass") {
            foundChange = true;
            REQUIRE(change["reversible"] == true);
            break;
        }
    }
    REQUIRE(foundChange);
}

// ===========================================================================
// 8. Failed operations do not incorrectly enter the working set as successful state
// ===========================================================================

TEST_CASE("AI-10.2: failed operations do not update working set",
          "[ai10][ai10_2][updates][failure]")
{
    WorkingSet ws;
    ws.setLastIntent("acid bass");

    // Simulate a successful generate_pattern first.
    nlohmann::json genResult = {
        {"step", "generate_pattern"},
        {"ok", true},
        {"canonical_notation", "[c1 e1 g1 b1]*8"},
        {"slot", "d1"},
        {"slot_index", 1}
    };
    ws.updateAfterStep("generate_pattern", genResult, true);

    // Now simulate a FAILED bind_asset — the working set should NOT
    // record a new instrument or a new change.
    nlohmann::json failedBind = {
        {"step", "bind_asset"},
        {"ok", false},
        {"error", "commit failed"}
    };
    ws.updateAfterStep("bind_asset", failedBind, false);

    nlohmann::json wsJson = ws.toJson();

    // The failed operation should not have added an instrument.
    bool foundFailedInstrument = false;
    for (const auto& item : wsJson["items"]) {
        if (item["id"] == "instrument:acid_bass")
            foundFailedInstrument = true;
    }
    REQUIRE_FALSE(foundFailedInstrument);

    // No change should have been recorded for the failed operation.
    bool foundFailedChange = false;
    for (const auto& change : wsJson["changes"]) {
        if (change["operation"] == "commit_rendered_asset")
            foundFailedChange = true;
    }
    REQUIRE_FALSE(foundFailedChange);

    // The working set should still have the original pattern from the
    // successful generate_pattern step.
    bool foundGenPattern = false;
    for (const auto& item : wsJson["items"]) {
        if (item["id"] == "pattern:d1")
            foundGenPattern = true;
    }
    REQUIRE(foundGenPattern);
}

// ===========================================================================
// 9. Stale working-set information is reconciled against authoritative project state
// ===========================================================================

TEST_CASE("AI-10.2: stale items removed by reconcile against authoritative state",
          "[ai10][ai10_2][reconcile][stale]")
{
    WorkingSet ws;
    ws.setLastIntent("bass");

    // Track an instrument and a pattern.
    ws.recordItem(makeInstrument("acid_bass", "d1"));
    ws.recordItem(makePattern("d1", "[c1 e1]*4"));
    ws.recordItem(makeInstrument("warm_pad", "d3"));

    nlohmann::json wsJson = ws.toJson();
    REQUIRE(wsJson["items"].size() == 3);

    // Reconcile against authoritative project state that does NOT include
    // "warm_pad" — it was removed from the project externally.
    nlohmann::json projectState;
    projectState["instruments"] = nlohmann::json::array({
        {{"name", "acid_bass"}, {"source_ck_exists", true}, {"rendered_wav_exists", true}}
    });
    projectState["samples"] = nlohmann::json::array();
    projectState["active_patterns"] = nlohmann::json::array({
        {{"slot", "d1"}, {"notation", "[c1 e1]*4"}}
    });

    ws.reconcile(projectState);

    wsJson = ws.toJson();

    // "acid_bass" should still be present.
    bool foundAcidBass = false;
    bool foundWarmPad = false;
    bool foundPatternD1 = false;

    for (const auto& item : wsJson["items"]) {
        if (item["id"] == "instrument:acid_bass") foundAcidBass = true;
        if (item["id"] == "instrument:warm_pad") foundWarmPad = true;
        if (item["id"] == "pattern:d1") foundPatternD1 = true;
    }

    REQUIRE(foundAcidBass);
    REQUIRE_FALSE(foundWarmPad);    // stale — removed by reconcile
    REQUIRE(foundPatternD1);        // still valid
}

// ===========================================================================
// 9b. Stale changes are NOT removed by reconcile (historical record)
// ===========================================================================

TEST_CASE("AI-10.2: reconcile preserves historical change records",
          "[ai10][ai10_2][reconcile][history]")
{
    WorkingSet ws;

    ws.recordItem(makeInstrument("old_pad", "d3"));
    ws.recordChange(makeChange(1, "edit_song", "song:d3.hathor", "restore", true));

    nlohmann::json projectState;
    projectState["instruments"] = nlohmann::json::array();
    projectState["samples"] = nlohmann::json::array();
    projectState["active_patterns"] = nlohmann::json::array();

    ws.reconcile(projectState);

    // The stale instrument should be removed, but the change record persists.
    nlohmann::json wsJson = ws.toJson();

    bool foundOldPad = false;
    for (const auto& item : wsJson["items"]) {
        if (item["id"] == "instrument:old_pad") foundOldPad = true;
    }
    REQUIRE_FALSE(foundOldPad);

    // Changes are historical — they should still be present.
    REQUIRE(wsJson["changes"].size() == 1);
    REQUIRE(wsJson["changes"][0]["change_id"] == 1);
}

// ===========================================================================
// 10. Working memory does not become a second project database
// ===========================================================================

TEST_CASE("AI-10.2: working set is session-scoped, not a project database",
          "[ai10][ai10_2][scope][not_database]")
{
    WorkingSet ws;

    // Track a minimal set of items.
    ws.recordItem(makeInstrument("acid_bass", "d1"));
    ws.recordItem(makePattern("d1", "[bd sn]*4"));

    nlohmann::json wsJson = ws.toJson();

    // The working set should NOT expose raw filesystem paths, full project
    // listings, or complete file contents — only tracked items and changes.
    REQUIRE(wsJson.contains("items"));
    REQUIRE(wsJson.contains("changes"));
    REQUIRE(wsJson.contains("last_intent"));
    REQUIRE(wsJson.contains("active_slot"));
    REQUIRE(wsJson.contains("project_dir"));

    // The working set should NOT contain fields like "all_files", "full_project",
    // "raw_fs_listing", etc. — it is NOT a second project database.
    REQUIRE_FALSE(wsJson.contains("all_files"));
    REQUIRE_FALSE(wsJson.contains("full_project"));
    REQUIRE_FALSE(wsJson.contains("raw_fs_listing"));
    REQUIRE_FALSE(wsJson.contains("complete_symbol_index"));

    // Each item should be a tracked entity, not a full project dump.
    for (const auto& item : wsJson["items"]) {
        REQUIRE(item.contains("id"));
        REQUIRE(item.contains("name"));
        REQUIRE(item.contains("type"));
        // Item state is a snapshot, not a full project dump.
        REQUIRE_FALSE(item.contains("all_project_files"));
    }
}

// ===========================================================================
// 11. Transient conversational state is correctly scoped and cleared
// ===========================================================================

TEST_CASE("AI-10.2: clear() removes all session-scoped state",
          "[ai10][ai10_2][scope][clear]")
{
    WorkingSet ws;
    ws.setLastIntent("acid bass");
    ws.setActiveSlot("d1");
    ws.recordItem(makeInstrument("acid_bass", "d1"));
    ws.recordChange(makeChange(1, "edit_song", "song:d1.hathor", "revert", true));

    // Verify state exists before clear.
    nlohmann::json before = ws.toJson();
    REQUIRE_FALSE(before["items"].empty());
    REQUIRE_FALSE(before["changes"].empty());
    REQUIRE(before["last_intent"] == "acid bass");

    // Clear the working set.
    ws.clear();

    nlohmann::json after = ws.toJson();

    // Everything should be cleared.
    REQUIRE(after["items"].empty());
    REQUIRE(after["changes"].empty());
    REQUIRE(after["last_intent"] == "");
    REQUIRE(after["active_slot"] == "");
    REQUIRE(after["project_dir"] == "");

    // After clear, reference resolution should find nothing.
    auto result = ws.resolveReference("make it darker", "darker");
    REQUIRE(result.found == false);
    REQUIRE(result.errorMessage.find("no tracked items") != std::string::npos);
}

// ===========================================================================
// 11b. clear() resets change ID counter
// ===========================================================================

TEST_CASE("AI-10.2: clear resets change ID counter",
          "[ai10][ai10_2][scope][clear][change_ids]")
{
    WorkingSet ws;

    ws.recordChange(makeChange(1, "edit_song", "resource:1", "revert1", true));
    ws.clear();

    // After clear, recordChange without an explicit ID should start at 1 again.
    ws.recordChange(makeChange(0, "edit_song", "resource:3", "revert3", true));

    nlohmann::json info = ws.getRevertInfo();
    REQUIRE(info["last_change"]["change_id"] == 1);
}

// ===========================================================================
// 12. The working set remains distinct from AI-8's per-request context injection
// ===========================================================================

TEST_CASE("AI-10.2: working set is distinct from AI-8 context injection",
          "[ai10][ai10_2][scope][distinct_from_ai8]")
{
    WorkingSet ws;
    ws.setLastIntent("acid bass");

    // The working set tracks conversational state — it should NOT contain
    // the fields that AI-8's AuthoringContext provides (file, cursor, line,
    // language, scope, metadata_version, etc.).
    nlohmann::json wsJson = ws.toJson();

    REQUIRE_FALSE(wsJson.contains("file"));
    REQUIRE_FALSE(wsJson.contains("cursor"));
    REQUIRE_FALSE(wsJson.contains("line"));
    REQUIRE_FALSE(wsJson.contains("language"));
    REQUIRE_FALSE(wsJson.contains("scope"));
    REQUIRE_FALSE(wsJson.contains("metadata_version"));
    REQUIRE_FALSE(wsJson.contains("sections"));

    // The working set provides its own fields specific to session memory.
    REQUIRE(wsJson.contains("items"));
    REQUIRE(wsJson.contains("changes"));
    REQUIRE(wsJson.contains("last_intent"));
    REQUIRE(wsJson.contains("active_slot"));
    REQUIRE(wsJson.contains("reconciled"));

    // Verify resolveReference does not depend on AI-8 providers — it only
    // uses the tracked items and changes within the working set.
    ws.recordItem(makeInstrument("acid_bass", "d1"));

    auto result = ws.resolveReference("make it darker", "darker");
    REQUIRE(result.found == true);
    REQUIRE(result.resolved["type"] == "instrument");
    REQUIRE(result.resolved["name"] == "acid_bass");
}

// ===========================================================================
// Bonus: Alias derivation from intent keywords
// ===========================================================================

TEST_CASE("AI-10.2: alias derived from intent keywords",
          "[ai10][ai10_2][aliases][intent]")
{
    WorkingSet ws;
    ws.setLastIntent("dark acid bassline");

    // After recording an instrument, the alias should be derived from the intent.
    ws.recordItem(makeInstrument("acid_bass", "d1"));

    nlohmann::json wsJson = ws.toJson();
    bool found = false;
    for (const auto& item : wsJson["items"]) {
        if (item["id"] == "instrument:acid_bass") {
            found = true;
            // "bass" keyword in intent should produce alias "the bass"
            REQUIRE(item["alias"] == "the bass");
            break;
        }
    }
    REQUIRE(found);
}

// ===========================================================================
// Bonus: Slot reference resolution
// ===========================================================================

TEST_CASE("AI-10.2: slot reference 'd1' resolves to pattern on that slot",
          "[ai10][ai10_2][resolution][slot]")
{
    WorkingSet ws;

    ws.recordItem(makePattern("d1", "[bd sn]*4"));
    ws.recordItem(makePattern("d2", "[hh]*8"));

    auto result = ws.resolveReference("the pattern on d1", "pattern");

    REQUIRE(result.found == true);
    REQUIRE_FALSE(result.ambiguous);
    REQUIRE(result.resolved["type"] == "pattern");
    REQUIRE(result.resolved["slot"] == "d1");
}

// ===========================================================================
// Bonus: "same as before" resolves to most recent change
// ===========================================================================

TEST_CASE("AI-10.2: 'same as before' resolves to most recent change",
          "[ai10][ai10_2][resolution][same_as_before]")
{
    WorkingSet ws;

    ws.recordChange(makeChange(1, "edit_song", "song:d1.hathor", "revert1", true));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ws.recordChange(makeChange(2, "commit_rendered_asset", "instrument:acid_bass",
        "remove asset", true));

    auto result = ws.resolveReference("same as before");

    REQUIRE(result.found == true);
    REQUIRE(result.resolved["change_id"] == 2);
    REQUIRE(result.resolved["operation"] == "commit_rendered_asset");
}

// ===========================================================================
// Bonus: No items → resolution fails gracefully
// ===========================================================================

TEST_CASE("AI-10.2: resolution on empty working set fails gracefully",
          "[ai10][ai10_2][resolution][empty]")
{
    WorkingSet ws;

    auto result = ws.resolveReference("make it darker", "darker");

    REQUIRE(result.found == false);
    REQUIRE(result.ambiguous == false);
    REQUIRE_FALSE(result.errorMessage.empty());
}

// ===========================================================================
// Bonus: getItemType name helper
// ===========================================================================

TEST_CASE("AI-10.2: itemType names are correctly stringified",
          "[ai10][ai10_2][types]")
{
    REQUIRE(std::string(WorkingSet::itemTypeName(WorkingSet::ItemType::Pattern)) == "pattern");
    REQUIRE(std::string(WorkingSet::itemTypeName(WorkingSet::ItemType::Instrument)) == "instrument");
    REQUIRE(std::string(WorkingSet::itemTypeName(WorkingSet::ItemType::Session)) == "session");
    REQUIRE(std::string(WorkingSet::itemTypeName(WorkingSet::ItemType::RenderJob)) == "render_job");
    REQUIRE(std::string(WorkingSet::itemTypeName(WorkingSet::ItemType::Song)) == "song");
    REQUIRE(std::string(WorkingSet::itemTypeName(WorkingSet::ItemType::Project)) == "project");
}
