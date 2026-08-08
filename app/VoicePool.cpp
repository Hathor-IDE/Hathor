// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "VoicePool.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

#include "hathor/Value.hpp"

// ---------------------------------------------------------------------------
// Helpers — extract typed values from a ParamMap entry
// ---------------------------------------------------------------------------

namespace {

/// Returns the double value if *v holds a double, else std::get<double> on the
/// int64_t case cast to double.  Handles both double and int64_t in params.
static double getDouble(const hathor::Value& v, double fallback) noexcept
{
    if (const double* d = std::get_if<double>(&v))
        return *d;
    if (const int64_t* i = std::get_if<int64_t>(&v))
        return static_cast<double>(*i);
    return fallback;
}

static int64_t getInt64(const hathor::Value& v, int64_t fallback) noexcept
{
    if (const int64_t* i = std::get_if<int64_t>(&v))
        return *i;
    if (const double* d = std::get_if<double>(&v))
        return static_cast<int64_t>(*d);
    return fallback;
}

static std::string_view getString(const hathor::Value& v) noexcept
{
    if (const std::string* s = std::get_if<std::string>(&v))
        return *s;
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// VoicePool::findVoiceSlot
// ---------------------------------------------------------------------------
// Implements the three-step voice stealing algorithm (Req 10.4):
//   1. Find any Free voice.
//   2. Find the oldest Playing voice NOT in the same cut group as the new event.
//   3. Fall back to the oldest Playing voice overall.
// ---------------------------------------------------------------------------

int VoicePool::findVoiceSlot(int64_t newCutGroup, uint64_t /*absoluteStart*/) noexcept
{
    // Step 1: free voice
    for (int i = 0; i < kVoices; ++i) {
        if (voices_[i].state == Voice::State::Free)
            return i;
    }

    // Step 2: oldest Playing voice not in the same cut group
    int    oldest2    = -1;
    uint64_t oldest2Start = std::numeric_limits<uint64_t>::max();
    for (int i = 0; i < kVoices; ++i) {
        const Voice& v = voices_[i];
        if (v.state == Voice::State::Playing && v.cutGroup != newCutGroup) {
            if (v.startSample < oldest2Start) {
                oldest2Start = v.startSample;
                oldest2      = i;
            }
        }
    }
    if (oldest2 != -1)
        return oldest2;

    // Step 3: oldest Playing voice overall
    int      oldest3      = 0;
    uint64_t oldest3Start = std::numeric_limits<uint64_t>::max();
    for (int i = 0; i < kVoices; ++i) {
        const Voice& v = voices_[i];
        if (v.state == Voice::State::Playing) {
            if (v.startSample < oldest3Start) {
                oldest3Start = v.startSample;
                oldest3      = i;
            }
        }
    }
    return oldest3;
}

// ---------------------------------------------------------------------------
// VoicePool::trigger
// ---------------------------------------------------------------------------

void VoicePool::trigger(const hathor::ParamMap& params,
                        const SampleBank&       bank,
                        int                     sampleOffset,
                        uint64_t                currentSample,
                        int8_t                  slotId)
{
    // ------------------------------------------------------------------
    // 1. Resolve sample name ("s") and index ("n")
    // ------------------------------------------------------------------
    std::string_view sampleName;
    int64_t          sampleIndex = 0;

    if (const hathor::Value* sv = params.get(hathor::keys::kS))
        sampleName = getString(*sv);
    if (const hathor::Value* nv = params.get(hathor::keys::kN))
        sampleIndex = getInt64(*nv, 0);

    if (sampleName.empty())
        return; // no sample specified — nothing to play

    const SampleEntry* entry = bank.find(sampleName, sampleIndex);
    if (!entry || entry->data.empty())
        return; // sample not found or empty

    // ------------------------------------------------------------------
    // 2. Read optional playback parameters
    // ------------------------------------------------------------------
    double gain  = 1.0;
    double speed = 1.0;
    double pan   = 0.5;
    double begin = 0.0;
    double end   = 0.0; // 0.0 means "play to end of sample"

    if (const hathor::Value* v = params.get(hathor::keys::kGain))
        gain = getDouble(*v, 1.0);
    if (const hathor::Value* v = params.get(hathor::keys::kSpeed))
        speed = getDouble(*v, 1.0);
    if (const hathor::Value* v = params.get(hathor::keys::kPan))
        pan = getDouble(*v, 0.5);
    if (const hathor::Value* v = params.get(hathor::keys::kBegin))
        begin = getDouble(*v, 0.0);
    if (const hathor::Value* v = params.get(hathor::keys::kEnd))
        end = getDouble(*v, 0.0);

    // Clamp pan to [0.0, 1.0] (Req 6.5)
    pan   = std::clamp(pan, 0.0, 1.0);
    // Clamp begin/end fractions to [0.0, 1.0]
    begin = std::clamp(begin, 0.0, 1.0);
    end   = std::clamp(end,   0.0, 1.0);
    // Guard against speed = 0 (would loop forever)
    if (speed == 0.0) speed = 1.0;

    // Derive frame count (data is interleaved, so total floats / numChannels = frames)
    const int         numCh      = entry->numChannels;
    const std::size_t totalFrames = entry->data.size() / static_cast<std::size_t>(numCh);

    // Convert begin/end fractions to absolute frame indices
    std::size_t beginFrame = static_cast<std::size_t>(begin * static_cast<double>(totalFrames));
    std::size_t endFrame;
    if (end <= 0.0 || end <= begin) {
        endFrame = totalFrames; // play to the end
    } else {
        endFrame = static_cast<std::size_t>(end * static_cast<double>(totalFrames));
    }
    // Ensure sensible range
    if (beginFrame >= totalFrames) beginFrame = 0;
    if (endFrame > totalFrames)    endFrame   = totalFrames;
    if (endFrame <= beginFrame)    endFrame   = totalFrames;

    // ------------------------------------------------------------------
    // 3. Read cut group
    // ------------------------------------------------------------------
    int64_t cutGroup = -1;
    if (const hathor::Value* v = params.get(hathor::keys::kCut))
        cutGroup = getInt64(*v, -1);

    // ------------------------------------------------------------------
    // 4. Apply cut-group silencing (Req 10.5)
    //    Immediately silence all voices in the same cut group (>0)
    // ------------------------------------------------------------------
    if (cutGroup > 0) {
        for (int i = 0; i < kVoices; ++i) {
            Voice& v = voices_[i];
            if (v.state == Voice::State::Playing && v.cutGroup == cutGroup) {
                // Silence at sampleOffset — for this Phase 1 implementation
                // we set the voice Free immediately (Req 10.5 says "immediately silence")
                (void)sampleOffset; // offset noted; instant silence is compliant
                v.state = Voice::State::Free;
            }
        }
    }

    // ------------------------------------------------------------------
    // 5. Voice stealing — find a slot (Req 10.4)
    // ------------------------------------------------------------------
    const uint64_t absoluteStart = currentSample + static_cast<uint64_t>(sampleOffset);
    const int      slot          = findVoiceSlot(cutGroup, absoluteStart);

    // ------------------------------------------------------------------
    // 6. Configure the voice (Req 10.6 — set once at trigger time)
    // ------------------------------------------------------------------
    Voice& voice        = voices_[slot];
    voice.state         = Voice::State::Playing;
    voice.startSample   = absoluteStart;
    voice.cutGroup      = cutGroup;
    voice.slotId        = slotId;
    // Raw pointer into the SampleEntry's immutable data — safe because the bank
    // is permanently read-only after load() (see design §2.4).
    voice.sampleData    = const_cast<float*>(entry->data.data());
    voice.numChannels   = numCh;
    voice.sampleLen     = entry->data.size(); // total interleaved floats
    voice.readPos       = static_cast<double>(beginFrame);
    voice.speed         = speed;
    voice.gain          = static_cast<float>(gain);
    voice.pan           = static_cast<float>(pan);
    voice.beginSample   = beginFrame;
    voice.endSample     = endFrame;
}

// ---------------------------------------------------------------------------
// VoicePool::mix
// ---------------------------------------------------------------------------
// Advances each Playing voice by numSamples, applying:
//   - Linear interpolation for fractional readPos when speed != 1.0 (Req 10.7)
//   - Equal-power pan: leftGain = sqrt(1-pan)*gain, rightGain = sqrt(pan)*gain
//   - Stereo and mono sample support (numChannels 1 or 2)
// No heap allocation; all arithmetic on stack variables.
// ---------------------------------------------------------------------------

void VoicePool::mix(float* left, float* right, int numSamples)
{
    for (int vi = 0; vi < kVoices; ++vi) {
        Voice& v = voices_[vi];
        if (v.state != Voice::State::Playing)
            continue;

        // Pre-compute per-voice gain/pan values
        const double sqrtPan    = std::sqrt(v.pan);
        const double sqrtOneMinusPan = std::sqrt(1.0f - v.pan);
        const float  leftGain   = static_cast<float>(sqrtOneMinusPan) * v.gain;
        const float  rightGain  = static_cast<float>(sqrtPan)         * v.gain;

        const int    numCh      = v.numChannels;
        // endFrame in terms of frames (sampleLen / numChannels = total frames)
        const std::size_t totalFrames = v.sampleLen / static_cast<std::size_t>(numCh);
        const std::size_t endFrame    = v.endSample;

        for (int s = 0; s < numSamples; ++s) {
            if (v.readPos >= static_cast<double>(endFrame)) {
                v.state = Voice::State::Free;
                break;
            }

            // Integer and fractional part of the read position (in frames)
            const std::size_t frame0 = static_cast<std::size_t>(v.readPos);
            const double      frac   = v.readPos - static_cast<double>(frame0);

            // Clamp frame0 to valid range
            if (frame0 >= totalFrames) {
                v.state = Voice::State::Free;
                break;
            }

            // Next frame index — clamped to avoid reading past the end
            const std::size_t frame1 = (frame0 + 1 < endFrame) ? frame0 + 1 : frame0;

            // Read left/mono channel with linear interpolation (Req 10.7)
            float sampleL, sampleR;

            if (numCh == 1) {
                // Mono: duplicate to both channels
                const float s0 = v.sampleData[frame0];
                const float s1 = v.sampleData[frame1];
                const float interp = static_cast<float>(s0 + frac * (s1 - s0));
                sampleL = interp;
                sampleR = interp;
            } else {
                // Stereo interleaved: [L0, R0, L1, R1, ...]
                const float l0 = v.sampleData[frame0 * 2];
                const float r0 = v.sampleData[frame0 * 2 + 1];
                const float l1 = v.sampleData[frame1 * 2];
                const float r1 = v.sampleData[frame1 * 2 + 1];
                sampleL = static_cast<float>(l0 + frac * (l1 - l0));
                sampleR = static_cast<float>(r0 + frac * (r1 - r0));
            }

            // Apply gain and pan, mix into output
            left[s]  += sampleL * leftGain;
            right[s] += sampleR * rightGain;

            // Advance read position by speed (Req 10.7)
            v.readPos += v.speed;
        }
    }
}

// ---------------------------------------------------------------------------
// VoicePool::silenceAll
// ---------------------------------------------------------------------------

void VoicePool::silenceAll() noexcept
{
    for (Voice& v : voices_)
        v.state = Voice::State::Free;
}

// ---------------------------------------------------------------------------
// VoicePool::silenceSlot
// ---------------------------------------------------------------------------

void VoicePool::silenceSlot(int8_t slotId) noexcept
{
    for (Voice& v : voices_)
        if (v.slotId == slotId)
            v.state = Voice::State::Free;
}
