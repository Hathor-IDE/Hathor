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

#include "AcpAgentSession.hpp"
#include "MessageHistoryView.hpp"
#include "PermissionPromptComponent.hpp"
#include "HathorLookAndFeel.hpp"
#include "ThreadConnState.hpp"

namespace hathor::ui {

/**
 * ChatThread
 *
 * Each instance owns:
 *   - Its own AcpAgentSession (one subprocess per tab, decision #3)
 *   - Its own MessageHistoryContainer (independent conversation history per thread)
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
    ChatThread();

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

    void setTabTitle(const juce::String& title) { tabTitle_ = title; }
    const juce::String& tabTitle() const noexcept { return tabTitle_; }

    // -----------------------------------------------------------------------
    // Connection state (thread-scoped — C2 §7)
    // -----------------------------------------------------------------------

    /**
     * Returns the current connection state for this thread (C2 §7).
     * Each thread has its own independent state.
     */
    ThreadConnState connState() const noexcept { return connState_.state; }

    /**
     * Returns the error message if the last restart attempt failed (C2 §6).
     * Empty string when there is no error.
     */
    juce::String lastRestartError() const
    {
        return juce::String(connState_.lastError);
    }

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
     * Does NOT block the JUCE message thread (C2 §4.7).
     */
    void reconnect();

    /**
     * Returns true if a reconnect is currently in progress for this thread.
     * Used to prevent duplicate restart attempts (C2 §4.4, §11).
     */
    bool isReconnecting() const noexcept
    {
        return connState_.isReconnecting();
    }

    // -----------------------------------------------------------------------
    // Active session access
    // -----------------------------------------------------------------------

    AcpAgentSession* session() const noexcept { return session_; }

    // -----------------------------------------------------------------------
    // Visible area for content (below reconnect/permission banners)
    // -----------------------------------------------------------------------

    /**
     * Returns the bounds within this thread's component where the message
     * history viewport should be placed. Called by ChatSidebar::resized()
     * to constrain the active thread's layout.
     */
    int contentTopY() const;

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

    void onDisconnected();
    void onError(const std::string& reason);
    void onReady();
    void onAgentMessageChunk(const std::string& text);
    void onToolCallUpdate(nlohmann::json update);
    void onPermissionRequest(int requestId, nlohmann::json options);

    // -----------------------------------------------------------------------
    // Layout constants
    // -----------------------------------------------------------------------
    static constexpr int kInputH       = 36;
    static constexpr int kReconnectH   = 36;
    static constexpr int kStatusH      = 28;
    static constexpr int kPermissionH  = 120;

private:
    friend class ChatSidebar;

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

    /** The session for this thread (not owned — owned by ChatSidebar). */
    AcpAgentSession* session_ = nullptr;

    /** Stored for restart() calls. */
    std::string agentExePath_;
    std::string projectDir_;
    std::string mcpPath_;

    /** Tab display title. */
    juce::String tabTitle_;

    // --- Thread-scoped connection state (C2 §7) ---
    // This is per-thread, NOT global. Each ChatThread has its own state
    // machine instance. A reconnect on one thread does NOT affect others.
    ThreadConnStateMachine connState_;

    // Reconnect banner — shown only when this thread is disconnected (C2 §2).
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
