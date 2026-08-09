// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Standalone ear-test for B7-K1 per-voice biquad low-pass filter.
// Generates a synth waveform, triggers two voices with different cutoff
// settings, and writes the output to a 16-bit WAV file for manual listening.
//
// Build:  g++ -std=c++20 -I/app -I/engine/include -o ear_test ear_test_b7_k1.cpp app/VoicePool.cpp
// Run:    ./ear_test
//

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "VoicePool.hpp"
#include "BiquadFilter.hpp"
#include "hathor/Value.hpp"
#include "SampleBank.hpp"

namespace {

constexpr int kRate = 44100;
constexpr int kNumVoices = 32;
constexpr int kBlockSize = 512;

// ---------------------------------------------------------------------------
// Simple WAV writer (16-bit PCM, stereo)
// ---------------------------------------------------------------------------

void writeWav(const std::string& path, const std::vector<float>& left,
              const std::vector<float>& right, int sampleRate)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror("fopen"); return; }

    const int32_t numSamples = static_cast<int32_t>(left.size());
    const int32_t dataSize = numSamples * 4;  // 2 channels * 16 bits = 4 bytes

    // RIFF header
    fwrite("RIFF", 1, 4, f);
    uint32_t chunkSize = 36 + static_cast<uint32_t>(dataSize);
    fwrite(&chunkSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    // fmt chunk
    fwrite("fmt ", 1, 4, f);
    uint32_t subchunk1Size = 16;
    fwrite(&subchunk1Size, 4, 1, f);
    uint16_t audioFormat = 1;  // PCM
    fwrite(&audioFormat, 2, 1, f);
    uint16_t numChannels = 2;
    fwrite(&numChannels, 2, 1, f);
    uint32_t sampleRate32 = static_cast<uint32_t>(sampleRate);
    fwrite(&sampleRate32, 4, 1, f);
    uint32_t byteRate = static_cast<uint32_t>(sampleRate) * 4;
    fwrite(&byteRate, 4, 1, f);
    uint16_t blockAlign = 4;
    fwrite(&blockAlign, 2, 1, f);
    uint16_t bitsPerSample = 16;
    fwrite(&bitsPerSample, 2, 1, f);

    // data chunk
    fwrite("data", 1, 4, f);
    uint32_t dataSize32 = static_cast<uint32_t>(dataSize);
    fwrite(&dataSize32, 4, 1, f);

    for (int i = 0; i < numSamples; ++i) {
        int16_t l = static_cast<int16_t>(
            std::clamp(left[i], -1.0f, 1.0f) * 32767.0f);
        int16_t r = static_cast<int16_t>(
            std::clamp(right[i], -1.0f, 1.0f) * 32767.0f);
        fwrite(&l, 2, 1, f);
        fwrite(&r, 2, 1, f);
    }

    fclose(f);
    printf("Wrote %s (%d samples, %d Hz, stereo, 16-bit)\n",
           path.c_str(), numSamples, sampleRate);
}

// ---------------------------------------------------------------------------
// Generate a SampleBank with a synthetic tone sample (sawtooth + sine)
// ---------------------------------------------------------------------------

SampleBank makeToneBank(int sampleRate)
{
    SampleBank bank;
    SampleEntry entry;
    entry.name = "tone";
    entry.index = 0;
    entry.numChannels = 1;
    entry.sampleRate = static_cast<double>(sampleRate);

    // 2 seconds of a rich harmonic signal (sawtooth + sine)
    const int numFrames = sampleRate * 2;
    entry.data.resize(numFrames);
    for (int i = 0; i < numFrames; ++i) {
        double t = static_cast<double>(i) / sampleRate;
        // Sawtooth at 220 Hz (rich in harmonics)
        double saw = 0.0;
        for (int h = 1; h <= 8; ++h)
            saw += std::sin(2.0 * M_PI * 220.0 * h * t) / h;
        // Sine at 440 Hz
        double sine = std::sin(2.0 * M_PI * 440.0 * t);
        entry.data[i] = 0.3f * static_cast<float>(saw) + 0.3f * static_cast<float>(sine);
    }

    bank.addTestEntry(std::move(entry));
    return bank;
}

} // namespace

int main()
{
    printf("B7-K1 ear-test: per-voice biquad low-pass filter\n");
    printf("Sample rate: %d Hz\n", kRate);

    SampleBank bank = makeToneBank(kRate);
    VoicePool pool;

    // We'll render ~4 seconds of audio
    const int kSeconds = 4;
    const int kTotalSamples = kRate * kSeconds;
    std::vector<float> left(kTotalSamples, 0.0f);
    std::vector<float> right(kTotalSamples, 0.0f);

    // --- Pass 1: Voice A (low cutoff 500 Hz) ---
    // Trigger at t=0
    {
        hathor::ParamMap params;
        params.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
        params.set(hathor::keys::kCutoff, 500.0);
        params.set(hathor::keys::kResonance, 0.707);
        params.set(hathor::keys::kGain, 0.8);
        params.set(hathor::keys::kPan, 0.0);  // hard left
        pool.trigger(params, bank, 0, 0, kRate);
    }

    // --- Pass 2: Voice B (high cutoff 12000 Hz) ---
    // Trigger at t=1s
    {
        hathor::ParamMap params;
        params.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
        params.set(hathor::keys::kCutoff, 12000.0);
        params.set(hathor::keys::kResonance, 0.707);
        params.set(hathor::keys::kGain, 0.8);
        params.set(hathor::keys::kPan, 1.0);  // hard right
        pool.trigger(params, bank, kRate, kRate, kRate);
    }

    // --- Pass 3: Voice C (very low cutoff 100 Hz, high resonance) ---
    // Trigger at t=2s
    {
        hathor::ParamMap params;
        params.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
        params.set(hathor::keys::kCutoff, 100.0);
        params.set(hathor::keys::kResonance, 5.0);
        params.set(hathor::keys::kGain, 0.8);
        params.set(hathor::keys::kPan, 0.5);  // center
        pool.trigger(params, bank, kRate * 2, kRate * 2, kRate);
    }

    // Render in blocks
    for (int offset = 0; offset < kTotalSamples; offset += kBlockSize) {
        int n = std::min(kBlockSize, kTotalSamples - offset);
        pool.mix(left.data() + offset, right.data() + offset, n, kRate);
    }

    // Write the output
    writeWav("/tmp/b7_k1_ear_test.wav", left, right, kRate);

    // Also generate a single-voice reference for comparison
    VoicePool poolLow, poolHigh;

    // Voice A: low cutoff only
    {
        hathor::ParamMap params;
        params.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
        params.set(hathor::keys::kCutoff, 500.0);
        params.set(hathor::keys::kGain, 1.0);
        poolLow.trigger(params, bank, 0, 0, kRate);
    }

    std::vector<float> leftLow(kRate * 2, 0.0f);
    std::vector<float> rightLow(kRate * 2, 0.0f);
    for (int offset = 0; offset < kRate * 2; offset += kBlockSize) {
        int n = std::min(kBlockSize, kRate * 2 - offset);
        poolLow.mix(leftLow.data() + offset, rightLow.data() + offset, n, kRate);
    }
    writeWav("/tmp/b7_k1_low_cutoff.wav", leftLow, rightLow, kRate);

    // Voice B: high cutoff only
    {
        hathor::ParamMap params;
        params.set(hathor::keys::kS, hathor::Value{std::string{"tone"}});
        params.set(hathor::keys::kCutoff, 12000.0);
        params.set(hathor::keys::kGain, 1.0);
        poolHigh.trigger(params, bank, 0, 0, kRate);
    }

    std::vector<float> leftHigh(kRate * 2, 0.0f);
    std::vector<float> rightHigh(kRate * 2, 0.0f);
    for (int offset = 0; offset < kRate * 2; offset += kBlockSize) {
        int n = std::min(kBlockSize, kRate * 2 - offset);
        poolHigh.mix(leftHigh.data() + offset, rightHigh.data() + offset, n, kRate);
    }
    writeWav("/tmp/b7_k1_high_cutoff.wav", leftHigh, rightHigh, kRate);

    // Compute and print energy comparison
    double eLow = 0, eHigh = 0;
    for (int i = 0; i < kRate * 2; ++i) {
        eLow  += leftLow[i] * leftLow[i] + rightLow[i] * rightLow[i];
        eHigh += leftHigh[i] * leftHigh[i] + rightHigh[i] * rightHigh[i];
    }
    printf("\nEnergy comparison (2 seconds):\n");
    printf("  Low cutoff (500 Hz):   %.4f\n", eLow);
    printf("  High cutoff (12 kHz):  %.4f\n", eHigh);
    printf("  Ratio high/low:        %.4f\n", eHigh / eLow);
    if (eHigh > eLow * 5.0)
        printf("  ✓ High cutoff preserves substantially more energy (per-voice filtering works)\n");
    else
        printf("  ✗ WARNING: energy ratio suggests filtering may not be working correctly\n");

    printf("\nEar-test complete. Listen to:\n");
    printf("  /tmp/b7_k1_ear_test.wav    — 3 voices with different cutoffs (spatialized)\n");
    printf("  /tmp/b7_k1_low_cutoff.wav  — single voice, 500 Hz cutoff (heavily filtered)\n");
    printf("  /tmp/b7_k1_high_cutoff.wav — single voice, 12 kHz cutoff (less filtered)\n");

    return 0;
}
