// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * DiagnosticRegistry.cpp — L-3: thread-safe central diagnostic store.
 *
 * Requirement references: L-3 §1 (unified diagnostic model), L-3 §2 (problems surface)
 */

#include "DiagnosticRegistry.hpp"

#include <algorithm>

namespace hathor::control {

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

void DiagnosticRegistry::setDiagnostics(DiagSource source,
                                        const std::string& uri,
                                        std::vector<Diagnostic> diags)
{
    {
        std::lock_guard<std::mutex> lock(mtx_);

        auto& entry = bySourceUri_[makeKey(source, uri)];
        entry.clear();

        for (auto& d : diags)
        {
            // Skip empties — an empty diagnostic means no problem.
            if (d.message.empty() && d.code.empty())
                continue;

            d.id      = nextId_.fetch_add(1, std::memory_order_relaxed);
            d.source  = source;
            d.sourceLabel = std::string(sourceLabel(source));
            entry.push_back(std::move(d));
        }

        countsDirty_ = true;
    }

    if (changeCb_)
        changeCb_();
}

void DiagnosticRegistry::addDiagnostic(Diagnostic diag)
{
    {
        std::lock_guard<std::mutex> lock(mtx_);

        diag.id       = nextId_.fetch_add(1, std::memory_order_relaxed);
        diag.sourceLabel = std::string(sourceLabel(diag.source));

        auto& entry = bySourceUri_[makeKey(diag.source, diag.uri)];
        entry.push_back(std::move(diag));

        countsDirty_ = true;
    }

    if (changeCb_)
        changeCb_();
}

void DiagnosticRegistry::clearDiagnostics(DiagSource source,
                                          const std::string& uri)
{
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = bySourceUri_.find(makeKey(source, uri));
        if (it != bySourceUri_.end())
        {
            bySourceUri_.erase(it);
            found = true;
            countsDirty_ = true;
        }
    }

    if (found && changeCb_)
        changeCb_();
}

void DiagnosticRegistry::clearSource(DiagSource source)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto it = bySourceUri_.begin(); it != bySourceUri_.end();)
        {
            if (it->first.source == source)
            {
                it = bySourceUri_.erase(it);
                changed = true;
            }
            else
            {
                ++it;
            }
        }
        if (changed)
            countsDirty_ = true;
    }

    if (changed && changeCb_)
        changeCb_();
}

void DiagnosticRegistry::clearAll()
{
    bool hadContent = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        hadContent = !bySourceUri_.empty();
        bySourceUri_.clear();
        countsDirty_ = true;
    }

    if (hadContent && changeCb_)
        changeCb_();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::vector<Diagnostic> DiagnosticRegistry::allDiagnostics() const
{
    std::vector<Diagnostic> result;

    {
        std::lock_guard<std::mutex> lock(mtx_);

        size_t total = 0;
        for (const auto& [key, diags] : bySourceUri_)
            total += diags.size();
        result.reserve(total);

        for (const auto& [key, diags] : bySourceUri_)
            for (const auto& d : diags)
                result.push_back(d);
    }

    // Stable sort: by URI, then line, then severity (errors first), then column
    std::sort(result.begin(), result.end(),
        [](const Diagnostic& a, const Diagnostic& b)
        {
            if (a.uri != b.uri)
                return a.uri < b.uri;
            if (a.line != b.line)
                return a.line < b.line;
            if (static_cast<int>(a.severity) != static_cast<int>(b.severity))
                return static_cast<int>(a.severity) < static_cast<int>(b.severity);
            return a.column < b.column;
        });

    return result;
}

std::vector<Diagnostic> DiagnosticRegistry::diagnosticsForUri(
    const std::string& uri) const
{
    std::vector<Diagnostic> result;

    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& [key, diags] : bySourceUri_)
    {
        if (key.uri == uri)
        {
            for (const auto& d : diags)
                result.push_back(d);
        }
    }

    std::sort(result.begin(), result.end(),
        [](const Diagnostic& a, const Diagnostic& b)
        {
            if (a.line != b.line)
                return a.line < b.line;
            return a.column < b.column;
        });

    return result;
}

std::vector<Diagnostic> DiagnosticRegistry::diagnosticsForSourceUri(
    DiagSource source,
    const std::string& uri) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = bySourceUri_.find(makeKey(source, uri));
    if (it == bySourceUri_.end())
        return {};
    return it->second;
}

std::vector<Diagnostic> DiagnosticRegistry::diagnosticsAtLine(
    const std::string& uri, int line) const
{
    std::vector<Diagnostic> result;

    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& [key, diags] : bySourceUri_)
    {
        if (key.uri != uri)
            continue;
        for (const auto& d : diags)
        {
            if (d.line == line || d.line == line + 1)
                result.push_back(d);
        }
    }

    return result;
}

std::vector<std::string> DiagnosticRegistry::urisWithDiagnostics() const
{
    std::vector<std::string> result;

    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& [key, diags] : bySourceUri_)
    {
        if (!diags.empty())
            result.push_back(key.uri);
    }

    // Deduplicate (same URI can appear under multiple sources)
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    return result;
}

// ---------------------------------------------------------------------------
// Counts
// ---------------------------------------------------------------------------

void DiagnosticRegistry::recomputeCountsLocked() const
{
    Counts c;
    for (const auto& [key, diags] : bySourceUri_)
    {
        for (const auto& d : diags)
        {
            ++c.total;
            switch (d.severity)
            {
                case DiagSeverity::Error:   ++c.errors;   break;
                case DiagSeverity::Warning: ++c.warnings; break;
                case DiagSeverity::Info:
                case DiagSeverity::Hint:     ++c.info;     break;
            }
        }
    }
    cachedCounts_ = c;
    countsDirty_ = false;
}

DiagnosticRegistry::Counts DiagnosticRegistry::counts() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (countsDirty_)
        recomputeCountsLocked();
    return cachedCounts_;
}

int DiagnosticRegistry::errorCount() const noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (countsDirty_)
        recomputeCountsLocked();
    return cachedCounts_.errors;
}

int DiagnosticRegistry::warningCount() const noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (countsDirty_)
        recomputeCountsLocked();
    return cachedCounts_.warnings;
}

int DiagnosticRegistry::infoCount() const noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (countsDirty_)
        recomputeCountsLocked();
    return cachedCounts_.info;
}

int DiagnosticRegistry::total() const noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (countsDirty_)
        recomputeCountsLocked();
    return cachedCounts_.total;
}

// ---------------------------------------------------------------------------
// Change notification
// ---------------------------------------------------------------------------

void DiagnosticRegistry::setChangeCallback(ChangeCallback cb)
{
    std::lock_guard<std::mutex> lock(mtx_);
    changeCb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// ID reset (tests)
// ---------------------------------------------------------------------------

void DiagnosticRegistry::resetIds()
{
    std::lock_guard<std::mutex> lock(mtx_);
    nextId_.store(1, std::memory_order_relaxed);
}

} // namespace hathor::control
