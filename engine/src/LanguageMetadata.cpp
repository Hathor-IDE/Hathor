// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * LanguageMetadata.cpp — implementation of the versioned supported-surface
 * metadata model.
 *
 * JUCE-free: uses only nlohmann/json and standard library. The JSON file
 * is parsed at startup/load time (not on the audio thread).
 *
 * Requirement references: AI-3, decision #18
 */

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
// Internal: get current ISO-8601 UTC timestamp
// ---------------------------------------------------------------------------

static std::string currentIsoTimestamp()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// assignToConsumer
// ---------------------------------------------------------------------------

void assignToConsumer(LanguageMetadata& metadata, std::string_view consumer)
{
    metadata.consumer = std::string(consumer);
    metadata.loadedAt = currentIsoTimestamp();
}

// ---------------------------------------------------------------------------
// loadAndValidate
// ---------------------------------------------------------------------------

LoadResult loadAndValidate(std::string_view jsonPath)
{
    LoadResult result;
    MetadataCompatibility& compat = result.compatibility;
    compat.schemaVersion = std::to_string(kSchemaVersion);
    compat.engineVersion = std::string(kHathorEngineCompat);

    // --- Step 1: Read the file ---
    std::error_code ec;
    if (!std::filesystem::exists(jsonPath, ec)) {
        compat.compatible = false;
        compat.errors.push_back(
            "Metadata file not found: " + std::string(jsonPath));
        return result;
    }

    std::ifstream ifs(jsonPath);
    if (!ifs.is_open()) {
        compat.compatible = false;
        compat.errors.push_back(
            "Cannot open metadata file: " + std::string(jsonPath));
        return result;
    }

    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string jsonText = ss.str();

    // --- Step 2: Parse JSON ---
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonText, nullptr, true, true);
    } catch (const nlohmann::json::exception& e) {
        compat.compatible = false;
        compat.errors.push_back(
            std::string("JSON parse error: ") + e.what());
        return result;
    }

    // --- Step 3: Parse top-level fields ---
    auto& meta = result.metadata;

    try {
        meta.schemaVersion             = j.at("schemaVersion").get<int>();
        meta.hathorEngineCompat        = j.at("hathorEngineCompat").get<std::string>();
        meta.strudelMiniNotationCompat = j.at("strudelMiniNotationCompat").get<std::string>();
        meta.chuckLibVersion           = j.value("chuckLibVersion", std::string{});
        meta.chuckIntegrationSurface   = j.value("chuckIntegrationSurface", std::string{});
    } catch (const nlohmann::json::exception& e) {
        compat.compatible = false;
        compat.errors.push_back(
            std::string("Missing required field: ") + e.what());
        return result;
    }

    // --- Step 4: Validate schema version ---
    if (meta.schemaVersion != kSchemaVersion) {
        compat.compatible = false;
        compat.errors.push_back(
            "Schema version mismatch: metadata has schema "
            + std::to_string(meta.schemaVersion) + ", engine expects "
            + std::to_string(kSchemaVersion));
    }

    // --- Step 5: Validate Hathor engine compatibility ---
    if (meta.hathorEngineCompat != kHathorEngineCompat) {
        compat.compatible = false;
        compat.errors.push_back(
            "Hathor engine version mismatch: metadata describes "
            + meta.hathorEngineCompat + ", running engine is "
            + std::string(kHathorEngineCompat));
    }

    // --- Step 6: Validate Strudel mini-notation compatibility ---
    if (meta.strudelMiniNotationCompat != kStrudelMiniNotationCompat) {
        compat.compatible = false;
        compat.errors.push_back(
            "Strudel mini-notation version mismatch: metadata describes "
            + meta.strudelMiniNotationCompat + ", engine expects "
            + std::string(kStrudelMiniNotationCompat));
    }

    // --- Step 7: Parse definition arrays ---

    // Functions
    if (j.contains("functions")) {
        for (const auto& fj : j["functions"]) {
            MiniNotationFunction fn;
            fn.name        = fj.at("name").get<std::string>();
            fn.signature   = fj.at("signature").get<std::string>();
            fn.description = fj.at("description").get<std::string>();
            fn.category    = fj.at("category").get<std::string>();
            fn.supported   = fj.value("supported", true);
            if (fj.contains("example"))
                fn.example = fj["example"].get<std::string>();
            meta.functions.push_back(std::move(fn));
        }
    }

    // Samples
    if (j.contains("samples")) {
        for (const auto& sj : j["samples"]) {
            SampleDefinition sd;
            sd.name        = sj.at("name").get<std::string>();
            sd.description = sj.at("description").get<std::string>();
            sd.category    = sj.at("category").get<std::string>();
            meta.samples.push_back(std::move(sd));
        }
    }

    // Operators
    if (j.contains("operators")) {
        for (const auto& oj : j["operators"]) {
            MiniNotationOperator op;
            op.name        = oj.at("name").get<std::string>();
            op.description = oj.at("description").get<std::string>();
            op.example     = oj.at("example").get<std::string>();
            meta.operators.push_back(std::move(op));
        }
    }

    // Grammar
    if (j.contains("grammar")) {
        for (const auto& gg : j["grammar"]) {
            GrammarElement ge;
            ge.name        = gg.at("name").get<std::string>();
            ge.syntax      = gg.at("syntax").get<std::string>();
            ge.description = gg.at("description").get<std::string>();
            ge.example     = gg.at("example").get<std::string>();
            ge.supported   = gg.value("supported", true);
            meta.grammar.push_back(std::move(ge));
        }
    }

    // Params
    if (j.contains("params")) {
        for (const auto& pj : j["params"]) {
            ParamDefinition pd;
            pd.key          = pj.at("key").get<std::string>();
            pd.valueType     = pj.at("valueType").get<std::string>();
            pd.description   = pj.at("description").get<std::string>();
            pd.supported     = pj.value("supported", true);
            meta.params.push_back(std::move(pd));
        }
    }

    // ChucK API
    if (j.contains("chuckApi")) {
        for (const auto& cj : j["chuckApi"]) {
            ChuckAPIDefinition ca;
            ca.name        = cj.at("name").get<std::string>();
            ca.kind        = cj.at("kind").get<std::string>();
            ca.signature   = cj.at("signature").get<std::string>();
            ca.description = cj.at("description").get<std::string>();
            ca.supported   = cj.value("supported", true);
            if (cj.contains("example"))
                ca.example = cj["example"].get<std::string>();
            meta.chuckApi.push_back(std::move(ca));
        }
    }

    // If there were version mismatch errors above, don't proceed with parsing
    // definitions — the metadata is incompatible and must not be used.
    if (compat.compatible == false && compat.errors.size() > 0) {
        // Check if any errors are version-mismatch errors (not just definition parse)
        bool hasVersionError = false;
        for (const auto& e : compat.errors) {
            if (e.find("mismatch") != std::string::npos) {
                hasVersionError = true;
                break;
            }
        }
        if (hasVersionError) {
            result.metadata = LanguageMetadata{};
            return result;
        }
    }

    compat.compatible = compat.errors.empty();
    return result;
}

// ---------------------------------------------------------------------------
// find* helpers
// ---------------------------------------------------------------------------

const MiniNotationFunction* findFunction(const LanguageMetadata& meta, std::string_view name) noexcept
{
    for (const auto& fn : meta.functions) {
        if (fn.name == name)
            return &fn;
    }
    return nullptr;
}

const SampleDefinition* findSample(const LanguageMetadata& meta, std::string_view name) noexcept
{
    for (const auto& sd : meta.samples) {
        if (sd.name == name)
            return &sd;
    }
    return nullptr;
}

const MiniNotationOperator* findOperator(const LanguageMetadata& meta, std::string_view name) noexcept
{
    for (const auto& op : meta.operators) {
        if (op.name == name)
            return &op;
    }
    return nullptr;
}

const GrammarElement* findGrammar(const LanguageMetadata& meta, std::string_view name) noexcept
{
    for (const auto& ge : meta.grammar) {
        if (ge.name == name)
            return &ge;
    }
    return nullptr;
}

const ParamDefinition* findParam(const LanguageMetadata& meta, std::string_view key) noexcept
{
    for (const auto& pd : meta.params) {
        if (pd.key == key)
            return &pd;
    }
    return nullptr;
}

const ChuckAPIDefinition* findChuckApi(const LanguageMetadata& meta, std::string_view name) noexcept
{
    for (const auto& ca : meta.chuckApi) {
        if (ca.name == name)
            return &ca;
    }
    return nullptr;
}

} // namespace hathor::language
