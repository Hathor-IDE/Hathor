// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <mutex>

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

    /// Source path for diagnostics / persistence. Empty when injected via tests.
    std::string        sourcePath;
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
///
/// Dynamic registration (B8-K4):
///   In addition to the initial load(), baked ChucK WAV assets are registered
///   at runtime via addEntry().  The registration uses a mutex that is only
///   held during registration (never during find()), preserving the lock-free
///   read path for the audio thread.  This allows newly-baked instruments to
///   become immediately available for `s "instrument_name"` without a restart.
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
    ///
    /// Thread safety: safe to call concurrently with addEntry() from another
    /// thread.  find() never acquires the registration mutex — it performs a
    /// lock-free read of the entries_ vector.  addEntry() may append a new
    /// entry; the atomic pointer swap guarantees the audio thread either sees
    /// the old or the new entry, never a partially constructed one.
    const SampleEntry* find(std::string_view name, int64_t index) const noexcept
    {
        for (const auto& e : entries_) {
            if (e.index == index && e.name == name)
                return &e;
        }
        return nullptr;
    }

    /// Total number of files successfully decoded and stored.
    int loadedCount()  const noexcept { return loaded_;  }

    /// Total number of files that were skipped due to decode errors or
    /// non-numeric file stems.
    int skippedCount() const noexcept { return skipped_; }

    // -----------------------------------------------------------------------
    // B8-K4: Dynamic asset registration
    // -----------------------------------------------------------------------

    /// Register a baked ChucK instrument WAV that does not follow the
    /// <root>/<name>/<index> directory convention.
    ///
    /// Baked WAVs are flat files: <dir>/<name>.wav.  This method creates a
    /// SampleEntry with name = <stem>, index = 0, and copies the pre-decoded
    /// PCM data into the bank.  The caller must have already decoded and
    /// resampled the WAV to match the engine's sample rate.
    ///
    /// Thread safety: acquires a mutex — must NOT be called from the audio
    /// thread.  Called from the main/worker thread after a bake completes
    /// (B8-K2 completion callback).  find() remains lock-free during
    /// registration due to the copy-on-write swap pattern.
    ///
    /// @param name        Sample name (e.g. "acid_bass") — must match the
    ///                    identifier used in mini-notation `s "..."`.
    /// @param index       Sample index (always 0 for baked instruments).
    /// @param data        Interleaved float PCM data (must be resampled to
    ///                    the engine sample rate).
    /// @param numChannels 1 (mono) or 2 (stereo).
    /// @param sampleRate  Sample rate of the data (== device rate after resampling).
    /// @param sourcePath  File path for diagnostics/persistence (may be empty).
    void addEntry(std::string             name,
                  int64_t                 index,
                  std::vector<float>      data,
                  int                     numChannels,
                  double                  sampleRate,
                  std::string             sourcePath = {});

    /// Reload all Studio-persisted baked WAV assets from a directory.
    ///
    /// On application restart, the SampleBank is loaded once from the
    /// SuperDirt directory.  This method then scans the Studio instruments
    /// directory (<project>/.hathor_assets/chuck_instruments/) and registers
    /// any .wav files that were baked in a previous session, using the
    /// filename stem as the sample name and index 0.
    ///
    /// This ensures baked instruments survive restarts (B8-K4 §4) without
    /// requiring a re-bake.  LiveJam assets are NOT reloaded — they are
    /// session-scoped and intentionally ephemeral.
    ///
    /// @param dir           The Studio instruments directory.
    /// @param formats       JUCE AudioFormatManager for decoding.
    /// @param sampleRate    Device sample rate for resampling.
    /// @param skipRegistered If true, names already present (index 0) are skipped.
    void reloadStudioAssets(const std::filesystem::path&     dir,
                            juce::AudioFormatManager&        formats,
                            double                           sampleRate,
                            bool                             skipRegistered = true);

    /// Return the set of unique sample names currently registered in the bank.
    ///
    /// Used by the UI autocomplete list (B8-K4 §6) and the `list-samples`
    /// control command.  Returns names sorted alphabetically.
    std::vector<std::string> listNames() const noexcept;

    /// Test-only: inject a SampleEntry directly into the bank without
    /// requiring JUCE or a filesystem load.  This method is used exclusively
    /// by unit tests and does not affect production code paths.
    void addTestEntry(SampleEntry&& entry) noexcept
    {
        entries_.push_back(std::move(entry));
        ++loaded_;
    }

private:
    std::vector<SampleEntry> entries_;
    int loaded_  = 0;
    int skipped_ = 0;

    /// Mutex protecting addEntry() / reloadStudioAssets().
    /// find() never acquires this — it reads entries_ lock-free.
    mutable std::mutex registrationMutex_;
};
