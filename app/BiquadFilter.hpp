// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * BiquadFilter.hpp — RBJ Audio EQ Cookbook biquad filters.
 *
 * Header-only, allocation-free, no mutex.
 *
 * B7-K1 — Used by VoicePool (app/VoicePool.cpp) for the per-voice low-pass
 *         filter.  Coefficients are computed ONCE at trigger time (on the
 *         non-audio path) and remain fixed for the voice's lifetime.  The
 *         per-voice delay state advances sample-by-sample inside the audio
 *         callback's mix() loop.
 *
 * B7-K2 — Used by MasterEq (app/MasterEq.hpp) for the master-bus preset EQ.
 *         Supports low-shelf, peaking, and high-shelf filter types.
 *         Coefficients are computed on the worker/control thread; the complete
 *         replacement filter state is published to the audio thread via the
 *         same atomic-swap pattern used for SlotState (std::shared_ptr +
 *         std::atomic_store/load_explicit, Apple-Clang-compatible).
 *
 * Design notes:
 *   - Direct-form I biquad (two input delays + two output delays per channel).
 *   - Separate left/right delay state so stereo processing keeps independent filters.
 *   - Mono processing uses only the "L" chain (left==right samples get the same
 *     treatment, which is correct).
 *   - When cutoff ≈ Nyquist (default), b0 ≈ 1.0 and all other coefficients ≈ 0,
 *     making the filter effectively a passthrough (unity gain, no attenuation).
 *
 * Requirement references: B7-K1, B7-K2
 */

#include <cmath>
#include <cstdint>

namespace hathor {

// ---------------------------------------------------------------------------
// B7-K1: Biquad low-pass filter — coefficient calculation (RBJ Audio EQ Cookbook)
// ---------------------------------------------------------------------------

/// Default cutoff frequency in Hz (effectively "off" at normal sample rates).
inline constexpr double kDefaultCutoff = 20000.0;

/// Default resonance (Q) — Butterworth response.
inline constexpr double kDefaultResonance = 0.707;

/// Minimum Q to keep the biquad numerically stable.
inline constexpr double kMinQ = 0.1;

/// Maximum Q — chosen to stay numerically stable without self-oscillation.
inline constexpr double kMaxQ = 20.0;

/// Compute RBJ Audio EQ Cookbook low-pass biquad coefficients.
///
/// Normalizes by a0 so the caller can use the coefficients directly in the
/// direct-form-I difference equation:
///
///   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
///
/// @param cutoffHz    Cutoff frequency in Hz.  Non-positive, NaN, or Inf →
///                    Nyquist (filter off).  Clamped to [epsilon, Nyquist).
/// @param q           Quality factor / resonance.  Non-finite or out of range
///                    is clamped to [kMinQ, kMaxQ].
/// @param sampleRate  Audio sample rate in Hz.  Non-positive or non-finite →
///                    identity (b0=1, rest=0).
///
/// @param outB0       (filled) feed-forward coefficient b0/a0
/// @param outB1       (filled) feed-forward coefficient b1/a0
/// @param outB2       (filled) feed-forward coefficient b2/a0
/// @param outA1       (filled) feedback coefficient a1/a0, already **negated**
///                    so the caller subtracts it (matches the difference
///                    equation above).
/// @param outA2       (filled) feedback coefficient a2/a0, already **negated**.
///
/// Requirement references: B7-K1 §3, B7-K1 §7
inline void computeLpCoeffs(double  cutoffHz,
                            double  q,
                            int     sampleRate,
                            float&  outB0,
                            float&  outB1,
                            float&  outB2,
                            float&  outA1,
                            float&  outA2) noexcept
{
    // --- Sanitize inputs (B7-K1 §7: numerical safety on the non-audio path) ---

    // Guard against non-finite or non-positive sample rate.
    if (sampleRate <= 0 || !std::isfinite(static_cast<double>(sampleRate))) {
        // Fall back to identity (no filtering).
        outB0 = 1.0f; outB1 = 0.0f; outB2 = 0.0f; outA1 = 0.0f; outA2 = 0.0f;
        return;
    }

    // Clamp Q to a safe range.
    if (!std::isfinite(q) || q < kMinQ)
        q = kMinQ;
    else if (q > kMaxQ)
        q = kMaxQ;

    // Clamp cutoff to (0, Nyquist).  Below 0 or non-finite → Nyquist (off).
    const double nyquist = static_cast<double>(sampleRate) * 0.5;
    double cutoff = cutoffHz;
    if (!std::isfinite(cutoff) || cutoff <= 0.0)
        cutoff = nyquist;          // effectively off
    else if (cutoff >= nyquist)
        cutoff = nyquist;          // clamp to Nyquist

    // --- RBJ Audio EQ Cookbook low-pass ---
    const double omega    = 2.0 * M_PI * cutoff / static_cast<double>(sampleRate);
    const double sinOmega = std::sin(omega);
    const double cosOmega = std::cos(omega);

    const double alpha = sinOmega / (2.0 * q);

    const double a0 = 1.0 + alpha;
    // Guard against a0 == 0 (impossible with clamped Q > 0, but be defensive).
    if (a0 == 0.0) {
        outB0 = 1.0f; outB1 = 0.0f; outB2 = 0.0f; outA1 = 0.0f; outA2 = 0.0f;
        return;
    }

    const double b0 = (1.0 - cosOmega) / (2.0 * a0);
    const double b1 =  (1.0 - cosOmega) / a0;
    const double b2 = (1.0 - cosOmega) / (2.0 * a0);
    const double a1 = (-2.0 * cosOmega) / a0;   // we negate below
    const double a2 = (1.0 - alpha) / a0;       // we negate below

    outB0 = static_cast<float>(b0);
    outB1 = static_cast<float>(b1);
    outB2 = static_cast<float>(b2);
    // Store cookbook values directly.  The difference equation in
    // biquadProcessSample uses:  - a1*y[n-1] - a2*y[n-2]
    // so a1 = -2*cos(omega)/a0 and a2 = (1-alpha)/a0 from the cookbook.
    outA1 = static_cast<float>(a1);
    outA2 = static_cast<float>(a2);
}

// ---------------------------------------------------------------------------
// B7-K1: Biquad low-pass filter — per-sample direct-form-I processing
// ---------------------------------------------------------------------------

/// Process one sample pair (left, right) through a direct-form-I biquad.
///
/// The coefficients (b0, b1, b2, a1, a2) must be pre-computed via
/// computeLpCoeffs() and stored in the Voice.  The delay state (x1, x2, y1, y2
/// for left; x1r, x2r, y1r, y2r for right) must be initialized to zero at
/// trigger time and updated by this function on every call.
///
/// @param inL    Input sample (left channel).
/// @param inR    Input sample (right channel).
/// @param b0     Feed-forward coefficient.
/// @param b1     Feed-forward coefficient.
/// @param b2     Feed-forward coefficient.
/// @param a1     Feedback coefficient a1/a0 from the cookbook (subtracted in the
///               difference equation: -a1*y[n-1]).
/// @param a2     Feedback coefficient a2/a0 from the cookbook (subtracted in the
///               difference equation: -a2*y[n-2]).
/// @param x1     Input delay x[n-1], left  (read + write).
/// @param x2     Input delay x[n-2], left  (read + write).
/// @param y1     Output delay y[n-1], left (read + write).
/// @param y2     Output delay y[n-2], left (read + write).
/// @param x1r    Input delay x[n-1], right (read + write).
/// @param x2r    Input delay x[n-2], right (read + write).
/// @param y1r    Output delay y[n-1], right(read + write).
/// @param y2r    Output delay y[n-2], right(read + write).
///
/// @param outL   (filled) Filtered output sample (left channel).
/// @param outR   (filled) Filtered output sample (right channel).
///
/// Requirement references: B7-K1 §5
inline void biquadProcessSample(float  inL,
                                float  inR,
                                float  b0,
                                float  b1,
                                float  b2,
                                float  a1,
                                float  a2,
                                float& x1,
                                float& x2,
                                float& y1,
                                float& y2,
                                float& x1r,
                                float& x2r,
                                float& y1r,
                                float& y2r,
                                float& outL,
                                float& outR) noexcept
{
    // Direct-form I:
    //   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]

    outL = b0 * inL + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    outR = b0 * inR + b1 * x1r + b2 * x2r - a1 * y1r - a2 * y2r;

    // Shift delay line.
    x2  = x1;   x1  = inL;
    y2  = y1;   y1  = outL;

    x2r = x1r;  x1r = inR;
    y2r = y1r;  y1r = outR;
}

} // namespace hathor

// ---------------------------------------------------------------------------
// B7-K1 + B7-K2: Generic biquad coefficient + processing
// ---------------------------------------------------------------------------

/**
 * BiquadCoeffs — a single normalized (a0=1) biquad coefficient set.
 *
 * The difference equation is:
 *   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
 *
 * where a1, a2 are the cookbook (a1/a0, a2/a0) values stored directly
 * (the processing function subtracts them).
 *
 * Requirement references: B7-K1 §5, B7-K2 §2
 */
struct BiquadCoeffs {
    float b0 = 1.0f;   ///< feed-forward coefficient (b0/a0)
    float b1 = 0.0f;   ///< feed-forward coefficient (b1/a0)
    float b2 = 0.0f;   ///< feed-forward coefficient (b2/a0)
    float a1 = 0.0f;   ///< feedback coefficient (a1/a0, cookbook value)
    float a2 = 0.0f;   ///< feedback coefficient (a2/a0, cookbook value)

    /// Check that all coefficients are finite (not NaN or Inf).
    bool isFinite() const noexcept
    {
        return std::isfinite(b0) && std::isfinite(b1) && std::isfinite(b2)
            && std::isfinite(a1) && std::isfinite(a2);
    }

    /// Returns true if this is an identity/passthrough filter
    /// (b0=1, all others 0).
    bool isIdentity() const noexcept
    {
        return std::abs(b0 - 1.0f) < 1e-7f
            && std::abs(b1) < 1e-7f
            && std::abs(b2) < 1e-7f
            && std::abs(a1) < 1e-7f
            && std::abs(a2) < 1e-7f;
    }
};

/**
 * BiquadFilterState — coefficients + runtime delay state for one channel-pair.
 *
 * Coefficients are immutable once set (prepared on the worker/control thread).
 * The four delay values (x1/x2/y1/y2 for left, plus the right chain) advance
 * as samples are processed.
 *
 * Requirement references: B7-K2 §6 (filter state vs coefficients)
 */
struct BiquadFilterState {
    BiquadCoeffs coeffs;

    // Delay line state (left channel)
    float x1  = 0.0f;  ///< input delay  x[n-1] (left)
    float x2  = 0.0f;  ///< input delay  x[n-2] (left)
    float y1  = 0.0f;  ///< output delay y[n-1] (left)
    float y2  = 0.0f;  ///< output delay y[n-2] (left)

    // Delay line state (right channel)
    float x1r = 0.0f;  ///< input delay  x[n-1] (right)
    float x2r = 0.0f;  ///< input delay  x[n-2] (right)
    float y1r = 0.0f;  ///< output delay y[n-1] (right)
    float y2r = 0.0f;  ///< output delay y[n-2] (right)

    /// Zero all delay-state values.
    void reset() noexcept
    {
        x1 = 0.0f; x2 = 0.0f; y1 = 0.0f; y2 = 0.0f;
        x1r = 0.0f; x2r = 0.0f; y1r = 0.0f; y2r = 0.0f;
    }
};

/**
 * RBJ Audio EQ Cookbook filter types for the master-bus EQ.
 *
 * Requirement references: B7-K2 §2, B7-K2 §13
 */
enum class EqFilterType {
    LowShelf,   ///< low-shelf boost/cut
    Peak,       ///< peaking filter
    HighShelf,  ///< high-shelf boost/cut
};

/**
 * Compute RBJ Audio EQ Cookbook coefficients for a generic biquad.
 *
 * Supports low-shelf, peaking, and high-shelf filter types.
 * All outputs are normalized by a0 so they can be used directly in the
 * direct-form-I difference equation.
 *
 * @param type       Filter type (LowShelf, Peak, HighShelf).
 * @param freqHz     Frequency in Hz (cutoff for shelf, center for peak).
 * @param gainDb     Gain in decibels (boost > 0, cut < 0).
 * @param q          Quality factor / bandwidth.
 * @param sampleRate Audio sample rate in Hz.
 *
 * @return BiquadCoeffs with normalized b0/b1/b2/a1/a2.
 *         On invalid input (non-finite, zero sample rate, frequency out of
 *         range), returns identity coefficients (b0=1, rest=0).
 *
 * Requirement references: B7-K2 §2, §6, §13
 */
inline BiquadCoeffs computeEqCoeffs(EqFilterType type,
                                    double freqHz,
                                    double gainDb,
                                    double q,
                                    int sampleRate) noexcept
{
    // Guard against non-finite or non-positive sample rate → identity.
    if (sampleRate <= 0 || !std::isfinite(static_cast<double>(sampleRate)))
        return BiquadCoeffs{};

    // Clamp Q to a safe range.
    if (!std::isfinite(q) || q < kMinQ)
        q = kMinQ;
    else if (q > kMaxQ)
        q = kMaxQ;

    // Clamp frequency to (0, Nyquist).
    const double nyquist = static_cast<double>(sampleRate) * 0.5;
    double freq = freqHz;
    if (!std::isfinite(freq) || freq <= 0.0)
        freq = 1.0; // avoid log(0) / division by zero
    else if (freq >= nyquist)
        freq = nyquist * 0.999; // below Nyquist

    // Guard against non-finite gain.
    if (!std::isfinite(gainDb))
        gainDb = 0.0;

    const double A  = std::pow(10.0, gainDb / 40.0);  // amplitude ratio (for shelf/peak)
    const double omega = 2.0 * M_PI * freq / static_cast<double>(sampleRate);
    const double sinOmega = std::sin(omega);
    const double cosOmega = std::cos(omega);

    // For shelf/peak filters, alpha = sin(omega) / (2*Q) (RBJ cookbook).
    const double alpha = sinOmega / (2.0 * q);

    BiquadCoeffs coeffs;
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, a0 = 1.0;

    switch (type) {
        case EqFilterType::LowShelf: {
            // RBJ Audio EQ Cookbook — Low Shelf (fixed Q variant)
            const double sqrtA = std::sqrt(A);
            a0 =        (A + 1.0) - (A - 1.0) * cosOmega + 2.0 * sqrtA * alpha;
            b0 = 2.0 * A * ((A + 1.0) - (A - 1.0) * cosOmega + 2.0 * sqrtA * alpha);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosOmega);
            b2 = 2.0 * A * ((A + 1.0) - (A - 1.0) * cosOmega - 2.0 * sqrtA * alpha);
            a1 = 2.0 * ((A - 1.0) + (A + 1.0) * cosOmega);
            a2 =        (A + 1.0) - (A - 1.0) * cosOmega - 2.0 * sqrtA * alpha;
            break;
        }
        case EqFilterType::HighShelf: {
            // RBJ Audio EQ Cookbook — High Shelf (fixed Q variant)
            const double sqrtA = std::sqrt(A);
            a0 =        (A + 1.0) + (A - 1.0) * cosOmega + 2.0 * sqrtA * alpha;
            b0 = 2.0 * A * ((A + 1.0) + (A - 1.0) * cosOmega + 2.0 * sqrtA * alpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosOmega);
            b2 = 2.0 * A * ((A + 1.0) + (A - 1.0) * cosOmega - 2.0 * sqrtA * alpha);
            a1 = -2.0 * ((A - 1.0) - (A + 1.0) * cosOmega);
            a2 =        (A + 1.0) + (A - 1.0) * cosOmega - 2.0 * sqrtA * alpha;
            break;
        }
        case EqFilterType::Peak: {
            // RBJ Audio EQ Cookbook — Peaking EQ
            a0 = 1.0 + alpha / A;
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cosOmega;
            b2 = 1.0 - alpha * A;
            a1 = 2.0 * (alpha / A - cosOmega);
            a2 = 1.0 - alpha / A;
            break;
        }
    }

    // Guard against a0 == 0 (should not happen with clamped Q > 0).
    if (a0 == 0.0)
        return BiquadCoeffs{};

    // Normalize by a0
    coeffs.b0 = static_cast<float>(b0 / a0);
    coeffs.b1 = static_cast<float>(b1 / a0);
    coeffs.b2 = static_cast<float>(b2 / a0);
    // Store cookbook a1, a2 directly — the processing function subtracts them.
    coeffs.a1 = static_cast<float>(a1 / a0);
    coeffs.a2 = static_cast<float>(a2 / a0);

    return coeffs;
}

/**
 * Process one sample pair (left, right) through a direct-form-I biquad
 * using the coefficients and delay state in @p state.
 *
 * Requirement references: B7-K1 §5, B7-K2 §5
 */
inline void biquadProcessSample(float  inL,
                                float  inR,
                                const BiquadCoeffs& coeffs,
                                BiquadFilterState& state,
                                float& outL,
                                float& outR) noexcept
{
    // Direct-form I:
    //   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
    outL = coeffs.b0 * inL + coeffs.b1 * state.x1 + coeffs.b2 * state.x2
         - coeffs.a1 * state.y1 - coeffs.a2 * state.y2;
    outR = coeffs.b0 * inR + coeffs.b1 * state.x1r + coeffs.b2 * state.x2r
         - coeffs.a1 * state.y1r - coeffs.a2 * state.y2r;

    // Shift delay line.
    state.x2  = state.x1;  state.x1  = inL;
    state.y2  = state.y1;  state.y1  = outL;
    state.x2r = state.x1r; state.x1r = inR;
    state.y2r = state.y1r; state.y1r = outR;
}