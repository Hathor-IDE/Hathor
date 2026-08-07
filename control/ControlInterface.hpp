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
#include <string>
#include <string_view>

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

private:
    // --- Command handlers ---------------------------------------------------

    /// set-pattern <slot> <notation>  (Req 11.5, 13.1–13.4)
    void handleSetPattern(std::string_view slot, std::string_view notation);

    /// clear-pattern <slot>  (Req 15.2, 15.3)
    void handleClearPattern(std::string_view slot);

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
