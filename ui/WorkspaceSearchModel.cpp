// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * WorkspaceSearchModel.cpp — implementation of multi-file text search.
 *
 * JUCE-free, unit-testable in hathor-ui-tests.
 *
 * Requirement references: L-2 §1
 */

#include "WorkspaceSearchModel.hpp"

#include "FindReplaceModel.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>

namespace hathor::ui {

WorkspaceSearchModel::WorkspaceSearchModel(std::filesystem::path workspaceRoot)
    : workspaceRoot_(std::move(workspaceRoot))
{
}

std::vector<std::string> WorkspaceSearchModel::supportedExtensions()
{
    return {".hathor", ".ck", ".txt", ".md"};
}

static std::string readFileToString(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file.is_open())
        return {};

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    return content;
}

static bool extensionSupported(const std::filesystem::path& path)
{
    static const auto exts = WorkspaceSearchModel::supportedExtensions();
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

static bool isWordBoundary(char c)
{
    return !std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '$';
}

static bool wholeWordMatch(const std::string& line, size_t start, size_t len)
{
    if (start > 0 && !isWordBoundary(line[start - 1]))
        return false;
    if (start + len < line.size() && !isWordBoundary(line[start + len]))
        return false;
    return true;
}

int WorkspaceSearchModel::search(std::string_view query,
                                  const WorkspaceSearchFlags& flags,
                                  int maxResults)
{
    clear();

    if (query.empty() || !std::filesystem::exists(workspaceRoot_))
        return 0;

    std::regex patternRegex;
    if (flags.useRegex)
    {
        try
        {
            patternRegex.assign(query.begin(), query.end(),
                               std::regex::ECMAScript |
                                   (flags.caseSensitive ? std::regex_constants::mask_none
                                                        : std::regex_constants::icase));
        }
        catch (const std::regex_error&)
        {
            return 0;
        }
    }

    std::string queryString(query);

    for (const auto& entry : std::filesystem::recursive_directory_iterator(workspaceRoot_))
    {
        if (!entry.is_regular_file())
            continue;

        if (!extensionSupported(entry.path()))
            continue;

        const auto& filePath = entry.path();
        std::string content = readFileToString(filePath);
        if (content.empty())
            continue;

        WorkspaceFileResult fileResult;
        fileResult.filePath = filePath;

        if (flags.useRegex)
        {
            std::sregex_iterator it(content.begin(), content.end(), patternRegex);
            std::sregex_iterator end;

            for (; it != end && static_cast<int>(fileResult.matches.size()) < 100; ++it)
            {
                const auto& match = *it;
                size_t pos = match.position();
                size_t matchLen = match.length();

                // Compute line and column
                int lineNum = 1;
                int col = 1;
                for (size_t i = 0; i < pos; ++i)
                {
                    if (content[i] == '\n')
                    {
                        lineNum++;
                        col = 1;
                    }
                    else
                    {
                        col++;
                    }
                }

                // Extract line text
                size_t lineStart = content.rfind('\n', pos);
                if (lineStart == std::string::npos)
                    lineStart = 0;
                else
                    lineStart++;

                size_t lineEnd = content.find('\n', pos);
                if (lineEnd == std::string::npos)
                    lineEnd = content.size();

                std::string lineText = content.substr(lineStart, lineEnd - lineStart);
                // Truncate very long lines for display
                if (lineText.size() > 200)
                    lineText = lineText.substr(0, 200) + "...";

                WorkspaceSearchMatch m;
                m.filePath = filePath;
                m.line = lineNum;
                m.column = col;
                m.matchStart = static_cast<int>(pos - lineStart);
                m.matchLength = static_cast<int>(matchLen);
                m.lineText = lineText;
                m.matchText = content.substr(pos, matchLen);

                // Whole word check
                if (flags.wholeWord && !wholeWordMatch(lineText, m.matchStart, m.matchLength))
                    continue;

                fileResult.matches.push_back(std::move(m));
            }
        }
        else
        {
            // Plain text search
            std::string haystack = content;
            std::string needle = queryString;

            if (!flags.caseSensitive)
            {
                std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                std::transform(needle.begin(), needle.end(), needle.begin(),
                               [](unsigned char c) { return std::tolower(c); });
            }

            size_t pos = 0;
            while ((pos = haystack.find(needle, pos)) != std::string::npos)
            {
                int lineNum = 1;
                int col = 1;
                for (size_t i = 0; i < pos; ++i)
                {
                    if (content[i] == '\n')
                    {
                        lineNum++;
                        col = 1;
                    }
                    else
                    {
                        col++;
                    }
                }

                size_t lineStart = content.rfind('\n', pos);
                if (lineStart == std::string::npos)
                    lineStart = 0;
                else
                    lineStart++;

                size_t lineEnd = content.find('\n', pos);
                if (lineEnd == std::string::npos)
                    lineEnd = content.size();

                std::string lineText = content.substr(lineStart, lineEnd - lineStart);
                if (lineText.size() > 200)
                    lineText = lineText.substr(0, 200) + "...";

                WorkspaceSearchMatch m;
                m.filePath = filePath;
                m.line = lineNum;
                m.column = col;
                m.matchStart = static_cast<int>(pos - lineStart);
                m.matchLength = static_cast<int>(needle.size());
                m.lineText = lineText;
                m.matchText = content.substr(pos, needle.size());

                if (flags.wholeWord && !wholeWordMatch(lineText, m.matchStart, m.matchLength))
                {
                    pos += needle.size();
                    continue;
                }

                fileResult.matches.push_back(std::move(m));
                pos += needle.size();

                if (static_cast<int>(fileResult.matches.size()) >= 100)
                    break;
            }
        }

        if (!fileResult.matches.empty())
        {
            results_.push_back(std::move(fileResult));
            totalMatches_ += static_cast<int>(results_.back().matches.size());

            if (static_cast<int>(results_.size()) >= maxResults)
                break;
        }
    }

    return totalMatches_;
}

int WorkspaceSearchModel::replaceInFile(const std::filesystem::path& filePath,
                                         std::string_view query,
                                         std::string_view replacement,
                                         const WorkspaceSearchFlags& flags)
{
    std::string content = readFileToString(filePath);
    if (content.empty())
        return 0;

    std::string queryString(query);
    int count = 0;

    if (flags.useRegex)
    {
        try
        {
            std::regex patternRegex(query.begin(), query.end(),
                                   std::regex::ECMAScript |
                                       (flags.caseSensitive ? std::regex_constants::mask_none
                                                            : std::regex_constants::icase));
            content = std::regex_replace(content, patternRegex, std::string(replacement));
            count = static_cast<int>(std::distance(
                std::sregex_iterator(content.begin(), content.end(), patternRegex),
                std::sregex_iterator()));
        }
        catch (const std::regex_error&)
        {
            return 0;
        }
    }
    else
    {
        std::string haystack = content;
        std::string needle = queryString;

        if (!flags.caseSensitive)
        {
            std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            std::transform(needle.begin(), needle.end(), needle.begin(),
                           [](unsigned char c) { return std::tolower(c); });
        }

        size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos)
        {
            if (!flags.wholeWord)
            {
                count++;
                pos += needle.size();
            }
            else
            {
                // Check whole word boundary
                bool beforeOk = (pos == 0) || isWordBoundary(content[pos - 1]);
                bool afterOk = (pos + needle.size() >= content.size()) ||
                               isWordBoundary(content[pos + needle.size()]);

                if (beforeOk && afterOk)
                    count++;

                pos += needle.size();
            }
        }

        // Actually perform the replacement on the original content
        std::string result;
        std::string lowerNeedle = queryString;
        std::string lowerContent = content;

        if (!flags.caseSensitive)
        {
            std::transform(lowerNeedle.begin(), lowerNeedle.end(), lowerNeedle.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            std::transform(lowerContent.begin(), lowerContent.end(), lowerContent.begin(),
                           [](unsigned char c) { return std::tolower(c); });
        }

        size_t lastPos = 0;
        pos = 0;
        while ((pos = lowerContent.find(lowerNeedle, pos)) != std::string::npos)
        {
            if (flags.wholeWord)
            {
                bool beforeOk = (pos == 0) || isWordBoundary(content[pos - 1]);
                bool afterOk = (pos + needle.size() >= content.size()) ||
                               isWordBoundary(content[pos + needle.size()]);
                if (!beforeOk || !afterOk)
                {
                    pos += needle.size();
                    continue;
                }
            }

            result.append(content, lastPos, pos - lastPos);
            result.append(replacement);
            pos += queryString.size();
            lastPos = pos;
        }
        result.append(content, lastPos, std::string::npos);
        content = std::move(result);
    }

    if (count > 0)
    {
        std::ofstream out(filePath);
        if (out.is_open())
            out << content;
    }

    return count;
}

} // namespace hathor::ui
