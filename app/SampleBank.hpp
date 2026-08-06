// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

// Forward-declare JUCE types to keep this header JUCE-agnostic where possible.
// The .cpp includes the full JUCE headers.
namespace juce { class AudioFormatManager; }

/// A single decoded audio sample, stored as interleaved float PCM.
///
/// Interleaving layout:
///   mono   – [S0, S1, S2, …]
///   stereo – [L0, R0, L1, R1, …]
///
/// `data` is populated once by SampleBank::load() and never mutated afterwards,
/// making it safe to read from the audio thread without synchronisation.
struct SampleEntry {
    std::string        name;        ///< Folder name, e.g. "bd"
    int64_t            index;       ///< File index, e.g. 0
    std::vector<float> data;        ///< Interleaved samples (mono or stereo)
    int                numChannels; ///< 1 (mono) or 2 (stereo)
    double             sampleRate;  ///< Device sample rate – already resampled at load time
};

/// Loads a SuperDirt-style sample bank from disk and provides lock-free lookup.
///
/// Directory convention:
///   <root>/<name>/<index>.{wav,aiff,flac}
///
/// Lifecycle:
///   1. Call load() once on the main thread before AudioDeviceManager is opened.
///   2. After load() returns the bank is permanently read-only.
///   3. find() may be called from the audio thread at any time after load().
class SampleBank {
public:
    /// Load all audio files under `root`.
    ///
    /// - Registers WAV/AIFF/FLAC formats on `formats` via registerBasicFormats().
    /// - Resamples every file to `deviceSampleRate` at load time so the audio
    ///   callback never needs to correct for a device/file rate mismatch.
    /// - Files that cannot be decoded are logged to stderr and skipped; loading
    ///   continues. After load() returns, loadedCount() + skippedCount() equals
    ///   the total number of candidate files encountered.
    void load(const std::filesystem::path& root,
              juce::AudioFormatManager&    formats,
              double                       deviceSampleRate);

    /// Look up a sample by (name, index).
    ///
    /// Returns nullptr if not found. Linear scan over entries_ – O(N) but
    /// N is small and the operation is allocation-free, so it is safe to
    /// call from the audio thread.
    const SampleEntry* find(std::string_view name, int64_t index) const noexcept;

    /// Total number of files successfully decoded and stored.
    int loadedCount()  const noexcept;

    /// Total number of files that were skipped due to decode errors or
    /// non-numeric file stems.
    int skippedCount() const noexcept;

private:
    std::vector<SampleEntry> entries_;
    int loaded_  = 0;
    int skipped_ = 0;
};
