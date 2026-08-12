// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetdexLoadPolicy.hpp — JUCE-free decision logic for the Petdex manifest load.
 *
 * PetdexManifestService applies these two pure decisions when a catalog load
 * is requested (D1 requirements 3–5: cache first, refresh on staleness,
 * graceful failure). Extracted so the branching is unit-testable headless:
 *
 *   planPreFetch  — what to deliver BEFORE any network I/O:
 *       ServeFreshCache  cache is fresh → serve it, no network at all
 *       ServeStaleCache  stale cache (or forced refresh) → serve immediately,
 *                        then refresh in the background
 *       None             no usable cache → fetch directly
 *
 *   planPostFetch — what to deliver AFTER a fetch attempt:
 *       ServeFetch       fetch succeeded → deliver it (and refresh the cache)
 *       KeepStaleCache   fetch failed but a cache exists → keep using it,
 *                        with an explanatory message
 *       Offline          fetch failed and no cache → cannot browse
 *
 * The policy stays deliberately outside any networking/cache implementation so
 * the UI and tests never depend on how fetching works.
 */

#include "PetdexCacheStore.hpp"

#include <cstdint>

namespace hathor::ui {

enum class PetdexPreFetch
{
    None,
    ServeFreshCache,
    ServeStaleCache,
};

enum class PetdexPostFetch
{
    ServeFetch,
    KeepStaleCache,
    Offline,
};

class PetdexLoadPolicy
{
public:
    /// Decision before any network work.
    static PetdexPreFetch planPreFetch(bool haveCache,
                                       bool forceRefresh,
                                       std::int64_t cachedAtEpochMs,
                                       std::int64_t nowEpochMs,
                                       std::int64_t maxAgeMs) noexcept
    {
        if (!haveCache)
            return PetdexPreFetch::None;
        if (!forceRefresh
            && !PetdexCacheStore::isStale(cachedAtEpochMs, nowEpochMs, maxAgeMs))
        {
            return PetdexPreFetch::ServeFreshCache;
        }
        return PetdexPreFetch::ServeStaleCache;
    }

    /// Decision after a fetch attempt.
    static PetdexPostFetch planPostFetch(bool fetchSucceeded,
                                         bool hadCache) noexcept
    {
        if (fetchSucceeded)
            return PetdexPostFetch::ServeFetch;
        return hadCache ? PetdexPostFetch::KeepStaleCache : PetdexPostFetch::Offline;
    }
};

} // namespace hathor::ui
