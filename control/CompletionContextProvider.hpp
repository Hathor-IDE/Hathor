// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * CompletionContextProvider.hpp — AI-G3 Hathor-specific authoring-context provider.
 *
 * llm-ls knows nothing about Hathor. AI-G3 is the Hathor-specific authoring
 * context provider that sits between Hathor's editor/context infrastructure and
 * `llm-ls`: it assembles a *compact, relevant, bounded* context for the
 * *current edit location* and feeds it into the llm-ls FIM request (as
 * `fim.prefix` / `GhostContext.authoringContext`).
 *
 * This is a retrieval/relevance problem, NOT a repository-dump problem. AI-G3
 * never injects the whole project into every completion request. It reuses the
 * shared models already owned by AI-8 — the same
 * EditorContextProvider / LspContextProvider / ProjectReadFacade /
 * LanguageMetadata (AI-3) — so there is no second project-context model.
 *
 * Architecture (AI-G3):
 *
 *   Hathor Editor (cursor/selection/document)
 *         │
 *         ▼
 *   CompletionContextProvider  ← AI-G3 (this class, JUCE-free)
 *      ├── reuses AI-8 providers (EditorContextProvider, LspContextProvider)
 *      ├── reuses AI-2 read facade (ProjectReadFacade)
 *      └── reuses AI-3 metadata + versioned surface
 *         │
 *         ▼  compact, location-aware, bounded JSON
 *     llm-ls  ── fim.prefix / GhostContext.authoringContext
 *         │
 *         ▼
 *      FIM completion (AI-G1/AI-G2)
 *
 * Relevance strategy (deterministic + inspectable):
 *   1. cursor proximity          — diagnostics & source regions near the cursor win
 *   2. syntactic/editor context  — cursor-location classification drives what is selected
 *   3. selected text             — selection scope is prioritised when present
 *   4. active language           — mininotation vs ChucK dispatch
 *   5. current pattern/slot      — from the editor snapshot
 *   6. project assets referenced nearby — name-prefix matching against samples/instruments
 *   7. diagnostics               — proximity-ordered, bounded
 *   8. supported-surface compat  — AI-3 version gate; stale metadata/examples are rejected
 *   9. semantic similarity       — current-project examples preferred over global; valid
 *                                  examples preferred over arbitrary snippets
 *
 * Context is explicitly bounded by ContextBounds (configurable per request;
 * defaults in ContextBounds). Assembly never exceeds maxContextChars.
 *
 * Threading: assemble() is JUCE-free and performs only lock-free / atomic
 * reads on the shared models. It is called from the JUCE message thread
 * (ghost tick) and/or the MCP accept-loop worker thread — NEVER from the
 * JUCE real-time audio callback thread (Requirement #10).
 *
 * Requirement references: AI-G3, AI-1, AI-2, AI-3, AI-4, AI-8, AI-G1, AI-G2, decision #18
 */

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "EditorContextProvider.hpp"
#include "LspContextProvider.hpp"

namespace hathor {
namespace language {
struct LanguageMetadata;
struct MetadataCompatibility;
}
}

namespace hathor::control {

class ProjectReadFacade;

// ---------------------------------------------------------------------------
// Cursor-context classification — deterministic, inspectable.
// ---------------------------------------------------------------------------

/**
 * The semantic category of the edit location, derived from the surrounding
 * text. Drives which subset of metadata / samples / instruments / examples
 * is considered relevant for this specific request (rather than dumping
 * everything).
 *
 * Determined by classifyCursorContext() — a pure, heuristic function over the
 * document text and cursor offset. It is intentionally local (a small window
 * around the cursor) so it is cheap and bounded.
 */
enum class CursorContextKind {
    General,        // no specific classification
    // --- .hathor (mini-notation) ---
    SampleExpr,     // cursor inside a sample/sound string: s "bd ..."
    Transform,      // cursor after or inside a transformation function call
    ScaleExpr,      // cursor inside a scale expression: scale "|minor"
    Rhythm,         // cursor inside a pattern / rhythmic structure
    // --- .ck (ChucK) ---
    UgenDecl,       // cursor near a UGen declaration / instantiation
    Routing,        // cursor near a `=>` audio-graph routing
    Timing,         // cursor near `now` / time advancement
    SynthSection,   // cursor inside an envelope/filter/oscillator section
};

/**
 * A classified cursor location plus a short human-readable label suitable for
 * the model instructions.
 */
struct CursorContext {
    CursorContextKind kind;
    std::string       label;       // e.g. "inside sample string"
    std::string       probe;       // the surrounding text probe used for classification
};

// ---------------------------------------------------------------------------
// Bounded context limits (configurable per request).
// ---------------------------------------------------------------------------

/**
 * Explicit upper bounds on every dimension of the assembled context. These
 * keep a completion request small and fast regardless of project size. They
 * are configurable per-request; see CompletionRequest::bounds.
 */
struct ContextBounds {
    int maxExamples         = 3;     ///< high-quality relevant examples
    int maxSamples          = 20;    ///< SampleBank entries surfaced to the model
    int maxInstruments      = 10;    ///< baked ChucK instruments
    int maxDiagnostics      = 8;     ///< diagnostics (proximity-ordered)
    int maxMetadataEntries  = 20;    ///< functions / ChucK API entries
    int maxGrammarEntries   = 12;    ///< grammar elements surfaced
    int maxOperators        = 8;     ///< mini-notation operators
    int maxRegionLines      = 5;     ///< surrounding source region (lines either side)
    int maxSurroundingChars = 512;   ///< surrounding source chars (region cap)
    int maxContextChars     = 4096;  ///< overall serialized-context character budget
};

// ---------------------------------------------------------------------------
// Request — an edit location + optional bounds override.
// ---------------------------------------------------------------------------

/**
 * Describes a completion request site. All fields except documentText may be
 * omitted; when absent the provider falls back to the current editor snapshot
 * (via EditorContextProvider) and auto-detects the language from the file path.
 */
struct CompletionRequest {
    std::string file;        // absolute path or empty
    std::string uri;         // file:// URI or synthetic URI (slot://...)
    int         line      = 0;   // 0-based cursor line
    int         character = 0;   // 0-based cursor character
    std::string language;   // "mininotation" | "chuck" | "" (auto-infer)

    /// Full current document text. Preferred over the editor snapshot content
    /// because it is already available to the caller (HathorTab) at request time.
    std::string documentText;

    /// Selected text (if a non-empty selection is active).
    std::string selectedText;
    struct Range { int startLine, startChar, endLine, endChar; };
    std::optional<Range> selection;

    /// Optional bounds override. When customBounds is false the provider's
    /// default bounds are used instead.
    bool        customBounds = false;
    ContextBounds bounds;
};

// ---------------------------------------------------------------------------
// Result — the compact context + the serialized form ready for fim.prefix.
// ---------------------------------------------------------------------------

/**
 * The compact, location-aware authoring context for a single completion
 * request, plus the serialized payload that is injected into the llm-ls
 * `fim.prefix` (and stored on GhostContext.authoringContext).
 */
struct CompletionContext {
    /// Compact context JSON (the payload injected into fim.prefix).
    nlohmann::json context;

    /// context.dump() — precomputed so callers can wire it into fim.prefix
    /// without re-serializing, and so tests can assert the exact FIM payload.
    std::string fimPrefix;

    /// Resolved language label ("mininotation" | "chuck" | "unknown").
    std::string language;

    /// Resolved cursor-context label (see CursorContext::label).
    std::string cursorContextLabel;

    /// True if assembly succeeded with real data.
    bool ok = true;

    /// Populated when ok == false.
    std::string error;
};

// ---------------------------------------------------------------------------
// CompletionContextProvider — the AI-G3 assembler.
// ---------------------------------------------------------------------------

/**
 * Assembles a compact, targeted, edit-location-specific authoring context for
 * llm-ls FIM completion.
 *
 * Constructed once per ControlInterface lifetime. All provider pointers may be
 * null — when a provider is absent the corresponding section reports
 * "unavailable" rather than crashing. The ProjectReadFacade reference is
 * required (it is the canonical AI-2 read facade).
 *
 * Thread-safety: assemble() is called from the MCP accept-loop worker thread
 * and/or the JUCE message thread (ghost tick). It reads thread-safe snapshots
 * from the providers and ProjectReadFacade (lock-free / atomic reads). It must
 * NEVER be called from the JUCE real-time audio thread.
 */
class CompletionContextProvider {
public:
    CompletionContextProvider(ProjectReadFacade&                       readFacade,
                              EditorContextProvider*                   editorCtx,
                              LspContextProvider*                      lspCtx,
                              const hathor::language::LanguageMetadata*    metadata,
                              const hathor::language::MetadataCompatibility* compat);

    ~CompletionContextProvider() = default;

    CompletionContextProvider(const CompletionContextProvider&)            = delete;
    CompletionContextProvider& operator=(const CompletionContextProvider&) = delete;

    /// Install/replace the editor context provider (may be null).
    void setEditorContextProvider(EditorContextProvider* provider) noexcept { editorCtx_ = provider; }

    /// Install/replace the LSP context provider (may be null).
    void setLspContextProvider(LspContextProvider* provider) noexcept { lspCtx_ = provider; }

    /// Update the LanguageMetadata pointer (e.g. after a hot-reload).
    void setMetadata(const hathor::language::LanguageMetadata* metadata,
                     const hathor::language::MetadataCompatibility* compat) noexcept;

    /// Set default bounds used when a request does not override them.
    void setBounds(ContextBounds bounds) noexcept { defaultBounds_ = bounds; }

    /// Assemble the compact, location-aware, bounded authoring context for the
    /// given edit location. Returns a CompletionContext whose `context` JSON is
    /// ready to be injected as llm-ls fim.prefix / GhostContext.authoringContext.
    CompletionContext assemble(const CompletionRequest& req) const;

    /// Convenience: assemble and return just the context JSON.
    nlohmann::json assembleJson(const CompletionRequest& req) const
    {
        return assemble(req).context;
    }

private:
    // --- Section assemblers (each returns a bounded JSON fragment) ---

    /// Classify the cursor location from the document text (deterministic).
    CursorContext classifyCursorContext(std::string_view documentText,
                                        int line, int character,
                                        std::string_view language) const;

    /// Bounded surrounding source region (lines around the cursor).
    nlohmann::json assembleRegion(const CompletionRequest& req,
                                  const EditorContextSnapshot& snap) const;

    /// Bounded, proximity-ordered diagnostics (compiler + LSP, AI-4).
    nlohmann::json assembleDiagnostics(const CompletionRequest& req,
                                       const EditorContextSnapshot& snap,
                                       std::string_view language) const;

    /// Relevance-filtered language metadata (AI-3), version-gated.
    nlohmann::json assembleMetadata(const CompletionRequest& req,
                                    std::string_view language) const;

    /// Bounded, relevance-filtered sample bank entries (AI-2).
    nlohmann::json assembleSamples(const CompletionRequest& req,
                                  const CursorContext& ctx) const;

    /// Bounded, relevance-filtered baked ChucK instruments (AI-2/AI-6).
    nlohmann::json assembleInstruments(const CompletionRequest& req,
                                      const CursorContext& ctx) const;

    /// Bounded, version-compatible few-shot examples.
    nlohmann::json assembleExamples(const CompletionRequest& req,
                                    const CursorContext& ctx) const;

    /// Bounded runtime/song context (BPM, transport, slot, AI-2).
    nlohmann::json assembleRuntime(const CompletionRequest& req,
                                   const EditorContextSnapshot& snap,
                                   std::string_view language) const;

    /// Compact project overview (not the whole repo — only what is relevant).
    nlohmann::json assembleProject(const CompletionRequest& req) const;

    // --- Helpers ---

    /// Resolve the editor snapshot, applying request overrides for cursor/file.
    EditorContextSnapshot resolveSnapshot(const CompletionRequest& req) const;

    /// Resolve the language ("mininotation" | "chuck" | "unknown").
    std::string inferLanguage(std::string_view file,
                              const std::string& reqLanguage,
                              const EditorContextSnapshot& snap) const noexcept;

    /// Resolve bounds (request override when customBounds is set, else defaults).
    const ContextBounds& resolveBounds(const CompletionRequest& req) const noexcept
    {
        return req.customBounds ? req.bounds : defaultBounds_;
    }

    /// Language label used in the JSON output.
    static std::string_view languageLabel(std::string_view language) noexcept;

    /// Metadata version block (AI-3) — always present, versioned.
    nlohmann::json metadataVersionBlock() const;

    /// True if the loaded metadata is compatible with the running surface.
    bool metadataCompatible() const noexcept;

    /// Bounded helper: collect the first N elements of a vector<string> as JSON.
    static nlohmann::json boundedNames(const std::vector<std::string>& names,
                                       int maxEntries);

    /// Bounded helper: collect the first N entries of a JSON array.
    static nlohmann::json truncateArray(const nlohmann::json& arr, int maxEntries);

    ProjectReadFacade&                              readFacade_;
    EditorContextProvider*                        editorCtx_;
    LspContextProvider*                           lspCtx_;
    const hathor::language::LanguageMetadata*        metadata_;
    const hathor::language::MetadataCompatibility*     compat_;
    ContextBounds                                   defaultBounds_;
};

} // namespace hathor::control
