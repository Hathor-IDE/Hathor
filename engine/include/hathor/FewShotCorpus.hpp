// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_FEW_SHOT_CORPUS_HPP
#define HATHOR_FEW_SHOT_CORPUS_HPP

/**
 * FewShotCorpus.hpp — versioned, Hathor-specific few-shot example corpus (AI-G4).
 *
 * This is the curated few-shot / domain-specific completion context for both
 * `.hathor` (Strudel mini-notation) and `.ck` (ChucK) domains. It is owned by
 * AI-G4 and is NOT a language-definition system: it does not redefine what is
 * supported (that is AI-3's LanguageMetadata domain). Instead it carries
 * *examples* that are explicitly curated, validated, and versioned against the
 * same supported-surface metadata that AI-3 publishes.
 *
 * Versioning contract (no independent versioning system):
 *   - The corpus file carries a `versions` block that MUST match the AI-3
 *     constants (kSchemaVersion, kHathorEngineCompat,
 *     kStrudelMiniNotationCompat, kChuckLibVersion,
 *     kChuckIntegrationSurface) at load time. A mismatch marks the corpus
 *     *incompatible* so AI-G3 rejects every example (stale-corpus rejection).
 *   - Each example carries a `surface_version` and a `validates_against`
 *     identifier so reviewers and tests can confirm an example is valid for
 *     the *current* runtime surface, not merely valid in some broader ecosystem.
 *
 * Selection contract (AI-G3):
 *   - AI-G3 reads the loaded corpus and selects only a small, relevant subset
 *     per completion request — never the whole corpus. Selection is based on
 *     language, cursor/edit context, surrounding syntax, and version
 *     compatibility. The corpus itself exposes a flat, indexed, queryable list;
 *     it performs no retrieval/relevance scoring (that is AI-G3's job).
 *
 * Requirement references: AI-G4, AI-3, AI-G3, decision #18
 */

#include <string>
#include <string_view>
#include <vector>

namespace hathor::language {

// ---------------------------------------------------------------------------
// Version block — must match AI-3 constants at load time
// ---------------------------------------------------------------------------

struct FewShotVersionBlock {
    int         schemaVersion;             ///< must == language::kSchemaVersion
    std::string hathorEngineCompat;         ///< must == language::kHathorEngineCompat
    std::string strudelMiniNotationCompat;  ///< must == language::kStrudelMiniNotationCompat
    std::string chuckLibVersion;            ///< must == language::kChuckLibVersion
    std::string chuckIntegrationSurface;    ///< must == language::kChuckIntegrationSurface
    std::string createdAt;                  ///< ISO-8601 creation timestamp
};

// ---------------------------------------------------------------------------
// A single curated few-shot example
// ---------------------------------------------------------------------------

/**
 * A single, curated few-shot example. Each example is tagged with:
 *   - language         — "mininotation" or "chuck"
 *   - surface_version  — the supported-surface version it is valid for
 *   - context          — the CursorContextKind label the example is relevant to
 *   - title            — short human-readable description (for model instructions)
 *   - code             — the example code (must be valid for the stated surface)
 *   - validates_against — human/automation-readable identifier of the surface
 *     the code was verified against (e.g. "strudel:1.2.6,engine:0.1.0")
 */
struct FewShotExample {
    std::string language;        // "mininotation" | "chuck"
    std::string surfaceVersion;  // matches the AI-3 surface identifier for this language
    std::string context;         // cursor context label, e.g. "sample_expr", "routing"
    std::string title;
    std::string code;
    std::string validatesAgainst; // e.g. "strudel:1.2.6,engine:0.1.0"
};

// ---------------------------------------------------------------------------
// The loaded corpus + compatibility result
// ---------------------------------------------------------------------------

struct FewShotCompatibility {
    bool        compatible;      ///< true if all version checks pass
    std::string schemaVersion;   ///< schema version that was checked
    std::string engineVersion;   ///< running engine version
    std::vector<std::string> errors; ///< human-readable error messages
    operator bool() const noexcept { return compatible; }
};

/**
 * The loaded, version-checked few-shot example corpus.
 *
 * Loaded from HathorFewShotExamples.json (same directory as
 * HathorLanguageMetadata.json). Validation mirrors AI-3's
 * loadAndValidate(): the corpus versions block is compared against the live
 * AI-3 constants. When incompatible, `compatible` is false and `examples` is
 * empty so AI-G3 cannot emit stale examples.
 */
struct FewShotCorpus {
    FewShotVersionBlock              versions;
    std::vector<FewShotExample>      examples;
    bool                             compatible = false;
    std::vector<std::string>         errors;
    std::string                      consumer;  // set by assignToConsumer
    std::string                      loadedAt;  // set by assignToConsumer
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Load and version-validate the few-shot example corpus.
 *
 * Validates the `versions` block against the running AI-3 constants
 * (kSchemaVersion, kHathorEngineCompat, kStrudelMiniNotationCompat,
 * kChuckLibVersion, kChuckIntegrationSurface). When any check fails,
 * `result.compatible == false` and `result.examples` is empty.
 *
 * @param jsonPath  Path to HathorFewShotExamples.json.
 * @return Loaded corpus + compatibility result.
 *
 * Requirement references: AI-G4 §3, §4, §6, AI-3 §4, decision #18
 */
struct FewShotLoadResult {
    FewShotCorpus           corpus;
    FewShotCompatibility    compatibility;
};

FewShotLoadResult loadFewShotCorpus(std::string_view jsonPath);

/**
 * Assign this corpus to a consumer (mirrors LanguageMetadata::assignToConsumer).
 */
void assignFewShotToConsumer(FewShotCorpus& corpus, std::string_view consumer);

} // namespace hathor::language

#endif // HATHOR_FEW_SHOT_CORPUS_HPP
