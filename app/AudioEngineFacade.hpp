// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * AudioEngineFacade.hpp — a JUCE-free interface exposing the AudioEngine
 * methods that the control/ layer needs.
 *
 * control/ translation units include this header instead of AudioEngine.hpp
 * so they are not dragged into the JUCE module compilation graph.
 *
 * AudioEngine (defined in app/AudioEngine.hpp) inherits from this class
 * so that the same object can be passed through both interfaces.
 *
 * Requirements: 11.1–11.5, 13.1–13.4, 14.1–14.6, 15.1–15.3
 */

#include "SlotState.hpp"
#include "MasterEq.hpp"
#include "AssetTarget.hpp"
#include "ChuckRenderWriter.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

// Bring AssetTarget into the global namespace so AudioEngineFacade's method
// signatures stay unqualified (matching the existing SlotState/EqPreset pattern).
using hathor::AssetTarget;

/**
 * Abstract facade for the methods AudioEngine exposes to the control layer.
 *
 * Using an abstract base class keeps control/ completely free of JUCE headers
 * while still providing virtual-dispatch access to the real AudioEngine at
 * runtime.
 */
class AudioEngineFacade {
public:
    virtual ~AudioEngineFacade() = default;

     // --- Transport ---
    virtual void   play()    noexcept = 0;
    virtual void   stop()    noexcept = 0;
    virtual void   setBpm(double bpm) noexcept = 0;
    virtual double getBpm()  const noexcept = 0;
    virtual bool   isRunning() const noexcept = 0;

    // --- Per-slot play/stop (A3) ---
    // slotPlay(slotIdx) — resume one slot independently (others unchanged).
    // slotStop(slotIdx) — stop one slot independently (others continue).
    virtual void slotPlay(int slotIdx) noexcept = 0;
    virtual void slotStop(int slotIdx) noexcept = 0;
    virtual bool isSlotRunning(int slotIdx) const noexcept = 0;

    // --- Master gain (Req 26.5, 26.6) ---

    /// Set the master output gain (clamped to [0.0, 2.0], relaxed ordering).
    virtual void  setMasterGain(float g) noexcept = 0;

    /// Get the current master output gain (relaxed ordering).
    virtual float getMasterGain() const noexcept = 0;

    // --- B7-K2: Master-bus preset EQ ---

    /// Select the master-bus EQ preset (worker/control thread).
    /// Publishes a complete replacement filter state atomically.
    virtual void setMasterEqPreset(hathor::EqPreset preset) noexcept = 0;

    /// Returns the currently active EQ preset.
    virtual hathor::EqPreset getMasterEqPreset() const noexcept = 0;

    // --- Hot-swap slot API ---

    /// Map a slot name to a 0-based index. Returns -1 if not found and the
    /// table is full (16 slots already registered).
    virtual int findOrAddSlot(const std::string& name) = 0;

    /// Store a new SlotState into slot @p idx (release ordering).
    virtual void storeSlot(int idx, std::shared_ptr<SlotState> state) noexcept = 0;

    /// Clear slot @p idx. Returns false if idx is out of range.
    virtual bool clearSlot(int idx) noexcept = 0;

    /// Number of registered slot names.
    virtual int slotCount() const noexcept = 0;

    /// Name of slot @p idx (empty string if unregistered).
    virtual std::string slotName(int idx) const = 0;

    /// Load the current SlotState for slot @p idx (acquire ordering).
    virtual std::shared_ptr<SlotState> loadSlot(int idx) const noexcept = 0;

    // --- B4-K7: Per-tab ChucK VM evaluation ---

    /// Check if the audio worker process is running.
    virtual bool hasWorker() const noexcept = 0;

    /// Evaluate ChucK source code for a tab (compile→load→execute path).
    /// This is the .ck equivalent of set-pattern for mini-notation.
    /// @param slotIdx  Pattern slot index (maps to tab slot).
    /// @param code     ChucK source code.
    /// @return true on successful compile+publish; false on error.
    virtual bool ckEval(int slotIdx, const std::string& code) noexcept = 0;

    /// Stop a .ck tab: destroy the per-tab VM and clear any pending handoff.
    /// The previously running shred is not affected (the VM is destroyed).
    /// @param slotIdx  Pattern slot index.
    /// @return true on success; false if no VM was running.
    virtual bool stopCkTab(int slotIdx) noexcept = 0;

    /// Query the .ck VM state for a tab slot.  Returns a human-readable
    /// status string (e.g. "active shred_id=5 source_hash=0x1234").
    /// @param slotIdx  Pattern slot index.
    /// @return Status string (empty if no worker or VM).
    virtual std::string queryCkTab(int slotIdx) const = 0;

    /// Async ChucK compilation (AI-5).  Compiles .ck source on the B4-K4
    /// dispatcher thread and notifies completion via callback.  Returns
    /// immediately with a job ID for status polling.
    /// @param slotIdx     Pattern slot index (maps to tab slot).
    /// @param code        ChucK source code.
    /// @param onComplete  Called on a background thread when compilation finishes.
    /// @return A job ID for status polling.
    virtual uint64_t startAsyncCkCompile(int slotIdx,
                                         const std::string& code,
                                         std::function<void(bool /*success*/,
                                                            const std::string& /*response*/)> onComplete) = 0;

    /// Query async job status (AI-5).
    /// @param jobId  Job ID returned by startAsyncCkCompile().
    /// @return JSON with {ok, job_id, status, result}.
    virtual nlohmann::json queryCkJob(uint64_t jobId) const = 0;

    /// Cancel an async ChucK compilation job (AI-5).
    /// @param jobId  Job ID returned by startAsyncCkCompile().
    /// @return true if cancellation was accepted, false if already complete.
    virtual bool cancelCkJob(uint64_t jobId) = 0;

    // --- B8-K1: Bake-to-Song render target (Studio vs Live Jam) ---
    //
    // The bake pipeline receives the selected AssetTarget explicitly.
    // B8-K1 owns target selection, representation, and path resolution;
    // B8-K2 owns rendering the ChucK instrument and writing PCM data.
    //
    // resolveRenderPath() turns (target, name, projectDir) into a concrete
    // .wav path.  The caller (B8-K2 renderer) then writes audio to that path.
    // Studio is ALWAYS the default; LiveJam is an explicit opt-in.

    /// Resolve the render destination path for a bake operation (B8-K1 §5).
    ///
    /// @param target      Studio (default) or LiveJam.
    /// @param name        Instrument name (sanitised — see sanitizeAssetName).
    /// @param projectDir  Current Hathor project directory.
    /// @return Absolute path to the .wav destination.  Empty string on error.
    ///
    /// For Studio:  <projectDir>/.hathor_assets/chuck_instruments/<name>.wav
    /// For LiveJam: <session-temp>/hathor_live_jam_<pid>_<seq>/<name>.wav
    ///
    /// The parent directory is created if it does not exist.
    /// Path construction is centralised here — never duplicated in the renderer.
    virtual std::filesystem::path resolveRenderPath(AssetTarget target,
                                                     std::string_view name,
                                                     const std::filesystem::path& projectDir) = 0;

    /// Set the LiveJam session directory (for the current Hathor session).
    /// Called once at session startup; required before resolveRenderPath(LiveJam, ...).
    virtual void setLiveJamSessionDir(std::filesystem::path dir) = 0;

    /// Set the current project directory (the application's working directory).
    /// Called once at application startup so that currentProjectDir(),
    /// studioInstrumentsDir(), and listChuckInstruments() all resolve
    /// against the correct project root.
    /// This is the canonical initialization point for AssetPathResolver.
    virtual void setProjectDir(std::filesystem::path dir) = 0;

    /// Returns the current project directory.
    /// Source of truth: AssetPathResolver::projectDir().
    /// Guaranteed non-empty after setProjectDir() has been called.
    virtual std::filesystem::path currentProjectDir() const noexcept = 0;

    /// Trigger cleanup of all LiveJam assets for the current session.
    /// Called from application shutdown (HathorApplication::shutdown).
    /// Removes only LiveJam temp files — NEVER Studio assets.
    virtual void cleanupLiveJamAssets() = 0;

    /// Returns true if the given path is inside the Studio permanent asset area.
    virtual bool isStudioAssetPath(const std::filesystem::path& path) const = 0;

    // --- B8-K2: Background ChucK render → .wav ---

    /// Start a background render of the active ChucK instrument.
    ///
    /// The destination path must have been resolved by B8-K1 (resolveRenderPath).
    /// Rendering occurs entirely on a background thread — the JUCE message thread
    /// is not blocked.  Completion (or failure) is reported via onComplete.
    ///
    /// @param tabId       Tab/slot index [0,16) for the render VM.
    /// @param ckSource    ChucK source code to render.
    /// @param numSamples  Number of samples to render at @p sampleRate.
    /// @param sampleRate  Sample rate (typically 44100).
    /// @param destPath    Resolved .wav destination (from B8-K1).
    /// @param onComplete  Async completion callback (called on the render thread).
     /// @return A RenderHandle for status polling / cancellation.
     virtual hathor::RenderHandle startBakeRender(uint8_t                            tabId,
                                                  std::string                        ckSource,
                                                  uint64_t                           numSamples,
                                                  unsigned                           sampleRate,
                                                  const std::filesystem::path&       destPath,
                                                  hathor::ChuckRenderWriter::CompletionCallback onComplete) = 0;

     /// Start a background render WITHOUT auto-registering the result in the
     /// SampleBank.  Used by AI-6 (render_chuck) which separates the render
     /// phase from the commit phase — registration happens explicitly at
     /// commit time, not when the render completes.
     ///
     /// @return A RenderHandle for status polling / cancellation.
     virtual hathor::RenderHandle startBakeRenderRaw(uint8_t                            tabId,
                                                     std::string                        ckSource,
                                                     uint64_t                           numSamples,
                                                     unsigned                           sampleRate,
                                                     const std::filesystem::path&       destPath,
                                                     hathor::ChuckRenderWriter::CompletionCallback onComplete) = 0;

    /// Number of renders currently in progress.
    virtual int activeRenderCount() const noexcept = 0;

    /// Shut down all in-flight renders (called from application shutdown).
    virtual void shutdownRender() noexcept = 0;

    // --- B8-K4: SampleBank registration after bake ---

    /// Register a baked WAV asset in the SampleBank so that `s "name"` resolves
    /// through the normal sample playback path.
    ///
    /// Called from the B8-K2 completion callback (on the render thread) after
    /// a WAV has been successfully published and validated.  The AudioEngine
    /// handles resampling to the device sample rate and SampleBank registration
    /// on the caller's behalf.
    ///
    /// @param name        Sample name (filename stem, e.g. "acid_bass").
    /// @param wavPath     Absolute path to the just-written .wav file.
    /// @return true on successful registration; false on decode/error.
    virtual bool registerBakedAsset(std::string name,
                                    const std::filesystem::path& wavPath) = 0;

    /// Return the list of all sample names currently registered in the SampleBank.
    /// Used for editor autocomplete (B8-K4 §6) and the `list-samples` command.
    virtual std::vector<std::string> listSamples() const = 0;

    // --- AI-2: Read-only project introspection (Phase 2.5 H0) ---
    //
    // These methods provide semantic, read-only access to the application's
    // current state.  They route through the real subsystems (SlotState,
    // AudioWorkerManager, AssetPathResolver, SampleBank) without bypassing
    // the facade layer.  None of these methods may mutate persistent state.
    //
    // Return types use plain structs (no JUCE) so control/ can serialize them
    // to JSON without depending on the JUCE module graph.

    /// Information about one registered pattern slot, returned by listSlots().
    struct SlotInfo {
        int         slotIndex;        ///< 0-based slot index [0, 16)
        std::string slotName;         ///< e.g. "d0", "d1" (empty if unregistered)
        bool        active;           ///< true if a SlotState is stored for this slot
        bool        running;          ///< true if the slot's running flag is set (A3)
        std::string notation;         ///< canonical mini-notation string (empty if no state)
        int         eventCount;       ///< max events per cycle for this slot's pattern
    };

    /// Read-only inventory of every registered slot.
    /// Source of truth: AudioEngine::slots_[] + slotNames_[] + slotNameCount_.
    virtual std::vector<SlotInfo> listSlots() const noexcept = 0;

    /// Detailed read-only status of a single slot (by index).
    /// Returns a SlotInfo with active=false if the slot is unregistered.
    virtual SlotInfo getSlotInfo(int slotIndex) const noexcept = 0;

    /// Per-tab ChucK VM introspection snapshot (B4-K3).
    /// Source of truth: AudioWorkerManager + VMManager::queryVM().
    struct VmStatus {
        bool         hasWorker;      ///< true if the worker process is alive
        std::string  state;          ///< "inactive" | "active" | "suspended" |
                                     ///  "destroyed" | "error" | "failed" | "recreating"
        std::string  shredInfo;      ///< e.g. "shred_id=5 source_hash=0x1234" (from queryTabVM)
        uint64_t     generation;     ///< VM generation counter (0 if none)
        std::string  lastError;      ///< last error message (empty if none)
    };

    /// Query the ChucK VM status for a tab/slot.
    /// Does NOT start or stop VMs — pure read path through the control plane.
    virtual VmStatus getVmStatus(int slotIndex) const noexcept = 0;

    /// Overall audio runtime state snapshot.
    /// Source of truth: AudioEngine atomics (running_, bpm_, sampleRate_).
    struct AudioStatus {
        bool         running;       ///< transport running flag
        double       bpm;           ///< current tempo [20, 400]
        int          sampleRate;    ///< device sample rate (Hz, 0 if not opened)
        float        masterGain;    ///< current master gain [0, 2]
        std::string  eqPreset;      ///< current EQ preset name ("flat", "bass-boost", "vocal", "bright")
        uint64_t     sampleClock;   ///< current sample clock value (monotonic)
        bool         deviceOpen;    ///< true if the audio device is open
        int          activeRenders; ///< number of in-flight bake renders
    };

    /// Snapshot of the current audio transport / engine state.
    virtual AudioStatus getAudioStatus() const noexcept = 0;

    /// Per-slot playback status.
    struct SlotPlayback {
        int         slotIndex;
        std::string slotName;
        bool        running;       ///< per-slot running flag (A3)
        bool        hasPattern;    ///< true if a SlotState is stored
        std::string notation;      ///< canonical mini-notation (empty if no pattern)
    };

    /// Per-slot playback status for all registered slots.
    virtual std::vector<SlotPlayback> listSlotPlayback() const noexcept = 0;

    /// Instrument asset lifecycle information (B8-K1/K2/K3/K4).
    struct InstrumentInfo {
        std::string name;         ///< instrument/sample name (sanitised)
        bool        sourceCkExists;    ///< true if the .ck source file exists
        bool        renderedWavExists; ///< true if the baked .wav exists in Studio assets
        bool        boundToSampleBank; ///< true if registered in SampleBank (addEntry)
        std::string sourcePath;   ///< path to the .ck source (empty if not found)
        std::string renderedPath; ///< path to the .wav (empty if not found)
        double      durationSeconds; ///< WAV duration (0.0 if not available)
    };

    /// Read-only inventory of ChucK instruments and their asset lifecycle state.
    /// Source of truth: AssetPathResolver (Studio .ck + .wav paths) + SampleBank.
    /// Does NOT compile or render — pure filesystem + SampleBank inspection.
    virtual std::vector<InstrumentInfo> listChuckInstruments(
        const std::filesystem::path& projectDir) const noexcept = 0;

    /// Resolve the Studio instruments directory for a project.
    /// Source of truth: AssetPathResolver::studioInstrumentsDir().
    virtual std::filesystem::path studioInstrumentsDir(
        const std::filesystem::path& projectDir) const noexcept = 0;

    /// The project directory initialized at application startup.
    /// Source of truth: AssetPathResolver::projectDir().
    /// Guaranteed non-empty after setProjectDir() has been called.
    /// (Replaces the earlier pure-virtual at line 314.)
};
