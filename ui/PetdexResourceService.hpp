// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetdexResourceService.hpp — async sprite acquisition + decoding (Phase G / D2).
 *
 * Fetches the SELECTED pet's spritesheet (the manifest's `spritesheetUrl`,
 * a WebP) on a background thread, caches the raw WebP on disk, decodes it to
 * RGBA via libwebp, and delivers the result on the JUCE message thread.
 *
 * Evidence-based fetch choice (docs/design/petdex-d1-d4-decision.md): the
 * `zipUrl` package adds nothing but a redundant pet.json (its spritesheet is
 * the same WebP), so the direct spritesheet URL is fetched — no zip handling
 * needed, and WebP decoding is genuinely required (JUCE 8.0.4 has none).
 *
 * Contract (same service-boundary discipline as PetdexManifestService):
 *   - Strictly opt-in: no network work until loadPet() is called, which the UI
 *     only does after the user explicitly applies a selection.
 *   - The message/audio threads are never blocked; results arrive via
 *     MessageManager::callAsync on the registered callback (message thread).
 *   - Corrupt disk cache → detected at decode time, removed, re-downloaded.
 *   - Repeated decodes of the same asset are avoided via a single-slot
 *     in-memory decoded-sprite cache (replaced when the selection changes).
 *   - All decode/cache helpers are JUCE-free and unit-tested; this class is
 *     thin orchestration on top of them.
 *
 * Attribution snapshots are persisted here too (write/read), so the D4 gate
 * can re-run on later launches without the manifest or network.
 */

#include "PetdexTypes.hpp"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hathor::ui {

// PetdexSpriteResult is defined in PetdexTypes.hpp (shared with PetWidget and
// the unit tests).

class PetdexResourceService
{
public:
    static constexpr std::int64_t kMaxSpriteBytes  = 32LL * 1024 * 1024;
    static constexpr int          kConnectionTimeoutMs = 20000;

    /// @param cacheDir  Petdex cache root (same as the manifest service).
    explicit PetdexResourceService(juce::File cacheDir);
    ~PetdexResourceService();

    // Copy/move ops deleted via JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR.

    /// Register the result callback (message thread delivery).
    void setResultCallback(std::function<void(const PetdexSpriteResult&)> callback);

    /// Asynchronously load the sprite for (slug, spritesheetUrl): in-memory
    /// cache → disk cache → network. One load at a time; if a load is already
    /// in flight the LATEST request is remembered and started when it
    /// finishes, so a fast selection change can never strand the UI.
    void loadPet(const std::string& slug, const std::string& spritesheetUrl);

    /// True if the pet's sprite is already on disk (offline launch check).
    bool hasCachedPet(const std::string& slug) const;

    /// Read the persisted D4 attribution snapshot (false if none/corrupt).
    bool readAttribution(const std::string& slug, PetdexAttributionSnapshot& out) const;

    /// Persist the D4 attribution snapshot (called at Apply time).
    void writeAttribution(const std::string& slug, const PetdexAttributionSnapshot& snapshot);

private:
    void beginLoad(const std::string& slug, const std::string& url);
    void runLoad(const std::string& slug, const std::string& url);
    bool tryInMemory(const std::string& slug, PetdexSpriteResult& out);
    bool tryDisk(const std::string& slug, PetdexSpriteResult& out, bool& corruptOut);
    // Mutates the in-memory decoded-sprite cache, so it is intentionally
    // non-const (cache writes are mutex-guarded; see the implementation).
    PetdexSpriteResult downloadAndDecode(const std::string& slug,
                                         const std::string& url);
    void deliver(const PetdexSpriteResult& result);
    void finishLoad();

    /// Start a worker for (slug, url). Caller must hold stateMtx_ and have
    /// already set loadInFlight_ = true.
    void startWorkerLocked(const std::string& slug, const std::string& url);

    juce::File cacheDir_;
    std::string cacheDirPath_;

    mutable std::mutex stateMtx_;
    std::function<void(const PetdexSpriteResult&)> callback_;
    bool loadInFlight_ = false;

    // Latest request that arrived while a load was in flight; started by
    // finishLoad() so a fast selection change is never dropped.
    std::string pendingSlug_;
    std::string pendingUrl_;

    // Single-slot in-memory decoded-sprite cache (avoids re-decoding).
    std::string cachedSlug_;
    std::shared_ptr<const PetdexSpriteResult> cachedSprite_;

    std::thread worker_;
    std::atomic<bool> cancelled_{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PetdexResourceService)
};

} // namespace hathor::ui
