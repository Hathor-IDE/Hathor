// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * WorkerThread.cpp — background pattern-compilation thread implementation.
 *
 * Requirements: 11.5, 13.1–13.4
 */

#include "WorkerThread.hpp"
#include "Commands.hpp"

// Engine headers
#include "hathor/MiniParser.hpp"
#include "hathor/PatternCompiler.hpp"
#include "hathor/PrettyPrinter.hpp"

// App headers (available when compiled as part of the hathor executable;
// use a path relative to this file (control/) so the include resolves
// even without app/ in hathor-control's include_directories).
#include "../app/AudioEngineFacade.hpp"

#include <memory>
#include <variant>

namespace hathor::control {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WorkerThread::WorkerThread(AudioEngineFacade& audio,
                           std::function<void(nlohmann::json)> onComplete)
    : audio_(audio)
    , onComplete_(std::move(onComplete))
    , thread_(&WorkerThread::workerLoop, this)
{}

WorkerThread::~WorkerThread()
{
    // Signal shutdown and wake the worker thread so it can exit its wait.
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        shutdown_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

// ---------------------------------------------------------------------------
// enqueue() — non-blocking, called from the main thread
// ---------------------------------------------------------------------------

void WorkerThread::enqueue(CompileJob job)
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        jobs_.push(std::move(job));
    }
    cv_.notify_one();
}

// ---------------------------------------------------------------------------
// workerLoop() — runs on the dedicated thread
// ---------------------------------------------------------------------------

void WorkerThread::workerLoop()
{
    for (;;) {
        CompileJob job;

        // Wait for a job or a shutdown signal.
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            cv_.wait(lock, [this] { return !jobs_.empty() || shutdown_; });

            if (shutdown_ && jobs_.empty()) {
                return;
            }

            job = std::move(jobs_.front());
            jobs_.pop();
        }

        // ---------------------------------------------------------------
        // 1. Parse the notation string.
        // ---------------------------------------------------------------
        auto parseResult = hathor::parseMini(job.notation);

        if (std::holds_alternative<hathor::ParseError>(parseResult)) {
            const auto& err = std::get<hathor::ParseError>(parseResult);
            // Report parse error — do not touch audio slots.
            nlohmann::json resp = {
                {"ok",       false},
                {"cmd",      "set-pattern"},
                {"slot",     job.slotName},
                {"error",    err.message},
                {"position", static_cast<int>(err.position)}
            };
            // Use per-job callback if supplied (UI eval path), otherwise global.
            if (job.onComplete)
                job.onComplete(std::move(resp));
            else
                onComplete_(std::move(resp));
            continue;
        }

        // ---------------------------------------------------------------
        // 2. Successful parse — lower to ParamMap pattern.
        // ---------------------------------------------------------------
        auto& compiled = std::get<hathor::CompiledPattern>(parseResult);

        // Canonical notation string for list-patterns (Req 15.1)
        const std::string canonicalNotation = hathor::printMini(compiled);

        // Lower Pattern<std::string> → Pattern<ParamMap>
        auto paramPattern = hathor::lowerToParamMap(compiled.pattern);

        const std::size_t maxEvents = paramPattern.maxEventsPerCycle();

        // ---------------------------------------------------------------
        // 3. Register/find the slot index.
        // ---------------------------------------------------------------
        const int idx = audio_.findOrAddSlot(job.slotName);
        if (idx < 0) {
            // All 16 slots are occupied by other names.
            nlohmann::json resp = {
                {"ok",    false},
                {"cmd",   "set-pattern"},
                {"slot",  job.slotName},
                {"error", "no free slot available (maximum 16 slots reached)"}
            };
            // Use per-job callback if supplied (UI eval path), otherwise global.
            if (job.onComplete)
                job.onComplete(std::move(resp));
            else
                onComplete_(std::move(resp));
            continue;
        }

        // ---------------------------------------------------------------
        // 4. Allocate SlotState and store atomically (release ordering).
        // ---------------------------------------------------------------
        auto slotState = std::make_shared<SlotState>();
        slotState->pattern = std::make_shared<hathor::Pattern<hathor::ParamMap>>(
            std::move(paramPattern));

        // Pre-allocate the event buffer.
        // Event<ParamMap> has no default constructor (Rational requires
        // explicit denominator), so initialise with a dummy zero-arc
        // placeholder.  The audio callback always overwrites before reading.
        {
            const hathor::Rational zero{0, 1};
            const hathor::Arc      zeroArc{zero, zero};
            const hathor::Event<hathor::ParamMap> dummy{zeroArc, zeroArc, {}};
            slotState->eventBuffer.assign(maxEvents, dummy);
        }

        slotState->notation = canonicalNotation;

        audio_.storeSlot(idx, std::move(slotState));

        // ---------------------------------------------------------------
        // 5. Notify caller of success.
        // ---------------------------------------------------------------
        nlohmann::json resp = {
            {"ok",                    true},
            {"cmd",                   "set-pattern"},
            {"slot",                  job.slotName},
            {"event_count_per_cycle", static_cast<int>(maxEvents)}
        };

        // Use per-job callback if supplied (UI eval path), otherwise global.
        if (job.onComplete)
            job.onComplete(std::move(resp));
        else
            onComplete_(std::move(resp));
    }
}

} // namespace hathor::control
