// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * GhostCompletionTelemetry.cpp — JUCE-free implementation of ghost completion
 * lifecycle event capture and per-language quality reporting (J-6).
 *
 * All logic is pure C++20 with no JUCE dependencies — runs on the message
 * thread only, never on the audio thread.
 */

#include "GhostCompletionTelemetry.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// Recording events
// ---------------------------------------------------------------------------

void GhostCompletionTelemetry::recordEvent(const TelemetryEvent& event)
{
    events_.push_back(event);
}

void GhostCompletionTelemetry::recordDisplayed(
    const std::string& languageId,
    const std::string& requestId,
    int64_t timestampMs,
    int revision)
{
    requestStartTimes_[requestId] = timestampMs;

    TelemetryEvent evt;
    evt.type = GhostEventType::Displayed;
    evt.languageId = languageId;
    evt.timestampMs = timestampMs;
    evt.revision = revision;
    evt.requestId = requestId;
    events_.push_back(std::move(evt));
}

void GhostCompletionTelemetry::recordAccepted(const std::string& requestId,
                                              int64_t timestampMs,
                                              int revision)
{
    // Look up the DISPLAYED time for this request to compute time-to-accept.
    auto it = requestStartTimes_.find(requestId);
    if (it != requestStartTimes_.end())
        requestStartTimes_.erase(it);

    // Find the languageId from the original DISPLAYED event for this request.
    std::string languageId = "hathor"; // fallback
    bool isStale = false;
    for (auto rit = events_.rbegin(); rit != events_.rend(); ++rit)
    {
        if (rit->requestId == requestId && rit->type == GhostEventType::Displayed)
        {
            languageId = rit->languageId;
            isStale = rit->isStale;
            break;
        }
    }

    TelemetryEvent evt;
    evt.type = GhostEventType::Accepted;
    evt.languageId = languageId;
    evt.timestampMs = timestampMs;
    evt.revision = revision;
    evt.requestId = requestId;
    evt.isStale = isStale;
    events_.push_back(std::move(evt));
}

void GhostCompletionTelemetry::recordPartiallyAccepted(const std::string& requestId,
                                                       int64_t timestampMs,
                                                       int revision,
                                                       size_t acceptLength)
{
    auto it = requestStartTimes_.find(requestId);
    if (it != requestStartTimes_.end())
        requestStartTimes_.erase(it);

    std::string languageId = "hathor";
    bool isStale = false;
    for (auto rit = events_.rbegin(); rit != events_.rend(); ++rit)
    {
        if (rit->requestId == requestId && rit->type == GhostEventType::Displayed)
        {
            languageId = rit->languageId;
            isStale = rit->isStale;
            break;
        }
    }

    TelemetryEvent evt;
    evt.type = GhostEventType::PartiallyAccepted;
    evt.languageId = languageId;
    evt.timestampMs = timestampMs;
    evt.revision = revision;
    evt.requestId = requestId;
    evt.acceptLength = static_cast<int>(acceptLength);
    evt.isStale = isStale;
    events_.push_back(std::move(evt));
}

void GhostCompletionTelemetry::recordRejected(const std::string& requestId,
                                              int64_t timestampMs,
                                              int revision)
{
    auto it = requestStartTimes_.find(requestId);
    if (it != requestStartTimes_.end())
        requestStartTimes_.erase(it);

    std::string languageId = "hathor";
    bool isStale = false;
    for (auto rit = events_.rbegin(); rit != events_.rend(); ++rit)
    {
        if (rit->requestId == requestId && rit->type == GhostEventType::Displayed)
        {
            languageId = rit->languageId;
            isStale = rit->isStale;
            break;
        }
    }

    TelemetryEvent evt;
    evt.type = GhostEventType::Rejected;
    evt.languageId = languageId;
    evt.timestampMs = timestampMs;
    evt.revision = revision;
    evt.requestId = requestId;
    evt.isStale = isStale;
    events_.push_back(std::move(evt));
}

void GhostCompletionTelemetry::recordStaleRejected(const std::string& requestId,
                                                   int64_t timestampMs,
                                                   int revision)
{
    auto it = requestStartTimes_.find(requestId);
    if (it != requestStartTimes_.end())
        requestStartTimes_.erase(it);

    TelemetryEvent evt;
    evt.type = GhostEventType::StaleRejected;
    evt.languageId = "hathor"; // stale events are excluded anyway
    evt.timestampMs = timestampMs;
    evt.revision = revision;
    evt.requestId = requestId;
    evt.isStale = true;
    events_.push_back(std::move(evt));
}

void GhostCompletionTelemetry::recordCompileResult(const std::string& requestId,
                                                   int64_t timestampMs,
                                                   bool compileSuccess)
{
    // Find the languageId + staleness from the originating DISPLAYED event.
    std::string languageId = "hathor";
    bool isStale = false;
    for (auto rit = events_.rbegin(); rit != events_.rend(); ++rit)
    {
        if (rit->requestId == requestId && rit->type == GhostEventType::Displayed)
        {
            languageId = rit->languageId;
            isStale = rit->isStale;
            break;
        }
    }

    TelemetryEvent evt;
    evt.type = GhostEventType::CompileResult;
    evt.languageId = languageId;
    evt.timestampMs = timestampMs;
    evt.requestId = requestId;
    evt.compileSuccess = compileSuccess;
    evt.isStale = isStale;
    events_.push_back(std::move(evt));
}

void GhostCompletionTelemetry::recordDiagnosticAdded(const std::string& requestId,
                                                     int64_t timestampMs,
                                                     int diagnosticCount)
{
    std::string languageId = "hathor";
    bool isStale = false;
    for (auto rit = events_.rbegin(); rit != events_.rend(); ++rit)
    {
        if (rit->requestId == requestId && rit->type == GhostEventType::Displayed)
        {
            languageId = rit->languageId;
            isStale = rit->isStale;
            break;
        }
    }

    TelemetryEvent evt;
    evt.type = GhostEventType::DiagnosticAdded;
    evt.languageId = languageId;
    evt.timestampMs = timestampMs;
    evt.requestId = requestId;
    evt.diagnosticCount = diagnosticCount;
    evt.isStale = isStale;
    events_.push_back(std::move(evt));
}

void GhostCompletionTelemetry::recordImmediateDeletion(const std::string& requestId,
                                                       int64_t timestampMs)
{
    std::string languageId = "hathor";
    bool isStale = false;
    for (auto rit = events_.rbegin(); rit != events_.rend(); ++rit)
    {
        if (rit->requestId == requestId && rit->type == GhostEventType::Displayed)
        {
            languageId = rit->languageId;
            isStale = rit->isStale;
            break;
        }
    }

    TelemetryEvent evt;
    evt.type = GhostEventType::ImmediateDeletion;
    evt.languageId = languageId;
    evt.timestampMs = timestampMs;
    evt.requestId = requestId;
    evt.isStale = isStale;
    events_.push_back(std::move(evt));
}

void GhostCompletionTelemetry::recordHeavyModification(const std::string& requestId,
                                                       int64_t timestampMs)
{
    std::string languageId = "hathor";
    bool isStale = false;
    for (auto rit = events_.rbegin(); rit != events_.rend(); ++rit)
    {
        if (rit->requestId == requestId && rit->type == GhostEventType::Displayed)
        {
            languageId = rit->languageId;
            isStale = rit->isStale;
            break;
        }
    }

    TelemetryEvent evt;
    evt.type = GhostEventType::HeavyModification;
    evt.languageId = languageId;
    evt.timestampMs = timestampMs;
    evt.requestId = requestId;
    evt.isStale = isStale;
    events_.push_back(std::move(evt));
}

// ---------------------------------------------------------------------------
// Querying events
// ---------------------------------------------------------------------------

std::vector<TelemetryEvent> GhostCompletionTelemetry::eventsForLanguage(const std::string& languageId) const
{
    std::vector<TelemetryEvent> result;
    for (const auto& evt : events_)
    {
        if (evt.languageId == languageId)
            result.push_back(evt);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Metrics computation
// ---------------------------------------------------------------------------

/**
 * Internal helper: find the DISPLAYED timestamp for a requestId.
 * Returns true + timestamp if found, false otherwise.
 */
static bool findDisplayTime(const std::vector<TelemetryEvent>& events,
                            const std::string& requestId,
                            int64_t& outTime)
{
    for (const auto& evt : events)
    {
        if (evt.requestId == requestId && evt.type == GhostEventType::Displayed && !evt.isStale)
        {
            outTime = evt.timestampMs;
            return true;
        }
    }
    return false;
}

GhostMetrics GhostCompletionTelemetry::computeMetricsForLanguage(const std::string& languageId) const
{
    GhostMetrics metrics;
    metrics.languageId = languageId;

    int totalTimeToAccept = 0;   // accumulates DISPLAYED→ACCEPTED latencies (ms)
    int timeToAcceptCount = 0;   // how many accepts had a matching display event

    for (const auto& evt : events_)
    {
        // Skip stale events — they are excluded from quality statistics.
        if (evt.isStale)
            continue;

        if (evt.languageId != languageId)
            continue;

        switch (evt.type)
        {
            case GhostEventType::Displayed:
                ++metrics.totalDisplayed;
                break;

            case GhostEventType::Accepted:
                ++metrics.acceptedCount;
                {
                    int64_t displayTime = 0;
                    if (findDisplayTime(events_, evt.requestId, displayTime))
                    {
                        int64_t tta = evt.timestampMs - displayTime;
                        if (tta > 0)
                        {
                            totalTimeToAccept += static_cast<int>(tta);
                            ++timeToAcceptCount;
                        }
                    }
                }
                break;

            case GhostEventType::PartiallyAccepted:
                ++metrics.partialAcceptCount;
                {
                    int64_t displayTime = 0;
                    if (findDisplayTime(events_, evt.requestId, displayTime))
                    {
                        int64_t tta = evt.timestampMs - displayTime;
                        if (tta > 0)
                        {
                            totalTimeToAccept += static_cast<int>(tta);
                            ++timeToAcceptCount;
                        }
                    }
                }
                break;

            case GhostEventType::Rejected:
                ++metrics.rejectedCount;
                break;

            case GhostEventType::CompileResult:
                ++metrics.compileAttempts;
                if (evt.compileSuccess)
                    ++metrics.compileSuccesses;
                break;

            case GhostEventType::DiagnosticAdded:
                ++metrics.diagnosticAddedCount;
                break;

            case GhostEventType::ImmediateDeletion:
                ++metrics.immediateDeletionCount;
                break;

            case GhostEventType::HeavyModification:
                ++metrics.heavyModCount;
                break;

            case GhostEventType::StaleRejected:
                // Already excluded by isStale check above.
                break;
        }
    }

    // Derived rates (guard against division by zero)
    int acceptedOrPartial = metrics.acceptedCount + metrics.partialAcceptCount;
    if (metrics.totalDisplayed > 0)
        metrics.acceptanceRate = static_cast<double>(acceptedOrPartial) / metrics.totalDisplayed;

    if (metrics.compileAttempts > 0)
        metrics.compileSuccessRate = static_cast<double>(metrics.compileSuccesses) / metrics.compileAttempts;

    if (acceptedOrPartial > 0)
    {
        metrics.diagnosticRate = static_cast<double>(metrics.diagnosticAddedCount) / acceptedOrPartial;
        metrics.immediateDeletionRate = static_cast<double>(metrics.immediateDeletionCount) / acceptedOrPartial;
        metrics.heavyModificationRate = static_cast<double>(metrics.heavyModCount) / acceptedOrPartial;
    }

    if (timeToAcceptCount > 0)
        metrics.meanTimeToAcceptMs = static_cast<double>(totalTimeToAccept) / timeToAcceptCount;

    return metrics;
}

std::vector<GhostMetrics> GhostCompletionTelemetry::computeMetrics() const
{
    // Collect distinct languageIds from non-stale Displayed events.
    std::vector<std::string> languages;
    for (const auto& evt : events_)
    {
        if (evt.type == GhostEventType::Displayed && !evt.isStale)
        {
            if (std::find(languages.begin(), languages.end(), evt.languageId) == languages.end())
                languages.push_back(evt.languageId);
        }
    }

    std::vector<GhostMetrics> result;
    for (const auto& lang : languages)
        result.push_back(computeMetricsForLanguage(lang));

    return result;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

std::string GhostCompletionTelemetry::truncateLabel(std::string_view text, size_t maxLen)
{
    if (text.size() <= maxLen)
        return std::string(text);
    return std::string(text.substr(0, maxLen));
}

std::string GhostCompletionTelemetry::generateReport() const
{
    auto allMetrics = computeMetrics();

    std::ostringstream report;
    report << "=== Ghost Completion Quality Report ===\n";

    if (allMetrics.empty())
    {
        report << "(no events recorded)\n";
        return report.str();
    }

    for (const auto& m : allMetrics)
    {
        report << "\nLanguage: " << m.languageId << "\n";
        report << "  Displayed:           " << m.totalDisplayed << "\n";
        report << "  Accepted:             " << m.acceptedCount << "\n";
        report << "  Partially Accepted:   " << m.partialAcceptCount << "\n";
        report << "  Rejected:             " << m.rejectedCount << "\n";
        report << "  Acceptance Rate:      " << static_cast<int>(m.acceptanceRate * 100) << "%\n";
        report << "  Mean Time-to-Accept:  " << static_cast<int>(m.meanTimeToAcceptMs) << " ms\n";
        report << "  Compile Attempts:     " << m.compileAttempts << "\n";
        report << "  Compile Success Rate: " << static_cast<int>(m.compileSuccessRate * 100) << "%\n";
        report << "  Diagnostics Added:    " << m.diagnosticAddedCount << "\n";
        report << "  Diagnostic Rate:      " << static_cast<int>(m.diagnosticRate * 100) << "%\n";
        report << "  Immediate Deletions:  " << m.immediateDeletionCount << "\n";
        report << "  Immediate Deletion Rate: " << static_cast<int>(m.immediateDeletionRate * 100) << "%\n";
        report << "  Heavy Modifications:  " << m.heavyModCount << "\n";
        report << "  Heavy Modification Rate: " << static_cast<int>(m.heavyModificationRate * 100) << "%\n";
    }

    report << "\n=== End of Report ===\n";
    return report.str();
}

} // namespace hathor::lsp
