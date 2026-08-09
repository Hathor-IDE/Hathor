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

#include "ChuckCompiler.hpp"
#include "ChuckVm.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

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

ChuckCompiler::ChuckCompiler(VmLookup lookup)
    : lookup_(std::move(lookup))
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
        // SERIALIZED CHUCk COMPILATION (K0.5 NO-GO enforced here)
        //
        // At this point, this is the ONLY thread calling any ChucK compile
        // API for this VM. The VM's run() thread never calls compileCode(),
        // so there is no concurrency on the ChucK instance.
        //
        // When libchuck is vendored, the actual compile looks like:
        //   ChucK* ck = vm->chuck;            // the VM's own ChucK instance
        //   std::vector<t_CKUINT> shredIds;
        //   t_CKBOOL ok = ck->compileCode(source, "", 1, FALSE, &shredIds);
        //   if (ok) { result->ok = true; result->shredId = shredIds[0]; }
        //   else    { result->ok = false; result->error = getCompilerError(ck); }
        //
        // For now (no libchuck linked), we simulate a successful compile
        // so the atomic handoff path is testable end-to-end.
        // ---------------------------------------------------------------
        {
            std::lock_guard<std::mutex> compileLock(dispatchMtx_);

            auto result = std::make_shared<CompiledShred>();
            result->sourceHash = fnv1a(cmd.sourceCode.data(), cmd.sourceCode.size());
            result->sourceCode = cmd.sourceCode;
            result->requestVersion = cmd.requestVersion;

            // --- Placeholder: simulate ChucK compile ---
            // A real compile would call compileCode() here. If it returns
            // FALSE, set result->ok = false and capture the compiler error
            // string. The VM must NOT see a partial result.
            result->ok = true;
            result->loadedShredId = -1; // assigned by the VM on consumption
            // --- End placeholder ---

            if (result->ok) {
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
            } else {
                // On failure: do NOT publish. The VM keeps its current valid
                // shred. Only invoke onResponse so the caller gets the error.
            }

            if (cmd.onResponse)
                cmd.onResponse(result);
        }
    }
}

} // namespace hathor::audio_worker
