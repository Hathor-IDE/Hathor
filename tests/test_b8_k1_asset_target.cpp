// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_b8_k1_asset_target.cpp — tests for B8-K1 Studio vs Live Jam asset target.
 *
 * Tests:
 *   - Target representation (AssetTarget enum values, string serialization)
 *   - Default behavior (Studio is the default)
 *   - Studio path resolution: .hathor_assets/chuck_instruments/<name>.wav
 *   - LiveJam path resolution: session temp directory
 *   - Name sanitization / path traversal prevention
 *   - Filename collisions
 *   - Studio persistence across simulated close/reopen
 *   - LiveJam session cleanup isolation (never removes Studio assets)
 *   - LiveJam stale session cleanup
 *
 * JUCE-free: uses std::filesystem + Catch2 only.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AssetTarget.hpp"
#include "AssetPathResolver.hpp"
#include "LiveJamSessionManager.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

using hathor::AssetTarget;
using hathor::AssetPathResolver;
using hathor::LiveJamSessionManager;
using hathor::kDefaultAssetTarget;
using hathor::sanitizeAssetName;
using hathor::toString;
using hathor::parseAssetTarget;

// ---------------------------------------------------------------------------
// Test fixture: creates a temporary project directory that is cleaned up
// after each test.
// ---------------------------------------------------------------------------

struct ProjectDirFixture {
    fs::path projectDir;
    fs::path tempBase;

    ProjectDirFixture()
    {
        tempBase = fs::temp_directory_path() / ("hathor_b8_k1_test_" + std::to_string(::getpid()));
        fs::create_directories(tempBase, ec);
        projectDir = tempBase / "test_project";
        fs::create_directories(projectDir, ec);
    }

    ~ProjectDirFixture()
    {
        std::error_code ec;
        fs::remove_all(tempBase, ec);
    }

    std::error_code ec;
};

// ---------------------------------------------------------------------------
// 1. Target representation
// ---------------------------------------------------------------------------

TEST_CASE("AssetTarget enum has correct values", "[b8-k1][target]")
{
    REQUIRE(static_cast<int>(AssetTarget::Studio) == 0);
    REQUIRE(static_cast<int>(AssetTarget::LiveJam) == 1);
}

TEST_CASE("AssetTarget toString produces correct labels", "[b8-k1][target]")
{
    REQUIRE(std::string(toString(AssetTarget::Studio)) == "studio");
    REQUIRE(std::string(toString(AssetTarget::LiveJam)) == "live_jam");
}

TEST_CASE("AssetTarget parseAssetTarget round-trips", "[b8-k1][target]")
{
    AssetTarget t{};

    REQUIRE(parseAssetTarget("studio", t));
    REQUIRE(t == AssetTarget::Studio);

    REQUIRE(parseAssetTarget("live_jam", t));
    REQUIRE(t == AssetTarget::LiveJam);

    REQUIRE(parseAssetTarget("live-jam", t));
    REQUIRE(t == AssetTarget::LiveJam);

    REQUIRE(parseAssetTarget("livejam", t));
    REQUIRE(t == AssetTarget::LiveJam);

    // Unknown targets should not parse.
    REQUIRE_FALSE(parseAssetTarget("bogus", t));
    REQUIRE_FALSE(parseAssetTarget("", t));
}

// ---------------------------------------------------------------------------
// 2. Default behavior — Studio is always the default
// ---------------------------------------------------------------------------

TEST_CASE("Studio is the default target", "[b8-k1][default]")
{
    REQUIRE(kDefaultAssetTarget == AssetTarget::Studio);
}

// ---------------------------------------------------------------------------
// 3. Studio path resolution
// ---------------------------------------------------------------------------

TEST_CASE("Studio resolves to .hathor_assets/chuck_instruments/<name>.wav", "[b8-k1][studio][path]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    auto result = resolver.resolveStudio("acid_bass");

    REQUIRE(result.ok);
    REQUIRE(result.path.has_filename());

    const auto expected = fx.projectDir / ".hathor_assets" / "chuck_instruments" / "acid_bass.wav";
    REQUIRE(result.path == expected);
}

TEST_CASE("Studio creates the instruments directory", "[b8-k1][studio][path]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    auto result = resolver.resolveStudio("my_inst");

    REQUIRE(result.ok);
    REQUIRE(fs::exists(resolver.studioInstrumentsDir()));
    REQUIRE(fs::is_directory(resolver.studioInstrumentsDir()));
}

TEST_CASE("Studio path via resolve() dispatch", "[b8-k1][studio][path]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    auto result = resolver.resolve(AssetTarget::Studio, "lead_synth");

    REQUIRE(result.ok);
    const auto expected = fx.projectDir / ".hathor_assets" / "chuck_instruments" / "lead_synth.wav";
    REQUIRE(result.path == expected);
}

// ---------------------------------------------------------------------------
// 4. LiveJam path resolution
// ---------------------------------------------------------------------------

TEST_CASE("LiveJam resolves to session temp directory", "[b8-k1][livejam][path]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    fs::path sessionDir = fx.tempBase / "hathor_live_jam_session_dir";
    fs::create_directories(sessionDir);

    auto result = resolver.resolveLiveJam("riser_01", sessionDir);

    REQUIRE(result.ok);
    REQUIRE(result.path.has_filename());
    REQUIRE(result.path.filename() == "riser_01.wav");
    REQUIRE(result.path.parent_path() == sessionDir);
}

TEST_CASE("LiveJam path via resolve() dispatch", "[b8-k1][livejam][path]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    fs::path sessionDir = fx.tempBase / "hathor_live_jam_session_dir";
    fs::create_directories(sessionDir);

    auto result = resolver.resolve(AssetTarget::LiveJam, "transition_01", sessionDir);

    REQUIRE(result.ok);
    REQUIRE(result.path == sessionDir / "transition_01.wav");
}

TEST_CASE("LiveJam resolve without session dir fails", "[b8-k1][livejam][path]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    auto result = resolver.resolveLiveJam("riser", {});

    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.error.empty());
}

TEST_CASE("LiveJam path is outside Studio asset directory", "[b8-k1][livejam][path]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    fs::path sessionDir = fs::temp_directory_path() / "hathor_test_livejam_outside";
    fs::create_directories(sessionDir);

    auto liveResult = resolver.resolveLiveJam("test", sessionDir);
    REQUIRE(liveResult.ok);

    const auto studioDir = resolver.studioInstrumentsDir();

    // The LiveJam path must NOT be inside the Studio instruments dir.
    auto rel = fs::relative(liveResult.path, studioDir);
    std::string relStr = rel.string();
    REQUIRE(relStr.find("..") != std::string::npos);

    fs::remove_all(sessionDir);
}

// ---------------------------------------------------------------------------
// 5. Name sanitization / path traversal prevention
// ---------------------------------------------------------------------------

TEST_CASE("sanitizeAssetName strips path separators", "[b8-k1][safety]")
{
    REQUIRE(sanitizeAssetName("../etc/passwd") == "___etc_passwd");
    REQUIRE(sanitizeAssetName("sub/dir/kick") == "sub_dir_kick");
    REQUIRE(sanitizeAssetName("a\\b\\c") == "a_b_c");
}

TEST_CASE("sanitizeAssetName strips leading dots", "[b8-k1][safety]")
{
    REQUIRE(sanitizeAssetName(".hidden") == "hidden");
    REQUIRE(sanitizeAssetName("...hidden") == "hidden");
    REQUIRE(sanitizeAssetName(".") == "unnamed");
    REQUIRE(sanitizeAssetName("..") == "unnamed");
}

TEST_CASE("sanitizeAssetName handles empty input", "[b8-k1][safety]")
{
    REQUIRE(sanitizeAssetName("") == "unnamed");
    REQUIRE(sanitizeAssetName("   ") == "   ");  // spaces are valid
    REQUIRE(sanitizeAssetName(std::string_view{}) == "unnamed");
}

TEST_CASE("sanitizeAssetName truncates long names", "[b8-k1][safety]")
{
    std::string longName(300, 'a');
    std::string result = sanitizeAssetName(longName);
    REQUIRE(result.size() <= 200);
    REQUIRE(result.size() == 200);
}

TEST_CASE("sanitizeAssetName handles NUL and mixed traversal", "[b8-k1][safety]")
{
    std::string input = "safe\0../unsafe";
    std::string result = sanitizeAssetName(input);
    REQUIRE(result.find('/') == std::string::npos);
    REQUIRE(result.find('\\') == std::string::npos);
    REQUIRE(result.find('\0') == std::string::npos);
}

TEST_CASE("Studio path with traversal name stays in instruments dir", "[b8-k1][safety][studio]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    auto result = resolver.resolveStudio("../escape_attempt");

    REQUIRE(result.ok);
    const auto parent = result.path.parent_path();
    REQUIRE(parent == resolver.studioInstrumentsDir());
}

TEST_CASE("LiveJam path with traversal name stays in session dir", "[b8-k1][safety][livejam]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    fs::path sessionDir = fx.tempBase / "hathor_live_jam_traverse";
    fs::create_directories(sessionDir);

    auto result = resolver.resolveLiveJam("../../escape", sessionDir);

    REQUIRE(result.ok);
    REQUIRE(result.path.parent_path() == sessionDir);

    fs::remove_all(sessionDir);
}

// ---------------------------------------------------------------------------
// 6. Filename collisions
// ---------------------------------------------------------------------------

TEST_CASE("Studio repeated render produces same path (collision = overwrite)", "[b8-k1][collision][studio]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    auto r1 = resolver.resolveStudio("acid_bass");
    auto r2 = resolver.resolveStudio("acid_bass");

    REQUIRE(r1.ok);
    REQUIRE(r2.ok);
    REQUIRE(r1.path == r2.path);
    // Both point to the same file — the bake pipeline replaces/overwrites.
}

TEST_CASE("Studio different names produce different paths", "[b8-k1][collision][studio]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    auto r1 = resolver.resolveStudio("acid_bass");
    auto r2 = resolver.resolveStudio("deep_bass");

    REQUIRE(r1.ok);
    REQUIRE(r2.ok);
    REQUIRE(r1.path != r2.path);
}

TEST_CASE("LiveJam different names produce different paths", "[b8-k1][collision][livejam]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    fs::path sessionDir = fx.tempBase / "hathor_live_jam_collide";
    fs::create_directories(sessionDir);

    auto r1 = resolver.resolveLiveJam("riser_01", sessionDir);
    auto r2 = resolver.resolveLiveJam("riser_02", sessionDir);

    REQUIRE(r1.ok);
    REQUIRE(r2.ok);
    REQUIRE(r1.path != r2.path);

    fs::remove_all(sessionDir);
}

// ---------------------------------------------------------------------------
// 7. Studio persistence — survives simulated close/reopen
// ---------------------------------------------------------------------------

TEST_CASE("Studio asset survives project close/reopen simulation", "[b8-k1][studio][persistence]")
{
    ProjectDirFixture fx;

    // Phase 1: render (write a real wav-like file)
    {
        AssetPathResolver resolver(fx.projectDir);
        auto result = resolver.resolveStudio("persistent_pad");

        REQUIRE(result.ok);

        // Simulate B8-K2 writing WAV data.
        std::ofstream out(result.path, std::ios::binary);
        out << "RIFFfakeWAVEfmt chunkdata";
        out.close();

        REQUIRE(fs::exists(result.path));
    }

    // Phase 2: simulate closing Hathor and reopening the project.
    // A new project dir is NOT needed — the asset is on disk at a
    // deterministic path.  We just re-resolve and verify.
    {
        AssetPathResolver resolver(fx.projectDir);
        auto result = resolver.resolveStudio("persistent_pad");

        REQUIRE(result.ok);
        REQUIRE(result.path ==
                fx.projectDir / ".hathor_assets" / "chuck_instruments" / "persistent_pad.wav");
        REQUIRE(fs::exists(result.path));

        // Verify content is intact.
        std::ifstream in(result.path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)),
                                                      std::istreambuf_iterator<char>());
        in.close();
        REQUIRE(content == "RIFFfakeWAVEfmt chunkdata");
    }
}

// ---------------------------------------------------------------------------
// 8. LiveJam session lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("LiveJamSessionManager creates a session temp dir", "[b8-k1][livejam][lifecycle]")
{
    LiveJamSessionManager mgr;
    REQUIRE(mgr.initialise());
    REQUIRE(mgr.isInitialised());
    REQUIRE_FALSE(mgr.sessionDir().empty());
    REQUIRE(fs::exists(mgr.sessionDir()));
    REQUIRE(fs::is_directory(mgr.sessionDir()));
}

TEST_CASE("LiveJamSessionManager cleanup removes session dir", "[b8-k1][livejam][lifecycle]")
{
    LiveJamSessionManager mgr;
    REQUIRE(mgr.initialise());

    fs::path dir = mgr.sessionDir();
    REQUIRE(fs::exists(dir));

    // Create a file inside the session dir.
    fs::path testFile = dir / "test.wav";
    std::ofstream out(testFile);
    out << "test data";
    out.close();
    REQUIRE(fs::exists(testFile));

    REQUIRE(mgr.cleanup());
    REQUIRE_FALSE(fs::exists(dir));
}

TEST_CASE("LiveJamSessionManager cleanup is idempotent", "[b8-k1][livejam][lifecycle]")
{
    LiveJamSessionManager mgr;
    REQUIRE(mgr.initialise());
    fs::path dir = mgr.sessionDir();

    REQUIRE(mgr.cleanup());
    REQUIRE_FALSE(fs::exists(dir));

    // Calling cleanup again should succeed (no error, no crash).
    REQUIRE(mgr.cleanup());
}

TEST_CASE("LiveJamSessionManager cleanup tolerates missing dir", "[b8-k1][livejam][lifecycle]")
{
    LiveJamSessionManager mgr;
    // Don't initialise — sessionDir() should be empty.
    REQUIRE_FALSE(mgr.isInitialised());

    // cleanup() should succeed (nothing to do).
    REQUIRE(mgr.cleanup());
}

TEST_CASE("LiveJamSessionManager cleanup tolerates partial files", "[b8-k1][livejam][lifecycle]")
{
    LiveJamSessionManager mgr;
    REQUIRE(mgr.initialise());

    fs::path dir = mgr.sessionDir();
    fs::path testFile = dir / "partial.wav";

    // Create an empty/partial file.
    std::ofstream out(testFile);
    out.close();

    REQUIRE(mgr.cleanup());
    REQUIRE_FALSE(fs::exists(dir));
}

TEST_CASE("LiveJamSessionManager destructor auto-cleans", "[b8-k1][livejam][lifecycle]")
{
    fs::path capturedDir;
    {
        LiveJamSessionManager mgr;
        REQUIRE(mgr.initialise());
        capturedDir = mgr.sessionDir();
        REQUIRE(fs::exists(capturedDir));
    }
    // After the scope, the destructor should have cleaned up.
    REQUIRE_FALSE(fs::exists(capturedDir));
}

// ---------------------------------------------------------------------------
// 9. Cleanup isolation — Studio assets NEVER removed
// ---------------------------------------------------------------------------

TEST_CASE("LiveJam cleanup does not touch Studio assets", "[b8-k1][livejam][isolation]")
{
    ProjectDirFixture fx;

    // Create a Studio asset that should survive.
    AssetPathResolver resolver(fx.projectDir);
    auto studioResult = resolver.resolveStudio("studio_kick");
    REQUIRE(studioResult.ok);

    std::ofstream out(studioResult.path, std::ios::binary);
    out << "studio data";
    out.close();
    REQUIRE(fs::exists(studioResult.path));

    // Create a LiveJam session and asset.
    LiveJamSessionManager mgr;
    REQUIRE(mgr.initialise());

    auto liveResult = resolver.resolveLiveJam("live_snare", mgr.sessionDir());
    REQUIRE(liveResult.ok);

    std::ofstream out2(liveResult.path, std::ios::binary);
    out2 << "live data";
    out2.close();
    REQUIRE(fs::exists(liveResult.path));

    // Clean up LiveJam.
    REQUIRE(mgr.cleanup());

    // Studio asset must still exist.
    REQUIRE(fs::exists(studioResult.path));

    // LiveJam asset should be gone.
    REQUIRE_FALSE(fs::exists(liveResult.path));
}

TEST_CASE("isStudioPath correctly identifies Studio assets", "[b8-k1][studio][isolation]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    auto studioResult = resolver.resolveStudio("test_kick");
    REQUIRE(studioResult.ok);

    REQUIRE(resolver.isStudioPath(studioResult.path));

    // A path outside the Studio dir should not be identified as Studio.
    fs::path outsidePath = fx.tempBase / "outside.wav";
    REQUIRE_FALSE(resolver.isStudioPath(outsidePath));
}

TEST_CASE("isLiveJamPath correctly identifies LiveJam assets", "[b8-k1][livejam][isolation]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    fs::path sessionDir = fx.tempBase / "hathor_live_jam_isolation_test";
    fs::create_directories(sessionDir);

    auto liveResult = resolver.resolveLiveJam("test_fx", sessionDir);
    REQUIRE(liveResult.ok);

    REQUIRE(resolver.isLiveJamPath(liveResult.path, sessionDir));

    // A path outside the session dir should not be identified as LiveJam.
    fs::path outsidePath = fx.tempBase / "outside.wav";
    REQUIRE_FALSE(resolver.isLiveJamPath(outsidePath, sessionDir));

    fs::remove_all(sessionDir);
}

// ---------------------------------------------------------------------------
// 10. Stale session cleanup
// ---------------------------------------------------------------------------

TEST_CASE("cleanupStaleSessions removes stale LiveJam dirs", "[b8-k1][livejam][stale]")
{
    // Create a fake stale LiveJam directory with a non-existent PID.
    // We use PID 99999999 which is almost certainly not running.
    fs::path tempBase = fs::temp_directory_path();
    fs::path staleDir = tempBase / "hathor_live_jam_99999999_0_dir";

    // Clean up any leftover from a previous run.
    std::error_code ec;
    fs::remove_all(staleDir, ec);

    fs::create_directories(staleDir, ec);
    REQUIRE(fs::exists(staleDir));

    fs::path sentinelFile = staleDir / "sentinel.txt";
    std::ofstream out(sentinelFile);
    out << "stale data";
    out.close();

    // Run stale cleanup.
    std::size_t removed = LiveJamSessionManager::cleanupStaleSessions();

    REQUIRE_FALSE(fs::exists(staleDir));
    REQUIRE(removed >= 1);
}

// ---------------------------------------------------------------------------
// 11. Path construction is centralised — resolve() dispatches correctly
// ---------------------------------------------------------------------------

TEST_CASE("resolve() dispatches Studio to permanent dir", "[b8-k1][centralized]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    auto studioResult = resolver.resolve(AssetTarget::Studio, "instrument");
    auto directStudio = resolver.resolveStudio("instrument");

    REQUIRE(studioResult.ok);
    REQUIRE(directStudio.ok);
    REQUIRE(studioResult.path == directStudio.path);
}

TEST_CASE("resolve() dispatches LiveJam to session dir", "[b8-k1][centralized]")
{
    ProjectDirFixture fx;
    AssetPathResolver resolver(fx.projectDir);

    fs::path sessionDir = fx.tempBase / "hathor_live_jam_dispatch_test";
    fs::create_directories(sessionDir);

    auto liveResult = resolver.resolve(AssetTarget::LiveJam, "instrument", sessionDir);
    auto directLive = resolver.resolveLiveJam("instrument", sessionDir);

    REQUIRE(liveResult.ok);
    REQUIRE(directLive.ok);
    REQUIRE(liveResult.path == directLive.path);

    fs::remove_all(sessionDir);
}
