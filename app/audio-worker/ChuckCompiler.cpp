// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckCompiler.cpp — serialized ChucK compilation + atomic handoff.
 *
 * K0.5 NO-GO: compileCode() and run() must not be called concurrently.
 * The dispatcher thread is the sole caller of any ChucK compile path.
 *
 * If/when libchuck is vendored into this process, the actual compileCode()
 * call goes here inside dispatcherLoop(), under no contention from any
 * VM run() thread. The compile thread publishes the CompiledShred via
 * std::atomic_store_explicit(release) on the VM's handoffShred slot.
 *
 * The VM render thread (per-tab) consumes via std::atomic_load_explicit(acquire)
 * in its loop, on the next iteration.
 *
 * Until libchuck is linked in, compile simulates the result (ok=true, shred
 * descriptor allocated) so the handoff path can be tested end-to-end.
 *
 * Requirements: B4-K4, K0.5, B4-K3, B4-K2
 */

// ---------------------------------------------------------------------------
// Includes (B4-K4: real libchuck integration)
// ---------------------------------------------------------------------------
#include "ChuckCompiler.hpp"
#include "ChuckVm.hpp"
#include "ChuckDiagnostics.hpp"

#ifdef CHUCK_AVAILABLE
#include "chuck.h"
#include "chuck_errmsg.h"
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <tuple>

namespace hathor::audio_worker {

// ---------------------------------------------------------------------------
// FNV-1a 64-bit hash — allocation-free, deterministic. Used for sourceHash.
// ---------------------------------------------------------------------------
static uint64_t fnv1a(const char* data, std::size_t len) noexcept
{
    const uint64_t kFNVOffsetBasis = 14695981039346656037ULL;
    const uint64_t kFNVPrime       = 1099511628211ULL;
    uint64_t h = kFNVOffsetBasis;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(static_cast<unsigned char>(data[i]));
        h *= kFNVPrime;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ChuckCompiler::ChuckCompiler(VmLookup lookup, CancelCheck cancelCheck)
    : lookup_(std::move(lookup)),
      cancelCheck_(std::move(cancelCheck))
{
    dispatchThread_ = std::thread(&ChuckCompiler::dispatcherLoop, this);
}

ChuckCompiler::~ChuckCompiler()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// enqueue() — non-blocking, called from the control thread
// ---------------------------------------------------------------------------

void ChuckCompiler::enqueue(CompileCommand&& cmd)
{
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        pending_.push(std::move(cmd));
    }
    cv_.notify_one();
}

// ---------------------------------------------------------------------------
// shutdown() — drains the queue, then signals the dispatcher to stop
// ---------------------------------------------------------------------------

void ChuckCompiler::shutdown()
{
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (dispatchThread_.joinable())
        dispatchThread_.join();
}

// ---------------------------------------------------------------------------
// dispatcherLoop() — the SOLE thread that calls ChucK compile APIs
// ---------------------------------------------------------------------------

void ChuckCompiler::dispatcherLoop()
{
    while (true) {
        CompileCommand cmd;

        {
            std::unique_lock<std::mutex> lock(queueMtx_);
            cv_.wait(lock, [this] {
                return !pending_.empty() || !running_.load(std::memory_order_acquire);
            });

            if (!running_.load(std::memory_order_acquire) && pending_.empty())
                return; // shutdown and drained

            if (pending_.empty())
                continue; // spurious wakeup

            cmd = std::move(pending_.front());
            pending_.pop();
        }

        // ---------------------------------------------------------------
        // Resolve the target VM. If it was destroyed/replaced, the
        // generation won't match — discard the result.
        // ---------------------------------------------------------------
        ChuckVmEntry* vm = lookup_(cmd.tabId, cmd.vmGeneration);
        if (!vm) {
            // VM doesn't exist, was destroyed, or generation mismatch.
            auto result = std::make_shared<CompiledShred>();
            result->ok = false;
            result->error = "target VM is inactive, destroyed, or generation mismatch";
            result->requestVersion = cmd.requestVersion;
            if (cmd.onResponse)
                cmd.onResponse(std::move(result));
            continue;
        }

        // ---------------------------------------------------------------
        // SERIALIZED CHUCk VALIDATION (K0.5 NO-GO enforced here)
        //
        // At this point, this is the ONLY thread calling the ChucK compile
        // API for diagnostics.  The VM's run() thread never calls
        // compileCode() concurrently — real compilation for a live VM is
        // performed on the VM's own thread (ChuckVM::loadShredFromHandoff)
        // between run() calls, which is the only K0.5-safe placement.
        //
        // When libchuck is linked, validateChuckSource() runs the REAL
        // vendored compiler (ChucK::compileCode on a transient, never-run
        // instance) and parses EM_lasterror().  We do NOT spork on a
        // throwaway instance here: a shred compiled on a transient VM that is
        // destroyed immediately would never execute.  Instead we publish the
        // validated source; the per-tab VM performs the authoritative
        // compile + spork and reports the real shred ID.
        //
        // The compile thread publishes results via the existing atomic handoff
        // discipline (std::atomic_store_explicit / std::atomic_load_explicit on
        // shared_ptr<CompiledShred>), matching the pattern in AudioEngine::slots_.
        // ---------------------------------------------------------------
        {
            std::lock_guard<std::mutex> compileLock(dispatchMtx_);

            auto result = std::make_shared<CompiledShred>();
            result->sourceHash = fnv1a(cmd.sourceCode.data(), cmd.sourceCode.size());
            result->sourceCode = cmd.sourceCode;
            result->requestVersion = cmd.requestVersion;
            result->vmGeneration = cmd.vmGeneration;

            // --- ChucK source validation (real diagnostic path, B4-K4) ---
            // validateChuckSource() is the same diagnostic entry point used by
            // the control layer (AI-2/AI-5). When libchuck is linked, it calls
            // ck.compileCode() and parses EM_lasterror().
            ChuckDiagnostic diag = validateChuckSource(cmd.sourceCode);

            if (!diag.ok) {
                result->ok = false;
                result->error = diag.message;
                result->errorLine = diag.errorLine;
                result->errorColumn = diag.errorColumn;
                // On failure: do NOT publish. The VM keeps its current valid shred.
                if (cmd.onResponse)
                    cmd.onResponse(result);
                continue;
            }

            // Validation passed (real compiler diagnostics when libchuck is
            // available; bracket-balancing heuristic otherwise).  The VM's
            // render thread consumes this and performs the REAL compile+
            // spork on its own persistent instance, then reports the actual
            // shred ID.  loadedShredId stays -1 here — it is assigned by the
            // VM on actual load, never fabricated by the dispatcher.
            result->ok = true;
            result->loadedShredId = -1;

            // -------------------------------------------------------
            // AI-5 Phase 2C: Check whether cancellation was requested
            // after the compile work completed but before the handoff
            // is published.  If so, skip the handoff and report a
            // cancellation error so the waiting main-process thread can
            // transition the job to Cancelled.  The callback must still
            // be invoked (the connection needs to be closed); the main
            // process will suppress its onComplete callback because
            // cancelRequested is already set.
            // -------------------------------------------------------
            if (cancelCheck_ && cancelCheck_(cmd.tabId)) {
                result->ok = false;
                result->error = "async compile cancelled";
                result->errorLine = 0;
                result->errorColumn = 0;
                // Do NOT publish the handoff — the VM render thread will
                // never see a cancelled result.
                if (cmd.onResponse)
                    cmd.onResponse(result);
                continue;
            }

            // -------------------------------------------------------
            // ATOMIC HANDOFF — matches AudioEngine::storeSlot() pattern.
            //
            // Apple-Clang compatibility: std::atomic<shared_ptr<T>> is
            // NOT specialized in libc++ (no C++20 specialization in the
            // SDK's headers). We use the C++11 free-function API
            // (std::atomic_store_explicit / std::atomic_load_explicit)
            // on a plain shared_ptr member, which provides the same
            // acquire/release semantics via an internal lock.
            // -------------------------------------------------------
            std::atomic_store_explicit(
                &vm->handoffShred, result,
                std::memory_order_release);

            if (cmd.onResponse)
                cmd.onResponse(result);
        }
    }
}

} // namespace hathor::audio_worker
