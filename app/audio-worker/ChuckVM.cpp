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
#include "ChuckRuntime.hpp"

#ifdef CHUCK_AVAILABLE
#include "chuck.h"
#include "chuck_errmsg.h"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <pthread.h>

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
    std::unique_lock<std::mutex> lock(mutex_);

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
        // The thread has been paused; the destroy() path will signal
        // and join.  Release the lock first to avoid deadlock.
        lock.unlock();
        return destroy();
    }
}

VMResult ChuckVM::forceDestroy(std::chrono::milliseconds timeout)
{
    (void)timeout; // Reserved for future cooperative-join with timeout
    std::unique_lock<std::mutex> lock(mutex_);

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

    // Release the lock before attempting to cancel/join — the thread may
    // be holding suspendMtx_ (via suspendCv_.wait) and we need wakeThread
    // to have been called (it was above) before trying to join.
    std::thread threadToJoin = std::move(chucKThread_);
    std::thread::native_handle_type tid = chucKThreadId_;
    chucKThreadId_ = 0;
    lock.unlock();

    // If the thread is hung (render callback is a busy loop), it won't
    // observe the Destroyed state until the callback returns.  We can't
    // join the thread without it exiting.  We use pthread_cancel to
    // forcibly terminate the thread — on POSIX, this sends a cancellation
    // at the next cancellation point (sched_yield, sleep, condvar wait).
    //
    // Strategy:
    //   1. First, try cooperative shutdown: poll pthread_kill(tid, 0) to
    //      check if the thread has exited on its own.
    //   2. If that fails, use pthread_cancel to forcibly terminate.
    //
    // NOTE: On macOS, pthread_cancel may interact poorly with signal
    // handlers in test frameworks (Catch2), which is why we try
    // cooperative shutdown first and give the thread a reasonable window.
    if (threadToJoin.joinable()) {
        // Poll for cooperative exit: check if the thread has exited.
        auto deadline = std::chrono::steady_clock::now() + timeout;
        bool cooperativeExit = false;

        while (std::chrono::steady_clock::now() < deadline) {
            // pthread_kill with signal 0 checks if the thread is alive.
            int rv = pthread_kill(tid, 0);
            if (rv == ESRCH) {
                cooperativeExit = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (!cooperativeExit) {
            int cancelRv = pthread_cancel(tid);
            (void)cancelRv;
        }

        // Join the thread (it should have exited by now).
        try {
            threadToJoin.join();
        } catch (...) {
            threadToJoin.detach();
        }
    }

    // The thread was cancelled — its RAII instance cleanup did not run, so
    // the real ChucK instance (if any) must be released here.  The thread has
    // been joined, so no other thread can be inside the instance.
    destroyChuckInstance();
    heartbeat_.store(0, std::memory_order_release);
    blocksProduced_.store(0, std::memory_order_release);

    return {true, 0, "vm force-destroyed"};
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
    // The VM thread's RAII cleanup destroyed the instance on exit; ensure the
    // member is null regardless (no-op if already cleaned up).
    destroyChuckInstance();
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
    // same VM from a different thread.  The B4-K7 pipeline routes real
    // compilation through the VM thread (handoff → loadShredFromHandoff),
    // which is the only legal compile path while the VM is Active.
    auto currentState = state_.load(std::memory_order_acquire);

    if (currentState == VMState::Active) {
        // Compilation must happen on the VM thread between run() calls.
        return {true, 0, "compile deferred to VM thread (K0.5) — use ck_compile handoff"};
    }

    if (currentState == VMState::Suspended && chuckInstance_) {
        // The VM thread is paused — safe to compile directly on the instance.
#ifdef CHUCK_AVAILABLE
        std::lock_guard<std::mutex> lock(chuckCompileMutex());
        EM_reset_msg();
        std::vector<t_CKUINT> shredIDs;
        t_CKBOOL ok = chuckInstance_->compileCode(code, "", 1, FALSE, &shredIDs, "test.ck");
        if (ok) {
            loadedShredId_.store(shredIDs.empty() ? -1 : static_cast<int>(shredIDs[0]),
                                 std::memory_order_release);
            loadedSourceHash_.store(0, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(errorMtx_);
                lastErrorMsg_.clear();
                lastError_ = "compiled while suspended";
            }
            lastErrorLine_.store(0, std::memory_order_release);
            return {true, 0, "compiled while suspended"};
        }
        const char* err = EM_lasterror();
        {
            std::lock_guard<std::mutex> lock(errorMtx_);
            lastErrorMsg_ = (err && err[0]) ? err : "chuck compile failed";
            lastError_ = lastErrorMsg_;
        }
        return {false, 4, lastErrorMsg_};
#else
        (void)code;
        return {false, 5, "libchuck unavailable (CHUCK_AVAILABLE=0)"};
#endif
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
    // Enable pthread cancellation so the watchdog can forcibly terminate
    // a hung thread (B4-K5 recovery path).  Cancellation is deferred —
    // the thread is only cancelled at well-defined cancellation points
    // (e.g., sleep_for, condition_variable::wait, sched_yield).  This is
    // safe for C++ threads: cleanup runs at defined points, no mid-
    // instruction interruption.  The render callback must contain at least
    // one cancellation point per iteration (e.g., std::this_thread::yield())
    // for this to work.  The watchdog enforces this contract: render
    // callbacks that spin without any cancellation points are a programming
    // error (they hold a CPU core indefinitely and cannot be interrupted).
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

    // This thread is the sole owner of the real ChucK instance.  It creates
    // the instance, calls run() to advance ChucK time and synthesize audio,
    // and — between run() calls — compiles handoff shreds via compileCode()
    // (K0.5: compileCode() is never called concurrently with run()).
    //
    // RAII cleanup: on ANY normal thread exit (destroy, suspend-destroyed,
    // init failure) the real instance is deleted here.  The forceDestroy()
    // path (pthread_cancel) does not unwind this scope, so it performs the
    // same cleanup after joining.
    struct ChuckInstanceCleanup {
        ChuckVM& vm;
        ~ChuckInstanceCleanup() { vm.destroyChuckInstance(); }
    } cleanup{*this};

    // Render scratch: one ChucK audio block (kRenderBlockSize frames × up to
    // 2 output channels, interleaved).  This thread is NOT the JUCE audio
    // thread, so a bounded stack buffer is fine.
    float renderBuf[kRenderBlockSize * 2];

    // Give the watchdog an immediate first heartbeat: the one-time ChucK
    // instance init below (~50-100ms) must never be mistaken for a hang.
    heartbeat_.fetch_add(1, std::memory_order_relaxed);

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

        // Check if the thread is being stopped (destroy/force-destroy).
        if (state_.load(std::memory_order_acquire) == VMState::Destroyed)
            return;

        // B4-K8 test-mode: if testHangFlag_ is set, spin without advancing
        // the heartbeat.  This simulates a hung ChucK shred (while(true){}
        // with no now =>).  The render callback itself is NOT called, so
        // heartbeat/blocksProduced stay stalled.  The watchdog (B4-K5) will
        // detect the stall and trigger recovery via forceDestroyVM.
        if (testHangFlag_.load(std::memory_order_acquire)) {
            // Spin — this is a cancellation point (yield), so the watchdog's
            // pthread_cancel can terminate this thread if cooperative cleanup
            // fails.  We deliberately do NOT increment heartbeat here.
            while (testHangFlag_.load(std::memory_order_acquire)) {
                if (state_.load(std::memory_order_acquire) == VMState::Destroyed)
                    return;
                std::this_thread::yield();  // cancellation point
            }
            // Fall through to normal rendering after hang cleared.
        }

        // -----------------------------------------------------------------
        // B4-K7: Consume any handoff shred that the compile dispatcher
        // validated for this tab.  The REAL compile+spork happens here, on
        // this thread, BETWEEN run() calls (K0.5 — never concurrent with
        // run()): the dispatcher only ran diagnostics on a transient
        // instance; the shred is actually loaded into THIS instance now.
        //
        // The real ChucK instance is created LAZILY — only when the first
        // handoff shred arrives.  An idle VM (no shred loaded) never pays
        // the one-time init cost (~65ms, globally serialized), so its
        // heartbeat/blocks stay fast for the watchdog and it renders plain
        // silence.  Init failure at load time is an honest runtime failure:
        // the VM enters Error state (not reported as successfully running).
        // -----------------------------------------------------------------
        if (handoffLoader_)
        {
            auto shred = handoffLoader_();
            if (shred && shred->ok)
            {
                if (!chuckInstance_)
                {
                    heartbeat_.fetch_add(1, std::memory_order_relaxed);
                    if (!createChuckInstance())
                    {
                        // lastError_ already recorded — surface as Error so
                        // the watchdog/main process can react honestly.
                        state_.store(VMState::Error, std::memory_order_release);
                        return;
                    }
                    heartbeat_.fetch_add(1, std::memory_order_relaxed);
                }
                loadShredFromHandoff(shred);
            }
            else if (shred && !shred->ok)
            {
                std::lock_guard<std::mutex> lock(errorMtx_);
                lastErrorMsg_ = shred->error;
                lastErrorLine_.store(shred->errorLine, std::memory_order_release);
                lastError_ = shred->error;
            }
        }

        // -----------------------------------------------------------------
        // B4-K4: advance ChucK time and synthesize REAL audio into the block.
        // With zero input channels the input pointer is never dereferenced
        // (verified against Chuck_VM::run in the vendored source).  With no
        // loaded shred the VM produces silence (halt=FALSE keeps it running).
        // -----------------------------------------------------------------
        if (chuckInstance_)
        {
            static const float kSilenceInput[kRenderBlockSize] = {0.0f};
            chuckInstance_->run(kSilenceInput, renderBuf, kRenderBlockSize);
        }
        else
        {
            std::memset(renderBuf, 0, sizeof(renderBuf));
        }

        // Publish the block.  The worker's render callback writes the real
        // samples into the shared-memory ring (no placeholder synthesis).
        renderCb_(renderBuf, kRenderBlockSize, channels_);

        // B4-K7: reflect actual execution status — if the loaded shred has
        // exited (finished or crashed at runtime), stop reporting it as live.
        refreshShredLiveness();

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

// ---------------------------------------------------------------------------
// B4-K4: Real ChucK instance lifecycle (VM thread only)
// ---------------------------------------------------------------------------

bool ChuckVM::createChuckInstance()
{
#ifdef CHUCK_AVAILABLE
    // Disable pthread cancellation for the duration of the global-mutex
    // critical section.  On macOS, pthread cancellation does NOT unwind C++
    // stack frames, so a cancel delivered while we hold chuckInstanceMutex()
    // would leave the mutex permanently locked and deadlock every later
    // instance create/destroy (including watchdog recovery).  Cancellation
    // is deferred anyway; we just push the delivery point past the lock.
    int oldCancel = 0;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldCancel);
    {
        // Serialize against other instances' construction/destruction —
        // libchuck's static counters (o_numVMs, o_isGlobalInit) are plain
        // statics.
        std::lock_guard<std::mutex> lock(chuckInstanceMutex());

        auto* ck = new ChucK();
        ck->setParam(CHUCK_PARAM_SAMPLE_RATE, static_cast<t_CKINT>(sampleRate_));
        ck->setParam(CHUCK_PARAM_INPUT_CHANNELS, 0);
        // The shared-memory transport carries mono blocks; clamp the VM's dac
        // to at most 2 channels (stereo is the ChucK norm) and downmix on
        // publish.
        const unsigned ckChannels = (channels_ == 0) ? 1u : (channels_ > 2 ? 2u : channels_);
        ck->setParam(CHUCK_PARAM_OUTPUT_CHANNELS, static_cast<t_CKINT>(ckChannels));
        ck->setParam(CHUCK_PARAM_VM_HALT, FALSE);       // keep VM running without shreds
        ck->setParam(CHUCK_PARAM_IS_REALTIME_AUDIO_HINT, FALSE);
        ck->setParam(CHUCK_PARAM_CHUGIN_ENABLE, FALSE); // no chugins in this build

        if (!ck->init()) {
            lastError_ = "libchuck init() failed for tab " + std::to_string(tabId_);
            delete ck;
            pthread_setcancelstate(oldCancel, nullptr);
            return false;
        }
        if (!ck->start()) {
            lastError_ = "libchuck start() failed for tab " + std::to_string(tabId_);
            delete ck;
            pthread_setcancelstate(oldCancel, nullptr);
            return false;
        }

        chuckInstance_ = ck;
    }
    pthread_setcancelstate(oldCancel, nullptr);
    return true;
#else
    lastError_ = "libchuck unavailable (CHUCK_AVAILABLE=0); no ChucK execution";
    return false;
#endif
}

void ChuckVM::destroyChuckInstance()
{
#ifdef CHUCK_AVAILABLE
    if (chuckInstance_)
    {
        // Same cancellation-disabled window as createChuckInstance(): the
        // instance delete runs under the global mutex and must not be
        // interrupted by a cancel that would strand the lock.
        int oldCancel = 0;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldCancel);
        {
            std::lock_guard<std::mutex> lock(chuckInstanceMutex());
            delete chuckInstance_;
            chuckInstance_ = nullptr;
        }
        pthread_setcancelstate(oldCancel, nullptr);
    }
#else
    chuckInstance_ = nullptr;
#endif
}

// ---------------------------------------------------------------------------
// B4-K7: Real shred load (VM thread only — between run() calls)
// ---------------------------------------------------------------------------

void ChuckVM::loadShredFromHandoff(const std::shared_ptr<CompiledShred>& shred)
{
    if (!shred)
        return;

    loadedSourceHash_.store(shred->sourceHash, std::memory_order_release);

#ifdef CHUCK_AVAILABLE
    if (!chuckInstance_)
        return;

    // Serialize all compileCode() calls across instances — libchuck's error
    // buffer (EM_reset_msg/EM_lasterror) is a global shared by every ChucK.
    // Cancellation is disabled while holding the global compile mutex: a
    // pthread_cancel delivered mid-compile (which can take tens of ms) would
    // strand the lock and deadlock every later compile (K0.5 recovery path).
    int oldCancel = 0;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldCancel);
    {
        std::lock_guard<std::mutex> lock(chuckCompileMutex());

        EM_reset_msg();
        std::vector<t_CKUINT> shredIDs;
        // immediate=FALSE: the shred is queued and sporked by the VM's compute()
        // on the next time step (the next run() call), entirely on this thread.
        t_CKBOOL ok = chuckInstance_->compileCode(
            shred->sourceCode, "", 1, FALSE, &shredIDs, "test.ck");

        if (ok && !shredIDs.empty())
        {
            // Replace semantics: remove any previously running shreds.  In the
            // vendored Chuck_VM::compute(), the remove-all flag is processed at
            // the TOP of the next compute() — BEFORE the newly queued shred is
            // sporked — so the old shred is removed and the new one survives.
            // On failure we do NOT remove the old shred (it keeps running).
            //
            // Also guard on shredIDs.empty(): a source that compiles to ZERO
            // shreds (e.g. a bare function/class definition) must NOT silently
            // kill the previously running program nor report success.  The old
            // shred keeps running and the empty result is reported honestly.
            chuckInstance_->removeAllShreds();

            loadedShredId_.store(static_cast<int>(shredIDs[0]),
                                 std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(errorMtx_);
                lastErrorMsg_.clear();
                lastError_ = "shred loaded (id="
                             + std::to_string(loadedShredId_.load(std::memory_order_relaxed))
                             + ")";
            }
            lastErrorLine_.store(0, std::memory_order_release);
        }
        else if (ok)
        {
            // Compiled but produced no shred — do not disturb the running
            // program and do not report a live shred (B4-K7 honesty).
            std::lock_guard<std::mutex> lock(errorMtx_);
            lastErrorMsg_ = "code compiled but produced no runnable shred; "
                            "previous program left running";
            lastError_ = lastErrorMsg_;
            lastErrorLine_.store(0, std::memory_order_release);
            loadedShredId_.store(-1, std::memory_order_release);
        }
        else
        {
            // Compile/load failed at the VM level despite passing dispatcher
            // validation — report honestly; the previous shred (if any) keeps
            // running and the loaded-shred id is not updated.
            const char* err = EM_lasterror();
            const std::string msg = (err && err[0]) ? std::string(err)
                                                    : "shred load failed (VM rejected)";
            std::lock_guard<std::mutex> lock(errorMtx_);
            lastErrorMsg_ = msg;
            lastError_ = msg;
            lastErrorLine_.store(0, std::memory_order_release);
        }
    }
    pthread_setcancelstate(oldCancel, nullptr);
#else
    // No libchuck: nothing can execute.  Keep loadedShredId_ = -1 so status
    // reflects that no shred is actually running (no fake success).
    loadedShredId_.store(-1, std::memory_order_release);
#endif
}

void ChuckVM::refreshShredLiveness()
{
    const int id = loadedShredId_.load(std::memory_order_relaxed);
    if (id < 0)
        return;
#ifdef CHUCK_AVAILABLE
    if (!chuckInstance_)
        return;
    // Called on the VM thread between run() calls; the shreduler is not
    // concurrently mutated (no other thread touches this instance).  The
    // scratch vector is thread_local so it is reused across iterations and
    // never shared between VM threads.
    static thread_local std::vector<t_CKUINT> ids;
    ids.clear();
    chuckInstance_->vm()->shreduler()->get_all_shred_ids(ids);
    for (t_CKUINT x : ids)
    {
        if (static_cast<int>(x) == id)
            return; // still running
    }
    // The loaded shred exited — finished or crashed at runtime.
    loadedShredId_.store(-1, std::memory_order_release);
#else
    loadedShredId_.store(-1, std::memory_order_release);
#endif
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

// ---------------------------------------------------------------------------
// B4-K8: Test-mode — simulate a hung shred
// ---------------------------------------------------------------------------

void ChuckVM::setTestHangCallback() noexcept
{
    testHangFlag_.store(true, std::memory_order_release);
}

void ChuckVM::clearTestHangCallback() noexcept
{
    testHangFlag_.store(false, std::memory_order_release);
}

} // namespace hathor::audio_worker
