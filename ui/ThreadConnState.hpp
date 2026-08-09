// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ThreadConnState.hpp — Thread-scoped connection state machine for chat threads.
 *
 * Per C2 §7, each chat thread has its own connection state rather than
 * a single global boolean. This header defines the state enum and a
 * pure (JUCE-free) state machine that governs transitions:
 *
 *   Connected → Disconnected (on subprocess exit)
 *   Disconnected → Reconnecting (on reconnect request)
 *   Reconnecting → Connected (on restart success)
 *   Reconnecting → Disconnected (on restart failure — preserves error)
 *
 * The state machine logic is extracted into a JUCE-free struct so it can
 * be unit-tested without a JUCE message loop (see tests-ui/test_thread_state.cpp).
 *
 * Requirements: B6, C2 (§2, §5, §6, §7, §11)
 */

#include <string>

namespace hathor::ui {

/**
 * Thread-scoped connection state (C2 §7).
 *
 * Replaces the global `bool disconnected_` from the original ChatSidebar.
 */
enum class ThreadConnState
{
    Connected,      ///< Agent subprocess running and session ready
    Disconnected,   ///< Agent subprocess exited; reconnect available
    Reconnecting,   ///< restart() in progress (non-blocking, async)
};

/**
 * Pure state machine for a single thread's connection lifecycle.
 *
 * No JUCE dependencies — can be tested without a message loop.
 * Thread safety: not thread-safe by design; all methods are called
 * from the JUCE message thread.
 *
 * State transitions (C2 §5):
 *
 *   Connected ──(disconnect)──▶ Disconnected
 *      │                           │
 *      │                           └──(reconnect)──▶ Reconnecting ──(success)──▶ Connected
 *      │                                                                 │
 *      └───────────────────────────────────────────────────────────────────┘
 *
 *   Reconnecting ─(failure)──▶ Disconnected (with error preserved)
 *
 * Duplicate reconnect while Reconnecting is a no-op (C2 §4.4, §11).
 */
struct ThreadConnStateMachine
{
    ThreadConnState state = ThreadConnState::Connected;
    std::string    lastError;  ///< Preserved from failed restart (C2 §6)

    /**
     * Handle a disconnect event (subprocess exited).
     * Only transitions if currently Connected.
     */
    void onDisconnect()
    {
        state = ThreadConnState::Disconnected;
        lastError.clear();
    }

    /**
     * Begin a reconnect attempt.
     * Returns true if the reconnect should proceed (state was not already
     * Reconnecting). Returns false if a reconnect is already in progress
     * (C2 §4.4 — prevents duplicate restart attempts).
     */
    bool beginReconnect()
    {
        if (state == ThreadConnState::Reconnecting)
            return false;  // Prevent duplicate reconnect (C2 §4.4, §11)

        if (state == ThreadConnState::Connected)
            return false;

        // Transition to Reconnecting.
        state = ThreadConnState::Reconnecting;
        lastError.clear();
        return true;
    }

    /**
     * Handle a successful reconnection.
     * Transitions from Reconnecting → Connected.
     */
    void onReconnectSuccess()
    {
        state = ThreadConnState::Connected;
        lastError.clear();
    }

    /**
     * Handle a failed reconnection.
     * Transitions from Reconnecting → Disconnected and preserves the error.
     *
     * @param error  The error message from the failed restart path.
     */
    void onReconnectFailure(const std::string& error)
    {
        state = ThreadConnState::Disconnected;
        lastError = error;  // Preserve the error (C2 §6)
    }

    /**
     * Returns true if a reconnect is currently in progress.
     */
    bool isReconnecting() const noexcept
    {
        return state == ThreadConnState::Reconnecting;
    }

    /**
     * Returns true if the thread is disconnected and a reconnect is available.
     */
    bool isDisconnected() const noexcept
    {
        return state == ThreadConnState::Disconnected;
    }

    /**
     * Returns true if the thread is connected.
     */
    bool isConnected() const noexcept
    {
        return state == ThreadConnState::Connected;
    }
};

} // namespace hathor::ui
