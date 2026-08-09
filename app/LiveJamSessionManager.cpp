// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * LiveJamSessionManager.cpp — implementation.
 *
 * Uses std::filesystem::temp_directory_path() for the platform temp area,
 * following the same $TMPDIR-fallback-to-/tmp convention already used by
 * AudioWorkerManager (AudioWorkerManager.cpp:204) and AcpAgentSession
 * (AcpAgentSession.cpp:268).
 */

#include "LiveJamSessionManager.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <thread>

// POSIX process info for PID-based uniqueness and stale-session detection.
#include <unistd.h>
#include <signal.h>

namespace hathor {

// ---------------------------------------------------------------------------
// Per-process sequence counter for unique temp dir names
// ---------------------------------------------------------------------------

namespace {
std::atomic<int> g_liveJamSeq{0};

/// Returns the platform temp directory, preferring $TMPDIR (consistent with
/// AudioWorkerManager.cpp and AcpAgentSession.cpp) and falling back to
/// std::filesystem::temp_directory_path().
std::filesystem::path platformTempDir()
{
    const char* tmpdir = ::getenv("TMPDIR");
    if (tmpdir && tmpdir[0] != '\0')
        return std::filesystem::path(tmpdir);

    return std::filesystem::temp_directory_path();
}

/// Check whether a directory name matches the LiveJam session pattern
/// "hathor_live_jam_<pid>_<seq>".  Returns the PID component if it matches,
/// or -1 otherwise.
pid_t extractPidFromLiveJamDir(const std::filesystem::path& dir)
{
    const auto name = dir.filename().string();
    const std::string prefix = LiveJamSessionManager::kLiveJamPrefix;

    if (name.rfind(prefix, 0) != 0)
        return -1;

    // name = "hathor_live_jam_<pid>_<seq>"
    const auto rest = name.substr(prefix.size());
    // Parse <pid>_<seq>
    const auto underscorePos = rest.rfind('_');
    if (underscorePos == std::string::npos || underscorePos == 0)
        return -1;

    const auto pidStr = rest.substr(0, underscorePos);
    try {
        return static_cast<pid_t>(std::stol(pidStr));
    } catch (...) {
        return -1;
    }
}

/// Returns true if the process with @p pid is currently alive.
/// Uses kill(pid, 0) — does not send a signal, just checks existence.
bool isProcessAlive(pid_t pid)
{
    if (pid <= 0)
        return false;
    return ::kill(pid, 0) == 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// LiveJamSessionManager::initialise
// ---------------------------------------------------------------------------

bool LiveJamSessionManager::initialise()
{
    lastError_.clear();

    const auto base = platformTempDir();

    const pid_t pid = ::getpid();
    const int   seq = g_liveJamSeq.fetch_add(1, std::memory_order_relaxed);

    sessionDir_ = base / (std::string(kLiveJamPrefix)
                          + std::to_string(pid)
                          + "_"
                          + std::to_string(seq)
                          + "_dir");

    std::error_code ec;
    std::filesystem::create_directories(sessionDir_, ec);
    if (ec)
    {
        lastError_   = "cannot create LiveJam session dir: " + ec.message();
        initialised_ = false;
        return false;
    }

    initialised_ = true;
    cleanedUp_   = false;
    return true;
}

// ---------------------------------------------------------------------------
// LiveJamSessionManager::cleanup
// ---------------------------------------------------------------------------

bool LiveJamSessionManager::cleanup() noexcept
{
    if (cleanedUp_)
        return true;

    cleanedUp_ = true;

    if (!initialised_ || sessionDir_.empty())
        return true;

    std::error_code ec;

    // Check existence (tolerant of missing files).
    if (!std::filesystem::exists(sessionDir_, ec))
        return true;  // already gone — success

    // Verify the directory is actually within the platform temp area to
    // prevent accidental deletion of something outside it.
    const auto tempBase = platformTempDir();
    std::error_code relEc;
    const auto rel = std::filesystem::relative(sessionDir_, tempBase, relEc);
    if (relEc)
    {
        lastError_ = "cannot compute relative path for LiveJam session dir";
        return false;
    }

    const std::string relStr = rel.string();
    if (relStr.empty() || relStr == "." || relStr.find("..") != std::string::npos)
    {
        // Session dir is somehow outside the temp area — refuse to delete.
        lastError_ = "refusing to clean up LiveJam dir outside temp area";
        return false;
    }

    // Remove the directory tree.  Tolerant of partially rendered files
    // (remove_all handles individual file failures gracefully).
    std::uintmax_t removed = 0;
    std::filesystem::remove_all(sessionDir_, ec);

    if (ec && std::filesystem::exists(sessionDir_, ec))
    {
        lastError_ = "partial failure during LiveJam cleanup: " + ec.message();
        return false;
    }

    (void) removed;  // count informational; not critical
    sessionDir_.clear();
    return true;
}

// ---------------------------------------------------------------------------
// LiveJamSessionManager::cleanupStaleSessions
// ---------------------------------------------------------------------------

std::size_t LiveJamSessionManager::cleanupStaleSessions()
{
    std::size_t removed   = 0;
    const auto  tempBase  = platformTempDir();

    std::error_code ec;
    if (!std::filesystem::exists(tempBase, ec) || ec)
        return 0;

    for (const auto& entry : std::filesystem::directory_iterator(tempBase, ec))
    {
        if (ec) break;

        std::error_code ec2;
        if (!entry.is_directory(ec2) || ec2)
            continue;

        const auto& dirPath = entry.path();
        if (extractPidFromLiveJamDir(dirPath) < 0)
            continue;  // not a LiveJam session dir — skip

        // Only remove if the owning PID is no longer alive.
        const pid_t pid = extractPidFromLiveJamDir(dirPath);
        if (pid > 0 && isProcessAlive(pid))
            continue;  // owner still alive — skip (active session)

        std::filesystem::remove_all(dirPath, ec);
        if (!ec)
            ++removed;
    }

    return removed;
}

} // namespace hathor
