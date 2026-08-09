// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_bake_to_song.cpp — Tests for B8-K6 "Bake to Song" components.
 *
 * Tests the JUCE-free portions of the bake pipeline:
 *   - AssetTarget (B8-K1 §4, §5) — target representation, naming, serialization.
 *   - AssetPathResolver (B8-K1 §5) — path resolution for Studio and LiveJam.
 *   - LiveJamSessionManager (B8-K1 §8, §9) — session temp dir lifecycle.
 *
 * Tests that require JUCE (BakeTargetDialog, BakeProgressDialog, BakeOrchestrator)
 * are deferred to the UI integration test suite.
 *
 * Requirements: B8-K6, B8-K1
 */

#include <catch2/catch_test_macros.hpp>

#include "AssetTarget.hpp"
#include "AssetPathResolver.hpp"
#include "LiveJamSessionManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>

// ---------------------------------------------------------------------------
// AssetTarget — B8-K1 §4: target representation
// ---------------------------------------------------------------------------

TEST_CASE("AssetTarget defaults to Studio", "[bake][b8-k1]")
{
    REQUIRE(hathor::kDefaultAssetTarget == hathor::AssetTarget::Studio);
}

TEST_CASE("AssetTarget toString round-trips", "[bake][b8-k1]")
{
    REQUIRE(std::string(hathor::toString(hathor::AssetTarget::Studio)) == "studio");
    REQUIRE(std::string(hathor::toString(hathor::AssetTarget::LiveJam)) == "live_jam");
}

TEST_CASE("parseAssetTarget accepts all recognised spellings", "[bake][b8-k1]")
{
    hathor::AssetTarget out;

    REQUIRE(hathor::parseAssetTarget("studio", out));
    REQUIRE(out == hathor::AssetTarget::Studio);

    REQUIRE(hathor::parseAssetTarget("live_jam", out));
    REQUIRE(out == hathor::AssetTarget::LiveJam);

    REQUIRE(hathor::parseAssetTarget("live-jam", out));
    REQUIRE(out == hathor::AssetTarget::LiveJam);

    REQUIRE(hathor::parseAssetTarget("livejam", out));
    REQUIRE(out == hathor::AssetTarget::LiveJam);
}

TEST_CASE("parseAssetTarget rejects unknown strings", "[bake][b8-k1]")
{
    hathor::AssetTarget out;
    REQUIRE_FALSE(hathor::parseAssetTarget("live", out));
    REQUIRE_FALSE(hathor::parseAssetTarget("LIVE_JAM", out));
    REQUIRE_FALSE(hathor::parseAssetTarget("", out));
    REQUIRE_FALSE(hathor::parseAssetTarget("studio ", out));
    REQUIRE_FALSE(hathor::parseAssetTarget(" studio", out));
}

// ---------------------------------------------------------------------------
// sanitizeAssetName — B8-K1 §5: name sanitisation
// ---------------------------------------------------------------------------

TEST_CASE("sanitizeAssetName strips path separators", "[bake][b8-k1][sanitize]")
{
    REQUIRE(hathor::sanitizeAssetName("../etc/passwd") == "_etc_passwd");
    REQUIRE(hathor::sanitizeAssetName("sub/dir/kick") == "sub_dir_kick");
    // Backslashes in the middle create _ separators; dots after a separator
    // are not "leading" (out is already non-empty).
    REQUIRE(hathor::sanitizeAssetName("..\\..\\windows\\system32") == "_.._windows_system32");
    REQUIRE(hathor::sanitizeAssetName("acid_bass") == "acid_bass");
}

TEST_CASE("sanitizeAssetName handles empty input", "[bake][b8-k1][sanitize]")
{
    REQUIRE(hathor::sanitizeAssetName("") == "unnamed");
}

TEST_CASE("sanitizeAssetName strips leading dots", "[bake][b8-k1][sanitize]")
{
    REQUIRE(hathor::sanitizeAssetName(".hathor_assets") == "hathor_assets");
    REQUIRE(hathor::sanitizeAssetName("...hidden") == "hidden");
}

TEST_CASE("sanitizeAssetName collapses consecutive separators", "[bake][b8-k1][sanitize]")
{
    REQUIRE(hathor::sanitizeAssetName("a//b\\\\c") == "a_b_c");
    REQUIRE(hathor::sanitizeAssetName("a///b") == "a_b");
}

TEST_CASE("sanitizeAssetName preserves NUL replacement", "[bake][b8-k1][sanitize]")
{
    const std::string withNul = std::string("a\0b", 3);
    REQUIRE(hathor::sanitizeAssetName(withNul) == "a_b");
}

// ---------------------------------------------------------------------------
// AssetPathResolver — B8-K1 §5: path resolution
// ---------------------------------------------------------------------------

TEST_CASE("AssetPathResolver resolves Studio path", "[bake][b8-k1][resolver]")
{
    const std::filesystem::path projectDir = std::filesystem::temp_directory_path() / "hathor-test-studio";

    // Clean up any previous test artefact.
    std::error_code ec;
    std::filesystem::remove_all(projectDir, ec);

    hathor::AssetPathResolver resolver(projectDir);
    auto result = resolver.resolve(hathor::AssetTarget::Studio, "acid_bass");

    REQUIRE(result.ok);
    REQUIRE(result.path.has_filename());
    REQUIRE(result.path.filename() == "acid_bass.wav");

    // The Studio instruments dir should have been created.
    REQUIRE(std::filesystem::exists(resolver.studioInstrumentsDir()));

    // Cleanup.
    std::filesystem::remove_all(projectDir, ec);
}

TEST_CASE("AssetPathResolver resolves LiveJam path with session dir", "[bake][b8-k1][resolver]")
{
    const std::filesystem::path projectDir = std::filesystem::temp_directory_path() / "hathor-test-livejam";

    std::error_code ec;
    std::filesystem::remove_all(projectDir, ec);

    const std::filesystem::path sessionDir = projectDir / "hathor_live_jam_1234_0";

    hathor::AssetPathResolver resolver(projectDir);
    auto result = resolver.resolve(hathor::AssetTarget::LiveJam, "riser", sessionDir);

    REQUIRE(result.ok);
    REQUIRE(result.path.has_filename());
    REQUIRE(result.path.filename() == "riser.wav");
    REQUIRE(result.path.parent_path() == sessionDir);
}

TEST_CASE("AssetPathResolver rejects LiveJam without session dir", "[bake][b8-k1][resolver]")
{
    const std::filesystem::path projectDir = std::filesystem::temp_directory_path() / "hathor-test-no-session";

    std::error_code ec;
    std::filesystem::remove_all(projectDir, ec);

    hathor::AssetPathResolver resolver(projectDir);
    auto result = resolver.resolve(hathor::AssetTarget::LiveJam, "kick", {});

    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.error.empty());
}

TEST_CASE("AssetPathResolver sanitises instrument names", "[bake][b8-k1][resolver]")
{
    const std::filesystem::path projectDir = std::filesystem::temp_directory_path() / "hathor-test-sanitise";

    std::error_code ec;
    std::filesystem::remove_all(projectDir, ec);

    hathor::AssetPathResolver resolver(projectDir);

    // Path traversal in the name must be neutralised.
    auto result = resolver.resolve(hathor::AssetTarget::Studio, "../evil");
    REQUIRE(result.ok);
    REQUIRE(result.path.filename() == "_evil.wav");

    // Cleanup.
    std::filesystem::remove_all(projectDir, ec);
}

TEST_CASE("AssetPathResolver isStudioPath distinguishes Studio vs external paths", "[bake][b8-k1][resolver]")
{
    const std::filesystem::path projectDir = std::filesystem::temp_directory_path() / "hathor-test-isstudio";

    std::error_code ec;
    std::filesystem::remove_all(projectDir, ec);

    hathor::AssetPathResolver resolver(projectDir);
    auto result = resolver.resolve(hathor::AssetTarget::Studio, "bd");

    REQUIRE(result.ok);
    REQUIRE(resolver.isStudioPath(result.path));

    // An unrelated path should not be a Studio path.
    const std::filesystem::path other = std::filesystem::temp_directory_path() / "other.wav";
    REQUIRE_FALSE(resolver.isStudioPath(other));

    // Cleanup.
    std::filesystem::remove_all(projectDir, ec);
}

// ---------------------------------------------------------------------------
// LiveJamSessionManager — B8-K1 §8, §9: session lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("LiveJamSessionManager initialises and creates a temp dir", "[bake][b8-k1][livejam]")
{
    hathor::LiveJamSessionManager mgr;

    REQUIRE_FALSE(mgr.isInitialised());

    bool ok = mgr.initialise();
    REQUIRE(ok);
    REQUIRE(mgr.isInitialised());

    const auto dir = mgr.sessionDir();
    REQUIRE_FALSE(dir.empty());
    REQUIRE(std::filesystem::exists(dir));

    // Cleanup should succeed.
    REQUIRE(mgr.cleanup());
    REQUIRE_FALSE(std::filesystem::exists(dir));
}

TEST_CASE("LiveJamSessionManager cleanup is idempotent", "[bake][b8-k1][livejam]")
{
    hathor::LiveJamSessionManager mgr;
    REQUIRE(mgr.initialise());

    REQUIRE(mgr.cleanup());
    // Second cleanup should also succeed (no-op).
    REQUIRE(mgr.cleanup());
}

TEST_CASE("LiveJamSessionManager never touches Studio assets on cleanup", "[bake][b8-k1][livejam]")
{
    const std::filesystem::path projectDir = std::filesystem::temp_directory_path() / "hathor-test-lj-studio-safety";

    std::error_code ec;
    std::filesystem::remove_all(projectDir, ec);

    // Create a fake Studio asset that must survive cleanup.
    hathor::AssetPathResolver resolver(projectDir);
    auto studioResult = resolver.resolve(hathor::AssetTarget::Studio, "survivor");
    REQUIRE(studioResult.ok);
    std::ofstream(studioResult.path, std::ios::binary) << "dummy";

    // Initialise LiveJam session and write a fake asset.
    hathor::LiveJamSessionManager mgr;
    REQUIRE(mgr.initialise());
    const auto sessionDir = mgr.sessionDir();

    auto liveResult = resolver.resolve(hathor::AssetTarget::LiveJam, "ephemeral", sessionDir);
    REQUIRE(liveResult.ok);
    std::ofstream(liveResult.path, std::ios::binary) << "dummy";

    // Cleanup LiveJam — Studio asset must survive.
    REQUIRE(mgr.cleanup());

    REQUIRE_FALSE(std::filesystem::exists(liveResult.path));
    REQUIRE(std::filesystem::exists(studioResult.path));

    // Cleanup.
    std::filesystem::remove_all(projectDir, ec);
}

// ---------------------------------------------------------------------------
// LiveJamSession cleanup stale sessions — B8-K1 §9
// ---------------------------------------------------------------------------

TEST_CASE("LiveJamSessionManager::cleanupStaleSessions is safe to call", "[bake][b8-k1][livejam]")
{
    // Best-effort cleanup of stale sessions.  This should never throw or crash,
    // regardless of whether any stale directories exist.
    std::size_t removed = hathor::LiveJamSessionManager::cleanupStaleSessions();
    // Removed count is non-negative (it's a size_t, so just verify it ran).
    (void)removed;
    SUCCEED();
}
