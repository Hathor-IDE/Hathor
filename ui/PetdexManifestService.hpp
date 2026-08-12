// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetdexManifestService.hpp — Petdex manifest fetch/cache service (Phase G / D1).
 *
 * The service boundary: the Settings UI (and later D2/D3) talks ONLY to this
 * class plus the JUCE-free model/parser/attribution types. It never sees URLs,
 * cache files, or threads. Petdex is deliberately NOT generalised into a
 * remote-resource framework — this service fetches exactly one manifest, caches
 * exactly one envelope, and knows exactly one platform.
 *
 * Threading contract (PROGRAM.md D1 requirement 2/5):
 *   - The JUCE message thread and audio thread are never blocked.
 *   - All work (cache I/O, HTTP, parsing) runs on a detached background thread.
 *   - Results are delivered via juce::MessageManager::callAsync to the callback
 *     registered with setResultCallback(). The callback fires on the message
 *     thread; the caller must guard `this` (e.g. Component::SafePointer).
 *   - start()/refresh() may be called from the message thread at any time.
 *
 * Opt-in contract (decision #5):
 *   - The service does NO network work in its constructor. Nothing happens
 *     until start() is called — which the Settings UI does only when the user
 *     opens the Settings tab. Merely starting Hathor never downloads anything.
 *   - Pet selection (a persisted slug) is strictly separate from the catalog
 *     state delivered here (requirement 11). No pet resources are downloaded.
 *
 * Cache/invalidation policy (requirement 3/4):
 *   - Local cache at <userApplicationDataDirectory>/Hathor/Petdex/manifest.json.
 *   - Fresh cache (< kCacheMaxAgeMs old) is served without any network call.
 *   - A stale cache is served immediately (picker stays usable) while a
 *     background refresh runs; on failure the stale cache remains in use with
 *     an explanatory message. No cache + network failure = Offline status.
 *   - refresh() forces a network fetch (the Settings "Refresh catalog" button).
 *
 * Platform caveat (documented in docs/design/petdex-d1-d4-decision.md):
 *   juce::URL works on macOS (NSURLSession) and Windows (WinInet). On Linux,
 *   JUCE lazily loads libcurl symbols; without curl installed the fetch fails
 *   cleanly into the Offline/UsingCache paths — never a hang or a crash.
 */

#include "PetdexTypes.hpp"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace hathor::ui {

class PetdexManifestService
{
public:
    static constexpr const char* kDefaultManifestUrl = "https://petdex.dev/api/manifest";
    static constexpr std::int64_t kCacheMaxAgeMs      = std::int64_t(24) * 60 * 60 * 1000; // 24 h
    static constexpr std::int64_t kMaxResponseBytes   = 8 * 1024 * 1024;
    static constexpr int          kConnectionTimeoutMs = 15000;

    /**
     * @param cacheDir     Directory for the manifest cache file.
     * @param manifestUrl  Manifest endpoint (overridable for tests).
     */
    explicit PetdexManifestService(juce::File cacheDir,
                                   std::string manifestUrl = kDefaultManifestUrl);

    /// Cancels and joins any in-flight fetch. May block up to the network
    /// timeout at application shutdown if a fetch was in progress.
    ~PetdexManifestService();

    /// Register the result callback (message thread delivery).
    void setResultCallback(std::function<void(const PetdexManifestResult&)> callback);

    /// Begin loading: serve a fresh cache if present, otherwise fetch.
    /// Safe to call repeatedly (each Settings-tab open); a fresh cache means
    /// no network traffic. No-op while a fetch is already in flight.
    void start();

    /// Force a network refresh, bypassing cache freshness (Refresh button).
    /// No-op while a fetch is already in flight.
    void refresh();

    /// The last delivered result (or Idle if none yet).
    PetdexManifestResult current() const;

private:
    void beginFetch(bool force);
    void runBackground(bool force);
    PetdexManifestResult fetchFromNetwork() const;
    void deliver(const PetdexManifestResult& result);
    void finishFetch();

    juce::File cacheDir_;
    std::string manifestUrl_;

    mutable std::mutex stateMtx_;
    std::function<void(const PetdexManifestResult&)> callback_;
    bool fetchInFlight_ = false;
    PetdexManifestResult current_;

    std::thread worker_;
    std::atomic<bool> cancelled_{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PetdexManifestService)
};

} // namespace hathor::ui
