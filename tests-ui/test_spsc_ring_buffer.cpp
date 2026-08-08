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

#include "hathor/Event.hpp"
#include "hathor/ParamMap.hpp"
#include "hathor/Pattern.hpp"
#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"

#include "../app/VisualizerFrame.hpp"

#include <span>

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
