// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_PROJECT_SYMBOL_INDEX_HPP
#define HATHOR_PROJECT_SYMBOL_INDEX_HPP

/**
 * ProjectSymbolIndex.hpp — lightweight, versioned, text-based project symbol
 * and file index for completion retrieval (J-5).
 *
 * J-5 (Codebase / Project Retrieval for Completion): makes AI completion
 * project-aware by indexing relevant symbols, snippets, and file-level
 * metadata from the user's project source files (.hathor and .ck) so that
 * relevant definitions from elsewhere in the project can be retrieved and
 * injected into the existing llm-ls/FIM path (AI-G3 / AI-8).
 *
 * Design constraints:
 *   - JUCE-free. Uses only the standard library and nlohmann/json.
 *   - NO vector database or full RAG system. Indexing is text-based with
 *     linear-scan / prefix / exact matching. No embeddings at this stage
 *     (see J-5 acceptance: embeddings only if the simpler approach is
 *     proven insufficient).
 *   - Bounded: scanning is capped by maxFiles and maxSymbolsPerFile.
 *   - Versioned: the index is bound to a versionToken derived from file
 *     paths + modification timestamps. When files change, only the affected
 *     files are re-indexed (maybeReindex). The token is always surfaced so
 *     callers can detect staleness.
 *   - Safe off the audio thread: all work is filesystem I/O + text parsing.
 *     Never called from the JUCE real-time audio callback thread.
 *
 * Architecture (J-5):
 *
 *   Project files (.hathor / .ck)
 *         │
 *         ▼
 *   ProjectSymbolIndex  ← this class (J-5 indexer, in engine/)
 *      ├── symbols:   name → {path, line, snippet, kind, language, mtime}
 *      ├── files:     path → {language, mtime, symbol_count, snippet}
 *      └── versionToken: hash of (path + mtime) over all tracked files
 *         │
 *         ▼
 *   ProjectRetrievalContext  ← in control/ (bounded + ranked)
 *         │
 *         ▼
 *   CompletionContextProvider (AI-G3)  ── fim.prefix / authoringContext
 *
 * Requirement references: J-5, AI-G3, AI-8, AI-3
 */

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hathor::language {

// ---------------------------------------------------------------------------
// Symbol kind — what kind of construct was extracted.
// ---------------------------------------------------------------------------

enum class SymbolKind : std::uint8_t {
    SampleRef,      ///< a sample name referenced in a mini-notation pattern
    FunctionCall,   ///< a mini-notation function call (e.g. fast(), slow())
    UgenInstantiation, ///< a ChucK UGen instantiation (e.g. SinOsc osc)
    UgenRouting,    ///< a ChucK `=>` routing connection
    ChuckFunction,  ///< a ChucK `fun` function definition
    ChuckClass,     ///< a ChucK `class` definition
    ChuckTiming,    ///< a ChucK `now` / time advancement usage
    InstrumentDef,  ///< a baked/rendered instrument name (from .ck source)
    PatternSlot,    ///< a .hathor front-matter slot name
    PatternVar,     ///< a mini-notation `var` or named pattern reference
    GenericSymbol,  ///< catch-all for identifier references
};

/**
 * A single indexed symbol entry — one occurrence of a name in a project file.
 *
 * The snippet is bounded to a small window of source text around the line
 * so that retrieval can return compact, context-rich extracts.
 */
struct IndexedSymbol {
    std::string name;           ///< the matched symbol name (e.g. "bd", "SinOsc")
    std::string filePath;       ///< absolute path to the source file
    std::string uri;            ///< file:// URI (or path-based URI)
    int         line         = 0;  ///< 1-based source line where the symbol occurs
    int         column       = 0;  ///< 1-based column (best-effort, 0 if unknown)
    SymbolKind  kind;            ///< what kind of construct
    std::string language;       ///< "mininotation" | "chuck" | "unknown"
    std::string snippet;        ///< bounded source around the symbol (truncated)
    std::uint64_t fileMtimeMs = 0; ///< file modification time at index time (ms since epoch)
};

/**
 * A single indexed file — lightweight metadata for retrieval ranking.
 */
struct IndexedFile {
    std::string path;
    std::string uri;
    std::string language;       ///< "mininotation" | "chuck" | "unknown"
    std::uint64_t mtimeMs      = 0;
    int         symbolCount    = 0;
    std::string preview;        ///< first N lines of the file (bounded)
};

/**
 * Index configuration — controls the bounded scanning behaviour.
 */
struct IndexConfig {
    int maxFiles           = 200;   ///< hard cap on files scanned per reindex
    int maxSymbolsPerFile  = 200;   ///< hard cap on symbols extracted per file
    int maxSnippetChars    = 256;   ///< max snippet text length per indexed symbol
    int maxPreviewLines    = 5;     ///< max lines of file preview
    int maxPreviewChars    = 512;   ///< max bytes of file preview
};

/**
 * ProjectSymbolIndex — the lightweight, versioned, text-based project indexer.
 *
 * Constructed once; re-indexable via reindex() / maybeReindex(). All methods
 * that return data return by-value snapshots (the index is internally
 * single-writer, multi-reader-safe via a mutex — safe to call from the MCP
 * accept-loop worker thread and the ghost-tick message thread, but NEVER from
 * the JUCE audio callback thread).
 *
 * Requirement references: J-5, AI-G2 (JUCE-free), threading requirement #10
 */
class ProjectSymbolIndex {
public:
    ProjectSymbolIndex();
    ~ProjectSymbolIndex();

    ProjectSymbolIndex(const ProjectSymbolIndex&) = delete;
    ProjectSymbolIndex& operator=(const ProjectSymbolIndex&) = delete;

    /**
     * Set configuration (bounds for scanning/extraction).
     * Thread-safe.
     */
    void setConfig(IndexConfig cfg) noexcept;

    /**
     * Full re-index of the given project directory.
     * Scans for .hathor and .ck files, extracts symbols and file metadata.
     * Bounded by config.maxFiles. Safe to call from any non-audio thread.
     *
     * @param projectDir  The project root to scan. If empty, the index is cleared.
     */
    void reindex(const std::filesystem::path& projectDir);

    /**
     * Lazy re-index: check file modification times and re-scan only files
     * whose mtime has changed since the last index. If the project directory
     * itself has changed (new/deleted files), falls back to a full reindex.
     *
     * @param projectDir  The project root to scan.
     * @return true if the index was updated (any file changed), false if unchanged.
     */
    bool maybeReindex(const std::filesystem::path& projectDir);

    /**
     * Clear all indexed data. The index becomes empty and versionToken()
     * reflects the cleared state.
     */
    void clear() noexcept;

    /**
     * Look up all occurrences of @p name in the index.
     * Returns symbols sorted by relevance (exact match first, then prefix match,
     * then file modification recency). If @p language is non-empty, only
     * symbols in that language are returned.
     *
     * Bounded by config.maxSymbolsPerFile total results.
     */
    std::vector<IndexedSymbol> lookupSymbol(std::string_view name,
                                            std::string_view language = {}) const;

    /**
     * Search for symbols whose name starts with @p prefix.
     * Used for completion-style prefix matching.
     * Bounded by config.maxSymbolsPerFile total results.
     */
    std::vector<IndexedSymbol> searchByPrefix(std::string_view prefix,
                                              std::string_view language = {}) const;

    /**
     * Search for symbols whose snippet contains @p queryText (substring search).
     * Bounded by maxResults.
     */
    std::vector<IndexedSymbol> searchByContent(std::string_view queryText,
                                               std::string_view language = {},
                                               int maxResults = 50) const;

    /**
     * Return all files in the index, optionally filtered by language.
     */
    std::vector<IndexedFile> listFiles(std::string_view language = {}) const;

    /**
     * Return the current version token — a string that uniquely identifies
     * the current index state (file paths + modification timestamps).
     * Callers compare this to detect staleness.
     *
     * An empty token means the index has never been populated (no project loaded).
     */
    std::string versionToken() const noexcept;

    /**
     * The number of symbols currently in the index.
     */
    std::size_t symbolCount() const noexcept;

    /**
     * The number of files currently in the index.
     */
    std::size_t fileCount() const noexcept;

    /**
     * Whether the index has never been populated.
     */
    bool empty() const noexcept;

private:
    IndexConfig config_;

    struct IndexData {
        std::vector<IndexedSymbol> symbols;
        std::vector<IndexedFile>   files;
        std::string                versionToken;
        std::filesystem::path      projectDir;

        // Path → mtime map for incremental reindexing.
        std::unordered_map<std::string, std::uint64_t> mtimes;
    };

    // Single mutable copy; readers take a snapshot via copy-on-write.
    // The mutex protects all access.
    mutable std::mutex mutex_;
    IndexData          data_;
    IndexData          snapshot_;  // last stable snapshot for lock-free reads

    /**
     * Update the snapshot from data_ (called after mutation).
     */
    void publishSnapshot() noexcept;

    /**
     * Index a single file: parse and extract symbols.
     */
    void indexFile(const std::filesystem::path& path,
                   const std::string& language,
                   std::uint64_t mtimeMs,
                   std::vector<IndexedSymbol>& outSymbols,
                   int& outSymbolCount);
};

} // namespace hathor::language

#endif // HATHOR_PROJECT_SYMBOL_INDEX_HPP
