// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * SymbolSearchModel.hpp — JUCE-free symbol search model.
 *
 * Aggregates symbol search results from two sources:
 *   1. Strudel LSP workspace/symbol (for .hathor files — functions, samples, scales)
 *   2. HathorLanguageMetadata.json (fallback / supplement for known Strudel functions)
 *
 * For .ck files, symbols come from ChuckTokeniser + ChuckCompiler metadata.
 *
 * The model is pure logic (no JUCE). The JUCE-dependent SymbolSearchPanel
 * owns this model and renders results.
 *
 * Requirement references: L-2 §3
 */

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "hathor/LanguageMetadata.hpp"

namespace hathor::ui {

/**
 * One symbol search result.
 */
struct SymbolSearchResult {
    std::string name;                    ///< symbol name (e.g. "s", "bd", "major")
    std::string kind;                    ///< "function" | "sample" | "scale" | "variable" | "chuck"
    std::string detail;                  ///< signature or description
    std::string containerName;           ///< parent context (e.g. file name)
    std::filesystem::path filePath;      ///< file where the symbol is defined (may be virtual for builtins)
    int          line         = 0;       ///< 0-based or -1 if N/A
    int          column       = 0;       ///< 0-based column or -1
    bool         isBuiltin    = false;   ///< true if from metadata, not a real file
    std::string  uri;                    ///< file:// URI or virtual URI
};

/**
 * SymbolSearchModel
 *
 * Searches for symbols across the workspace by name. Can be populated from:
 *   - LSP workspace/symbol results (async callback from HathorLspClient)
 *   - LanguageMetadata functions (synchronous, local)
 *   - File-based symbol indexing (.hathor and .ck files in the workspace)
 */
class SymbolSearchModel
{
public:
    explicit SymbolSearchModel(const hathor::language::LanguageMetadata* metadata = nullptr);

    /**
     * Search local metadata (LanguageMetadata) for symbols matching @p query.
     * Results are added to results_.
     */
    void searchMetadata(std::string_view query);

    /**
     * Set results from an LSP workspace/symbol response.
     * Replaces any LSP-provided results but preserves metadata results.
     */
    void setLspResults(const std::vector<SymbolSearchResult>& lspResults);

    /**
     * Search workspace files for symbol definitions (file-based indexing).
     * Scans .hathor and .ck files for function-call patterns and definitions.
     */
    void searchWorkspaceFiles(const std::filesystem::path& workspaceRoot,
                              std::string_view query);

    /** Combined results from all sources. */
    const std::vector<SymbolSearchResult>& results() const noexcept { return results_; }

    /** Clear all results. */
    void clear() noexcept { results_.clear(); lspResultsValid_ = false; }

    /** Returns true if LSP results have been set. */
    bool hasLspResults() const noexcept { return lspResultsValid_; }

private:
    const hathor::language::LanguageMetadata* metadata_;
    std::vector<SymbolSearchResult> results_;
    std::vector<SymbolSearchResult> metadataResults_;
    std::vector<SymbolSearchResult> lspResults_;
    bool lspResultsValid_ = false;

    std::vector<SymbolSearchResult> fileResults_;
};

} // namespace hathor::ui
