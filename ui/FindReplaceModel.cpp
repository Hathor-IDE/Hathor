// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * FindReplaceModel.cpp — implementation.
 *
 * Requirement references: L-1 §4
 */

#include "FindReplaceModel.hpp"

#include <algorithm>
#include <cctype>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Pattern compilation
// ---------------------------------------------------------------------------

bool FindReplaceModel::compilePattern()
{
    if (!hasFlag(flags_, FindFlags::UseRegex) || search_.empty())
    {
        // Plain text — no regex needed.
        return true;
    }

    try
    {
        std::regex::flag_type rf = std::regex::ECMAScript | std::regex::optimize;
        if (hasFlag(flags_, FindFlags::CaseSensitive))
            rf = rf;  // no icase
        else
            rf |= std::regex::icase;

        regex_ = std::regex(search_, rf);
        return true;
    }
    catch (const std::regex_error&)
    {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

bool isWordChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

/// For plain-text whole-word search, check that the match is bounded
/// by non-word characters (or document start/end) at both ends.
bool isWholeWordMatch(const std::string& doc, size_t start, size_t end)
{
    bool leftOk  = (start == 0) || !isWordChar(doc[start - 1]);
    bool rightOk = (end   == doc.size()) || !isWordChar(doc[end]);
    return leftOk && rightOk;
}

/// Case-insensitive character comparison.
char toLowerChar(char c) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// Case-insensitive substring search.  Returns offset or npos.
size_t findCaseInsensitive(const std::string& doc,
                           const std::string& pat,
                           size_t from)
{
    if (pat.empty() || from > doc.size())
        return std::string::npos;

    std::string lowerDoc;
    lowerDoc.reserve(doc.size());
    for (char c : doc) lowerDoc.push_back(toLowerChar(c));

    std::string lowerPat;
    lowerPat.reserve(pat.size());
    for (char c : pat) lowerPat.push_back(toLowerChar(c));

    return lowerDoc.find(lowerPat, from);
}

} // namespace

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

std::optional<FindMatch> FindReplaceModel::findNext(const std::string& doc,
                                                     size_t fromOffset) const
{
    if (search_.empty())
        return std::nullopt;

    size_t pos = fromOffset;
    size_t docLen = doc.size();

    if (hasFlag(flags_, FindFlags::UseRegex))
    {
        std::regex_iterator<char> it(doc.begin() + std::clamp(pos, size_t{0}, docLen),
                                     doc.end(), regex_);
        std::regex_iterator<char> end;

        if (it != end)
        {
            const auto& m = *it;
            size_t start = static_cast<size_t>(m.position());
            size_t len   = m.length();
            size_t endPos = start + len;

            if (start <= docLen && endPos <= docLen)
                return FindMatch{start, endPos};
        }

        // Wrap around if requested and no match found
        if (hasFlag(flags_, FindFlags::WrapAround) && fromOffset > 0)
        {
            pos = 0;
            std::regex_iterator<char> it2(doc.begin(), doc.end(), regex_);
            if (it2 != end)
            {
                const auto& m = *it2;
                size_t start = static_cast<size_t>(m.position());
                size_t len   = m.length();
                size_t endPos = start + len;
                if (start <= docLen && endPos <= docLen)
                    return FindMatch{start, endPos};
            }
        }
        return std::nullopt;
    }

    // Plain text search
    bool caseSensitive = hasFlag(flags_, FindFlags::CaseSensitive);
    bool wholeWord     = hasFlag(flags_, FindFlags::WholeWord);

    auto findPlain = [&](size_t from) -> size_t {
        if (caseSensitive)
            return doc.find(search_, from);
        else
            return findCaseInsensitive(doc, search_, from);
    };

    // Clamp starting position
    if (pos > docLen) pos = docLen;

    size_t found = findPlain(pos);
    if (found == std::string::npos)
    {
        if (hasFlag(flags_, FindFlags::WrapAround) && pos > 0)
        {
            found = findPlain(0);
            if (found == std::string::npos)
                return std::nullopt;
        }
        else
        {
            return std::nullopt;
        }
    }

    size_t end = found + search_.size();

    // For whole-word, skip matches that don't fit the criteria
    if (wholeWord)
    {
        // Keep searching from found+1
        while (!isWholeWordMatch(doc, found, end))
        {
            size_t next = findPlain(found + 1);
            if (next == std::string::npos)
            {
                if (hasFlag(flags_, FindFlags::WrapAround) && found > 0)
                {
                    next = findPlain(0);
                    if (next == std::string::npos || next >= found)
                        return std::nullopt;
                }
                else
                {
                    return std::nullopt;
                }
            }
            found = next;
            end = found + search_.size();
            if (found >= docLen || end > docLen)
                return std::nullopt;
        }
    }

    return FindMatch{found, end};
}

std::optional<FindMatch> FindReplaceModel::findPrev(const std::string& doc,
                                                     size_t fromOffset) const
{
    // Simple reverse: search forward from 0 and pick the last match before fromOffset.
    // For regex, we'd need reverse iteration — but std::regex doesn't support that.
    // For now, do a linear scan forward collecting matches and pick the last one
    // that starts before fromOffset.

    if (search_.empty())
        return std::nullopt;

    auto allMatches = findAll(doc);
    if (allMatches.empty())
        return std::nullopt;

    // Pick the last match whose start < fromOffset (or <= if fromOffset is past end)
    size_t clampedFrom = std::min(fromOffset, doc.size());

    std::optional<FindMatch> best;
    for (const auto& m : allMatches)
    {
        if (m.start < clampedFrom)
            best = m;
        else
            break;
    }

    if (!best.has_value())
    {
        // Wrap around
        if (hasFlag(flags_, FindFlags::WrapAround))
            return allMatches.back();
        return std::nullopt;
    }

    return best;
}

std::vector<FindMatch> FindReplaceModel::findAll(const std::string& doc) const
{
    std::vector<FindMatch> results;
    if (search_.empty())
        return results;

    if (hasFlag(flags_, FindFlags::UseRegex))
    {
        std::regex_iterator<char> it(doc.begin(), doc.end(), regex_);
        std::regex_iterator<char> end;

        while (it != end)
        {
            const auto& m = *it;
            size_t start = static_cast<size_t>(m.position());
            size_t len   = m.length();
            size_t endPos = start + len;

            if (start <= doc.size() && endPos <= doc.size())
            {
                bool wholeOk = true;
                if (hasFlag(flags_, FindFlags::WholeWord))
                    wholeOk = isWholeWordMatch(doc, start, endPos);
                if (wholeOk || !hasFlag(flags_, FindFlags::WholeWord))
                    results.push_back({start, endPos});
            }
            ++it;
        }
    }
    else
    {
        bool caseSensitive = hasFlag(flags_, FindFlags::CaseSensitive);
        bool wholeWord     = hasFlag(flags_, FindFlags::WholeWord);

        auto findPlain = [&](size_t from) -> size_t {
            if (caseSensitive)
                return doc.find(search_, from);
            else
                return findCaseInsensitive(doc, search_, from);
        };

        size_t pos = 0;
        while (pos <= doc.size())
        {
            size_t found = findPlain(pos);
            if (found == std::string::npos)
                break;
            size_t end = found + search_.size();
            if (end > doc.size())
                break;

            if (!wholeWord || isWholeWordMatch(doc, found, end))
                results.push_back({found, end});

            pos = end > found ? end : found + 1;
        }
    }

    return results;
}

// ---------------------------------------------------------------------------
// Replace
// ---------------------------------------------------------------------------

size_t FindReplaceModel::replaceOne(std::string& doc, const FindMatch& match) const
{
    if (match.start > doc.size() || match.end > doc.size() || match.start >= match.end)
        return 0;

    size_t matchLen = match.end - match.start;
    size_t replLen  = replace_.size();

    doc.replace(match.start, matchLen, replace_);

    // Return delta: positive if replacement is longer.
    if (replLen > matchLen)
        return replLen - matchLen;
    return 0;
}

size_t FindReplaceModel::replaceAll(std::string& doc)
{
    size_t count = 0;
    size_t offset = 0;

    while (offset <= doc.size())
    {
        auto match = findNext(doc, offset);
        if (!match.has_value())
            break;

        // Guard against zero-length regex matches that could loop forever.
        if (match->end == match->start && hasFlag(flags_, FindFlags::UseRegex))
        {
            if (offset >= doc.size())
                break;
            ++offset;
            continue;
        }

        // Clamp replacement to the match range
        std::string_view repl(replace_);
        if (match->end - match->start < repl.size())
            repl = std::string_view(repl.data(), match->end - match->start);

        size_t beforeLen = doc.size();
        doc.replace(match->start, match->end - match->start, std::string(repl));
        size_t afterLen = doc.size();

        offset = match->start + repl.size();
        if (doc.size() == beforeLen && repl.empty())
            ++offset;  // guard against infinite loop on empty replacement

        (void)afterLen;
        ++count;
    }

    return count;
}

} // namespace hathor::ui
