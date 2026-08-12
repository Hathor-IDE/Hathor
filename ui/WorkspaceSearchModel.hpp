// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * WorkspaceSearchModel.hpp — JUCE-free multi-file text search and replace model.
 *
 * Searches across all files in the workspace for a text or regex pattern.
 * Reuses FindReplaceModel's search flag types for consistency, but operates
 * across multiple files and aggregates results.
 *
 * The model is pure logic (no JUCE). The JUCE-dependent WorkspaceSearchPanel
 * owns this model and renders results in a Listbox.
 *
 * Requirement references: L-2 §1
 */

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "FindReplaceModel.hpp"

namespace hathor::ui {

/**
 * One match within a file.
 */
struct WorkspaceSearchMatch {
    std::filesystem::path filePath;   ///< absolute path to the matching file
    int          line         = 0;    ///< 1-based line number
    int          column       = 0;    ///< 1-based column (byte offset in line)
    int          matchStart   = 0;    ///< 0-based character offset of match in line
    int          matchLength  = 0;    ///< length of match in characters
    std::string  lineText;            ///< the full text of the matching line (trimmed)
    std::string  matchText;           ///< the matched substring
};

/**
 * One file's worth of matches.
 */
struct WorkspaceFileResult {
    std::filesystem::path filePath;
    std::vector<WorkspaceSearchMatch> matches;
};

/**
 * Search flags that mirror FindReplaceModel's flags for consistency.
 */
struct WorkspaceSearchFlags {
    bool caseSensitive     = false;
    bool useRegex          = false;
    bool wholeWord         = false;
    bool searchInComments  = true;  ///< for ChucK, skip comments if false
};

/**
 * WorkspaceSearchModel
 *
 * Performs multi-file text search across the workspace directory.
 * Results are collected in batches (by file) and can be queried incrementally.
 *
 * The search is synchronous (blocking) — for large workspaces, the JUCE
 * panel should run it on a background thread. The model itself is thread-safe
 * for reading results but the search must not be called concurrently.
 */
class WorkspaceSearchModel
{
public:
    explicit WorkspaceSearchModel(std::filesystem::path workspaceRoot);

    /**
     * Search all supported files in the workspace for @p query.
     * Populates results_ and returns total match count.
     *
     * @param query The search string or regex pattern.
     * @param flags Search options (case sensitivity, regex, whole word).
     * @param maxResults Maximum number of file-result entries to collect.
     */
    int search(std::string_view query, const WorkspaceSearchFlags& flags,
               int maxResults = 500);

    /**
     * Replace all occurrences of @p query with @p replacement in the given file.
     * Returns the number of replacements made.
     */
    int replaceInFile(const std::filesystem::path& filePath,
                      std::string_view query,
                      std::string_view replacement,
                      const WorkspaceSearchFlags& flags);

    /** Current search results. */
    const std::vector<WorkspaceFileResult>& results() const noexcept { return results_; }

    /** Total number of matches across all files. */
    int totalMatchCount() const noexcept { return totalMatches_; }

    /** The workspace root directory. */
    const std::filesystem::path& workspaceRoot() const noexcept { return workspaceRoot_; }

    /** Clear all cached results. */
    void clear() noexcept { results_.clear(); totalMatches_ = 0; }

    /** Returns the list of file extensions that are search targets. */
    static std::vector<std::string> supportedExtensions();

private:
    std::filesystem::path workspaceRoot_;
    std::vector<WorkspaceFileResult> results_;
    int totalMatches_ = 0;
};

} // namespace hathor::ui
