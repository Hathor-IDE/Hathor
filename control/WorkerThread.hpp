// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * WorkerThread.hpp — background compilation thread for mini-notation patterns.
 *
 * Accepts CompileJob objects via enqueue() (non-blocking) and processes them
 * sequentially on a dedicated thread:
 *   1. parseMini(notation)
 *   2. On ParseError: call onComplete with error JSON.
 *   3. On success: lower to Pattern<ParamMap>, allocate SlotState, store via
 *      audio.storeSlot(), call onComplete with success JSON.
 *
 * Requirements: 11.5, 13.1–13.4
 */

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

// AudioEngineFacade is defined in app/AudioEngineFacade.hpp (JUCE-free).
// Forward-declaring it here avoids pulling JUCE into this header's consumers.
class AudioEngineFacade;

namespace hathor::control {

/// A single pending compilation job.
struct CompileJob {
    std::string slotName;  ///< Destination slot name
    std::string notation;  ///< Raw mini-notation string

    /// Optional per-job response callback (used by UI eval path — Req 23.7).
    /// If set, this callback is invoked instead of the WorkerThread's global
    /// onComplete_. The callback is invoked on the worker thread.
    std::function<void(nlohmann::json)> onComplete;
};

/**
 * WorkerThread — serialised background pattern-compilation queue.
 *
 * Thread model:
 *   - enqueue()     : called on the main thread (non-blocking, O(1))
 *   - worker loop   : runs on the dedicated thread_
 *   - onComplete    : called on the worker thread; writes to stdout via
 *                     the mutex-protected respond() in Commands.hpp
 *
 * Destructor joins the worker thread cleanly.
 */
class WorkerThread {
public:
    /**
     * Construct and start the worker thread.
     *
     * @param audio       AudioEngine used to register slots and store states.
     * @param onComplete  Callback invoked after each job with a JSON result.
     *                    Must be thread-safe (called from worker thread).
     */
    explicit WorkerThread(AudioEngineFacade& audio,
                          std::function<void(nlohmann::json)> onComplete);

    /// Destructor — signals shutdown and joins the worker thread.
    ~WorkerThread();

    // Non-copyable, non-movable.
    WorkerThread(const WorkerThread&)            = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;
    WorkerThread(WorkerThread&&)                 = delete;
    WorkerThread& operator=(WorkerThread&&)      = delete;

    /**
     * Enqueue a compilation job.
     *
     * Non-blocking: pushes the job onto the internal queue and notifies the
     * worker thread.  Returns immediately.
     *
     * Requirement: 13.3 (non-blocking enqueue)
     */
    void enqueue(CompileJob job);

private:
    /// The worker loop — runs on thread_.
    void workerLoop();

    AudioEngineFacade&                       audio_;
    std::function<void(nlohmann::json)>  onComplete_;

    std::mutex              queueMutex_;
    std::condition_variable cv_;
    std::queue<CompileJob>  jobs_;
    bool                    shutdown_{false};

    std::thread thread_;
};

} // namespace hathor::control
