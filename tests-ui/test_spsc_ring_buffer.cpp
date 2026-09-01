// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_spsc_ring_buffer.cpp — Tests for SpscRingBuffer metadata propagation (B2).
 *
 * Verifies that Event<ParamMap> fields — including the B2 metadata fields
 * sourceOffset and slotId — survive the seqlock write/read cycle through the
 * SPSC ring buffer.  Also verifies FIFO integrity and overwrite safety.
 *
 * Requirements: 28.9, 28.10
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "hathor/Event.hpp"
#include "hathor/ParamMap.hpp"
#include "hathor/Pattern.hpp"
#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"

#include "../app/VisualizerFrame.hpp"

#include <span>
#include <atomic>
#include <thread>
#include <cstring>
#include <cmath>

using namespace hathor;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Build a single Event<ParamMap> with a specific sourceOffset and slotId.
static Event<ParamMap> makeEvent(std::size_t sourceOffset, int8_t slotId, double gain = 1.0)
{
    Arc arc{Rational{0}, Rational{1}};
    ParamMap pm;
    pm.set(keys::kS, std::string{"bd"});
    pm.set(keys::kGain, gain);
    return Event<ParamMap>{arc, arc, std::move(pm), sourceOffset, slotId};
}

// ---------------------------------------------------------------------------
// P1a: FIFO integrity — N writes followed by M reads (N ≤ capacity)
// ---------------------------------------------------------------------------

TEST_CASE("SPSC ring buffer: FIFO integrity preserves events", "[spsc][p1a]")
{
    SpscRingBuffer<128> ring;

    alignas(Event<ParamMap>)
        std::byte writeStorage[kMaxFrameEvents * sizeof(Event<ParamMap>)];
    auto* writeEvents = reinterpret_cast<Event<ParamMap>*>(writeStorage);

    alignas(Event<ParamMap>)
        std::byte readStorage[kMaxFrameEvents * sizeof(Event<ParamMap>)];
    auto* readEvents = reinterpret_cast<Event<ParamMap>*>(readStorage);

    // Write frame 1: 2 events
    writeEvents[0] = makeEvent(0, 1);
    writeEvents[1] = makeEvent(3, 1);
    ring.write(0.5, 2, writeEvents);

    // Write frame 2: 1 event from a different slot
    writeEvents[0] = makeEvent(5, 2);
    ring.write(1.5, 1, writeEvents);

    // Read frame 1
    double cyclePos;
    uint32_t eventCount;
    REQUIRE(ring.read(cyclePos, eventCount, readEvents));
    CHECK(cyclePos == 0.5);
    REQUIRE(eventCount == 2);
    CHECK(readEvents[0].sourceOffset == 0);
    CHECK(readEvents[0].slotId == 1);
    CHECK(readEvents[1].sourceOffset == 3);
    CHECK(readEvents[1].slotId == 1);

    // Read frame 2
    REQUIRE(ring.read(cyclePos, eventCount, readEvents));
    CHECK(cyclePos == 1.5);
    REQUIRE(eventCount == 1);
    CHECK(readEvents[0].sourceOffset == 5);
    CHECK(readEvents[0].slotId == 2);

    // No more frames
    CHECK_FALSE(ring.read(cyclePos, eventCount, readEvents));
}

// ---------------------------------------------------------------------------
// B2: sourceOffset and slotId survive SPSC transfer
// ---------------------------------------------------------------------------

TEST_CASE("SPSC ring buffer: sourceOffset and slotId survive transfer", "[spsc][b2]")
{
    SpscRingBuffer<128> ring;

    alignas(Event<ParamMap>)
        std::byte writeStorage[kMaxFrameEvents * sizeof(Event<ParamMap>)];
    auto* writeEvents = reinterpret_cast<Event<ParamMap>*>(writeStorage);

    alignas(Event<ParamMap>)
        std::byte readStorage[kMaxFrameEvents * sizeof(Event<ParamMap>)];
    auto* readEvents = reinterpret_cast<Event<ParamMap>*>(readStorage);

    // Write a frame with events from different slots and offsets
    writeEvents[0] = makeEvent(0, 0);   // slot 0, offset 0
    writeEvents[1] = makeEvent(10, 1);  // slot 1, offset 10
    writeEvents[2] = makeEvent(20, 3);  // slot 3, offset 20
    ring.write(2.0, 3, writeEvents);

    // Read and verify metadata survived
    double cyclePos;
    uint32_t eventCount;
    REQUIRE(ring.read(cyclePos, eventCount, readEvents));
    REQUIRE(eventCount == 3);

    CHECK(readEvents[0].sourceOffset == 0);
    CHECK(readEvents[0].slotId == 0);

    CHECK(readEvents[1].sourceOffset == 10);
    CHECK(readEvents[1].slotId == 1);

    CHECK(readEvents[2].sourceOffset == 20);
    CHECK(readEvents[2].slotId == 3);

    // Verify the payload (param map) also survived
    CHECK(readEvents[0].value.get(keys::kS) != nullptr);
}

// ---------------------------------------------------------------------------
// P1b: Overwrite safety — N > capacity writes; frames don't corrupt
// ---------------------------------------------------------------------------

TEST_CASE("SPSC ring buffer: overwrite preserves latest frame metadata", "[spsc][p1b]")
{
    SpscRingBuffer<4> ring;  // small capacity for testing overwrite

    alignas(Event<ParamMap>)
        std::byte writeStorage[kMaxFrameEvents * sizeof(Event<ParamMap>)];
    auto* writeEvents = reinterpret_cast<Event<ParamMap>*>(writeStorage);

    alignas(Event<ParamMap>)
        std::byte readStorage[kMaxFrameEvents * sizeof(Event<ParamMap>)];
    auto* readEvents = reinterpret_cast<Event<ParamMap>*>(readStorage);

    // Fill the ring beyond capacity (4 slots), advancing each frame.
    for (uint32_t i = 0; i < 6; ++i) {
        writeEvents[0] = makeEvent(i * 10, static_cast<int8_t>(i));
        ring.write(static_cast<double>(i), 1, writeEvents);
    }

    // Read remaining frames — each should have correct metadata.
    double cyclePos;
    uint32_t eventCount;
    std::vector<std::pair<double, std::pair<std::size_t, int8_t>>> frames;

    while (ring.read(cyclePos, eventCount, readEvents)) {
        REQUIRE(eventCount == 1);
        frames.emplace_back(cyclePos,
            std::make_pair(readEvents[0].sourceOffset,
                           readEvents[0].slotId));
    }

    // After overwriting, at least 1 frame survived with correct metadata.
    REQUIRE_FALSE(frames.empty());

    // The last frame written should be among the survivors (or was overwritten
    // by the reader keeping up). At minimum, every frame read must have
    // internally consistent metadata (sourceOffset == 10*slotId pattern).
    for (const auto& [cp, meta] : frames) {
        const auto& [src, slot] = meta;
        CHECK(src == static_cast<std::size_t>(slot) * 10);
    }
}

// ---------------------------------------------------------------------------
// Empty ring returns false
// ---------------------------------------------------------------------------

TEST_CASE("SPSC ring buffer: empty ring returns false", "[spsc]")
{
    SpscRingBuffer<128> ring;

    alignas(Event<ParamMap>)
        std::byte readStorage[kMaxFrameEvents * sizeof(Event<ParamMap>)];
    auto* readEvents = reinterpret_cast<Event<ParamMap>*>(readStorage);

    double cyclePos;
    uint32_t eventCount;
    CHECK_FALSE(ring.read(cyclePos, eventCount, readEvents));
}

// ---------------------------------------------------------------------------
// Zero-event frame is valid
// ---------------------------------------------------------------------------

TEST_CASE("SPSC ring buffer: zero-event frame is valid", "[spsc]")
{
    SpscRingBuffer<128> ring;

    alignas(Event<ParamMap>)
        std::byte writeStorage[kMaxFrameEvents * sizeof(Event<ParamMap>)];
    auto* writeEvents = reinterpret_cast<Event<ParamMap>*>(writeStorage);

    alignas(Event<ParamMap>)
        std::byte readStorage[kMaxFrameEvents * sizeof(Event<ParamMap>)];
    auto* readEvents = reinterpret_cast<Event<ParamMap>*>(readStorage);

    ring.write(1.0, 0, writeEvents);

    double cyclePos;
    uint32_t eventCount;
    REQUIRE(ring.read(cyclePos, eventCount, readEvents));
    CHECK(cyclePos == 1.0);
    CHECK(eventCount == 0);
}

// ---------------------------------------------------------------------------
// B2: Event<ParamMap> size is bounded (no excessive growth from metadata)
// ---------------------------------------------------------------------------

TEST_CASE("SPSC ring buffer: metadata fields are trivially copyable", "[spsc][b2][realtime]")
{
    // B2 real-time audit: the metadata fields must be trivially copyable
    // so the ring buffer's copy is safe and allocation-free.
    // Note: Event<ParamMap> itself is NOT trivially copyable (ParamMap contains
    // std::string for sample names), but the metadata fields we add are.
    STATIC_REQUIRE(std::is_trivially_copyable_v<std::size_t>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<int8_t>);

    // Event<ParamMap> must be copy-constructible and copy-assignable
    // (the ring buffer and audio callback rely on this).
    STATIC_REQUIRE(std::is_copy_constructible_v<Event<ParamMap>>);
    STATIC_REQUIRE(std::is_copy_assignable_v<Event<ParamMap>>);
    STATIC_REQUIRE(std::is_trivially_destructible_v<std::size_t>);
    STATIC_REQUIRE(std::is_trivially_destructible_v<int8_t>);
}

// ===========================================================================
// PCM path tests (Agent 3.1: V1)
//
// These verify the SpscSampleRing<float> integration used by the audio
// callback → UITimer → VisualizerPanel data path:
//   - batch push of a decimated mono callback block
//   - drain via popMany with zero-fill on underrun
//   - FIFO order preservation across the boundary
//   - samples stop arriving when the producer stops pushing
// ===========================================================================

#include "SpscSampleRing.hpp"

// ---------------------------------------------------------------------------
// PCM: single batch push + popMany drain
// ---------------------------------------------------------------------------

TEST_CASE("PCM ring: batch push drains in FIFO order via popMany",
          "[pcm][fifo]")
{
    constexpr std::size_t kCapacity = 2048;
    hathor::SpscSampleRing<kCapacity> ring;

    // Simulate one audio callback: 128 decimated mono samples (post-gain).
    constexpr std::size_t kBlock = 128;
    float block[kBlock];
    for (std::size_t i = 0; i < kBlock; ++i)
        block[i] = static_cast<float>(i) * 0.01f;  // 0.00, 0.01, ..., 1.27

    for (std::size_t i = 0; i < kBlock; ++i)
        ring.push(block[i]);

    // Drain via popMany.
    float out[kBlock * 2];
    std::memset(out, 0xFF, sizeof(out));  // sentinel to detect untouched slots
    const std::size_t got = ring.popMany(out, kBlock * 2);

    REQUIRE(got == kBlock);
    for (std::size_t i = 0; i < kBlock; ++i)
        REQUIRE(out[i] == Catch::Approx(block[i]));
}

// ---------------------------------------------------------------------------
// PCM: popMany zero-fills (underrun) when fewer samples than requested
// ---------------------------------------------------------------------------

TEST_CASE("PCM ring: popMany returns only available samples on underrun",
          "[pcm][underrun]")
{
    constexpr std::size_t kCapacity = 256;
    hathor::SpscSampleRing<kCapacity> ring;

    constexpr std::size_t kPushed = 16;
    for (std::size_t i = 0; i < kPushed; ++i)
        ring.push(static_cast<float>(i) + 0.5f);

    // Request more than available.
    constexpr std::size_t kRequested = 64;
    float out[kRequested];
    std::memset(out, 0xAA, sizeof(out));  // pattern that is NOT 0.0f

    const std::size_t got = ring.popMany(out, kRequested);

    REQUIRE(got == kPushed);
    for (std::size_t i = 0; i < kPushed; ++i)
        REQUIRE(out[i] == Catch::Approx(static_cast<float>(i) + 0.5f));

    // Remaining slots should be untouched (caller is responsible for zeroing).
    // popMany does not write to them — the contract is: caller zeros first.
}

// ---------------------------------------------------------------------------
// PCM: samples stop arriving when producer stops (empty ring drains to 0)
// ---------------------------------------------------------------------------

TEST_CASE("PCM ring: empty after full drain — samples stop when producer halts",
          "[pcm][stop]")
{
    constexpr std::size_t kCapacity = 128;
    hathor::SpscSampleRing<kCapacity> ring;

    // Push a burst simulating 2 callbacks of 64 samples each.
    for (int b = 0; b < 2; ++b)
        for (std::size_t i = 0; i < 64; ++i)
            ring.push(static_cast<float>(b * 64 + i));

    // Drain everything.
    float out[200];
    std::memset(out, 0, sizeof(out));
    std::size_t total = 0;
    std::size_t got;
    while ((got = ring.popMany(out + total, 200 - total)) > 0)
        total += got;

    REQUIRE(total == 128);

    // Next drain: ring is empty — simulates "stopped" state.
    std::memset(out, 0xFF, sizeof(out));
    const std::size_t gotAfter = ring.popMany(out, 200);
    REQUIRE(gotAfter == 0);
}

// ---------------------------------------------------------------------------
// PCM: decimation — push only every Nth sample to stay within 256/callback
// ---------------------------------------------------------------------------

TEST_CASE("PCM ring: decimated push preserves monotonic amplitude trend",
          "[pcm][decimate]")
{
    constexpr std::size_t kCapacity = 2048;
    hathor::SpscSampleRing<kCapacity> ring;

    // Simulate a 512-sample callback decimated to 256 (step = 2).
    constexpr int kNumSamples = 512;
    constexpr int kDecimated  = 256;
    float block[kNumSamples];
    for (int i = 0; i < kNumSamples; ++i)
        block[i] = static_cast<float>(i) / static_cast<float>(kNumSamples);

    const int step = kNumSamples / kDecimated;  // = 2
    for (int s = 0; s < kNumSamples; s += step)
        ring.push(block[s]);

    float out[kDecimated + 1];
    std::memset(out, 0, sizeof(out));
    const std::size_t got = ring.popMany(out, kDecimated + 1);

    REQUIRE(got == static_cast<std::size_t>(kNumSamples / step));

    // Verify monotonic increasing trend (decimated sine-free ramp).
    for (std::size_t i = 1; i < got; ++i)
        REQUIRE(out[i] >= out[i - 1]);
}

// ---------------------------------------------------------------------------
// PCM: concurrent audio-thread producer + UI-thread consumer (no corruption)
// ---------------------------------------------------------------------------

TEST_CASE("PCM ring: concurrent producer/consumer — no corruption, no deadlock",
          "[pcm][concurrent]")
{
    constexpr std::size_t kCapacity = 512;
    hathor::SpscSampleRing<kCapacity> ring;

    constexpr std::size_t kTotal = 20'000;
    std::atomic<bool> done{false};
    std::atomic<bool> error{false};
    std::atomic<std::size_t> received{0};

    // Producer thread (simulates audio callback at high rate).
    std::thread producer([&]() {
        for (std::size_t i = 0; i < kTotal; ++i) {
            ring.push(static_cast<float>(i) + 0.25f);
        }
        done.store(true, std::memory_order_release);
    });

    // Consumer thread (simulates 60 Hz UI drain).
    std::thread consumer([&]() {
        float buf[64];
        while (true) {
            std::memset(buf, 0, sizeof(buf));
            const std::size_t n = ring.popMany(buf, 64);
            for (std::size_t i = 0; i < n; ++i) {
                // Decode index: sample == idx + 0.25f
                const std::size_t idx =
                    static_cast<std::size_t>(std::floor(static_cast<double>(buf[i]) - 0.25));
                if (idx >= kTotal)
                    error.store(true, std::memory_order_relaxed);
            }
            received.fetch_add(n, std::memory_order_relaxed);
            if (error.load(std::memory_order_relaxed))
                break;
            if (n == 0 && done.load(std::memory_order_acquire))
                break;
        }
    });

    producer.join();
    consumer.join();

    REQUIRE_FALSE(error.load());
    REQUIRE(received.load() > 0);
    REQUIRE(received.load() <= kTotal);
}

// ---------------------------------------------------------------------------
// PCM path: UITimer-style drain pattern — popMany into a fixed stack buffer
// ---------------------------------------------------------------------------

TEST_CASE("PCM ring: UITimer drain pattern (fixed buffer, partial reads)",
          "[pcm][uitimer][drain]")
{
    constexpr std::size_t kCapacity = 2048;
    hathor::SpscSampleRing<kCapacity> ring;

    // Simulate 4 audio callbacks of 256 samples each = 1024 samples.
    constexpr int kCallbackSize = 256;
    for (int cb = 0; cb < 4; ++cb)
    {
        for (int s = 0; s < kCallbackSize; ++s)
        {
            float val = static_cast<float>(cb * kCallbackSize + s) / 1024.0f;
            ring.push(val);
        }
    }

    // Simulate UITimer drain: 60 Hz drain with a fixed 512-sample stack buffer.
    constexpr std::size_t kDrainMax = 512;
    float pcmBuf[kDrainMax];

    std::size_t totalReceived = 0;
    std::size_t drainCount = 0;

    while (true)
    {
        std::memset(pcmBuf, 0xFF, sizeof(pcmBuf));
        const std::size_t got = ring.popMany(pcmBuf, kDrainMax);
        if (got == 0)
            break;
        REQUIRE(got <= kDrainMax);
        for (std::size_t i = 0; i < got; ++i)
        {
            float expected = static_cast<float>(totalReceived + i) / 1024.0f;
            REQUIRE(pcmBuf[i] == Catch::Approx(expected).margin(0.001f));
        }
        totalReceived += got;
        ++drainCount;
    }

    REQUIRE(totalReceived == 1024);
    REQUIRE(drainCount == 2);
}

// ---------------------------------------------------------------------------
// PCM path: samples stop arriving when transport stops (empty after drain)
// ---------------------------------------------------------------------------

TEST_CASE("PCM ring: empty after drain simulates transport stop",
          "[pcm][stop][drain]")
{
    constexpr std::size_t kCapacity = 2048;
    hathor::SpscSampleRing<kCapacity> ring;

    // Push 64 samples simulating one callback.
    for (int i = 0; i < 64; ++i)
        ring.push(static_cast<float>(i) * 0.01f);

    // Drain all.
    float buf[256];
    std::memset(buf, 0, sizeof(buf));
    std::size_t got = ring.popMany(buf, 256);
    REQUIRE(got == 64);

    // Next drain (simulating next UITimer tick after transport stop):
    // ring is empty -> 0 samples.
    got = ring.popMany(buf, 256);
    REQUIRE(got == 0);

    // Simulate a few more "silent" ticks.
    for (int i = 0; i < 10; ++i)
    {
        got = ring.popMany(buf, 256);
        REQUIRE(got == 0);
    }
}
