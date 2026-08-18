// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckCompiler.hpp — serialized ChucK compilation + handoff inside the worker.
 *
 * K0.5 decision: NO-GO for concurrent compileCode()+run(). Therefore ALL
 * ChucK compilation operations on a given VM are serialized through a single
 * WorkerDispatcher thread. The compile thread is the ONLY caller of compileCode()
 * for any VM, and it publishes results via the atomic handoff slot.
 *
 * Threading model:
 *   - Control thread (socket): receives "ck_compile <tab> <version> <source>"
 *     commands, enqueues them on the dispatcher's command queue.
 *   - Dispatcher thread (serialized): the sole thread that calls compileCode().
 *     It processes commands sequentially, ensuring no concurrent compile/run
 *     on the same ChucK instance. After compiling, it publishes the result
 *     into the target VM's handoffShred via std::atomic_store_explicit.
 *   - VM render thread: per-tab, calls run() continuously. Loads the handoff
 *     via std::atomic_load_explicit and processes on the next loop iteration.
 *
 * The compile thread NEVER touches the VM's run() path. The VM render thread
 * NEVER calls compileCode(). This enforces K0.5's serialization requirement.
 *
 * Request/version/generation protection:
 *   - Each compile request carries a per-tab requestVersion (monotonic).
 *   - Each VM carries a vmGeneration that increments on destroy/recreate.
 *   - The compile thread checks vmGeneration at publication time. If the
 *     VM was replaced, the result is discarded.
 *   - The VM render thread checks requestVersion == currentRequestVersion
 *     at consumption time. Stale results (lower version) are ignored.
 *
 * Requirements: B4-K4, K0.5, B4-K3
 */

#pragma once

#include "ChuckVm.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>

namespace hathor::audio_worker {

/// Command to the serialized dispatcher thread.
struct CompileCommand {
    TabId       tabId;
    uint32_t    requestVersion;   ///< matches the version the caller expects
    uint64_t    vmGeneration;       ///< the VM generation this compile targets
    std::string sourceCode;
    /// Response callback — invoked on the dispatcher thread after compile
    /// completes (or is rejected). Must be thread-safe / non-blocking.
    std::function<void(std::shared_ptr<CompiledShred>)> onResponse;
};

/**
 * ChuckCompiler — serialized compilation engine inside the worker process.
 *
 * Spawns a single dispatcher thread that processes CompileCommands sequentially.
 * This thread is the ONLY caller of any ChucK compile API, satisfying K0.5's
 * NO-GO constraint (no concurrent compileCode + run on the same ChucK instance).
 *
 * Compilation may allocate (source parsing, error construction, temporary
 * ChucK compiler state). This is safe because it runs on the dispatcher thread,
 * which is NOT a real-time audio/render path.
 *
 * The compile thread publishes results via the existing atomic handoff
 * discipline (std::atomic_store_explicit / std::atomic_load_explicit on
 * shared_ptr<CompiledShred>), matching the pattern in AudioEngine::slots_.
 */
class ChuckCompiler {
public:
    /// VM table accessor type — a function that resolves a TabId to its
    /// current VM entry. The compiler calls this to check liveness and
    /// publish the handoff. Must be callable from the dispatcher thread.
    /// Returns nullptr if the VM is Inactive, Destroyed, or if the
    /// generation does not match cmd.vmGeneration.
    using VmLookup = std::function<ChuckVmEntry*(TabId, uint64_t)>;

    /// Cancel-check accessor type — returns true if cancellation has been
    /// requested for the given tab. The dispatcher calls this after the
    /// dispatch lock is released but before calling onResponse, so it can
    /// suppress the handoff for cancelled jobs.
    using CancelCheck = std::function<bool(TabId)>;

    explicit ChuckCompiler(VmLookup lookup, CancelCheck cancelCheck = {});
    ~ChuckCompiler();

    ChuckCompiler(const ChuckCompiler&) = delete;
    ChuckCompiler& operator=(const ChuckCompiler&) = delete;

    /// Enqueue a compile request. Non-blocking — returns immediately.
    /// The request will be processed on the dispatcher thread.
    /// @param cmd  The compile command (tab, version, generation, source).
    void enqueue(CompileCommand&& cmd);

    /// Shut down the dispatcher thread. Waits for all queued commands
    /// to drain before returning.
    void shutdown();

private:
    void dispatcherLoop();

    VmLookup       lookup_;
    CancelCheck    cancelCheck_;
    std::mutex     queueMtx_;
    std::queue<CompileCommand> pending_;
    std::mutex     dispatchMtx_;  ///< serializes ChucK operations
    std::thread    dispatchThread_;
    std::atomic<bool> running_{true};
    std::condition_variable cv_;
};

} // namespace hathor::audio_worker
