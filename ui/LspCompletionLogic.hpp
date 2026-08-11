// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * LspCompletionLogic.hpp — JUCE-free completion logic with metadata fallback.
 *
 * This module implements the completion *decision logic*: given an LSP
 * completion response (from the Strudel LSP server) and an optional
 * LanguageMetadata snapshot (AI-3), it produces a merged CompletionResult
 * that:
 *
 *   1. Prefers LSP-provided completions.
 *   2. Supplements with LanguageMetadata-supported items for Hathor-specific
 *      supported-surface gaps (items the LSP might not know about).
 *   3. Verifies AI-3 version compatibility before using metadata fallback
 *      (per AI-3 decision #18 — consumers must NOT silently combine metadata
 *      from an incompatible version).
 *
 * The completion logic is organized into three levels:
 *   L1 — Basic: prefix matching against LSP items + metadata function/sample names.
 *   L2 — Signature-aware: if the LSP returns signature information or the
 *        cursor is inside a function call, enriches with parameter hints.
 *   L3 — Context-aware: filters by project context (slot, current sample bank,
 *        front-matter label) to narrow completions.
 *
 * All code is JUCE-free and fully unit-testable in the hathor-ui-tests target.
 *
 * Requirement references: AI-4, AI-3 decision #18
 */

#include "LspProtocol.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hathor {
namespace language {
// Forward-declare LanguageMetadata types (defined in ai-3)
struct LanguageMetadata;
struct MetadataCompatibility;
struct MiniNotationFunction;
struct SampleDefinition;
}

namespace lsp {

// ---------------------------------------------------------------------------
// CompletionContext analysis
// ---------------------------------------------------------------------------

/**
 * Analyze the document text and cursor position to determine the completion
 * context (what kind of completion is being requested) and extract the
 * prefix that should be matched against completion items.
 *
 * This analysis is JUCE-free and operates on raw text + line/character.
 */
struct ContextAnalysis {
    CompletionContextKind kind;
    std::string           prefix;       ///< text being completed (lowercase for matching)
    std::string           fullPrefix;   ///< exact text before cursor (preserves case)
    bool                  insideParens  = false;
    bool                  insideString  = false;
    std::string           functionName; ///< if insideParens, the enclosing function
};

/**
 * Analyze the completion context at a given position in the document.
 *
 * @param documentText  The full text of the document.
 * @param line           0-based line number.
 * @param character      0-based character offset on the line.
 * @return ContextAnalysis describing what kind of completion is requested.
 */
ContextAnalysis analyzeContext(std::string_view documentText, int line, int character);

// ---------------------------------------------------------------------------
// Prefix matching and filtering
// ---------------------------------------------------------------------------

/**
 * Check if a candidate label matches the prefix (case-insensitive prefix match).
 */
bool matchesPrefix(std::string_view label, std::string_view prefix) noexcept;

/**
 * Filter a list of completion items by prefix match (case-insensitive).
 */
std::vector<CompletionItem> filterByPrefix(const std::vector<CompletionItem>& items,
                                           std::string_view prefix);

/**
 * Sort completion items: functions first, then samples, then others.
 * Within each group, sort alphabetically.
 */
void sortCompletionItems(std::vector<CompletionItem>& items) noexcept;

// ---------------------------------------------------------------------------
// Metadata fallback (L1) — uses LanguageMetadata from AI-3
// ---------------------------------------------------------------------------

/**
 * Build completion candidates from LanguageMetadata for mininotation context.
 * Only items marked as `supported` in the metadata are included.
 *
 * @param metadata    The validated LanguageMetadata (must pass compatibility check).
 * @param compatibility The result of loadAndValidate() — must be compatible.
 * @param context     The analyzed completion context.
 * @return CompletionCandidates derived from metadata.
 */
std::vector<CompletionCandidate> metadataFallback(
    const language::LanguageMetadata& metadata,
    const language::MetadataCompatibility& compatibility,
    const ContextAnalysis& context);

/**
 * Build ChucK completion candidates from LanguageMetadata chuck API entries.
 *
 * Unlike mininotation, ChucK has no reusable LSP server (AI-G7 investigation).
 * Completion is therefore purely deterministic from the versioned Hathor
 * supported-surface metadata (chuckApi array) plus the built-in ChucK keyword
 * sets (ChuckKeywords). Only items marked as `supported` are included.
 *
 * @param metadata    The validated LanguageMetadata (must pass compatibility check).
 * @param compatibility The result of loadAndValidate() — must be compatible.
 * @param context     The analyzed completion context.
 * @return CompletionCandidates derived from ChucK metadata + keywords.
 */
std::vector<CompletionCandidate> chuckMetadataFallback(
    const language::LanguageMetadata& metadata,
    const language::MetadataCompatibility& compatibility,
    const ContextAnalysis& context);

/**
 * Build a CompletionCandidate from a LanguageMetadata MiniNotationFunction.
 */

/**
 * Build a CompletionCandidate from a LanguageMetadata MiniNotationFunction.
 */
CompletionCandidate makeCandidate(const language::LanguageMetadata& metadata,
                                  const language::MiniNotationFunction& fn);

/**
 * Build CompletionCandidates from LanguageMetadata SampleDefinition entries.
 */
std::vector<CompletionCandidate> makeSampleCandidates(
    const language::LanguageMetadata& metadata,
    const language::SampleDefinition* sampleDef,
    std::string_view prefix);

// ---------------------------------------------------------------------------
// L1: Basic completion
// ---------------------------------------------------------------------------

/**
 * Merge LSP completion results with metadata fallback for basic completion.
 *
 * Strategy:
 *   - Start with LSP-provided items (filtered by prefix).
 *   - If LSP items don't cover a requested item (e.g. a Hathor-specific sample
 *     alias not in the LSP's hardcoded list), add it from metadata.
 *   - De-duplicate: if the same label appears in both LSP and metadata, keep
 *     the LSP version (it has richer information from the actual parser).
 *
 * @param lspItems     Completion items from the LSP server (may be empty if LSP is down).
 * @param metadata     LanguageMetadata (may be nullptr if metadata failed to load).
 * @param compatibility Metadata compatibility check result.
 * @param context      Analyzed completion context.
 * @return Merged CompletionResult.
 */
CompletionResult mergeCompletion(
    const std::vector<CompletionItem>& lspItems,
    const language::LanguageMetadata* metadata,
    const language::MetadataCompatibility* compatibility,
    const ContextAnalysis& context);

// ---------------------------------------------------------------------------
// L2: Signature-aware enrichment
// ---------------------------------------------------------------------------

/**
 * Enrich a completion result with signature information.
 * If the cursor is inside a function call and the LSP provides no signature
 * help, use metadata to look up the function's signature.
 *
 * @param result     The completion result to enrich (modified in place).
 * @param metadata   LanguageMetadata for signature lookup.
 * @param context    The analyzed completion context.
 */
void enrichWithSignatureInfo(CompletionResult& result,
                             const language::LanguageMetadata* metadata,
                             const ContextAnalysis& context) noexcept;

// ---------------------------------------------------------------------------
// L3: Context-aware filtering
// ---------------------------------------------------------------------------

/**
 * Filter completions based on project context (slot, sample bank, front-matter).
 * This is the L3 "context-aware" layer — it narrows results to what's actually
 * relevant in the current project context, not just what matches the prefix.
 *
 * @param result     The completion result to filter (modified in place).
 * @param projectSamples  A set of sample names actually available in the project.
 *                        May be empty (no project context).
 * @param frontMatterSlot The slot name from front-matter, if any.
 */
void filterByProjectContext(CompletionResult& result,
                            const std::unordered_set<std::string>& projectSamples,
                            const std::optional<std::string>& frontMatterSlot) noexcept;

// ---------------------------------------------------------------------------
// Hover merging
// ---------------------------------------------------------------------------

/**
 * Merge LSP hover result with metadata fallback.
 * If the LSP returns no hover content, try the metadata for the symbol at
 * the position.
 */
std::optional<Hover> mergeHover(
    const std::optional<Hover>& lspHover,
    const language::LanguageMetadata* metadata,
    const language::MetadataCompatibility* compatibility,
    std::string_view word);

// ---------------------------------------------------------------------------
// Diagnostics merging
// ---------------------------------------------------------------------------

/**
 * Merge LSP diagnostics with metadata-aware checks.
 *
 * The LSP provides parse-error diagnostics. The metadata layer adds:
 *   - "unsupported function used" (e.g. note(), rev()) → warning severity
 *   - "unknown sample name" → error severity
 *
 * Only checks that pass the metadata compatibility gate are applied.
 */
std::vector<Diagnostic> mergeDiagnostics(
    const std::vector<Diagnostic>& lspDiagnostics,
    const language::LanguageMetadata* metadata,
    const language::MetadataCompatibility* compatibility,
    std::string_view documentText);

// ---------------------------------------------------------------------------
// ChucK diagnostics (AI-G7) — real compiler + metadata-aware checks
// ---------------------------------------------------------------------------

/**
 * Result of ChucK source validation from the real compiler (AI-5).
 * Mirrors audio_worker::ChuckDiagnostic but JUCE-free.
 */
struct ChuckCompileDiagnostic {
    bool        ok;
    int         errorLine;    ///< 1-based line (0 if not provided)
    int         errorColumn;  ///< 1-based column (0 if not provided)
    std::string message;
};

/**
 * Produce LSP diagnostics for ChucK source text.
 *
 * This is the AI-G7 ChucK diagnostic pipeline:
 *   1. Real compiler diagnostic (validateChuckSource / libchuck) — authoritative
 *      for ChucK correctness.
 *   2. Metadata-aware warnings for unsupported ChucK APIs used in the source.
 *
 * When the compiler reports an error, it is emitted as an Error diagnostic.
 * When the compiler reports OK but metadata is available, unsupported API
 * references are emitted as Warning diagnostics.
 *
 * @param compileDiag  The diagnostic from the real compiler (validateChuckSource).
 * @param metadata     LanguageMetadata (may be nullptr if metadata failed to load).
 * @param compatibility Metadata compatibility (may be nullptr if not loaded).
 * @param documentText  The full .ck source text for API reference analysis.
 * @return LSP diagnostics combining compiler + metadata-aware checks.
 */
std::vector<Diagnostic> chuckDiagnostics(
    const ChuckCompileDiagnostic& compileDiag,
    const language::LanguageMetadata* metadata,
    const language::MetadataCompatibility* compatibility,
    std::string_view documentText);

} // namespace lsp
} // namespace hathor
