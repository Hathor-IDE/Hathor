// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * MasterEq.hpp — B7-K2 Master-bus preset EQ.
 *
 * Four fixed presets (Flat, Bass Boost, Vocal, Bright) applied as a 2-3 band
 * filter chain at the master mix stage, AFTER per-voice B7-K1 filtering and
 * AFTER ChucK audio is mixed into the master signal, but BEFORE the final
 * master gain.
 *
 * Signal chain (decision #13 — NOT negotiable):
 *
 *     per-voice processing
 *         ↓
 *     voice mix  +  ChucK audio
 *         ↓
 *     Master EQ    ← B7-K2 lives here
 *         ↓
 *     Final Master Gain
 *         ↓
 *     Output
 *
 * Architecture:
 *
 *   - EqBand: a single filter band (type, freq, gain, Q) — a pure value type
 *     used to describe a band at design time.
 *
 *   - MasterEqState: an INSTANTIATED, IMMUTABLE filter chain. Contains the
 *     concrete BiquadFilterState delay values (which advance per sample) and
 *     the frozen BiquadCoeffs (prepared once at preset-selection time).
 *     The audio thread loads the current state via atomic_load, processes
 *     samples against it, and never mutates the coefficients.
 *
 *   - The control/worker thread calls computePresetState() to build a complete
 *     replacement MasterEqState, then publishes it via
 *     AudioEngine::publishEqState() using the same
 *     std::atomic_store_explicit(shared_ptr, release) /
 *     std::atomic_load_explicit(shared_ptr, acquire) pattern already used for
 *     SlotState (Apple-Clang-compatible per AudioEngine.hpp §"Hot-swap slots").
 *
 *   - No mutex, no allocation in the audio callback.  The worker thread may
 *     allocate while building the replacement; the audio thread only dereferences
 *     an already-constructed shared_ptr and processes a fixed number of bands
 *     with simple float arithmetic.
 *
 * Preset band specifications (deterministic, B7-K2 §2):
 *
 *   Flat:
 *     - No bands.  Effect is a passthrough (identity).  Initial/default.
 *
 *   Bass Boost:
 *     - Low-shelf: f=100 Hz, gain=+3.0 dB, Q=0.9
 *     One band — controlled low-frequency lift, no excessive gain.
 *
 *   Vocal:
 *     - Peak:      f=1000 Hz, gain=+2.0 dB, Q=1.2
 *     - Low-shelf: f=120 Hz, gain=+1.5 dB, Q=0.9  (gentle low-end lift)
 *     Two bands — mid-focused response that brings vocal/melodic content forward.
 *
 *   Bright:
 *     - High-shelf: f=5000 Hz, gain=+2.0 dB, Q=0.9
 *     - Peak:        f=12000 Hz, gain=+1.0 dB, Q=0.8 (gentle air)
 *     Two bands — controlled high-frequency emphasis, avoids harshness.
 *
 * Requirement references: B7-K2 §1 through §13, Decision #13
 */

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "BiquadFilter.hpp"

namespace hathor {

// ---------------------------------------------------------------------------
// EqPreset — the exactly-four v1 presets (B7-K2 §1)
// ---------------------------------------------------------------------------

enum class EqPreset : int {
    Flat      = 0,
    BassBoost = 1,
    Vocal     = 2,
    Bright    = 3,
};

/// String names for each preset (for logging / future UI).
inline const char* presetName(EqPreset p) noexcept
{
    switch (p) {
        case EqPreset::Flat:      return "Flat";
        case EqPreset::BassBoost: return "Bass Boost";
        case EqPreset::Vocal:     return "Vocal";
        case EqPreset::Bright:    return "Bright";
    }
    return "Unknown";
}

/// Returns the full list of EqPreset values (exactly 4 — no more, no less).
inline constexpr std::array<EqPreset, 4> allPresets() noexcept
{
    return {EqPreset::Flat, EqPreset::BassBoost, EqPreset::Vocal, EqPreset::Bright};
}

/// Count of v1 presets (must be exactly 4 — B7-K2 §1).
inline constexpr int kNumEqPresets = 4;

// ---------------------------------------------------------------------------
// EqBand — a single filter band descriptor (design-time, value type)
// ---------------------------------------------------------------------------

struct EqBand {
    EqFilterType type;
    double       freqHz;   ///< center / shelf frequency in Hz
    double       gainDb;   ///< boost/cut in dB
    double       q;        ///< quality factor

    /// Compute the biquad coefficients for this band at the given sample rate.
    BiquadCoeffs computeCoeffs(int sampleRate) const noexcept
    {
        return computeEqCoeffs(type, freqHz, gainDb, q, sampleRate);
    }
};

// ---------------------------------------------------------------------------
// EqPresetSpec — the fixed band definitions for each preset (B7-K2 §2)
// ---------------------------------------------------------------------------
// These are compile-time constants — deterministic, no runtime computation
// beyond coefficient evaluation.  Each preset has 0–2 bands (Flat has 0).

/// Maximum bands across all presets (Bass Boost = 1, Vocal/Bright = 2).
inline constexpr int kMaxEqBands = 2;

/// Fixed band specifications for each v1 preset.
struct PresetSpec {
    EqPreset              preset;
    const char*           name;
    std::array<EqBand, kMaxEqBands> bands;
    int                   bandCount;
};

/// Returns the band specifications for a given preset.
/// Flat has 0 bands; all others have 1–2 bands.
inline const PresetSpec& presetSpec(EqPreset p) noexcept
{
    static const std::array<PresetSpec, kNumEqPresets> specs = {{
        // Flat — identity / passthrough (B7-K2 §8)
        { EqPreset::Flat, "Flat",
          { EqBand{EqFilterType::LowShelf, 100.0, 0.0, 0.9},
            EqBand{EqFilterType::LowShelf, 5000.0, 0.0, 0.9} },
          0 /* bandCount: 0 bands → identity */ },
        // Bass Boost — controlled low-frequency lift (B7-K2 §2)
        { EqPreset::BassBoost, "Bass Boost",
          { EqBand{EqFilterType::LowShelf, 100.0, +3.0, 0.9},
            EqBand{EqFilterType::LowShelf, 5000.0, 0.0, 0.9} },
          1 },
        // Vocal — mid-focused response (B7-K2 §2)
        { EqPreset::Vocal, "Vocal",
          { EqBand{EqFilterType::Peak,     1000.0, +2.0, 1.2},
            EqBand{EqFilterType::LowShelf, 120.0,  +1.5, 0.9} },
          2 },
        // Bright — controlled high-frequency emphasis (B7-K2 §2)
        { EqPreset::Bright, "Bright",
          { EqBand{EqFilterType::HighShelf, 5000.0,  +2.0, 0.9},
            EqBand{EqFilterType::Peak,      12000.0, +1.0, 0.8} },
          2 },
    }};
    return specs[static_cast<int>(p)];
}

// ---------------------------------------------------------------------------
// MasterEqState — immutable, complete EQ filter chain + delay state
// ---------------------------------------------------------------------------
//
// This is the state that is atomically swapped into the audio thread.
// It contains BOTH the frozen coefficients AND the runtime delay values.
//
// The control/worker thread constructs a COMPLETE MasterEqState (with all
// coefficients pre-computed and all delay values zeroed), then publishes it
// atomically.  The audio thread loads it, processes samples against it
// (advancing delays), and the next swap replaces the whole object.
//
// The delay state advances naturally as the old state is consumed — it is
// NOT carried over to the new state during a swap (see §6: "transition from
// old preset state to the new preset safely" via a complete replacement).
// The small number of bands and the smooth coefficient transitions of the
// RBJ cookbook minimize click risk.
//
// Requirement references: B7-K2 §5, §6
// ---------------------------------------------------------------------------

struct MasterEqState {
    /// The preset that produced these coefficients (for introspection).
    EqPreset preset;

    /// Number of active filter bands (0 for Flat → identity).
    int bandCount;

    /// Frozen coefficients per band.  Only bands[0..bandCount-1] are active.
    std::array<BiquadCoeffs, kMaxEqBands> coeffs;

    /// Runtime delay state per band.  These advance as samples are processed.
    /// Zeroed at construction; the audio thread mutates these in-place.
    std::array<BiquadFilterState, kMaxEqBands> filters;

    MasterEqState() noexcept
        : preset(EqPreset::Flat)
        , bandCount(0)
    {}

    /// Construct a MasterEqState for a specific preset, computing all
    /// coefficients upfront and zeroing all filter delay state.
    /// Called on the control/worker thread (may allocate via shared_ptr).
    static std::shared_ptr<MasterEqState> create(EqPreset p, int sampleRate)
    {
        auto state = std::make_shared<MasterEqState>();
        state->preset = p;

        const PresetSpec& spec = presetSpec(p);
        state->bandCount = spec.bandCount;

        for (int i = 0; i < spec.bandCount; ++i) {
            state->coeffs[i]  = spec.bands[i].computeCoeffs(sampleRate);
            state->filters[i] = BiquadFilterState{};  // zeroed delay state
        }

        return state;
    }

    /// Process a single stereo sample pair through the entire band chain.
    /// Allocation-free, no mutex — called from the audio thread only.
    ///
    /// Bands are applied in series: sample flows through band[0], then band[1].
    /// For Flat (bandCount==0), input passes through unchanged.
    void processSample(float inL, float inR, float& outL, float& outR) noexcept
    {
        float curL = inL, curR = inR;
        for (int i = 0; i < bandCount; ++i) {
            float tmpL, tmpR;
            biquadProcessSample(curL, curR, coeffs[i], filters[i], tmpL, tmpR);
            curL = tmpL;
            curR = tmpR;
        }
        outL = curL;
        outR = curR;
    }
};

/// Number of presets (compile-time check).
static_assert(kNumEqPresets == 4,
    "B7-K2 §1: exactly four EQ presets must exist");

} // namespace hathor
