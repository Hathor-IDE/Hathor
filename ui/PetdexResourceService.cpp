// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PetdexResourceService.hpp"

#include "PetdexHttp.hpp"
#include "PetdexResourceCache.hpp"
#include "PetdexWebpDecoder.hpp"

// MessageManager::callAsync lives in juce_events (transitively linked via
// juce_gui_basics on hathor-ui).
#include <juce_events/juce_events.h>

#include <algorithm>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

PetdexResourceService::PetdexResourceService(juce::File cacheDir)
    : cacheDir_(std::move(cacheDir))
    , cacheDirPath_(cacheDir_.getFullPathName().toStdString())
{
}

PetdexResourceService::~PetdexResourceService()
{
    cancelled_ = true;

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

void PetdexResourceService::setResultCallback(
    std::function<void(const PetdexSpriteResult&)> callback)
{
    std::lock_guard<std::mutex> lock(stateMtx_);
    callback_ = std::move(callback);
}

void PetdexResourceService::loadPet(const std::string& slug, const std::string& spritesheetUrl)
{
    beginLoad(slug, spritesheetUrl);
}

bool PetdexResourceService::hasCachedPet(const std::string& slug) const
{
    return PetdexResourceCache::hasSprite(cacheDirPath_, slug);
}

bool PetdexResourceService::readAttribution(const std::string& slug,
                                            PetdexAttributionSnapshot& out) const
{
    return PetdexResourceCache::readAttribution(cacheDirPath_, slug, out);
}

void PetdexResourceService::writeAttribution(const std::string& slug,
                                             const PetdexAttributionSnapshot& snapshot)
{
    PetdexResourceCache::writeAttribution(cacheDirPath_, slug, snapshot);
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

void PetdexResourceService::beginLoad(const std::string& slug, const std::string& url)
{
    std::lock_guard<std::mutex> lock(stateMtx_);
    if (loadInFlight_)
    {
        // Remember the LATEST request; finishLoad() starts it when the
        // in-flight load completes, so a fast selection change is never
        // silently dropped (the widget would otherwise stay Loading).
        pendingSlug_ = slug;
        pendingUrl_  = url;
        return;
    }

    loadInFlight_ = true;
    startWorkerLocked(slug, url);
}

void PetdexResourceService::startWorkerLocked(const std::string& slug,
                                              const std::string& url)
{
    // Caller holds stateMtx_ and has set loadInFlight_ = true.
    if (worker_.joinable())
        worker_.detach();

    worker_ = std::thread([this, slug, url] { runLoad(slug, url); });
}

void PetdexResourceService::runLoad(const std::string& slug, const std::string& url)
{
    // 1. In-memory decoded cache (same pet loaded recently) — no decode at all.
    {
        PetdexSpriteResult cached;
        if (tryInMemory(slug, cached))
        {
            deliver(cached);
            finishLoad();
            return;
        }
    }

    // 2. Disk cache — read the raw WebP and decode.
    {
        PetdexSpriteResult fromDisk;
        bool corrupt = false;
        if (tryDisk(slug, fromDisk, corrupt))
        {
            deliver(fromDisk);
            finishLoad();
            return;
        }
        if (corrupt)
            PetdexResourceCache::removeSprite(cacheDirPath_, slug);   // re-download below
    }

    // 3. Network.
    const PetdexSpriteResult fetched = downloadAndDecode(slug, url);
    if (cancelled_)
    {
        finishLoad();
        return;
    }
    deliver(fetched);
    finishLoad();
}

bool PetdexResourceService::tryInMemory(const std::string& slug, PetdexSpriteResult& out)
{
    std::lock_guard<std::mutex> lock(stateMtx_);
    if (cachedSlug_ == slug && cachedSprite_ != nullptr)
    {
        out = *cachedSprite_;
        return true;
    }
    return false;
}

bool PetdexResourceService::tryDisk(const std::string& slug,
                                    PetdexSpriteResult& out,
                                    bool& corruptOut)
{
    std::vector<std::uint8_t> bytes;
    if (!PetdexResourceCache::readSprite(cacheDirPath_, slug, bytes))
        return false;

    const auto decoded = PetdexWebpDecoder::decode(bytes.data(), bytes.size());
    if (!decoded.ok)
    {
        corruptOut = true;   // cached bytes exist but won't decode
        return false;
    }

    out.ok     = true;
    out.slug   = slug;
    out.width  = decoded.width;
    out.height = decoded.height;
    out.rgba   = std::move(decoded.rgba);

    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        cachedSlug_   = slug;
        cachedSprite_ = std::make_shared<PetdexSpriteResult>(out);
    }
    return true;
}

PetdexSpriteResult PetdexResourceService::downloadAndDecode(const std::string& slug,
                                                            const std::string& url)
{
    PetdexSpriteResult result;
    result.slug = slug;

    const auto http = PetdexHttp::get(url, kMaxSpriteBytes, kConnectionTimeoutMs,
                                      "Hathor (Petdex sprite)");
    switch (http.error)
    {
        case PetdexHttp::Error::Network:
            result.error = "Cannot download the pet sprite (network unavailable).";
            return result;
        case PetdexHttp::Error::Http:
            result.error = "Downloading the pet sprite failed (HTTP "
                         + std::to_string(http.statusCode) + ").";
            return result;
        case PetdexHttp::Error::TooLarge:
            result.error = "Downloaded sprite too large; ignored.";
            return result;
        case PetdexHttp::Error::Empty:
            result.error = "Empty sprite response.";
            return result;
        case PetdexHttp::Error::None:
            break;
    }

    // Persist the raw WebP so the pet works offline on later launches.
    PetdexResourceCache::writeSprite(cacheDirPath_, slug,
                                     reinterpret_cast<const std::uint8_t*>(http.body.data()),
                                     http.body.size());

    const auto decoded = PetdexWebpDecoder::decode(
        reinterpret_cast<const std::uint8_t*>(http.body.data()), http.body.size());
    if (!decoded.ok)
    {
        result.error = "The downloaded sprite could not be decoded (corrupt file).";
        return result;
    }

    result.ok     = true;
    result.width  = decoded.width;
    result.height = decoded.height;
    result.rgba   = std::move(decoded.rgba);

    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        cachedSlug_   = slug;
        cachedSprite_ = std::make_shared<PetdexSpriteResult>(result);
    }
    return result;
}

void PetdexResourceService::deliver(const PetdexSpriteResult& result)
{
    std::function<void(const PetdexSpriteResult&)> callback;
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        callback = callback_;
    }

    if (!callback)
        return;

    const auto res = result;
    juce::MessageManager::callAsync([callback, res]() { callback(res); });
}

void PetdexResourceService::finishLoad()
{
    std::string slug, url;
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        loadInFlight_ = false;
        if (!pendingSlug_.empty())
        {
            // A newer request arrived while this load ran — start it now,
            // under the same lock so no intervening loadPet can steal the slot.
            slug = pendingSlug_;
            url  = pendingUrl_;
            pendingSlug_.clear();
            pendingUrl_.clear();
            loadInFlight_ = true;
            startWorkerLocked(slug, url);
        }
    }
}

} // namespace hathor::ui
