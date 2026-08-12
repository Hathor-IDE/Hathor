// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_petdex_manifest.cpp — Phase G / D1 unit tests (JUCE-free).
 *
 * Covers:
 *   - PetdexManifestParser: real-shaped manifest, missing fields, malformed
 *     entries, invalid JSON, cache-envelope round-trip.
 *   - PetdexCacheStore: write/read, staleness boundaries, missing files.
 *   - PetdexAttribution (D4 gate): displayable iff attribution is resolvable,
 *     credit shown, and no license is ever claimed.
 */

#include <catch2/catch_test_macros.hpp>

#include "PetdexManifestParser.hpp"
#include "PetdexCacheStore.hpp"
#include "PetdexAttribution.hpp"
#include "PetdexLoadPolicy.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using namespace hathor::ui;

namespace {

/// A slice of the LIVE manifest shape (verified 2026-08-12 against
/// https://petdex.dev/api/manifest — homelander + mecha-xiaobai entries).
std::string realShapedManifestJson()
{
    return R"({
      "generatedAt": "2026-08-12T00:00:00Z",
      "total": 2,
      "pets": [
        {
          "slug": "homelander",
          "displayName": "Homelander",
          "kind": "character",
          "submittedBy": "Serhat",
          "spritesheetUrl": "https://assets.petdex.dev/pets/homelander-dbbb6a60a484/sprite.webp",
          "petJsonUrl": "https://assets.petdex.dev/pets/homelander-dbbb6a60a484/petjson.json",
          "zipUrl": "https://assets.petdex.dev/pets/homelander-dbbb6a60a484/zip.zip",
          "spriteVersionNumber": 1
        },
        {
          "slug": "mecha-xiaobai",
          "displayName": "\u673a\u7532\u5c0f\u767d",
          "kind": "character",
          "submittedBy": "369772958",
          "spritesheetUrl": "https://assets.petdex.dev/pets/mecha-xiaobai-d9c53c86fadc/sprite.webp",
          "petJsonUrl": "https://assets.petdex.dev/pets/mecha-xiaobai-d9c53c86fadc/petjson.json",
          "zipUrl": "https://assets.petdex.dev/pets/mecha-xiaobai-d9c53c86fadc/zip.zip",
          "spriteVersionNumber": 1
        }
      ]
    })";
}

/// Temporary directory unique to the calling test; removed on destruction.
struct TempDir
{
    fs::path path;

    TempDir()
    {
        const auto base = fs::temp_directory_path() / "hathor-petdex-test";
        std::error_code ec;
        fs::create_directories(base, ec);
        path = base / (std::string("t-") + std::to_string(++counter));
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

} // anonymous namespace

// ---------------------------------------------------------------------------
// PetdexManifestParser
// ---------------------------------------------------------------------------

TEST_CASE("PetdexManifestParser parses a real-shaped manifest", "[petdex][parser]")
{
    const auto result = PetdexManifestParser::parseManifest(realShapedManifestJson());

    REQUIRE(result.ok);
    REQUIRE(result.error.empty());
    REQUIRE(result.manifest.total == 2);
    REQUIRE(result.manifest.pets.size() == 2);
    REQUIRE(result.skipped == 0);
    REQUIRE(result.manifest.generatedAt == "2026-08-12T00:00:00Z");

    const auto& home = result.manifest.pets[0];
    REQUIRE(home.slug == "homelander");
    REQUIRE(home.displayName == "Homelander");
    REQUIRE(home.kind == "character");
    REQUIRE(home.submittedBy == "Serhat");
    REQUIRE(home.spritesheetUrl.find("https://") == 0);
    REQUIRE(home.petJsonUrl.find("https://") == 0);
    REQUIRE(home.zipUrl.find("https://") == 0);
    REQUIRE(home.spriteVersionNumber == 1);

    // total reflects the parsed count, never a hard-coded number.
    REQUIRE(result.manifest.total == static_cast<int>(result.manifest.pets.size()));
}

TEST_CASE("PetdexManifestParser tolerates missing fields and skips bad entries",
          "[petdex][parser]")
{
    const std::string json = R"({
      "generatedAt": "x",
      "total": 4,
      "pets": [
        { "slug": "only-slug" },
        { "slug": " full-pet ",
          "displayName": "  Full Pet  ",
          "kind": "creature",
          "submittedBy": "  author  ",
          "spritesheetUrl": "https://example.com/s.png",
          "zipUrl": "not-a-url",
          "spriteVersionNumber": "nope" },
        { "displayName": "No slug" },
        "not an object",
        { "slug": "malicious", "zipUrl": "file:///etc/passwd" }
      ]
    })";

    const auto result = PetdexManifestParser::parseManifest(json);
    REQUIRE(result.ok);

    // only-slug + full-pet + malicious survive; no-slug + non-object skipped.
    REQUIRE(result.manifest.pets.size() == 3);
    REQUIRE(result.skipped == 2);
    REQUIRE(result.manifest.total == 3);

    const auto& fallback = result.manifest.pets[0];
    REQUIRE(fallback.slug == "only-slug");
    REQUIRE(fallback.displayName == "only-slug");   // displayName falls back to slug
    REQUIRE(fallback.kind.empty());
    REQUIRE(fallback.submittedBy.empty());
    REQUIRE(fallback.spritesheetUrl.empty());
    REQUIRE(fallback.spriteVersionNumber == 1);     // default when absent/garbage

    const auto& trimmed = result.manifest.pets[1];
    REQUIRE(trimmed.slug == "full-pet");
    REQUIRE(trimmed.displayName == "Full Pet");
    REQUIRE(trimmed.kind == "creature");
    REQUIRE(trimmed.submittedBy == "author");
    REQUIRE(trimmed.spritesheetUrl == "https://example.com/s.png");
    REQUIRE(trimmed.zipUrl.empty());                // non-http URL discarded

    const auto& malicious = result.manifest.pets[2];
    REQUIRE(malicious.slug == "malicious");
    REQUIRE(malicious.zipUrl.empty());              // file:// discarded
}

TEST_CASE("PetdexManifestParser rejects invalid input", "[petdex][parser]")
{
    SECTION("not JSON")
    {
        const auto r = PetdexManifestParser::parseManifest("<html>not json</html>");
        REQUIRE_FALSE(r.ok);
        REQUIRE_FALSE(r.error.empty());
        REQUIRE(r.manifest.pets.empty());
    }

    SECTION("JSON but not an object or array")
    {
        const auto r = PetdexManifestParser::parseManifest("42");
        REQUIRE_FALSE(r.ok);
        REQUIRE_FALSE(r.error.empty());
    }

    SECTION("object without pets array")
    {
        const auto r = PetdexManifestParser::parseManifest(R"({"generatedAt":"x"})");
        REQUIRE_FALSE(r.ok);
    }

    SECTION("empty pets array is valid (dynamic catalog)")
    {
        const auto r = PetdexManifestParser::parseManifest(R"({"pets":[]})");
        REQUIRE(r.ok);
        REQUIRE(r.manifest.pets.empty());
        REQUIRE(r.manifest.total == 0);
    }

    SECTION("bare top-level array is accepted")
    {
        const auto r = PetdexManifestParser::parseManifest(R"([{"slug":"a"}])");
        REQUIRE(r.ok);
        REQUIRE(r.manifest.pets.size() == 1);
    }
}

TEST_CASE("PetdexManifestParser cache envelope round-trips", "[petdex][parser][cache]")
{
    const auto parsed = PetdexManifestParser::parseManifest(realShapedManifestJson());
    REQUIRE(parsed.ok);

    const std::int64_t fetchedAt = 1234567890000LL;
    const std::string envelope =
        PetdexManifestParser::makeCacheEnvelope(parsed.manifest, fetchedAt);

    const auto read = PetdexManifestParser::parseCacheEnvelope(envelope);
    REQUIRE(read.ok);
    REQUIRE(read.fetchedAtEpochMs == fetchedAt);
    REQUIRE(read.manifest.pets.size() == parsed.manifest.pets.size());
    REQUIRE(read.manifest.pets[0].slug == "homelander");
    REQUIRE(read.manifest.pets[0].submittedBy == "Serhat");
    REQUIRE(read.manifest.pets[0].zipUrl.find("https://") == 0);

    // Corrupt cache data must fail cleanly (never throw / never crash).
    SECTION("corrupt envelope")
    {
        const auto bad = PetdexManifestParser::parseCacheEnvelope("not-json-at-all");
        REQUIRE_FALSE(bad.ok);
    }

    SECTION("unsupported version")
    {
        nlohmann::json j = nlohmann::json::parse(envelope);
        j["version"] = "99";
        const auto bad = PetdexManifestParser::parseCacheEnvelope(j.dump());
        REQUIRE_FALSE(bad.ok);
        REQUIRE(bad.error.find("version") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// PetdexCacheStore
// ---------------------------------------------------------------------------

TEST_CASE("PetdexCacheStore writes and reads the envelope", "[petdex][cache]")
{
    TempDir dir;
    const std::string envelope = R"({"version":"1","fetchedAtEpochMs":1000,"manifest":{"pets":[]}})";

    // Missing cache → not present.
    REQUIRE_FALSE(PetdexCacheStore::read(dir.path).present);

    REQUIRE(PetdexCacheStore::write(dir.path, envelope));
    REQUIRE(fs::exists(PetdexCacheStore::manifestPath(dir.path)));

    const auto read = PetdexCacheStore::read(dir.path);
    REQUIRE(read.present);
    REQUIRE(read.json == envelope);
}

TEST_CASE("PetdexCacheStore staleness policy", "[petdex][cache]")
{
    SECTION("fresh below max age")
    {
        REQUIRE_FALSE(PetdexCacheStore::isStale(0, 1000, 5000));
        REQUIRE(PetdexCacheStore::ageMs(0, 1000) == 1000);
    }

    SECTION("stale at exactly max age (boundary inclusive)")
    {
        REQUIRE(PetdexCacheStore::isStale(0, 5000, 5000));
    }

    SECTION("stale past max age")
    {
        REQUIRE(PetdexCacheStore::isStale(0, 5001, 5000));
    }

    SECTION("future timestamps clamp to zero age")
    {
        REQUIRE(PetdexCacheStore::ageMs(5000, 1000) == 0);
        REQUIRE_FALSE(PetdexCacheStore::isStale(5000, 1000, 100));
    }
}

// ---------------------------------------------------------------------------
// PetdexLoadPolicy — D1 cache/failure decision logic
// ---------------------------------------------------------------------------

TEST_CASE("PetdexLoadPolicy plans pre-fetch decisions", "[petdex][policy]")
{
    constexpr std::int64_t kMax = 1000;

    SECTION("fresh cache is served with no network")
    {
        REQUIRE(PetdexLoadPolicy::planPreFetch(true, false, 0, 500, kMax)
                == PetdexPreFetch::ServeFreshCache);
    }

    SECTION("stale cache is served but flagged for refresh")
    {
        REQUIRE(PetdexLoadPolicy::planPreFetch(true, false, 0, 5000, kMax)
                == PetdexPreFetch::ServeStaleCache);
    }

    SECTION("no cache means fetch directly")
    {
        REQUIRE(PetdexLoadPolicy::planPreFetch(false, false, 0, 0, kMax)
                == PetdexPreFetch::None);
    }

    SECTION("forced refresh bypasses a fresh cache")
    {
        REQUIRE(PetdexLoadPolicy::planPreFetch(true, true, 0, 500, kMax)
                == PetdexPreFetch::ServeStaleCache);
    }
}

TEST_CASE("PetdexLoadPolicy plans post-fetch decisions", "[petdex][policy]")
{
    SECTION("success serves the fresh fetch")
    {
        REQUIRE(PetdexLoadPolicy::planPostFetch(true, true) == PetdexPostFetch::ServeFetch);
        REQUIRE(PetdexLoadPolicy::planPostFetch(true, false) == PetdexPostFetch::ServeFetch);
    }

    SECTION("failure keeps a stale cache in use")
    {
        REQUIRE(PetdexLoadPolicy::planPostFetch(false, true) == PetdexPostFetch::KeepStaleCache);
    }

    SECTION("failure without cache goes offline")
    {
        REQUIRE(PetdexLoadPolicy::planPostFetch(false, false) == PetdexPostFetch::Offline);
    }
}

// ---------------------------------------------------------------------------
// PetdexAttribution — D4 gate
// ---------------------------------------------------------------------------

TEST_CASE("PetdexAttribution gates display on resolvable attribution",
          "[petdex][d4][attribution]")
{
    SECTION("pet with submitter is displayable with credit")
    {
        PetdexPet pet;
        pet.slug = "homelander";
        pet.displayName = "Homelander";
        pet.submittedBy = "Serhat";

        const auto info = PetdexAttribution::resolve(pet);
        REQUIRE(info.canDisplay);
        REQUIRE(info.submitter == "Serhat");
        REQUIRE(info.creditLine.find("Serhat") != std::string::npos);
        REQUIRE(info.creditLine.find("petdex.dev") != std::string::npos);
        // Attribution only — never a license claim (the manifest has none).
        REQUIRE(info.creditLine.find("license") == std::string::npos);
        REQUIRE(info.creditLine.find("Licensed") == std::string::npos);
        REQUIRE(info.notice.find("no per-pet license") != std::string::npos);
    }

    SECTION("whitespace-only submitter is treated as missing")
    {
        PetdexPet pet;
        pet.slug = "ghost";
        pet.submittedBy = "   ";

        const auto info = PetdexAttribution::resolve(pet);
        REQUIRE_FALSE(info.canDisplay);
        REQUIRE(info.creditLine.empty());
        REQUIRE(info.notice == PetdexAttribution::kMissingAttributionNotice);
    }

    SECTION("missing submitter blocks display with an explanatory notice")
    {
        PetdexPet pet;
        pet.slug = "anonymous";
        pet.displayName = "Anonymous";

        const auto info = PetdexAttribution::resolve(pet);
        REQUIRE_FALSE(info.canDisplay);
        REQUIRE_FALSE(info.notice.empty());
        REQUIRE(info.notice.find("cannot be displayed") != std::string::npos);
        REQUIRE(info.notice.find("no license") != std::string::npos);
    }
}
