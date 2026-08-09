// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckVM.cpp — per-tab ChucK VM lifecycle implementation (B4-K3).
 *
 * Implements the VM lifecycle state machine, dedicated OS thread management,
 * deterministic suspend/resume, K0.5 serialization, and watchdog heartbeat
 * tracking.
 */

#include "ChuckVm.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

namespace hathor::audio_worker {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ChuckVM::ChuckVM(TabId tabId, RenderCallback renderCb)
    : tabId_(tabId)
    , renderCb_(renderCb ? renderCb : RenderCallback{
        [](float* outBuf, unsigned numFrames, unsigned /*numChannels*/) {
            std::memset(outBuf, 0, numFrames * sizeof(float));
        }})
{
}

ChuckVM::~ChuckVM()
{
    // Ensure the VM is fully torn down.  This is safe to call from any thread;
    // deactivate() and destroy() are idempotent.
    if (chucKThread_.joinable()) {
        state_.store(VMState::Inactive, std::memory_order_release);
        suspendRequested_.store(true, std::memory_order_release);
        resumeRequested_.store(true, std::memory_order_release);
        wakeThread();
        chucKThread_.join();
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

VMResult ChuckVM::activate(unsigned sampleRate, unsigned channels)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto currentState = state_.load(std::memory_order_acquire);
    if (currentState == VMState::Active) {
        return {true, 0, "vm already active"};
    }

    // If we're in Suspended state, just resume the thread (no re-creation needed).
    if (currentState == VMState::Suspended) {
        return resume();
    }

    // If we're in Error state, we can recover by re-creating.
    if (currentState == VMState::Error) {
        // Fall through — we'll create a fresh thread.
    }

    // Inactive or Destroyed: create a new ChucK thread.
    sampleRate_ = sampleRate;
    channels_   = channels;

    // K0.5: This is the only place where thread creation happens.
    // The ChucK thread is created here (not on the audio callback thread).
    // Initialisation is single-threaded — no concurrent compile/run.

    vmRunning_.store(true, std::memory_order_release);
    suspendRequested_.store(false, std::memory_order_release);
    resumeRequested_.store(false, std::memory_order_release);

    // Reset heartbeat for a fresh VM.
    heartbeat_.store(0, std::memory_order_release);
    blocksProduced_.store(0, std::memory_order_release);
    // Increment the VM generation for this new lifecycle.
    generation_.fetch_add(1, std::memory_order_acq_rel);

    try {
        chucKThread_ = std::thread(&ChuckVM::chucKThreadLoop, this);
        chucKThreadId_ = chucKThread_.native_handle();
    } catch (const std::exception& e) {
        lastError_ = "failed to create ChucK thread: " + std::string(e.what());
        state_.store(VMState::Error, std::memory_order_release);
        return {false, 1, lastError_};
    }

    state_.store(VMState::Active, std::memory_order_release);
    return {true, 0, "vm activated"};
}

// ---------------------------------------------------------------------------
// B4-K7: Handoff loader registration
// ---------------------------------------------------------------------------

void ChuckVM::setHandoffLoader(HandoffLoader loader) noexcept
{
    handoffLoader_ = std::move(loader);
}

VMResult ChuckVM::deactivate(bool suspend)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto currentState = state_.load(std::memory_order_acquire);
    if (currentState != VMState::Active) {
        return {true, 0, "vm not active"};
    }

    // Signal the ChucK thread to pause.
    suspendRequested_.store(true, std::memory_order_release);
    wakeThread();

    // Wait for the thread to pause (bounded — 100ms).
    // The ChucK thread checks suspendRequested_ at the top of each render loop.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < deadline) {
        // The thread sets vmRunning_ to false when it pauses.
        if (!vmRunning_.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (suspend) {
        // Deterministic suspend: Chuck instance stays alive, thread is paused.
        state_.store(VMState::Suspended, std::memory_order_release);
        return {true, 0, "vm suspended"};
    } else {
        // Full destroy: tear down the Chuck instance.
        // The thread has been paused; now we let it exit.
        state_.store(VMState::Destroyed, std::memory_order_release);
        resumeRequested_.store(true, std::memory_order_release);
        wakeThread();

        if (chucKThread_.joinable()) {
            chucKThread_.join();
        }
        chucKThreadId_ = 0;
        chuckInstance_ = nullptr;

        return {true, 0, "vm destroyed"};
    }
}

VMResult ChuckVM::resume()
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto currentState = state_.load(std::memory_order_acquire);
    if (currentState != VMState::Suspended) {
        return {false, 2, "vm not suspended (current state: "
                          + std::to_string(static_cast<int>(currentState)) + ")"};
    }

    // Resume the thread.  The Chuck instance is still alive.
    vmRunning_.store(true, std::memory_order_release);
    suspendRequested_.store(false, std::memory_order_release);
    resumeRequested_.store(true, std::memory_order_release);

    // If the thread was joined (destroy path), we need to restart it.
    // In the suspend path, the thread is just paused, not joined.
    if (!chucKThread_.joinable()) {
        try {
            chucKThread_ = std::thread(&ChuckVM::chucKThreadLoop, this);
            chucKThreadId_ = chucKThread_.native_handle();
        } catch (const std::exception& e) {
            lastError_ = "failed to restart ChucK thread: " + std::string(e.what());
            state_.store(VMState::Error, std::memory_order_release);
            return {false, 1, lastError_};
        }
    } else {
        // Thread is paused — just signal it to resume.
        wakeThread();
    }

    state_.store(VMState::Active, std::memory_order_release);
    return {true, 0, "vm resumed"};
}

VMResult ChuckVM::destroy()
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto currentState = state_.load(std::memory_order_acquire);
    if (currentState == VMState::Inactive || currentState == VMState::Destroyed) {
        return {true, 0, "vm already destroyed"};
    }

    // Signal the ChucK thread to stop.
    state_.store(VMState::Destroyed, std::memory_order_release);
    vmRunning_.store(false, std::memory_order_release);
    suspendRequested_.store(false, std::memory_order_release);
    resumeRequested_.store(false, std::memory_order_release);
    wakeThread();

    if (chucKThread_.joinable()) {
        chucKThread_.join();
    }
    chucKThreadId_ = 0;
    chuckInstance_ = nullptr;
    heartbeat_.store(0, std::memory_order_release);
    blocksProduced_.store(0, std::memory_order_release);

    return {true, 0, "vm destroyed"};
}

// ---------------------------------------------------------------------------
// Compile (K0.5 serialized)
// ---------------------------------------------------------------------------

VMResult ChuckVM::compileCode(const std::string& code)
{
    // K0.5: compileCode() must NOT be called concurrently with run() on the
    // same VM from a different thread.  The B4-K4 pipeline serializes this:
    // compilation happens on the worker control thread, and only when the
    // VM is in a known state.  Here we enforce that the VM is active or
    // that we're in the suspended state (safe to compile without running).
    (void)code;  // K3 placeholder — actual libchuck compile lands in B4-K4
    auto currentState = state_.load(std::memory_order_acquire);

    if (currentState == VMState::Active) {
        // K0.5: When the VM is active and running, compile must use the
        // deferred path (immediate=FALSE).  The actual compile happens on
        // the VM's own thread via a serialized request mechanism.  For K3,
        // we record the compile request; the actual libchuck integration
        // (B4-K4) implements the deferred spork.
        //
        // The key invariant: we never call compileCode() from a different
        // thread while run() is executing on this VM's thread.
        return {true, 0, "compile deferred to VM thread (K0.5)"};
    }

    if (currentState == VMState::Suspended) {
        // Safe to compile while suspended (VM thread is paused).
        return {true, 0, "compile queued while suspended"};
    }

    return {false, 3, "vm not available for compile (state="
                       + std::to_string(static_cast<int>(currentState)) + ")"};
}

// ---------------------------------------------------------------------------
// State / introspection
// ---------------------------------------------------------------------------

std::string ChuckVM::lastError() const
{
    std::lock_guard<std::mutex> lock(errorMtx_);
    return lastError_;
}

// ---------------------------------------------------------------------------
// ChucK thread entry point
// -----------------------------------------------------------------------

void ChuckVM::chucKThreadLoop()
{
    // This thread is the sole owner of the ChucK instance.  It calls run()
    // (via the render callback) and handles suspend/resume coordination.
    //
    // K0.5: compileCode() is serialized — it is either:
    //   (a) called from this thread directly (when the VM is the only caller), or
    //   (b) queued via a request pipe and processed here between run() calls.
    // The B4-K4 implementation will provide the request pipe and deferred
    // spork mechanism.  For K3, we focus on the lifecycle + thread model.

    float renderBuf[64 * 2]; // kRenderBlockSize * maxChannels

    while (true) {
        // Check for stop / destroy.
        auto s = state_.load(std::memory_order_acquire);
        if (s == VMState::Destroyed || !vmRunning_.load(std::memory_order_acquire)) {
            return; // Thread exits.
        }

        // Check for suspend request.
        if (suspendRequested_.load(std::memory_order_acquire) &&
            s == VMState::Active) {
            // Pause: set vmRunning_ false so the watchdog sees we're paused.
            vmRunning_.store(false, std::memory_order_release);
            state_.store(VMState::Suspended, std::memory_order_release);

            // Wait until resumed or destroyed.
            std::unique_lock<std::mutex> lk(suspendMtx_);
            suspendCv_.wait(lk, [this] {
                return resumeRequested_.load(std::memory_order_acquire) ||
                       state_.load(std::memory_order_acquire) == VMState::Destroyed;
            });

            auto ns = state_.load(std::memory_order_acquire);
            if (ns == VMState::Destroyed) {
                return; // Thread exits.
            }

            // Resume: set state back to Active and vmRunning_ true.
            vmRunning_.store(true, std::memory_order_release);
            state_.store(VMState::Active, std::memory_order_release);
            suspendRequested_.store(false, std::memory_order_release);
            resumeRequested_.store(false, std::memory_order_release);
        }

        // Render one audio block via the callback.
        renderCb_(renderBuf, kRenderBlockSize, channels_);

        // -----------------------------------------------------------------
        // B4-K7: Consume any handoff shred that the compile dispatcher
        // published for this tab.  This is the compile→load→execute path:
        // the dispatcher compiled the code and published a CompiledShred
        // via std::atomic_store_explicit; here we load it via the
        // injected HandoffLoader callback (lock-free atomic load).
        // The actual shred execution is simulated (placeholder tone above);
        // when libchuck is linked, loadShred() will spork the shred here.
        // -----------------------------------------------------------------
        if (handoffLoader_)
        {
            auto shred = handoffLoader_();
            if (shred && shred->ok)
            {
                loadedShredId_.store(static_cast<int>(shred->loadedShredId),
                                     std::memory_order_release);
                loadedSourceHash_.store(shred->sourceHash, std::memory_order_release);
            }
            else if (shred && !shred->ok)
            {
                std::lock_guard<std::mutex> lock(errorMtx_);
                lastErrorMsg_ = shred->error;
                lastErrorLine_.store(shred->errorLine, std::memory_order_release);
                lastError_ = shred->error;
            }
        }

        // Increment heartbeat (for B4-K5 watchdog).
        // Per B4-K5 §HEARTBEAT: use relaxed atomic — the heartbeat is a
        // progress indicator, not a data channel.  Synchronization semantics
        // do not require stronger ordering here.
        heartbeat_.fetch_add(1, std::memory_order_relaxed);
        blocksProduced_.fetch_add(1, std::memory_order_relaxed);

        // Sleep until next render tick (5ms ≈ 12800 Hz at kBlockSize=64).
        // The ChucK thread is real-time-ish but not in the audio callback
        // path; the audio thread consumes from the shared-memory ring.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void ChuckVM::signalPause()
{
    suspendRequested_.store(true, std::memory_order_release);
}

void ChuckVM::signalResume()
{
    resumeRequested_.store(true, std::memory_order_release);
}

void ChuckVM::wakeThread()
{
    // Wake the ChucK thread if it's blocked in suspendCv_.
    // We use suspendMtx_ to synchronize; the thread checks the condition
    // variables atomically.
    std::lock_guard<std::mutex> lk(suspendMtx_);
    suspendCv_.notify_one();
}

} // namespace hathor::audio_worker
