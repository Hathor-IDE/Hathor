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
#include <juce_audio_formats/juce_audio_formats.h>

// Engine
#include "hathor/Event.hpp"
#include "hathor/ParamMap.hpp"
#include "hathor/Pattern.hpp"

// App
#include "SampleBank.hpp"
#include "SlotState.hpp"
#include "VoicePool.hpp"
#include "VisualizerFrame.hpp"
#include "audio-worker/AudioWorkerManager.hpp"
#include "MasterEq.hpp"
#include "AssetTarget.hpp"
#include "AssetPathResolver.hpp"
#include "LiveJamSessionManager.hpp"

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
    // B4-K3: Worker process management
    // ------------------------------------------------------------------

    /// Set the path to the hathor-audio-worker executable and start the
    /// worker process.  Must be called before slotPlay() to enable per-tab
    /// VM activation.  Safe to call from the main thread only.
    /// Returns an error string on failure (empty = success).
    [[nodiscard]] std::string startWorker(const std::string& workerPath);

    /// Shut down the worker process (if running).  Called automatically
    /// from the destructor.
    void shutdownWorker() noexcept;

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

     // --- Per-slot play/stop (A3) ---
    // These manipulate the per-slot running bit on SlotState, independent of
    // the global transport.  slotStop() also silences voices currently being
    // driven by that slot.
    //
    // B4-K3: slotPlay() also activates the per-tab ChucK VM (if a worker is
    // running).  slotStop() suspends the VM (keeps state for fast resume).
    void slotPlay(int slotIdx) noexcept override;
    void slotStop(int slotIdx) noexcept override;
    bool isSlotRunning(int slotIdx) const noexcept override;

    // ------------------------------------------------------------------
    // Master gain (Req 26.5, 26.6, 26.7, 26.8)
    // ------------------------------------------------------------------

    /// Set the master output gain. Clamped to [0.0, 2.0] (relaxed ordering).
    void setMasterGain(float g) noexcept override;

    /// Get the current master output gain (relaxed ordering).
    float getMasterGain() const noexcept override;

    // ------------------------------------------------------------------
    // B7-K2: Master-bus preset EQ
    // ------------------------------------------------------------------
    //
    // Four fixed presets (Flat / Bass Boost / Vocal / Bright) applied at the
    // master mix stage, AFTER per-voice filtering and ChucK audio, but BEFORE
    // final master gain (decision #13):
    //
    //   Master EQ → Final Master Gain → Output
    //
    // Preset selection is called from the worker/control thread (or command
    // handler).  Coefficients are computed there and published to the audio
    // thread via the same std::shared_ptr + atomic_store/load pattern used for
    // SlotState.  No mutex, no allocation in the audio callback.
    //
    // Requirement references: B7-K2 §3, §4, §5

    /// Select the master-bus EQ preset.
    /// Called on the worker/control thread.  Computes the complete replacement
    /// filter state and publishes it atomically.
    void setMasterEqPreset(hathor::EqPreset preset) noexcept override;

    /// Returns the currently active EQ preset.
    hathor::EqPreset getMasterEqPreset() const noexcept override;

    /// Load the current MasterEqState (acquire ordering).
    /// May return nullptr during early startup (before the first preset is set).
    std::shared_ptr<hathor::MasterEqState> loadEqState() const noexcept;

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
    // B4-K7: Per-tab ChucK VM evaluation
    // ------------------------------------------------------------------

    /// Check if the audio worker process is running.
    bool hasWorker() const noexcept override;

    /// Evaluate ChucK source code for a tab (compile→load→execute path).
    bool ckEval(int slotIdx, const std::string& code) noexcept override;

    /// Stop a .ck tab: destroy the per-tab VM and clear pending handoff.
    bool stopCkTab(int slotIdx) noexcept override;

    /// Query the .ck VM state for a tab slot.
    std::string queryCkTab(int slotIdx) const override;

    // ------------------------------------------------------------------
    // B8-K1: Bake-to-Song render target (Studio vs Live Jam)
    // ------------------------------------------------------------------
    //
    // B8-K1 owns target selection, representation, path resolution, and
    // lifetime semantics.  B8-K2 receives the resolved path and renders
    // PCM data into it.
    //
    // API surface:
    //   resolveRenderPath()  — turn (target, name, projectDir) into a .wav path
    //   setLiveJamSessionDir() — register the session temp dir for LiveJam
    //   cleanupLiveJamAssets() — remove LiveJam temp files at session end
    //   isStudioAssetPath()  — verify a path is in the permanent asset area

    /// Resolve the render destination path for a bake operation (B8-K1).
    /// @param target      Studio (default) or LiveJam.
    /// @param name        Instrument name (sanitised).
    /// @param projectDir  Current Hathor project directory.
    /// @return Absolute .wav path, or empty on error.
    std::filesystem::path resolveRenderPath(AssetTarget target,
                                            std::string_view name,
                                            const std::filesystem::path& projectDir) override;

    /// Register the LiveJam session directory (created by LiveJamSessionManager).
    /// Required before resolveRenderPath(LiveJam, ...) will succeed.
    void setLiveJamSessionDir(std::filesystem::path dir) override;

    /// Set the current project directory (the application's working directory).
    /// Called once at startup so AssetPathResolver is initialised.
    void setProjectDir(std::filesystem::path dir) override;

    /// Clean up all LiveJam assets for the current session (session end hook).
    /// Removes only LiveJam temp files — NEVER Studio assets.
    void cleanupLiveJamAssets() override;

    /// Returns true if @p path is inside the Studio permanent asset area.
    bool isStudioAssetPath(const std::filesystem::path& path) const override;

    // ------------------------------------------------------------------
    // B8-K2: Background ChucK render → .wav
    // ------------------------------------------------------------------
    //
    // Delegates to a ChuckRenderWriter that drains audio from the worker's
    // shared-memory ring on a dedicated background thread.  The JUCE message
    // thread is never blocked.
    //
    // The destination path is supplied by B8-K1 (resolveRenderPath above).
    // The renderer never duplicates target-selection/path-resolution logic.

     /// Start a background render.  Returns immediately with a handle.
     hathor::RenderHandle startBakeRender(uint8_t                            tabId,
                                          std::string                        ckSource,
                                          uint64_t                           numSamples,
                                          unsigned                           sampleRate,
                                          const std::filesystem::path&       destPath,
                                          hathor::ChuckRenderWriter::CompletionCallback onComplete) override;

     /// Start a background render without auto-registering in SampleBank (AI-6).
     hathor::RenderHandle startBakeRenderRaw(uint8_t                            tabId,
                                             std::string                        ckSource,
                                             uint64_t                           numSamples,
                                             unsigned                           sampleRate,
                                             const std::filesystem::path&       destPath,
                                             hathor::ChuckRenderWriter::CompletionCallback onComplete) override;

    /// Number of renders currently in progress.
    int activeRenderCount() const noexcept override;

    /// Shut down all in-flight renders (called from AudioEngine destructor).
    void shutdownRender() noexcept override;

    // ------------------------------------------------------------------
    // B8-K4: SampleBank registration after bake
    // ------------------------------------------------------------------

    /// Register a baked WAV asset in the SampleBank (B8-K4 §2, §3).
    /// Called from the B8-K2 completion callback after successful WAV
    /// publication.  Decodes + resamples the WAV and calls SampleBank::addEntry().
    /// Thread-safe (acquires SampleBank registration mutex) — may be called
    /// from the render thread.
    bool registerBakedAsset(std::string name,
                            const std::filesystem::path& wavPath) override;

    /// Return all sample names registered in the SampleBank (B8-K4 §6).
    std::vector<std::string> listSamples() const override;

    // ------------------------------------------------------------------
    // AI-2: Read-only introspection (overrides on AudioEngineFacade)
    // ------------------------------------------------------------------

    /// Read-only slot inventory.  Source of truth: slots_[] + slotNames_[].
    std::vector<SlotInfo> listSlots() const noexcept override;

    /// Read-only status of a single slot.
    SlotInfo getSlotInfo(int slotIndex) const noexcept override;

    /// Read-only ChucK VM status for a tab (B4-K3).
    VmStatus getVmStatus(int slotIndex) const noexcept override;

    /// Read-only audio transport/engine state snapshot.
    AudioStatus getAudioStatus() const noexcept override;

    /// L-6: Number of currently-playing voices (≤ 32). RT-safe.
    int activeVoiceCount() const noexcept override;

    /// L-6: Copy active-voice snapshots into @p out. May allocate.
    void activeVoices(std::vector<VoiceInfo>& out) const override;

    /// Read-only per-slot playback status.
    std::vector<SlotPlayback> listSlotPlayback() const noexcept override;

    /// Read-only ChucK instrument inventory (B8-K1/K2/K3/K4).
    std::vector<InstrumentInfo> listChuckInstruments(
        const std::filesystem::path& projectDir) const noexcept override;

    /// Resolve the Studio instruments directory for a project.
    std::filesystem::path studioInstrumentsDir(
        const std::filesystem::path& projectDir) const noexcept override;

    /// The project directory last used for render-path resolution.
    std::filesystem::path currentProjectDir() const noexcept override;

    // ------------------------------------------------------------------
    // AI-2: Read-only introspection helpers
    // ------------------------------------------------------------------

    /// Compute the duration of a WAV file in seconds (read-only).
    /// Uses JUCE's AudioFormatManager — must be called from the main thread,
    /// not the audio callback.  Returns 0.0 on any error.
    static double wavDurationSeconds(const std::filesystem::path& wavPath) noexcept;

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

    // ------------------------------------------------------------------
    // File capture (--capture-to-file <path>.wav)
    // ------------------------------------------------------------------

    /// Open a WAV file for capturing audio output.
    /// Must be called BEFORE initialise() so the device rate is known, or after
    /// initialise() with the actual sample rate. Returns an error string on failure.
    /// Once open, every mixed buffer is written to the file in addition to
    /// the live device output.
    [[nodiscard]] std::string openCapture(const std::string& path);

    /// Flush and close the capture file. Called from main thread on shutdown.
    void closeCapture();

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

    // B4-K3: Worker process generation — used by tryReadAudioBlock to reject
    // stale shared-memory samples when the worker has been restarted.
    std::atomic<uint64_t> workerGeneration_{0};

    // ------------------------------------------------------------------
    // Master gain (Req 26.5, 26.6)
    // ------------------------------------------------------------------
    /// Linear amplitude multiplier applied to the mixed output before capture.
    /// Range [0.0, 2.0]; relaxed ordering — continuous fader, no sync dependency.
    std::atomic<float>    masterGain_{1.0f};

    // ------------------------------------------------------------------
    // Visualizer ring buffer (Req 28.1, 28.3, 28.4, 28.5, 28.8)
    // ------------------------------------------------------------------
    /// Pre-allocated SPSC ring buffer; written by audio thread, read by UITimer.
    hathor::SpscRingBuffer<128>   vizRingBuffer_;

public:
    /// Accessor for the UI timer to drain visualizer frames (non-const ref).
    hathor::SpscRingBuffer<128>& visualizerBuffer() noexcept { return vizRingBuffer_; }

private:

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

    // ------------------------------------------------------------------
    // File capture (optional, --capture-to-file)
    // ------------------------------------------------------------------
    // Opened/closed on the main thread (before device starts / after device stops).
    // Written from the audio thread after the mix step.
    // No locking needed: lifecycle is strictly main-thread-controlled.
    std::unique_ptr<juce::AudioFormatWriter> captureWriter_;
    std::atomic<bool>                        captureOpen_{false};

    // ------------------------------------------------------------------
    // B4-K3: Per-tab ChucK VM worker (out-of-process)
    // ------------------------------------------------------------------
    // Manages the hathor-audio-worker companion process and per-tab VM
    // lifecycle. nullptr when no worker is configured (e.g. in tests
    // without a worker path). slotPlay/slotStop delegate VM activation
    // to this manager.
    std::unique_ptr<hathor::AudioWorkerManager> workerMgr_;

    // ------------------------------------------------------------------
    // B7-K2: Master-bus preset EQ state
    // ------------------------------------------------------------------
    //
    // The active EQ state is an immutable shared_ptr<MasterEqState>.
    // The control/worker thread publishes a COMPLETE replacement via
    // std::atomic_store_explicit(release); the audio thread consumes it via
    // std::atomic_load_explicit(acquire).  This is the same pattern used for
    // SlotState above (Apple-Clang-compatible).
    //
    // The audio thread never mutates the COEFFICIENTS — only the delay-line
    // state inside the loaded MasterEqState advances per sample.  When a new
    // preset is selected, a fresh MasterEqState (zeroed delays) is published;
    // the audio thread transitions to it on the next callback.
    //
    // Requirement references: B7-K2 §4, §5, §6
    std::shared_ptr<hathor::MasterEqState> activeEqState_;
    /// Current preset (atomic for quick introspection without loading the SP).
    std::atomic<int> eqPreset_{static_cast<int>(hathor::EqPreset::Flat)};

    // ------------------------------------------------------------------
    // B8-K1: Asset target plumbing (Studio vs Live Jam)
    // ------------------------------------------------------------------
    // B8-K1 owns target selection, representation, path resolution, and
    // lifetime semantics.  These members provide the concrete implementation
    // of the AudioEngineFacade bake API.
    //
    // The AssetPathResolver is stateless given a projectDir and is safe to
    // call from any thread.  The LiveJamSessionManager owns the session temp
    // directory and is initialised once per Hathor session (at startup).
    hathor::AssetPathResolver     resolver_;
    hathor::LiveJamSessionManager liveJamSession_;

    // B8-K2: render writer — created when the worker is started.
    // nullptr when no worker process is configured (tests, etc.).
    std::unique_ptr<hathor::ChuckRenderWriter> renderWriter_;

     // Storage for a caller-provided LiveJam session directory (when
    // setLiveJamSessionDir is called with a non-empty path).
    std::filesystem::path liveJamSessionDirStorage_;

    // AI-5: Async ChucK compilation stubs (not yet wired up)
    uint64_t startAsyncCkCompile(int /*slotIdx*/,
                                 const std::string& /*code*/,
                                 std::function<void(bool, const std::string&)> /*onComplete*/) override
    {
        return 0;
    }
    nlohmann::json queryCkJob(uint64_t /*jobId*/) const override
    {
        return nlohmann::json::object();
    }
    bool cancelCkJob(uint64_t /*jobId*/) override
    {
        return false;
    }
};
