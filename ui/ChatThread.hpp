// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ChatThread.hpp — One chat thread: owns its AcpAgentSession + UI state.
 *
 * Per decision #3 (PROGRAM.md §2), each chat thread gets its own subprocess
 * (AcpAgentSession). This class encapsulates the per-thread lifecycle:
 *
 *   - MessageHistoryView (user/agent/tool bubbles)
 *   - ChatInputField (Enter to send)
 *   - Thread-scoped connection state: Connected / Disconnected / Reconnecting
 *   - Thread-scoped reconnect banner (C2)
 *   - Thread-scoped permission prompt
 *
 * The thread's connection state is entirely local — no global flags.
 * A reconnect on one thread does NOT affect any other thread.
 *
 * Requirements: B6, C2 (§B6, §C2)
 */

#include <memory>
#include <string>

#include <juce_gui_basics/juce_gui_basics.h>
#include <nlohmann/json.hpp>

#include "../app/AudioEngine.hpp"
#include "../control/ControlInterface.hpp"
#include "AcpAgentSession.hpp"
#include "MessageHistoryView.hpp"
#include "PermissionPromptComponent.hpp"
#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

/**
 * Thread-scoped connection state for a single ChatThread.
 *
 * This is the per-thread enum that replaces the global `bool disconnected_`
 * from the original ChatSidebar (C2 §7).
 */
enum class ThreadConnState
{
    Connected,      ///< Agent subprocess running and session ready
    Disconnected,   ///< Agent subprocess exited; reconnect available
    Reconnecting,   ///< restart() in progress (non-blocking, async)
};

/**
 * ChatThread
 *
 * Each instance owns:
 *   - Its own AcpAgentSession (one subprocess per tab, decision #3)
 *   - Its own MessageHistoryView (independent conversation history per thread)
 *   - Its own connection state (Connected / Disconnected / Reconnecting)
 *   - Its own reconnect banner (shown only when this thread is disconnected)
 *
 * A reconnect on one ChatThread only restarts that thread's session.
 * Other threads are completely unaffected (C2 §2, §7).
 *
 * All public methods run on the JUCE message thread.
 * AcpAgentSession callbacks are already marshalled to the message thread
 * before reaching ChatThread (via juce::MessageManager::callAsync).
 *
 * Requirements: B6, C2
 */
class ChatThread : public juce::Component,
                   public juce::TextEditor::Listener
{
public:
    /**
     * @param audio      AudioEngine reference (passed to SliderPanel).
     * @param ci         ControlInterface for slider dispatches.
     */
    ChatThread(AudioEngine& audio,
               hathor::control::ControlInterface& ci);

    ~ChatThread() override;

    // -----------------------------------------------------------------------
    // Session lifecycle — call before AcpAgentSession::start()
    // -----------------------------------------------------------------------

    /**
     * Wire all AcpAgentSession callbacks to this thread.
     *
     * Must be called on the JUCE message thread before session.start().
     * Does NOT take ownership of the session.
     *
     * @param session       The session whose callbacks we register on.
     * @param agentExePath  Stored so reconnect can pass it to restart().
     * @param projectDir    Stored so reconnect can pass it to restart().
     * @param mcpPath       Stored so reconnect can pass it to restart().
     */
    void setSession(AcpAgentSession& session,
                    std::string agentExePath,
                    std::string projectDir,
                    std::string mcpPath);

    // -----------------------------------------------------------------------
    // Tab identity
    // -----------------------------------------------------------------------

    /**
     * Set the display title for this thread tab.
     * Defaults to "Thread 1", "Thread 2", etc.
     */
    void setTabTitle(const juce::String& title) { tabTitle_ = title; }
    const juce::String& tabTitle() const noexcept { return tabTitle_; }

    // -----------------------------------------------------------------------
    // Connection state (thread-scoped — C2 §7)
    // -----------------------------------------------------------------------

    ThreadConnState connState() const noexcept { return connState_; }

    /**
     * Returns the error message if the last restart attempt failed (C2 §6).
     * Empty string when there is no error.
     */
    const juce::String& lastRestartError() const noexcept { return lastRestartError_; }

    // -----------------------------------------------------------------------
    // Reconnect action (C2 §4, §5, §6, §11)
    // -----------------------------------------------------------------------

    /**
     * Trigger a thread-scoped reconnect.
     *
     * 1. Identifies this thread's session.
     * 2. Transitions state to Reconnecting.
     * 3. Invokes the existing restart() path on this thread's AcpAgentSession.
     * 4. Prevents duplicate restarts while Reconnecting (C2 §4.4).
     *
     * Does NOT block the JUCE message thread — restart() spawns a background
     * sender thread; callbacks fire asynchronously (C2 §4.7).
     */
    void reconnect();

    /**
     * Returns true if a reconnect is currently in progress for this thread.
     * Used to prevent duplicate restart attempts (C2 §4.4, §11).
     */
    bool isReconnecting() const noexcept
    {
        return connState_ == ThreadConnState::Reconnecting;
    }

    // -----------------------------------------------------------------------
    // Active session access
    // -----------------------------------------------------------------------

    /** Returns the session pointer, or nullptr if not wired. */
    AcpAgentSession* session() const noexcept { return session_; }

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------

    void resized() override;
    void paint(juce::Graphics& g) override;

    // -----------------------------------------------------------------------
    // juce::TextEditor::Listener
    // -----------------------------------------------------------------------

    void textEditorReturnKeyPressed(juce::TextEditor& editor) override;
    void textEditorTextChanged(juce::TextEditor& editor) override;

    // -----------------------------------------------------------------------
    // Session callback handlers — all called on JUCE message thread
    // -----------------------------------------------------------------------

    /** Called when the agent subprocess exits unexpectedly (reader EOF). */
    void onDisconnected();

    /** Called on start failure or fatal error (C2 §6). */
    void onError(const std::string& reason);

    /** Called when the session is ready (restart succeeded). */
    void onReady();

    /** Called for each streaming text chunk from the agent. */
    void onAgentMessageChunk(const std::string& text);

    /** Called for tool_call / tool_call_update notifications. */
    void onToolCallUpdate(nlohmann::json update);

    /** Called for permission requests. */
    void onPermissionRequest(int requestId, nlohmann::json options);

    // -----------------------------------------------------------------------
    // Layout constants
    // -----------------------------------------------------------------------
    static constexpr int kInputH       = 36;
    static constexpr int kReconnectH   = 36;
    static constexpr int kStatusH      = 28;
    static constexpr int kPermissionH  = 120;

private:
    friend class ChatSidebar;  // MainWindow/ChatSidebar create and own sessions

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    void showStatus(const juce::String& msg);
    void clearStatus();
    void setInputEnabled(bool enabled);
    void scrollToBottom();

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    /** The session for this thread (not owned). Set by setSession(). */
    AcpAgentSession* session_ = nullptr;

    /** Stored for restart() calls. */
    std::string agentExePath_;
    std::string projectDir_;
    std::string mcpPath_;

    /** Tab display title. */
    juce::String tabTitle_;

    // --- Thread-scoped connection state (C2 §7) ---
    // This is per-thread, NOT global. Each ChatThread has its own state.
    ThreadConnState connState_ = ThreadConnState::Connected;

    /** Error from the last failed restart attempt (C2 §6). */
    juce::String lastRestartError_;

    // Reconnect banner — shown only when this thread is disconnected.
    juce::TextButton reconnectBanner_;

    // Status label — thread-scoped.
    juce::Label statusLabel_;
    bool statusVisible_ = false;

    // Permission prompt — thread-scoped.
    std::unique_ptr<PermissionPromptComponent> permissionPrompt_;

    // Message history viewport.
    juce::Viewport historyViewport_;
    std::unique_ptr<MessageHistoryContainer> historyContainer_;
    MessageBubble* lastAgentBubble_ = nullptr;

    // Chat input field.
    juce::TextEditor inputField_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChatThread)
};

} // namespace hathor::ui
