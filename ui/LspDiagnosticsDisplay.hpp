// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * LspDiagnosticsDisplay.hpp — manages LSP diagnostics for the editor gutter.
 *
 * Stores diagnostics received from the LSP server (publishDiagnostics) and
 * provides query APIs used by the editor to render squiggly underlines and
 * gutter markers.
 *
 * The diagnostics are stored per-URI and indexed by line for efficient lookup.
 *
 * Requirement references: AI-4
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "LspProtocol.hpp"

#include <map>
#include <string>
#include <vector>

namespace hathor::ui {

/**
 * LspDiagnosticsDisplay
 *
 * Not a visual component — it's the data model for editor diagnostics.
 * The HathorTab queries it to render squiggly underlines and gutter marks.
 *
 * Diagnostics are stored indexed by document URI and by line number.
 */
class LspDiagnosticsDisplay
{
public:
    LspDiagnosticsDisplay() = default;
    ~LspDiagnosticsDisplay() = default;

    LspDiagnosticsDisplay(const LspDiagnosticsDisplay&) = delete;
    LspDiagnosticsDisplay& operator=(const LspDiagnosticsDisplay&) = delete;

    // -----------------------------------------------------------------------
    // Update
    // -----------------------------------------------------------------------

    /**
     * Replace all diagnostics for the given URI.
     * Called when the LSP server publishes new diagnostics.
     */
    void setDiagnostics(const std::string& uri,
                        const std::vector<lsp::Diagnostic>& diagnostics);

    /**
     * Clear all diagnostics for the given URI (called on document close).
     */
    void clearDiagnostics(const std::string& uri);

    /** Clear all diagnostics for all URIs. */
    void clearAll();

    // -----------------------------------------------------------------------
    // Query
    // -----------------------------------------------------------------------

    /**
     * Get all diagnostics for the given URI and line (0-based).
     */
    std::vector<lsp::Diagnostic> getDiagnosticsForLine(
        const std::string& uri, int line) const;

    /**
     * Get all diagnostics for the given URI.
     */
    const std::vector<lsp::Diagnostic>& getAllDiagnostics(const std::string& uri) const;

    /**
     * Check if there are any errors (severity == Error) for the given URI.
     */
    bool hasErrors(const std::string& uri) const;

    /**
     * Get the error count for the given URI.
     */
    int errorCount(const std::string& uri) const;

    /**
     * Get a summary string for the status bar (e.g. "2 errors, 1 warning").
     */
    juce::String summary(const std::string& uri) const;

private:
    /** Diagnostics for a single document. */
    struct DocDiagnostics
    {
        std::vector<lsp::Diagnostic> all;
        std::map<int, std::vector<const lsp::Diagnostic*>> byLine;
    };

    std::map<std::string, DocDiagnostics> docs_;
};

} // namespace hathor::ui
