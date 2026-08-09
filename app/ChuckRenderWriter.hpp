// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckRenderWriter.hpp — B8-K2: background ChucK instrument render -> .wav.
 *
 * Renders the active ChucK instrument through the existing B4 worker /
 * shared-memory audio transport (B4-K1 / B4-K2), non-real-time, writing the
 * resulting PCM data into an uncompressed .wav file at a destination path
 * supplied by B8-K1 (Studio or Live Jam).
 *
 * Design summary (B8-K2):
 *   - The render request is issued from the main/JUCE thread (or the control
 *     worker thread) via startRender().  The caller supplies:
 *       * the instrument source (.ck) to compile + activate on the worker
 *       * the render duration (samples, at the worker's sample rate)
 *       * the destination .wav path (already resolved by B8-K1)
 *       * an async completion callback
 *   - startRender() returns immediately with a RenderHandle (job id + state
 *     handle).  All rendering and file I/O happen on a dedicated background
 *     thread — the JUCE message thread is never blocked.
 *   - The background thread drives the worker (via AudioWorkerManager IPC):
 *       1. activate a per-tab VM for this render (or reuse an active one)
 *       2. compile + publish the instrument source (ck_compile with generation)
 *       3. drain audio blocks from the shared-memory ring via the RT-safe
 *          tryReadAudioBlock() path — but since this is the *only* consumer
 *          during a render, there is no contention with the live callback
 *       4. write interleaved PCM to a temporary .wav file
 *       5. atomically rename the temp file to the final destination path
 *   - The render callback on the ChucK thread produces audio into the ring
 *     buffer.  The render writer is the sole consumer during a bake, so it
 *     drains blocks at its own pace (non-real-time).
 *   - Cancellation: the caller may invoke cancel() on the RenderHandle; the
 *     background thread checks a cancellation flag between blocks and, on
 *     cancel/failure, removes the temp file and reports failure.
 *   - The live audio callback is completely unaffected: it reads the same
 *     ring with its own expected generation; the render writer uses the
 *     same generation, and the worker only has one producer.  When a render
 *     is active, the main audio callback will *also* see the ChucK audio
 *     (it's the same ring) — this is acceptable per the V2 architecture
 *     ("ChucK is an instrument workshop, not a permanent residency"): the
 *     baked instrument plays live while being baked, then the VM is shut
 *     down after a successful bake (B8-K3).  The render writer simply drains
 *     the ring faster than the audio callback to capture enough samples.
 *
 * Threading boundaries (B8-K2 §13):
 *     JUCE message / main thread
 *         → startRender() / cancel() / queryState() / request progress
 *     Background render thread  (one per active render)
 *         → drive worker IPC
 *         → drain shared-memory ring (non-RT consumer)
 *         → allocate/render PCM buffers
 *         → write WAV (file I/O allowed — non-RT)
 *     JUCE audio thread
 *         → continues normal live audio
 *         → does NOT perform file I/O
 *         → does NOT wait for render
 *         → does NOT share a mutex with the writer
 *
 * Requirements: B8-K2, B4-K1, B4-K2, B4-K3, B4-K4, B4-K6, B4-K7
 */

#pragma once

#include "audio-worker/AudioWorkerManager.hpp"
#include "AssetTarget.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hathor {

// ---------------------------------------------------------------------------
// Render state — observable by the UI (B8-K2 §8)
// ---------------------------------------------------------------------------

enum class RenderState : std::uint8_t {
    Pending,    ///< request received, not yet started
    Rendering,  ///< background thread actively producing audio
    Writing,    ///< audio captured, writing WAV to disk
    Completed,  ///< WAV successfully written and renamed to final path
    Failed,     ///< error occurred (compile, render, file I/O, etc.)
    Cancelled,  ///< explicitly cancelled by the caller
};

inline const char* toString(RenderState s) noexcept
{
    switch (s) {
        case RenderState::Pending:   return "pending";
        case RenderState::Rendering: return "rendering";
        case RenderState::Writing:   return "writing";
        case RenderState::Completed: return "completed";
        case RenderState::Failed:    return "failed";
        case RenderState::Cancelled: return "cancelled";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Render result — delivered to the completion callback (B8-K2 §8, §10)
// ---------------------------------------------------------------------------

struct RenderResult {
    bool        success            = false;
    RenderState state              = RenderState::Pending;
    std::string  errorMessage;      ///< populated on failure/cancellation
    std::filesystem::path outputPath; ///< final destination path (if success)
    uint64_t    samplesWritten     = 0; ///< total samples written to disk
    double      durationSeconds    = 0.0; ///< actual duration of the file
};

// ---------------------------------------------------------------------------
// RenderHandle — opaque handle to an in-flight or completed render (B8-K2 §8)
// ---------------------------------------------------------------------------

class RenderHandle {
public:
    RenderHandle() = default;
    explicit RenderHandle(uint64_t id,
                            std::shared_ptr<std::atomic<RenderState>> state,
                            std::shared_ptr<std::atomic<bool>> cancelFlag) noexcept
        : id_(id), state_(std::move(state)), cancelFlag_(std::move(cancelFlag)) {}

    /// Unique identifier for this render job.
    uint64_t id() const noexcept { return id_; }

    /// Query the current state (non-blocking, safe from any thread).
    RenderState state() const noexcept
    {
        if (state_)
            return state_->load(std::memory_order_acquire);
        return RenderState::Failed;
    }

    /// Cancel an in-flight render (B8-K2 §12).
    /// Safe to call from any thread.  The background thread polls the flag
    /// between blocks and, on observing it, cleans up the temp file and
    /// reports Failed/Cancelled.  Calling cancel on a completed render is a
    /// no-op.
    void cancel() noexcept
    {
        if (cancelFlag_)
            cancelFlag_->store(true, std::memory_order_release);
        if (state_) {
            RenderState expected = RenderState::Pending;
            state_->compare_exchange_strong(expected, RenderState::Cancelled,
                                           std::memory_order_acq_rel);
            expected = RenderState::Rendering;
            state_->compare_exchange_strong(expected, RenderState::Cancelled,
                                           std::memory_order_acq_rel);
        }
    }

    /// Returns true if the render is still in progress (Pending, Rendering, or Writing).
    bool isDone() const noexcept
    {
        const RenderState s = state();
        return s == RenderState::Completed ||
               s == RenderState::Failed ||
               s == RenderState::Cancelled;
    }

private:
    uint64_t id_ = 0;
    std::shared_ptr<std::atomic<RenderState>> state_;
    std::shared_ptr<std::atomic<bool>>        cancelFlag_;
};

// ---------------------------------------------------------------------------
// ChuckRenderWriter — renders a ChucK instrument to a .wav file (B8-K2)
// ---------------------------------------------------------------------------

/**
 * B8-K2: Background ChucK instrument render -> .wav.
 *
 * Usage:
 *
 *     ChuckRenderWriter writer(&audioWorkerManager);
 *
 *     // Resolve the destination via B8-K1 (AudioEngine::resolveRenderPath):
 *     auto dest = audioEngine.resolveRenderPath(AssetTarget::Studio,
 *                                               "acid_bass", projectDir);
 *
 *     RenderHandle handle = writer.startRender(
 *         /*tabId*/ 0,
 *         /*ckSource*/ "SinOsc s => dac; 2::second => now;",
 *         /*numSamples*/ 44100 * 4,  // 4 seconds @ 44.1 kHz
 *         /*sampleRate*/ 44100,
 *         /*destPath*/ dest,
 *         /*onComplete*/ [](const RenderResult& r) { ... });
 *
 *     // UI can poll handle.state() or wait for the callback.
 */
class ChuckRenderWriter {
public:
    using CompletionCallback = std::function<void(const RenderResult&)>;

    /**
     * Construct a render writer bound to an AudioWorkerManager.
     *
     * @param worker  The AudioWorkerManager that owns the hathor-audio-worker
     *                process.  Must be started (start() called and the worker
     *                is alive) before startRender().  The writer does NOT own
     *                or manage the worker lifecycle — it is a non-real-time
     *                consumer of the worker's shared-memory audio ring.
     */
    explicit ChuckRenderWriter(AudioWorkerManager* worker) noexcept;
    ~ChuckRenderWriter();

    ChuckRenderWriter(const ChuckRenderWriter&)            = delete;
    ChuckRenderWriter& operator=(const ChuckRenderWriter&) = delete;

    // -----------------------------------------------------------------------
    // B8-K2 §1: Background render entry point
    // -----------------------------------------------------------------------

    /**
     * Start a background render of the given ChucK instrument.
     *
     * This call returns immediately (does not block).  All rendering and
     * file writing happen on a dedicated background thread.
     *
     * @param tabId       The tab/slot index [0,16) to use for the render VM.
     *                    The writer activates (or recreates) a per-tab VM
     *                    for this render, compiles the source, drains audio
     *                    from the ring, then deactivates the VM.
     * @param ckSource    The ChucK source code to render (e.g. "SinOsc s => dac; ...").
     * @param numSamples  Total number of samples to render (at @p sampleRate).
     *                    The writer produces exactly this many samples, subject
     *                    to block-alignment rounding (see RenderResult::samplesWritten).
     * @param sampleRate  Sample rate for the render (should match the worker's
     *                    rate; the writer uses 44100 by default).
     * @param destPath    Absolute path to the final .wav destination (resolved by B8-K1).
     * @param onComplete  Called on the background thread when the render completes,
     *                    fails, or is cancelled.  Must be thread-safe and non-blocking.
     *
     * @return A RenderHandle for status polling / cancellation.
     */
    RenderHandle startRender(uint8_t                         tabId,
                             std::string                     ckSource,
                             uint64_t                        numSamples,
                             unsigned                        sampleRate,
                             std::filesystem::path           destPath,
                             CompletionCallback              onComplete);

    // -----------------------------------------------------------------------
    // B8-K2 §8: Background-task lifecycle
    // -----------------------------------------------------------------------

    /// Number of renders currently in progress.
    int activeRenderCount() const noexcept;

    /// Shut down: signals all in-flight renders to cancel and waits for their
    /// background threads to finish.  Safe to call from the main thread at
    /// application shutdown.
    void shutdown() noexcept;

private:
    struct RenderJob;

    AudioWorkerManager* worker_;

    // Active render jobs (protected by jobsMtx_).
    // Each job is held via shared_ptr so both the tracking list and the
    // background thread can access it without ownership races.
    std::mutex jobsMtx_;
    std::vector<std::shared_ptr<RenderJob>> jobs_;

    // Monotonic job-id counter.
    std::atomic<uint64_t> nextJobId_{1};

    // Background thread entry point — runs entirely on the render thread.
    void runRender(std::shared_ptr<RenderJob> job);
};

} // namespace hathor
