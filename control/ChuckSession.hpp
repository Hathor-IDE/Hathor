// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ChuckSession.hpp — AI-5: canonical ChucK session abstraction.
 *
 * Architecture boundary (AI-5):
 *
 *   MCP / AI / UI
 *         ↓
 *   ChuckSessionService  ← this layer (canonical session contract)
 *         ↓
 *   AudioWorkerManager + VmLifecycle + ChuckCompiler  (B4-K3/K4/K7)
 *
 * The caller reasons about a ChucK SESSION — a stable identity for an isolated
 * ChucK execution context — NOT about raw VM pointers, worker threads,
 * watchdog objects, or process IDs.
 *
 * A session corresponds to one tab slot [0, 15] in the B4-K3 per-tab VM
 * architecture.  The session ID is a stable string like "ck:3" (tab 3).
 *
 * Requirement references: AI-5 §1–§12, §14
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace hathor {

/**
 * Session lifecycle states — the semantic vocabulary exposed to callers.
 * Internal VMState values (B4-K3: Inactive/Active/Suspended/Destroyed/Error/
 * Failed/Recreating) are translated into these canonical states.
 */
enum class SessionState : std::uint8_t {
    Open,      ///< Session created but no live VM (tab idle / not playing).
    Live,      ///< VM exists and is running (playing/eval'd).
    Suspended, ///< VM paused deterministically; state retained (resumable).
    Destroyed, ///< VM fully torn down.
    Error,     ///< VM hit a fatal error; needs restart.
    Failed,    ///< VM was detected as hung (B4-K5); recovery pending.
    Recovering, ///< VM is being torn down and a fresh one created (B4-K5).
};

/**
 * Structured diagnostic from the real ChucK compiler/runtime.
 * This is the canonical diagnostic representation used by editor, MCP, AI.
 * No separate AI/editor diagnostic formats.
 */
struct ChuckDiagnosticInfo {
    std::string severity;    ///< "error" | "warning" | "info"
    std::string code;        ///< e.g. "CK_COMPILE_ERROR", "CK_OK"
    std::string message;     ///< human-readable message
    int         line = 0;    ///< 1-based line number (0 if not provided)
    int         column = 0;  ///< 1-based column (0 if not provided)
};

/**
 * Async job state — follows the canonical AI-1 job model conventions.
 */
enum class JobState : std::uint8_t {
    Queued,     ///< Job submitted, waiting to start.
    Running,    ///< Job is actively executing.
    Succeeded,  ///< Job completed successfully.
    Failed,     ///< Job completed with errors.
    Cancelled,  ///< Job was explicitly cancelled.
};

/**
 * Result of a compile job — delivered when the async job completes.
 */
struct CompileResult {
    bool              success = false;
    std::vector<ChuckDiagnosticInfo> diagnostics;
    std::string       sourceHash;     ///< hash of the source that was compiled
    int               shredId = -1;   ///< loaded shred ID (-1 if not loaded)
    std::string       errorMessage;   ///< populated on failure
};

/**
 * A ChucK session — an abstract, stable identity for an isolated ChucK
 * execution context.  Each session maps to a tab slot [0, 15] in the B4
 * per-tab VM architecture.
 *
 * The session ID is stable for the lifetime of the session.  Internally,
 * the session maps to a TabId; the caller should never see raw TabId values.
 */
struct ChuckSession {
    std::string                           sessionId;       ///< e.g. "ck:3"
    std::string                           source;          ///< .ck source text
    SessionState                          state = SessionState::Open;
    uint64_t                              vmGeneration = 0; ///< internal VM generation
    std::vector<ChuckDiagnosticInfo>      diagnostics;    ///< last diagnostics
    int                                   shredId = -1;    ///< loaded shred ID
    std::string                           lastError;      ///< last error message
};

/**
 * Canonical async job handle — for polling job status and cancellation.
 * Follows the RenderHandle pattern from B8-K2 (ChuckRenderWriter.hpp).
 */
class AsyncJobHandle {
public:
    AsyncJobHandle() = default;
    explicit AsyncJobHandle(uint64_t id) : id_(id) {}

    uint64_t id() const noexcept { return id_; }

private:
    uint64_t id_ = 0;
};

// ---------------------------------------------------------------------------
// Session state string helpers
// ---------------------------------------------------------------------------

inline const char* toString(SessionState s) noexcept
{
    switch (s) {
        case SessionState::Open:       return "open";
        case SessionState::Live:       return "live";
        case SessionState::Suspended:  return "suspended";
        case SessionState::Destroyed:  return "destroyed";
        case SessionState::Error:      return "error";
        case SessionState::Failed:     return "failed";
        case SessionState::Recovering: return "recovering";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Job state string helpers
// ---------------------------------------------------------------------------

inline const char* toString(JobState s) noexcept
{
    switch (s) {
        case JobState::Queued:     return "queued";
        case JobState::Running:    return "running";
        case JobState::Succeeded:  return "succeeded";
        case JobState::Failed:     return "failed";
        case JobState::Cancelled:  return "cancelled";
    }
    return "unknown";
}

} // namespace hathor
