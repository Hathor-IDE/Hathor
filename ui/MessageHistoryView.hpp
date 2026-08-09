// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * MessageHistoryView.hpp — Message bubbles + scroll history container.
 *
 * Shared between ChatThread and ChatSidebar. Provides the scrollable
 * message history used in the chat sidebar.
 *
 * Requirements: 25.1, 25.5
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

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

    void appendText(const juce::String& extra);
    void setText(const juce::String& t);
    int preferredHeight(int width) const;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    juce::TextEditor label_;
    Role             role_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MessageBubble)
};

/**
 * Holds an ordered list of MessageBubble children and stacks them vertically.
 * The Viewport scrolls this component.
 */
class MessageHistoryContainer : public juce::Component
{
public:
    MessageHistoryContainer();

    MessageBubble* addBubble(const juce::String& text, MessageBubble::Role role);
    void reflowToWidth(int w);

private:
    juce::OwnedArray<MessageBubble> bubbles_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MessageHistoryContainer)
};

} // namespace hathor::ui
