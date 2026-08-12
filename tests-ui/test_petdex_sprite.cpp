// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_petdex_sprite.cpp — Phase G / D2–D3 unit tests (JUCE-free).
 *
 * Covers:
 *   - PetdexFrameGrid: verified 8×9 / 192×208 geometry, frame rects, the
 *     verified 9-row state convention (idle…review), invalid-sheet handling.
 *   - PetdexAnimation: deterministic timing, state transitions, fallback to
 *     the idle state when an expected animation is unavailable.
 *   - PetdexWebpDecoder: lossless RGBA round-trip via libwebp, corrupt-input
 *     rejection (never throws, never crashes).
 *   - Frame slicing end-to-end: a synthetic 8×2 sheet is WebP-encoded, decoded,
 *     and every frame rectangle sampled against its expected colour.
 *   - PetdexResourceCache: per-pet sprite + D4 attribution snapshot storage,
 *     corrupt-cache detection, slug sanitisation.
 */

#include <catch2/catch_test_macros.hpp>

#include "PetdexFrameGrid.hpp"
#include "PetdexAnimation.hpp"
#include "PetdexWebpDecoder.hpp"
#include "PetdexResourceCache.hpp"
#include "PetdexAttribution.hpp"

#include <webp/encode.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace hathor::ui;

namespace {

/// Temporary directory unique to the calling test; removed on destruction.
struct TempDir
{
    fs::path path;

    TempDir()
    {
        const auto base = fs::temp_directory_path() / "hathor-petdex-test";
        std::error_code ec;
        fs::create_directories(base, ec);
        path = base / ("sprite-" + std::to_string(++counter));
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    static int counter;
};

int TempDir::counter = 0;

/// Encode unpremultiplied RGBA losslessly (returns {} on failure).
std::vector<std::uint8_t> encodeLosslessWebp(const std::vector<std::uint8_t>& rgba,
                                             int width, int height)
{
    std::vector<std::uint8_t> out;
    std::uint8_t* webp = nullptr;
    const std::size_t size = WebPEncodeLosslessRGBA(
        rgba.data(), width, height, width * 4, &webp);
    if (size > 0 && webp != nullptr)
        out.assign(webp, webp + size);
    WebPFree(webp);
    return out;
}

/// Build an 8×N sheet where every frame (row, col) is a solid, unique colour.
std::vector<std::uint8_t> buildTestSheet(int rows, std::uint8_t& unusedSeed)
{
    constexpr int kCols = PetdexFrameGrid::kConventionCols;
    const int width  = kCols * PetdexFrameGrid::kFrameWidth;
    const int height = rows * PetdexFrameGrid::kFrameHeight;

    std::vector<std::uint8_t> rgba(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0);

    for (int row = 0; row < rows; ++row)
    {
        for (int col = 0; col < kCols; ++col)
        {
            const int idx = row * kCols + col;
            const std::uint8_t r = static_cast<std::uint8_t>((idx * 37) % 256);
            const std::uint8_t g = static_cast<std::uint8_t>((idx * 71) % 256);
            const std::uint8_t b = static_cast<std::uint8_t>((idx * 113) % 256);

            for (int fy = 0; fy < PetdexFrameGrid::kFrameHeight; ++fy)
            {
                const int y = row * PetdexFrameGrid::kFrameHeight + fy;
                for (int fx = 0; fx < PetdexFrameGrid::kFrameWidth; ++fx)
                {
                    const int x = col * PetdexFrameGrid::kFrameWidth + fx;
                    const std::size_t i =
                        (static_cast<std::size_t>(y) * width + x) * 4u;
                    rgba[i + 0] = r;
                    rgba[i + 1] = g;
                    rgba[i + 2] = b;
                    rgba[i + 3] = 255;
                }
            }
        }
    }

    unusedSeed = 0;
    return rgba;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PetdexFrameGrid — verified geometry + state convention
// ---------------------------------------------------------------------------

TEST_CASE("Frame grid analyzes the verified 8x9 sheet geometry", "[petdex][sprite][grid]")
{
    // Live pet spritesheet: exactly 1536 x 1872 = 8 * 192 x 9 * 208.
    const auto grid = PetdexFrameGrid::analyze(1536, 1872);

    REQUIRE(grid.valid);
    REQUIRE(grid.error.empty());
    REQUIRE(grid.sheetWidth  == 1536);
    REQUIRE(grid.sheetHeight == 1872);
    REQUIRE(grid.cols == PetdexFrameGrid::kConventionCols);
    REQUIRE(grid.rows == 9);

    // All 9 convention states are available on a 9-row sheet.
    REQUIRE(grid.states.size() == 9);

    // Frame rectangles slice the verified grid correctly.
    const auto f00 = grid.frameRect(0, 0);
    REQUIRE(f00.x == 0);
    REQUIRE(f00.y == 0);
    REQUIRE(f00.w == PetdexFrameGrid::kFrameWidth);
    REQUIRE(f00.h == PetdexFrameGrid::kFrameHeight);

    const auto f87 = grid.frameRect(8, 7);
    REQUIRE(f87.x == 7 * PetdexFrameGrid::kFrameWidth);
    REQUIRE(f87.y == 8 * PetdexFrameGrid::kFrameHeight);
    REQUIRE(f87.w == PetdexFrameGrid::kFrameWidth);
    REQUIRE(f87.h == PetdexFrameGrid::kFrameHeight);
}

TEST_CASE("Frame grid exposes the verified state convention", "[petdex][sprite][grid]")
{
    const auto& states = PetdexFrameGrid::conventionStates();
    REQUIRE(states.size() == 9);

    // Verified against crafter-station/petdex src/lib/pet-states.ts (2026-08-12).
    REQUIRE(states[0].id == "idle");
    REQUIRE(states[0].row == 0);
    REQUIRE(states[0].frames == 6);
    REQUIRE(states[0].durationMs == 1100);

    REQUIRE(states[1].id == "running-right");
    REQUIRE(states[1].row == 1);
    REQUIRE(states[1].frames == 8);
    REQUIRE(states[1].durationMs == 1060);

    REQUIRE(states[2].id == "running-left");
    REQUIRE(states[2].row == 2);

    REQUIRE(states[3].id == "waving");
    REQUIRE(states[3].row == 3);
    REQUIRE(states[3].frames == 4);
    REQUIRE(states[3].durationMs == 700);

    REQUIRE(states[4].id == "jumping");
    REQUIRE(states[4].row == 4);
    REQUIRE(states[4].frames == 5);
    REQUIRE(states[4].durationMs == 840);

    REQUIRE(states[5].id == "failed");
    REQUIRE(states[5].row == 5);
    REQUIRE(states[5].frames == 8);
    REQUIRE(states[5].durationMs == 1220);

    REQUIRE(states[6].id == "waiting");
    REQUIRE(states[6].row == 6);
    REQUIRE(states[6].frames == 6);
    REQUIRE(states[6].durationMs == 1010);

    REQUIRE(states[7].id == "running");
    REQUIRE(states[7].row == 7);
    REQUIRE(states[7].frames == 6);
    REQUIRE(states[7].durationMs == 820);

    REQUIRE(states[8].id == "review");
    REQUIRE(states[8].row == 8);
    REQUIRE(states[8].frames == 6);
    REQUIRE(states[8].durationMs == 1030);

    // Lookup by id on a real grid.
    const auto grid = PetdexFrameGrid::analyze(1536, 1872);
    const auto* idle = grid.findState(PetdexFrameGrid::kDefaultStateId);
    REQUIRE(idle != nullptr);
    REQUIRE(idle->row == 0);
    REQUIRE(grid.findState("running") != nullptr);
    REQUIRE(grid.findState("no-such-state") == nullptr);
}

TEST_CASE("Frame grid handles non-convention sheets safely", "[petdex][sprite][grid]")
{
    SECTION("column count other than 8 is rejected")
    {
        // 2 columns x 2 rows — valid frame multiples but not the convention.
        const auto grid = PetdexFrameGrid::analyze(384, 416);
        REQUIRE_FALSE(grid.valid);
        REQUIRE(grid.error.find("columns") != std::string::npos);
    }

    SECTION("dimensions not a frame multiple are rejected")
    {
        const auto grid = PetdexFrameGrid::analyze(1000, 800);
        REQUIRE_FALSE(grid.valid);
        REQUIRE(grid.error.find("multiple") != std::string::npos);
    }

    SECTION("degenerate sheets are rejected")
    {
        REQUIRE_FALSE(PetdexFrameGrid::analyze(0, 0).valid);
        REQUIRE_FALSE(PetdexFrameGrid::analyze(-1, 10).valid);
    }

    SECTION("v2 sheet (extra rows) only advertises present rows")
    {
        // v2 layout is 8 columns x 11 rows (1536 x 2288).
        const auto grid = PetdexFrameGrid::analyze(1536, 2288);
        REQUIRE(grid.valid);
        REQUIRE(grid.rows == 11);
        REQUIRE(grid.states.size() == 9);   // rows 9-10 have no convention state
    }

    SECTION("short sheet only advertises its rows")
    {
        // 8 columns x 2 rows: only idle + running-right are usable.
        const auto grid = PetdexFrameGrid::analyze(1536, 416);
        REQUIRE(grid.valid);
        REQUIRE(grid.rows == 2);
        REQUIRE(grid.states.size() == 2);
        REQUIRE(grid.findState("idle") != nullptr);
        REQUIRE(grid.findState("running-right") != nullptr);
        REQUIRE(grid.findState("running") == nullptr);   // row 7 not present
    }
}

// ---------------------------------------------------------------------------
// PetdexAnimation — deterministic timing + state fallback
// ---------------------------------------------------------------------------

TEST_CASE("Animation timing is deterministic", "[petdex][sprite][animation]")
{
    const auto grid = PetdexFrameGrid::analyze(1536, 1872);
    REQUIRE(grid.valid);

    PetdexAnimation anim;
    anim.configure(grid);
    REQUIRE(anim.state() == PetdexFrameGrid::kDefaultStateId);
    REQUIRE(anim.currentStateRow() == 0);
    REQUIRE(anim.currentFrame() == 0);
    REQUIRE(anim.frameCount() == 6);

    // idle: 1100 ms / 6 frames → 183 ms per frame (integer division).
    constexpr int kPerFrameMs = 1100 / 6;
    REQUIRE(kPerFrameMs == 183);

    // A fresh advance sequence always produces the same frames (loops).
    std::vector<int> sequence;
    for (int i = 0; i < 6; ++i)
        sequence.push_back(anim.advance(kPerFrameMs));
    REQUIRE(sequence == std::vector<int>({ 1, 2, 3, 4, 5, 0 }));

    // State transition resets the clock to frame 0.
    anim.setState("running");
    REQUIRE(anim.state() == "running");
    REQUIRE(anim.currentStateRow() == 7);
    REQUIRE(anim.currentFrame() == 0);
    REQUIRE(anim.advance(136) == 1);   // running: 820 / 6 → 136 ms per frame

    // Transition back to idle resets again.
    anim.setState(PetdexFrameGrid::kDefaultStateId);
    REQUIRE(anim.currentFrame() == 0);
}

TEST_CASE("Animation falls back to a safe state when expected one is missing",
          "[petdex][sprite][animation]")
{
    const auto grid = PetdexFrameGrid::analyze(1536, 1872);

    PetdexAnimation anim;
    anim.configure(grid);

    SECTION("unknown state id collapses to idle")
    {
        anim.setState("teleporting");   // not part of the convention
        REQUIRE(anim.state() == PetdexFrameGrid::kDefaultStateId);
        REQUIRE(anim.currentStateRow() == 0);
        REQUIRE(anim.frameCount() == 6);
    }

    SECTION("state absent from a short sheet collapses to idle")
    {
        const auto shortGrid = PetdexFrameGrid::analyze(1536, 416);   // rows 0-1
        REQUIRE(shortGrid.valid);
        anim.configure(shortGrid);
        anim.setState("running");       // row 7 not on the sheet
        REQUIRE(anim.state() == PetdexFrameGrid::kDefaultStateId);
        REQUIRE(anim.currentStateRow() == 0);
    }

    SECTION("invalid grid leaves the animation inert")
    {
        anim.configure(PetdexFrameGrid::analyze(0, 0));
        anim.setState("idle");
        REQUIRE(anim.frameCount() == 0);
        REQUIRE(anim.currentStateRow() == 0);
        REQUIRE(anim.advance(1000) == 0);
        REQUIRE(anim.currentFrame() == 0);
    }

    SECTION("negative elapsed time is clamped (no time travel)")
    {
        anim.configure(grid);
        anim.advance(100);
        const int before = anim.currentFrame();
        anim.advance(-50);
        REQUIRE(anim.currentFrame() == before);
    }
}

// ---------------------------------------------------------------------------
// PetdexWebpDecoder — lossless round-trip + corrupt input
// ---------------------------------------------------------------------------

TEST_CASE("WebP decode round-trips lossless RGBA", "[petdex][sprite][webp]")
{
    constexpr int kWidth  = PetdexFrameGrid::kFrameWidth;    // 192
    constexpr int kHeight = PetdexFrameGrid::kFrameHeight;   // 208

    std::vector<std::uint8_t> rgba(
        static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight) * 4u);
    for (std::size_t i = 0; i < rgba.size(); i += 4)
    {
        rgba[i + 0] = static_cast<std::uint8_t>((i / 4) % 256);         // R
        rgba[i + 1] = static_cast<std::uint8_t>(((i / 4) / 256) % 256); // G
        rgba[i + 2] = static_cast<std::uint8_t>(i % 137);               // B
        rgba[i + 3] = 255;                                              // A
    }

    const auto webp = encodeLosslessWebp(rgba, kWidth, kHeight);
    REQUIRE_FALSE(webp.empty());

    const auto decoded = PetdexWebpDecoder::decode(webp.data(), webp.size());
    REQUIRE(decoded.ok);
    REQUIRE(decoded.error.empty());
    REQUIRE(decoded.width  == kWidth);
    REQUIRE(decoded.height == kHeight);
    REQUIRE(decoded.rgba.size() == rgba.size());

    // Lossless encoding: decoded pixels are byte-identical.
    REQUIRE(decoded.rgba == rgba);
}

TEST_CASE("WebP decoder rejects corrupt input without crashing", "[petdex][sprite][webp]")
{
    SECTION("empty input")
    {
        const auto d = PetdexWebpDecoder::decode(nullptr, 0);
        REQUIRE_FALSE(d.ok);
        REQUIRE_FALSE(d.error.empty());
    }

    SECTION("non-WebP bytes")
    {
        const char junk[] = "this is definitely not a webp image";
        const auto d = PetdexWebpDecoder::decode(
            reinterpret_cast<const std::uint8_t*>(junk), sizeof(junk) - 1);
        REQUIRE_FALSE(d.ok);
    }

    SECTION("truncated WebP")
    {
        constexpr int kSize = 64;
        std::vector<std::uint8_t> rgba(
            static_cast<std::size_t>(kSize) * kSize * 4u, 128);
        const auto webp = encodeLosslessWebp(rgba, kSize, kSize);
        REQUIRE_FALSE(webp.empty());

        const auto half = std::vector<std::uint8_t>(webp.begin(),
                                                    webp.begin() + webp.size() / 2);
        const auto d = PetdexWebpDecoder::decode(half.data(), half.size());
        REQUIRE_FALSE(d.ok);
    }
}

// ---------------------------------------------------------------------------
// Frame slicing end-to-end (real WebP encode → decode → slice)
// ---------------------------------------------------------------------------

TEST_CASE("Frame slicing matches the verified grid on decoded WebP",
          "[petdex][sprite][grid][webp]")
{
    // An 8-column x 2-row sheet (1536 x 416): every frame a solid colour.
    constexpr int kRows = 2;
    std::uint8_t seed = 0;
    const auto rgba = buildTestSheet(kRows, seed);
    REQUIRE_FALSE(rgba.empty());

    const auto webp = encodeLosslessWebp(rgba, 1536, 416);
    REQUIRE_FALSE(webp.empty());

    const auto decoded = PetdexWebpDecoder::decode(webp.data(), webp.size());
    REQUIRE(decoded.ok);
    REQUIRE(decoded.width  == 1536);
    REQUIRE(decoded.height == 416);

    const auto grid = PetdexFrameGrid::analyze(decoded.width, decoded.height);
    REQUIRE(grid.valid);
    REQUIRE(grid.rows == kRows);

    // Every frame rectangle must contain its expected colour at its centre —
    // this validates both column slicing (x) and row slicing (y).
    for (int row = 0; row < kRows; ++row)
    {
        for (int col = 0; col < PetdexFrameGrid::kConventionCols; ++col)
        {
            const int idx = row * PetdexFrameGrid::kConventionCols + col;
            const auto fr = grid.frameRect(row, col);
            const int cx = fr.x + fr.w / 2;
            const int cy = fr.y + fr.h / 2;
            const std::size_t i = (static_cast<std::size_t>(cy) * 1536u + cx) * 4u;

            CAPTURE(row, col, cx, cy);
            REQUIRE(decoded.rgba[i + 0] == static_cast<std::uint8_t>((idx * 37) % 256));
            REQUIRE(decoded.rgba[i + 1] == static_cast<std::uint8_t>((idx * 71) % 256));
            REQUIRE(decoded.rgba[i + 2] == static_cast<std::uint8_t>((idx * 113) % 256));
            REQUIRE(decoded.rgba[i + 3] == 255);
        }
    }
}

// ---------------------------------------------------------------------------
// PetdexResourceCache — per-pet sprite + D4 attribution snapshot
// ---------------------------------------------------------------------------

TEST_CASE("Resource cache stores and retrieves the sprite", "[petdex][sprite][cache]")
{
    TempDir dir;
    const std::string slug = "homelander";

    REQUIRE_FALSE(PetdexResourceCache::hasSprite(dir.path, slug));

    const std::vector<std::uint8_t> bytes = { 0x52, 0x49, 0x46, 0x46, 0x01, 0x02, 0x03 };
    REQUIRE(PetdexResourceCache::writeSprite(dir.path, slug, bytes.data(), bytes.size()));
    REQUIRE(PetdexResourceCache::hasSprite(dir.path, slug));
    REQUIRE(fs::exists(PetdexResourceCache::spritePath(dir.path, slug)));

    std::vector<std::uint8_t> read;
    REQUIRE(PetdexResourceCache::readSprite(dir.path, slug, read));
    REQUIRE(read == bytes);

    SECTION("removeSprite clears a corrupt cache entry")
    {
        REQUIRE(PetdexResourceCache::removeSprite(dir.path, slug));
        REQUIRE_FALSE(PetdexResourceCache::hasSprite(dir.path, slug));
    }
}

TEST_CASE("Resource cache stores and retrieves the D4 attribution snapshot",
          "[petdex][sprite][cache][d4]")
{
    TempDir dir;
    const std::string slug = "mecha-xiaobai";

    PetdexAttributionSnapshot snap;
    snap.canDisplay   = true;
    snap.slug         = slug;
    snap.displayName  = "Mecha Xiaobai";
    snap.submitter    = "369772958";
    snap.creditLine   = "Submitted by 369772958 · Petdex community gallery (petdex.dev)";
    snap.notice       = PetdexAttribution::kNoLicenseNotice;
    snap.spritesheetUrl = "https://assets.petdex.dev/pets/x/sprite.webp";

    REQUIRE(PetdexResourceCache::writeAttribution(dir.path, slug, snap));

    PetdexAttributionSnapshot out;
    REQUIRE(PetdexResourceCache::readAttribution(dir.path, slug, out));
    REQUIRE(out.canDisplay);
    REQUIRE(out.slug == slug);
    REQUIRE(out.displayName == snap.displayName);
    REQUIRE(out.submitter == snap.submitter);
    REQUIRE(out.creditLine == snap.creditLine);
    REQUIRE(out.notice == snap.notice);
    REQUIRE(out.spritesheetUrl == snap.spritesheetUrl);

    SECTION("missing snapshot is not present (display is blocked)")
    {
        PetdexAttributionSnapshot missing;
        REQUIRE_FALSE(PetdexResourceCache::readAttribution(dir.path, "no-such-pet", missing));
    }

    SECTION("corrupt snapshot file is treated as absent (D4 blocks display)")
    {
        const auto path = PetdexResourceCache::attributionPath(dir.path, slug);
        std::ofstream(path, std::ios::binary) << "{ not valid json";
        PetdexAttributionSnapshot corrupt;
        REQUIRE_FALSE(PetdexResourceCache::readAttribution(dir.path, slug, corrupt));
    }
}

TEST_CASE("Resource cache sanitises slugs for use in paths", "[petdex][sprite][cache]")
{
    SECTION("safe slugs pass through")
    {
        REQUIRE(PetdexResourceCache::sanitizeSlug("homelander") == "homelander");
        REQUIRE(PetdexResourceCache::sanitizeSlug("mecha-xiaobai_1.2") == "mecha-xiaobai_1.2");
    }

    SECTION("path traversal and separators are neutralised")
    {
        const auto dirty = PetdexResourceCache::sanitizeSlug("../etc/passwd");
        REQUIRE(dirty.find('/') == std::string::npos);
        // The result must stay a single safe path component inside the cache
        // root (dots alone cannot traverse once separators are gone).
        REQUIRE(fs::path(dirty).filename().string() == dirty);

        TempDir dir;
        const auto petPath = PetdexResourceCache::petDir(dir.path, dirty);
        REQUIRE(petPath.string().find(dir.path.string()) == 0);
    }

    SECTION("a literal '..' slug is neutralised")
    {
        REQUIRE(PetdexResourceCache::sanitizeSlug("..") == "_");
    }

    SECTION("spaces and non-ASCII are replaced")
    {
        REQUIRE(PetdexResourceCache::sanitizeSlug("a b") == "a_b");
        REQUIRE(PetdexResourceCache::sanitizeSlug("caf\xc3\xa9") == "caf__");
    }

    SECTION("empty slug becomes a safe placeholder")
    {
        REQUIRE(PetdexResourceCache::sanitizeSlug("") == "_");
    }
}
