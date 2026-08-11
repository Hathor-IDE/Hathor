// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * WorkingSet.hpp — AI-10.2: Conversational Memory / Working Set.
 *
 * Provides short-term conversational working memory for the current agentic
 * musical session.  This is DISTINCT from AI-8 context injection:
 *
 *   - AI-8 supplies relevant project/language context to an individual
 *     authoring request via dynamic context assembly (AuthoringContext).
 *   - AI-10.2 remembers what the agent has actually been doing during this
 *     session, so that conversational references such as "it", "that bass",
 *     "make it darker", "undo the last change", or "use the same instrument"
 *     resolve against the current working set rather than against nothing.
 *
 * Working-set contents (session-scoped, NOT a second project database):
 *   - current project / song snapshot
 *   - current selected/active slot
 *   - recently inspected assets (samples, instruments, patterns)
 *   - recently created/modified instruments (ChucK sessions, rendered WAVs)
 *   - recently generated patterns
 *   - current musical intent
 *   - recent changes (with canonical revert info)
 *   - validation/diagnostic state
 *   - active render jobs and pending confirmations
 *   - references/aliases such as "the bass", "that pattern", "the last change"
 *
 * Memory lifecycle:
 *   - Created lazily on the first workflow operation.
 *   - Updated after every successful workflow step.
 *   - Survives AgenticWorkflow::reset() to maintain multi-turn continuity.
 *   - Cleared explicitly via clear() — called when a new chat session starts,
 *     the project changes, or the application restarts.
 *   - Transient conversational state is NEVER persisted to the project model.
 *
 * Source of truth:
 *   - Persistent project state remains authoritative in the real
 *     ProjectReadFacade / AudioEngineFacade / SongMutationService models.
 *   - The working set is contextual/session state only.  If the project
 *     changes outside the working set, reconcile() must be called to update.
 *   - Stale working-set information is never trusted over current project state.
 *
 * Undo/revert:
 *   - The working set records which recent operations are reversible and
 *     captures the canonical revert action.
 *   - Actual rollback is ALWAYS performed through the canonical mutation/undo
 *     mechanisms (AI-7 SongMutationService transactional edits, AI-6 asset
 *     removal).  The working set never implements its own parallel rollback.
 *
 * Integration with AI-10 loop:
 *
 *   conversation
 *       ↓
 *   working-set resolution  ← AI-10.2 (this layer)
 *       ↓
 *   current intent
 *       ↓
 *   AI-10.1 actionable plan
 *       ↓
 *   execution
 *       ↓
 *   update working set  ← after every meaningful operation
 *       ↓
 *   next conversational turn
 *
 * Requirement references: AI-10.2, AI-10, AI-7, PROGRAM.md §1416
 */

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace hathor::control {

/**
 * WorkingSet — session-scoped conversational memory for agentic musical workflow.
 *
 * Thread safety: all public methods are safe to call from any thread.
 * The mutex is held only for brief in-memory updates and released before
 * any callback or external call (no lock ordering issues with service mutexes).
 *
 * Constructed by AgenticWorkflow (which owns it).  Persists across workflow
 * runs within the same agent session for multi-turn continuity.
 */
class WorkingSet {
public:
    // -----------------------------------------------------------------------
    // Item type classification
    // -----------------------------------------------------------------------

    enum class ItemType {
        Pattern,       ///< A generated mini-notation pattern (slot-bound)
        Instrument,    ///< A ChucK instrument (source + possibly rendered WAV)
        Session,       ///< A ChucK compilation/audition session (ck:N)
        RenderJob,     ///< An in-flight or completed render job
        Song,          ///< A .hathor song file
        Project,       ///< The overall project snapshot
    };

    /// Stringify an ItemType for JSON output.
    static const char* itemTypeName(ItemType t) noexcept;

    // -----------------------------------------------------------------------
    // Tracked item — an entity the agent has observed or created
    // -----------------------------------------------------------------------

    struct TrackedItem {
        std::string id;                       ///< stable canonical resource_id (e.g. "instrument:acid_bass")
        std::string name;                     ///< human-readable name
        ItemType type;                        ///< classification
        std::string slotName;                 ///< applicable slot (e.g. "d1") for Pattern/Session items
        std::string alias;                    ///< conversational alias (e.g. "the bass") — empty if none
        nlohmann::json state;                 ///< snapshot of item state (notation, source, lifecycle, etc.)
        std::chrono::steady_clock::time_point createdAt;
        std::chrono::steady_clock::time_point lastTouched;
    };

    // -----------------------------------------------------------------------
    // Recorded change — a mutation that occurred during the session
    // -----------------------------------------------------------------------

    struct RecordedChange {
        int changeId;                         ///< monotonic ID, starts at 1
        std::string operation;                ///< canonical op name (e.g. "edit_song", "commit_rendered_asset")
        std::string resourceId;               ///< resource that was changed
        std::string slotName;                 ///< applicable slot (e.g. "d1")
        nlohmann::json before;                ///< pre-change snapshot (if captured)
        nlohmann::json after;                 ///< post-change snapshot
        bool reversible;                      ///< whether this change can be undone
        std::string revertAction;             ///< canonical undo description (human-readable)
        std::chrono::steady_clock::time_point timestamp;
    };

    // -----------------------------------------------------------------------
    // Resolution result — outcome of resolving a conversational reference
    // -----------------------------------------------------------------------

    struct ResolveResult {
        bool found = false;                   ///< true if a unique resolution was found
        bool ambiguous = false;                ///< true if multiple candidates matched
        nlohmann::json resolved;              ///< the resolved item (when found && !ambiguous)
        std::vector<nlohmann::json> candidates; ///< candidate items (when ambiguous)
        std::string errorMessage;             ///< empty on success, description on failure
    };

    // -----------------------------------------------------------------------
    // Construction / destruction
    // -----------------------------------------------------------------------

    WorkingSet() = default;
    ~WorkingSet() = default;

    WorkingSet(const WorkingSet&)            = delete;
    WorkingSet& operator=(const WorkingSet&) = delete;

    // -----------------------------------------------------------------------
    // Item management
    // -----------------------------------------------------------------------

    /**
     * Record or update a tracked item (instrument, pattern, session, etc.).
     *
     * If an item with the same id already exists, it is updated (state replaced,
     * lastTouched refreshed).  Otherwise the item is appended.
     *
     * The alias is auto-derived from the intent keywords + item type if the
     * caller provides intent context (via setLastIntent) — otherwise the caller
     * may set alias explicitly on the TrackedItem.
     */
    void recordItem(const TrackedItem& item);

    /**
     * Remove a tracked item by canonical id.  Called when an item is deleted
     * or replaced by a newer version.
     */
    void removeItem(std::string_view itemId);

    // -----------------------------------------------------------------------
    // Change tracking
    // -----------------------------------------------------------------------

    /**
     * Record a successful mutation that occurred during a workflow step.
     *
     * @param change  The change metadata (operation, resource, before/after,
     *                reversible flag, revert action).
     */
    void recordChange(const RecordedChange& change);

    // -----------------------------------------------------------------------
    // Workflow integration
    // -----------------------------------------------------------------------

    /**
     * Update the working set after a workflow step completes.
     *
     * Called by AgenticWorkflow after every canonical step.  Extracts
     * relevant items from the step result and records tracked items +
     * changes as appropriate.
     *
     * @param stepName   The canonical step name (e.g. "generate_pattern").
     * @param stepResult The StepResult data JSON from the step.
     * @param success    Whether the step succeeded (only successful steps
     *                    update the working set as reliable state).
     */
    void updateAfterStep(const std::string& stepName,
                         const nlohmann::json& stepResult,
                         bool success);

    /**
     * Set the last user intent (natural-language phrase) so that aliases can
     * be derived from intent keywords (e.g. "bass" → "the bass").
     */
    void setLastIntent(std::string intent) noexcept;

    /**
     * Set the current active slot name (e.g. "d1") for pronoun resolution.
     */
    void setActiveSlot(std::string slotName) noexcept;

    // -----------------------------------------------------------------------
    // Reference resolution
    // -----------------------------------------------------------------------

    /**
     * Resolve a conversational reference phrase against the working set.
     *
     * Handles pronouns ("it", "that", "this"), aliases ("the bass"),
     * named references ("that instrument", "the pattern"), slot references
     * ("d1", "that slot"), and special phrases ("the last change",
     * "same as before").
     *
     * If multiple working-set objects could satisfy a reference:
     *   - For read-only queries, the best candidate is returned with a
     *     warning flag.
     *   - For mutable operations, ambiguity is surfaced (ambiguous=true)
     *     and no mutation is applied.
     *
     * @param phrase         The conversational reference text.
     * @param intentContext  Optional intent keyword hint (e.g. "darker", "simpler")
     *                       to help disambiguate the musical domain.
     * @return A ResolveResult with the resolved item or candidates.
     */
    ResolveResult resolveReference(std::string_view phrase,
                                   std::string_view intentContext = {}) const;

    // -----------------------------------------------------------------------
    // Undo / revert support
    // -----------------------------------------------------------------------

    /**
     * Get the most recent reversible change.
     *
     * @return The last RecordedChange where reversible == true, or nullopt
     *         if no reversible changes exist.
     */
    std::optional<RecordedChange> getLastReversibleChange() const;

    /**
     * Get revert information as JSON (for the MCP layer to surface).
     *
     * Includes:
     *   - has_revertable: whether any reversible change exists
     *   - last_change: the most recent reversible change (or null)
     *   - revert_command: the canonical command to execute the revert
     */
    nlohmann::json getRevertInfo() const;

    // -----------------------------------------------------------------------
    // Serialization / inspection
    // -----------------------------------------------------------------------

    /**
     * Get a thread-safe JSON snapshot of the entire working set.
     *
     * Includes: tracked items, recorded changes, aliases, last intent,
     * active slot, and project reconciliation status.
     */
    nlohmann::json toJson() const;

    /**
     * Clear the working set entirely.
     *
     * Called when:
     *   - A new chat session starts
     *   - The project changes outside the working set
     *   - The application restarts (transient state does not persist)
     *
     * After clear(), all items, changes, and context are empty.  The working
     * set is ready to be rebuilt from a fresh workflow.
     */
    void clear() noexcept;

    /**
     * Reconcile the working set against authoritative project state.
     *
     * Called when the project may have changed outside the working set
     * (e.g. the user edited a file directly).  Removes items from the
     * working set whose canonical resource_ids no longer appear in the
     * current project snapshot.
     *
     * @param projectState  JSON from ProjectReadFacade::inspectProject()
     *                       (or inspect_song / inspect_assets combined).
     */
    void reconcile(const nlohmann::json& projectState);

private:
    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    /**
     * Derive a conversational alias from intent keywords and the item.
     *
     * Examples:
     *   intent "dark acid bass" + Instrument → "the bass"
     *   intent "kick drum"      + Instrument → "the kick"
     *   intent "hi-hat"         + Instrument → "the hi-hat"
     *   intent "lead"           + Instrument → "the lead"
     *   intent "chord"          + Pattern    → "the chord pattern"
     */
    static std::string deriveAlias(const std::string& intentKeywords,
                                   const TrackedItem& item) noexcept;

    /**
     * Classify an intent phrase into a musical domain keyword.
     * Returns "bass", "kick", "snare", "hi-hat", "lead", "pad", "chord",
     * or "" if no known keyword is found.
     */
    static std::string classifyMusicalDomain(std::string_view intent) noexcept;

    /**
     * Lowercase a string for keyword matching.
     */
    static std::string toLower(std::string_view s) noexcept;

    /**
     * Check if a pronoun refers to the most recent item.
     */
    static bool isPronoun(std::string_view word) noexcept;

    /**
     * Detect an explicit item-type word ("instrument", "pattern", "session",
     * "render", "song", "project") in a reference phrase.
     */
    static std::optional<ItemType> typeHintFromText(
        std::string_view text) noexcept;

    /**
     * Detect an implied item-type from a conversational modifier, e.g.
     * "darker"/"warmer" → Instrument, "simpler"/"notes" → Pattern.
     */
    static std::optional<ItemType> typeHintFromContext(
        std::string_view context) noexcept;

    /**
     * Extract a slot token ("d1", "d15") mentioned in a reference phrase.
     * Returns "" if none is present.
     */
    static std::string extractSlotReference(std::string_view text) noexcept;

    /**
     * True if the item's alias/name/id classify into the given musical domain.
     */
    static bool itemMatchesDomain(const TrackedItem& item,
                                  const std::string& domain) noexcept;

    /**
     * Resolve a change reference ("revert that", "last change", "same as
     * before").
     *
     * @param kind "revert" → most recent reversible change, "same" → the most
     *             recent change regardless of reversibility.
     */
    ResolveResult resolveChangeReference(std::string_view kind) const;

    /**
     * Resolve a pronoun reference ("it", "that", "this" + optional qualifier)
     * against the most recently touched matching item.
     *
     * When a musical domain is present and multiple items match, the reference
     * is surfaced as ambiguous rather than silently picking one.
     */
    ResolveResult resolvePronoun(const std::string& query,
                                 const std::string& intentContext,
                                 std::optional<ItemType> typeHint,
                                 const std::string& slot,
                                 const std::string& domain) const;

    /**
     * Resolve a named reference (e.g. "the bass", "the pattern on d1",
     * "acid_bass") by matching against item aliases, names, domains, and
     * slots.
     */
    ResolveResult resolveNamedReference(const std::string& query,
                                        const std::string& intentContext,
                                        std::optional<ItemType> typeHint,
                                        const std::string& slot,
                                        const std::string& domain) const;

    // -----------------------------------------------------------------------
    // Internal state (protected by mtx_)
    // -----------------------------------------------------------------------

    mutable std::mutex mtx_;
    std::vector<TrackedItem> items_;
    std::vector<RecordedChange> changes_;
    int nextChangeId_ = 1;

    std::string lastIntent_;
    std::string activeSlot_;
    std::chrono::steady_clock::time_point lastUpdated_ =
        std::chrono::steady_clock::now();
    std::string currentProjectDir_;

    // Flag set when reconcile detects a mismatch with authoritative state.
    bool reconciled_ = true;
};

} // namespace hathor::control
