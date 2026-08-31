// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChatThread.cpp — One chat thread: AcpAgentSession + UI state.
 *
 * Per-thread connection state (C2 §7): each ChatThread has its own
 * connState_ (Connected / Disconnected / Reconnecting), its own
 * reconnectBanner_, its own status label, and its own
 * MessageHistoryContainer.
 *
 * Reconnect reuses the existing disconnect → restart() path
 * (AcpAgentSession::restart). No second restart mechanism is introduced.
 *
 * Requirements: B6, C2 (§2, §4, §5, §6, §7, §8, §9, §10, §11)
 */

#include "ChatThread.hpp"
#include "HathorLookAndFeel.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace hathor::ui {

// ===========================================================================
// ChatThread
// ===========================================================================

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ChatThread::ChatThread()
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // -----------------------------------------------------------------------
    // Status label (hidden by default) — thread-scoped (C2 §7)
    // -----------------------------------------------------------------------
    addChildComponent(statusLabel_);
    statusLabel_.setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::labelMd));
    statusLabel_.setColour(juce::Label::backgroundColourId,
                           palette.error.withAlpha(0.2f));
    statusLabel_.setColour(juce::Label::textColourId,
                           palette.textPrimary);
    statusLabel_.setJustificationType(juce::Justification::centredLeft);
    statusLabel_.setBorderSize(juce::BorderSize<int>(0, 6, 0, 6));

    // -----------------------------------------------------------------------
    // Reconnect banner — thread-scoped (C2 §2, §3)
    // Hidden by default; shown only when THIS thread's session disconnects.
    // -----------------------------------------------------------------------
    addChildComponent(reconnectBanner_);
    reconnectBanner_.setButtonText("Agent disconnected \xe2\x80\x94 click to reconnect");
    reconnectBanner_.setColour(juce::TextButton::buttonColourId,
                               palette.error.withAlpha(0.6f));
    reconnectBanner_.setColour(juce::TextButton::buttonOnColourId,
                               palette.error.withAlpha(0.7f));
    reconnectBanner_.setColour(juce::TextButton::textColourOffId,
                               palette.background);
    reconnectBanner_.setColour(juce::TextButton::textColourOnId,
                               palette.background);

    // Wire the reconnect click — reuses the existing restart() path (C2 §1).
    // This handler is specific to THIS thread; other threads are unaffected.
    reconnectBanner_.onClick = [this]()
    {
        reconnect();
    };

    // -----------------------------------------------------------------------
    // Message history viewport
    // -----------------------------------------------------------------------
    historyContainer_ = std::make_unique<MessageHistoryContainer>();
    historyViewport_.setViewedComponent(historyContainer_.get(),
                                        /*deleteComponentWhenNoLongerNeeded=*/false);
    historyViewport_.setScrollBarsShown(/*showVertical=*/true,
                                       /*showHorizontal=*/false);
    historyViewport_.setScrollBarThickness(8);
    addAndMakeVisible(historyViewport_);

    // -----------------------------------------------------------------------
    // Chat input field
    // -----------------------------------------------------------------------
    const auto& inputPalette = HathorLookAndFeel::fromComponent(*this).getPalette();
    addAndMakeVisible(inputField_);
    inputField_.setMultiLine(false);
    inputField_.setReturnKeyStartsNewLine(false);
    inputField_.setScrollbarsShown(false);
    inputField_.setFont(HathorLookAndFeel::fontRegular(13.0f));
    inputField_.setColour(juce::TextEditor::backgroundColourId,
                           inputPalette.surfaceContainer);
    inputField_.setColour(juce::TextEditor::textColourId,
                           inputPalette.textPrimary);
    inputField_.setColour(juce::TextEditor::outlineColourId,
                           inputPalette.surfaceHighest);
    inputField_.setColour(juce::TextEditor::focusedOutlineColourId,
                           inputPalette.accent);
    inputField_.setColour(juce::CaretComponent::caretColourId,
                           inputPalette.accent);
    inputField_.setTextToShowWhenEmpty("Message agent...",
                                       inputPalette.textDisabled);
    inputField_.addListener(this);
}

ChatThread::~ChatThread()
{
    inputField_.removeListener(this);
}

// ---------------------------------------------------------------------------
// Session wiring
// ---------------------------------------------------------------------------

void ChatThread::setSession(AcpAgentSession& session,
                            std::string agentExePath,
                            std::string projectDir,
                            std::string mcpPath)
{
    session_       = &session;
    agentExePath_  = std::move(agentExePath);
    projectDir_    = std::move(projectDir);
    mcpPath_       = std::move(mcpPath);

    // Register all callbacks. All are invoked on the JUCE message thread by
    // AcpAgentSession (it uses callAsync for every reader-thread notification).
    //
    // These callbacks are bound to THIS ChatThread instance — when the
    // session fires onAgentDisconnected, it calls THIS thread's onDisconnected,
    // which updates THIS thread's reconnectBanner_, not any other thread's.
    //
    // SafePointer guards against use-after-free: when a tab is closed,
    // session->stop() joins all worker threads, but any callback already
    // queued via MessageManager::callAsync may still be pending.  SafePointer
    // becomes null when the ChatThread component is destroyed, so the
    // callback becomes a safe no-op instead of targeting a freed object.

    juce::Component::SafePointer<ChatThread> safeThread(this);

    session_->setOnError([safeThread](std::string reason)
    {
        if (safeThread)
            safeThread->onError(reason);
    });

    session_->setOnAgentDisconnected([safeThread]()
    {
        if (safeThread)
            safeThread->onDisconnected();
    });

    session_->setOnAgentMessageChunk([safeThread](std::string text)
    {
        if (safeThread)
            safeThread->onAgentMessageChunk(text);
    });

    session_->setOnToolCallUpdate([safeThread](nlohmann::json update)
    {
        if (safeThread)
            safeThread->onToolCallUpdate(std::move(update));
    });

    session_->setOnPermissionRequest([safeThread](int requestId, nlohmann::json options)
    {
        if (safeThread)
            safeThread->onPermissionRequest(requestId, std::move(options));
    });

    // Handshake lifecycle (issue A6) — connecting status + ready signal.
    session_->setOnConnecting([safeThread](std::string status)
    {
        if (safeThread)
            safeThread->onConnecting(std::move(status));
    });

    session_->setOnAgentReady([safeThread]()
    {
        if (safeThread)
            safeThread->onReady();
    });

    // Post-init prompt error response (issue A5).
    session_->setOnPromptError([safeThread](std::string error)
    {
        if (safeThread)
            safeThread->onPromptError(std::move(error));
    });
}

// ---------------------------------------------------------------------------
// Reconnect (C2 §4, §5, §6, §11)
// ---------------------------------------------------------------------------

void ChatThread::reconnect()
{
    // Prevent duplicate reconnect attempts while one is already in progress
    // (C2 §4.4, §11). If we're already reconnecting, ignore the click.
    // beginReconnect() returns false if state is already Reconnecting.
    if (!connState_.beginReconnect())
        return;

    // Transition to Reconnecting state (C2 §5) — this gives the user immediate
    // visual feedback that a reconnect is underway.
    reconnectBanner_.setButtonText("Reconnecting...");
    reconnectBanner_.setVisible(true);
    setInputEnabled(false);

    // Add a status line to the history for visibility.
    historyContainer_->addBubble("Reconnecting...",
                                 MessageBubble::Role::StatusLine);
    lastAgentBubble_ = nullptr;
    scrollToBottom();

    resized();

    if (session_ != nullptr)
    {
        // Invoke the existing restart() path on THIS thread's session only.
        // (C2 §1 — reuse, do not implement a second restart mechanism.)
        // restart() spawns a background sender thread — does NOT block the
        // JUCE message thread (C2 §4.7).
        //
        // On success: the session will fire onAgentMessageChunk or the
        // session/update notification, and we detect readiness in the callback.
        // If the session was started fresh, isReady() will eventually become
        // true. We rely on the existing callback path.
        session_->restart(agentExePath_, projectDir_, mcpPath_);
    }
    else
    {
        // No session wired — handle gracefully by returning to disconnected.
        connState_.onReconnectFailure("No agent session configured");
        reconnectBanner_.setButtonText("Agent disconnected \xe2\x80\x94 click to reconnect");
        resized();
    }
}

// ---------------------------------------------------------------------------
// Connection state callbacks (C2 §5, §6)
// ---------------------------------------------------------------------------

void ChatThread::onDisconnected()
{
    // Thread-scoped disconnect (C2 §2, §7).
    // Only THIS thread's banner and state change.
    connState_.onDisconnect();

    reconnectBanner_.setButtonText("Agent disconnected \xe2\x80\x94 click to reconnect");
    reconnectBanner_.setVisible(true);
    setInputEnabled(false);
    clearStatus();
    resized();

    // Add a status line to THIS thread's history for visibility.
    historyContainer_->addBubble("Agent disconnected.",
                                 MessageBubble::Role::StatusLine);
    lastAgentBubble_ = nullptr;
    scrollToBottom();
}

void ChatThread::onError(const std::string& reason)
{
    // C2 §6 — preserve the existing restart path's error message rather than
    // replacing it with an opaque generic failure.

    // If we were reconnecting and got an error, we land back in Disconnected.
    // The error is preserved so the user sees what went wrong.
    connState_.onReconnectFailure(reason);

    showStatus("Error: " + juce::String(reason));

    // Update the reconnect banner to show "Reconnect" (retry available).
    reconnectBanner_.setButtonText("Reconnect");
    reconnectBanner_.setVisible(true);
    setInputEnabled(false);
    resized();

    // Add an error status line to history.
    historyContainer_->addBubble("Error: " + juce::String(reason),
                                 MessageBubble::Role::StatusLine);
    lastAgentBubble_ = nullptr;
    scrollToBottom();
}

void ChatThread::onReady()
{
    // The init handshake completed successfully (issue A6). Was this a
    // reconnect or a fresh start? Only the reconnect path posts a
    // "Reconnected." status line; a fresh connection silently clears the
    // "Connecting…" status.
    const bool wasReconnecting = connState_.isReconnecting();
    connState_.onReconnectSuccess();

    connecting_ = false;
    reconnectBanner_.setVisible(false);
    setInputEnabled(true);
    clearStatus();
    resized();

    // Add a status line only for the reconnect case (preserves prior UX).
    if (wasReconnecting)
    {
        historyContainer_->addBubble("Reconnected.",
                                     MessageBubble::Role::StatusLine);
        lastAgentBubble_ = nullptr;
        scrollToBottom();
    }
}

void ChatThread::onConnecting(const std::string& status)
{
    // Visible "connecting" state during the ACP handshake (issue A6).
    connecting_ = true;
    showStatus(juce::String("Connecting: ") + juce::String(status));
    // Don't let the user submit a prompt before the session is ready.
    setInputEnabled(false);
}

void ChatThread::onPromptError(const std::string& error)
{
    // Post-init prompt error response — surface it as a visible message
    // rather than silently dropping it (issue A5).
    juce::String msg = juce::String("Agent error: ") + juce::String(error);

    showStatus(msg);

    historyContainer_->addBubble(msg, MessageBubble::Role::StatusLine);
    lastAgentBubble_ = nullptr;
    scrollToBottom();
}


// ---------------------------------------------------------------------------
// Agent message callbacks
// ---------------------------------------------------------------------------

void ChatThread::onAgentMessageChunk(const std::string& text)
{
    const juce::String chunk(text);

    // If we were in a Reconnecting state and just received a message,
    // the session is now live — transition to Connected.
    // This handles the case where restart() succeeds but onReady() wasn't
    // explicitly called (the original code doesn't have an onReady callback
    // wired from AcpAgentSession, so we detect readiness from message arrival).
    if (connState_.isReconnecting())
    {
        connState_.onReconnectSuccess();
        reconnectBanner_.setVisible(false);
        setInputEnabled(true);
        clearStatus();
        resized();
    }

    if (lastAgentBubble_ != nullptr)
    {
        lastAgentBubble_->appendText(chunk);
    }
    else
    {
        lastAgentBubble_ = historyContainer_->addBubble(chunk,
                                                        MessageBubble::Role::Agent);
    }

    scrollToBottom();
}

void ChatThread::onToolCallUpdate(nlohmann::json update)
{
    // If we were reconnecting, first message chunk or tool call means we're live.
    if (connState_.isReconnecting())
    {
        onReady();
    }

    std::string label;

    try
    {
        const std::string updateType = update.value("sessionUpdate", "tool_call");
        // ACP v1 spec: ToolCall / ToolCallUpdate use "title" (human-readable)
        // and "toolCallId" for the id. Older code assumed "toolName" which is
        // not a real ACP field — real agents (claude-code-acp, gemini) use "title".
        const std::string title = update.value("title",
                                   update.value("toolCallId", "(unknown tool)"));

        if (updateType == "tool_call")
        {
            label = "\xe2\x86\x92 " + title;  // → title
        }
        else
        {
            const std::string status = update.value("status", "");
            label = "\xe2\x86\x92 " + title;
            if (!status.empty())
                label += " [" + status + "]";
        }
    }
    catch (...)
    {
        label = "(tool call)";
    }

    historyContainer_->addBubble(juce::String(label),
                                 MessageBubble::Role::ToolCall);
    lastAgentBubble_ = nullptr;   // next message chunk starts a new bubble
    scrollToBottom();
}

void ChatThread::onPermissionRequest(int requestId, nlohmann::json options)
{
    // If we were reconnecting, permission request means session is live.
    if (connState_.isReconnecting())
    {
        onReady();
    }

    // Remove any previous permission prompt on THIS thread.
    if (permissionPrompt_ != nullptr)
    {
        removeChildComponent(permissionPrompt_.get());
        permissionPrompt_.reset();
    }

    // Create a new inline permission prompt (thread-scoped — C2 §3).
    permissionPrompt_ = std::make_unique<PermissionPromptComponent>(
        requestId,
        options,
        [this](int id, std::string optId)
        {
            // Respond via the session's sender queue (non-blocking).
            if (session_ != nullptr)
                session_->respondPermission(id, std::move(optId));

            // Hide and remove the prompt.
            if (permissionPrompt_ != nullptr)
            {
                removeChildComponent(permissionPrompt_.get());
                permissionPrompt_.reset();
            }
            resized();
        });

    addAndMakeVisible(*permissionPrompt_);
    permissionPrompt_->start();
    resized();
}

// ---------------------------------------------------------------------------
// juce::Component overrides
// ---------------------------------------------------------------------------

void ChatThread::resized()
{
    auto b = getLocalBounds();

    // Status label (conditionally shown at top)
    if (statusVisible_)
    {
        statusLabel_.setBounds(b.removeFromTop(kStatusH));
    }

    // Reconnect banner (conditionally shown — C2 §3)
    // Shown whenever we're NOT in Connected state.
    if (connState_.state != ThreadConnState::Connected)
    {
        reconnectBanner_.setBounds(b.removeFromTop(kReconnectH));
    }

    // Permission prompt overlay (sits below status/reconnect row)
    if (permissionPrompt_ != nullptr && permissionPrompt_->isVisible())
    {
        permissionPrompt_->setBounds(b.removeFromTop(kPermissionH));
    }

    // Input field at the bottom
    inputField_.setBounds(b.removeFromBottom(kInputH));

    // History viewport fills the remaining space
    historyViewport_.setBounds(b);
    historyContainer_->reflowToWidth(b.getWidth());
}

int ChatThread::contentTopY() const
{
    // Calculate the Y offset where content (history viewport) begins,
    // accounting for visible status, reconnect, and permission banners.
    int y = 0;

    if (statusVisible_)
        y += kStatusH;

    if (connState_.state != ThreadConnState::Connected)
        y += kReconnectH;

    if (permissionPrompt_ != nullptr && permissionPrompt_->isVisible())
        y += kPermissionH;

    return y;
}

void ChatThread::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    g.fillAll(palette.surfaceContainer);
}

// ---------------------------------------------------------------------------
// TextEditor::Listener — Enter key (Req 25.2)
// ---------------------------------------------------------------------------

void ChatThread::textEditorReturnKeyPressed(juce::TextEditor& editor)
{
    if (&editor != &inputField_)
        return;

    // Only send if connected (C2 §4 — don't send while disconnected/reconnecting).
    if (connState_.state != ThreadConnState::Connected)
        return;

    const juce::String rawText = inputField_.getText();

    // Reject empty or whitespace-only content (Req 25.2).
    if (rawText.trim().isEmpty())
        return;

    const std::string text = rawText.toStdString();

    // Add user bubble to THIS thread's history.
    historyContainer_->addBubble(rawText, MessageBubble::Role::User);
    lastAgentBubble_ = nullptr;
    scrollToBottom();

    // Clear input.
    inputField_.setText(juce::String{}, juce::dontSendNotification);

    // Forward to session (fire-and-forget, non-blocking — C2 §4.7).
    if (session_ != nullptr && session_->isReady())
    {
        session_->sendPrompt(text);
    }
}

void ChatThread::textEditorTextChanged(juce::TextEditor& editor)
{
    if (&editor != &inputField_)
        return;

    // Enforce 2048-character limit (Req 25.6).
    constexpr int kMaxChars = 2048;
    if (inputField_.getText().length() > kMaxChars)
    {
        const juce::String truncated = inputField_.getText().substring(0, kMaxChars);
        inputField_.setText(truncated, juce::dontSendNotification);
        inputField_.setCaretPosition(kMaxChars);
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void ChatThread::showStatus(const juce::String& msg)
{
    statusLabel_.setText(msg, juce::dontSendNotification);
    statusVisible_ = true;
    statusLabel_.setVisible(true);
    resized();
}

void ChatThread::clearStatus()
{
    statusLabel_.setText({}, juce::dontSendNotification);
    statusVisible_ = false;
    statusLabel_.setVisible(false);
    resized();
}

void ChatThread::scrollToBottom()
{
    historyViewport_.setViewPosition(
        0,
        std::max(0, historyContainer_->getHeight() - historyViewport_.getHeight()));
}

void ChatThread::setInputEnabled(bool enabled)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    inputField_.setEnabled(enabled);
    inputField_.setColour(juce::TextEditor::backgroundColourId,
                           juce::Colour(enabled ? palette.surfaceContainer
                                                : palette.background));
    inputField_.repaint();
}

} // namespace hathor::ui
