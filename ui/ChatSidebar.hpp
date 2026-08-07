// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ChatSidebar.hpp — AI chat sidebar with message history, input, and sliders.
 *
 * Component hierarchy (320 px wide, full window height):
 *
 *   ┌─────────────────────────────────┐
 *   │  [Status/error label]           │  ← hidden unless there's a message
 *   │  [Reconnect banner]             │  ← hidden unless disconnected (Req 32.8)
 *   │  [Permission prompt]            │  ← hidden unless permission pending
 *   ├─────────────────────────────────┤
 *   │                                 │
 *   │  MessageHistoryView (Viewport)  │  ← scrollable agent/user bubbles
 *   │                                 │
 *   ├─────────────────────────────────┤
 *   │  ChatInputField (TextEditor)    │  ← max 2048 chars, Enter to send
 *   ├─────────────────────────────────┤
 *   │  SliderPanel (BPM + Gain)       │  ← always visible, always interactive
 *   └─────────────────────────────────┘
 *
 * Thread model:
 *   - All public methods run on the JUCE message thread.
 *   - AcpAgentSession callbacks are already marshalled to the message thread
 *     before reaching ChatSidebar — no additional synchronisation needed.
 *
 * Audio isolation (Req 32.9):
 *   - ChatSidebar and AcpAgentSession have no access to AudioEngine state.
 *   - SliderPanel dispatches commands via ControlInterface (worker thread).
 *   - The SPSC ring buffer and AudioEngine teardown paths are completely
 *     independent of the chat/agent lifecycle.
 *
 * Requirements: 25.1, 25.2, 25.3, 25.5, 25.6, 26.1, 32.1, 32.3, 32.5,
 *               32.6, 32.8, 32.9
 */

// This guard is checked in MainWindow.cpp to suppress the stub ChatSidebar.
#define HATHOR_CHAT_SIDEBAR_DEFINED

#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>
#include <nlohmann/json.hpp>

// App / control
#include "../app/AudioEngine.hpp"
#include "../control/ControlInterface.hpp"

// Sibling UI components
#include "AcpAgentSession.hpp"
#include "PermissionPromptComponent.hpp"
#include "SliderPanel.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// AsciiArtHeader — decorative generative ASCII art (Req 25.4)
// ---------------------------------------------------------------------------

/**
 * Renders decorative procedural ASCII art in the ChatSidebar panel header.
 *
 * The art is generative: a simple sine-based pattern that cycles through
 * different character sets and phases. No heap allocation on paint, no
 * heavy computation.
 *
 * Requirement 25.4: LOW PRIORITY / NON-BLOCKING — this component is fully
 * optional and must not gate any other Phase 2 work.
 */
class AsciiArtHeader : public juce::Component,
                       public juce::Timer
{
public:
    AsciiArtHeader();

    void paint(juce::Graphics& g) override;

    /// Preferred height for this header.
    static constexpr int kPreferredHeight = 48;

private:
    void timerCallback() override;

    float phase_ = 0.0f;  ///< Animation phase counter

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AsciiArtHeader)
};

// ---------------------------------------------------------------------------
// MessageBubble — one message entry in the history view
// ---------------------------------------------------------------------------

/**
 * A single message bubble rendered inside the scrollable history viewport.
 *
 * User messages are right-aligned with a slight accent tint.
 * Agent messages (and tool-call status lines) are left-aligned.
 */
class MessageBubble : public juce::Component
{
public:
    enum class Role { User, Agent, ToolCall, StatusLine };

    MessageBubble(const juce::String& text, Role role);

    /// Append more text to the bubble (for streaming agent_message_chunk).
    void appendText(const juce::String& extra);

    /// Replace the label text entirely.
    void setText(const juce::String& t);

    /// Compute the preferred height for this bubble given a fixed width.
    int preferredHeight(int width) const;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    juce::TextEditor label_;
    Role             role_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MessageBubble)
};

// ---------------------------------------------------------------------------
// MessageHistoryContainer — inner content component inside the Viewport
// ---------------------------------------------------------------------------

/**
 * Holds an ordered list of MessageBubble children and stacks them vertically.
 * The Viewport scrolls this component.
 */
class MessageHistoryContainer : public juce::Component
{
public:
    MessageHistoryContainer();

    /// Add a new bubble and return a raw pointer to it.
    MessageBubble* addBubble(const juce::String& text, MessageBubble::Role role);

    /// Reflow all bubbles to the given width and update the component height.
    void reflowToWidth(int w);

private:
    juce::OwnedArray<MessageBubble> bubbles_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MessageHistoryContainer)
};

// ---------------------------------------------------------------------------
// ChatSidebar
// ---------------------------------------------------------------------------

/**
 * The AI chat sidebar: 320 px wide right-hand panel.
 *
 * Lifecycle:
 *   1. Construct (MainWindow constructor)
 *   2. Call setSession() to wire the AcpAgentSession callbacks
 *   3. AcpAgentSession::start() is called by the application after construction
 *
 * Disconnect / reconnect (Req 32.8):
 *   - onDisconnected() is registered as AcpAgentSession::setOnAgentDisconnected().
 *   - When called (on JUCE message thread), shows reconnectBanner_ and disables
 *     the input field.
 *   - Clicking reconnectBanner_ hides it, re-enables input, and calls
 *     session_->restart(...).
 *
 * Audio independence (Req 32.9):
 *   - No AudioEngine members; SliderPanel holds the only CI reference.
 *   - AcpAgentSession teardown does not touch AudioEngine state.
 */
class ChatSidebar : public juce::Component,
                    public juce::TextEditor::Listener
{
public:
    /**
     * @param audio  AudioEngine reference (used only to pass to SliderPanel).
     * @param ci     ControlInterface for slider dispatches.
     */
    ChatSidebar(AudioEngine& audio,
                hathor::control::ControlInterface& ci);

    ~ChatSidebar() override;

    // -----------------------------------------------------------------------
    // Session wiring — call before AcpAgentSession::start()
    // -----------------------------------------------------------------------

    /**
     * Wire all AcpAgentSession callbacks to this sidebar.
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
    // SliderPanel access (for UITimer bidirectional sync)
    // -----------------------------------------------------------------------
    SliderPanel& getSliderPanel() noexcept { return *sliderPanel_; }

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------
    void resized() override;
    void paint(juce::Graphics& g) override;

    // -----------------------------------------------------------------------
    // juce::TextEditor::Listener overrides
    // -----------------------------------------------------------------------

    /// Called when the user presses Enter in the input field (Req 25.2).
    void textEditorReturnKeyPressed(juce::TextEditor& editor) override;

    /// Enforce 2048-character limit (Req 25.6).
    void textEditorTextChanged(juce::TextEditor& editor) override;

private:
    // -----------------------------------------------------------------------
    // Session callback handlers — all called on the JUCE message thread
    // -----------------------------------------------------------------------

    /// onError_ — start failure or fatal error.
    void onError(const std::string& reason);

    /// onAgentDisconnected_ — subprocess stdout EOF (Req 32.8, 32.9).
    void onDisconnected();

    /// onAgentMessageChunk_ — streaming text chunk from agent (Req 32.5).
    void onAgentMessageChunk(const std::string& text);

    /// onToolCallUpdate_ — tool_call / tool_call_update notification.
    void onToolCallUpdate(nlohmann::json update);

    /// onPermissionRequest_ — session/request_permission (Req 32.6).
    void onPermissionRequest(int requestId, nlohmann::json options);

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /// Show a one-line status message (error / info) at the top.
    void showStatus(const juce::String& msg);

    /// Clear the status message.
    void clearStatus();

    /// Scroll the history viewport to the bottom.
    void scrollToBottom();

    /// Enable / disable the chat input field.
    void setInputEnabled(bool enabled);

    // -----------------------------------------------------------------------
    // Layout constants
    // -----------------------------------------------------------------------
    static constexpr int kStatusH      = 28;   ///< Status label height
    static constexpr int kReconnectH   = 36;   ///< Reconnect banner height
    static constexpr int kPermissionH  = 120;  ///< Permission prompt height
    static constexpr int kInputH       = 36;   ///< Chat input field height
    static constexpr int kSliderH      = 80;   ///< SliderPanel height
    static constexpr int kAsciiArtH    = AsciiArtHeader::kPreferredHeight;  ///< ASCII art header height

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    /// Pointer to the session (not owned). Set by setSession().
    AcpAgentSession* session_ = nullptr;

    /// Stored for restart() calls (Req 32.8).
    std::string agentExePath_;
    std::string projectDir_;
    std::string mcpPath_;

    // ASCII art header — decorative, generative (Req 25.4)
    AsciiArtHeader asciiHeader_;

    // Status bar (errors / info)
    juce::Label statusLabel_;
    bool        statusVisible_ = false;

    // Reconnect banner (Req 32.8): hidden unless the subprocess exits unexpectedly.
    juce::TextButton reconnectBanner_;
    bool             disconnected_ = false;

    // Permission prompt (Req 32.6): shown as an overlay in the sidebar.
    // Owned via unique_ptr; replaced each time a new permission request arrives.
    std::unique_ptr<PermissionPromptComponent> permissionPrompt_;

    // Message history — Viewport wraps a MessageHistoryContainer.
    juce::Viewport                            historyViewport_;
    std::unique_ptr<MessageHistoryContainer>  historyContainer_;

    /// Raw pointer to the last agent bubble — used for streaming appends.
    /// Invalidated when a non-agent bubble is added.
    MessageBubble* lastAgentBubble_ = nullptr;

    // Chat input field (Req 25.2, 25.6)
    juce::TextEditor inputField_;

    // Slider panel (Req 26.1)
    std::unique_ptr<SliderPanel> sliderPanel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChatSidebar)
};

} // namespace hathor::ui
