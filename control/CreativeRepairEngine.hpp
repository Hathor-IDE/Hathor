// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * CreativeRepairEngine.hpp — AI-10.5: Conversational Creative Repair.
 *
 * Extends the technical repair loop (AI-10 / stepRepair) into targeted
 * *creative* feedback: "too busy", "make it darker", "more spacious",
 * "keep the sound but simplify the rhythm", etc.
 *
 * Design principles (AI-10.5):
 *   - Reuse-first: inspects the WorkingSet for existing tracked items
 *     (patterns, instruments, sessions) before proposing ANY mutation.
 *     Existing assets are adapted in-place — never regenerated from scratch.
 *   - Smallest-mutation: each repair produces the minimal targeted change
 *     needed to address the feedback (e.g. wrapping a pattern in `slow(2)`
 *     rather than rewriting its notation; lowering a filter cutoff value
 *     in ChucK source rather than replacing the whole instrument).
 *   - Transactional: all planned ops are structured (capability class tagged)
 *     so the AgenticWorkflow can route them through AI-7 (SongMutationService)
 *     and AI-5 (ChuckSessionService) with full diff/preview/undo via AI-10.3.
 *   - No duplicate names: repairs modify existing assets, not new ones, so
 *     `kick_1`, `kick_2`, `kick_final` chains never arise.
 *
 * Architecture boundary (AI-10.5):
 *
 *   AgenticWorkflow::stepCreativeRepair()  ← this layer
 *         ↓
 *   CreativeRepairEngine::planRepair()
 *         ↓
 *   WorkingSet           (AI-10.2 reference resolution)
 *   SongMutationService  (AI-7 pattern mutations)
 *   ChuckSessionService  (AI-5 ChucK compile + audition)
 *
 * Requirement references: AI-10.5, AI-10.2, AI-10.3, AI-7, AI-5, PROGRAM.md §1421
 */

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "WorkingSet.hpp"

namespace hathor::control {

class SongMutationService;
class ChuckSessionService;

/**
 * CreativeRepairEngine — classifies conversational creative feedback into a
 * targeted, minimal repair plan that reuses existing assets.
 *
 * Constructed with references to the WorkingSet (for target resolution) and
 * the canonical mutation services.  The engine is stateless aside from these
 * references — planRepair() is a pure query that produces a RepairPlan.
 *
 * The plan itself does NOT execute mutations — that is the workflow's job
 * through the canonical AI-7/AI-5 services.  This keeps the engine testable
 * in isolation and ensures all persistent mutations cross the AI-1
 * confirmation boundary.
 */
class CreativeRepairEngine {
public:
    // -----------------------------------------------------------------------
    // Musical property classification
    // -----------------------------------------------------------------------

    /// The musical dimension a piece of feedback targets.
    /// Mapped from natural-language modifiers, NOT from a giant taxonomy —
    /// the application model defines the representation; we only map keywords.
    enum class Property {
        RhythmicDensity,    ///< "too busy", "simpler", "less going on", "sparse"
        TimbralDarkness,    ///< "darker", "warmer", "muffled", "dull"
        TimbralBrightness,  ///< "brighter", "sharper", "edgier", "present"
        Loudness,           ///< "louder", "quieter", "punchier"
        StereoSpread,       ///< "more spacious", "wider", "narrower", "mono"
        Timing,             ///< "tighter", "looser", "off-grid", "swing"
        Pitch,              ///< "higher", "lower", "melancholy", "brighter pitch"
        TimbralCharacter,   ///< "more aggressive", "softer", "analog", "rough"
        Unknown,            ///< no recognisable musical dimension
    };

    /// The domain a repair targets.
    enum class TargetDomain {
        Pattern,     ///< mini-notation pattern (SongMutationService)
        Instrument,  ///< ChucK instrument / session (ChuckSessionService)
        Unknown,
    };

    // -----------------------------------------------------------------------
    // Repair operation — one atomic mutation in the plan
    // -----------------------------------------------------------------------

    /// Mirrors IntentPlanner::CapabilityClass for consistent capability tagging.
    enum class CapabilityClass {
        ReadOnly,
        NonDestructive,
        PersistentMutation,
    };

    inline static const char* capabilityClassName(CapabilityClass c) noexcept
    {
        switch (c) {
            case CapabilityClass::ReadOnly:            return "read_only";
            case CapabilityClass::NonDestructive:      return "non_destructive";
            case CapabilityClass::PersistentMutation:  return "persistent_mutation";
        }
        return "unknown";
    }

    struct RepairOp {
        std::string op;                    ///< canonical op name (e.g. "insert", "compile_chuck")
        std::string service;               ///< canonical service ("SongMutationService" | "ChuckSessionService")
        std::string method;                ///< method to invoke ("editSong" | "compileChuck")
        CapabilityClass capabilityClass = CapabilityClass::ReadOnly;
        bool        requiresConfirmation = false; ///< true if persistent_mutation
        std::string description;           ///< human-readable explanation of what/why
        nlohmann::json params;            ///< op-specific parameters

        nlohmann::json toJson() const;
    };

    // -----------------------------------------------------------------------
    // Repair plan — the complete, inspectable plan
    // -----------------------------------------------------------------------

    struct RepairPlan {
        std::string  feedback;           ///< the original feedback text
        Property     property = Property::Unknown;
        TargetDomain targetDomain = TargetDomain::Unknown;
        std::string  slotName;           ///< resolved target slot (e.g. "d1")
        std::string  resourceId;         ///< canonical resource id (e.g. "slot:d1")
        std::string  explanation;        ///< natural-language explanation of the plan
        std::vector<RepairOp> ops;       ///< ordered operations to execute

        /// For pattern repairs: the transformed notation (after applying the
        /// smallest targeted mutation).  Empty if not a pattern repair.
        std::string  targetNotation;

        /// For ChucK repairs: the transformed source text.  Empty if not a
        /// ChucK repair.
        std::string  targetSource;

        /// For ChucK repairs: the session ID to compile into.  Empty for pattern repairs.
        std::string  sessionId;

        bool needsConfirmation = false;  ///< true if any op requires confirmation

        nlohmann::json toJson() const;
    };

    // -----------------------------------------------------------------------
    // Classification result
    // -----------------------------------------------------------------------

    struct Classification {
        Property      property;
        TargetDomain  domain;
        std::string   explanation;  ///< natural-language description
    };

    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    CreativeRepairEngine(WorkingSet&          workingSet,
                         SongMutationService& songService,
                         ChuckSessionService& chuckService) noexcept
        : workingSet_(workingSet)
        , songService_(songService)
        , chuckService_(chuckService) {}

    ~CreativeRepairEngine() = default;
    CreativeRepairEngine(const CreativeRepairEngine&)            = delete;
    CreativeRepairEngine& operator=(const CreativeRepairEngine&) = delete;

    // -----------------------------------------------------------------------
    // Core API
    // -----------------------------------------------------------------------

    /**
     * Classify a piece of creative feedback into a musical property and domain.
     *
     * This is a pure classification step — it does NOT resolve targets or
     * generate mutations.  Uses keyword matching against mapped musical
     * dimensions (not a giant universal taxonomy).
     *
     * @param feedback  Natural-language feedback (e.g. "make it darker").
     * @return Classification with property, domain, and explanation.
     */
    Classification classifyFeedback(std::string_view feedback) const noexcept;

    /**
     * Resolve a conversational reference (e.g. "it", "the bass", "d1")
     * against the working set to find the target of the repair.
     *
     * Delegates to WorkingSet::resolveReference().
     *
     * @param phrase         The reference text (often part of the feedback).
     * @param intentContext  Optional intent keyword hint for disambiguation.
     * @return JSON with "found", "resolved", "candidates", etc.
     */
    nlohmann::json resolveTarget(std::string_view phrase,
                                 std::string_view intentContext = {}) const;

    /**
     * Produce a complete, executable repair plan from conversational feedback.
     *
     * This is the main entry point.  It:
     *   1. Classifies the feedback into a musical property.
     *   2. Resolves the target (pattern or instrument) via WorkingSet.
     *   3. Reads the current state (notation or ChucK source).
     *   4. Generates the smallest targeted mutation.
     *   5. Packages it as structured RepairOps with capability tags.
     *
     * The plan does NOT execute — the AgenticWorkflow does that through the
     * canonical services.
     *
     * @param feedback       Natural-language feedback (e.g. "too busy").
     * @param intentContext  Optional context for disambiguation (e.g. "bass").
     * @return A RepairPlan ready for execution by the workflow.
     */
    RepairPlan planRepair(std::string_view feedback,
                          std::string_view intentContext = {});

    /**
     * Serialise a RepairPlan to JSON for MCP/UI consumption.
     */
    nlohmann::json toJson(const RepairPlan& plan) const;

private:
    // -----------------------------------------------------------------------
    // Pattern repair generation
    // -----------------------------------------------------------------------

    /**
     * For a "rhythmic density" feedback on a pattern, generate the smallest
     * targeted mutation: wrap the existing notation in a combinator.
     *
     * - "too busy" / "simpler" → slow(2) (halve the density)
     * - "keep the sound but simplify the rhythm" → degradeBy(0.3)
     *
     * @param currentNotation  The existing mini-notation body.
     * @param feedback         Original feedback for keyword nuance.
     * @return Transformed notation string.
     */
    std::string generatePatternDensityRepair(
        std::string_view currentNotation,
        std::string_view feedback) const noexcept;

    // -----------------------------------------------------------------------
    // ChucK repair generation
    // -----------------------------------------------------------------------

    /**
     * For a timbral/procedural feedback on a ChucK instrument, generate the
     * smallest targeted source mutation (parameter adjustment).
     *
     * - "darker" / "warmer" → lower filter cutoff freq
     * - "brighter" / "sharper" → raise filter cutoff freq
     * - "louder" → increase gain
     * - "quieter" → decrease gain
     * - "more spacious" → adjust stereo spread
     *
     * Uses simple text-based parameter adjustment (regex-free, manual scan)
     * to avoid full ChucK parsing.  If the parameter is not found, the source
     * is augmented with an inline adjustment.
     *
     * @param currentSource  The existing .ck source text.
     * @param prop           The classified musical property.
     * @param feedback       Original feedback for nuance.
     * @return Transformed ChucK source text.
     */
    std::string generateChuckRepair(
        std::string_view currentSource,
        Property         prop,
        std::string_view feedback) const noexcept;

    /**
     * Extract just the notation body from a .hathor file's parsed content
     * or from raw text (fallback).  Used to get the current pattern for
     * targeted wrapping.
     */
    static std::string extractNotationFromHathorContent(std::string_view content) noexcept;

    /**
     * Lowercase a string for keyword matching.
     */
    static std::string toLower(std::string_view s) noexcept;

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    WorkingSet&          workingSet_;
    SongMutationService& songService_;
    ChuckSessionService& chuckService_;
};

// ---------------------------------------------------------------------------
// Stringify helpers
// ---------------------------------------------------------------------------

inline const char* propertyToString(CreativeRepairEngine::Property p) noexcept
{
    switch (p) {
        case CreativeRepairEngine::Property::RhythmicDensity:    return "rhythmic_density";
        case CreativeRepairEngine::Property::TimbralDarkness:    return "timbral_darkness";
        case CreativeRepairEngine::Property::TimbralBrightness:  return "timbral_brightness";
        case CreativeRepairEngine::Property::Loudness:           return "loudness";
        case CreativeRepairEngine::Property::StereoSpread:       return "stereo_spread";
        case CreativeRepairEngine::Property::Timing:             return "timing";
        case CreativeRepairEngine::Property::Pitch:               return "pitch";
        case CreativeRepairEngine::Property::TimbralCharacter:   return "timbral_character";
        case CreativeRepairEngine::Property::Unknown:            return "unknown";
    }
    return "unknown";
}

inline const char* domainToString(CreativeRepairEngine::TargetDomain d) noexcept
{
    switch (d) {
        case CreativeRepairEngine::TargetDomain::Pattern:    return "pattern";
        case CreativeRepairEngine::TargetDomain::Instrument: return "instrument";
        case CreativeRepairEngine::TargetDomain::Unknown:    return "unknown";
    }
    return "unknown";
}

} // namespace hathor::control
