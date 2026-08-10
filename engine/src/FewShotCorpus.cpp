// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * FewShotCorpus.cpp — implementation of the versioned few-shot example corpus
 * loader (AI-G4).
 *
 * JUCE-free: uses only nlohmann/json and the standard library. Mirrors the
 * validation strategy of LanguageMetadata::loadAndValidate (AI-3) so there is
 * no independent versioning system — both are checked against the same live
 * constants in hathor::language.
 *
 * Requirement references: AI-G4, AI-3, decision #18
 */

#include "hathor/FewShotCorpus.hpp"
#include "hathor/LanguageMetadata.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace hathor::language {

// ---------------------------------------------------------------------------
// Internal: current ISO-8601 UTC timestamp
// ---------------------------------------------------------------------------

static std::string currentIsoTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// assignFewShotToConsumer
// ---------------------------------------------------------------------------

void assignFewShotToConsumer(FewShotCorpus& corpus, std::string_view consumer)
{
    corpus.consumer = std::string(consumer);
    corpus.loadedAt = currentIsoTimestamp();
}

// ---------------------------------------------------------------------------
// loadFewShotCorpus
// ---------------------------------------------------------------------------

FewShotLoadResult loadFewShotCorpus(std::string_view jsonPath)
{
    FewShotLoadResult result;
    FewShotCompatibility& compat = result.compatibility;
    compat.schemaVersion  = std::to_string(kSchemaVersion);
    compat.engineVersion  = std::string(kHathorEngineCompat);

    // --- Step 1: Read the file ---
    std::error_code ec;
    if (!std::filesystem::exists(jsonPath, ec))
    {
        compat.compatible = false;
        compat.errors.push_back(
            "Few-shot corpus file not found: " + std::string(jsonPath));
        return result;
    }

    std::ifstream ifs(jsonPath);
    if (!ifs.is_open())
    {
        compat.compatible = false;
        compat.errors.push_back(
            "Cannot open few-shot corpus file: " + std::string(jsonPath));
        return result;
    }

    std::stringstream ss;
    ss << ifs.rdbuf();
    const std::string jsonText = ss.str();

    // --- Step 2: Parse JSON ---
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonText, nullptr, true, true);
    } catch (const nlohmann::json::exception& e) {
        compat.compatible = false;
        compat.errors.push_back(std::string("JSON parse error: ") + e.what());
        return result;
    }

    // --- Step 3: Validate the versions block against AI-3 constants ---
    // The versions block MUST match the running AI-3 constants so examples can
    // never be silently reused against a changed surface. This is the single
    // version gate — not a parallel versioning system.
    auto& ver = result.corpus.versions;

    try {
        ver.schemaVersion            = j.at("versions").at("schemaVersion").get<int>();
        ver.hathorEngineCompat       = j.at("versions").at("hathorEngineCompat").get<std::string>();
        ver.strudelMiniNotationCompat= j.at("versions").at("strudelMiniNotationCompat").get<std::string>();
        ver.chuckLibVersion          = j.at("versions").at("chuckLibVersion").get<std::string>();
        ver.chuckIntegrationSurface  = j.at("versions").at("chuckIntegrationSurface").get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        compat.compatible = false;
        compat.errors.push_back(
            std::string("Missing required versions field in few-shot corpus: ") + e.what());
        return result;
    }

    if (j.contains("versions") && j["versions"].contains("createdAt"))
        ver.createdAt = j["versions"]["createdAt"].get<std::string>();

    // Schema version check
    if (ver.schemaVersion != kSchemaVersion) {
        compat.compatible = false;
        compat.errors.push_back(
            "Schema version mismatch: few-shot corpus has schema "
            + std::to_string(ver.schemaVersion) + ", engine expects "
            + std::to_string(kSchemaVersion));
    }

    // Hathor engine compatibility check
    if (ver.hathorEngineCompat != kHathorEngineCompat) {
        compat.compatible = false;
        compat.errors.push_back(
            "Hathor engine version mismatch: corpus describes "
            + ver.hathorEngineCompat + ", running engine is "
            + std::string(kHathorEngineCompat));
    }

    // Strudel mini-notation compatibility check
    if (ver.strudelMiniNotationCompat != kStrudelMiniNotationCompat) {
        compat.compatible = false;
        compat.errors.push_back(
            "Strudel mini-notation version mismatch: corpus describes "
            + ver.strudelMiniNotationCompat + ", engine expects "
            + std::string(kStrudelMiniNotationCompat));
    }

    // ChucK lib version check
    if (ver.chuckLibVersion != kChuckLibVersion) {
        compat.compatible = false;
        compat.errors.push_back(
            "libchuck version mismatch: corpus describes "
            + ver.chuckLibVersion + ", engine expects "
            + std::string(kChuckLibVersion));
    }

    // ChucK integration surface check
    if (ver.chuckIntegrationSurface != kChuckIntegrationSurface) {
        compat.compatible = false;
        compat.errors.push_back(
            "ChucK integration surface mismatch: corpus describes "
            + ver.chuckIntegrationSurface + ", engine expects "
            + std::string(kChuckIntegrationSurface));
    }

    // If any version check failed, do NOT load examples — stale-corpus
    // rejection (AI-G3 requirement). Empty examples vector.
    if (!compat.errors.empty()) {
        compat.compatible = false;
        result.corpus.compatible = false;
        result.corpus.errors = compat.errors;
        return result;
    }

    compat.compatible = true;
    result.corpus.compatible = true;

    // --- Step 4: Parse examples ---
    if (j.contains("examples")) {
        for (const auto& ej : j["examples"]) {
            FewShotExample ex;
            ex.language          = ej.at("language").get<std::string>();
            ex.surfaceVersion    = ej.at("surface_version").get<std::string>();
            ex.context           = ej.value("context", std::string{});
            ex.title             = ej.at("title").get<std::string>();
            ex.code              = ej.at("code").get<std::string>();
            ex.validatesAgainst  = ej.value("validates_against", std::string{});

            result.corpus.examples.push_back(std::move(ex));
        }
    }

    return result;
}

} // namespace hathor::language
