// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * MusicalEvent.hpp — timestamped musical event model for B4-K6.
 *
 * This is the event representation that flows from the Tidal/Hathor master
 * clock (Phase-1 pattern scheduler) through IPC to the ChucK worker.
 *
 * Per Decision #22: delivery timing != execution timing.
 * Per Decision #23: control plane and audio plane are separate concerns;
 * the event metadata travels in-band as event metadata over the control/event
 * transport, but the actual execution timing is based on the authoritative
 * sample/frame timestamp.
 *
 * Event model (PROGRAM.md §B4-K6):
 *   Event
 *   ├── type          — MusicalEvent::Type enum
 *   ├── payload       — ParamMap (reuses the engine's existing ParamMap)
 *   ├── musicalTs     — musical timestamp (Rational cycles, from Hathor clock)
 *   ├── sampleTs      — audio sample-frame timestamp (absolute sample position)
 *   ├── sequence      — monotonic per-producer sequence number (deterministic ordering)
 *   ├── targetTabId   — intended ChucK target VM (TabId)
 *   └── vmGeneration  — the VM generation this event targets (stale-event guard)
 *
 * Clock ownership (PROGRAM.md V2 Architecture §4):
 *   - Tidal/Hathor master clock = authoritative musical time.
 *   - The worker is an execution engine/renderer; it does NOT become a second
 *     independent musical clock.
 *
 * Timestamp generation (PROGRAM.md §"Timestamp Generation"):
 *   The Phase-1 scheduler (AudioEngine::audioDeviceIOCallbackWithContext)
 *   generates timestamps BEFORE transport — never after IPC delivery.
 *
 * Sample/frame timestamp semantics:
 *   - Unit: audio samples (frames) at the configured sample rate.
 *   - Origin: sample 0 = the moment the transport clock started (audioDeviceAboutToStart).
 *   - The sample timestamp refers to the START of the event's logical execution
 *     within the audio timeline — i.e., the first sample at which the event
 *     should take effect.
 *   - Conversion from Hathor cycle time to samples:
 *       sampleTs = (cyclePosition * 60 * sampleRate) / bpm
 *     where cyclePosition is in Hathor cycles (Rational).
 *
 * Requirements: B4-K6, Decision #22, Decision #23
 */

#include "hathor/ParamMap.hpp"
#include "hathor/Rational.hpp"

#include <atomic>
#include <cstdint>

namespace hathor {

// ---------------------------------------------------------------------------
// Event types — Phase-D requirement for explicit ChucK events
// ---------------------------------------------------------------------------

enum class EventType : uint8_t {
    NoteOn,          ///< Start a note / instrument trigger
    NoteOff,         ///< Stop a note / instrument trigger
    ParameterChange, ///< Scheduled parameter change (gain, cutoff, etc.)
    InstrumentTrigger,///< Instrument trigger (e.g. start a ChucK shred)
    ControlChange,   ///< Scheduled control change (LFO, envelope, etc.)
};

// ---------------------------------------------------------------------------
// MusicalEvent — the timestamped musical event
// ---------------------------------------------------------------------------

struct MusicalEvent {
    EventType  type;           ///< event type (enum, not string — allocation-free)
    ParamMap   payload;       ///< event payload (reuses engine's ParamMap — SSO keys)
    Rational   musicalTs;     ///< musical timestamp (cycles from master clock)
    uint64_t   sampleTs;      ///< audio sample-frame timestamp (master/absolute, at sampleRate)
    uint64_t   localExecTs;   ///< worker-local sample position for execution (precomputed, RT-safe)
    uint64_t   sequence;      ///< monotonic per-producer sequence number (deterministic ordering)
    uint8_t    targetTabId;   ///< intended ChucK VM (TabId, matches SlotState)
    uint64_t   vmGeneration;  ///< VM generation this event targets (stale guard)

    // Default: NoteOn with empty payload, at cycle 0 / sample 0.
    // sequence defaults to 0 — callers should assign a monotonic value.
    MusicalEvent()
        : type(EventType::NoteOn)
        , musicalTs(0)
        , sampleTs(0)
        , localExecTs(0)
        , sequence(0)
        , targetTabId(0)
        , vmGeneration(0)
    {}

    // Full constructor for explicit construction.
    MusicalEvent(EventType   t,
                 ParamMap  && p,
                 Rational    musTs,
                 uint64_t    sampTs,
                 uint64_t    locTs,
                 uint64_t    seq,
                 uint8_t     tabId = 0,
                 uint64_t    gen   = 0)
        : type(t)
        , payload(std::move(p))
        , musicalTs(musTs)
        , sampleTs(sampTs)
        , localExecTs(locTs)
        , sequence(seq)
        , targetTabId(tabId)
        , vmGeneration(gen)
    {}

    // Compare by LOCAL execution timestamp for ordering. Used by the event
    // scheduler's sort key. Equal timestamps are further ordered by sequence.
    bool operator<(const MusicalEvent& other) const noexcept
    {
        if (localExecTs != other.localExecTs)
            return localExecTs < other.localExecTs;
        return sequence < other.sequence;
    }
};

// ---------------------------------------------------------------------------
// Timing measurement record (off-thread instrumentation, NOT in audio callback)
// ---------------------------------------------------------------------------

struct EventTimingRecord {
    uint64_t eventSequence;     ///< which event this measurement is for
    uint64_t targetSampleTs;    ///< the event's authoritative sample timestamp
    uint64_t sendTimeNs;        ///< wall-clock when the event was sent (perf_clock)
    uint64_t receiveTimeNs;     ///< wall-clock when the worker received the event
    uint64_t actualExecSample;  ///< sample position at which the event actually executed
    int64_t  schedulingError;   ///< actualExecSample - targetSampleTs (signed)
    int64_t  transportLatency;  ///< receiveTimeNs - sendTimeNs (signed, nanoseconds)
    int64_t  clockOffset;       ///< measured worker_sample_ts - master_sample_ts at receive
    uint8_t  targetTabId;       ///< which VM the event targeted
    uint64_t vmGeneration;      ///< VM generation at time of scheduling
    bool     wasLate;           ///< true if the event arrived after its target timestamp
    bool     wasEarly;          ///< true if the event arrived before its target timestamp
};

} // namespace hathor
