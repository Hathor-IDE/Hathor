// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PetdexManifestParser.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>

namespace hathor::ui {

namespace {

std::string trim(const std::string& s)
{
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    auto b = s.begin();
    while (b != s.end() && isSpace(static_cast<unsigned char>(*b)))
        ++b;
    auto e = s.end();
    while (e != b && isSpace(static_cast<unsigned char>(*(e - 1))))
        --e;
    return std::string(b, e);
}

bool looksLikeHttpUrl(const std::string& s)
{
    const auto p = s.find("://");
    if (p == std::string::npos || p == 0)
        return false;
    std::string scheme;
    scheme.reserve(p);
    for (std::size_t i = 0; i < p; ++i)
        scheme.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(s[i]))));
    return scheme == "http" || scheme == "https";
}

/// Read an optional string field; trims; missing/empty → "".
std::string optString(const nlohmann::json& obj, const char* key)
{
    if (!obj.is_object())
        return {};
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string())
        return {};
    return trim(it->get<std::string>());
}

/// Parse a single pet entry. Returns false if the entry must be skipped
/// (non-object, or no usable slug).
bool parsePet(const nlohmann::json& j, PetdexPet& out)
{
    if (!j.is_object())
        return false;

    out = PetdexPet{};
    out.slug = optString(j, "slug");
    if (out.slug.empty())
        return false;   // slug is the stable selection key — required

    out.displayName = optString(j, "displayName");
    if (out.displayName.empty())
        out.displayName = out.slug;

    out.kind        = optString(j, "kind");
    out.submittedBy = optString(j, "submittedBy");
    out.spritesheetUrl = optString(j, "spritesheetUrl");
    out.petJsonUrl     = optString(j, "petJsonUrl");
    out.zipUrl         = optString(j, "zipUrl");

    // Defence: only keep URLs that look like http(s). Anything else is
    // garbage (e.g. "file://" or arbitrary text) and must not be used later.
    if (!looksLikeHttpUrl(out.spritesheetUrl)) out.spritesheetUrl.clear();
    if (!looksLikeHttpUrl(out.petJsonUrl))     out.petJsonUrl.clear();
    if (!looksLikeHttpUrl(out.zipUrl))         out.zipUrl.clear();

    const auto it = j.find("spriteVersionNumber");
    if (it != j.end() && it->is_number_integer())
        out.spriteVersionNumber = it->get<int>();
    else
        out.spriteVersionNumber = 1;

    return true;
}

/// Shared: parse a JSON array of pet entries into `result.manifest.pets`.
/// Entries that fail parsePet are counted in result.skipped.
void parsePetsInto(const nlohmann::json& arr, PetdexManifestParser::ParseResult& result)
{
    if (!arr.is_array())
    {
        result.error = "no 'pets' array in manifest";
        return;
    }
    result.manifest.pets.reserve(arr.size());
    for (const auto& entry : arr)
    {
        PetdexPet pet;
        if (parsePet(entry, pet))
            result.manifest.pets.push_back(std::move(pet));
        else
            ++result.skipped;
    }
    result.manifest.total = static_cast<int>(result.manifest.pets.size());
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PetdexManifestParser
// ---------------------------------------------------------------------------

PetdexManifestParser::ParseResult
PetdexManifestParser::parseManifest(const std::string& json)
{
    ParseResult result;
    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(json);
    }
    catch (const std::exception& e)
    {
        result.error = "invalid JSON: " + std::string(e.what());
        return result;
    }

    if (root.is_object())
    {
        const auto it = root.find("generatedAt");
        if (it != root.end() && it->is_string())
            result.manifest.generatedAt = it->get<std::string>();

        const auto petsIt = root.find("pets");
        if (petsIt == root.end())
        {
            result.error = "no 'pets' array in manifest";
            return result;
        }
        parsePetsInto(*petsIt, result);
    }
    else if (root.is_array())
    {
        // Tolerant of a bare top-level array.
        parsePetsInto(root, result);
    }
    else
    {
        result.error = "manifest root is neither an object nor an array";
        return result;
    }

    result.ok = result.error.empty();
    return result;
}

PetdexManifestParser::EnvelopeResult
PetdexManifestParser::parseCacheEnvelope(const std::string& json)
{
    EnvelopeResult result;
    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(json);
    }
    catch (const std::exception& e)
    {
        result.error = "invalid cache JSON: " + std::string(e.what());
        return result;
    }

    if (!root.is_object())
    {
        result.error = "cache envelope is not an object";
        return result;
    }

    try
    {
        if (root.value("version", std::string()) != kCacheVersion)
        {
            result.error = "unsupported cache version";
            return result;
        }
        result.fetchedAtEpochMs = root.value("fetchedAtEpochMs", std::int64_t(0));

        const auto it = root.find("manifest");
        if (it == root.end() || !it->is_object())
        {
            result.error = "cache envelope has no manifest object";
            return result;
        }

        ParseResult inner;
        const auto petsIt = it->find("pets");
        if (petsIt == it->end())
        {
            result.error = "cached manifest has no pets array";
            return result;
        }
        parsePetsInto(*petsIt, inner);
        result.manifest = std::move(inner.manifest);
    }
    catch (const std::exception& e)
    {
        result.error = "malformed cache envelope: " + std::string(e.what());
        return result;
    }

    result.ok = true;
    return result;
}

std::string PetdexManifestParser::makeCacheEnvelope(const PetdexManifest& manifest,
                                                    std::int64_t fetchedAtEpochMs)
{
    nlohmann::json pets = nlohmann::json::array();
    for (const auto& p : manifest.pets)
    {
        nlohmann::json j;
        j["slug"]                 = p.slug;
        j["displayName"]          = p.displayName;
        j["kind"]                 = p.kind;
        j["submittedBy"]          = p.submittedBy;
        j["spritesheetUrl"]       = p.spritesheetUrl;
        j["petJsonUrl"]           = p.petJsonUrl;
        j["zipUrl"]               = p.zipUrl;
        j["spriteVersionNumber"]  = p.spriteVersionNumber;
        pets.push_back(std::move(j));
    }

    nlohmann::json manifestJson;
    manifestJson["generatedAt"] = manifest.generatedAt;
    manifestJson["total"]       = static_cast<int>(manifest.pets.size());
    manifestJson["pets"]        = std::move(pets);

    nlohmann::json envelope;
    envelope["version"]         = kCacheVersion;
    envelope["fetchedAtEpochMs"] = fetchedAtEpochMs;
    envelope["manifest"]        = std::move(manifestJson);
    return envelope.dump();
}

} // namespace hathor::ui
