// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * GhostCompletionTelemetry.hpp — JUCE-free capture and reporting of
 * ghost-completion lifecycle events for quality feedback (J-6).
 *
 * Tracks the full ghost completion lifecycle: displayed, accepted,
 * partially accepted, rejected, stale-rejected, time-to-accept, compile
 * results, introduced diagnostics, immediate deletion, and heavy modification.
 *
 * All event types and metrics are tagged with a `languageId` ("hathor" or
 * "chuck") so per-language quality can be reported independently.
 *
 * This class is JUCE-free and fully unit-testable in hathor-ui-tests.
 * It runs only on the JUCE message thread — never on the audio thread.
 *
 * Requirement references: J-6 (completion quality feedback loop)
 */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// TelemetryEvent — a single lifecycle event for a ghost completion
// ---------------------------------------------------------------------------

/**
 * Lifecycle stage of a ghost completion suggestion.
 *
 * Events form a partial ordering per completion "instance":
 *
 *   DISPLAYED → {ACCEPTED | PARTIALLY_ACCEPTED+acceptedText | REJECTED | IMMEDIATE_DELETION}
 *   DISPLAYED → STALE_REJECTED  (stale response discarded, excluded from stats)
 *
 * COMPILE_RESULT and DIAGNOSTIC_ADDED are outcome events that attach to the
 * most recent accepted completion.
 */
enum class GhostEventType : uint8_t {
    Displayed,            ///< a ghost result was shown to the user
    Accepted,             ///< the full ghost text was accepted
    PartiallyAccepted,    ///< a prefix of the ghost was accepted
    Rejected,             ///< the ghost was dismissed or not used
    StaleRejected,        ///< a stale/late response was discarded (excluded from stats)
    CompileResult,        ///< compilation after acceptance succeeded or failed
    DiagnosticAdded,      ///< a new diagnostic appeared after acceptance
    ImmediateDeletion,    ///< the accepted text was deleted within the grace window
    HeavyModification,    ///< the accepted text was substantially edited
};

/**
 * A single telemetry event for a ghost completion lifecycle.
 *
 * - `languageId` distinguishes .hathor ("hathor") from ChucK ("chuck").
 * - `timestampMs` is a steady-clock epoch millisecond, used for time-to-accept.
 * - `revision` is the document revision at event time (for debugging).
 * - `acceptLength` is set for PARTIALLY_ACCEPTED events (chars accepted).
 * - `compileSuccess` is set for COMPILE_RESULT events.
 * - `diagnosticCount` is the number of new diagnostics for DIAGNOSTIC_ADDED.
 * - `isStale` marks events that should be excluded from quality statistics
 *   (stale responses, completions cancelled before display, etc.).
 * - `requestId` correlates events to a single completion instance.
 * - `ghostText` is a short debug label (NOT full source) — at most the
 *   first N characters of the ghost text, for debugging aggregate patterns.
 */
struct TelemetryEvent {
    GhostEventType  type;
    std::string     languageId;           ///< "hathor" or "chuck"
    int64_t         timestampMs            = 0;
    int             revision               = 0;
    std::string     requestId;            ///< correlates to a completion instance
    int             acceptLength           = 0;     ///< PARTIALLY_ACCEPTED only
    bool            compileSuccess         = false; ///< COMPILE_RESULT only
    int             diagnosticCount        = 0;     ///< DIAGNOSTIC_ADDED only
    bool            isStale                = false; ///< excluded from stats
    std::string     ghostLabel;           ///< short debug label (≤64 chars, not full source)
};

// ---------------------------------------------------------------------------
// Metrics — aggregate summary of ghost completion quality per language
// ---------------------------------------------------------------------------

/**
 * Aggregate quality metrics for ghost completions, computed per-language.
 *
 * All rates are in [0, 1]. `totalDisplayed > 0` is required for rates to be
 * meaningful; otherwise rates default to 0.
 */
struct GhostMetrics {
    std::string languageId;
    int         totalDisplayed     = 0;
    int         acceptedCount      = 0;       ///< full accepts
    int         partialAcceptCount = 0;       ///< partial accepts
    int         rejectedCount      = 0;       ///< explicit rejections
    int         compileAttempts    = 0;       ///< COMPILE_RESULT events
    int         compileSuccesses   = 0;       ///< successful compiles
    int         diagnosticAddedCount = 0;     ///< DIAGNOSTIC_ADDED events
    int         immediateDeletionCount = 0;  ///< IMMEDIATE_DELETION events
    int         heavyModCount      = 0;       ///< HEAVY_MODIFICATION events
    double      meanTimeToAcceptMs = 0.0;     ///< mean DISPLAYED→ACCEPTED latency

    // Derived rates (computed by GhostCompletionTelemetry) — all in [0,1].
    double      acceptanceRate     = 0.0;     ///< (accepted + partial) / displayed
    double      compileSuccessRate = 0.0;     ///< compileSuccesses / compileAttempts
    double      diagnosticRate     = 0.0;     ///< diagnosticAddedCount / accepted
    double      immediateDeletionRate = 0.0; ///< immediateDeletionCount / accepted
    double      heavyModificationRate = 0.0; ///< heavyModCount / accepted
};

// ---------------------------------------------------------------------------
// GhostCompletionTelemetry — captures events and computes metrics
// ---------------------------------------------------------------------------

/**
 * Captures ghost-completion lifecycle events and computes per-language
 * quality metrics (J-6).
 *
 * Design notes:
 *   - All events are stored in-memory per session. A follow-up task will
 *     persist them via the existing ApplicationProperties pattern.
 *   - Events marked `isStale = true` are retained for debugging but
 *     excluded from all metric computations.
 *   - Per-language isolation is achieved via the `languageId` field on
 *     TelemetryEvent; metrics are computed per distinct languageId seen.
 *   - This class is JUCE-free and non-blocking — all operations are O(1)
 *     or O(n) on the event vector (n = events per session, typically small).
 */
class GhostCompletionTelemetry
{
public:
    GhostCompletionTelemetry() = default;
    ~GhostCompletionTelemetry() = default;

    GhostCompletionTelemetry(const GhostCompletionTelemetry&) = delete;
    GhostCompletionTelemetry& operator=(const GhostCompletionTelemetry&) = delete;

    // -----------------------------------------------------------------------
    // Recording events
    // -----------------------------------------------------------------------

    /** Record a single telemetry event. */
    void recordEvent(const TelemetryEvent& event);

    /**
     * Convenience: record a DISPLAYED event.
     * @param languageId  "hathor" or "chuck"
     * @param requestId   correlates to the completion instance
     * @param timestampMs steady-clock epoch ms
     * @param revision    document revision at display time
     */
    void recordDisplayed(const std::string& languageId,
                         const std::string& requestId,
                         int64_t timestampMs,
                         int revision);

    /**
     * Convenience: record an ACCEPTED event.
     * Computes time-to-accept from the most recent DISPLAYED event for
     * the same requestId.
     */
    void recordAccepted(const std::string& requestId,
                        int64_t timestampMs,
                        int revision);

    /**
     * Convenience: record a PARTIALLY_ACCEPTED event.
     * @param acceptLength  number of characters accepted
     */
    void recordPartiallyAccepted(const std::string& requestId,
                                 int64_t timestampMs,
                                 int revision,
                                 size_t acceptLength);

    /**
     * Convenience: record a REJECTED event.
     */
    void recordRejected(const std::string& requestId,
                        int64_t timestampMs,
                        int revision);

    /**
     * Convenience: record a STALE_REJECTED event (excluded from stats).
     */
    void recordStaleRejected(const std::string& requestId,
                             int64_t timestampMs,
                             int revision);

    /**
     * Convenience: record a COMPILE_RESULT event.
     */
    void recordCompileResult(const std::string& requestId,
                             int64_t timestampMs,
                             bool compileSuccess);

    /**
     * Convenience: record a DIAGNOSTIC_ADDED event.
     * @param diagnosticCount  number of new diagnostics observed
     */
    void recordDiagnosticAdded(const std::string& requestId,
                               int64_t timestampMs,
                               int diagnosticCount);

    /**
     * Convenience: record an IMMEDIATE_DELETION event.
     */
    void recordImmediateDeletion(const std::string& requestId,
                                 int64_t timestampMs);

    /**
     * Convenience: record a HEAVY_MODIFICATION event.
     */
    void recordHeavyModification(const std::string& requestId,
                                 int64_t timestampMs);

    // -----------------------------------------------------------------------
    // Querying events
    // -----------------------------------------------------------------------

    /** Read-only access to all recorded events. */
    const std::vector<TelemetryEvent>& events() const noexcept { return events_; }

    /** Read-only access to events for a specific languageId. */
    std::vector<TelemetryEvent> eventsForLanguage(const std::string& languageId) const;

    // -----------------------------------------------------------------------
    // Metrics computation
    // -----------------------------------------------------------------------

    /**
     * Compute aggregate metrics from recorded events, grouped by languageId.
     * Stale events are excluded.
     */
    std::vector<GhostMetrics> computeMetrics() const;

    /**
     * Compute metrics for a specific languageId.
     * Returns an empty-metrics struct if no events were recorded for that
     * language.
     */
    GhostMetrics computeMetricsForLanguage(const std::string& languageId) const;

    // -----------------------------------------------------------------------
    // Reporting
    // -----------------------------------------------------------------------

    /**
     * Produce a human-readable per-language quality report.
     * Each line shows the language, displayed count, acceptance rate,
     * mean time-to-accept, compile success rate, and other key rates.
     */
    std::string generateReport() const;

    // -----------------------------------------------------------------------
    // Persistence (serialization for disk storage)
    // -----------------------------------------------------------------------

    /**
     * Serialize all events to a JSON string for persistent storage.
     * Uses nlohmann::json — JUCE-free, no JUCE dependencies.
     */
    std::string toJson() const;

    /**
     * Load events from a JSON string (e.g. restored from disk on startup).
     * Merges with existing in-memory events. Clears existing state first.
     */
    void loadFromJson(const std::string& jsonStr);

    /**
     * Save all events to a file path (for testing / standalone use).
     * Uses simple file I/O — no JUCE dependencies.
     * Returns true on success.
     */
    bool saveToFile(const std::string& filePath) const;

    /**
     * Load events from a file path.
     * Returns true on success (file existed and was parsed).
     */
    bool loadFromFile(const std::string& filePath);

    /**
     * Truncate a ghost text label to at most `maxLen` characters for
     * diagnostic logging (does NOT store full source code).
     */
    static std::string truncateLabel(std::string_view text, size_t maxLen = 64);

    // -----------------------------------------------------------------------
    // Reset
    // -----------------------------------------------------------------------

    /** Clear all recorded events (for testing / session reset). */
    void clear() noexcept { events_.clear(); requestStartTimes_.clear(); }

private:
    std::vector<TelemetryEvent> events_;

    /// Maps requestId → DISPLAYED timestamp for time-to-accept computation.
    std::map<std::string, int64_t> requestStartTimes_;
};

} // namespace hathor::lsp
