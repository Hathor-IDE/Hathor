// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_LANGUAGE_METADATA_HPP
#define HATHOR_LANGUAGE_METADATA_HPP

/**
 * LanguageMetadata.hpp — versioned supported-surface metadata model for Hathor.
 *
 * This is Hathor's canonical, owned source of truth for the language/runtime
 * capabilities that Hathor actually implements and promises to users. It is
 * NOT a reimplementation of the Strudel LSP's language intelligence — it is a
 * versioned metadata description of Hathor's *supported surface*.
 *
 * Architecture boundary (AI-3):
 *
 *   Strudel LSP          → language intelligence (completion, hover, diagnostics)
 *       ↓
 *   HathorLanguageMetadata ← Hathor-owned supported-surface metadata (this file)
 *       ↓
 *   ┌─────┼─────┐
 *   ↓     ↓     ↓
 * Editor   AI    validation/integration
 *
 * The metadata describes:
 *   - Which metadata schema version this is
 *   - Which Hathor engine compatibility level it describes
 *   - Which Strudel mini-notation compatibility level it describes
 *   - Which vendored libchuck version it describes
 *   - The supported definitions (functions, samples, patterns, ChucK API)
 *
 * Consumers must NOT silently combine metadata from version A with runtime
 * version B when those versions are incompatible. loadAndValidate() enforces
 * this by checking the hathorEngineCompat against the running engine.
 *
 * Requirement references: AI-3, decision #18
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

namespace hathor::language {

// ---------------------------------------------------------------------------
// Version identifiers
// ---------------------------------------------------------------------------

/**
 * The current metadata schema version. Bumped whenever the JSON schema or
 * field semantics change in a way that requires consumer awareness.
 */
inline constexpr int kSchemaVersion = 1;

/**
 * The Hathor engine compatibility level this metadata describes.
 * Format: "X.Y.Z" (semver). This should match the Hathor binary version that
 * produced this metadata. When the running engine version does not match,
 * loadAndValidate() returns an error.
 */
inline constexpr std::string_view kHathorEngineCompat = "0.1.0";

/**
 * The Strudel mini-notation compatibility level.
 * Hathor's C++ parser is differential-tested against Strudel golden fixtures
 * at this Strudel version. Do NOT claim support for features beyond what has
 * been verified.
 */
inline constexpr std::string_view kStrudelMiniNotationCompat = "1.2.6";

/**
 * The vendored libchuck version this ChucK metadata describes.
 * Empty string when libchuck is not yet vendored (A6 future state).
 * When non-empty, must match the actual libchuck version linked by hathor-audio-worker.
 */
inline constexpr std::string_view kChuckLibVersion = "3.8.3";

/**
 * The supported ChucK integration surface identifier.
 * Describes which ChucK APIs Hathor exposes to .ck files in its current
 * V2 architecture (per-tab VM isolation, timestamped event queue, etc.).
 */
inline constexpr std::string_view kChuckIntegrationSurface = "B4-K3";

// ---------------------------------------------------------------------------
// Definition categories
// ---------------------------------------------------------------------------

/**
 * A supported mini-notation function / pattern operator.
 * These are the Strudel functions that Hathor's engine implements and
 * exposes to .hathor files. Only Hathor-supported functions are listed —
 * upstream Strudel/Tidal functions that Hathor does not implement are
 * deliberately excluded.
 */
struct MiniNotationFunction {
    std::string name;           ///< e.g. "fast", "slow", "stack"
    std::string signature;      ///< e.g. "fast(multiplier: number)"
    std::string description;    ///< short description of what it does
    std::string category;       ///< "pattern" | "effect" | "control" | "operator"
    bool        supported;      ///< true if Hathor's engine implements this

    /** Example usage, suitable for hover/docs. */
    std::optional<std::string> example;
};

/**
 * A supported sample name.
 * Derived from the SampleBank's available samples at load time, but also
 * includes the standard SuperDirt sample names that Hathor's engine resolves.
 */
struct SampleDefinition {
    std::string name;           ///< e.g. "bd", "sn", "hh", "cp"
    std::string description;    ///< human-readable description
    std::string category;       ///< "drum" | "synth" | "noise" | "gm" | "user"
};

/**
 * A supported mini-notation operator (syntactic, not function-call style).
 * e.g. "*", "/", "!", "(", ")", "[", "]", "<", ">", "~", ","
 */
struct MiniNotationOperator {
    std::string name;           ///< e.g. "*"
    std::string description;    ///< what the operator does
    std::string example;        ///< e.g. "bd*4"
};

/**
 * A supported ChucK API definition for the .ck surface.
 * This is NOT a ChucK compiler — it is a description of the APIs Hathor's
 * supported surface exposes. Real ChucK diagnostics come from the actual
 * libchuck compiler (AI-5).
 */
struct ChuckAPIDefinition {
    std::string name;           ///< e.g. "SinOsc", "dac", "now"
    std::string kind;           ///< "ugen" | "constant" | "library" | "class"
    std::string signature;      ///< e.g. "SinOsc osc => dac" (for UGens, the pattern of use)
    std::string description;    ///< what it does
    bool        supported;      ///< true if Hathor's V2 architecture supports this

    /** Example usage in a .ck file. */
    std::optional<std::string> example;
};

/**
 * A supported parameter key for the Hathor ParamMap.
 * These are the parameter keys that Hathor's PatternCompiler populates and
 * that VoicePool consumes. See engine/include/hathor/ParamMap.hpp.
 */
struct ParamDefinition {
    std::string key;            ///< e.g. "s", "n", "gain", "speed", "pan"
    std::string valueType;      ///< "string" | "int" | "double"
    std::string description;    ///< what this parameter controls
    bool        supported;      ///< true if Hathor implements this parameter
};

/**
 * A supported mini-notation grammar element.
 * Documents which syntactic constructs Hathor's parser supports.
 * Only constructs that are verified against Strudel golden fixtures.
 */
struct GrammarElement {
    std::string name;           ///< e.g. "atom", "sequence", "stack", "fast", "slow", "euclid", "rep"
    std::string syntax;         ///< e.g. "space-separated", "comma-separated", "child*N", "child(pulses,steps[,rotation])"
    std::string description;    ///< what the grammar element does
    std::string example;        ///< e.g. "bd sn", "bd, sn", "bd*4", "bd/2", "bd(3,8)", "bd!3"
    bool        supported;      ///< true if Hathor implements this grammar element
};

// ---------------------------------------------------------------------------
// Main metadata structure
// ---------------------------------------------------------------------------

/**
 * The versioned language metadata — Hathor's canonical supported-surface
 * description.
 *
 * This structure is loaded from reference/language-metadata/HathorLanguageMetadata.json
 * and validated against the running engine version. It answers:
 *
 * - Which metadata schema version is this? → schemaVersion
 * - Which Hathor engine version/compatibility level? → hathorEngineCompat
 * - Which Strudel mini-notation compatibility level? → strudelMiniNotationCompat
 * - Which vendored libchuck version? → chuckLibVersion
 * - Which supported definitions are included? → definitions[]
 * - Which consumer is using this? → set by consumer at load time
 *
 * Requirement references: AI-3, decision #18
 */
struct LanguageMetadata {
    // --- Version identification ---

    int         schemaVersion;          ///< kSchemaVersion
    std::string hathorEngineCompat;      ///< e.g. "0.1.0"
    std::string strudelMiniNotationCompat; ///< e.g. "1.2.6"
    std::string chuckLibVersion;         ///< e.g. "3.8.3" or "" if not vendored
    std::string chuckIntegrationSurface; ///< e.g. "B4-K3" or "not-vendored"

    // --- Supported surface definitions ---

    /** Mini-notation functions that Hathor implements (subset of Strudel). */
    std::vector<MiniNotationFunction> functions;

    /** Sample names available in Hathor's SampleBank + standard names. */
    std::vector<SampleDefinition> samples;

    /** Mini-notation operators/syntax elements Hathor supports. */
    std::vector<MiniNotationOperator> operators;

    /** Mini-notation grammar elements Hathor's parser supports. */
    std::vector<GrammarElement> grammar;

    /** Parameter keys Hathor's ParamMap supports. */
    std::vector<ParamDefinition> params;

    /** ChucK API surface Hathor supports in .ck files. */
    std::vector<ChuckAPIDefinition> chuckApi;

    // --- Consumer identification (set by the consumer at load time) ---

    /**
     * Which consumer/runtime loaded this metadata.
     * e.g. "hathor-editor", "hathor-mcp", "ai-authoring", or "validation".
     * Empty when not set (metadata loaded but not yet assigned to a consumer).
     */
    std::optional<std::string> consumer;

    /**
     * When the consumer loaded this metadata (ISO-8601 timestamp).
     * Empty when metadata is loaded but not yet consumed.
     */
    std::optional<std::string> loadedAt;
};

// ---------------------------------------------------------------------------
// Compatibility check result
// ---------------------------------------------------------------------------

/**
 * Result of validating loaded metadata against the running system.
 */
struct MetadataCompatibility {
    bool        compatible  = false;  ///< true if all checks pass
    std::string schemaVersion;    ///< the schema version that was checked
    std::string engineVersion;     ///< the running engine version
    std::vector<std::string> errors; ///< human-readable error messages for each failure

    /** Returns true if all checks passed. */
    operator bool() const noexcept { return compatible; }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Load and validate LanguageMetadata from a JSON file.
 *
 * This function:
 *   1. Parses the JSON file into a LanguageMetadata struct.
 *   2. Validates that the schemaVersion matches kSchemaVersion.
 *   3. Validates that hathorEngineCompat matches kHathorEngineCompat.
 *   4. Validates that strudelMiniNotationCompat matches kStrudelMiniNotationCompat.
 *
 * If any check fails, the metadata is considered incompatible and the error
 * is returned in the compatibility check. Consumers MUST NOT use metadata
 * that fails validation — they must surface a clear incompatibility state.
 *
 * @param jsonPath  Path to the HathorLanguageMetadata.json file.
 * @return The loaded metadata and a compatibility check result.
 *         If the file cannot be read or parsed, metadata is empty and
 *         compatibility.compatible == false with an error message.
 *
 * Requirement references: AI-3 §4, §6, §8, decision #18
 */
struct LoadResult {
    LanguageMetadata metadata;
    MetadataCompatibility compatibility;
};

LoadResult loadAndValidate(std::string_view jsonPath);

/**
 * Assign this metadata to a consumer. Sets the consumer name and timestamp.
 * This is how the system tracks which runtime/editor/AI consumer is using
 * which metadata version.
 *
 * @param metadata  The metadata to assign (modified in place).
 * @param consumer  Consumer identifier (e.g. "hathor-editor").
 *
 * Requirement references: AI-3 §5, §6
 */
void assignToConsumer(LanguageMetadata& metadata, std::string_view consumer);

/**
 * Check if a given Strudel mini-notation function is supported by Hathor.
 * Returns nullptr if not found, or a pointer to the definition if found.
 */
const MiniNotationFunction* findFunction(const LanguageMetadata& metadata, std::string_view name) noexcept;

/**
 * Check if a given sample name is supported by Hathor's surface.
 */
const SampleDefinition* findSample(const LanguageMetadata& metadata, std::string_view name) noexcept;

/**
 * Check if a given mini-notation operator is supported.
 */
const MiniNotationOperator* findOperator(const LanguageMetadata& metadata, std::string_view name) noexcept;

/**
 * Check if a given grammar element is supported.
 */
const GrammarElement* findGrammar(const LanguageMetadata& metadata, std::string_view name) noexcept;

/**
 * Check if a given parameter key is supported.
 */
const ParamDefinition* findParam(const LanguageMetadata& metadata, std::string_view key) noexcept;

/**
 * Check if a given ChucK API is in Hathor's supported surface.
 */
const ChuckAPIDefinition* findChuckApi(const LanguageMetadata& metadata, std::string_view name) noexcept;

} // namespace hathor::language

#endif // HATHOR_LANGUAGE_METADATA_HPP
