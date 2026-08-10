// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * LspDiagnosticsDisplay.cpp — implementation.
 *
 * Requirement references: AI-4
 */

#include "LspDiagnosticsDisplay.hpp"

namespace hathor::ui {

void LspDiagnosticsDisplay::setDiagnostics(const std::string& uri,
                                            const std::vector<lsp::Diagnostic>& diagnostics)
{
    auto& doc = docs_[uri];
    doc.all = diagnostics;
    doc.byLine.clear();

    for (const auto& d : diagnostics)
    {
        int line = d.range.start.line;
        doc.byLine[line].push_back(&d);
    }
}

void LspDiagnosticsDisplay::clearDiagnostics(const std::string& uri)
{
    docs_.erase(uri);
}

void LspDiagnosticsDisplay::clearAll()
{
    docs_.clear();
}

std::vector<lsp::Diagnostic> LspDiagnosticsDisplay::getDiagnosticsForLine(
    const std::string& uri, int line) const
{
    auto it = docs_.find(uri);
    if (it == docs_.end())
        return {};

    std::vector<lsp::Diagnostic> result;
    auto lineIt = it->second.byLine.find(line);
    if (lineIt != it->second.byLine.end())
    {
        for (const auto* d : lineIt->second)
        {
            result.push_back(*d);
        }
    }
    return result;
}

const std::vector<lsp::Diagnostic>& LspDiagnosticsDisplay::getAllDiagnostics(const std::string& uri) const
{
    static const std::vector<lsp::Diagnostic> empty;
    auto it = docs_.find(uri);
    if (it == docs_.end())
        return empty;
    return it->second.all;
}

bool LspDiagnosticsDisplay::hasErrors(const std::string& uri) const
{
    auto it = docs_.find(uri);
    if (it == docs_.end())
        return false;

    for (const auto& d : it->second.all)
    {
        if (d.severity.has_value() && d.severity.value() == lsp::DiagnosticSeverity::Error)
            return true;
    }
    return false;
}

int LspDiagnosticsDisplay::errorCount(const std::string& uri) const
{
    auto it = docs_.find(uri);
    if (it == docs_.end())
        return 0;

    int count = 0;
    for (const auto& d : it->second.all)
    {
        if (d.severity.has_value() && d.severity.value() == lsp::DiagnosticSeverity::Error)
            ++count;
    }
    return count;
}

std::string LspDiagnosticsDisplay::summary(const std::string& uri) const
{
    auto it = docs_.find(uri);
    if (it == docs_.end() || it->second.all.empty())
        return {};

    int errors = 0;
    int warnings = 0;
    for (const auto& d : it->second.all)
    {
        if (d.severity.has_value())
        {
            switch (d.severity.value())
            {
                case lsp::DiagnosticSeverity::Error:   ++errors; break;
                case lsp::DiagnosticSeverity::Warning: ++warnings; break;
                default: break;
            }
        }
    }

    std::string s;
    if (errors > 0)
    {
        s += std::to_string(errors) + " error" + (errors > 1 ? "s" : "");
        if (warnings > 0)
            s += ", ";
    }
    if (warnings > 0)
        s += std::to_string(warnings) + " warning" + (warnings > 1 ? "s" : "");

    return s;
}

} // namespace hathor::ui
