// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ChangeSet.hpp — AI-10.3: First-class Diff / Preview / Undo for AI changes.
 *
 * AI-7 provides transactional correctness and rollback safety for a single
 * `edit_song` batch.  AI-10.3 adds the human-facing layer that groups every
 * agentic mutation that changes project/song state into an explicit, coherent
 * AI change-set that the composer can understand, approve, reject, or undo.
 *
 * A change-set:
 *   - identifies what the agent intended to change (the intent);
 *   - enumerates the affected files / project objects / assets;
 *   - records the operations performed, with before/after state where it is
 *     safely representable;
 *   - carries validation / diagnostic results and checkpoint information;
 *   - marks whether the change-set (and each operation) is reversible;
 *   - is reviewable as a complete unit (Accept / Reject / Undo).
 *
 * Safety boundary:
 *   - This module is a PURE in-memory model of a change-set.  It does NOT
 *     perform any filesystem mutation itself.
 *   - Reject/Undo produce a *revert plan* of canonical actions.  Executing
 *     that plan is done by the caller through the canonical AI-7
 *     SongMutationService (restore_song) and AI-6 asset removal — reusing
 *     their transactional rollback, never a parallel implementation.
 *   - Destructive revert actions are flagged `destructive` and remain subject
 *     to AI-1 confirmation.  A preview does not grant authorization.
 *   - Irreversible operations are surfaced explicitly rather than pretended
 *     to be reversible.
 *
 * Requirement references: AI-10.3, AI-10, AI-7, PROGRAM.md §1421
 */

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace hathor::control {

/// Lifecycle status of a whole change-set.
enum class ChangeSetStatus {
    Pending,   ///< Mutations applied, awaiting composer review (Accept/Reject).
    Accepted,  ///< Composer accepted — mutations finalised (no reapply).
    Rejected,  ///< Composer rejected — entire change-set reverted.
    Undone,    ///< An accepted change-set was later undone.
    Cancelled, ///< Workflow failed/cancelled before completion — not accepted.
};

/// Stringify a ChangeSetStatus.
const char* changeSetStatusName(ChangeSetStatus s) noexcept;

/// A single canonical mutation grouped inside a change-set.
struct ChangeSetOperation {
    std::string op;           ///< canonical op: "edit_song" | "commit_rendered_asset"
    std::string resourceId;   ///< e.g. "song:d1.hathor" | "instrument:acid_bass"
    std::string slotName;     ///< applicable slot (e.g. "d1")
    std::string summary;      ///< human-readable line (computed)
    nlohmann::json before;    ///< structured pre-change snapshot
    nlohmann::json after;     ///< structured post-change snapshot
    bool reversible = true;   ///< whether reject/undo can restore pre-change state
    std::string revertAction; ///< canonical revert description

    // edit_song restore payload — the raw file content captured before and
    // after the mutation, so a whole change-set can be restored through the
    // AI-7 atomic write path.
    std::string songFile;         ///< bare .hathor filename (e.g. "d1.hathor")
    std::string originalContent;  ///< full file content before mutation
    std::string newContent;       ///< full file content after mutation

    // commit_rendered_asset payload.
    std::string assetName;
    bool assetExistedBefore = false;
};

/// A complete, reviewable AI change-set.
struct ChangeSet {
    int         id = 0;
    std::string intent;                       ///< what the agent intended to change
    ChangeSetStatus status = ChangeSetStatus::Pending;
    std::vector<ChangeSetOperation> operations;
    nlohmann::json validation;                ///< validation / diagnostic results
    nlohmann::json checkpoint;                ///< checkpoint at which it can be restored
    std::chrono::steady_clock::time_point createdAt;
    std::chrono::steady_clock::time_point updatedAt;
};

/**
 * ChangeSetManager — owns the active change-set and its review lifecycle.
 *
 * Pure in-memory model; performs NO filesystem mutation.  Provides:
 *   - beginChangeSet() to start a new pending change-set for a workflow run;
 *   - addOperation() to append a recorded mutation (with before/after);
 *   - human-readable preview (preview() / previewToJson());
 *   - whole change-set actions: accept(), reject(), undo();
 *   - buildRevertPlan() to produce the canonical revert actions that the
 *     caller executes through AI-7 / AI-6.
 *
 * Thread safety: all public methods are mutex-guarded.
 */
class ChangeSetManager {
public:
    ChangeSetManager() = default;
    ~ChangeSetManager() = default;

    ChangeSetManager(const ChangeSetManager&)            = delete;
    ChangeSetManager& operator=(const ChangeSetManager&) = delete;

    /// A canonical revert action produced by buildRevertPlan().
    struct RevertAction {
        std::string kind;         ///< "restore_song" | "remove_asset"
        std::string resourceId;   ///< e.g. "song:d1.hathor" | "instrument:acid_bass"
        bool        destructive;  ///< true if it crosses the AI-1 confirmation boundary
        // restore_song payload
        std::string songFile;     ///< bare .hathor filename to restore
        std::string content;      ///< original content to write back
        // remove_asset payload
        std::string assetName;    ///< asset to remove
    };

    // -----------------------------------------------------------------------
    // Change-set lifecycle
    // -----------------------------------------------------------------------

    /**
     * Begin a new pending change-set for a workflow run.
     *
     * If a previous pending change-set exists it is not discarded silently;
     * a caller that starts a fresh run while one is pending must have already
     * accepted/rejected it.  The active change-set id is returned.
     */
    int beginChangeSet(std::string intent);

    /// Append a mutation operation to the active pending change-set.
    void addOperation(const ChangeSetOperation& op);

    /// Set validation / diagnostic results on the active change-set.
    void setValidation(nlohmann::json validation);

    /// Set the checkpoint descriptor on the active change-set.
    void setCheckpoint(nlohmann::json checkpoint);

    /**
     * Mark the active change-set as Accepted (finalised).  This performs NO
     * mutation and does NOT reapply operations — the mutations were already
     * applied by the workflow through AI-7.  It only transitions status.
     */
    bool acceptCurrent();

    /**
     * Reject the active pending change-set: revert the ENTIRE change-set to
     * pre-change state.  Returns the revert plan to execute, or nullopt if
     * no active pending change-set exists.
     */
    std::optional<std::vector<RevertAction>> rejectCurrent();

    /**
     * Undo a previously ACCEPTED change-set.  Reverts the entire change-set.
     * Returns the revert plan to execute, or nullopt if there is no accepted
     * change-set (or it is not reversible).
     */
    std::optional<std::vector<RevertAction>> undoAccepted(int changeSetId);

    /// After a revert plan has been executed, finalise status (Rejected/Undone).
    void markRejected();
    void markUndone();

    /// Mark the current change-set cancelled (failed/aborted workflow).
    void cancelCurrent();

    // -----------------------------------------------------------------------
    // Inspection / preview
    // -----------------------------------------------------------------------

    /// The active change-set id (0 if none).
    int currentChangeSetId() const;

    /// Does an active pending change-set exist?
    bool hasPending() const;

    /// Get the active change-set (const snapshot) if any.
    std::optional<ChangeSet> currentChangeSet() const;

    /// Get a change-set by id from the retained history.
    std::optional<ChangeSet> getChangeSet(int id) const;

    /**
     * Build a human-readable, structured preview of the active change-set.
     * JSON form: {change_set_id, intent, status, summary, operations:[...]}.
     */
    nlohmann::json previewCurrent() const;

    /**
     * Serialise a whole change-set (with its operations, before/after, and
     * computed human summary) to JSON.
     */
    nlohmann::json toJson(const ChangeSet& cs) const;

    /// Serialise the active change-set to JSON (null if none).
    nlohmann::json toJsonActive() const;

    // -----------------------------------------------------------------------
    // Revert plan
    // -----------------------------------------------------------------------

    /**
     * Produce the canonical revert plan for the given change-set.
     *
     * Operations are reverted in reverse order.  Song mutations map to a
     * restore_song action carrying the original content (executed via AI-7
     * SongMutationService::restoreSongFile).  Asset creations map to a
     * remove_asset action (destructive, requires AI-1 confirmation).
     *
     * If any operation is marked non-reversible, the returned plan is
     * nullopt and no partial revert is produced (a whole change-set reverts
     * coherently or not at all).
     */
    std::optional<std::vector<RevertAction>> buildRevertPlan(const ChangeSet& cs) const;

private:
    /// Compute a human-readable summary line for a single operation.
    static std::string operationSummary(const ChangeSetOperation& op);

    /// Serialise a single operation to JSON (with human summary + before/after).
    static nlohmann::json operationToJson(const ChangeSetOperation& op);

    /// Build a preview JSON for a change-set (caller must hold mtx_).
    nlohmann::json previewLocked(const ChangeSet& cs) const;

    // -----------------------------------------------------------------------
    // State (protected by mtx_)
    // -----------------------------------------------------------------------

    mutable std::mutex mtx_;
    std::vector<ChangeSet> history_;
    int nextId_ = 1;
};

} // namespace hathor::control
