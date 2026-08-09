// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * LiveJamSessionManager.hpp — session-scoped temp directory lifecycle for
 * LiveJam assets (B8-K1 §8, §9).
 *
 * LiveJam assets are session-scoped:
 *   1. Available for use during the current session.
 *   2. Usable by the SampleBank / rendering workflow during that session.
 *   3. Cleaned up when the session ends.
 *
 * "Session end" is defined by Hathor's existing application lifecycle:
 * the JUCEApplication::shutdown() callback (HathorApplication.cpp).  The
 * manager registers a cleanup callable that MainWindow / HathorApplication
 * invokes from shutdown.  This ties cleanup to the real app lifecycle, not
 * to arbitrary UI destruction.
 *
 * Cleanup semantics:
 *   - Removes only the LiveJam session directory.
 *   - NEVER removes Studio assets (.hathor_assets/).
 *   - Tolerates: missing files, partially rendered files, already-cleaned
 *     files, and application shutdown occurring after some assets were created.
 *   - Cleanup failures are reported via the return value but never block
 *     or destabilise the main audio/UI path.
 *
 * Crash safety:
 *   - If the process crashes and normal cleanup cannot execute, stale
 *     LiveJam directories from previous sessions MAY be cleaned at startup.
 *     This is safe: the session directory name includes the PID, so a stale
 *     directory from a crashed process is identifiable and removable without
 *     risk of deleting an active/current-session directory.
 *   - Crash cleanup is NOT guaranteed — it runs best-effort at startup only
 *     if no worker is actively using the same PID's directory (which is
 *     impossible for a crashed PID).
 *
 * JUCE-free: uses std::filesystem only.
 */

#pragma once

#include "AssetPathResolver.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace hathor {

// ---------------------------------------------------------------------------
// LiveJamSessionManager — one per Hathor session
// ---------------------------------------------------------------------------

/// Manages the LiveJam session temp directory.
///
/// Constructed once at session start (e.g. in MainWindow or on first LiveJam
/// bake).  Provides the session temp directory path for AssetPathResolver and
/// performs cleanup at session end.
///
/// Thread model:
///   - sessionDir()   — may be called from any thread after initialise().
///   - cleanup()      — called once from the main thread at shutdown.
///   - initialise()   — called once at session start; may be called from the
///                      main thread.
class LiveJamSessionManager {
public:
    LiveJamSessionManager() = default;
    ~LiveJamSessionManager()
    {
        // Destructor calls cleanup() best-effort.  If the host (MainWindow /
        // HathorApplication) already called cleanup() at shutdown, this is a
        // no-op (idempotent).  The destructor never throws.
        (void) cleanup();
    }

    LiveJamSessionManager(const LiveJamSessionManager&)            = delete;
    LiveJamSessionManager& operator=(const LiveJamSessionManager&) = delete;
    LiveJamSessionManager(LiveJamSessionManager&&)                 = default;
    LiveJamSessionManager& operator=(LiveJamSessionManager&&)      = default;

    /// Prefix for LiveJam session directory names: "hathor_live_jam_".
    /// Used by LiveJamSessionManager and the stale-session cleanup helper.
    static constexpr const char* kLiveJamPrefix = "hathor_live_jam_";

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Initialise the session temp directory.  Must be called once before
    /// sessionDir() or resolveLiveJam().
    ///
    /// The directory is created under the platform temp area (TMPDIR on
    /// macOS/Unix, or the Windows temp path), using a PID- and sequence-
    /// unique name:
    ///
    ///   <TMPDIR>/hathor_live_jam_<pid>_<seq>/
    ///
    /// @return true on success; false + errorMessage on failure.
    bool initialise();

    /// Returns the session temp directory path.
    /// Empty (default-constructed) if initialise() has not been called or
    /// failed.
    std::filesystem::path sessionDir() const noexcept { return sessionDir_; }

    /// Returns true if the session has been initialised and the temp dir exists.
    bool isInitialised() const noexcept { return initialised_; }

    /// The last error message from initialise() or cleanup(), or empty.
    const std::string& lastError() const noexcept { return lastError_; }

    // -----------------------------------------------------------------------
    // Cleanup — session end (B8-K1 §8, §9)
    // -----------------------------------------------------------------------

    /// Clean up all LiveJam assets for this session.
    ///
    /// Removes the entire session temp directory.  Tolerates missing, partial,
    /// or already-removed files.  Never blocks or throws — returns success
    /// or failure via the boolean (error details in lastError_).
    ///
    /// MUST NOT be called while a LiveJam render is actively writing to a file
    /// in the directory.  In practice, shutdown order ensures renders are
    /// quiesced before cleanup.
    ///
    /// @return true if cleanup completed (or directory was already gone);
    ///         false if an unexpected error occurred (see lastError_).
    bool cleanup() noexcept;

    // -----------------------------------------------------------------------
    // Stale-session cleanup (B8-K1 §9 — startup best-effort)
    // -----------------------------------------------------------------------

    /// Best-effort: remove stale LiveJam session directories from previous
    /// crashed sessions.
    ///
    /// A directory is "stale" if its name matches the pattern
    /// "hathor_live_jam_<pid>_<seq>" and the process with that PID is no longer
    /// running.  This function does NOT verify the PID belongs to Hathor —
    /// only that it is dead — so it cannot remove an active session directory
    /// for a different running Hathor instance.
    ///
    /// This is NOT guaranteed crash-cleanup: it is a best-effort startup sweep.
    /// If the OS reuses the PID quickly (unlikely in the 0.5–30 s window),
    /// a stale dir may be skipped.  That is acceptable — the file just lives
    /// until the next launch.
    ///
    /// @return number of stale directories removed.
    static std::size_t cleanupStaleSessions();

private:
    std::filesystem::path sessionDir_;
    bool                  initialised_   = false;
    bool                  cleanedUp_     = false;
    std::string           lastError_;
};

} // namespace hathor
