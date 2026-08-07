// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ControlInterface.cpp — command dispatch and stdin reader loop.
 *
 * Requirements: 12.1–12.5, 13.5, 14.1–14.6, 15.1–15.3, 16.2, 16.5
 */

#include "ControlInterface.hpp"
#include "Commands.hpp"
#include "WorkerThread.hpp"

// App headers (available when compiled as part of the hathor executable;
// use paths relative to this file (control/) so the includes resolve
// even without app/ in hathor-control's include_directories).
#include "../app/AudioEngineFacade.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace hathor::control {

// ---------------------------------------------------------------------------
// Global mutex that serialises all stdout writes (defined here, declared in
// Commands.hpp so WorkerThread.cpp can acquire it too).
// ---------------------------------------------------------------------------
std::mutex g_stdoutMutex;

// ---------------------------------------------------------------------------
// Impl — holds the WorkerThread so it stays out of the public header.
// ---------------------------------------------------------------------------
struct ControlInterface::Impl {
    WorkerThread worker;

    explicit Impl(AudioEngineFacade& audio)
        : worker(audio, [](nlohmann::json j) { respond(j); })
    {}
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ControlInterface::ControlInterface(AudioEngineFacade& audio, SampleBank& bank)
    : audio_(audio)
    , bank_(bank)
    , impl_(new Impl(audio))
{}

ControlInterface::~ControlInterface()
{
    delete impl_;
}

// ---------------------------------------------------------------------------
// Helpers: whitespace trimming and token splitting
// ---------------------------------------------------------------------------

namespace {

/// Returns true if c is ASCII whitespace.
inline bool isWS(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// Trim leading and trailing whitespace from sv.
std::string_view trim(std::string_view sv) noexcept
{
    while (!sv.empty() && isWS(sv.front())) sv.remove_prefix(1);
    while (!sv.empty() && isWS(sv.back()))  sv.remove_suffix(1);
    return sv;
}

/**
 * Split sv on the first run of whitespace.
 *
 * Returns:
 *   first  — the token before the whitespace
 *   second — everything after the whitespace run (may be empty)
 */
std::pair<std::string_view, std::string_view>
splitFirst(std::string_view sv) noexcept
{
    // Find end of first token
    std::size_t i = 0;
    while (i < sv.size() && !isWS(sv[i])) ++i;
    std::string_view first = sv.substr(0, i);

    // Skip whitespace run
    while (i < sv.size() && isWS(sv[i])) ++i;
    std::string_view rest = sv.substr(i);

    return {first, rest};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// dispatch() — O(n) command routing
// ---------------------------------------------------------------------------

void ControlInterface::dispatch(std::string_view rawLine)
{
    // Record the time as soon as we start processing the line so that ping
    // can report accurate end-to-end latency (Req 14.6).
    const auto receiveTime = std::chrono::steady_clock::now();

    std::string_view line = trim(rawLine);
    if (line.empty()) return;

    auto [cmd, rest] = splitFirst(line);

    if (cmd == "ping") {
        handlePing(receiveTime);
    } else if (cmd == "play") {
        handlePlay();
    } else if (cmd == "stop") {
        handleStop();
    } else if (cmd == "quit") {
        handleQuit();
    } else if (cmd == "list-patterns") {
        handleListPatterns();
    } else if (cmd == "bpm") {
        handleBpm(trim(rest));
    } else if (cmd == "set-gain") {
        handleSetGain(trim(rest));
    } else if (cmd == "clear-pattern") {
        handleClearPattern(trim(rest));
    } else if (cmd == "set-pattern") {
        // split rest into slot + notation (everything after second ws token)
        auto [slot, notation] = splitFirst(rest);
        handleSetPattern(slot, notation);
    } else {
        respond({
            {"ok",    false},
            {"error", "unknown command"},
            {"cmd",   std::string(cmd)}
        });
    }
}

// ---------------------------------------------------------------------------
// enqueueSetPattern() — UI eval path (Req 23.7)
// ---------------------------------------------------------------------------

void ControlInterface::enqueueSetPattern(
    const std::string& slotName,
    const std::string& notation,
    std::function<void(nlohmann::json)> onComplete)
{
    impl_->worker.enqueue(CompileJob{slotName, notation, std::move(onComplete)});
}

// ---------------------------------------------------------------------------
// run() — blocking stdin reader loop (Req 12.1, 16.2)
// ---------------------------------------------------------------------------

void ControlInterface::run()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        dispatch(line);
    }
    // EOF on stdin — clean shutdown (Req 16.2)
    std::exit(0);
}

// ---------------------------------------------------------------------------
// handlePing() — Req 14.6
// ---------------------------------------------------------------------------

void ControlInterface::handlePing(std::chrono::steady_clock::time_point receiveTime)
{
    const auto now = std::chrono::steady_clock::now();
    const double latencyMs =
        std::chrono::duration<double, std::milli>(now - receiveTime).count();

    respond({
        {"ok",         true},
        {"cmd",        "ping"},
        {"latency_ms", latencyMs}
    });
}

// ---------------------------------------------------------------------------
// handlePlay() / handleStop() — Req 14.1, 14.2
// ---------------------------------------------------------------------------

void ControlInterface::handlePlay()
{
    audio_.play();
    respond({{"ok", true}, {"cmd", "play"}});
}

void ControlInterface::handleStop()
{
    audio_.stop();
    respond({{"ok", true}, {"cmd", "stop"}});
}

// ---------------------------------------------------------------------------
// handleBpm() — Req 14.3, 14.4
// ---------------------------------------------------------------------------

void ControlInterface::handleBpm(std::string_view arg)
{
    if (arg.empty()) {
        respond({
            {"ok",    false},
            {"cmd",   "bpm"},
            {"error", "missing BPM argument"}
        });
        return;
    }

    // Parse as double — use std::stod via a temporary std::string.
    double bpm = 0.0;
    try {
        std::size_t pos = 0;
        bpm = std::stod(std::string(arg), &pos);
        if (pos != arg.size()) {
            throw std::invalid_argument("trailing characters");
        }
    } catch (...) {
        respond({
            {"ok",    false},
            {"cmd",   "bpm"},
            {"error", "invalid BPM value — expected a number"}
        });
        return;
    }

    if (bpm < 20.0 || bpm > 400.0) {
        respond({
            {"ok",    false},
            {"cmd",   "bpm"},
            {"error", "BPM out of range [20, 400]"},
            {"value", bpm}
        });
        return;
    }

    audio_.setBpm(bpm);
    respond({
        {"ok",  true},
        {"cmd", "bpm"},
        {"bpm", bpm}
    });
}

// ---------------------------------------------------------------------------
// handleSetGain() — Req 26.7, 26.8
// ---------------------------------------------------------------------------

void ControlInterface::handleSetGain(std::string_view arg)
{
    if (arg.empty()) {
        respond({
            {"ok",    false},
            {"cmd",   "set-gain"},
            {"error", "missing gain argument"}
        });
        return;
    }

    // Parse as float. std::from_chars for floating-point is not available on
    // Apple Clang libc++ (only integers); use std::stof instead.
    float val = 0.f;
    try {
        std::size_t pos = 0;
        val = std::stof(std::string(arg), &pos);
        if (pos != arg.size()) {
            throw std::invalid_argument("trailing characters");
        }
    } catch (...) {
        respond({
            {"ok",    false},
            {"cmd",   "set-gain"},
            {"error", "invalid value"}
        });
        return;
    }

    // Clamp to [0.0, 2.0] — out-of-range values are clamped, not rejected (Req 26.8).
    const float clamped = std::clamp(val, 0.f, 2.f);
    audio_.setMasterGain(clamped);

    respond({
        {"ok",   true},
        {"cmd",  "set-gain"},
        {"gain", clamped}
    });
}

// ---------------------------------------------------------------------------
// handleSetPattern() — Req 11.5, 13.1–13.4
// ---------------------------------------------------------------------------

void ControlInterface::handleSetPattern(std::string_view slot,
                                         std::string_view notation)
{
    if (slot.empty()) {
        respond({
            {"ok",    false},
            {"cmd",   "set-pattern"},
            {"error", "missing slot name"}
        });
        return;
    }

    if (notation.empty()) {
        respond({
            {"ok",    false},
            {"cmd",   "set-pattern"},
            {"slot",  std::string(slot)},
            {"error", "missing notation string"}
        });
        return;
    }

    // Enqueue on the worker thread (non-blocking) — Req 11.5
    impl_->worker.enqueue({std::string(slot), std::string(notation)});
    // Response will be sent asynchronously by the worker's onComplete callback.
}

// ---------------------------------------------------------------------------
// handleClearPattern() — Req 15.2, 15.3
// ---------------------------------------------------------------------------

void ControlInterface::handleClearPattern(std::string_view slotSV)
{
    if (slotSV.empty()) {
        respond({
            {"ok",    false},
            {"cmd",   "clear-pattern"},
            {"error", "missing slot name"}
        });
        return;
    }

    const std::string slotName(slotSV);
    const int idx = audio_.findOrAddSlot(slotName);

    // findOrAddSlot returns -1 if the table is full AND the slot is unknown.
    // But if the slot was never registered, it would have been newly added
    // (consuming a slot entry).  The spec says: error if slot doesn't exist.
    //
    // Strategy: if idx < 0 (table full, name unknown) → not found.
    // If idx >= 0 but loadSlot(idx) == nullptr → slot was cleared or never set.
    if (idx < 0 || audio_.loadSlot(idx) == nullptr) {
        respond({
            {"ok",    false},
            {"cmd",   "clear-pattern"},
            {"slot",  slotName},
            {"error", "slot does not exist or is empty"}
        });
        return;
    }

    audio_.clearSlot(idx);
    respond({
        {"ok",   true},
        {"cmd",  "clear-pattern"},
        {"slot", slotName}
    });
}

// ---------------------------------------------------------------------------
// handleListPatterns() — Req 15.1
// ---------------------------------------------------------------------------

void ControlInterface::handleListPatterns()
{
    nlohmann::json patterns = nlohmann::json::array();

    const int count = audio_.slotCount();
    for (int i = 0; i < count; ++i) {
        auto state = audio_.loadSlot(i);
        if (!state) continue; // slot was cleared or never set

        nlohmann::json entry = {
            {"slot",        audio_.slotName(i)},
            {"notation",    state->notation},
            {"event_count", static_cast<int>(state->eventBuffer.size())}
        };
        patterns.push_back(std::move(entry));
    }

    respond({
        {"ok",       true},
        {"cmd",      "list-patterns"},
        {"patterns", std::move(patterns)}
    });
}

// ---------------------------------------------------------------------------
// handleQuit() — Req 16.5
// ---------------------------------------------------------------------------

void ControlInterface::handleQuit()
{
    respond({{"ok", true}, {"cmd", "quit"}});
    // Flush is handled inside respond(), but call it again to be safe.
    std::fflush(stdout);
    std::exit(0);
}

} // namespace hathor::control
