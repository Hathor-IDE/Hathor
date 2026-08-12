// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * Diagnostic.hpp — L-3: Unified, normalized diagnostic model for the IDE
 * Problems / Diagnostics surface.
 *
 * This is the single, JUCE-free canonical diagnostic abstraction consumed by:
 *   - The UI ProblemsPanel (ui/)
 *   - The StatusRibbon (ui/)
 *   - The MCP read surface (control/ via DiagnosticRegistry)
 *   - Unit tests (tests-ui/)
 *
 * DESIGN PRINCIPLES:
 *   - One model for every diagnostic source; each source keeps its authority.
 *   - A diagnostic never loses its provenance — the `source` field is always
 *     set and is the primary grouping key.
 *   - Replace-by-source-key semantics: the same (DiagSource, uri) pair is
 *     replaced wholesale on re-publish (mirrors LSP publishDiagnostics).
 *   - Stable identity: each Diagnostic gets a monotonic `id` so the UI can
 *     refresh/replace rows rather than duplicating.
 *   - Severity is normalised to four tiers (Error/Warning/Info/Hint) that map
 *     1:1 onto LSP DiagnosticSeverity.
 *
 * Diagnostic sources:
 *   StrudelLsp       — AI-4 Strudel LSP server (language diagnostics for .hathor)
 *   ChuckCompiler    — AI-5 real libchuck/validateChuckSource compiler diagnostics (.ck)
 *   HathorValidation — AI-3 supported-surface / metadata compatibility validation
 *   BuildSystem      — C++/JUCE build diagnostics (when available from the build tool)
 *   TaskTestFailure  — build / test / task failures surfaced by the task runner
 *   ChuckWorker      — ChucK worker / session lifecycle failures (B4-K3/K5/K8)
 *   Runtime          — audio-engine runtime errors
 *
 * No JUCE dependency — the registry can be unit-tested without the JUCE GUI stack.
 *
 * L-3 acceptance: diagnostics remain deterministic and authoritative; this
 * layer never accepts AI guesses. AI remains available through the existing
 * Phase H-K contextual AI/action architecture, not through this surface.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace hathor::control {

// ---------------------------------------------------------------------------
// Severity — normalised across all sources (maps 1:1 to LSP DiagnosticSeverity)
// ---------------------------------------------------------------------------
enum class DiagSeverity : int
{
    Error   = 1,
    Warning = 2,
    Info    = 3,
    Hint    = 4,
};

// ---------------------------------------------------------------------------
// Source — identifies which authoritative system produced this diagnostic
// ---------------------------------------------------------------------------
enum class DiagSource
{
    StrudelLsp,        ///< AI-4: Strudel LSP server
    ChuckCompiler,     ///< AI-5: real libchuck compiler diagnostics
    HathorValidation,  ///< AI-3: supported-surface / metadata validation
    BuildSystem,       ///< C++/JUCE build diagnostics
    TaskTestFailure,   ///< build / test / task failures
    ChuckWorker,       ///< ChucK worker / session lifecycle failures (B4)
    Runtime,           ///< runtime / audio-engine errors
};

// ---------------------------------------------------------------------------
// Diagnostic — a single normalised diagnostic entry
// ---------------------------------------------------------------------------
struct Diagnostic
{
    uint64_t     id       = 0;   ///< stable identity (monotonic, assigned by registry)
    DiagSeverity severity = DiagSeverity::Error;
    DiagSource   source   = DiagSource::Runtime;
    std::string  sourceLabel;     ///< human-readable source name ("Strudel LSP", etc.)
    std::string  code;          ///< source-specific code (e.g. "CK_COMPILE_ERROR")
    std::string  message;       ///< human-readable message
    std::string  uri;           ///< file:// URI, or synthetic URI for non-file sources
    int          line   = 0;    ///< 1-based line number (0 if not applicable)
    int          column = 0;    ///< 1-based column (0 if not applicable)
    std::string  relatedInfo;   ///< optional extra context (e.g. worker PID, job ID)
};

// ---------------------------------------------------------------------------
// Static helpers — JUCE-free, no allocations in the common path
// ---------------------------------------------------------------------------

/// Human-readable label for a DiagSource.
std::string_view sourceLabel(DiagSource source) noexcept;

/// Human-readable label for a DiagSeverity.
std::string_view severityLabel(DiagSeverity sev) noexcept;

/// Parse a severity label back to an enum. Returns false on unknown label.
bool parseSeverity(std::string_view label, DiagSeverity& out) noexcept;

/// Parse a source label back to an enum. Returns false on unknown label.
bool parseSource(std::string_view label, DiagSource& out) noexcept;

/// Returns true if the source is a language/compiler diagnostic (LSP or ChucK).
bool isLanguageDiagnostic(DiagSource source) noexcept;

} // namespace hathor::control

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

#include <utility>

namespace hathor::control {

inline std::string_view sourceLabel(DiagSource source) noexcept
{
    switch (source)
    {
        case DiagSource::StrudelLsp:       return "Strudel LSP";
        case DiagSource::ChuckCompiler:    return "ChucK Compiler";
        case DiagSource::HathorValidation: return "Hathor Validation";
        case DiagSource::BuildSystem:      return "Build System";
        case DiagSource::TaskTestFailure:  return "Task / Test";
        case DiagSource::ChuckWorker:      return "ChucK Worker";
        case DiagSource::Runtime:          return "Runtime";
    }
    return "Unknown";
}

inline std::string_view severityLabel(DiagSeverity sev) noexcept
{
    switch (sev)
    {
        case DiagSeverity::Error:   return "Error";
        case DiagSeverity::Warning: return "Warning";
        case DiagSeverity::Info:    return "Info";
        case DiagSeverity::Hint:    return "Hint";
    }
    return "Unknown";
}

inline bool parseSeverity(std::string_view label, DiagSeverity& out) noexcept
{
    if (label == "Error" || label == "error" || label == "E") { out = DiagSeverity::Error;   return true; }
    if (label == "Warning" || label == "warning" || label == "W") { out = DiagSeverity::Warning; return true; }
    if (label == "Info" || label == "info" || label == "I") { out = DiagSeverity::Info;    return true; }
    if (label == "Hint" || label == "hint" || label == "H") { out = DiagSeverity::Hint;    return true; }
    return false;
}

inline bool parseSource(std::string_view label, DiagSource& out) noexcept
{
    if (label == "Strudel LSP" || label == "strudel_lsp") { out = DiagSource::StrudelLsp; return true; }
    if (label == "ChucK Compiler" || label == "chuck_compiler") { out = DiagSource::ChuckCompiler; return true; }
    if (label == "Hathor Validation" || label == "hathor_validation") { out = DiagSource::HathorValidation; return true; }
    if (label == "Build System" || label == "build") { out = DiagSource::BuildSystem; return true; }
    if (label == "Task / Test" || label == "task") { out = DiagSource::TaskTestFailure; return true; }
    if (label == "ChucK Worker" || label == "chuck_worker") { out = DiagSource::ChuckWorker; return true; }
    if (label == "Runtime" || label == "runtime") { out = DiagSource::Runtime; return true; }
    return false;
}

inline bool isLanguageDiagnostic(DiagSource source) noexcept
{
    return source == DiagSource::StrudelLsp || source == DiagSource::ChuckCompiler;
}

} // namespace hathor::control
