// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PetdexCacheStore.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

namespace hathor::ui {

std::filesystem::path
PetdexCacheStore::manifestPath(const std::filesystem::path& dir)
{
    return dir / kFileName;
}

PetdexCacheStore::ReadResult
PetdexCacheStore::read(const std::filesystem::path& dir)
{
    ReadResult result;
    const auto path = manifestPath(dir);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return result;

    std::ifstream f(path, std::ios::binary);
    if (!f)
        return result;

    std::ostringstream ss;
    ss << f.rdbuf();
    result.present = true;
    result.json = ss.str();
    return result;
}

bool PetdexCacheStore::write(const std::filesystem::path& dir,
                             const std::string& envelopeJson)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return false;

    std::ofstream f(manifestPath(dir), std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f << envelopeJson;
    f.flush();
    return f.good();
}

std::int64_t PetdexCacheStore::ageMs(std::int64_t fetchedAtEpochMs,
                                     std::int64_t nowEpochMs)
{
    const auto diff = nowEpochMs - fetchedAtEpochMs;
    return diff < 0 ? 0 : diff;
}

bool PetdexCacheStore::isStale(std::int64_t fetchedAtEpochMs,
                               std::int64_t nowEpochMs,
                               std::int64_t maxAgeMs)
{
    return ageMs(fetchedAtEpochMs, nowEpochMs) >= maxAgeMs;
}

} // namespace hathor::ui
