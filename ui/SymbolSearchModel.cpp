// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * SymbolSearchModel.cpp — implementation of symbol search model.
 *
 * JUCE-free, unit-testable in hathor-ui-tests.
 *
 * Requirement references: L-2 §3
 */

#include "SymbolSearchModel.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace hathor::ui {

SymbolSearchModel::SymbolSearchModel(const hathor::language::LanguageMetadata* metadata)
    : metadata_(metadata)
{
}

static std::string toLowerStr(std::string_view s)
{
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

void SymbolSearchModel::searchMetadata(std::string_view query)
{
    metadataResults_.clear();
    if (!metadata_ || query.empty())
        return;

    std::string lowerQuery = toLowerStr(query);

    const auto& functions = metadata_->functions();
    for (const auto& fn : functions)
    {
        std::string lowerName = toLowerStr(fn.name);
        if (lowerName.find(lowerQuery) != std::string::npos)
        {
            SymbolSearchResult result;
            result.name = fn.name;
            result.kind = "function";
            result.detail = fn.signature.value_or("");
            result.containerName = fn.category.value_or("");
            result.isBuiltin = true;
            result.uri = "hathor://builtin/strudel";
            result.line = 0;
            result.column = 0;
            metadataResults_.push_back(std::move(result));
        }
    }

    const auto& samples = metadata_->samples();
    for (const auto& sample : samples)
    {
        std::string lowerName = toLowerStr(sample);
        if (lowerName.find(lowerQuery) != std::string::npos)
        {
            SymbolSearchResult result;
            result.name = sample;
            result.kind = "sample";
            result.detail = "sample";
            result.isBuiltin = true;
            result.uri = "hathor://builtin/samples";
            result.line = 0;
            result.column = 0;
            metadataResults_.push_back(std::move(result));
        }
    }

    const auto& scales = metadata_->scales();
    for (const auto& scale : scales)
    {
        std::string lowerName = toLowerStr(scale);
        if (lowerName.find(lowerQuery) != std::string::npos)
        {
            SymbolSearchResult result;
            result.name = scale;
            result.kind = "scale";
            result.detail = "scale";
            result.isBuiltin = true;
            result.uri = "hathor://builtin/scales";
            result.line = 0;
            result.column = 0;
            metadataResults_.push_back(std::move(result));
        }
    }
}

void SymbolSearchModel::setLspResults(const std::vector<SymbolSearchResult>& lspResults)
{
    lspResults_ = lspResults;
    lspResultsValid_ = !lspResults_.empty();
    results_.clear();
    results_.insert(results_.end(), metadataResults_.begin(), metadataResults_.end());
    results_.insert(results_.end(), fileResults_.begin(), fileResults_.end());
    results_.insert(results_.end(), lspResults_.begin(), lspResults_.end());
}

void SymbolSearchModel::searchWorkspaceFiles(const std::filesystem::path& workspaceRoot,
                                              std::string_view query)
{
    fileResults_.clear();
    if (query.empty() || !std::filesystem::exists(workspaceRoot))
        return;

    std::string lowerQuery = toLowerStr(query);

    for (const auto& entry : std::filesystem::recursive_directory_iterator(workspaceRoot))
    {
        if (!entry.is_regular_file())
            continue;

        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (ext != ".hathor" && ext != ".ck")
            continue;

        const auto& filePath = entry.path();

        // For .hathor files, scan for function call patterns
        if (ext == ".hathor")
        {
            std::ifstream file(filePath);
            if (!file.is_open())
                continue;

            std::string line;
            int lineNum = 0;
            while (std::getline(file, line))
            {
                lineNum++;
                // Look for pattern: functionName( at start of line
                size_t pos = 0;
                while (pos < line.size())
                {
                    size_t parenIdx = line.find('(', pos);
                    if (parenIdx == std::string::npos)
                        break;

                    // Extract word before (
                    size_t wordStart = parenIdx;
                    while (wordStart > 0 && (std::isalnum(static_cast<unsigned char>(line[wordStart - 1])) ||
                                           line[wordStart - 1] == '_' || line[wordStart - 1] == '$'))
                    {
                        wordStart--;
                    }

                    if (wordStart < parenIdx)
                    {
                        std::string funcName = line.substr(wordStart, parenIdx - wordStart);
                        std::string lowerName = toLowerStr(funcName);

                        if (lowerName.find(lowerQuery) != std::string::npos)
                        {
                            SymbolSearchResult result;
                            result.name = funcName;
                            result.kind = "function";
                            result.detail = "function call";
                            result.containerName = filePath.filename().string();
                            result.filePath = filePath;
                            result.line = lineNum - 1;  // 0-based
                            result.column = static_cast<int>(wordStart);
                            result.isBuiltin = false;

                            std::string uri = "file://";
                            std::string pathStr = filePath.string();
                            uri += pathStr;
                            result.uri = std::move(uri);

                            fileResults_.push_back(std::move(result));
                        }
                    }

                    pos = parenIdx + 1;
                }
            }
        }
    }

    // Rebuild combined results
    results_.clear();
    results_.insert(results_.end(), metadataResults_.begin(), metadataResults_.end());
    results_.insert(results_.end(), fileResults_.begin(), fileResults_.end());
    if (lspResultsValid_)
        results_.insert(results_.end(), lspResults_.begin(), lspResults_.end());
}

} // namespace hathor::ui
