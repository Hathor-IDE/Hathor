// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_thread_state.cpp — Unit tests for ThreadConnState state machine (C2).
 *
 * Tests the per-thread connection state machine that replaces the global
 * `bool disconnected_` from the original ChatSidebar. Verifies:
 *
 *   - Test 1: Single thread disconnect → reconnect state transitions
 *   - Test 2: Multiple threads have independent state
 *   - Test 3: Duplicate reconnect attempts are prevented
 *   - Test 4: Reconnect failure preserves the error
 *   - Test 5: Repeated disconnect/reconnect cycles remain stable
 *   - Test 6: Background thread state persists across tab switches
 *
 * This test file is JUCE-free — it only tests the pure state machine
 * in ThreadConnState.hpp, which ChatThread delegates to (C2 §7).
 *
 * Requirements: B6, C2 (§2, §4, §5, §6, §7, §8, §11)
 */

#include <catch2/catch_test_macros.hpp>

#include "ThreadConnState.hpp"

#include <string>

using namespace hathor::ui;

// ---------------------------------------------------------------------------
// Test 1 — Single thread: disconnect → reconnect → connected
// (C2 Test 1 — single thread)
// ---------------------------------------------------------------------------

TEST_CASE("ThreadConnState: single thread disconnect → reconnect → connected", "[c2][thread-state]")
{
    ThreadConnStateMachine sm;

    // Initial state: connected.
    REQUIRE(sm.isConnected());
    REQUIRE_FALSE(sm.isDisconnected());
    REQUIRE_FALSE(sm.isReconnecting());

    // Disconnect (subprocess exited).
    sm.onDisconnect();
    REQUIRE(sm.isDisconnected());
    REQUIRE_FALSE(sm.isConnected());
    REQUIRE_FALSE(sm.isReconnecting());
    REQUIRE(sm.lastError.empty());

    // Begin reconnect (user clicks "Reconnect").
    bool shouldProceed = sm.beginReconnect();
    REQUIRE(shouldProceed);
    REQUIRE(sm.isReconnecting());
    REQUIRE_FALSE(sm.isConnected());
    REQUIRE_FALSE(sm.isDisconnected());
    REQUIRE(sm.lastError.empty());

    // Reconnect succeeds.
    sm.onReconnectSuccess();
    REQUIRE(sm.isConnected());
    REQUIRE_FALSE(sm.isDisconnected());
    REQUIRE_FALSE(sm.isReconnecting());
    REQUIRE(sm.lastError.empty());
}

// ---------------------------------------------------------------------------
// Test 2 — Multiple threads: independent state (C2 §2, §7)
// (C2 Test 2 — multiple threads)
// ---------------------------------------------------------------------------

TEST_CASE("ThreadConnState: multiple threads have independent state", "[c2][thread-state][isolation]")
{
    ThreadConnStateMachine threadA;  // Connected
    ThreadConnStateMachine threadB;  // Connected

    // Both start connected.
    REQUIRE(threadA.isConnected());
    REQUIRE(threadB.isConnected());

    // Thread A disconnects; Thread B stays connected.
    threadA.onDisconnect();
    REQUIRE(threadA.isDisconnected());
    REQUIRE(threadB.isConnected());

    // Thread A begins reconnect.
    REQUIRE(threadA.beginReconnect());
    REQUIRE(threadA.isReconnecting());
    REQUIRE(threadB.isConnected());

    // Thread A reconnect succeeds.
    threadA.onReconnectSuccess();
    REQUIRE(threadA.isConnected());
    REQUIRE(threadB.isConnected());

    // Thread B disconnects; Thread A stays connected.
    threadB.onDisconnect();
    REQUIRE(threadB.isDisconnected());
    REQUIRE(threadA.isConnected());

    // Thread B reconnects independently.
    REQUIRE(threadB.beginReconnect());
    REQUIRE(threadB.isReconnecting());
    REQUIRE(threadA.isConnected());

    threadB.onReconnectSuccess();
    REQUIRE(threadB.isConnected());
    REQUIRE(threadA.isConnected());
}

// ---------------------------------------------------------------------------
// Test 3 — Duplicate reconnect attempts are prevented (C2 §4.4, §11)
// (C2 Test 5 — duplicate clicks)
// ---------------------------------------------------------------------------

TEST_CASE("ThreadConnState: duplicate reconnect while reconnecting is blocked", "[c2][thread-state][duplicate]")
{
    ThreadConnStateMachine sm;

    // Disconnect first.
    sm.onDisconnect();
    REQUIRE(sm.isDisconnected());

    // First reconnect request — should succeed.
    REQUIRE(sm.beginReconnect());
    REQUIRE(sm.isReconnecting());

    // Second reconnect request while already reconnecting — should be blocked.
    REQUIRE_FALSE(sm.beginReconnect());
    REQUIRE(sm.isReconnecting());

    // Third reconnect request — still blocked.
    REQUIRE_FALSE(sm.beginReconnect());
    REQUIRE(sm.isReconnecting());

    // After success, a new reconnect cycle should work.
    sm.onReconnectSuccess();
    REQUIRE(sm.isConnected());

    // Disconnect again and reconnect.
    sm.onDisconnect();
    REQUIRE(sm.isDisconnected());
    REQUIRE(sm.beginReconnect());
    REQUIRE(sm.isReconnecting());
}

// ---------------------------------------------------------------------------
// Test 4 — Reconnect failure preserves error (C2 §6)
// (C2 Test 4 — reconnect failure)
// ---------------------------------------------------------------------------

TEST_CASE("ThreadConnState: reconnect failure preserves error", "[c2][thread-state][failure]")
{
    ThreadConnStateMachine sm;

    // Disconnect.
    sm.onDisconnect();
    REQUIRE(sm.isDisconnected());
    REQUIRE(sm.lastError.empty());

    // Begin reconnect.
    REQUIRE(sm.beginReconnect());
    REQUIRE(sm.isReconnecting());

    // Reconnect fails with a specific error.
    sm.onReconnectFailure("Agent subprocess crashed during init");
    REQUIRE(sm.isDisconnected());
    REQUIRE_FALSE(sm.isConnected());
    REQUIRE_FALSE(sm.isReconnecting());
    REQUIRE(sm.lastError == "Agent subprocess crashed during init");

    // The error is preserved until a successful reconnect or a new disconnect.
    REQUIRE_FALSE(sm.lastError.empty());

    // Retry: begin reconnect again.
    REQUIRE(sm.beginReconnect());
    REQUIRE(sm.lastError.empty());  // Error cleared on new attempt.
    REQUIRE(sm.isReconnecting());

    // This time succeed.
    sm.onReconnectSuccess();
    REQUIRE(sm.isConnected());
    REQUIRE(sm.lastError.empty());
}

// ---------------------------------------------------------------------------
// Test 5 — Repeated disconnect/reconnect cycles (C2 §11)
// (C2 Test 6 — repeated lifecycle)
// ---------------------------------------------------------------------------

TEST_CASE("ThreadConnState: repeated connect/disconnect/reconnect cycles", "[c2][thread-state][repeat]")
{
    ThreadConnStateMachine sm;

    for (int cycle = 0; cycle < 5; ++cycle)
    {
        INFO("cycle " << cycle);

        // Start connected.
        REQUIRE(sm.isConnected());

        // Disconnect.
        sm.onDisconnect();
        REQUIRE(sm.isDisconnected());

        // Reconnect.
        REQUIRE(sm.beginReconnect());
        REQUIRE(sm.isReconnecting());

        // Success.
        sm.onReconnectSuccess();
        REQUIRE(sm.isConnected());
    }
}

// ---------------------------------------------------------------------------
// Test 6 — Background thread state persists (C2 §8)
//
// This test models the scenario where Thread A disconnects while the user
// has switched to Thread B. Thread A's state must remain Disconnected
// until the user returns to it and reconnects.
// ---------------------------------------------------------------------------

TEST_CASE("ThreadConnState: background thread state persists across switches", "[c2][thread-state][background]")
{
    ThreadConnStateMachine threadA;
    ThreadConnStateMachine threadB;

    // Simulate: user is on Thread A. Thread A disconnects.
    threadA.onDisconnect();
    REQUIRE(threadA.isDisconnected());

    // User switches to Thread B — Thread A's state is preserved.
    REQUIRE(threadA.isDisconnected());
    REQUIRE(threadB.isConnected());

    // User returns to Thread A — state is still disconnected.
    REQUIRE(threadA.isDisconnected());

    // User reconnects Thread A.
    REQUIRE(threadA.beginReconnect());
    REQUIRE(threadA.isReconnecting());

    // Thread B was never affected.
    REQUIRE(threadB.isConnected());
    REQUIRE_FALSE(threadB.isDisconnected());
    REQUIRE_FALSE(threadB.isReconnecting());
}

// ---------------------------------------------------------------------------
// Test 7 — Reconnect only transitions from Disconnected (not Connected)
// ---------------------------------------------------------------------------

TEST_CASE("ThreadConnState: cannot begin reconnect from Connected state", "[c2][thread-state][guard]")
{
    ThreadConnStateMachine sm;

    REQUIRE(sm.isConnected());

    // Attempt to reconnect while connected — should be ignored.
    bool shouldProceed = sm.beginReconnect();
    REQUIRE_FALSE(shouldProceed);

    // State unchanged.
    REQUIRE(sm.isConnected());
    REQUIRE_FALSE(sm.isReconnecting());
}

// ---------------------------------------------------------------------------
// Test 8 — Reconnecting → failure → reconnect again (C2 §6, §11)
// ---------------------------------------------------------------------------

TEST_CASE("ThreadConnState: reconnect failure then retry succeeds", "[c2][thread-state][retry]")
{
    ThreadConnStateMachine sm;

    // disconnect → reconnect → fail → reconnect → success
    sm.onDisconnect();
    REQUIRE(sm.isDisconnected());

    REQUIRE(sm.beginReconnect());
    REQUIRE(sm.isReconnecting());

    sm.onReconnectFailure("Connection refused");
    REQUIRE(sm.isDisconnected());
    REQUIRE(sm.lastError == "Connection refused");

    // Retry — should work.
    REQUIRE(sm.beginReconnect());
    REQUIRE(sm.isReconnecting());

    sm.onReconnectSuccess();
    REQUIRE(sm.isConnected());
    REQUIRE(sm.lastError.empty());
}
