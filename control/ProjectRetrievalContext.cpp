// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ProjectRetrievalContext.cpp — bounded, ranked, project-aware retrieval (J-5).
 *
 * Implements the J-5 retrieval strategy:
 *   1. Symbol-based lookup (exact + prefix)
 *   2. File/metadata lookup (language-scoped file listing)
 *   3. Targeted project search (prefix + content search)
 *   4. Ranking by language match, symbol match, context-type match, recency
 *   5. Bounded output (maxSnippets, maxSnippetChars, maxTotalChars)
 *
 * JUCE-free. Safe on the message thread and MCP accept-loop worker thread.
 *
 * Requirement references: J-5, AI-G3, AI-8, AI-2
 */

#include "ProjectRetrievalContext.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hathor::control {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ProjectRetrievalContext::ProjectRetrievalContext(
    hathor::language::ProjectSymbolIndex* index)
    : index_(index)
{
}

// ---------------------------------------------------------------------------
// Bounds resolution
// ---------------------------------------------------------------------------

RetrievalBounds ProjectRetrievalContext::resolveBounds(
    const RetrievalBounds& override) const noexcept
{
    RetrievalBounds result = defaultBounds_;
    // Treat a zero-valued override field as "use default".
    if (override.maxSnippets > 0)        result.maxSnippets        = override.maxSnippets;
    if (override.maxSnippetChars > 0)    result.maxSnippetChars    = override.maxSnippetChars;
    if (override.maxTotalChars > 0)      result.maxTotalChars      = override.maxTotalChars;
    if (override.maxFiles > 0)           result.maxFiles           = override.maxFiles;
    if (override.maxSearchedSymbols > 0) result.maxSearchedSymbols = override.maxSearchedSymbols;
    return result;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string_view ProjectRetrievalContext::kindLabel(
    hathor::language::SymbolKind kind) noexcept
{
    switch (kind) {
        case hathor::language::SymbolKind::SampleRef:            return "SampleRef";
        case hathor::language::SymbolKind::FunctionCall:         return "FunctionCall";
        case hathor::language::SymbolKind::UgenInstantiation:      return "UgenInstantiation";
        case hathor::language::SymbolKind::UgenRouting:          return "UgenRouting";
        case hathor::language::SymbolKind::ChuckFunction:        return "ChuckFunction";
        case hathor::language::SymbolKind::ChuckClass:           return "ChuckClass";
        case hathor::language::SymbolKind::ChuckTiming:          return "ChuckTiming";
        case hathor::language::SymbolKind::InstrumentDef:        return "InstrumentDef";
        case hathor::language::SymbolKind::PatternSlot:          return "PatternSlot";
        case hathor::language::SymbolKind::PatternVar:           return "PatternVar";
        case hathor::language::SymbolKind::GenericSymbol:        return "GenericSymbol";
    }
    return "GenericSymbol";
}

std::string ProjectRetrievalContext::truncateStr(std::string_view content, int maxLen)
{
    if (maxLen <= 0 || static_cast<int>(content.size()) <= maxLen)
        return std::string(content);
    return std::string(content.substr(0, static_cast<std::size_t>(maxLen - 3))) + "...";
}

// ---------------------------------------------------------------------------
// Relevance scoring
// ---------------------------------------------------------------------------

double ProjectRetrievalContext::relevanceScore(
    const hathor::language::IndexedSymbol& sym,
    std::string_view query,
    std::string_view cursorContextKind,
    std::string_view language) noexcept
{
    double score = 0.0;

    // --- 1. Language match (highest weight) ---
    // A symbol in the same language as the cursor context is always more
    // relevant — ChucK code should inform ChucK completion, not mininotation.
    if (language.empty() || sym.language == language)
        score += 0.40;

    // --- 2. Exact symbol name match ---
    if (sym.name == query)
        score += 0.35;
    else if (!query.empty() && sym.name.size() >= query.size() &&
             sym.name.compare(0, query.size(), query) == 0)
    {
        // Prefix match — partial token the user is typing.
        score += 0.25;
    }

    // --- 3. Context-type match ---
    // If the cursor is in a sample_expr context and the symbol is a SampleRef,
    // bump the score. If the cursor is in a routing context and the symbol is
    // a UgenInstantiation or UgenRouting, bump similarly.
    if (!cursorContextKind.empty())
    {
        const bool isSampleCtx  = (cursorContextKind == "sample_expr" ||
                                   cursorContextKind == "rhythm");
        const bool isUgenCtx    = (cursorContextKind == "ugen_decl" ||
                                   cursorContextKind == "routing" ||
                                   cursorContextKind == "synth_section" ||
                                   cursorContextKind == "timing");
        const bool isFuncCtx    = (cursorContextKind == "transform");

        if (isSampleCtx &&
            (sym.kind == hathor::language::SymbolKind::SampleRef ||
             sym.kind == hathor::language::SymbolKind::PatternSlot))
            score += 0.15;
        if (isUgenCtx &&
            (sym.kind == hathor::language::SymbolKind::UgenInstantiation ||
             sym.kind == hathor::language::SymbolKind::UgenRouting ||
             sym.kind == hathor::language::SymbolKind::ChuckFunction ||
             sym.kind == hathor::language::SymbolKind::ChuckClass))
            score += 0.15;
        if (isFuncCtx &&
            sym.kind == hathor::language::SymbolKind::FunctionCall)
            score += 0.15;
        // Generic symbols get a small bump in any context if they match.
        if (sym.kind == hathor::language::SymbolKind::GenericSymbol)
            score += 0.05;
    }
    else
    {
        // No specific cursor context — give a small bump to sample refs and
        // UGen instantiations (the most commonly completed constructs).
        if (sym.kind == hathor::language::SymbolKind::SampleRef ||
            sym.kind == hathor::language::SymbolKind::UgenInstantiation)
            score += 0.08;
    }

    // --- 4. Recency (file modification time) ---
    // More recently modified files are more likely to be relevant.
    // This is a small contribution — recency is a tie-breaker, not a primary
    // signal, since mtime can be stale.
    // (sym.fileMtimeMs is a uint64 timestamp; we can't know the max, so
    // we give a small constant bump for any symbol from a recently-modified
    // file — but since we don't have a "now", we skip the temporal decay
    // and instead use file recency as a weak signal via the sort order
    // in ProjectSymbolIndex::lookupSymbol which already sorts by mtime desc.)

    // Clamp to [0, 1].
    return std::clamp(score, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// retrieve
// ---------------------------------------------------------------------------

nlohmann::json ProjectRetrievalContext::retrieve(
    const RetrievalContext& ctx,
    const RetrievalBounds& boundsOverride) const
{
    const auto bounds = resolveBounds(boundsOverride);

    nlohmann::json result;
    result["ok"] = false;
    result["snippets"] = nlohmann::json::array();
    result["files"] = nlohmann::json::array();
    result["count"] = 0;
    result["max"] = bounds.maxSnippets;
    result["truncated"] = false;

    if (index_ == nullptr)
    {
        result["reason"] = "ProjectSymbolIndex not bound";
        return result;
    }

    result["ok"] = true;
    result["version_token"] = index_->versionToken();
    result["query"] = ctx.typedText;

    // If the index is empty (never indexed), report ok=false with a reason.
    if (index_->empty())
    {
        result["ok"] = false;
        result["reason"] = "ProjectSymbolIndex is empty (no files indexed)";
        result["count"] = 0;
        result["truncated"] = false;
        return result;
    }

    if (!ctx.currentFile.empty())
        result["current_file"] = ctx.currentFile;

    // --- Step 1: Symbol-based lookup ---
    // Find the identifier at the cursor by name. Look it up in the index.
    // If the typed text is a prefix of a symbol name, also include prefix matches.

    std::vector<hathor::language::IndexedSymbol> foundSymbols;

    if (!ctx.typedText.empty())
    {
        // Exact + prefix matches from the symbol index.
        auto exact = index_->lookupSymbol(ctx.typedText, ctx.language);
        foundSymbols.insert(foundSymbols.end(),
                            std::make_move_iterator(exact.begin()),
                            std::make_move_iterator(exact.end()));

        auto prefixMatches = index_->searchByPrefix(ctx.typedText, ctx.language);
        for (const auto& sym : prefixMatches)
        {
            // Avoid duplicates — check if already in foundSymbols by (name, file, line).
            bool dupe = false;
            for (const auto& existing : foundSymbols)
            {
                if (existing.name == sym.name &&
                    existing.filePath == sym.filePath &&
                    existing.line == sym.line)
                {
                    dupe = true;
                    break;
                }
            }
            if (!dupe)
                foundSymbols.push_back(sym);
        }
    }

    // --- Step 2: File/metadata lookup ---
    // Even if no symbol matches the typed text, return a few relevant files
    // in the same language for broader context.
    auto files = index_->listFiles(ctx.language);
    // Exclude the current file from snippets (we already have it locally).
    {
        std::vector<hathor::language::IndexedFile> otherFiles;
        for (const auto& f : files)
        {
            if (f.path != ctx.currentFile)
                otherFiles.push_back(f);
        }
        std::swap(files, otherFiles);
    }

    // If no symbol was found, fall back to content search on files that
    // are relevant context-type-wise.
    if (foundSymbols.empty() && !files.empty())
    {
        // Derive a search query from the context kind.
        std::string_view searchQuery;
        if (ctx.cursorContextKind == "routing" || ctx.cursorContextKind == "synth_section")
            searchQuery = "=>";
        else if (ctx.cursorContextKind == "timing")
            searchQuery = "now";
        else if (!ctx.typedText.empty())
            searchQuery = ctx.typedText;

        if (!searchQuery.empty())
        {
            auto contentMatches = index_->searchByContent(
                searchQuery, ctx.language, bounds.maxSearchedSymbols);
            foundSymbols.insert(foundSymbols.end(),
                                std::make_move_iterator(contentMatches.begin()),
                                std::make_move_iterator(contentMatches.end()));
        }
    }

    // If still empty and no typed text, pick a few snippets from other files
    // to provide general project context.
    if (foundSymbols.empty() && !files.empty())
    {
        for (const auto& f : files)
        {
            if (static_cast<int>(foundSymbols.size()) >= bounds.maxSnippets)
                break;
            // Create a pseudo-symbol from the file preview.
            hathor::language::IndexedSymbol sym;
            sym.name = f.path.substr(f.path.find_last_of('/') + 1);
            sym.filePath = f.path;
            sym.uri = f.uri;
            sym.line = 1;
            sym.kind = hathor::language::SymbolKind::GenericSymbol;
            sym.language = f.language;
            sym.fileMtimeMs = f.mtimeMs;
            sym.snippet = truncateStr(f.preview, bounds.maxSnippetChars);
            foundSymbols.push_back(std::move(sym));
        }
    }

    // --- Step 3: Rank + bound ---
    // Compute relevance scores and sort descending.
    struct RankedSymbol {
        hathor::language::IndexedSymbol sym;
        double score;
    };
    std::vector<RankedSymbol> ranked;
    ranked.reserve(foundSymbols.size());

    for (auto& sym : foundSymbols)
    {
        double s = relevanceScore(sym, ctx.typedText,
                                  ctx.cursorContextKind, ctx.language);
        ranked.push_back({std::move(sym), s});
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const RankedSymbol& a, const RankedSymbol& b)
    {
        if (a.score != b.score) return a.score > b.score;
        // Tie-break: more recent file first.
        return a.sym.fileMtimeMs > b.sym.fileMtimeMs;
    });

    // --- Step 4: Bound by count and total character budget ---
    nlohmann::json snippets = nlohmann::json::array();
    int totalChars = 0;
    int emitted = 0;

    for (const auto& rs : ranked)
    {
        if (emitted >= bounds.maxSnippets)
            break;

        // Truncate the snippet.
        std::string snippetText = truncateStr(rs.sym.snippet, bounds.maxSnippetChars);

        // Check total character budget.
        // Approximate JSON field overhead.
        const int approxOverhead = 120;
        if (totalChars + static_cast<int>(snippetText.size()) + approxOverhead > bounds.maxTotalChars)
        {
            result["truncated"] = true;
            break;
        }

        nlohmann::json entry;
        entry["name"] = rs.sym.name;
        entry["file"] = rs.sym.filePath;
        entry["uri"] = rs.sym.uri;
        entry["line"] = rs.sym.line;
        entry["column"] = rs.sym.column;
        entry["kind"] = kindLabel(rs.sym.kind);
        entry["language"] = rs.sym.language;
        entry["snippet"] = snippetText;
        entry["relevance_score"] = rs.score;

        snippets.push_back(std::move(entry));
        totalChars += static_cast<int>(snippetText.size()) + approxOverhead;
        ++emitted;
    }

    result["snippets"] = std::move(snippets);

    // --- File listing (bounded) ---
    nlohmann::json boundedFiles = nlohmann::json::array();
    int fileEmitted = 0;
    for (const auto& f : files)
    {
        if (fileEmitted >= bounds.maxFiles) break;
        nlohmann::json fj;
        fj["path"] = f.path;
        fj["uri"] = f.uri;
        fj["language"] = f.language;
        fj["symbol_count"] = f.symbolCount;
        fj["preview"] = truncateStr(f.preview, bounds.maxSnippetChars / 2);
        boundedFiles.push_back(std::move(fj));
        ++fileEmitted;
    }
    result["files"] = std::move(boundedFiles);

    result["count"] = emitted;
    result["file_count"] = fileEmitted;
    result["total_symbols_indexed"] = static_cast<int>(index_->symbolCount());
    result["total_files_indexed"] = static_cast<int>(index_->fileCount());

    return result;
}

// ---------------------------------------------------------------------------
// retrieveFiles
// ---------------------------------------------------------------------------

nlohmann::json ProjectRetrievalContext::retrieveFiles(
    const RetrievalContext& ctx,
    const RetrievalBounds& boundsOverride) const
{
    const auto bounds = resolveBounds(boundsOverride);

    nlohmann::json result;
    result["ok"] = false;
    result["files"] = nlohmann::json::array();
    result["count"] = 0;
    result["max"] = bounds.maxFiles;

    if (index_ == nullptr)
    {
        result["reason"] = "ProjectSymbolIndex not bound";
        return result;
    }

    result["ok"] = true;
    result["version_token"] = index_->versionToken();

    auto files = index_->listFiles(ctx.language);

    nlohmann::json fileArr = nlohmann::json::array();
    int emitted = 0;
    for (const auto& f : files)
    {
        if (emitted >= bounds.maxFiles) break;
        nlohmann::json fj;
        fj["path"] = f.path;
        fj["uri"] = f.uri;
        fj["language"] = f.language;
        fj["symbol_count"] = f.symbolCount;
        fj["preview"] = truncateStr(f.preview, bounds.maxSnippetChars / 2);
        fileArr.push_back(std::move(fj));
        ++emitted;
    }

    result["files"] = std::move(fileArr);
    result["count"] = emitted;
    result["total_files_indexed"] = static_cast<int>(index_->fileCount());

    return result;
}

} // namespace hathor::control
