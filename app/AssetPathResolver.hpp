// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * AssetPathResolver.hpp — centralized target path resolution (B8-K1 §5).
 *
 * B8-K1 owns target path resolution.  There is exactly ONE well-defined
 * mechanism to turn (AssetTarget, instrumentName, projectDir) into a concrete
 * .wav path.  Path construction is never duplicated throughout the renderer.
 *
 *   Studio   → <project>/.hathor_assets/chuck_instruments/<name>.wav
 *   LiveJam  → <session-temp>/hathor_live_jam_<pid>_<seq>/<name>.wav
 *
 * The resolver also ensures the required directory exists before B8-K2 writes
 * the audio, and handles name sanitisation (no path traversal).
 *
 * JUCE-free: uses std::filesystem only, so it links by both the engine
 * (JUCE-free) and the UI (JUCE-dependent) layers.
 */

#pragma once

#include "AssetTarget.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace hathor {

// ---------------------------------------------------------------------------
// AssetPathResolver — single source of truth for .wav destination paths
// ---------------------------------------------------------------------------

/// Resolves the concrete filesystem path for a ChucK-rendered .wav asset
/// based on the selected AssetTarget.
///
/// Usage (typical B8-K2 bake flow):
///
///     AssetPathResolver resolver(projectDir);
///     auto result = resolver.resolve(AssetTarget::Studio, "acid_bass");
///     if (!result.ok) { /* handle error */ }
///     // ... B8-K2 writes to result.path ...
///
/// For LiveJam, the same resolver instance (or a shared LiveJamSessionManager)
/// hands out paths within a single session-unique temp directory.
class AssetPathResolver {
public:
    /// Result of path resolution.
    struct ResolveResult {
        bool        ok        = false;
        std::filesystem::path path;
        std::string           error;   // populated when ok == false
    };

    /// Construct a resolver bound to a project directory.
    ///
    /// @param projectDir  The Hathor project root.  Studio assets are written
    ///                    under <projectDir>/.hathor_assets/chuck_instruments/.
    ///                    The directory does not need to exist yet; resolvePath()
    ///                    will create it.
    explicit AssetPathResolver(std::filesystem::path projectDir)
        : projectDir_(std::move(projectDir)) {}

    /// Default constructor — projectDir must be set later via setProjectDir().
    AssetPathResolver() = default;

    /// Set / change the project directory.
    void setProjectDir(std::filesystem::path dir) { projectDir_ = std::move(dir); }

    /// The current project directory.
    const std::filesystem::path& projectDir() const noexcept { return projectDir_; }

    // -----------------------------------------------------------------------
    // Studio target
    // -----------------------------------------------------------------------

    /// The canonical Studio instruments subdirectory.
    ///   <projectDir>/.hathor_assets/chuck_instruments
    std::filesystem::path studioInstrumentsDir() const
    {
        return projectDir_ / kStudioAssetsDir / kInstrumentsSubdir;
    }

    /// Resolve the Studio path for @p name.
    /// Creates the directory if it does not exist.
    /// Returns ok=true with the path, or ok=false with an error message.
    ResolveResult resolveStudio(std::string_view name) const
    {
        const std::string safe = sanitizeAssetName(name);
        const auto dir = studioInstrumentsDir();

        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return {false, {}, "cannot create Studio instruments dir: " + ec.message()};

        const auto path = dir / (safe + ".wav");
        return {true, path, {}};
    }

    // -----------------------------------------------------------------------
    // LiveJam target
    // -----------------------------------------------------------------------

    /// Resolve the LiveJam path for @p name.
    /// Uses the session temp directory created by LiveJamSessionManager.
    /// @param sessionTempDir  The session-unique temp directory (set by
    ///                        LiveJamSessionManager before resolving any path).
    ResolveResult resolveLiveJam(std::string_view name,
                                 const std::filesystem::path& sessionTempDir) const
    {
        if (sessionTempDir.empty())
            return {false, {}, "LiveJam session temp directory not initialised"};

        const std::string safe = sanitizeAssetName(name);

        std::error_code ec;
        std::filesystem::create_directories(sessionTempDir, ec);
        if (ec)
            return {false, {}, "cannot create LiveJam temp dir: " + ec.message()};

        const auto path = sessionTempDir / (safe + ".wav");
        return {true, path, {}};
    }

    // -----------------------------------------------------------------------
    // Unified entry point — dispatches on AssetTarget
    // -----------------------------------------------------------------------

    /// Resolve a path for the given target.  For LiveJam, @p liveJamDir must
    /// be non-empty (see LiveJamSessionManager::sessionDir()).
    /// For Studio, liveJamDir is ignored.
    ResolveResult resolve(AssetTarget target,
                          std::string_view name,
                          const std::filesystem::path& liveJamDir = {}) const
    {
        if (target == AssetTarget::Studio)
            return resolveStudio(name);
        return resolveLiveJam(name, liveJamDir);
    }

    /// Check whether @p path is inside the Studio permanent asset directory.
    /// Used to guarantee that cleanup never touches Studio assets.
    bool isStudioPath(const std::filesystem::path& path) const noexcept
    {
        std::error_code ec;
        const auto studioDir = std::filesystem::weakly_canonical(studioInstrumentsDir(), ec);
        if (ec) return false;

        const auto canonPath = std::filesystem::weakly_canonical(path, ec);
        if (ec) return false;

        const auto rel = std::filesystem::relative(canonPath, studioDir, ec);
        if (ec) return false;

        const std::string relStr = rel.string();
        return relStr != "." && relStr.find("..") == std::string::npos;
    }

    /// Check whether @p path is inside the given LiveJam temp directory.
    bool isLiveJamPath(const std::filesystem::path& path,
                       const std::filesystem::path& liveJamDir) const noexcept
    {
        std::error_code ec;
        const auto canonJam = std::filesystem::weakly_canonical(liveJamDir, ec);
        if (ec) return false;

        const auto canonPath = std::filesystem::weakly_canonical(path, ec);
        if (ec) return false;

        const auto rel = std::filesystem::relative(canonPath, canonJam, ec);
        if (ec) return false;

        const std::string relStr = rel.string();
        // If relative path starts with "..", it escaped the jam dir.
        return relStr != "." && relStr.find("..") == std::string::npos;
    }

private:
    std::filesystem::path projectDir_;

    static constexpr const char* kStudioAssetsDir    = ".hathor_assets";
    static constexpr const char* kInstrumentsSubdir  = "chuck_instruments";
};

} // namespace hathor
