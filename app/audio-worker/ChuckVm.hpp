// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ChuckVM.hpp — per-tab ChucK VM lifecycle with bounded resource policy (B4-K3).
 *
 * Each active .ck tab owns one ChuckVM instance, one dedicated OS thread,
 * and one watchdog attachment point.  A failure in one VM never silences
 * another.
 *
 * Lifecycle state machine:
 *
 *   Open (no VM)
 *     │  activate (tab playing / eval'd)
 *     ▼
 *   Live ──► suspended ──► live  (suspend: deterministic pause, state retained)
 *     │ deactivate (policy: suspend or destroy)
 *     ▼
 *   destroyed (VM torn down; metadata retained for re-activation)
 *
 * K0.5 conformance: compileCode() MUST be called with immediate=FALSE so
 * shreds are queued via libchuck's lock-free FinalRingBuffer and sporked
 * on the VM thread.  compileCode() is never called concurrently with
 * run() on the same VM by another thread.
 *
 * Requirements: B4-K3, B4-K0.5, Decision #24
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_ipc.h"

namespace hathor::audio_worker {

/**
 * ChuckVM — wraps a single ChucK VM instance with its dedicated OS thread.
 *
     * Thread model:
     *   - The ChucK thread is created/destroyed by activate()/deactivate(),
     *     NEVER by the audio callback thread.
     *   - compileCode() must use immediate=FALSE (deferred spork) per K0.5.
     *   - The heartbeat counter is atomic and incremented once per render
     *     block for watchdog consumption (B4-K5).
     *   - B4-K7: The render thread consumes handoff shreds via an injected
     *     HandoffLoader callback, keeping the VM decoupled from VmLifecycle.
     */
class ChuckVM {
class ChuckVM {
public:
    /// Audio render callback signature.
    /// Produces numFrames of audio into outBuf (numFrames * numChannels).
    /// Called on the ChucK thread only; must be real-time safe within the VM.
    using RenderCallback = std::function<void(float* outBuf, unsigned numFrames,
                                               unsigned numChannels)>;

    /// B4-K7: Handoff loader callback signature.
    /// Called on the ChucK thread (render loop) to consume a compiled shred
    /// from the atomic handoff slot.  Returns a loaded shred or nullptr
    /// if none is available.  Must be non-blocking and real-time safe
    /// (lock-free via std::atomic_load_explicit).
    using HandoffLoader = std::function<std::shared_ptr<CompiledShred>()>;

    /// Construct a VM for the given tab, with a render callback to fill
    /// the audio block buffer.
    ChuckVM(TabId tabId, RenderCallback renderCb);
    ~ChuckVM();

    ChuckVM(const ChuckVM&)            = delete;
    ChuckVM& operator=(const ChuckVM&) = delete;
    ChuckVM(ChuckVM&&)                 = delete;
    ChuckVM& operator=(ChuckVM&&)      = delete;

    // -----------------------------------------------------------------------
    // Lifecycle (called from the worker control thread, never RT)
    // -----------------------------------------------------------------------

    /// Create the ChucK instance and start the dedicated OS thread.
    /// K0.5: initialisation is single-threaded; no concurrent compile/run.
    /// @return VMResult with ok=true on success.
    VMResult activate(unsigned sampleRate = 44100, unsigned channels = 1);

    /// B4-K7: Set the handoff loader callback.  Called once on activate (or
    /// via setHandoffLoader before activate).  The render thread invokes this
    /// to consume compiled shreds from the atomic handoff slot.
    void setHandoffLoader(HandoffLoader loader) noexcept;

    /// Stop the VM thread and tear down the ChucK instance.
    /// Per policy this can be a suspend (retain state) or full destroy.
    /// @param suspend  If true, keep the Chuck instance alive and just
    ///                 pause the thread (deterministic suspend).  If false,
    ///                 fully destroy the instance.
    VMResult deactivate(bool suspend = true);

    /// Resume a suspended VM: wake its thread without re-creating the
    /// Chuck instance (state is retained).
    VMResult resume();

    /// Fully destroy the VM and release all resources.  The ChuckVM object
    /// is then reusable via activate().
    VMResult destroy();

    // -----------------------------------------------------------------------
    // Compile (K0.5 serialized — only from the VM's own thread context)
    // -----------------------------------------------------------------------

    /// Compile ChucK source code for this VM.
    /// MUST be called from the VM's dedicated thread or via a serialized
    /// request path.  Uses immediate=FALSE (deferred spork) per K0.5.
    /// @return VMResult with ok=true on success; message contains error text on failure.
    VMResult compileCode(const std::string& code);

    // -----------------------------------------------------------------------
    // State / introspection
    // -----------------------------------------------------------------------

    /// Current lifecycle state.
    VMState state() const noexcept { return state_.load(std::memory_order_acquire); }

    /// Set the lifecycle state (used by watchdog for hang detection/recovery).
    void setState(VMState s, std::memory_order /*order*/ = std::memory_order_release) noexcept
    {
        state_.store(s, std::memory_order_release);
    }

    /// Tab identity.
    TabId tabId() const noexcept { return tabId_; }

    /// Thread ID of the ChucK thread (for watchdog identification).
    /// Returns 0 if the thread is not running.
    std::thread::native_handle_type threadHandle() const noexcept {
        return chucKThreadId_;
    }

    /// Heartbeat counter — incremented once per render block.
    /// Read by the watchdog (B4-K5) to detect stalls.
    /// Uses relaxed ordering per B4-K5 §HEARTBEAT (progress indicator, not data channel).
    uint64_t heartbeat() const noexcept {
        return heartbeat_.load(std::memory_order_relaxed);
    }

    /// Number of render blocks produced since activation.
    uint64_t blocksProduced() const noexcept {
        return blocksProduced_.load(std::memory_order_acquire);
    }

    /// Estimated memory usage of this VM in bytes (best-effort).
    std::size_t memoryUsage() const noexcept {
        return memoryUsage_.load(std::memory_order_acquire);
    }

    /// Last error message (if state == Error).
    std::string lastError() const;

    /// Whether the VM is currently active (running on its thread).
    bool isActive() const noexcept {
        return state_.load(std::memory_order_acquire) == VMState::Active;
    }

    /// Whether the VM is suspended (paused but retainable).
    bool isSuspended() const noexcept {
        return state_.load(std::memory_order_acquire) == VMState::Suspended;
    }

    /// Whether the VM has been destroyed, is in an error state, or is inactive.
    bool isTerminated() const noexcept {
        auto s = state_.load(std::memory_order_acquire);
        return s == VMState::Inactive || s == VMState::Destroyed || s == VMState::Error;
    }

    /// VM generation — increments on every create/replace/destroy.
    /// Used by the watchdog (B4-K5) to detect stale recovery attempts.
    uint64_t generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

    /// Set the VM generation (called by VMManager on activation/recreation).
    void setGeneration(uint64_t gen) noexcept {
        generation_.store(gen, std::memory_order_release);
    }

private:
    // -----------------------------------------------------------------------
    // Internal: ChucK thread entry point
    // -----------------------------------------------------------------------

    /// The ChucK thread's main loop.  Calls the render callback, increments
    /// the heartbeat, and publishes audio to the shared-memory ring.
    void chucKThreadLoop();

    /// Signal the ChucK thread to pause (internal, called from deactivate).
    void signalPause();

    /// Signal the ChucK thread to resume (internal, called from resume).
    void signalResume();

    /// Wake the ChucK thread so it can observe the running flag change.
    void wakeThread();

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    /// Tab identity (slot index [0,15]).
    const TabId tabId_;

    /// Render callback (set at construction; invoked on the ChucK thread).
    RenderCallback renderCb_;

    /// Lifecycle state (atomic for lock-free checks from watchdog/control thread).
    std::atomic<VMState> state_{VMState::Inactive};

    /// VM generation — increments on every create/replace/destroy.
    /// Used by the watchdog (B4-K5) for stale-runtime protection.
    std::atomic<uint64_t> generation_{0};

    /// Control mutex for state transitions (non-RT, called from control thread).
    mutable std::mutex mutex_;

    /// The ChucK thread (created on activate, joined on deactivate/destroy).
    std::thread chucKThread_;
    std::thread::native_handle_type chucKThreadId_{0};

    /// Pulse mechanism for suspend/resume coordination.
    std::mutex        suspendMtx_;
    std::condition_variable suspendCv_;
    std::atomic<bool>  suspendRequested_{false};
    std::atomic<bool>  resumeRequested_{false};
    std::atomic<bool>  vmRunning_{false};

    /// Heartbeat counter (incremented per render block for watchdog).
    std::atomic<uint64_t> heartbeat_{0};

    /// Blocks produced since activation.
    std::atomic<uint64_t> blocksProduced_{0};

    /// Best-effort memory usage.
    std::atomic<std::size_t> memoryUsage_{0};

    /// Last error message (set when state transitions to Error).
    std::string lastError_;
    mutable std::mutex errorMtx_;

    /// Audio format (set on activate).
    unsigned sampleRate_{44100};
    unsigned channels_{1};

    /// Block size for rendering.
    static constexpr unsigned kRenderBlockSize = 64;

    // NOTE: The actual ChucK* instance is not stored here — this is a
    // forward-compatible design.  When B4-K4 adds libchuck integration,
    // the ChucK instance will be created here with immediate=FALSE compile
    // semantics per the K0.5 decision.  For B4-K3 we manage the lifecycle
    // state machine and thread model, with a placeholder render callback
    // that produces silence (real ChucK output arrives via B4-K4).

    /// Opaque placeholder for the ChucK instance (set when B4-K4 lands).
    /// While B4-K3 is active, this remains null and the render callback
    /// is expected to produce silence or a placeholder tone.
    void* chuckInstance_{nullptr};
};

} // namespace hathor::audio_worker
