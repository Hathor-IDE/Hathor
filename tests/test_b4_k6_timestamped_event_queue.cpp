// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_b4_k6_timestamped_event_queue.cpp — tests for B4-K6 timestamped
 * musical event queue.
 *
 * Tests the following subsystems:
 *   1. MusicalEvent struct (creation, fields, ordering)
 *   2. SpscEventRing (lock-free SPSC ring for events)
 *   3. ClockSync (worker/master clock mapping, offset, drift)
 *   4. EventTransport (shared-memory seqlock protocol)
 *   5. EventScheduler (staging, late/early/buffer-boundary, VM generation guards)
 *   6. EventPublisher (timestamp generation, event creation)
 *   7. Integration: end-to-end timestamp preservation through transport
 *
 * JUCE-free: these tests link Catch2 + the header-only components only.
 * No worker process spawn needed — the SPSC ring and scheduler are
 * tested in-process.
 *
 * Requirements: B4-K6, Decision #22, Decision #23
 */

// ---------------------------------------------------------------------------
// Zero-allocation operator new/delete override (must be before Catch2/STL)
// ---------------------------------------------------------------------------
#include <cstdlib>
#include <new>

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

void operator delete(void* ptr) noexcept       { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept     { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }

// ---------------------------------------------------------------------------
// Catch2 and hathor headers
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "MusicalEvent.hpp"
#include "SpscEventRing.hpp"
#include "ClockSync.hpp"
#include "EventTransport.hpp"
#include "EventScheduler.hpp"
#include "EventPublisher.hpp"

#include "hathor/Event.hpp"
#include "hathor/ParamMap.hpp"
#include "hathor/Rational.hpp"
#include "hathor/Arc.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using hathor::Event;
using hathor::MusicalEvent;
using hathor::ParamMap;
using hathor::Rational;
using hathor::SpscEventRing;
using hathor::ClockSync;
using hathor::EventScheduler;
using hathor::EventPublisher;
using namespace hathor::audio_worker;

// ---------------------------------------------------------------------------
// 1. MusicalEvent — creation and field correctness
// ---------------------------------------------------------------------------

TEST_CASE("MusicalEvent — default construction sets sensible defaults", "[b4-k6][event]")
{
    MusicalEvent ev;
    REQUIRE(ev.type == hathor::EventType::NoteOn);
    REQUIRE(ev.musicalTs == Rational(0));
    REQUIRE(ev.sampleTs == 0);
    REQUIRE(ev.localExecTs == 0);
    REQUIRE(ev.sequence == 0);
    REQUIRE(ev.targetTabId == 0);
    REQUIRE(ev.vmGeneration == 0);
}

TEST_CASE("MusicalEvent — full construction preserves all fields", "[b4-k6][event]")
{
    ParamMap pm;
    pm.set(hathor::keys::kS, hathor::Value(std::string("bd")));

    MusicalEvent ev(
        hathor::EventType::InstrumentTrigger,
        std::move(pm),
        Rational(3, 2),       // musical timestamp: 1.5 cycles
        44100,                // sample timestamp: 1 second @ 44.1kHz
        44100,                // local execution timestamp
        42,                   // sequence number
        3,                    // target tab
        7                     // VM generation
    );

    REQUIRE(ev.type == hathor::EventType::InstrumentTrigger);
    REQUIRE(ev.musicalTs == Rational(3, 2));
    REQUIRE(ev.sampleTs == 44100);
    REQUIRE(ev.localExecTs == 44100);
    REQUIRE(ev.sequence == 42);
    REQUIRE(ev.targetTabId == 3);
    REQUIRE(ev.vmGeneration == 7);

    // Verify payload survived the move
    const auto* s = ev.payload.get(hathor::keys::kS);
    REQUIRE(s != nullptr);
    REQUIRE(std::get<std::string>(*s) == "bd");
}

// ---------------------------------------------------------------------------
// 2. MusicalEvent — deterministic ordering by (sampleTs, sequence)
// ---------------------------------------------------------------------------

TEST_CASE("MusicalEvent — ordering is deterministic for equal sampleTs", "[b4-k6][ordering]")
{
    MusicalEvent a;
    a.sampleTs = 1000;
    a.localExecTs = 1000;
    a.sequence = 1;

    MusicalEvent b;
    b.sampleTs = 1000;
    b.localExecTs = 1000;
    b.sequence = 2;

    MusicalEvent c;
    c.sampleTs = 1000;
    c.localExecTs = 1000;
    c.sequence = 3;

    // For a min-heap, earlier sequence should be "less" (higher priority)
    REQUIRE(a < b);
    REQUIRE(b < c);
    REQUIRE(a < c);

    // Events at different sampleTs are ordered by timestamp first
    MusicalEvent early;
    early.sampleTs = 100;
    early.localExecTs = 100;
    early.sequence = 999;

    MusicalEvent late;
    late.sampleTs = 200;
    late.localExecTs = 200;
    late.sequence = 1;

    REQUIRE(early < late); // sampleTs dominates sequence
}

// ---------------------------------------------------------------------------
// 3. SpscEventRing — basic FIFO behavior
// ---------------------------------------------------------------------------

TEST_CASE("SpscEventRing — basic FIFO order", "[b4-k6][ring][fifo]")
{
    SpscEventRing<64> ring;

    // Push 32 events with distinct sampleTs
    for (uint32_t i = 0; i < 32; ++i) {
        MusicalEvent ev;
        ev.sampleTs = i;
        ev.localExecTs = i;
        ev.sequence = i;
        REQUIRE(ring.push(ev));
    }

    // Pop them back in FIFO order
    for (uint32_t i = 0; i < 32; ++i) {
        auto opt = ring.pop();
        REQUIRE(opt.has_value());
        REQUIRE(opt->sampleTs == i);
        REQUIRE(opt->sequence == i);
    }

    // Ring is now empty
    REQUIRE_FALSE(ring.pop().has_value());
}

// ---------------------------------------------------------------------------
// 4. SpscEventRing — empty ring returns nullopt immediately (non-blocking)
// ---------------------------------------------------------------------------

TEST_CASE("SpscEventRing — pop from empty returns immediately", "[b4-k6][ring][empty]")
{
    SpscEventRing<64> ring;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100'000; ++i) {
        REQUIRE_FALSE(ring.pop().has_value());
    }
    auto elapsed = std::chrono::high_resolution_clock::now() - start;
    REQUIRE(elapsed < std::chrono::milliseconds(50));
}

// ---------------------------------------------------------------------------
// 5. SpscEventRing — full ring returns false on push (no drop)
// ---------------------------------------------------------------------------

TEST_CASE("SpscEventRing — full ring returns false on push", "[b4-k6][ring][full]")
{
    SpscEventRing<8> ring;

    // Fill to capacity (capacity-1 usable slots since we use the
    // nextHead == tail test)
    uint32_t pushed = 0;
    MusicalEvent ev;
    ev.sampleTs = 0;
    while (ring.push(ev)) {
        ev.sampleTs++;
        ++pushed;
    }

    REQUIRE(pushed > 0);
    REQUIRE(pushed <= 7); // capacity-1 because we need one slot gap

    // Verify all pushed events are retrievable in order
    for (uint32_t i = 0; i < pushed; ++i) {
        auto opt = ring.pop();
        REQUIRE(opt.has_value());
        REQUIRE(opt->sampleTs == i);
    }
}

// ---------------------------------------------------------------------------
// 6. SpscEventRing — concurrent producer/consumer (no corruption)
// ---------------------------------------------------------------------------

TEST_CASE("SpscEventRing — concurrent producer/consumer preserves ordering", "[b4-k6][ring][concurrent]")
{
    constexpr uint32_t kTotal = 10'000;
    SpscEventRing<256> ring;

    std::atomic<bool> producerDone{false};
    std::atomic<bool> consumerError{false};
    std::atomic<uint32_t> consumedCount{0};

    std::thread producer([&]() {
        for (uint32_t i = 0; i < kTotal; ++i) {
            MusicalEvent ev;
            ev.sampleTs = i;
            ev.localExecTs = i;
            ev.sequence = i;
            while (!ring.push(ev)) {
                std::this_thread::yield();
            }
            if (i % 500 == 0)
                std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        uint32_t expected = 0;
        while (true) {
            auto opt = ring.pop();
            if (opt.has_value()) {
                if (opt->sampleTs != expected) {
                    consumerError.store(true, std::memory_order_relaxed);
                    break;
                }
                ++expected;
                ++consumedCount;
            } else if (producerDone.load(std::memory_order_acquire)) {
                // Drain any remaining
                while (auto opt2 = ring.pop()) {
                    if (opt2->sampleTs != expected) {
                        consumerError.store(true, std::memory_order_relaxed);
                        break;
                    }
                    ++expected;
                    ++consumedCount;
                }
                break;
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE_FALSE(consumerError.load());
    REQUIRE(consumedCount.load() == kTotal);
    REQUIRE(consumedCount.load() <= kTotal);
}

// ---------------------------------------------------------------------------
// 7. SpscEventRing — steady-state allocates nothing
// ---------------------------------------------------------------------------

TEST_CASE("SpscEventRing — steady-state push/pop performs no allocations", "[b4-k6][ring][allocation]")
{
    SpscEventRing<256> ring;

    g_counting = true;
    g_alloc_count = 0;

    constexpr uint32_t kOps = 10'000;
    for (uint32_t i = 0; i < kOps; ++i) {
        MusicalEvent ev;
        ev.sampleTs = i;
        ring.push(ev);
        ring.pop();
    }

    g_counting = false;
    REQUIRE(g_alloc_count == 0);
}

// ---------------------------------------------------------------------------
// 8. SpscEventRing — reset returns to empty
// ---------------------------------------------------------------------------

TEST_CASE("SpscEventRing — reset returns ring to empty state", "[b4-k6][ring][reset]")
{
    SpscEventRing<32> ring;

    for (uint32_t i = 0; i < 20; ++i) {
        MusicalEvent ev;
        ev.sampleTs = i;
        ring.push(ev);
    }

    ring.reset();

    REQUIRE_FALSE(ring.pop().has_value());

    // Push after reset works
    MusicalEvent ev;
    ev.sampleTs = 42;
    REQUIRE(ring.push(ev));
    auto opt = ring.pop();
    REQUIRE(opt.has_value());
    REQUIRE(opt->sampleTs == 42);
}

// ---------------------------------------------------------------------------
// 9. ClockSync — master to local conversion with zero offset
// ---------------------------------------------------------------------------

TEST_CASE("ClockSync — zero offset converts master to local identity", "[b4-k6][clock]")
{
    ClockSync clock;

    // With zero offset, masterToLocal should be identity
    const uint64_t masterTs = 441000;
    const uint64_t localTs = clock.masterToLocal(masterTs);
    REQUIRE(localTs == Catch::Approx(masterTs));

    REQUIRE(clock.isWithinTolerance());
    REQUIRE_FALSE(clock.needsRestart());
}

TEST_CASE("ClockSync — positive offset shifts local timestamps backward", "[b4-k6][clock]")
{
    ClockSync clock;
    clock.setSampleRates(44100.0, 44100.0);

    // Simulate: local clock is 1000 samples ahead of master
    clock.updateOffset(10000, 11000, 0);

    // Master position 10000 should map to local position 11000
    const uint64_t localTs = clock.masterToLocal(10000);
    REQUIRE(localTs == Catch::Approx(11000).margin(1));

    // Offset should be ~1000 samples = ~22.7ms @ 44.1kHz
    const double offsetMs = clock.getOffsetMs();
    REQUIRE(std::abs(offsetMs - 22.67) < 1.0);
}

TEST_CASE("ClockSync — RTT compensation corrects offset estimate", "[b4-k6][clock]")
{
    ClockSync clock;
    clock.setSampleRates(44100.0, 44100.0);

    // Master at 10000, local at 11000, RTT = 2ms
    // RTT/2 in samples = 0.001 * 44100 = 44.1
    // Corrected offset = (11000 - 10000) - 44.1 = 955.9
    clock.updateOffset(10000, 11000, 2000);

    const uint64_t localTs = clock.masterToLocal(10000);
    // Expected: 10000 + 955.9 ≈ 10956
    REQUIRE(localTs == Catch::Approx(10956).margin(2));
}

TEST_CASE("ClockSync — large offset triggers restart condition", "[b4-k6][clock]")
{
    ClockSync clock(/*driftToleranceMs=*/0.5, /*maxOffsetMs=*/10.0);
    clock.setSampleRates(44100.0, 44100.0);

    // 100ms offset = ~4410 samples = way beyond maxOffsetMs (10ms)
    clock.updateOffset(0, 4410, 0);

    REQUIRE(clock.needsRestart());
    REQUIRE_FALSE(clock.isWithinTolerance());
}

TEST_CASE("ClockSync — drift rate is computed from consecutive measurements", "[b4-k6][clock]")
{
    ClockSync clock;
    clock.setSampleRates(44100.0, 44100.0);

    clock.updateOffset(0, 0, 0);   // offset = 0
    clock.updateOffset(1000, 1100, 0); // offset = 100 (drifted +100 over 1000 samples)

    // Drift rate should be 100/1000 = 0.1
    REQUIRE(clock.getDriftRate() == Catch::Approx(0.1).epsilon(0.01));
}

// ---------------------------------------------------------------------------
// 10. EventScheduler — late event is dropped
// ---------------------------------------------------------------------------

TEST_CASE("EventScheduler — late events are dropped", "[b4-k6][scheduler][late]")
{
    ClockSync clock;
    clock.setSampleRates(44100.0, 44100.0);
    EventScheduler scheduler;

    SpscEventRing<256> ring;

    // Current local sample cursor is at 1000
    scheduler.setSampleCursor(1000);
    scheduler.setCurrentVmGeneration(1);

    // Event targeting sample 500 (500 samples in the past, beyond grace window of 64)
    MusicalEvent lateEv;
    lateEv.sampleTs = 500;
    lateEv.sequence = 1;
    lateEv.vmGeneration = 1;
    ring.push(lateEv);

    // Stage events — late event should be dropped
    uint32_t staged = scheduler.stageEvents(ring, clock);
    REQUIRE(staged == 0);
    REQUIRE(scheduler.numStagedEvents() == 0);
}

TEST_CASE("EventScheduler — early events are held in staging", "[b4-k6][scheduler][early]")
{
    ClockSync clock;
    clock.setSampleRates(44100.0, 44100.0);
    EventScheduler scheduler;

    SpscEventRing<256> ring;

    scheduler.setSampleCursor(1000);
    scheduler.setCurrentVmGeneration(1);

    // Event targeting sample 5000 (far in the future)
    MusicalEvent earlyEv;
    earlyEv.sampleTs = 5000;
    earlyEv.localExecTs = 5000; // clock has zero offset, so local = master
    earlyEv.sequence = 1;
    earlyEv.vmGeneration = 1;
    ring.push(earlyEv);

    uint32_t staged = scheduler.stageEvents(ring, clock);
    REQUIRE(staged == 1);
    REQUIRE(scheduler.numStagedEvents() == 1);
}

TEST_CASE("EventScheduler — event within buffer is dispatched as ready", "[b4-k6][scheduler][ready]")
{
    ClockSync clock;
    clock.setSampleRates(44100.0, 44100.0);
    EventScheduler scheduler;

    SpscEventRing<256> ring;

    scheduler.setSampleCursor(1000);
    scheduler.setCurrentVmGeneration(1);

    // Event targeting sample 1030 (within [1000, 1064))
    MusicalEvent ev;
    ev.sampleTs = 1030;
    ev.localExecTs = 1030;
    ev.sequence = 1;
    ev.vmGeneration = 1;
    ring.push(ev);

    scheduler.stageEvents(ring, clock);
    REQUIRE(scheduler.numStagedEvents() == 1);

    // Request events for buffer [1000, 1064)
    MusicalEvent outEvents[16];
    uint32_t outCount = 0;
    bool ok = scheduler.getReadyEvents(1000, 64, outEvents, &outCount, 16);

    REQUIRE(ok);
    REQUIRE(outCount == 1);
    REQUIRE(outEvents[0].sampleTs == 1030);
    REQUIRE(scheduler.numStagedEvents() == 0);
}

TEST_CASE("EventScheduler — event outside buffer is held for next buffer", "[b4-k6][scheduler][boundary]")
{
    ClockSync clock;
    clock.setSampleRates(44100.0, 44100.0);
    EventScheduler scheduler;

    SpscEventRing<256> ring;

    scheduler.setSampleCursor(1000);
    scheduler.setCurrentVmGeneration(1);

    // Event targeting sample 1100 (outside [1000, 1064))
    MusicalEvent ev;
    ev.sampleTs = 1100;
    ev.localExecTs = 1100;
    ev.sequence = 1;
    ev.vmGeneration = 1;
    ring.push(ev);

    scheduler.stageEvents(ring, clock);

    // Request events for buffer [1000, 1064) — event should NOT be dispatched
    MusicalEvent outEvents[16];
    uint32_t outCount = 0;
    scheduler.getReadyEvents(1000, 64, outEvents, &outCount, 16);
    REQUIRE(outCount == 0);
    REQUIRE(scheduler.numStagedEvents() == 1);

    // Now request events for buffer [1100, 1164) — event should be dispatched
    scheduler.setSampleCursor(1100);
    scheduler.getReadyEvents(1100, 64, outEvents, &outCount, 16);
    REQUIRE(outCount == 1);
    REQUIRE(scheduler.numStagedEvents() == 0);
}

TEST_CASE("EventScheduler — stale VM generation events are rejected", "[b4-k6][scheduler][stale-gen]")
{
    ClockSync clock;
    EventScheduler scheduler;

    SpscEventRing<256> ring;

    scheduler.setSampleCursor(1000);
    scheduler.setCurrentVmGeneration(5);

    // Event targeting generation 3 (stale)
    MusicalEvent ev;
    ev.sampleTs = 5000;
    ev.localExecTs = 5000;
    ev.sequence = 1;
    ev.vmGeneration = 3;
    ring.push(ev);

    uint32_t staged = scheduler.stageEvents(ring, clock);
    REQUIRE(staged == 0);
    REQUIRE(scheduler.numStagedEvents() == 0);
}

TEST_CASE("EventScheduler — future VM generation events are skipped", "[b4-k6][scheduler][future-gen]")
{
    ClockSync clock;
    EventScheduler scheduler;

    SpscEventRing<256> ring;

    scheduler.setSampleCursor(1000);
    scheduler.setCurrentVmGeneration(5);

    // Event targeting generation 6 (future)
    MusicalEvent ev;
    ev.sampleTs = 5000;
    ev.localExecTs = 5000;
    ev.sequence = 1;
    ev.vmGeneration = 6;
    ring.push(ev);

    uint32_t staged = scheduler.stageEvents(ring, clock);
    REQUIRE(staged == 0);
    REQUIRE(scheduler.numStagedEvents() == 0);
}

TEST_CASE("EventScheduler — clearStagedEvents removes all pending events", "[b4-k6][scheduler][clear]")
{
    ClockSync clock;
    EventScheduler scheduler;

    SpscEventRing<256> ring;

    scheduler.setSampleCursor(1000);
    scheduler.setCurrentVmGeneration(1);

    for (uint32_t i = 0; i < 10; ++i) {
        MusicalEvent ev;
        ev.sampleTs = 5000 + i;
        ev.localExecTs = 5000 + i;
        ev.sequence = i;
        ev.vmGeneration = 1;
        ring.push(ev);
    }

    scheduler.stageEvents(ring, clock);
    REQUIRE(scheduler.numStagedEvents() == 10);

    scheduler.clearStagedEvents();
    REQUIRE(scheduler.numStagedEvents() == 0);
}

// ---------------------------------------------------------------------------
// 11. EventScheduler — getReadyEvents is RT-safe (noexcept, no allocation)
// ---------------------------------------------------------------------------

TEST_CASE("EventScheduler — getReadyEvents is noexcept and allocation-free", "[b4-k6][scheduler][rt-safe]")
{
    ClockSync clock;
    clock.setSampleRates(44100.0, 44100.0);
    EventScheduler scheduler;

    SpscEventRing<256> ring;
    scheduler.setSampleCursor(1000);
    scheduler.setCurrentVmGeneration(1);

    MusicalEvent ev;
    ev.sampleTs = 1030;
    ev.localExecTs = 1030;
    ev.sequence = 1;
    ev.vmGeneration = 1;
    ring.push(ev);
    scheduler.stageEvents(ring, clock);

    // Allocation audit
    g_counting = true;
    g_alloc_count = 0;

    MusicalEvent outEvents[16];
    uint32_t outCount = 0;
    bool ok = scheduler.getReadyEvents(1000, 64, outEvents, &outCount, 16);

    g_counting = false;
    REQUIRE(g_alloc_count == 0);
    REQUIRE(ok);
    REQUIRE(outCount == 1);
}

// ---------------------------------------------------------------------------
// 12. EventPublisher — timestamp generation from pattern event
// ---------------------------------------------------------------------------

TEST_CASE("EventPublisher — createEventFromPattern preserves musical and sample timestamps", "[b4-k6][publisher]")
{
    EventPublisher publisher(44100.0);

    // Simulate a pattern event at cycle 1.5 (3/2) with 120 BPM
    // samplesPerCycle = 44100 * 60 / 120 = 22050
    const double samplesPerCycle = 22050.0;
    const uint64_t clockNow = 44100; // 1 second into the timeline
    const hathor::Rational cycleStart = hathor::Rational(44100, 22050); // 2.0 cycles

    // Create a pattern event with a specific Arc
    Event<ParamMap> hathorEv;
    hathorEv.active = Arc(Rational(5, 2), Rational(3, 1)); // starts at cycle 2.5
    hathorEv.whole = hathorEv.active;
    hathorEv.value.set(hathor::keys::kS, hathor::Value(std::string("snare")));

    const uint64_t vmGen = 1;
    MusicalEvent me = publisher.createEventFromPattern(hathorEv, cycleStart, clockNow,
                                                        samplesPerCycle, 2, vmGen);

    // Musical timestamp should be the event's cycle start
    REQUIRE(me.musicalTs == Rational(5, 2));

    // Sample timestamp: clockNow + (2.5 - 2.0) * 22050 = 44100 + 0.5 * 22050 = 55125
    REQUIRE(me.sampleTs == 55125);

    // Sequence should be 0 (first event)
    REQUIRE(me.sequence == 0);

    // Target tab and VM generation preserved
    REQUIRE(me.targetTabId == 2);
    REQUIRE(me.vmGeneration == vmGen);
}

TEST_CASE("EventPublisher — sequential events get monotonic sequence numbers", "[b4-k6][publisher][sequence]")
{
    EventPublisher publisher(44100.0);
    publisher.setCurrentGeneration(1);

    ParamMap pm;
    pm.set(hathor::keys::kS, hathor::Value(std::string("bd")));

    // Generate 100 events
    uint64_t lastSeq = 0;
    for (uint64_t i = 0; i < 100; ++i) {
        MusicalEvent ev(hathor::EventType::NoteOn, std::move(pm),
                        Rational(i), 44100 + i, 44100 + i, 0, 0, 1);
        REQUIRE(publisher.publishEvent(ev));
        lastSeq = ev.sequence;
        REQUIRE(ev.sequence == i);
    }
}

// ---------------------------------------------------------------------------
// 13. EventTransport — seqlock protocol validation (in-process simulation)
// ---------------------------------------------------------------------------

TEST_CASE("EventTransport — write and read via seqlock, no torn reads", "[b4-k6][transport]")
{
    // We can't use actual shared memory in a unit test easily, but we can
    // test the seqlock protocol by simulating it with the slot structure.
    EventSlot slot;
    SharedEventTransport transport;
    transport.magic.store(kEventMagic, std::memory_order_release);
    transport.generation.store(1, std::memory_order_release);

    MusicalEvent writeEv;
    writeEv.type = hathor::EventType::NoteOn;
    writeEv.musicalTs = Rational(1, 4);
    writeEv.sampleTs = 44100;
    writeEv.localExecTs = 44100;
    writeEv.sequence = 42;
    writeEv.targetTabId = 3;
    writeEv.vmGeneration = 1;
    writeEv.payload.set(hathor::keys::kS, hathor::Value(std::string("bd")));

    // Simulate writer
    const uint64_t wSeq = transport.writeSeq.load(std::memory_order_relaxed);
    slot.seq.store(wSeq | 1u, std::memory_order_release);
    slot.event = writeEv;
    slot.seq.store(wSeq + 2u, std::memory_order_release);
    transport.writeSeq.store(wSeq + 1, std::memory_order_release);

    // Simulate reader
    const uint64_t rSeq = transport.readSeq.load(std::memory_order_relaxed);
    const uint64_t wSeq2 = transport.writeSeq.load(std::memory_order_acquire);
    REQUIRE(rSeq < wSeq2);

    const uint32_t slotIdx = static_cast<uint32_t>(rSeq) & kEventRingMask;
    const auto& sl = transport.slots[slotIdx];

    const uint64_t s0 = sl.seq.load(std::memory_order_acquire);
    REQUIRE((s0 & 1u) == 0); // not in-progress
    REQUIRE(s0 == wSeq + 2u);

    MusicalEvent readEv = sl.event;

    const uint64_t s1 = sl.seq.load(std::memory_order_acquire);
    REQUIRE(s1 == s0); // no torn read

    // Verify all fields survived
    REQUIRE(readEv.type == hathor::EventType::NoteOn);
    REQUIRE(readEv.musicalTs == Rational(1, 4));
    REQUIRE(readEv.sampleTs == 44100);
    REQUIRE(readEv.sequence == 42);
    REQUIRE(readEv.targetTabId == 3);
    REQUIRE(readEv.vmGeneration == 1);

    const auto* s = readEv.payload.get(hathor::keys::kS);
    REQUIRE(s != nullptr);
    REQUIRE(std::get<std::string>(*s) == "bd");
}

TEST_CASE("EventTransport — generation mismatch causes publisher to skip", "[b4-k6][transport][gen-mismatch]")
{
    // Test the EventPublisher's generation check logic
    // (actual shared-memory requires /dev/shm which may not be available)
    SharedEventTransport fakeTransport;
    fakeTransport.magic.store(kEventMagic, std::memory_order_release);
    fakeTransport.generation.store(1, std::memory_order_release);

    EventPublisher publisher(44100.0);
    publisher.setTransport(&fakeTransport);
    publisher.setCurrentGeneration(1);

    MusicalEvent ev;
    ev.sampleTs = 100;
    ev.localExecTs = 100;
    ev.sequence = 0;
    ev.vmGeneration = 1;
    REQUIRE(publisher.publishEvent(ev)); // should succeed

    // Change transport generation (simulates worker restart)
    fakeTransport.generation.store(2, std::memory_order_release);
    publisher.setCurrentGeneration(2);

    // Publisher generation 2 still matches transport generation 2 — should work
    ev.sampleTs = 200;
    ev.sequence = 1;
    REQUIRE(publisher.publishEvent(ev)); // should succeed with matching gen

    // Now mismatch: publisher says gen 1, transport is gen 2
    publisher.setCurrentGeneration(1);
    ev.sampleTs = 300;
    ev.sequence = 2;
    REQUIRE_FALSE(publisher.publishEvent(ev)); // should fail (gen mismatch)
}

// ---------------------------------------------------------------------------
// 14. Integration — end-to-end timestamp preservation
// ---------------------------------------------------------------------------

TEST_CASE("EventScheduler integration — timestamped event flows from publisher to scheduler", "[b4-k6][integration]")
{
    ClockSync clock(0.5, 50.0);
    clock.setSampleRates(44100.0, 44100.0);
    EventScheduler scheduler;

    SpscEventRing<256> ring;

    scheduler.setSampleCursor(0);
    scheduler.setCurrentVmGeneration(1);

    // Publisher generates an event at sample 320 (within the first buffer)
    MusicalEvent ev(hathor::EventType::InstrumentTrigger,
                    ParamMap{},
                    Rational(1, 2),  // musical timestamp
                    320,               // sample timestamp
                    0,                 // localExecTs (will be computed)
                    0,                 // sequence
                    1,                 // tab
                    1                  // vmGeneration
    );
    ev.sequence = 7;

    // Simulate transport through the ring
    ring.push(ev);

    // Scheduler stages the event
    uint32_t staged = scheduler.stageEvents(ring, clock);
    REQUIRE(staged == 1);

    // With zero clock offset, localExecTs == sampleTs == 320
    REQUIRE(scheduler.numStagedEvents() == 1);

    // Audio callback dispatches: buffer [0, 64) — event not ready
    MusicalEvent out[16];
    uint32_t outCount = 0;
    scheduler.getReadyEvents(0, 64, out, &outCount, 16);
    REQUIRE(outCount == 0);

    // Buffer [320, 384) — event should be ready
    scheduler.setSampleCursor(320);
    scheduler.getReadyEvents(320, 64, out, &outCount, 16);
    REQUIRE(outCount == 1);

    // Verify timestamp integrity
    REQUIRE(out[0].sampleTs == 320);
    REQUIRE(out[0].musicalTs == Rational(1, 2));
    REQUIRE(out[0].sequence == 7);
    REQUIRE(out[0].targetTabId == 1);

    REQUIRE(scheduler.numStagedEvents() == 0);
}
