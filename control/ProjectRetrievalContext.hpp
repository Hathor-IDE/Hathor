// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ProjectRetrievalContext.hpp — bounded, ranked, project-aware retrieval layer
 * (J-5) that sits above ProjectSymbolIndex and feeds both AI-G3 (completion)
 * and AI-8 (dynamic authoring context).
 *
 * J-5 retrieval strategy (deterministic + inspectable, no vector DB):
 *   1. Symbol-based lookup    — find the identifier at the cursor, look it up
 *                              in the ProjectSymbolIndex.
 *   2. File/metadata lookup    — find files in the same project + language.
 *   3. Targeted project search — name-prefix matching against indexed symbols
 *                              (for partial-token completion).
 *   4. Ranking                 — language match > symbol match > context-type
 *                              match > recency (file mtime).
 *   5. Bounded output           — max snippets, max chars per snippet,
 *                              max total context chars.
 *
 * The retrieved snippets are injected as a `project_retrieval` section into the
 * existing AI-G3 / AI-8 assembled context JSON, which flows into the llm-ls
 * FIM request path (fim.prefix). No separate AI completion service is created.
 *
 * Threading: retrieve() is JUCE-free and performs only lock-free / atomic
 * reads on the ProjectSymbolIndex snapshot. Safe on the ghost-tick message
 * thread and the MCP accept-loop worker thread — NEVER on the JUCE real-time
 * audio callback thread (Requirement #10).
 *
 * Versioning / invalidation: ProjectSymbolIndex maintains a versionToken
 * (hash of file paths + mtimes). retrieve() includes this token in the output
 * so downstream consumers can detect staleness. When project files change,
 * maybeReindex() on the index updates the token.
 *
 * Requirement references: J-5, AI-G3, AI-8, AI-2, AI-3
 */

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

#include "hathor/ProjectSymbolIndex.hpp"

namespace hathor::control {

/**
 * Bounded limits for project retrieval output.
 */
struct RetrievalBounds {
    int maxSnippets        = 5;     ///< maximum number of snippets to retrieve
    int maxSnippetChars    = 200;   ///< maximum characters per snippet
    int maxTotalChars      = 2048;  ///< hard cap on total retrieved context chars
    int maxFiles           = 10;    ///< maximum number of files to list
    int maxSearchedSymbols = 50;    ///< cap for prefix/content search
};

/**
 * Parsed cursor-context information used to guide retrieval ranking.
 * Mirrors the AI-G3 CursorContextKind enum values.
 */
struct RetrievalContext {
    std::string language;           ///< "mininotation" | "chuck" | ""
    std::string cursorContextLabel;  ///< e.g. "inside sample string"
    std::string cursorContextKind;   ///< e.g. "sample_expr", "routing", "ugen_decl"
    std::string typedText;           ///< the partial token being typed at the cursor
    std::string currentFile;         ///< path of the file being edited
};

/**
 * ProjectRetrievalContext — the J-5 bounded, ranked retrieval layer.
 *
 * Constructed once per ControlInterface lifetime, bound to a
 * ProjectSymbolIndex. The index pointer may be null at construction and
 * installed later via setIndex(). All lookups on a null index return empty
 * results with ok=false.
 *
 * Thread-safety: retrieve() reads a lock-free snapshot from the index. Safe
 * on the message thread and the MCP accept-loop worker thread.
 */
class ProjectRetrievalContext {
public:
    ProjectRetrievalContext() = default;

    /**
     * Construct with a bound ProjectSymbolIndex (may be null — set later
     * via setIndex()).
     */
    explicit ProjectRetrievalContext(hathor::language::ProjectSymbolIndex* index);

    /** Install or replace the symbol index (may be null). */
    void setIndex(hathor::language::ProjectSymbolIndex* index) noexcept { index_ = index; }

    /** Set default retrieval bounds (overridable per-request). */
    void setBounds(RetrievalBounds bounds) noexcept { defaultBounds_ = bounds; }

    /**
     * Retrieve a bounded, ranked set of project snippets relevant to the
     * given retrieval context.
     *
     * @param ctx    The cursor context (language, kind, typed text, etc.).
     * @param bounds Optional per-request bounds override. When zero-valued,
     *               defaults are used.
     * @return JSON object:
     *   {
     *     "ok": bool,
     *     "version_token": "...",    // freshness identifier
     *     "query": "...",            // the typed text used for symbol lookup
     *     "snippets": [
     *       {
     *         "name": "bd",             // matched symbol name
     *         "file": "/path/to/file",   // source file
     *         "uri": "file://...",
     *         "line": 3,                 // 1-based
     *         "kind": "SampleRef",       // symbol kind
     *         "language": "mininotation",
     *         "snippet": "d1 $ s \"bd sn\"", // bounded source excerpt
     *         "relevance_score": 0.95    // 0..1 ranking score
     *       }, ...
     *     ],
     *     "files": [ ... ],            // bounded file list
     *     "count": N,
     *     "max": bounds.maxSnippets,
     *     "truncated": bool            // true if results were cut short
     *   }
     */
    nlohmann::json retrieve(const RetrievalContext& ctx,
                            const RetrievalBounds& bounds = {}) const;

    /**
     * Retrieve file-level metadata (previews + paths) for files relevant to
     * the current editing language. Bounded by bounds.maxFiles.
     */
    nlohmann::json retrieveFiles(const RetrievalContext& ctx,
                                 const RetrievalBounds& bounds = {}) const;

private:
    /**
     * Compute a relevance score (0..1) for a symbol relative to the query
     * context. Higher is better.
     */
    static double relevanceScore(
        const hathor::language::IndexedSymbol& sym,
        std::string_view query,
        std::string_view cursorContextKind,
        std::string_view language) noexcept;

    /**
     * Return the symbol-kind label string for JSON output.
     */
    static std::string_view kindLabel(hathor::language::SymbolKind kind) noexcept;

    /**
     * Resolve bounds (override or defaults).
     */
    RetrievalBounds resolveBounds(const RetrievalBounds& override) const noexcept;

    /**
     * Truncate a string to maxLen, appending "...[trunc]" if truncated.
     */
    static std::string truncateStr(std::string_view content, int maxLen);

    hathor::language::ProjectSymbolIndex* index_ = nullptr;
    RetrievalBounds defaultBounds_;
};

} // namespace hathor::control
