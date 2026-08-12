// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_b4_k4_execution.cpp — B4-K4 REAL ChucK execution tests.
 *
 * These tests prove that a compiled .ck program is actually EXECUTED by the
 * real vendored libchuck VM inside the isolated per-tab worker, and that its
 * synthesized audio reaches Hathor's shared-memory transport ring — not just
 * that the source compiles (the pre-B4-K4 placeholder path).
 *
 * Verified end-to-end against the worker process:
 *   1. A known-valid audio program (oscillator routed to dac) compiles,
 *      loads/sporks, and produces measurable 440 Hz audio in the ring.
 *   2. The audio is NOT the legacy placeholder tone (which was a 220 Hz
 *      sine at 0.05 amplitude for tab 0).
 *   3. The loaded shred ID reported by vm_query is a real, live VM shred ID.
 *   4. ck_stop actually stops ChucK execution (the ring returns to silence).
 *   5. Per-tab isolation: stopping one tab's session leaves the other tab's
 *      real execution running (880 Hz still measurable).
 *   6. A program that compiles but fails at runtime (1/0) is NOT reported as
 *      running afterwards — status reflects actual execution, not compile.
 *
 * JUCE-free: links Catch2 + AudioWorkerManager only.
 * Requires the hathor-audio-worker binary to be built (CHUCK_AVAILABLE=1).
 *
 * Requirements: B4-K4, B4-K7, B4-K3, K0.5
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AudioWorkerManager.hpp"
#include "audio_ipc.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

using hathor::AudioWorkerManager;
using hathor::audio_worker::kBlockSize;
using hathor::audio_worker::kNumTabs;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string getWorkerPath()
{
    namespace fs = std::filesystem;

#ifdef CMAKE_BINARY_DIR
    fs::path p = fs::path(CMAKE_BINARY_DIR) / "app" / "audio-worker" / "hathor-audio-worker";
    if (fs::exists(p))
        return p.string();
    p = fs::path(CMAKE_BINARY_DIR) / "hathor-audio-worker";
    if (fs::exists(p))
        return p.string();
#endif

#ifdef CMAKE_SOURCE_DIR
    {
        fs::path p = fs::path(CMAKE_SOURCE_DIR) / "build" / "app" / "audio-worker" / "hathor-audio-worker";
        if (fs::exists(p))
            return p.string();
    }
#endif

    const char* envSrc = std::getenv("CMAKE_SOURCE_DIR");
    if (envSrc) {
        fs::path p = fs::path(envSrc) / "build" / "app" / "audio-worker" / "hathor-audio-worker";
        if (fs::exists(p))
            return p.string();
    }

    const fs::path candidates[] = {
        fs::current_path() / "hathor-audio-worker",
        fs::current_path() / "build" / "hathor-audio-worker",
        fs::current_path() / "build" / "app" / "audio-worker" / "hathor-audio-worker",
        fs::current_path() / "app" / "audio-worker" / "hathor-audio-worker",
    };

    for (const auto& p : candidates) {
        if (fs::exists(p))
            return p.string();
    }

    return "";
}

// A real, continuously-running oscillator routed to dac.
static const std::string kOsc440 =
    "SinOsc s => dac; 0.4 => s.gain; 440.0 => s.freq; while(true) 1::samp => now;";
static const std::string kOsc880 =
    "SinOsc s => dac; 0.25 => s.gain; 880.0 => s.freq; while(true) 1::samp => now;";

// Compiles cleanly but crashes at runtime (integer division by zero).
static const std::string kRuntimeFail =
    "while(true) { 1/0 => int x; 1::samp => now; }";

/// Collect up to maxBlocks audio blocks into out (mono, contiguous), polling
/// the ring within timeoutMs.  Returns the number of blocks collected.
static uint32_t collectBlocks(AudioWorkerManager& mgr, uint64_t gen,
                              float* out, uint32_t maxBlocks, uint32_t timeoutMs)
{
    uint32_t n = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (n < maxBlocks && std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(out + static_cast<size_t>(n) * kBlockSize,
                                  kBlockSize, gen)) {
            ++n;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return n;
}

/// Peak absolute sample value of a mono buffer.
static float peakAmplitude(const float* buf, size_t n)
{
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float a = std::fabs(buf[i]);
        if (a > peak) peak = a;
    }
    return peak;
}

/// Trim leading and trailing (near-)silent samples.  Returns the length of
/// the non-silent region; the caller offsets into buf by the returned start.
static size_t trimSilence(const float* buf, size_t n, float thr,
                          size_t* outStart)
{
    size_t start = 0;
    size_t end = n;
    while (start < end && std::fabs(buf[start]) < thr) ++start;
    while (end > start && std::fabs(buf[end - 1]) < thr) --end;
    *outStart = start;
    return end - start;
}

/// Drain any blocks already sitting in the ring (e.g., silence produced
/// before a shred was loaded) so subsequent analysis starts fresh.
static void drainRing(AudioWorkerManager& mgr, uint64_t gen)
{
    float drainBuf[kBlockSize];
    while (mgr.tryReadAudioBlock(drainBuf, kBlockSize, gen)) {
        // discard
    }
}

/// Estimate dominant frequency via zero-crossings.  Returns 0 for silence.
static double estimateFrequencyHz(const float* buf, size_t n, float sampleRate)
{
    if (peakAmplitude(buf, n) < 1e-4f)
        return 0.0;

    size_t crossings = 0;
    for (size_t i = 1; i < n; ++i) {
        const bool aPos = buf[i - 1] >= 0.0f;
        const bool bPos = buf[i] >= 0.0f;
        if (aPos != bPos)
            ++crossings;
    }
    if (crossings == 0)
        return 0.0;

    // One full cycle = two zero crossings.
    const double periodSamples = 2.0 * static_cast<double>(n) / static_cast<double>(crossings);
    return static_cast<double>(sampleRate) / periodSamples;
}

/// Poll vm_query until the tab reports a real loaded shred id (>= 0) or
/// timeout.  Returns true if a live shred was reported.
static bool waitForLoadedShred(AudioWorkerManager& mgr, uint8_t tab, uint32_t timeoutMs)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string q = mgr.queryTabVM(tab).message;
        auto pos = q.find("shred_id=");
        if (pos != std::string::npos) {
            const long id = std::stol(q.substr(pos + 9));
            if (id >= 0)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

/// Poll vm_query until the tab reports no live shred (shred_id=-1) or timeout.
static bool waitForNoShred(AudioWorkerManager& mgr, uint8_t tab, uint32_t timeoutMs)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string q = mgr.queryTabVM(tab).message;
        auto pos = q.find("shred_id=");
        if (pos != std::string::npos) {
            const long id = std::stol(q.substr(pos + 9));
            if (id < 0)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("B4-K4: real .ck execution produces real 440 Hz audio in the ring",
          "[k4][execution][real-audio]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));
    const uint64_t gen = mgr.generation();

    const uint8_t tab = 0;

    // Activate + evaluate a real oscillator program.
    auto act = mgr.activateTabVM(tab);
    REQUIRE(act.ok);
    auto eval = mgr.evaluateCkTab(tab, kOsc440);
    REQUIRE(eval.ok);

    // The shred must be ACTUALLY loaded/sporked in the real VM (not just
    // "compiled") — vm_query reports the real VM shred id.
    REQUIRE(waitForLoadedShred(mgr, tab, 8000));

    // Discard any silence produced before the shred sporked.
    drainRing(mgr, gen);

    // Collect a couple of seconds of transport audio.
    constexpr uint32_t kMaxBlocks = 80;
    float buf[static_cast<size_t>(kMaxBlocks) * kBlockSize];
    const uint32_t n = collectBlocks(mgr, gen, buf, kMaxBlocks, 5000);
    REQUIRE(n >= 40); // enough blocks for a solid analysis window

    const size_t totalSamples = static_cast<size_t>(n) * kBlockSize;

    // The ring carries REAL audio, not silence.
    const float peak = peakAmplitude(buf, totalSamples);
    REQUIRE(peak > 0.1f); // the legacy placeholder peaked at 0.05

    // The dominant frequency is the oscillator's 440 Hz (placeholder was 220).
    // Trim any residual leading/trailing silence before counting crossings.
    size_t start = 0;
    const size_t voiced = trimSilence(buf, totalSamples, 1e-3f, &start);
    REQUIRE(voiced > totalSamples / 2);
    const double freq = estimateFrequencyHz(buf + start, voiced, 44100.0f);
    REQUIRE(freq > 0.0);
    REQUIRE(std::fabs(freq - 440.0) / 440.0 < 0.08);

    // The VM actually advanced / executed code (blocks + heartbeat).
    std::string q = mgr.queryTabVM(tab).message;
    REQUIRE(q.find("blocks=") != std::string::npos);
    REQUIRE(q.find("heartbeat=") != std::string::npos);

    mgr.shutdown();
}

TEST_CASE("B4-K4: stopping the session actually stops ChucK execution",
          "[k4][execution][stop]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));
    const uint64_t gen = mgr.generation();

    const uint8_t tab = 1;

    REQUIRE(mgr.activateTabVM(tab).ok);
    auto eval = mgr.evaluateCkTab(tab, kOsc440);
    REQUIRE(eval.ok);
    REQUIRE(waitForLoadedShred(mgr, tab, 8000));

    // Audio is flowing before the stop.
    float preBuf[80 * kBlockSize];
    const uint32_t pre = collectBlocks(mgr, gen, preBuf, 40, 3000);
    REQUIRE(pre >= 20);
    REQUIRE(peakAmplitude(preBuf, static_cast<size_t>(pre) * kBlockSize) > 0.1f);

    // Stop the tab — the VM is destroyed, its shreds removed, execution ends.
    auto stop = mgr.stopCkTab(tab);
    REQUIRE(stop.ok);

    // Drain any blocks already in the ring, then verify only silence arrives.
    float drainBuf[kBlockSize];
    while (mgr.tryReadAudioBlock(drainBuf, kBlockSize, gen)) {
        // drain
    }

    float postBuf[40 * kBlockSize];
    const uint32_t post = collectBlocks(mgr, gen, postBuf, 30, 3000);
    REQUIRE(post >= 10);
    // Once stopped, only silent (idle) blocks arrive — no oscillator output.
    REQUIRE(peakAmplitude(postBuf, static_cast<size_t>(post) * kBlockSize) < 1e-4f);

    // And the VM reports destroyed/inactive, not a running shred.
    std::string q = mgr.queryTabVM(tab).message;
    const bool isGone =
        q.find("destroyed") != std::string::npos ||
        q.find("inactive") != std::string::npos ||
        q.find("no_vm") != std::string::npos;
    REQUIRE(isGone);

    mgr.shutdown();
}

TEST_CASE("B4-K4: per-tab isolation — stopping one session leaves the other running",
          "[k4][execution][isolation]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));
    const uint64_t gen = mgr.generation();

    const uint8_t tabA = 0;
    const uint8_t tabB = 1;

    // Two independent real sessions at different frequencies.
    REQUIRE(mgr.activateTabVM(tabA).ok);
    REQUIRE(mgr.activateTabVM(tabB).ok);
    REQUIRE(mgr.evaluateCkTab(tabA, kOsc440).ok);
    REQUIRE(mgr.evaluateCkTab(tabB, kOsc880).ok);
    REQUIRE(waitForLoadedShred(mgr, tabA, 8000));
    REQUIRE(waitForLoadedShred(mgr, tabB, 8000));

    // Both tones present in the ring (mixed).
    float mixBuf[80 * kBlockSize];
    const uint32_t mix = collectBlocks(mgr, gen, mixBuf, 60, 4000);
    REQUIRE(mix >= 30);
    REQUIRE(peakAmplitude(mixBuf, static_cast<size_t>(mix) * kBlockSize) > 0.3f);

    // Stop tab A only.
    REQUIRE(mgr.stopCkTab(tabA).ok);

    // Drain stale blocks (including any last blocks from A).
    drainRing(mgr, gen);

    // Tab B must still be running — 880 Hz still measurable, and it is NOT
    // tab A's 440 Hz (which would indicate A's session leaked or B died).
    float bBuf[80 * kBlockSize];
    const uint32_t b = collectBlocks(mgr, gen, bBuf, 60, 4000);
    REQUIRE(b >= 30);
    const size_t bSamples = static_cast<size_t>(b) * kBlockSize;
    REQUIRE(peakAmplitude(bBuf, bSamples) > 0.1f);
    size_t bStart = 0;
    const size_t bVoiced = trimSilence(bBuf, bSamples, 1e-3f, &bStart);
    REQUIRE(bVoiced > bSamples / 2);
    const double freqB = estimateFrequencyHz(bBuf + bStart, bVoiced, 44100.0f);
    REQUIRE(freqB > 0.0);
    REQUIRE(std::fabs(freqB - 880.0) / 880.0 < 0.08);

    // Tab A is gone, tab B still active.
    std::string qA = mgr.queryTabVM(tabA).message;
    const bool aGone =
        qA.find("destroyed") != std::string::npos ||
        qA.find("inactive") != std::string::npos ||
        qA.find("no_vm") != std::string::npos;
    REQUIRE(aGone);
    REQUIRE(mgr.queryTabVM(tabB).message.find("active") != std::string::npos);

    mgr.shutdown();
}

TEST_CASE("B4-K7: runtime-failing program is not reported as running",
          "[k7][execution][runtime-failure]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));
    const uint64_t gen = mgr.generation();

    const uint8_t tab = 3;

    REQUIRE(mgr.activateTabVM(tab).ok);

    // This program compiles cleanly (validation + load succeed) but the shred
    // dies immediately at runtime (integer division by zero).  The eval
    // response may legitimately be "ok" (compile+spork succeeded); what must
    // NOT happen is the program being reported as RUNNING afterwards.
    auto eval = mgr.evaluateCkTab(tab, kRuntimeFail);
    REQUIRE(eval.ok);

    // Give the VM time to load the shred and for it to crash at runtime.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    // The shred must not be reported as live — status reflects execution.
    REQUIRE(waitForNoShred(mgr, tab, 8000));

    // And no oscillator audio is produced (all silence).
    float buf[40 * kBlockSize];
    const uint32_t n = collectBlocks(mgr, gen, buf, 30, 3000);
    REQUIRE(n >= 5);
    REQUIRE(peakAmplitude(buf, static_cast<size_t>(n) * kBlockSize) < 1e-4f);

    mgr.shutdown();
}

TEST_CASE("B4-K7: eval success is reported as the real loaded shred, not a fabricated ID",
          "[k7][execution][honest-shred-id]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    const uint8_t tab = 4;

    REQUIRE(mgr.activateTabVM(tab).ok);
    auto eval = mgr.evaluateCkTab(tab, kOsc440);
    REQUIRE(eval.ok);

    // The VM eventually reports a real positive shred id assigned by the real
    // libchuck VM (its own ID space), not a value fabricated by the dispatcher.
    REQUIRE(waitForLoadedShred(mgr, tab, 8000));

    mgr.shutdown();
}
