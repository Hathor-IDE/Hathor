// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * DiagnosticRegistry.hpp — L-3: thread-safe central diagnostic store.
 *
 * The single coherent IDE diagnostics surface. All deterministic diagnostic
 * systems publish here using replace-by-source-key semantics:
 *
 *   - Strudel LSP:        setDiagnostics(StrudelLsp, uri, diags)
 *   - ChucK compiler:     setDiagnostics(ChuckCompiler, uri, diags)
 *   - Hathor validation:  setDiagnostics(HathorValidation, uri, diags)
 *   - Build system:       setDiagnostics(BuildSystem, uri, diags)
 *   - Task/test failures: setDiagnostics(TaskTestFailure, uri, diags)
 *   - ChucK worker:       setDiagnostics(ChuckWorker, uri, diags)
 *   - Runtime errors:     setDiagnostics(Runtime, uri, diags)
 *
 * Each publish REPLACES all diagnostics for that (source, uri) pair — never
 * appends — mirroring LSP's publishDiagnostics replace semantics. Stale entries
 * are removed when a source re-publishes with an empty vector.
 *
 * JUCE-free: fully unit-testable without the JUCE GUI stack.
 *
 * L-3 acceptance: compiler/LSP/runtime diagnostics remain authoritative;
 * AI guesses are never substituted.
 */

#include "Diagnostic.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hathor::control {

// Forward declaration of the key type used internally.
struct DiagKey;

class DiagnosticRegistry
{
public:
    DiagnosticRegistry() = default;
    ~DiagnosticRegistry() = default;

    DiagnosticRegistry(const DiagnosticRegistry&) = delete;
    DiagnosticRegistry& operator=(const DiagnosticRegistry&) = delete;

    // -----------------------------------------------------------------------
    // Publishing — called by deterministic diagnostic sources
    // -----------------------------------------------------------------------

    /**
     * Replace ALL diagnostics for the given (source, uri) pair.
     * The supplied diagnostics are assigned stable, monotonic IDs.
     * If `diags` is empty, any existing diagnostics for this key are removed.
     */
    void setDiagnostics(DiagSource source,
                        const std::string& uri,
                        std::vector<Diagnostic> diags);

    /**
     * Convenience: add a single diagnostic (append-only, gets a new ID).
     * Used by sources that produce sporadic runtime/worker events rather
     * than a full document diagnostic set.
     */
    void addDiagnostic(Diagnostic diag);

    /**
     * Remove all diagnostics for a specific (source, uri) pair.
     */
    void clearDiagnostics(DiagSource source,
                          const std::string& uri);

    /**
     * Remove all diagnostics for all URIs from a specific source.
     */
    void clearSource(DiagSource source);

    /**
     * Remove all diagnostics from all sources.
     */
    void clearAll();

    // -----------------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------------

    /**
     * Snapshot of all current diagnostics across all sources.
     * Diagnostics are sorted by (source, uri, line, column) for stable display.
     */
    std::vector<Diagnostic> allDiagnostics() const;

    /**
     * All diagnostics for a specific URI (across all sources).
     */
    std::vector<Diagnostic> diagnosticsForUri(const std::string& uri) const;

    /**
     * Diagnostics for a specific URI and source.
     */
    std::vector<Diagnostic> diagnosticsForSourceUri(DiagSource source,
                                                    const std::string& uri) const;

    /**
     * Diagnostics at or near a specific line within a URI.
     * Returns diagnostics whose line matches, or the closest ones.
     */
    std::vector<Diagnostic> diagnosticsAtLine(const std::string& uri,
                                              int line) const;

    /** Sorted list of URIs that have at least one diagnostic. */
    std::vector<std::string> urisWithDiagnostics() const;

    // Convenience aggregate counts (across all sources)
    int errorCount()   const noexcept;
    int warningCount() const noexcept;
    int infoCount()    const noexcept;  // Info + Hint
    int total()        const noexcept;

    struct Counts
    {
        int errors   = 0;
        int warnings = 0;
        int info     = 0;  // Info + Hint
        int total    = 0;
    };

    Counts counts() const;

    // -----------------------------------------------------------------------
    // Change notification
    // -----------------------------------------------------------------------

    /**
     * Called (on the message thread or whichever thread calls setDiagnostics)
     * whenever the diagnostic set changes after setDiagnostics/clear/add.
     */
    using ChangeCallback = std::function<void()>;
    void setChangeCallback(ChangeCallback cb);

    // -----------------------------------------------------------------------
    // Stable ID management
    // -----------------------------------------------------------------------

    /** Reset the monotonic ID counter (useful for tests). */
    void resetIds();

private:
    struct DiagKey
    {
        DiagSource  source;
        std::string uri;

        bool operator<(const DiagKey& other) const noexcept
        {
            if (static_cast<int>(source) != static_cast<int>(other.source))
                return static_cast<int>(source) < static_cast<int>(other.source);
            return uri < other.uri;
        }
    };

    DiagKey makeKey(DiagSource source, const std::string& uri) const
    {
        DiagKey k;
        k.source = source;
        k.uri    = uri;
        return k;
    }

    mutable std::mutex mtx_;

    /// Diagnostics stored grouped by (source, uri) for replace semantics.
    std::map<DiagKey, std::vector<Diagnostic>> bySourceUri_;

    /// Monotonic ID counter — assigned atomically so diagnostics are
    /// uniquely identifiable across all sources.
    std::atomic<uint64_t> nextId_{1};

    ChangeCallback changeCb_;

    /// Recompute aggregate counts (called under lock).
    void recomputeCountsLocked() const;

    /// Cached aggregate counts, updated lazily.
    mutable Counts cachedCounts_;
    mutable bool  countsDirty_ = true;
};

} // namespace hathor::control
