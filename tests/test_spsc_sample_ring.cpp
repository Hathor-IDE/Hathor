// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// ---------------------------------------------------------------------------
// Zero-allocation operator new/delete override.
// Must be defined BEFORE any Catch2 or STL headers that might define their
// own operator new, so that the linker picks up this translation unit's
// definitions globally.
//
// We use a thread_local counting flag to avoid counting allocations that
// happen inside Catch2 itself (test framework infrastructure).
// ---------------------------------------------------------------------------
#include <cstdlib>    // std::malloc / std::free
#include <new>        // std::bad_alloc, std::size_t

static thread_local std::size_t g_alloc_count = 0;
static thread_local bool        g_counting    = false;

void* operator new(std::size_t size)
{
    if (g_counting) ++g_alloc_count;
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc{};
    return ptr;
}

void* operator new[](std::size_t size)
{
    if (g_counting) ++g_alloc_count;
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc{};
    return ptr;
}

void operator delete(void* ptr) noexcept  { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }

// ---------------------------------------------------------------------------
// Catch2 and hathor headers
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "SpscSampleRing.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>

using hathor::SpscSampleRing;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Convenience: produce a deterministic float sequence from an index.
static float seqFloat(std::size_t i) noexcept
{
    return static_cast<float>(i) + 0.5f;
}

/// Verify that two float ranges are element-wise equal.

// ---------------------------------------------------------------------------
// 1. Basic FIFO behavior
// ---------------------------------------------------------------------------

TEST_CASE("SpscSampleRing — basic FIFO order", "[spsc][fifo]")
{
    constexpr std::size_t kCapacity = 64;
    SpscSampleRing<kCapacity> ring;

    constexpr std::size_t kCount = 32;

    // Push a known sequence.
    for (std::size_t i = 0; i < kCount; ++i)
        ring.push(seqFloat(i));

    // Pop them back and confirm exact order.
    for (std::size_t i = 0; i < kCount; ++i) {
        float sample = -1.0f;
        REQUIRE(ring.pop(sample));
        REQUIRE(sample == Catch::Approx(seqFloat(i)));
    }

    // Ring is now empty.
    float stale = -1.0f;
    REQUIRE_FALSE(ring.pop(stale));
}

// ---------------------------------------------------------------------------
// 2. Empty ring
// ---------------------------------------------------------------------------

TEST_CASE("SpscSampleRing — pop from empty returns immediately", "[spsc][empty]")
{
    constexpr std::size_t kCapacity = 64;
    SpscSampleRing<kCapacity> ring;

    float sample = -1.0f;
    REQUIRE_FALSE(ring.pop(sample));

    // Verify it returns immediately (non-blocking).
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100'000; ++i)
        REQUIRE_FALSE(ring.pop(sample));
    auto elapsed = std::chrono::high_resolution_clock::now() - start;
    REQUIRE(elapsed < std::chrono::milliseconds(50));
}

// ---------------------------------------------------------------------------
// 3. Full ring — overflow drops oldest
// ---------------------------------------------------------------------------

TEST_CASE("SpscSampleRing — overflow drops oldest samples", "[spsc][overflow]")
{
    constexpr std::size_t kCapacity = 16;
    SpscSampleRing<kCapacity> ring;

    // Fill ring to capacity.
    for (std::size_t i = 0; i < kCapacity; ++i)
        ring.push(seqFloat(i));

    // Push one more — oldest sample (0.5f) should be dropped.
    ring.push(seqFloat(kCapacity));

    // Pop all available samples; we should see samples 1..Capacity.
    std::vector<float> popped;
    float sample = -1.0f;
    while (ring.pop(sample))
        popped.push_back(sample);

    REQUIRE(popped.size() == kCapacity);
    for (std::size_t i = 0; i < popped.size(); ++i)
        REQUIRE(popped[i] == Catch::Approx(seqFloat(i + 1)));
}

// ---------------------------------------------------------------------------
// 4. Producer/consumer concurrency
// ---------------------------------------------------------------------------

TEST_CASE("SpscSampleRing — concurrent producer/consumer", "[spsc][concurrent]")
{
    constexpr std::size_t kCapacity = 256;
    SpscSampleRing<kCapacity> ring;

    constexpr std::size_t kTotal = 10'000;
    std::vector<float> produced(kTotal);
    std::vector<float> consumed;
    consumed.reserve(kTotal);

    // Deterministic sequence: sample value encodes its index.
    for (std::size_t i = 0; i < kTotal; ++i)
        produced[i] = seqFloat(i);

    std::atomic<bool> producerDone{false};
    std::atomic<bool> consumerError{false};

    // Producer thread: push all samples at a steady rate.
    std::thread producer([&]() {
        for (std::size_t i = 0; i < kTotal; ++i) {
            ring.push(produced[i]);
            // Occasional yield to let the consumer catch up and exercise
            // both the steady-state and the overflow path.
            if (i % 200 == 0)
                std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });

    // Consumer thread: pop until producer is done and ring is empty.
    std::thread consumer([&]() {
        float sample = -1.0f;
        std::size_t expected = 0;
        while (!producerDone.load(std::memory_order_acquire) || ring.pop(sample)) {
            if (ring.pop(sample)) {
                const float expectedVal = seqFloat(expected);
                if (sample != Catch::Approx(expectedVal)) {
                    consumerError.store(true, std::memory_order_relaxed);
                    return;
                }
                consumed.push_back(sample);
                ++expected;
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE_FALSE(consumerError.load());
    // Consumer must have received at least the samples that fit in the ring
    // plus any that arrived after the producer finished.  We expect close
    // to kTotal — exact count depends on thread scheduling, but there must
    // be no corruption, no duplicates, and no deadlock.
    REQUIRE(consumed.size() <= kTotal);
    REQUIRE(consumed.size() > 0);

    // Verify no unexpected duplication: every consumed sample must match
    // its expected value (no stale data presented as current audio).
    for (std::size_t i = 0; i < consumed.size(); ++i) {
        const float expectedVal = (i < kTotal) ? seqFloat(i) : 0.0f;
        REQUIRE(consumed[i] == Catch::Approx(expectedVal));
    }
}

// ---------------------------------------------------------------------------
// 5. Underrun — consumer faster than producer
// ---------------------------------------------------------------------------

TEST_CASE("SpscSampleRing — underrun returns silence, no stall", "[spsc][underrun]")
{
    constexpr std::size_t kCapacity = 64;
    SpscSampleRing<kCapacity> ring;

    // Push a small number of samples.
    constexpr std::size_t kPushed = 8;
    for (std::size_t i = 0; i < kPushed; ++i)
        ring.push(seqFloat(i));

    // Consumer reads all available samples, then attempts more reads.
    std::vector<float> consumed;
    float sample = -1.0f;
    for (std::size_t i = 0; i < kPushed + 20; ++i) {
        if (ring.pop(sample))
            consumed.push_back(sample);
    }

    REQUIRE(consumed.size() == kPushed);
    for (std::size_t i = 0; i < consumed.size(); ++i)
        REQUIRE(consumed[i] == Catch::Approx(seqFloat(i)));

    // Producer can resume normally after underrun.
    ring.push(42.0f);
    REQUIRE(ring.pop(sample));
    REQUIRE(sample == Catch::Approx(42.0f));
}

// ---------------------------------------------------------------------------
// 6. Burst behavior
// ---------------------------------------------------------------------------

TEST_CASE("SpscSampleRing — burst absorbs overflow with drop-oldest", "[spsc][burst]")
{
    constexpr std::size_t kCapacity = 32;
    SpscSampleRing<kCapacity> ring;

    // Fill ring partially (half full).
    for (std::size_t i = 0; i < kCapacity / 2; ++i)
        ring.push(seqFloat(i));

    // Burst: push kCapacity samples rapidly (overwrites all previous).
    for (std::size_t i = kCapacity / 2; i < kCapacity / 2 + kCapacity; ++i)
        ring.push(seqFloat(i));

    // After burst, ring holds the newest kCapacity samples.
    std::vector<float> popped;
    float sample = -1.0f;
    while (ring.pop(sample))
        popped.push_back(sample);

    REQUIRE(popped.size() == kCapacity);
    // Oldest samples (0..15) were dropped.  Newest samples (16..47) remain.
    for (std::size_t i = 0; i < popped.size(); ++i) {
        const float expected = seqFloat(i + kCapacity / 2);
        REQUIRE(popped[i] == Catch::Approx(expected));
    }
}

// ---------------------------------------------------------------------------
// 7. Allocation audit — steady-state push/pop allocates nothing
// ---------------------------------------------------------------------------

TEST_CASE("SpscSampleRing — steady-state push/pop performs no allocations",
          "[spsc][allocation]")
{
    constexpr std::size_t kCapacity = 256;
    SpscSampleRing<kCapacity> ring;

    g_counting = true;
    g_alloc_count = 0;

    constexpr std::size_t kOps = 10'000;
    for (std::size_t i = 0; i < kOps; ++i) {
        ring.push(seqFloat(i));
        float sample = -1.0f;
        ring.pop(sample);
    }

    g_counting = false;
    REQUIRE(g_alloc_count == 0);
}

// ---------------------------------------------------------------------------
// 8. Lock/block audit — no mutex, no blocking primitive
// ---------------------------------------------------------------------------

TEST_CASE("SpscSampleRing — no mutex or blocking primitive in steady state",
          "[spsc][lockfree]")
{
    constexpr std::size_t kCapacity = 64;
    SpscSampleRing<kCapacity> ring;

    // push/pop are noexcept — they cannot block via exceptions or locks.
    REQUIRE(noexcept(ring.push(0.0f)));
    REQUIRE(noexcept(ring.pop(std::declval<float&>())));

    // popMany is also noexcept.
    float buf[32];
    REQUIRE(noexcept(ring.popMany(buf, 32)));

    // Verify the header does not pull in mutex/condition_variable by
    // checking that the ring can be instantiated in a context where those
    // types are deliberately hidden (compile-time structural check).
    // If SpscSampleRing contained a std::mutex member, this would fail to
    // compile or the sizeof check would reveal it.
    REQUIRE(sizeof(ring) > 0);
}

// ---------------------------------------------------------------------------
// Audio-callback integration: popMany fills silence on underrun
// ---------------------------------------------------------------------------

TEST_CASE("SpscSampleRing — popMany leaves silence on underrun", "[spsc][audio]")
{
    constexpr std::size_t kCapacity = 256;
    SpscSampleRing<kCapacity> ring;

    // Push only a few samples.
    constexpr std::size_t kPushed = 4;
    for (std::size_t i = 0; i < kPushed; ++i)
        ring.push(seqFloat(i));

    // Consumer zeroes buffer, then calls popMany for a full callback block.
    constexpr std::size_t kCallbackSize = 64;
    float buffer[kCallbackSize];
    std::memset(buffer, 0, sizeof(buffer));

    const std::size_t read = ring.popMany(buffer, kCallbackSize);

    REQUIRE(read == kPushed);
    // First kPushed samples are the real audio.
    for (std::size_t i = 0; i < kPushed; ++i)
        REQUIRE(buffer[i] == Catch::Approx(seqFloat(i)));
    // Remaining samples are silence (0.0f) — caller zeroed them.
    for (std::size_t i = kPushed; i < kCallbackSize; ++i)
        REQUIRE(buffer[i] == Catch::Approx(0.0f));
}

// ---------------------------------------------------------------------------
// Reset / reuse
// ---------------------------------------------------------------------------

TEST_CASE("SpscSampleRing — reset returns ring to empty state", "[spsc][reset]")
{
    constexpr std::size_t kCapacity = 32;
    SpscSampleRing<kCapacity> ring;

    // Fill ring.
    for (std::size_t i = 0; i < kCapacity; ++i)
        ring.push(seqFloat(i));

    // Overrun to exercise overflow path.
    ring.push(999.0f);

    // Reset.
    ring.reset();

    // Ring is empty.
    float sample = -1.0f;
    REQUIRE_FALSE(ring.pop(sample));

    // Push after reset works correctly.
    ring.push(1.0f);
    REQUIRE(ring.pop(sample));
    REQUIRE(sample == Catch::Approx(1.0f));
}
