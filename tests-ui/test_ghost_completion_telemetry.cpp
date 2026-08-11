// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_ghost_completion_telemetry.cpp — J-6 unit tests for ghost completion
 * lifecycle event capture and per-language quality metrics.
 *
 * Tests cover:
 *   1.  DISPLAYED events are captured and counted.
 *   2.  ACCEPTED events are captured and counted.
 *   3.  PARTIALLY_ACCEPTED events record accept length.
 *   4.  REJECTED events are captured and counted.
 *   5.  STALE_REJECTED events are excluded from stats.
 *   6.  Time-to-accept is computed from DISPLAYED→ACCEPTED.
 *   7.  Compile results (success/fail) are tracked.
 *   8.  Diagnostic-added events are tracked.
 *   9.  Immediate-deletion events are tracked.
 *   10. Heavy-modification events are tracked.
 *   11. Per-language metrics are isolated (hathor vs chuck).
 *   12. Acceptance rate is computed correctly.
 *   13. All rates are in [0, 1].
 *   14. Stale events excluded from all metric computations.
 *   15. No events for an empty telemetry store.
 *   16. truncateLabel truncates to maxLen.
 *   17. generateReport produces readable output.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
 */

#include <catch2/catch_test_macros.hpp>

#include "GhostCompletionTelemetry.hpp"

#include <string>
#include <vector>

using namespace hathor::lsp;

// ===========================================================================
// 1. DISPLAYED events are captured and counted.
// ===========================================================================

TEST_CASE("J-6: DISPLAYED events are captured", "[j-6][displayed]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "req-1", 1000, 1);
    telemetry.recordDisplayed("chuck", "req-2", 1050, 2);

    REQUIRE(telemetry.events().size() == 2);
    REQUIRE(telemetry.events()[0].type == GhostEventType::Displayed);
    REQUIRE(telemetry.events()[0].languageId == "hathor");
    REQUIRE(telemetry.events()[0].requestId == "req-1");
    REQUIRE(telemetry.events()[1].languageId == "chuck");
}

// ===========================================================================
// 2. ACCEPTED events are captured and counted.
// ===========================================================================

TEST_CASE("J-6: ACCEPTED events are captured", "[j-6][accepted]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "req-1", 1000, 1);
    telemetry.recordAccepted("req-1", 1050, 1);

    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics.totalDisplayed == 1);
    REQUIRE(metrics.acceptedCount == 1);
}

// ===========================================================================
// 3. PARTIALLY_ACCEPTED events record accept length.
// ===========================================================================

TEST_CASE("J-6: PARTIALLY_ACCEPTED records accept length", "[j-6][partial]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "req-1", 1000, 1);
    telemetry.recordPartiallyAccepted("req-1", 1030, 1, 5);

    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics.totalDisplayed == 1);
    REQUIRE(metrics.partialAcceptCount == 1);

    // Verify the event was stored correctly
    bool found = false;
    for (const auto& evt : telemetry.events())
    {
        if (evt.type == GhostEventType::PartiallyAccepted)
        {
            REQUIRE(evt.acceptLength == 5);
            found = true;
        }
    }
    REQUIRE(found);
}

// ===========================================================================
// 4. REJECTED events are captured and counted.
// ===========================================================================

TEST_CASE("J-6: REJECTED events are captured", "[j-6][rejected]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "req-1", 1000, 1);
    telemetry.recordRejected("req-1", 1010, 1);

    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics.totalDisplayed == 1);
    REQUIRE(metrics.rejectedCount == 1);
}

// ===========================================================================
// 5. STALE_REJECTED events are excluded from stats.
// ===========================================================================

TEST_CASE("J-6: STALE_REJECTED events excluded from stats", "[j-6][stale]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "req-1", 1000, 1);
    telemetry.recordStaleRejected("req-1", 1010, 1);

    // The stale event IS stored
    REQUIRE(telemetry.events().size() == 2);
    REQUIRE(telemetry.events()[1].isStale == true);
    REQUIRE(telemetry.events()[1].type == GhostEventType::StaleRejected);

    // But it's excluded from metrics
    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics.totalDisplayed == 1);
    REQUIRE(metrics.rejectedCount == 0);
    REQUIRE(metrics.acceptedCount == 0);
}

// ===========================================================================
// 6. Time-to-accept is computed from DISPLAYED→ACCEPTED.
// ===========================================================================

TEST_CASE("J-6: time-to-accept is computed correctly", "[j-6][tta]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "req-1", 1000, 1);
    telemetry.recordDisplayed("hathor", "req-2", 2000, 2);

    // req-1: 50ms TTA
    telemetry.recordAccepted("req-1", 1050, 1);
    // req-2: 100ms TTA
    telemetry.recordAccepted("req-2", 2100, 2);

    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics.meanTimeToAcceptMs == 75.0); // (50 + 100) / 2
}

TEST_CASE("J-6: time-to-accept includes partial accepts", "[j-6][tta-partial]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "req-1", 1000, 1);

    // Partial accept at 1050ms (TTA = 50ms)
    telemetry.recordPartiallyAccepted("req-1", 1050, 1, 5);

    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics.meanTimeToAcceptMs == 50.0);
    REQUIRE(metrics.partialAcceptCount == 1);
    REQUIRE(metrics.acceptedCount == 0);
}

// ===========================================================================
// 7. Compile results (success/fail) are tracked.
// ===========================================================================

TEST_CASE("J-6: compile results are tracked", "[j-6][compile]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "req-1", 1000, 1);
    telemetry.recordAccepted("req-1", 1050, 1);

    // First accept: compile success
    telemetry.recordCompileResult("req-1", 1100, true);
    // Second display + accept: compile failure
    telemetry.recordDisplayed("hathor", "req-2", 1200, 2);
    telemetry.recordAccepted("req-2", 1250, 2);
    telemetry.recordCompileResult("req-2", 1300, false);

    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics.compileAttempts == 2);
    REQUIRE(metrics.compileSuccesses == 1);
    REQUIRE(metrics.compileSuccessRate == 0.5);
}

// ===========================================================================
// 8. Diagnostic-added events are tracked.
// ===========================================================================

TEST_CASE("J-6: diagnostic-added events are tracked", "[j-6][diagnostics]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "req-1", 1000, 1);
    telemetry.recordAccepted("req-1", 1050, 1);
    telemetry.recordDiagnosticAdded("req-1", 1100, 2);

    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics.diagnosticAddedCount == 1);
    REQUIRE(metrics.acceptedCount == 1);
    // 1 diagnostic event / 1 accept = 100%
    REQUIRE(metrics.diagnosticRate == 1.0);
}

// ===========================================================================
// 9. Immediate-deletion events are tracked.
// ===========================================================================

TEST_CASE("J-6: immediate-deletion events are tracked", "[j-6][deletion]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "req-1", 1000, 1);
    telemetry.recordAccepted("req-1", 1050, 1);
    telemetry.recordImmediateDeletion("req-1", 1075);

    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics.immediateDeletionCount == 1);
    REQUIRE(metrics.acceptedCount == 1);
    REQUIRE(metrics.immediateDeletionRate == 1.0);
}

// ===========================================================================
// 10. Heavy-modification events are tracked.
// ===========================================================================

TEST_CASE("J-6: heavy-modification events are tracked", "[j-6][modify]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "req-1", 1000, 1);
    telemetry.recordAccepted("req-1", 1050, 1);
    telemetry.recordHeavyModification("req-1", 1075);

    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics.heavyModCount == 1);
    REQUIRE(metrics.acceptedCount == 1);
    REQUIRE(metrics.heavyModificationRate == 1.0);
}

// ===========================================================================
// 11. Per-language metrics are isolated (hathor vs chuck).
// ===========================================================================

TEST_CASE("J-6: per-language metrics are isolated", "[j-6][per-language]")
{
    GhostCompletionTelemetry telemetry;

    // Hathor: 2 displayed, 1 accepted, 1 rejected
    telemetry.recordDisplayed("hathor", "h1", 1000, 1);
    telemetry.recordDisplayed("hathor", "h2", 2000, 2);
    telemetry.recordAccepted("h1", 1050, 1);
    telemetry.recordRejected("h2", 2010, 2);

    // Chuck: 3 displayed, 2 accepted, 1 rejected
    telemetry.recordDisplayed("chuck", "c1", 1100, 1);
    telemetry.recordDisplayed("chuck", "c2", 2100, 2);
    telemetry.recordDisplayed("chuck", "c3", 3100, 3);
    telemetry.recordAccepted("c1", 1150, 1);
    telemetry.recordAccepted("c2", 2150, 2);
    telemetry.recordRejected("c3", 3110, 3);

    auto hMetrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(hMetrics.totalDisplayed == 2);
    REQUIRE(hMetrics.acceptedCount == 1);
    REQUIRE(hMetrics.rejectedCount == 1);
    REQUIRE(hMetrics.acceptanceRate == 0.5);

    auto cMetrics = telemetry.computeMetricsForLanguage("chuck");
    REQUIRE(cMetrics.totalDisplayed == 3);
    REQUIRE(cMetrics.acceptedCount == 2);
    REQUIRE(cMetrics.rejectedCount == 1);
    REQUIRE(cMetrics.acceptanceRate ==
            static_cast<double>(2) / 3); // ~0.667

    // Verify both languages appear in the composite metrics
    auto all = telemetry.computeMetrics();
    REQUIRE(all.size() == 2);
}

// ===========================================================================
// 12. Acceptance rate is computed correctly.
// ===========================================================================

TEST_CASE("J-6: acceptance rate includes partial accepts", "[j-6][acceptance-rate]")
{
    GhostCompletionTelemetry telemetry;

    // 4 displayed: 1 full accept, 1 partial accept, 2 rejected
    telemetry.recordDisplayed("hathor", "r1", 1000, 1);
    telemetry.recordDisplayed("hathor", "r2", 2000, 2);
    telemetry.recordDisplayed("hathor", "r3", 3000, 3);
    telemetry.recordDisplayed("hathor", "r4", 4000, 4);

    telemetry.recordAccepted("r1", 1050, 1);
    telemetry.recordPartiallyAccepted("r2", 2050, 2, 5);
    telemetry.recordRejected("r3", 3010, 3);
    telemetry.recordRejected("r4", 4010, 4);

    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    // (1 full + 1 partial) / 4 displayed = 0.5
    REQUIRE(metrics.acceptanceRate == 0.5);
    REQUIRE(metrics.acceptedCount == 1);
    REQUIRE(metrics.partialAcceptCount == 1);
    REQUIRE(metrics.rejectedCount == 2);
}

// ===========================================================================
// 13. All rates are in [0, 1].
// ===========================================================================

TEST_CASE("J-6: all rates are in [0, 1]", "[j-6][rates-bounded]")
{
    GhostCompletionTelemetry telemetry;

    // Start with a clean slate: one displayed, one accepted, compile success,
    // one diagnostic, one immediate deletion, one heavy modification.
    telemetry.recordDisplayed("hathor", "r1", 1000, 1);
    telemetry.recordAccepted("r1", 1050, 1);
    telemetry.recordCompileResult("r1", 1075, true);
    telemetry.recordDiagnosticAdded("r1", 1100, 1);
    telemetry.recordImmediateDeletion("r1", 1100);
    telemetry.recordHeavyModification("r1", 1100);

    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics.acceptanceRate >= 0.0);
    REQUIRE(metrics.acceptanceRate <= 1.0);
    REQUIRE(metrics.compileSuccessRate >= 0.0);
    REQUIRE(metrics.compileSuccessRate <= 1.0);
    REQUIRE(metrics.diagnosticRate >= 0.0);
    REQUIRE(metrics.diagnosticRate <= 1.0);
    REQUIRE(metrics.immediateDeletionRate >= 0.0);
    REQUIRE(metrics.immediateDeletionRate <= 1.0);
    REQUIRE(metrics.heavyModificationRate >= 0.0);
    REQUIRE(metrics.heavyModificationRate <= 1.0);

    // Edge case: 100% rejection → acceptance rate 0
    telemetry.clear();
    telemetry.recordDisplayed("hathor", "r2", 1000, 1);
    telemetry.recordRejected("r2", 1010, 1);
    auto metrics2 = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics2.acceptanceRate == 0.0);
    REQUIRE(metrics2.acceptedCount == 0);

    // Edge case: 100% acceptance → acceptance rate 1.0
    telemetry.clear();
    telemetry.recordDisplayed("hathor", "r3", 1000, 1);
    telemetry.recordAccepted("r3", 1050, 1);
    auto metrics3 = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(metrics3.acceptanceRate == 1.0);
}

// ===========================================================================
// 14. Stale events excluded from all metric computations.
// ===========================================================================

TEST_CASE("J-6: stale events excluded from metrics", "[j-6][stale-excluded]")
{
    GhostCompletionTelemetry telemetry;

    // Valid display + accept
    telemetry.recordDisplayed("hathor", "good-1", 1000, 1);
    telemetry.recordAccepted("good-1", 1050, 1);

    // Stale display + stale accept (should be excluded)
    TelemetryEvent staleDisplay;
    staleDisplay.type = GhostEventType::Displayed;
    staleDisplay.languageId = "hathor";
    staleDisplay.timestampMs = 2000;
    staleDisplay.requestId = "bad-1";
    staleDisplay.revision = 2;
    staleDisplay.isStale = true;
    telemetry.recordEvent(staleDisplay);

    telemetry.recordStaleRejected("bad-1", 2010, 2);

    auto metrics = telemetry.computeMetricsForLanguage("hathor");
    // Only the valid display should count
    REQUIRE(metrics.totalDisplayed == 1);
    REQUIRE(metrics.acceptedCount == 1);
}

// ===========================================================================
// 15. No events for an empty telemetry store.
// ===========================================================================

TEST_CASE("J-6: empty telemetry produces zero metrics", "[j-6][empty]")
{
    GhostCompletionTelemetry telemetry;

    auto allMetrics = telemetry.computeMetrics();
    REQUIRE(allMetrics.empty());

    auto hMetrics = telemetry.computeMetricsForLanguage("hathor");
    REQUIRE(hMetrics.totalDisplayed == 0);
    REQUIRE(hMetrics.acceptedCount == 0);
    REQUIRE(hMetrics.acceptanceRate == 0.0);
    REQUIRE(hMetrics.meanTimeToAcceptMs == 0.0);
}

// ===========================================================================
// 16. truncateLabel truncates to maxLen.
// ===========================================================================

TEST_CASE("J-6: truncateLabel truncates correctly", "[j-6][truncate]")
{
    REQUIRE(GhostCompletionTelemetry::truncateLabel("short") == "short");
    REQUIRE(GhostCompletionTelemetry::truncateLabel("short", 10) == "short");

    std::string longText(100, 'x');
    std::string truncated = GhostCompletionTelemetry::truncateLabel(longText, 64);
    REQUIRE(truncated.size() == 64);

    // Default maxLen is 64
    std::string truncated2 = GhostCompletionTelemetry::truncateLabel(longText);
    REQUIRE(truncated2.size() == 64);
}

// ===========================================================================
// 17. generateReport produces readable output.
// ===========================================================================

TEST_CASE("J-6: generateReport produces readable output", "[j-6][report]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "r1", 1000, 1);
    telemetry.recordAccepted("r1", 1050, 1);
    telemetry.recordRejected("r1", 1100, 1); // not possible in real flow, but for testing

    // Add a real rejection
    telemetry.recordDisplayed("hathor", "r2", 2000, 2);
    telemetry.recordRejected("r2", 2010, 2);

    telemetry.recordCompileResult("r1", 1075, true);

    std::string report = telemetry.generateReport();
    REQUIRE_FALSE(report.empty());
    REQUIRE(report.find("Ghost Completion Quality Report") != std::string::npos);
    REQUIRE(report.find("Language: hathor") != std::string::npos);
    REQUIRE(report.find("End of Report") != std::string::npos);
}

TEST_CASE("J-6: generateReport with empty telemetry", "[j-6][report-empty]")
{
    GhostCompletionTelemetry telemetry;
    std::string report = telemetry.generateReport();
    REQUIRE(report.find("no events recorded") != std::string::npos);
}

// ===========================================================================
// Bonus: recordEvent stores arbitrary events.
// ===========================================================================

TEST_CASE("J-6: recordEvent stores arbitrary events", "[j-6][record-event]")
{
    GhostCompletionTelemetry telemetry;

    TelemetryEvent evt;
    evt.type = GhostEventType::Displayed;
    evt.languageId = "hathor";
    evt.timestampMs = 1000;
    evt.requestId = "custom-1";
    evt.ghostLabel = "myGhostText";
    evt.revision = 42;
    evt.isStale = false;

    telemetry.recordEvent(evt);
    REQUIRE(telemetry.events().size() == 1);
    REQUIRE(telemetry.events()[0].ghostLabel == "myGhostText");
    REQUIRE(telemetry.events()[0].revision == 42);
}

// ===========================================================================
// Bonus: eventsForLanguage filters correctly.
// ===========================================================================

TEST_CASE("J-6: eventsForLanguage filters correctly", "[j-6][filter-language]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "h1", 1000, 1);
    telemetry.recordDisplayed("chuck", "c1", 2000, 2);
    telemetry.recordDisplayed("hathor", "h2", 3000, 3);

    auto hathorEvents = telemetry.eventsForLanguage("hathor");
    REQUIRE(hathorEvents.size() == 2);

    auto chuckEvents = telemetry.eventsForLanguage("chuck");
    REQUIRE(chuckEvents.size() == 1);

    auto emptyEvents = telemetry.eventsForLanguage("nonexistent");
    REQUIRE(emptyEvents.empty());
}

// ===========================================================================
// Bonus: clear() resets all state.
// ===========================================================================

TEST_CASE("J-6: clear resets all state", "[j-6][clear]")
{
    GhostCompletionTelemetry telemetry;
    telemetry.recordDisplayed("hathor", "h1", 1000, 1);
    telemetry.recordDisplayed("chuck", "c1", 2000, 2);

    REQUIRE(telemetry.events().size() == 2);

    telemetry.clear();

    REQUIRE(telemetry.events().empty());
    REQUIRE(telemetry.computeMetrics().empty());
}
