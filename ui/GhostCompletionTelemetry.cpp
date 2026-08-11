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

#ifdef HATHOR_ENABLE_GHOST_TELEMETRY
#include "GhostCompletionTelemetry.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <fstream>
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

// ---------------------------------------------------------------------------
// Persistence (serialization for disk storage)
// ---------------------------------------------------------------------------

/** Helper: serialize a GhostEventType to its string name. */
static const char* eventTypeToString(GhostEventType type)
{
    switch (type)
    {
        case GhostEventType::Displayed:            return "displayed";
        case GhostEventType::Accepted:             return "accepted";
        case GhostEventType::PartiallyAccepted:    return "partially_accepted";
        case GhostEventType::Rejected:             return "rejected";
        case GhostEventType::StaleRejected:        return "stale_rejected";
        case GhostEventType::CompileResult:        return "compile_result";
        case GhostEventType::DiagnosticAdded:      return "diagnostic_added";
        case GhostEventType::ImmediateDeletion:    return "immediate_deletion";
        case GhostEventType::HeavyModification:    return "heavy_modification";
    }
    return "unknown";
}

/** Helper: parse a GhostEventType from its string name. */
static GhostEventType stringToEventType(const std::string& s)
{
    if (s == "displayed")            return GhostEventType::Displayed;
    if (s == "accepted")             return GhostEventType::Accepted;
    if (s == "partially_accepted")   return GhostEventType::PartiallyAccepted;
    if (s == "rejected")             return GhostEventType::Rejected;
    if (s == "stale_rejected")       return GhostEventType::StaleRejected;
    if (s == "compile_result")       return GhostEventType::CompileResult;
    if (s == "diagnostic_added")     return GhostEventType::DiagnosticAdded;
    if (s == "immediate_deletion")   return GhostEventType::ImmediateDeletion;
    if (s == "heavy_modification")   return GhostEventType::HeavyModification;
    return GhostEventType::Displayed;
}

std::string GhostCompletionTelemetry::toJson() const
{
    nlohmann::json j;
    j["events"] = nlohmann::json::array();

    for (const auto& evt : events_)
    {
        nlohmann::json e;
        e["type"] = eventTypeToString(evt.type);
        e["languageId"] = evt.languageId;
        e["timestampMs"] = evt.timestampMs;
        e["revision"] = evt.revision;
        e["requestId"] = evt.requestId;
        e["acceptLength"] = evt.acceptLength;
        e["compileSuccess"] = evt.compileSuccess;
        e["diagnosticCount"] = evt.diagnosticCount;
        e["isStale"] = evt.isStale;
        e["ghostLabel"] = evt.ghostLabel;
        j["events"].push_back(e);
    }

    return j.dump();
}

void GhostCompletionTelemetry::loadFromJson(const std::string& jsonStr)
{
    clear();

    try
    {
        auto j = nlohmann::json::parse(jsonStr);

        if (!j.is_object() || !j.contains("events") || !j["events"].is_array())
            return;

        for (const auto& e : j["events"])
        {
            TelemetryEvent evt;
            if (e.contains("type"))
                evt.type = stringToEventType(e["type"].get<std::string>());
            if (e.contains("languageId"))
                evt.languageId = e["languageId"].get<std::string>();
            if (e.contains("timestampMs"))
                evt.timestampMs = e["timestampMs"].get<int64_t>();
            if (e.contains("revision"))
                evt.revision = e["revision"].get<int>();
            if (e.contains("requestId"))
                evt.requestId = e["requestId"].get<std::string>();
            if (e.contains("acceptLength"))
                evt.acceptLength = e["acceptLength"].get<int>();
            if (e.contains("compileSuccess"))
                evt.compileSuccess = e["compileSuccess"].get<bool>();
            if (e.contains("diagnosticCount"))
                evt.diagnosticCount = e["diagnosticCount"].get<int>();
            if (e.contains("isStale"))
                evt.isStale = e["isStale"].get<bool>();
            if (e.contains("ghostLabel"))
                evt.ghostLabel = e["ghostLabel"].get<std::string>();

            // Reconstruct requestStartTimes_ for time-to-accept computation.
            if (evt.type == GhostEventType::Displayed && !evt.isStale)
                requestStartTimes_[evt.requestId] = evt.timestampMs;

            events_.push_back(std::move(evt));
        }
    }
    catch (const nlohmann::json::exception&)
    {
        // JSON parse error — silently ignore (start fresh).
        events_.clear();
        requestStartTimes_.clear();
    }
}

bool GhostCompletionTelemetry::saveToFile(const std::string& filePath) const
{
    std::ofstream file(filePath, std::ios::trunc | std::ios::out);
    if (!file.is_open())
        return false;

    file << toJson();
    return file.good();
}

bool GhostCompletionTelemetry::loadFromFile(const std::string& filePath)
{
    std::ifstream file(filePath);
    if (!file.is_open())
        return false;

    std::stringstream ss;
    ss << file.rdbuf();
    loadFromJson(ss.str());
    return true;
}

} // namespace hathor::lsp
