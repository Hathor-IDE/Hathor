// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * FindReplaceModel.hpp — JUCE-free find/replace engine for the editor.
 *
 * Operates purely on std::string (document content) and integer offsets.
 * The JUCE UI layer (FindReplacePanel) wraps this model and maps results
 * to CodeDocument/CodeEditorComponent APIs.
 *
 * Supports:
 *   - Plain-text and regex search (std::regex).
 *   - Case-sensitive / case-insensitive.
 *   - Whole-word matching.
 *   - "Loop" / wrap-around.
 *   - Cursor-preserving replace (single and replace-all).
 *
 * Requirement references: L-1 §4 (find/replace, regex)
 */

#include <cstddef>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace hathor::ui {

enum class FindFlags : uint8_t
{
    None          = 0,
    CaseSensitive = 1,
    WholeWord     = 2,
    UseRegex      = 4,
    WrapAround    = 8,
    Backwards     = 16,
};

inline FindFlags operator|(FindFlags a, FindFlags b) noexcept
{
    return static_cast<FindFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline FindFlags operator&(FindFlags a, FindFlags b) noexcept
{
    return static_cast<FindFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool hasFlag(FindFlags v, FindFlags f) noexcept
{
    return (static_cast<uint8_t>(v) & static_cast<uint8_t>(f)) != 0;
}

/// A match found by the search engine: [start, end) in document offsets.
struct FindMatch
{
    size_t start;
    size_t end;
};

/**
 * FindReplaceModel
 *
 * Holds search parameters and performs search / replace operations on a
 * std::string document snapshot.  Does NOT own the document — callers pass
 * the current text in and receive match offsets back.
 *
 * The model is value-semantic and trivially copyable per instance, making
 * it suitable for use in tests without JUCE.
 */
class FindReplaceModel
{
public:
    void setFlags(FindFlags f) noexcept { flags_ = f; }
    FindFlags flags() const noexcept { return flags_; }

    void setSearchText(std::string s) noexcept { search_ = std::move(s); }
    const std::string& searchText() const noexcept { return search_; }

    void setReplaceText(std::string s) noexcept { replace_ = std::move(s); }
    const std::string& replaceText() const noexcept { return replace_; }

    /**
     * Compile the search pattern.  Call before searching when the search
     * text or flags have changed.  Returns false on regex syntax error.
     */
    bool compilePattern();

    /**
     * Find the next match after `fromOffset` (or at if it matches).
     * Returns the match, or nullopt if no match found.
     * Respects WrapAround / Backwards flags.
     */
    std::optional<FindMatch> findNext(
        const std::string& doc,
        size_t fromOffset) const;

    /**
     * Find the previous match before `fromOffset`.
     */
    std::optional<FindMatch> findPrev(
        const std::string& doc,
        size_t fromOffset) const;

    /**
     * Find all non-overlapping matches in the document.
     */
    std::vector<FindMatch> findAll(const std::string& doc) const;

    /**
     * Replace a single match at `match` with `replaceText()`.
     * Returns the new document length delta (replaceLen - matchedLen).
     * The caller updates cursor offset accordingly.
     */
    size_t replaceOne(std::string& doc, const FindMatch& match) const;

    /**
     * Replace all matches in the document.
     * Returns the number of replacements made.
     */
    size_t replaceAll(std::string& doc);

private:
    FindFlags   flags_            = FindFlags::None;
    std::string search_;
    std::string replace_;
    std::regex  regex_;
};

} // namespace hathor::ui
