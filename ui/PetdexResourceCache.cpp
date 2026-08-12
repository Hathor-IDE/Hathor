// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PetdexResourceCache.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <system_error>

namespace hathor::ui {

namespace {

std::filesystem::path petsDir(const std::filesystem::path& root)
{
    return root / PetdexResourceCache::kPetsSubdir;
}

} // anonymous namespace

std::filesystem::path PetdexResourceCache::petDir(const std::filesystem::path& root,
                                                  const std::string& slug)
{
    return petsDir(root) / sanitizeSlug(slug);
}

std::filesystem::path PetdexResourceCache::spritePath(const std::filesystem::path& root,
                                                      const std::string& slug)
{
    return petDir(root, slug) / kSpriteName;
}

std::filesystem::path PetdexResourceCache::attributionPath(const std::filesystem::path& root,
                                                           const std::string& slug)
{
    return petDir(root, slug) / kAttributionName;
}

std::string PetdexResourceCache::sanitizeSlug(const std::string& slug)
{
    std::string out;
    out.reserve(slug.size());
    for (char c : slug)
    {
        const bool safe = (c >= 'a' && c <= 'z')
                       || (c >= 'A' && c <= 'Z')
                       || (c >= '0' && c <= '9')
                       || c == '-' || c == '_' || c == '.';
        out.push_back(safe ? c : '_');
    }
    if (out.empty())
        out = "_";
    // A slug of exactly ".." would alias the pet directory's parent —
    // neutralise it (dots otherwise stay legal in slugs such as "x_1.2").
    if (out == "..")
        out = "_";
    return out;
}

bool PetdexResourceCache::hasSprite(const std::filesystem::path& root,
                                    const std::string& slug)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(spritePath(root, slug), ec);
}

bool PetdexResourceCache::readSprite(const std::filesystem::path& root,
                                     const std::string& slug,
                                     std::vector<std::uint8_t>& outBytes)
{
    std::ifstream f(spritePath(root, slug), std::ios::binary);
    if (!f)
        return false;
    outBytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !outBytes.empty();
}

bool PetdexResourceCache::writeSprite(const std::filesystem::path& root,
                                      const std::string& slug,
                                      const std::uint8_t* data,
                                      std::size_t size)
{
    std::error_code ec;
    std::filesystem::create_directories(petDir(root, slug), ec);
    if (ec)
        return false;

    std::ofstream f(spritePath(root, slug), std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    f.flush();
    return f.good();
}

bool PetdexResourceCache::removeSprite(const std::filesystem::path& root,
                                       const std::string& slug)
{
    std::error_code ec;
    return std::filesystem::remove(spritePath(root, slug), ec);
}

bool PetdexResourceCache::writeAttribution(const std::filesystem::path& root,
                                           const std::string& slug,
                                           const PetdexAttributionSnapshot& snapshot)
{
    nlohmann::json j;
    j["version"]      = 1;
    j["canDisplay"]   = snapshot.canDisplay;
    j["slug"]         = snapshot.slug;
    j["displayName"]  = snapshot.displayName;
    j["submitter"]    = snapshot.submitter;
    j["creditLine"]   = snapshot.creditLine;
    j["notice"]       = snapshot.notice;
    j["spritesheetUrl"] = snapshot.spritesheetUrl;

    std::error_code ec;
    std::filesystem::create_directories(petDir(root, slug), ec);
    if (ec)
        return false;

    std::ofstream f(attributionPath(root, slug), std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f << j.dump();
    f.flush();
    return f.good();
}

bool PetdexResourceCache::readAttribution(const std::filesystem::path& root,
                                          const std::string& slug,
                                          PetdexAttributionSnapshot& outSnapshot)
{
    std::ifstream f(attributionPath(root, slug), std::ios::binary);
    if (!f)
        return false;

    std::ostringstream ss;
    ss << f.rdbuf();

    try
    {
        const auto j = nlohmann::json::parse(ss.str());
        if (!j.is_object() || j.value("version", 0) != 1)
            return false;
        outSnapshot.canDisplay   = j.value("canDisplay", false);
        outSnapshot.slug         = j.value("slug", std::string());
        outSnapshot.displayName  = j.value("displayName", std::string());
        outSnapshot.submitter    = j.value("submitter", std::string());
        outSnapshot.creditLine   = j.value("creditLine", std::string());
        outSnapshot.notice       = j.value("notice", std::string());
        outSnapshot.spritesheetUrl = j.value("spritesheetUrl", std::string());
    }
    catch (const std::exception&)
    {
        return false;   // corrupt snapshot — display stays blocked (D4)
    }

    return true;
}

} // namespace hathor::ui
