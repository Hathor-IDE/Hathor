// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * BiquadFilter.hpp — RBJ Audio EQ Cookbook low-pass biquad filter.
 *
 * Header-only, allocation-free, no mutex.
 *
 * Used by VoicePool (app/VoicePool.cpp) for the B7-K1 per-voice low-pass filter.
 * Coefficients are computed ONCE at trigger time (on the non-audio path) and
 * remain fixed for the voice's lifetime.  The per-voice delay state advances
 * sample-by-sample inside the audio callback's mix() loop.
 *
 * Design notes:
 *   - Direct-form I biquad (two input delays + two output delays per channel).
 *   - Separate left/right delay state so stereo voices keep independent filters.
 *   - Mono voices use only the "L" chain (left==right samples get the same
 *     treatment, which is correct).
 *   - When cutoff ≈ Nyquist (default), b0 ≈ 1.0 and all other coefficients ≈ 0,
 *     making the filter effectively a passthrough (unity gain, no attenuation).
 *
 * Requirement references: B7-K1
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
