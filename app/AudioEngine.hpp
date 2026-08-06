// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// JUCE
#include <juce_audio_devices/juce_audio_devices.h>

// Engine
#include "hathor/Event.hpp"
#include "hathor/ParamMap.hpp"
#include "hathor/Pattern.hpp"

// App
#include "SampleBank.hpp"
#include "SlotState.hpp"
#include "VoicePool.hpp"

// ---------------------------------------------------------------------------
// SlotState — see app/SlotState.hpp
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// AudioEngine
// ---------------------------------------------------------------------------
//
// Wraps juce::AudioDeviceManager for device management and implements the
// JUCE audio callback (juce::AudioIODeviceCallback).
//
// Also inherits AudioEngineFacade so the control/ layer can use it
// without depending on JUCE headers.
//
// Thread model:
//   Audio thread   — getNextAudioBlock(); no mutex, no heap alloc
//   Worker thread  — stores new SlotState via slots_[i].store(release)
//   Main thread    — initialise(), setBpm(), play(), stop(), etc.
//
// Requirements:
//   7.4, 7.5         — query once per callback; silence when stopped
//   9.1–9.5          — sample-accurate scheduling, sample clock, BPM
//   11.1–11.4        — hot-swap, no mutex in callback
//   13.2             — 16 named slots
//   14.1–14.5        — transport commands, 120 BPM default
//   8.1–8.5          — device management
// ---------------------------------------------------------------------------

#include "AudioEngineFacade.hpp"

class AudioEngine : public AudioEngineFacade, public juce::AudioIODeviceCallback {
public:
    /// Maximum number of named pattern slots (Req 13.2).
    static constexpr int kNumSlots = 16;

    // ------------------------------------------------------------------
    // Construction / destruction
    // ------------------------------------------------------------------

    /// @param bank  Fully-loaded SampleBank (must outlive this object).
    explicit AudioEngine(const SampleBank& bank);
    ~AudioEngine() override;

    // Non-copyable, non-movable (owns atomics and JUCE resources).
    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&)                 = delete;
    AudioEngine& operator=(AudioEngine&&)      = delete;

    // ------------------------------------------------------------------
    // Device management (Req 8.1–8.5)
    // ------------------------------------------------------------------

    /// Open the default system audio output device.
    /// Returns an empty string on success, or an error description on failure.
    /// Reports actual sample rate and buffer size to stderr (Req 8.5).
    [[nodiscard]] std::string initialise();

    // ------------------------------------------------------------------
    // Transport (Req 14.1–14.5)
    // ------------------------------------------------------------------

    /// Start the cycle clock (no-op if already running).
    void play() noexcept override;

    /// Halt the cycle clock and immediately silence all voices.
    void stop() noexcept override;

    /// Set the tempo. Clamped to [20, 400] BPM (Req 14.3, 14.4).
    /// The new value takes effect at the start of the next audio callback (Req 9.5).
    void setBpm(double bpm) noexcept override;

    /// Returns the current BPM.
    double getBpm() const noexcept override;

    /// Returns true if the transport is currently running.
    bool isRunning() const noexcept override;

    // ------------------------------------------------------------------
    // Hot-swap slot API (called from WorkerThread — Req 11.1–11.5, 13.1–13.4)
    // ------------------------------------------------------------------

    /// Map a slot name to a 0-based index in slots_[].
    /// Returns -1 if the name does not correspond to a registered slot.
    /// Slots are auto-registered on first use (up to kNumSlots).
    int findOrAddSlot(const std::string& name) override;

    /// Store a new SlotState into slot index @p idx (worker thread, release order).
    void storeSlot(int idx, std::shared_ptr<SlotState> state) noexcept override;

    /// Clear a slot (worker thread). Returns false if idx is out of range.
    bool clearSlot(int idx) noexcept override;

    /// Returns the number of registered slot names (for list-patterns).
    int slotCount() const noexcept override;

    /// Returns the slot name for a given index (empty string if unregistered).
    std::string slotName(int idx) const override;

    /// Loads a slot's current SlotState (acquire order). May return nullptr.
    std::shared_ptr<SlotState> loadSlot(int idx) const noexcept override;

    // ------------------------------------------------------------------
    // juce::AudioIODeviceCallback interface
    // ------------------------------------------------------------------
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(const float* const*                inputChannelData,
                                         int                                numInputChannels,
                                         float* const*                      outputChannelData,
                                         int                                numOutputChannels,
                                         int                                numSamples,
                                         const juce::AudioIODeviceCallbackContext& context) override;

private:
    // ------------------------------------------------------------------
    // Subsystem references
    // ------------------------------------------------------------------
    const SampleBank& bank_;     ///< immutable after SampleBank::load()
    VoicePool         voicePool_;

    // ------------------------------------------------------------------
    // Transport state (Req 9.4, 14.5)
    // ------------------------------------------------------------------
    std::atomic<uint64_t> sampleClock_{0};   ///< incremented by bufferSize each callback
    std::atomic<double>   bpm_{120.0};        ///< current tempo
    std::atomic<bool>     running_{true};     ///< transport running flag
    std::atomic<int>      sampleRate_{44100}; ///< set when device opens

    // ------------------------------------------------------------------
    // Hot-swap slots (Req 11.1–11.4, 13.2)
    // ------------------------------------------------------------------
    // std::atomic<std::shared_ptr<T>> is not supported on Apple Clang's libc++
    // (no C++20 specialization in the SDK's headers). We use the C++11/14
    // free-function API (std::atomic_store / std::atomic_load) on plain
    // shared_ptr objects instead, which provides the same acquire/release
    // semantics via an internal lock.
    //
    // Worker writes with std::atomic_store_explicit(..., release);
    // Audio thread reads with std::atomic_load_explicit(..., acquire).
    std::shared_ptr<SlotState> slots_[kNumSlots];

    // Slot name registry — written only on the main/worker thread.
    // Protected by the same "only modified before slot is used" convention.
    // Access pattern: main thread writes via findOrAddSlot(); audio thread
    // never reads slotNames_ (it iterates by index over slots_[]).
    std::string  slotNames_[kNumSlots];
    int          slotNameCount_{0};

    // ------------------------------------------------------------------
    // JUCE device manager
    // ------------------------------------------------------------------
    juce::AudioDeviceManager deviceManager_;
};
