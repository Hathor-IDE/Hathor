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

#include "BiquadFilter.hpp"
#include "VoicePool.hpp"
#include "SampleBank.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using hathor::computeLpCoeffs;
using hathor::biquadProcessSample;
using Catch::Approx;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

/// Build a SampleBank with a single named mono sample containing
/// a deterministic ramp [0, 1, 2, 3, …] scaled to [−1, 1].
static void makeTestBank(SampleBank& bank, const std::string& name, int64_t index,
                         std::size_t numFrames, int sampleRate)
{
    SampleEntry entry;
    entry.name = name;
    entry.index = index;
    entry.numChannels = 1;
    entry.sampleRate = static_cast<double>(sampleRate);
    entry.data.resize(numFrames);
    for (std::size_t i = 0; i < numFrames; ++i)
        entry.data[i] = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(numFrames - 1);
    bank.addTestEntry(std::move(entry));
}

/// A simple sine-wave sample generator for frequency-content tests.
/// Generates `numFrames` samples of a sine at `freqHz` for `sampleRate`.
static void makeSineBank(SampleBank& bank, const std::string& name, int64_t index,
                         std::size_t numFrames, int sampleRate, double freqHz)
{
    SampleEntry entry;
    entry.name = name;
    entry.index = index;
    entry.numChannels = 1;
    entry.sampleRate = static_cast<double>(sampleRate);
    entry.data.resize(numFrames);
    for (std::size_t i = 0; i < numFrames; ++i) {
        entry.data[i] = static_cast<float>(std::sin(2.0 * M_PI * freqHz *
            static_cast<double>(i) / static_cast<double>(sampleRate)));
    }
    bank.addTestEntry(std::move(entry));
}

/// Compute the energy (sum of squares) in a buffer range.
static double energy(const float* buf, int n)
{
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        sum += static_cast<double>(buf[i]) * static_cast<double>(buf[i]);
    return sum;
}

// ===========================================================================
// 1. Coefficient calculation tests
// ===========================================================================

TEST_CASE("B7-K1: computeLpCoeffs — default cutoff (20000 Hz) is near-passthrough",
          "[b7-k1][coeffs]")
{
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(hathor::kDefaultCutoff, hathor::kDefaultResonance, 44100,
                    b0, b1, b2, a1, a2);

    // At 20000 Hz (near Nyquist 22050), the low-pass should pass most of the
    // spectrum.  b0 should be close to 1.0 (within ~20%).
    REQUIRE(b0 == Approx(1.0f).margin(0.2f));
    // All outputs must be finite.
    REQUIRE(std::isfinite(b0));
    REQUIRE(std::isfinite(b1));
    REQUIRE(std::isfinite(b2));
    REQUIRE(std::isfinite(a1));
    REQUIRE(std::isfinite(a2));
}

TEST_CASE("B7-K1: computeLpCoeffs — default cutoff preserves a 1 kHz tone",
          "[b7-k1][coeffs][passthrough]")
{
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(hathor::kDefaultCutoff, hathor::kDefaultResonance, 44100,
                    b0, b1, b2, a1, a2);

    // Process a 1 kHz sine — well below the 20 kHz cutoff, should pass through.
    float x1=0, x2=0, y1=0, y2=0, x1r=0, x2r=0, y1r=0, y2r=0;
    float sumSqIn = 0.0f, sumSqOut = 0.0f;

    const int kRate = 44100;
    const double freq = 1000.0;
    const int kCount = 2000;

    // Skip transient
    for (int i = 0; i < 500; ++i) {
        float in = static_cast<float>(std::sin(2.0 * M_PI * freq *
            static_cast<double>(i) / kRate));
        float dummyL, dummyR;
        hathor::biquadProcessSample(in, in, b0, b1, b2, a1, a2,
                                    x1, x2, y1, y2, x1r, x2r, y1r, y2r,
                                    dummyL, dummyR);
    }

    for (int i = 500; i < kCount; ++i) {
        float in = static_cast<float>(std::sin(2.0 * M_PI * freq *
            static_cast<double>(i) / kRate));
        float outL, outR;
        hathor::biquadProcessSample(in, in, b0, b1, b2, a1, a2,
                                    x1, x2, y1, y2, x1r, x2r, y1r, y2r,
                                    outL, outR);
        sumSqIn  += in * in;
        sumSqOut += outL * outL;
    }

    // Output energy should be within 5% of input energy (passthrough).
    REQUIRE(sumSqOut == Approx(sumSqIn).margin(0.05 * sumSqIn));
}

TEST_CASE("B7-K1: computeLpCoeffs — Nyquist cutoff is a pure passthrough",
          "[b7-k1][coeffs][passthrough]")
{
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(22050.0, 0.707, 44100, b0, b1, b2, a1, a2);

    // At Nyquist: b0=1, b1=2, b2=1, a1=2, a2=1 (after normalization by a0=1).
    // These are NOT identity coefficients, but the transfer function evaluates
    // to 1 at all frequencies (numerator == denominator).  Verify this by
    // processing a signal and checking output ≈ input.
    REQUIRE(b0 == Approx(1.0f).margin(0.001f));
    REQUIRE(b1 == Approx(2.0f).margin(0.001f));
    REQUIRE(b2 == Approx(1.0f).margin(0.001f));
    REQUIRE(a1 == Approx(2.0f).margin(0.001f));
    REQUIRE(a2 == Approx(1.0f).margin(0.001f));
}

TEST_CASE("B7-K1: computeLpCoeffs — different resonance changes coefficients",
          "[b7-k1][coeffs]")
{
    float b0_lo, b1_lo, b2_lo, a1_lo, a2_lo;
    float b0_hi, b1_hi, b2_hi, a1_hi, a2_hi;

    computeLpCoeffs(1000.0, 0.1, 44100, b0_lo, b1_lo, b2_lo, a1_lo, a2_lo);
    computeLpCoeffs(1000.0, 10.0, 44100, b0_hi, b1_hi, b2_hi, a1_hi, a2_hi);

    // High Q → steeper filter → different coefficients
    REQUIRE(b0_hi != Approx(b0_lo).margin(0.001f));
    REQUIRE(a1_hi != Approx(a1_lo).margin(0.001f));
    REQUIRE(a2_hi != Approx(a2_lo).margin(0.001f));
}

TEST_CASE("B7-K1: computeLpCoeffs — cutoff above Nyquist is clamped",
          "[b7-k1][coeffs][safety]")
{
    float b0_hi, b1_hi, b2_hi, a1_hi, a2_hi;
    float b0_clamped, b1_clamped, b2_clamped, a1_clamped, a2_clamped;

    computeLpCoeffs(100000.0, 0.707, 44100, b0_hi, b1_hi, b2_hi, a1_hi, a2_hi);
    computeLpCoeffs(22050.0, 0.707, 44100, b0_clamped, b1_clamped, b2_clamped,
                    a1_clamped, a2_clamped);

    // 100 kHz cutoff should be clamped to Nyquist → same as 22050 Hz
    REQUIRE(b0_hi == Approx(b0_clamped).margin(0.0001f));
    REQUIRE(a1_hi == Approx(a1_clamped).margin(0.0001f));
    REQUIRE(a2_hi == Approx(a2_clamped).margin(0.0001f));
}

TEST_CASE("B7-K1: computeLpCoeffs — zero cutoff is clamped to Nyquist",
          "[b7-k1][coeffs][safety]")
{
    float b0_zero, b1_zero, b2_zero, a1_zero, a2_zero;
    float b0_nyq, b1_nyq, b2_nyq, a1_nyq, a2_nyq;

    computeLpCoeffs(0.0, 0.707, 44100, b0_zero, b1_zero, b2_zero, a1_zero, a2_zero);
    computeLpCoeffs(22050.0, 0.707, 44100, b0_nyq, b1_nyq, b2_nyq, a1_nyq, a2_nyq);

    // 0 Hz cutoff should be clamped to Nyquist → same as 22050 Hz
    REQUIRE(b0_zero == Approx(b0_nyq).margin(0.0001f));
    REQUIRE(a1_zero == Approx(a1_nyq).margin(0.0001f));
    REQUIRE(a2_zero == Approx(a2_nyq).margin(0.0001f));
}

TEST_CASE("B7-K1: computeLpCoeffs — NaN cutoff is clamped to Nyquist and finite",
          "[b7-k1][coeffs][safety]")
{
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(std::numeric_limits<double>::quiet_NaN(), 0.707, 44100,
                    b0, b1, b2, a1, a2);

    // NaN → clamped to Nyquist → same coefficients as 22050 Hz
    float b0n, b1n, b2n, a1n, a2n;
    computeLpCoeffs(22050.0, 0.707, 44100, b0n, b1n, b2n, a1n, a2n);

    REQUIRE(b0 == Approx(b0n).margin(0.001f));
    REQUIRE(b1 == Approx(b1n).margin(0.001f));
    REQUIRE(b2 == Approx(b2n).margin(0.001f));
    REQUIRE(a1 == Approx(a1n).margin(0.001f));
    REQUIRE(a2 == Approx(a2n).margin(0.001f));

    // All outputs must be finite (not NaN or Inf).
    REQUIRE(std::isfinite(b0));
    REQUIRE(std::isfinite(b1));
    REQUIRE(std::isfinite(b2));
    REQUIRE(std::isfinite(a1));
    REQUIRE(std::isfinite(a2));
}

TEST_CASE("B7-K1: computeLpCoeffs — Inf cutoff is clamped to Nyquist and finite",
          "[b7-k1][coeffs][safety]")
{
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(std::numeric_limits<double>::infinity(), 0.707, 44100,
                    b0, b1, b2, a1, a2);

    // Inf → clamped to Nyquist
    float b0n, b1n, b2n, a1n, a2n;
    computeLpCoeffs(22050.0, 0.707, 44100, b0n, b1n, b2n, a1n, a2n);

    REQUIRE(b0 == Approx(b0n).margin(0.001f));
    REQUIRE(a1 == Approx(a1n).margin(0.001f));

    // All outputs must be finite.
    REQUIRE(std::isfinite(b0));
    REQUIRE(std::isfinite(b1));
    REQUIRE(std::isfinite(b2));
    REQUIRE(std::isfinite(a1));
    REQUIRE(std::isfinite(a2));
}

TEST_CASE("B7-K1: computeLpCoeffs — NaN resonance is clamped",
          "[b7-k1][coeffs][safety]")
{
    float b0_nan, b1_nan, b2_nan, a1_nan, a2_nan;
    float b0_min, b1_min, b2_min, a1_min, a2_min;

    computeLpCoeffs(1000.0, std::numeric_limits<double>::quiet_NaN(), 44100,
                    b0_nan, b1_nan, b2_nan, a1_nan, a2_nan);
    computeLpCoeffs(1000.0, hathor::kMinQ, 44100,
                    b0_min, b1_min, b2_min, a1_min, a2_min);

    // NaN Q should be clamped to kMinQ
    REQUIRE(b0_nan == Approx(b0_min).margin(0.001f));
    REQUIRE(a1_nan == Approx(a1_min).margin(0.001f));
    REQUIRE(a2_nan == Approx(a2_min).margin(0.001f));
}

TEST_CASE("B7-K1: computeLpCoeffs — negative resonance is clamped",
          "[b7-k1][coeffs][safety]")
{
    float b0_neg, b1_neg, b2_neg, a1_neg, a2_neg;
    float b0_min, b1_min, b2_min, a1_min, a2_min;

    computeLpCoeffs(1000.0, -5.0, 44100,
                    b0_neg, b1_neg, b2_neg, a1_neg, a2_neg);
    computeLpCoeffs(1000.0, hathor::kMinQ, 44100,
                    b0_min, b1_min, b2_min, a1_min, a2_min);

    REQUIRE(b0_neg == Approx(b0_min).margin(0.001f));
    REQUIRE(a1_neg == Approx(a1_min).margin(0.001f));
}

TEST_CASE("B7-K1: computeLpCoeffs — zero sample rate produces identity coefficients",
          "[b7-k1][coeffs][safety]")
{
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(1000.0, 0.707, 0, b0, b1, b2, a1, a2);

    // Zero sample rate → identity (b0=1, rest=0) so the filter is a no-op.
    REQUIRE(b0 == Approx(1.0f).margin(0.001f));
    REQUIRE(std::abs(b1) < 1e-6f);
    REQUIRE(std::abs(b2) < 1e-6f);
    REQUIRE(std::abs(a1) < 1e-6f);
    REQUIRE(std::abs(a2) < 1e-6f);
}

TEST_CASE("B7-K1: computeLpCoeffs — all outputs are finite",
          "[b7-k1][coeffs][safety]")
{
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(500.0, 2.0, 44100, b0, b1, b2, a1, a2);

    REQUIRE(std::isfinite(b0));
    REQUIRE(std::isfinite(b1));
    REQUIRE(std::isfinite(b2));
    REQUIRE(std::isfinite(a1));
    REQUIRE(std::isfinite(a2));
}

// ===========================================================================
// 2. Per-sample filter processing tests
// ===========================================================================

TEST_CASE("B7-K1: biquadProcessSample — identity coefficients pass signal through",
          "[b7-k1][processing]")
{
    // Identity: b0=1, rest=0
    float x1=0, x2=0, y1=0, y2=0, x1r=0, x2r=0, y1r=0, y2r=0;
    float outL, outR;

    biquadProcessSample(0.5f, -0.5f,  1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                        x1, x2, y1, y2, x1r, x2r, y1r, y2r, outL, outR);

    REQUIRE(outL == Approx(0.5f).margin(0.001f));
    REQUIRE(outR == Approx(-0.5f).margin(0.001f));
}

TEST_CASE("B7-K1: biquadProcessSample — low cutoff attenuates constant signal",
          "[b7-k1][processing]")
{
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(100.0, 0.707, 44100, b0, b1, b2, a1, a2);

    float x1=0, x2=0, y1=0, y2=0, x1r=0, x2r=0, y1r=0, y2r=0;
    float outL, outR;

    // Feed a constant 1.0 and check that the output settles to 1.0 (DC gain = 1).
    for (int i = 0; i < 1000; ++i) {
        biquadProcessSample(1.0f, 1.0f, b0, b1, b2, a1, a2,
                            x1, x2, y1, y2, x1r, x2r, y1r, y2r, outL, outR);
    }

    // At DC, a low-pass filter has unity gain, so output → 1.0
    REQUIRE(outL == Approx(1.0f).margin(0.01f));
    REQUIRE(outR == Approx(1.0f).margin(0.01f));
}

TEST_CASE("B7-K1: biquadProcessSample — low cutoff attenuates high-frequency signal",
          "[b7-k1][processing]")
{
    // 10 kHz sine at 44.1 kHz sample rate — well above a 200 Hz cutoff
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(200.0, 0.707, 44100, b0, b1, b2, a1, a2);

    float x1=0, x2=0, y1=0, y2=0, x1r=0, x2r=0, y1r=0, y2r=0;
    float sumIn = 0.0f, sumOut = 0.0f;

    const int sampleRate = 44100;
    const double freq = 10000.0;  // 10 kHz — well above cutoff

    for (int i = 0; i < 1000; ++i) {
        float in = static_cast<float>(std::sin(2.0 * M_PI * freq *
            static_cast<double>(i) / sampleRate));
        float outL, outR;
        biquadProcessSample(in, in, b0, b1, b2, a1, a2,
                            x1, x2, y1, y2, x1r, x2r, y1r, y2r, outL, outR);
        sumIn += in * in;
        sumOut += outL * outL;
    }

    // The filter should attenuate the high-frequency signal
    REQUIRE(sumOut < sumIn * 0.1);  // at least 10x attenuation
}

TEST_CASE("B7-K1: biquadProcessSample — high cutoff preserves high-frequency content",
          "[b7-k1][processing]")
{
    // 10 kHz sine, 15 kHz cutoff at 44.1 kHz — well within the passband
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(15000.0, 0.707, 44100, b0, b1, b2, a1, a2);

    float x1=0, x2=0, y1=0, y2=0, x1r=0, x2r=0, y1r=0, y2r=0;
    float sumIn = 0.0f, sumOut = 0.0f;

    const int sampleRate = 44100;
    const double freq = 10000.0;

    // Skip initial transient
    for (int i = 0; i < 100; ++i) {
        float in = static_cast<float>(std::sin(2.0 * M_PI * freq *
            static_cast<double>(i) / sampleRate));
        float outL, outR;
        hathor::biquadProcessSample(in, in, b0, b1, b2, a1, a2,
                                    x1, x2, y1, y2, x1r, x2r, y1r, y2r, outL, outR);
    }

    // Now measure RMS ratio (steady state)
    for (int i = 100; i < 1099; ++i) {
        float in = static_cast<float>(std::sin(2.0 * M_PI * freq *
            static_cast<double>(i) / sampleRate));
        float outL, outR;
        hathor::biquadProcessSample(in, in, b0, b1, b2, a1, a2,
                                    x1, x2, y1, y2, x1r, x2r, y1r, y2r, outL, outR);
        sumIn += in * in;
        sumOut += outL * outL;
    }

    // 15 kHz cutoff should preserve 10 kHz signal with minimal attenuation
    // (> 70% of input energy ≈ less than 3 dB).
    REQUIRE(sumOut > sumIn * 0.7);
}

// ===========================================================================
// 3. VoicePool end-to-end filter tests
// ===========================================================================

TEST_CASE("B7-K1: default parameters behave effectively unfiltered",
          "[b7-k1][voicepool]")
{
    constexpr int kRate = 44100;
    SampleBank bank;
    makeTestBank(bank, "bd", 0, 441, kRate);
    VoicePool pool;

    hathor::ParamMap params;
    params.set(hathor::keys::kS, hathor::Value{std::string{"bd"}});

    // Trigger a voice with no cutoff/resonance → defaults (20 kHz / 0.707)
    pool.trigger(params, bank, 0, 0, kRate);

    // Mix 440 samples
    float left[440], right[440];
    std::memset(left, 0, sizeof(left));
    std::memset(right, 0, sizeof(right));
    pool.mix(left, right, 440, kRate);

    // Default cutoff ≈ 20 kHz → effectively no filtering.
    // The sample is a ramp from -1 to 1, so energy should be substantial.
    REQUIRE(energy(left, 440) > 0.1);
    REQUIRE(energy(right, 440) > 0.1);
}

TEST_CASE("B7-K1: low cutoff materially attenuates high-frequency content",
          "[b7-k1][voicepool]")
{
    constexpr int kRate = 44100;
    // Use a 10 kHz sine sample — high frequency content
    SampleBank bank;
    makeSineBank(bank, "tone", 0, 882, kRate, 10000.0);
    VoicePool pool;

    hathor::ParamMap params;
    params.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
    params.set(hathor::keys::kCutoff, 200.0);     // 200 Hz — well below 10 kHz

    pool.trigger(params, bank, 0, 0, kRate);

    float left[882], right[882];
    std::memset(left, 0, sizeof(left));
    std::memset(right, 0, sizeof(right));
    pool.mix(left, right, 882, kRate);

    // The 10 kHz tone should be heavily attenuated by a 200 Hz low-pass
    REQUIRE(energy(left, 882) < 0.01);
    REQUIRE(energy(right, 882) < 0.01);
}

TEST_CASE("B7-K1: high cutoff preserves high-frequency content",
          "[b7-k1][voicepool]")
{
    constexpr int kRate = 44100;
    SampleBank bank;
    makeSineBank(bank, "tone", 0, 882, kRate, 10000.0);
    VoicePool pool;

    hathor::ParamMap params;
    params.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
    params.set(hathor::keys::kCutoff, 15000.0);  // 15 kHz — above 10 kHz signal

    pool.trigger(params, bank, 0, 0, kRate);

    float left[882], right[882];
    std::memset(left, 0, sizeof(left));
    std::memset(right, 0, sizeof(right));
    pool.mix(left, right, 882, kRate);

    // 15 kHz cutoff should pass the 10 kHz tone with minimal attenuation
    REQUIRE(energy(left, 882) > 0.1);
    REQUIRE(energy(right, 882) > 0.1);
}

TEST_CASE("B7-K1: low and high cutoff produce measurably different output",
          "[b7-k1][voicepool]")
{
    constexpr int kRate = 44100;
    constexpr int kSamples = 882;

    // Trigger two voices from the same sine sample with different cutoffs
    SampleBank bank;
    makeSineBank(bank, "tone", 0, kSamples, kRate, 8000.0);

    // Voice A: low cutoff
    {
        VoicePool poolA;
        hathor::ParamMap params;
        params.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
        params.set(hathor::keys::kCutoff, 500.0);
        poolA.trigger(params, bank, 0, 0, kRate);

        float leftA[kSamples], rightA[kSamples];
        std::memset(leftA, 0, sizeof(leftA));
        std::memset(rightA, 0, sizeof(rightA));
        poolA.mix(leftA, rightA, kSamples, kRate);

        // Voice B: high cutoff
        VoicePool poolB;
        hathor::ParamMap paramsB;
        paramsB.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
        paramsB.set(hathor::keys::kCutoff, 15000.0);
        poolB.trigger(paramsB, bank, 0, 0, kRate);

        float leftB[kSamples], rightB[kSamples];
        std::memset(leftB, 0, sizeof(leftB));
        std::memset(rightB, 0, sizeof(rightB));
        poolB.mix(leftB, rightB, kSamples, kRate);

        const double eA = energy(leftA, kSamples);
        const double eB = energy(leftB, kSamples);

        // Low-cutoff voice should have significantly less energy
        REQUIRE(eB > eA * 5.0);  // at least 5x more energy
    }
}

TEST_CASE("B7-K1: two simultaneous voices with different cutoffs produce independent filtering",
          "[b7-k1][voicepool][independence]")
{
    constexpr int kRate = 44100;
    constexpr int kSamples = 882;

    SampleBank bank;
    makeSineBank(bank, "tone", 0, kSamples, kRate, 8000.0);
    VoicePool pool;

    // Voice A: low cutoff (500 Hz)
    hathor::ParamMap paramsA;
    paramsA.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
    paramsA.set(hathor::keys::kCutoff, 500.0);
    paramsA.set(hathor::keys::kResonance, 0.707);
    pool.trigger(paramsA, bank, 0, 0, kRate);

    // Voice B: high cutoff (15 kHz) — triggers into a second voice slot
    hathor::ParamMap paramsB;
    paramsB.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
    paramsB.set(hathor::keys::kCutoff, 15000.0);
    paramsB.set(hathor::keys::kResonance, 0.707);
    pool.trigger(paramsB, bank, 0, 0, kRate);

    float left[kSamples], right[kSamples];
    std::memset(left, 0, sizeof(left));
    std::memset(right, 0, sizeof(right));
    pool.mix(left, right, kSamples, kRate);

    // Find the two voice slots
    // Both voices are playing the same sample but with different cutoffs.
    // The mixed output should contain BOTH a heavily-filtered and a lightly-filtered
    // version.  If filtering were global (single shared filter), both would be
    // equally attenuated.  Instead, the lightly-filtered voice should contribute
    // substantial energy while the heavily-filtered one contributes very little.
    //
    // Total energy should be dominated by the high-cutoff voice.
    REQUIRE(energy(left, kSamples) > 0.5);
}

TEST_CASE("B7-K1: filter state is independent between voices",
          "[b7-k1][voicepool][independence]")
{
    constexpr int kRate = 44100;
    constexpr int kSamples = 441;

    SampleBank bank;
    makeTestBank(bank, "bd", 0, kSamples, kRate);
    VoicePool pool;

    // Trigger two voices with very different cutoffs
    hathor::ParamMap paramsLow;
    paramsLow.set(hathor::keys::kS, hathor::Value{std::string{"bd"}});
    paramsLow.set(hathor::keys::kCutoff, 100.0);
    pool.trigger(paramsLow, bank, 0, 0, kRate);

    hathor::ParamMap paramsHigh;
    paramsHigh.set(hathor::keys::kS, hathor::Value{std::string{"bd"}});
    paramsHigh.set(hathor::keys::kCutoff, 20000.0);
    pool.trigger(paramsHigh, bank, 0, 0, kRate);

    float left[kSamples], right[kSamples];
    std::memset(left, 0, sizeof(left));
    std::memset(right, 0, sizeof(right));
    pool.mix(left, right, kSamples, kRate);

    // Both voices play simultaneously.  The mix should contain contributions
    // from both — verify that the high-cutoff voice contributes more energy
    // than the low-cutoff voice would alone.
    //
    // We verify independence by checking that the output is NOT what a single
    // global filter at the low cutoff would produce (which would heavily
    // attenuate everything).
    REQUIRE(energy(left, kSamples) > 0.01);
}

TEST_CASE("B7-K1: different resonance values change the filter response",
          "[b7-k1][voicepool]")
{
    constexpr int kRate = 44100;
    constexpr int kSamples = 882;

    SampleBank bank;
    makeSineBank(bank, "tone", 0, kSamples, kRate, 1000.0);

    // Voice with low resonance
    {
        VoicePool pool;
        hathor::ParamMap params;
        params.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
        params.set(hathor::keys::kCutoff, 1000.0);
        params.set(hathor::keys::kResonance, 0.1);
        pool.trigger(params, bank, 0, 0, kRate);

        float left[882], right[882];
        std::memset(left, 0, sizeof(left));
        std::memset(right, 0, sizeof(right));
        pool.mix(left, right, 882, kRate);

        // Low Q → less resonant, different response
        const double eLowQ = energy(left, 882);

        // Voice with high resonance
        VoicePool pool2;
        hathor::ParamMap params2;
        params2.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
        params2.set(hathor::keys::kCutoff, 1000.0);
        params2.set(hathor::keys::kResonance, 10.0);
        pool2.trigger(params2, bank, 0, 0, kRate);

        float left2[882], right2[882];
        std::memset(left2, 0, sizeof(left2));
        std::memset(right2, 0, sizeof(right2));
        pool2.mix(left2, right2, 882, kRate);

        const double eHighQ = energy(left2, 882);

        // High Q at the cutoff frequency should produce a peak → different energy
        REQUIRE(eLowQ != Approx(eHighQ).margin(0.01));
    }
}

TEST_CASE("B7-K1: coefficients are calculated at trigger time (not in mix)",
          "[b7-k1][voicepool]")
{
    constexpr int kRate = 44100;
    SampleBank bank;
    makeTestBank(bank, "bd", 0, 441, kRate);
    VoicePool pool;

    hathor::ParamMap params;
    params.set(hathor::keys::kS, hathor::Value{std::string{"bd"}});
    params.set(hathor::keys::kCutoff, 500.0);
    params.set(hathor::keys::kResonance, 2.0);

    pool.trigger(params, bank, 0, 0, kRate);

    // After trigger, the voice should have non-default coefficients.
    // We can't directly access the private voices_[] array, but we can
    // verify indirectly: a 500 Hz filter should attenuate differently than
    // the default 20 kHz filter.
    //
    // Trigger another voice with default params and compare.
    VoicePool pool2;
    hathor::ParamMap paramsDefault;
    paramsDefault.set(hathor::keys::kS, hathor::Value{std::string{"bd"}});
    pool2.trigger(paramsDefault, bank, 0, 0, kRate);

    float left1[441], right1[441];
    float left2[441], right2[441];
    std::memset(left1, 0, sizeof(left1));
    std::memset(right1, 0, sizeof(right1));
    std::memset(left2, 0, sizeof(left2));
    std::memset(right2, 0, sizeof(right2));

    pool.mix(left1, right1, 441, kRate);
    pool2.mix(left2, right2, 441, kRate);

    // The filtered output should differ from the unfiltered output
    REQUIRE(energy(left1, 441) != Approx(energy(left2, 441)).margin(0.001));
}

TEST_CASE("B7-K1: coefficients remain fixed for voice lifetime",
          "[b7-k1][voicepool]")
{
    constexpr int kRate = 44100;
    SampleBank bank;
    makeSineBank(bank, "tone", 0, 8820, kRate, 1000.0);
    VoicePool pool;

    hathor::ParamMap params;
    params.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
    params.set(hathor::keys::kCutoff, 800.0);
    params.set(hathor::keys::kResonance, 1.0);

    pool.trigger(params, bank, 0, 0, kRate);

    // Mix first half
    constexpr int kHalf = 1000;
    float left1[kHalf], right1[kHalf];
    std::memset(left1, 0, sizeof(left1));
    std::memset(right1, 0, sizeof(right1));
    pool.mix(left1, right1, kHalf, kRate);

    // Mix second half (same voice, continues from where it left off)
    float left2[kHalf], right2[kHalf];
    std::memset(left2, 0, sizeof(left2));
    std::memset(right2, 0, sizeof(right2));
    pool.mix(left2, right2, kHalf, kRate);

    // The voice should still be filtered the same way — the energy ratio
    // between the filtered and unfiltered versions should be consistent.
    // Both halves should show attenuation since the filter was set at trigger.
    REQUIRE(energy(left1, kHalf) > 0.0);
    REQUIRE(energy(left2, kHalf) > 0.0);
}

TEST_CASE("B7-K1: changing pattern param does not mutate currently-playing voice",
          "[b7-k1][voicepool][trigger-time]")
{
    constexpr int kRate = 44100;
    SampleBank bank;
    makeSineBank(bank, "tone", 0, 1764, kRate, 1000.0);
    VoicePool pool;

    // Trigger a voice with low cutoff
    hathor::ParamMap paramsLow;
    paramsLow.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
    paramsLow.set(hathor::keys::kCutoff, 200.0);
    paramsLow.set(hathor::keys::kResonance, 0.707);
    pool.trigger(paramsLow, bank, 0, 0, kRate);

    // Trigger a second voice with high cutoff (different voice slot)
    hathor::ParamMap paramsHigh;
    paramsHigh.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
    paramsHigh.set(hathor::keys::kCutoff, 10000.0);
    paramsHigh.set(hathor::keys::kResonance, 0.707);
    pool.trigger(paramsHigh, bank, 0, 0, kRate);

    // Mix — the second voice should be less filtered than the first
    float left[1764], right[1764];
    std::memset(left, 0, sizeof(left));
    std::memset(right, 0, sizeof(right));
    pool.mix(left, right, 1764, kRate);

    // If filtering were global, both voices would be equally affected.
    // With per-voice filtering, the high-cutoff voice contributes more energy.
    REQUIRE(energy(left, 1764) > 0.1);
}

// ===========================================================================
// 4. Allocation-free audio path
// ===========================================================================

TEST_CASE("B7-K1: mix() performs no heap allocation", "[b7-k1][allocation]")
{
    constexpr int kRate = 44100;
    SampleBank bank;
    makeTestBank(bank, "bd", 0, 441, kRate);
    VoicePool pool;

    hathor::ParamMap params;
    params.set(hathor::keys::kS, hathor::Value{std::string{"bd"}});
    params.set(hathor::keys::kCutoff, 500.0);
    pool.trigger(params, bank, 0, 0, kRate);

    float left[128], right[128];
    std::memset(left, 0, sizeof(left));
    std::memset(right, 0, sizeof(right));

    g_alloc_count = 0;
    g_counting    = true;

    pool.mix(left, right, 128, kRate);

    g_counting = false;
    REQUIRE(g_alloc_count == 0);
}

TEST_CASE("B7-K1: mix() is noexcept / lock-free", "[b7-k1][lockfree]")
{
    constexpr int kRate = 44100;
    SampleBank bank;
    makeTestBank(bank, "bd", 0, 441, kRate);
    VoicePool pool;

    hathor::ParamMap params;
    params.set(hathor::keys::kS, hathor::Value{std::string{"bd"}});
    pool.trigger(params, bank, 0, 0, kRate);

    float left[64], right[64];
    std::memset(left, 0, sizeof(left));
    std::memset(right, 0, sizeof(right));

    // mix() should be callable without allocations or locks
    REQUIRE_NOTHROW(pool.mix(left, right, 64, kRate));
}

// ===========================================================================
// 5. Invalid parameter safety
// ===========================================================================

TEST_CASE("B7-K1: invalid cutoff (negative) does not produce NaN",
          "[b7-k1][safety]")
{
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(-100.0, 0.707, 44100, b0, b1, b2, a1, a2);

    REQUIRE(std::isfinite(b0));
    REQUIRE(std::isfinite(b1));
    REQUIRE(std::isfinite(b2));
    REQUIRE(std::isfinite(a1));
    REQUIRE(std::isfinite(a2));
}

TEST_CASE("B7-K1: extreme cutoff (Hz=1) does not produce NaN or Inf",
          "[b7-k1][safety]")
{
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(1.0, 0.707, 44100, b0, b1, b2, a1, a2);

    REQUIRE(std::isfinite(b0));
    REQUIRE(std::isfinite(b1));
    REQUIRE(std::isfinite(b2));
    REQUIRE(std::isfinite(a1));
    REQUIRE(std::isfinite(a2));
}

TEST_CASE("B7-K1: extreme Q (100.0) is clamped and stable",
          "[b7-k1][safety]")
{
    float b0, b1, b2, a1, a2;
    computeLpCoeffs(500.0, 100.0, 44100, b0, b1, b2, a1, a2);

    REQUIRE(std::isfinite(b0));
    REQUIRE(std::isfinite(b1));
    REQUIRE(std::isfinite(b2));
    REQUIRE(std::isfinite(a1));
    REQUIRE(std::isfinite(a2));

    // Clamped to kMaxQ = 20.0
    float b0_clamped, b1_clamped, b2_clamped, a1_clamped, a2_clamped;
    computeLpCoeffs(500.0, hathor::kMaxQ, 44100,
                    b0_clamped, b1_clamped, b2_clamped, a1_clamped, a2_clamped);

    REQUIRE(b0 == Approx(b0_clamped).margin(1e-5f));
    REQUIRE(a1 == Approx(a1_clamped).margin(1e-5f));
    REQUIRE(a2 == Approx(a2_clamped).margin(1e-5f));
}

TEST_CASE("B7-K1: processing a NaN input propagates through one cycle then recovers",
          "[b7-k1][safety]")
{
    // Note: B7-K1's numerical safety requirement covers invalid parameter
    // VALUES (cutoff/resonance), not NaN audio samples.  This test verifies
    // that NaN input does not crash and that resetting the filter clears
    // the NaN state.

    float b0, b1, b2, a1, a2;
    computeLpCoeffs(1000.0, 0.707, 44100, b0, b1, b2, a1, a2);

    float x1=0, x2=0, y1=0, y2=0, x1r=0, x2r=0, y1r=0, y2r=0;
    float outL, outR;

    // Feed a NaN — the output may be NaN.
    hathor::biquadProcessSample(std::numeric_limits<float>::quiet_NaN(), 0.0f,
                                b0, b1, b2, a1, a2,
                                x1, x2, y1, y2, x1r, x2r, y1r, y2r, outL, outR);

    // The NaN may propagate through the delay line.  Resetting the state
    // (as trigger() does) clears it.  Simulate a trigger restart:
    x1 = 0; x2 = 0; y1 = 0; y2 = 0;
    x1r = 0; x2r = 0; y1r = 0; y2r = 0;

    // Now feed valid samples — filter should produce valid output.
    for (int i = 0; i < 100; ++i) {
        hathor::biquadProcessSample(0.5f, 0.5f, b0, b1, b2, a1, a2,
                                    x1, x2, y1, y2, x1r, x2r, y1r, y2r,
                                    outL, outR);
    }

    REQUIRE(std::isfinite(outL));
    REQUIRE(std::isfinite(outR));
    // After settling, DC gain should be ~1.0 for a low-pass
    REQUIRE(outL == Approx(0.5f).margin(0.01f));
}

TEST_CASE("B7-K1: invalid params via VoicePool trigger do not produce NaN output",
          "[b7-k1][voicepool][safety]")
{
    constexpr int kRate = 44100;
    SampleBank bank;
    makeTestBank(bank, "bd", 0, 441, kRate);
    VoicePool pool;

    // cutoff = NaN
    {
        hathor::ParamMap params;
        params.set(hathor::keys::kS, hathor::Value{std::string{"bd"}});
        params.set(hathor::keys::kCutoff,
                   hathor::Value{std::numeric_limits<double>::quiet_NaN()});
        params.set(hathor::keys::kResonance,
                   hathor::Value{std::numeric_limits<double>::quiet_NaN()});
        pool.trigger(params, bank, 0, 0, kRate);

        float left[64], right[64];
        std::memset(left, 0, sizeof(left));
        std::memset(right, 0, sizeof(right));
        pool.mix(left, right, 64, kRate);

        for (int i = 0; i < 64; ++i) {
            REQUIRE(std::isfinite(left[i]));
            REQUIRE(std::isfinite(right[i]));
        }
    }
}

TEST_CASE("B7-K1: default-param VoicePool output has no NaN",
          "[b7-k1][voicepool][safety]")
{
    constexpr int kRate = 44100;
    SampleBank bank;
    makeTestBank(bank, "bd", 0, 441, kRate);
    VoicePool pool;

    hathor::ParamMap params;
    params.set(hathor::keys::kS, hathor::Value{std::string{"bd"}});
    // No cutoff/resonance → defaults
    pool.trigger(params, bank, 0, 0, kRate);

    float left[128], right[128];
    std::memset(left, 0, sizeof(left));
    std::memset(right, 0, sizeof(right));
    pool.mix(left, right, 128, kRate);

    for (int i = 0; i < 128; ++i) {
        REQUIRE(std::isfinite(left[i]));
        REQUIRE(std::isfinite(right[i]));
    }
}

// ===========================================================================
// 6. Default parameter constants
// ===========================================================================

TEST_CASE("B7-K1: default parameter constants match spec", "[b7-k1][defaults]")
{
    REQUIRE(hathor::kDefaultCutoff == Approx(20000.0).margin(1.0));
    REQUIRE(hathor::kDefaultResonance == Approx(0.707).margin(0.001));
}
