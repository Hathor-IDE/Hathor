// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChatSidebar.cpp — AI chat sidebar implementation.
 *
 * Implements the right-hand 320 px chat panel:
 *   - Scrollable message history (user + agent bubbles, tool-call status lines)
 *   - Single-line text input (Enter to send, max 2048 chars)
 *   - BPM + master-gain SliderPanel beneath the history
 *   - Status label for errors and informational messages
 *   - Reconnect banner shown when the agent subprocess exits unexpectedly (Req 32.8)
 *   - Inline PermissionPromptComponent for session/request_permission (Req 32.6)
 *
 * Audio independence (Req 32.9):
 *   - This file includes no AudioEngine state access beyond construction.
 *   - SliderPanel holds the only ControlInterface reference (for bpm/gain dispatch).
 *   - AcpAgentSession teardown is completely independent of AudioEngine state.
 *
 * Requirements: 25.1, 25.2, 25.3, 25.5, 25.6, 26.1, 32.1, 32.3, 32.5,
 *               32.6, 32.8, 32.9
 */

#include "ChatSidebar.hpp"

#include <algorithm>
#include <string>

namespace hathor::ui {

// ===========================================================================
// Colour constants (consistent with HathorLookAndFeel in MainWindow.cpp)
// ===========================================================================

namespace colours {
    static constexpr juce::uint32 kBackground  = 0xff1e1e1e;
    static constexpr juce::uint32 kSurface     = 0xff252526;
    static constexpr juce::uint32 kText        = 0xffd4d4d4;
    static constexpr juce::uint32 kAccent      = 0xff569cd6;
    static constexpr juce::uint32 kError       = 0xffcd3131;
    static constexpr juce::uint32 kWarning     = 0xffe0a020;
    static constexpr juce::uint32 kUserBubble  = 0xff2a3040;
    static constexpr juce::uint32 kAgentBubble = 0xff252526;
    static constexpr juce::uint32 kBorder      = 0xff3c3c3c;
    static constexpr juce::uint32 kReconnect   = 0xff5a3020;
}

// ===========================================================================
// MessageBubble
// ===========================================================================

MessageBubble::MessageBubble(const juce::String& text, Role role)
    : role_(role)
{
    addAndMakeVisible(label_);
    label_.setMultiLine(true);
    label_.setReadOnly(true);
    label_.setScrollbarsShown(false);
    label_.setCaretVisible(false);
    label_.setFont(juce::Font(13.0f));
    label_.setColour(juce::TextEditor::backgroundColourId,
                     juce::Colour(0));
    label_.setColour(juce::TextEditor::outlineColourId,
                     juce::Colour(0));
    label_.setColour(juce::TextEditor::textColourId,
                     juce::Colour(colours::kText));
    label_.setText(text, juce::dontSendNotification);
}

void MessageBubble::appendText(const juce::String& extra)
{
    label_.setText(label_.getText() + extra, juce::dontSendNotification);
    // Notify the parent container to reflow.
    if (auto* parent = getParentComponent())
        parent->resized();
}

void MessageBubble::setText(const juce::String& t)
{
    label_.setText(t, juce::dontSendNotification);
    if (auto* parent = getParentComponent())
        parent->resized();
}

int MessageBubble::preferredHeight(int width) const
{
    if (width <= 0)
        return 40;

    // Use a temporary AttributedString to measure line wrapping.
    juce::AttributedString as;
    as.append(label_.getText(), juce::Font(13.0f), juce::Colour(colours::kText));
    as.setWordWrap(juce::AttributedString::byWord);
    as.setJustification(juce::Justification::topLeft);

    juce::TextLayout layout;
    layout.createLayout(as, static_cast<float>(width - 16));

    const int textH = static_cast<int>(std::ceil(layout.getHeight())) + 16;
    return std::max(textH, 40);
}

void MessageBubble::resized()
{
    label_.setBounds(getLocalBounds().reduced(8, 6));
}

void MessageBubble::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced(2.0f, 2.0f);

    juce::uint32 bgColour;
    float cornerRadius = 6.0f;

    switch (role_)
    {
        case Role::User:
            bgColour = colours::kUserBubble;
            break;
        case Role::Agent:
            bgColour = colours::kAgentBubble;
            break;
        case Role::ToolCall:
            bgColour = colours::kSurface;
            cornerRadius = 3.0f;
            break;
        case Role::StatusLine:
        default:
            bgColour = colours::kBackground;
            cornerRadius = 0.0f;
            break;
    }

    g.setColour(juce::Colour(bgColour));
    g.fillRoundedRectangle(bounds, cornerRadius);
}

// ===========================================================================
// MessageHistoryContainer
// ===========================================================================

MessageHistoryContainer::MessageHistoryContainer()
{
    setSize(320, 0);
}

MessageBubble* MessageHistoryContainer::addBubble(const juce::String& text,
                                                    MessageBubble::Role role)
{
    auto* bubble = new MessageBubble(text, role);
    bubbles_.add(bubble);
    addAndMakeVisible(bubble);
    reflowToWidth(getWidth());
    return bubble;
}

void MessageHistoryContainer::reflowToWidth(int w)
{
    if (w <= 0)
        return;

    int y = 4;
    for (auto* b : bubbles_)
    {
        const int h = b->preferredHeight(w);
        b->setBounds(4, y, w - 8, h);
        y += h + 4;
    }

    setSize(w, y + 4);
}

// ===========================================================================
// ChatSidebar
// ===========================================================================

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ChatSidebar::ChatSidebar(AudioEngine& /*audio*/,
                         hathor::control::ControlInterface& ci)
{
    // -----------------------------------------------------------------------
    // Status label (hidden by default)
    // -----------------------------------------------------------------------
    addChildComponent(statusLabel_);
    statusLabel_.setFont(juce::Font(12.0f));
    statusLabel_.setColour(juce::Label::backgroundColourId,
                           juce::Colour(colours::kError).withAlpha(0.2f));
    statusLabel_.setColour(juce::Label::textColourId,
                           juce::Colour(colours::kError));
    statusLabel_.setJustificationType(juce::Justification::centredLeft);
    statusLabel_.setBorderSize(juce::BorderSize<int>(0, 6, 0, 6));

    // -----------------------------------------------------------------------
    // Reconnect banner (hidden by default — shown on disconnect, Req 32.8)
    // -----------------------------------------------------------------------
    addChildComponent(reconnectBanner_);
    reconnectBanner_.setButtonText("Agent disconnected \xe2\x80\x94 click to reconnect");
    reconnectBanner_.setColour(juce::TextButton::buttonColourId,
                                juce::Colour(colours::kReconnect));
    reconnectBanner_.setColour(juce::TextButton::buttonOnColourId,
                                juce::Colour(colours::kReconnect).brighter(0.2f));
    reconnectBanner_.setColour(juce::TextButton::textColourOffId,
                                juce::Colour(colours::kWarning));
    reconnectBanner_.setColour(juce::TextButton::textColourOnId,
                                juce::Colour(colours::kWarning));

    // Wire the reconnect click — hides the banner, re-enables input,
    // and calls session_->restart() (Req 32.8).
    reconnectBanner_.onClick = [this]()
    {
        disconnected_ = false;
        reconnectBanner_.setVisible(false);
        setInputEnabled(true);
        resized();

        if (session_ != nullptr)
        {
            // Optimistic re-enable already done above.
            // Increment socketSeq_ is handled inside restart() (Req 32.8).
            session_->restart(agentExePath_, projectDir_, mcpPath_);
        }
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
    // Chat input field (Req 25.2, 25.6)
    // -----------------------------------------------------------------------
    addAndMakeVisible(inputField_);
    inputField_.setMultiLine(false);
    inputField_.setReturnKeyStartsNewLine(false);
    inputField_.setScrollbarsShown(false);
    inputField_.setFont(juce::Font(13.0f));
    inputField_.setColour(juce::TextEditor::backgroundColourId,
                           juce::Colour(colours::kSurface));
    inputField_.setColour(juce::TextEditor::textColourId,
                           juce::Colour(colours::kText));
    inputField_.setColour(juce::TextEditor::outlineColourId,
                           juce::Colour(colours::kBorder));
    inputField_.setColour(juce::TextEditor::focusedOutlineColourId,
                           juce::Colour(colours::kAccent));
    inputField_.setColour(juce::CaretComponent::caretColourId,
                           juce::Colour(colours::kAccent));
    inputField_.setTextToShowWhenEmpty("Message agent...",
                                        juce::Colour(0xff666666));
    inputField_.addListener(this);

    // -----------------------------------------------------------------------
    // Slider panel (Req 26.1)
    // -----------------------------------------------------------------------
    sliderPanel_ = std::make_unique<SliderPanel>(ci);
    addAndMakeVisible(*sliderPanel_);
}

ChatSidebar::~ChatSidebar()
{
    inputField_.removeListener(this);
}

// ---------------------------------------------------------------------------
// Session wiring
// ---------------------------------------------------------------------------

void ChatSidebar::setSession(AcpAgentSession& session,
                              std::string agentExePath,
                              std::string projectDir,
                              std::string mcpPath)
{
    session_       = &session;
    agentExePath_  = std::move(agentExePath);
    projectDir_    = std::move(projectDir);
    mcpPath_       = std::move(mcpPath);

    // Register all callbacks.  All are invoked on the JUCE message thread by
    // AcpAgentSession (it uses callAsync for every reader-thread notification).

    session_->setOnError([this](std::string reason)
    {
        onError(reason);
    });

    session_->setOnAgentDisconnected([this]()
    {
        onDisconnected();
    });

    session_->setOnAgentMessageChunk([this](std::string text)
    {
        onAgentMessageChunk(text);
    });

    session_->setOnToolCallUpdate([this](nlohmann::json update)
    {
        onToolCallUpdate(std::move(update));
    });

    session_->setOnPermissionRequest([this](int requestId, nlohmann::json options)
    {
        onPermissionRequest(requestId, std::move(options));
    });
}

// ---------------------------------------------------------------------------
// juce::Component overrides
// ---------------------------------------------------------------------------

void ChatSidebar::resized()
{
    auto b = getLocalBounds();

    // Status label (conditionally shown at top)
    if (statusVisible_)
    {
        statusLabel_.setBounds(b.removeFromTop(kStatusH));
    }

    // Reconnect banner (conditionally shown below status, Req 32.8)
    if (disconnected_)
    {
        reconnectBanner_.setBounds(b.removeFromTop(kReconnectH));
    }

    // Permission prompt overlay (sits below status/reconnect row)
    if (permissionPrompt_ != nullptr && permissionPrompt_->isVisible())
    {
        permissionPrompt_->setBounds(b.removeFromTop(kPermissionH));
    }

    // Slider panel at bottom
    sliderPanel_->setBounds(b.removeFromBottom(kSliderH));

    // Input field above slider panel
    inputField_.setBounds(b.removeFromBottom(kInputH));

    // Thin separator between input and history
    b.removeFromBottom(1);

    // History viewport fills the remaining space
    historyViewport_.setBounds(b);
    historyContainer_->reflowToWidth(b.getWidth());
}

void ChatSidebar::paint(juce::Graphics& g)
{
    // Background fill
    g.fillAll(juce::Colour(colours::kSurface));

    // 1 px left border (separates sidebar from editor area)
    g.setColour(juce::Colour(colours::kBorder));
    g.drawVerticalLine(0, 0.0f, static_cast<float>(getHeight()));

    // 1 px separator above the input field
    const int sepY = getHeight() - kSliderH - kInputH - 1;
    if (sepY > 0)
    {
        g.drawHorizontalLine(sepY, 0.0f, static_cast<float>(getWidth()));
    }

    // 1 px separator above the slider panel
    const int sliderSepY = getHeight() - kSliderH;
    if (sliderSepY > 0)
    {
        g.drawHorizontalLine(sliderSepY, 0.0f, static_cast<float>(getWidth()));
    }
}

// ---------------------------------------------------------------------------
// TextEditor::Listener — Enter key (Req 25.2)
// ---------------------------------------------------------------------------

void ChatSidebar::textEditorReturnKeyPressed(juce::TextEditor& editor)
{
    if (&editor != &inputField_)
        return;

    const juce::String rawText = inputField_.getText();

    // Reject empty or whitespace-only content (Req 25.2).
    if (rawText.trim().isEmpty())
        return;

    const std::string text = rawText.toStdString();

    // Add user bubble to the history.
    historyContainer_->addBubble(rawText, MessageBubble::Role::User);
    lastAgentBubble_ = nullptr;
    scrollToBottom();

    // Clear input.
    inputField_.setText(juce::String{}, juce::dontSendNotification);

    // Forward to session (fire-and-forget, non-blocking — Req 32.3).
    if (session_ != nullptr && session_->isReady())
    {
        session_->sendPrompt(text);
    }
}

void ChatSidebar::textEditorTextChanged(juce::TextEditor& editor)
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
// Session callbacks — all run on JUCE message thread
// ---------------------------------------------------------------------------

void ChatSidebar::onError(const std::string& reason)
{
    showStatus("Error: " + juce::String(reason));
    setInputEnabled(false);
}

void ChatSidebar::onDisconnected()
{
    // Req 32.8: show reconnect prompt, disable input.
    disconnected_ = true;
    reconnectBanner_.setVisible(true);
    setInputEnabled(false);
    clearStatus();
    resized();

    // Add a status line to the history for visibility.
    historyContainer_->addBubble("Agent disconnected.",
                                   MessageBubble::Role::StatusLine);
    lastAgentBubble_ = nullptr;
    scrollToBottom();
}

void ChatSidebar::onAgentMessageChunk(const std::string& text)
{
    const juce::String chunk(text);

    if (lastAgentBubble_ != nullptr)
    {
        // Append to the current agent bubble (streaming).
        lastAgentBubble_->appendText(chunk);
    }
    else
    {
        // Start a new agent bubble.
        lastAgentBubble_ = historyContainer_->addBubble(chunk,
                                                         MessageBubble::Role::Agent);
    }

    scrollToBottom();
}

void ChatSidebar::onToolCallUpdate(nlohmann::json update)
{
    // Render a compact status line for tool invocations (Req 32.5).
    // ChatSidebar does NOT re-dispatch — agent + hathor-mcp handle execution.
    std::string label;

    try
    {
        const std::string updateType = update.value("sessionUpdate", "tool_call");
        const std::string toolName   = update.value("toolName", "(unknown tool)");

        if (updateType == "tool_call")
        {
            label = "\xe2\x86\x92 " + toolName;  // → toolName
        }
        else
        {
            const std::string status = update.value("status", "");
            label = "\xe2\x86\x92 " + toolName;
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

void ChatSidebar::onPermissionRequest(int requestId, nlohmann::json options)
{
    // Remove any previous permission prompt.
    if (permissionPrompt_ != nullptr)
    {
        removeChildComponent(permissionPrompt_.get());
        permissionPrompt_.reset();
    }

    // Create a new inline permission prompt (Req 32.6).
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
// Internal helpers
// ---------------------------------------------------------------------------

void ChatSidebar::showStatus(const juce::String& msg)
{
    statusLabel_.setText(msg, juce::dontSendNotification);
    statusVisible_ = true;
    statusLabel_.setVisible(true);
    resized();
}

void ChatSidebar::clearStatus()
{
    statusLabel_.setText({}, juce::dontSendNotification);
    statusVisible_ = false;
    statusLabel_.setVisible(false);
    resized();
}

void ChatSidebar::scrollToBottom()
{
    // Scroll the viewport to show the newest message at the bottom.
    historyViewport_.setViewPosition(
        0,
        std::max(0, historyContainer_->getHeight() - historyViewport_.getHeight()));
}

void ChatSidebar::setInputEnabled(bool enabled)
{
    inputField_.setEnabled(enabled);
    inputField_.setColour(juce::TextEditor::backgroundColourId,
                           juce::Colour(enabled ? colours::kSurface
                                                : colours::kBackground));
    inputField_.repaint();
}

} // namespace hathor::ui
