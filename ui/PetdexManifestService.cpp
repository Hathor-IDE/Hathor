// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PetdexManifestService.hpp"

#include "PetdexCacheStore.hpp"
#include "PetdexManifestParser.hpp"
#include "PetdexLoadPolicy.hpp"

// MessageManager::callAsync lives in juce_events (transitively linked via
// juce_gui_basics on hathor-ui).
#include <juce_events/juce_events.h>

#include <algorithm>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

PetdexManifestService::PetdexManifestService(juce::File cacheDir,
                                             std::string manifestUrl)
    : cacheDir_(std::move(cacheDir))
    , manifestUrl_(std::move(manifestUrl))
{
}

PetdexManifestService::~PetdexManifestService()
{
    cancelled_ = true;

    // Move the thread out before joining so the worker can still acquire
    // stateMtx_ while we wait for it to finish.
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        worker = std::move(worker_);
    }
    if (worker.joinable())
        worker.join();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void PetdexManifestService::setResultCallback(
    std::function<void(const PetdexManifestResult&)> callback)
{
    std::lock_guard<std::mutex> lock(stateMtx_);
    callback_ = std::move(callback);
}

void PetdexManifestService::start()
{
    beginFetch(false);
}

void PetdexManifestService::refresh()
{
    beginFetch(true);
}

PetdexManifestResult PetdexManifestService::current() const
{
    std::lock_guard<std::mutex> lock(stateMtx_);
    return current_;
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

void PetdexManifestService::beginFetch(bool force)
{
    std::lock_guard<std::mutex> lock(stateMtx_);
    if (fetchInFlight_)
        return;   // one fetch at a time; an in-flight refresh already covers it

    fetchInFlight_ = true;
    if (worker_.joinable())
        worker_.detach();   // previous fetch completed (fetchInFlight_ guard)

    worker_ = std::thread([this, force] { runBackground(force); });
}

void PetdexManifestService::runBackground(bool force)
{
    // -----------------------------------------------------------------------
    // 1. Local cache first — browsing never depends on the network.
    // -----------------------------------------------------------------------
    PetdexManifest cached;
    std::int64_t cachedAt = 0;
    bool haveCache = false;
    {
        const auto store = PetdexCacheStore::read(cacheDir_.getFullPathName().toStdString());
        if (store.present)
        {
            const auto envelope = PetdexManifestParser::parseCacheEnvelope(store.json);
            if (envelope.ok)
            {
                cached   = envelope.manifest;
                cachedAt = envelope.fetchedAtEpochMs;
                haveCache = true;
            }
            // A corrupt cache envelope is treated as "no cache" — it will be
            // overwritten on the next successful fetch.
        }
    }

    const std::int64_t now = juce::Time::currentTimeMillis();

    // -----------------------------------------------------------------------
    // 1b. Pre-fetch decision — serve the cache without any network if fresh.
    // -----------------------------------------------------------------------
    switch (PetdexLoadPolicy::planPreFetch(haveCache, force, cachedAt, now,
                                           kCacheMaxAgeMs))
    {
        case PetdexPreFetch::ServeFreshCache:
        {
            PetdexManifestResult r;
            r.status    = PetdexManifestStatus::Ready;
            r.manifest  = std::move(cached);
            r.fromCache = true;
            r.fetchedAtEpochMs = cachedAt;
            r.message   = "Catalog loaded from local cache.";
            deliver(r);
            finishFetch();
            return;
        }
        case PetdexPreFetch::ServeStaleCache:
        {
            // Keep the picker usable while the background refresh runs.
            PetdexManifestResult r;
            r.status    = PetdexManifestStatus::UsingCache;
            r.manifest  = cached;
            r.fromCache = true;
            r.fetchedAtEpochMs = cachedAt;
            r.message   = "Showing cached catalog \xE2\x80\x94 refreshing\xE2\x80\xA6";
            deliver(r);
            break;
        }
        case PetdexPreFetch::None:
            break;
    }

    // -----------------------------------------------------------------------
    // 2. Network fetch.
    // -----------------------------------------------------------------------
    const PetdexManifestResult fetched = fetchFromNetwork();
    if (cancelled_)
    {
        finishFetch();
        return;
    }

    const bool fetchSucceeded = (fetched.status == PetdexManifestStatus::Ready);
    if (fetchSucceeded)
    {
        const auto envelope = PetdexManifestParser::makeCacheEnvelope(
            fetched.manifest, fetched.fetchedAtEpochMs);
        PetdexCacheStore::write(cacheDir_.getFullPathName().toStdString(), envelope);
        deliver(fetched);
        finishFetch();
        return;
    }

    // -----------------------------------------------------------------------
    // 3. Fetch failed — degrade gracefully (stale cache stays in use).
    // -----------------------------------------------------------------------
    switch (PetdexLoadPolicy::planPostFetch(false, haveCache))
    {
        case PetdexPostFetch::KeepStaleCache:
        {
            PetdexManifestResult r;
            r.status    = PetdexManifestStatus::UsingCache;
            r.manifest  = std::move(cached);
            r.fromCache = true;
            r.fetchedAtEpochMs = cachedAt;
            r.message   = "Refresh failed \xE2\x80\x94 showing cached catalog. "
                        + fetched.message;
            deliver(r);
            break;
        }
        case PetdexPostFetch::Offline:
        {
            PetdexManifestResult r;
            r.status  = PetdexManifestStatus::Offline;
            r.message = fetched.message;
            deliver(r);
            break;
        }
        case PetdexPostFetch::ServeFetch:
            break;   // unreachable here (fetchSucceeded == false)
    }
    finishFetch();
}

PetdexManifestResult PetdexManifestService::fetchFromNetwork() const
{
    PetdexManifestResult result;

    juce::URL url(manifestUrl_);
    int statusCode = 0;
    auto stream = url.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(kConnectionTimeoutMs)
            .withNumRedirectsToFollow(4)
            .withExtraHeaders("User-Agent: Hathor (Petdex)\r\n")
            .withStatusCode(&statusCode));

    if (stream == nullptr)
    {
        result.status  = PetdexManifestStatus::Offline;
        result.message = "Cannot reach petdex.dev (network unavailable).";
        return result;
    }

    if (statusCode != 0 && statusCode != 200)
    {
        result.status  = PetdexManifestStatus::Offline;
        result.message = "Petdex server returned HTTP "
                       + juce::String(statusCode).toStdString() + ".";
        return result;
    }

    std::string body;
    body.reserve(256 * 1024);
    char buf[8192];
    for (;;)
    {
        const int n = stream->read(buf, static_cast<int>(sizeof(buf)));
        if (n <= 0)
            break;
        body.append(buf, static_cast<std::size_t>(n));
        if (body.size() > kMaxResponseBytes)
        {
            result.status  = PetdexManifestStatus::Offline;
            result.message = "Manifest response too large; ignored.";
            return result;
        }
    }

    if (body.empty())
    {
        result.status  = PetdexManifestStatus::Offline;
        result.message = "Empty manifest response.";
        return result;
    }

    const auto parsed = PetdexManifestParser::parseManifest(body);
    if (!parsed.ok)
    {
        result.status  = PetdexManifestStatus::Offline;
        result.message = "Malformed manifest: " + parsed.error;
        return result;
    }

    result.status    = PetdexManifestStatus::Ready;
    result.manifest  = parsed.manifest;
    result.fromCache = false;
    result.fetchedAtEpochMs = juce::Time::currentTimeMillis();
    result.message   = "Catalog ready \xE2\x80\x94 "
                     + std::to_string(parsed.manifest.pets.size())
                     + " pets from petdex.dev.";
    return result;
}

void PetdexManifestService::deliver(const PetdexManifestResult& result)
{
    std::function<void(const PetdexManifestResult&)> callback;
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        current_ = result;
        callback = callback_;
    }

    if (!callback)
        return;

    // Copy everything we need into the lambda — it must not touch `this`.
    const auto res = result;
    juce::MessageManager::callAsync([callback, res]() { callback(res); });
}

void PetdexManifestService::finishFetch()
{
    std::lock_guard<std::mutex> lock(stateMtx_);
    fetchInFlight_ = false;
}

} // namespace hathor::ui
