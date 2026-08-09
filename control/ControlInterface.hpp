// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ControlInterface.hpp — command dispatch and stdin reader loop.
 *
 * ControlInterface reads newline-delimited commands from stdin, dispatches
 * them to the appropriate handlers, and writes JSON responses to stdout via
 * the thread-safe respond() function in Commands.hpp.
 *
 * Requirements: 12.1–12.5, 13.5, 14.1–14.6, 15.1–15.3, 16.2, 16.5
 */

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// Forward declarations — full headers are only needed in the .cpp.
class AudioEngineFacade;
class SampleBank;

namespace hathor::control {

/**
 * ControlInterface — owns the WorkerThread and processes ACP commands.
 *
 * Lifecycle:
 *   1. Construct with AudioEngine& and SampleBank& references.
 *   2. Call run() which blocks until EOF on stdin or a quit command.
 */
class ControlInterface {
public:
    explicit ControlInterface(AudioEngineFacade& audio, SampleBank& bank);
    ~ControlInterface();

    // Non-copyable, non-movable.
    ControlInterface(const ControlInterface&)            = delete;
    ControlInterface& operator=(const ControlInterface&) = delete;
    ControlInterface(ControlInterface&&)                 = delete;
    ControlInterface& operator=(ControlInterface&&)      = delete;

    /**
     * Blocking stdin reader loop.
     *
     * Reads one line at a time; dispatches each to dispatch().
     * Exits via std::exit(0) on EOF or after a "quit" command.
     *
     * Requirement: 12.1, 16.2
     */
    void run();

    /**
     * Parse and dispatch a single command line.
     *
     * Trims leading/trailing whitespace, splits on the first whitespace
     * token to extract the command name, then routes to the appropriate
     * handle*() method.  Unknown commands produce an error JSON response.
     *
     * Requirement: 12.4, 12.5
     */
    void dispatch(std::string_view line);

    /**
     * Enqueue a `set-pattern` job directly on the worker thread with a
     * per-job response callback.  Used by the UI eval path (Req 23.7) so
     * the result can be marshalled to the JUCE message thread without going
     * through stdout.
     *
     * @param slotName   Destination slot name (e.g. "d0").
     * @param notation   Raw mini-notation string.
     * @param onComplete Callback invoked on the worker thread with the JSON
     *                   result.  Must marshal to the message thread if it
     *                   touches JUCE components.
     *
     * Requirement: 23.7
     */
    void enqueueSetPattern(const std::string& slotName,
                           const std::string& notation,
                           std::function<void(nlohmann::json)> onComplete);

    /**
      * UI-facing dispatch: same routing as dispatch(), but instead of writing
      * the JSON response to stdout, the response is delivered to @p onResult
      * called on the worker thread (for async commands like set-pattern) or
      * on the calling thread (for synchronous commands like bpm, set-gain).
      *
      * MUST be called on a worker thread, never on the JUCE message thread
      * (Req 23.7).
      *
      * @param line      The command line (e.g. "set-pattern d0 bd sn").
      * @param onResult  Callback invoked with the JSON response.
      *                  For set-pattern, called on the WorkerThread after
      *                  pattern compilation completes. For other commands,
      *                  called synchronously before this function returns.
      *
      * Requirements: 23.1, 23.3, 23.7
      */
    void dispatchWithCallback(std::string_view line,
                               std::function<void(nlohmann::json)> onResult);

    /**
     * Dispatch a per-slot play/stop command synchronously.
     *
     * Convenience wrapper around dispatchWithCallback for B1 (per-tab Play/Stop).
     * Issues "slot-play <slotName>" or "slot-stop <slotName>" and calls onResult
     * with the JSON response.  For synchronous slot commands the callback fires
     * before this function returns.
     *
     * MUST be called on a worker thread (not the JUCE message thread).
     *
     * @param slotName  Slot name string (e.g. "d0", "d1").
     * @param start     true = slot-play, false = slot-stop.
     * @param onResult  Callback invoked with the JSON response.
     */
    void dispatchSlotPlayStop(const std::string& slotName,
                              bool start,
                              std::function<void(nlohmann::json)> onResult);

private:
    // --- Command handlers ---------------------------------------------------

    /// set-pattern <slot> <notation>  (Req 11.5, 13.1–13.4)
    void handleSetPattern(std::string_view slot, std::string_view notation);

    /// clear-pattern <slot>  (Req 15.2, 15.3)
    void handleClearPattern(std::string_view slot);

    /// slot-play <slot> / slot-stop <slot>  (A3)
    void handleSlotPlayStop(std::string_view slot, bool start);

    /// list-patterns  (Req 15.1)
    void handleListPatterns();

    /// bpm <value>  (Req 14.3, 14.4)
    void handleBpm(std::string_view arg);

    /// play  (Req 14.1)
    void handlePlay();

    /// stop  (Req 14.2)
    void handleStop();

    /// ping  (Req 14.6)
    void handlePing(std::chrono::steady_clock::time_point receiveTime);

    /// set-gain <value>  (Req 26.7, 26.8)
    void handleSetGain(std::string_view arg);

    /// set-eq-preset <preset>  (B7-K2)
    /// Preset names: flat, bass-boost, vocal, bright
    void handleSetEqPreset(std::string_view arg);

    /// quit  (Req 16.5)
    void handleQuit();

    // --- Members ------------------------------------------------------------
    AudioEngineFacade& audio_;
    SampleBank&  bank_;

    // WorkerThread is allocated on the heap to keep this header JUCE-free.
    // (WorkerThread.hpp only forward-declares AudioEngine, so it is safe.)
    struct Impl;
    Impl* impl_{nullptr};
};

} // namespace hathor::control
