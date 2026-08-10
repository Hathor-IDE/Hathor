// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_language_metadata.cpp — Tests for the versioned supported-surface
 * metadata model (AI-3).
 *
 * Verifies that:
 * - HathorLanguageMetadata.json loads and parses correctly
 * - Schema and engine compatibility version checks work
 * - Incompatible metadata is rejected
 * - find* helpers work correctly for all definition categories
 * - Consumer assignment sets metadata fields
 * - Only Hathor-supported definitions are marked as such
 */

#include <catch2/catch_test_macros.hpp>

#include "hathor/LanguageMetadata.hpp"

#include <string>
#include <string_view>

using namespace hathor::language;

#ifndef LANGUAGE_METADATA_DIR
#error "LANGUAGE_METADATA_DIR compile definition is not set. Check CMakeLists.txt."
#endif

static std::string metadataPath()
{
    return std::string(LANGUAGE_METADATA_DIR) + "/HathorLanguageMetadata.json";
}

// ---------------------------------------------------------------------------
// Loading and validation
// ---------------------------------------------------------------------------

TEST_CASE("LanguageMetadata loads from the canonical JSON file", "[ai3][metadata][load]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);
    REQUIRE(result.compatibility.errors.empty());
}

TEST_CASE("LanguageMetadata has correct schema version", "[ai3][metadata][version]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);
    REQUIRE(result.metadata.schemaVersion == kSchemaVersion);
}

TEST_CASE("LanguageMetadata has matching engine compat version", "[ai3][metadata][version]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);
    REQUIRE(result.metadata.hathorEngineCompat == kHathorEngineCompat);
}

TEST_CASE("LanguageMetadata has matching Strudel compat version", "[ai3][metadata][version]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);
    REQUIRE(result.metadata.strudelMiniNotationCompat == kStrudelMiniNotationCompat);
}

TEST_CASE("LanguageMetadata has libchuck version", "[ai3][metadata][version]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);
    REQUIRE_FALSE(result.metadata.chuckLibVersion.empty());
    REQUIRE(result.metadata.chuckLibVersion == kChuckLibVersion);
}

TEST_CASE("LanguageMetadata has ChucK integration surface", "[ai3][metadata][version]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);
    REQUIRE_FALSE(result.metadata.chuckIntegrationSurface.empty());
    REQUIRE(result.metadata.chuckIntegrationSurface == kChuckIntegrationSurface);
}

// ---------------------------------------------------------------------------
// Definition counts
// ---------------------------------------------------------------------------

TEST_CASE("LanguageMetadata contains supported functions", "[ai3][metadata][functions]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);
    REQUIRE_FALSE(result.metadata.functions.empty());

    // "s" and "sound" must be present and supported
    auto* sFn = findFunction(result.metadata, "s");
    REQUIRE(sFn != nullptr);
    REQUIRE(sFn->supported == true);

    auto* soundFn = findFunction(result.metadata, "sound");
    REQUIRE(soundFn != nullptr);
    REQUIRE(soundFn->supported == true);
}

TEST_CASE("LanguageMetadata marks unsupported functions as unsupported", "[ai3][metadata][functions]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);

    // "note" should be listed but unsupported (Hathor doesn't implement pitched note patterns yet)
    auto* noteFn = findFunction(result.metadata, "note");
    REQUIRE(noteFn != nullptr);
    REQUIRE(noteFn->supported == false);
}

TEST_CASE("LanguageMetadata contains standard samples", "[ai3][metadata][samples]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);
    REQUIRE_FALSE(result.metadata.samples.empty());

    // Core drum samples must be listed
    REQUIRE(findSample(result.metadata, "bd") != nullptr);
    REQUIRE(findSample(result.metadata, "sn") != nullptr);
    REQUIRE(findSample(result.metadata, "hh") != nullptr);
}

TEST_CASE("LanguageMetadata contains supported grammar elements", "[ai3][metadata][grammar]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);
    REQUIRE_FALSE(result.metadata.grammar.empty());

    // Core grammar elements must be present and supported
    auto* atom = findGrammar(result.metadata, "atom");
    REQUIRE(atom != nullptr);
    REQUIRE(atom->supported == true);

    auto* seq = findGrammar(result.metadata, "sequence");
    REQUIRE(seq != nullptr);
    REQUIRE(seq->supported == true);
}

TEST_CASE("LanguageMetadata contains supported operators", "[ai3][metadata][operators]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);
    REQUIRE_FALSE(result.metadata.operators.empty());

    // Core operators must be listed
    REQUIRE(findOperator(result.metadata, "*") != nullptr);
    REQUIRE(findOperator(result.metadata, "/") != nullptr);
    REQUIRE(findOperator(result.metadata, "!") != nullptr);
}

TEST_CASE("LanguageMetadata contains supported params", "[ai3][metadata][params]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);
    REQUIRE_FALSE(result.metadata.params.empty());

    // Core params must be present and supported
    auto* sParam = findParam(result.metadata, "s");
    REQUIRE(sParam != nullptr);
    REQUIRE(sParam->supported == true);

    auto* gainParam = findParam(result.metadata, "gain");
    REQUIRE(gainParam != nullptr);
    REQUIRE(gainParam->supported == true);
}

TEST_CASE("LanguageMetadata contains ChucK API definitions", "[ai3][metadata][chuckapi]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);
    REQUIRE_FALSE(result.metadata.chuckApi.empty());

    // Core ChucK UGen must be present
    auto* sinOsc = findChuckApi(result.metadata, "SinOsc");
    REQUIRE(sinOsc != nullptr);
    REQUIRE(sinOsc->supported == true);
    REQUIRE(sinOsc->kind == "ugen");

    // Constants must be present
    auto* dac = findChuckApi(result.metadata, "dac");
    REQUIRE(dac != nullptr);
    REQUIRE(dac->kind == "constant");

    auto* now = findChuckApi(result.metadata, "now");
    REQUIRE(now != nullptr);
    REQUIRE(now->kind == "constant");
}

// ---------------------------------------------------------------------------
// Compatibility failures
// ---------------------------------------------------------------------------

TEST_CASE("LanguageMetadata rejects missing file", "[ai3][metadata][compat]")
{
    auto result = loadAndValidate("/nonexistent/path/to/HathorLanguageMetadata.json");
    REQUIRE_FALSE(result.compatibility.compatible);
    REQUIRE_FALSE(result.compatibility.errors.empty());
}

TEST_CASE("LanguageMetadata rejects schema version mismatch", "[ai3][metadata][compat]")
{
    // Create a temp file with wrong schema version
    std::string tmpPath = "/tmp/hathor_test_metadata_bad_schema.json";
    {
        std::FILE* f = std::fopen(tmpPath.c_str(), "w");
        REQUIRE(f != nullptr);
        std::fprintf(f, R"({
            "schemaVersion": 999,
            "hathorEngineCompat": "0.1.0",
            "strudelMiniNotationCompat": "1.2.6",
            "chuckLibVersion": "3.8.3",
            "chuckIntegrationSurface": "B4-K3",
            "functions": [],
            "samples": [],
            "operators": [],
            "grammar": [],
            "params": [],
            "chuckApi": []
        })");
        std::fclose(f);
    }

    auto result = loadAndValidate(tmpPath);
    REQUIRE_FALSE(result.compatibility.compatible);
    REQUIRE_FALSE(result.compatibility.errors.empty());
    bool foundSchemaError = false;
    for (const auto& e : result.compatibility.errors) {
        if (e.find("Schema version mismatch") != std::string::npos)
            foundSchemaError = true;
    }
    REQUIRE(foundSchemaError);

    std::remove(tmpPath.c_str());
}

TEST_CASE("LanguageMetadata rejects engine version mismatch", "[ai3][metadata][compat]")
{
    std::string tmpPath = "/tmp/hathor_test_metadata_bad_engine.json";
    {
        std::FILE* f = std::fopen(tmpPath.c_str(), "w");
        REQUIRE(f != nullptr);
        std::fprintf(f, R"({
            "schemaVersion": 1,
            "hathorEngineCompat": "0.2.0",
            "strudelMiniNotationCompat": "1.2.6",
            "chuckLibVersion": "3.8.3",
            "chuckIntegrationSurface": "B4-K3",
            "functions": [],
            "samples": [],
            "operators": [],
            "grammar": [],
            "params": [],
            "chuckApi": []
        })");
        std::fclose(f);
    }

    auto result = loadAndValidate(tmpPath);
    REQUIRE_FALSE(result.compatibility.compatible);
    REQUIRE_FALSE(result.compatibility.errors.empty());
    bool foundEngineError = false;
    for (const auto& e : result.compatibility.errors) {
        if (e.find("Hathor engine version mismatch") != std::string::npos)
            foundEngineError = true;
    }
    REQUIRE(foundEngineError);

    std::remove(tmpPath.c_str());
}

TEST_CASE("LanguageMetadata rejects Strudel version mismatch", "[ai3][metadata][compat]")
{
    std::string tmpPath = "/tmp/hathor_test_metadata_bad_strudel.json";
    {
        std::FILE* f = std::fopen(tmpPath.c_str(), "w");
        REQUIRE(f != nullptr);
        std::fprintf(f, R"({
            "schemaVersion": 1,
            "hathorEngineCompat": "0.1.0",
            "strudelMiniNotationCompat": "99.0.0",
            "chuckLibVersion": "3.8.3",
            "chuckIntegrationSurface": "B4-K3",
            "functions": [],
            "samples": [],
            "operators": [],
            "grammar": [],
            "params": [],
            "chuckApi": []
        })");
        std::fclose(f);
    }

    auto result = loadAndValidate(tmpPath);
    REQUIRE_FALSE(result.compatibility.compatible);
    REQUIRE_FALSE(result.compatibility.errors.empty());
    bool foundStrudelError = false;
    for (const auto& e : result.compatibility.errors) {
        if (e.find("Strudel mini-notation version mismatch") != std::string::npos)
            foundStrudelError = true;
    }
    REQUIRE(foundStrudelError);

    std::remove(tmpPath.c_str());
}

// ---------------------------------------------------------------------------
// Consumer assignment
// ---------------------------------------------------------------------------

TEST_CASE("LanguageMetadata assigns to consumer", "[ai3][metadata][consumer]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);

    assignToConsumer(result.metadata, "hathor-editor");

    REQUIRE(result.metadata.consumer.has_value());
    REQUIRE(result.metadata.consumer.value() == "hathor-editor");
    REQUIRE(result.metadata.loadedAt.has_value());
    REQUIRE_FALSE(result.metadata.loadedAt.value().empty());
}

TEST_CASE("LanguageMetadata loadedAt has ISO-8601 format", "[ai3][metadata][consumer]")
{
    auto result = loadAndValidate(metadataPath());
    REQUIRE(result.compatibility.compatible);

    assignToConsumer(result.metadata, "ai-authoring");

    std::string ts = result.metadata.loadedAt.value();
    // Basic format check: "YYYY-MM-DDTHH:MM:SSZ"
    REQUIRE(ts.size() >= 20);
    REQUIRE(ts[4] == '-');
    REQUIRE(ts[7] == '-');
    REQUIRE(ts[10] == 'T');
    REQUIRE(ts[13] == ':');
    REQUIRE(ts[16] == ':');
    REQUIRE(ts[19] == 'Z');
}
