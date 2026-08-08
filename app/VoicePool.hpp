// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "SampleBank.hpp"
#include "hathor/ParamMap.hpp"

// ---------------------------------------------------------------------------
// Voice — a single concurrent sample-playback unit
// ---------------------------------------------------------------------------
// All fields set at trigger time; none change mid-voice (Req 10.6).
// readPos is double to support fractional sample positions for linear
// interpolation when speed != 1.0 (Req 10.7).
// ---------------------------------------------------------------------------

struct Voice {
    enum class State { Free, Playing };

    State       state       = State::Free;
    uint64_t    startSample = 0;       ///< Absolute sample when voice was triggered
    int64_t     cutGroup    = -1;      ///< -1 = no cut group
    int8_t      slotId      = -1;      ///< Originating slot index (-1 = no owner)
    float*      sampleData  = nullptr; ///< Raw pointer into SampleEntry::data (never owned)
    int         numChannels = 1;       ///< 1 (mono) or 2 (stereo) — matches SampleEntry
    std::size_t sampleLen   = 0;       ///< Total number of interleaved float samples
    double      readPos     = 0.0;     ///< Fractional read index (in frames, not samples)
    double      speed       = 1.0;     ///< Playback rate multiplier
    float       gain        = 1.0f;    ///< Amplitude multiplier
    float       pan         = 0.5f;    ///< Stereo position [0.0 = hard left, 1.0 = hard right]
    std::size_t beginSample = 0;       ///< Start frame index (after begin fraction applied)
    std::size_t endSample   = 0;       ///< Exclusive end frame index
};

// ---------------------------------------------------------------------------
// VoicePool — manages 32 concurrent voices
// ---------------------------------------------------------------------------
// Satisfies requirements:
//   Req 10.3  32-voice pool minimum
//   Req 10.4  Voice stealing (free → oldest non-group → oldest overall)
//   Req 10.5  Cut-group silencing on new trigger
//   Req 10.6  Params set at trigger time, not changed mid-voice
//   Req 10.7  Linear interpolation for speed != 1.0
//
// No heap allocation is performed inside trigger() or mix().
// ---------------------------------------------------------------------------

class VoicePool {
public:
    /// Minimum (and actual) number of concurrent voices.
    static constexpr int kVoices = 32;

    VoicePool() = default;

    /// Trigger a new voice from the given parameter map.
    ///
    /// @param params        Per-event parameters ("s", "n", "gain", "speed", "pan",
    ///                      "begin", "end", "cut").
    /// @param bank          Sample bank used to resolve "s"/"n" lookup.
    /// @param sampleOffset  Frame offset within the current audio buffer where the
    ///                      event fires (used for voice age comparison).
    /// @param currentSample Absolute sample clock at the start of the current buffer.
    /// @param slotId        Index of the slot that owns this voice (-1 if none).
    void trigger(const hathor::ParamMap& params,
                 const SampleBank&       bank,
                 int                     sampleOffset,
                 uint64_t                currentSample,
                 int8_t                  slotId = -1);

    /// Mix all Playing voices into the output buffers.
    ///
    /// @param left       Pointer to left-channel output buffer (numSamples frames).
    /// @param right      Pointer to right-channel output buffer (numSamples frames).
    /// @param numSamples Number of frames to render.
    void mix(float* left, float* right, int numSamples);

    /// Immediately silence all Playing voices (e.g. on transport stop).
    void silenceAll() noexcept;

    /// Immediately silence all Playing voices belonging to @p slotId (A3).
    void silenceSlot(int8_t slotId) noexcept;

private:
    std::array<Voice, kVoices> voices_{};

    /// Find an available voice slot using the stealing algorithm (Req 10.4).
    /// @param newCutGroup  Cut group of the incoming event (-1 if none).
    /// @param absoluteStart Absolute sample position of the new event (for age comparison).
    /// @return Index into voices_.
    int findVoiceSlot(int64_t newCutGroup, uint64_t absoluteStart) noexcept;
};
