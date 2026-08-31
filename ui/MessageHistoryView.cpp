// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * MessageHistoryView.cpp — MessageBubble + MessageHistoryContainer implementation.
 *
 * Extracted from ChatSidebar.cpp during B6/C2 refactor to allow reuse
 * by ChatThread (per-thread message history).
 *
 * Requirements: 25.1, 25.5
 */

#include "MessageHistoryView.hpp"
#include "HathorLookAndFeel.hpp"

#include <algorithm>
#include <cmath>

namespace hathor::ui {

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
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    label_.setFont(HathorLookAndFeel::uiFontRegular(13.0f));
    label_.setColour(juce::TextEditor::backgroundColourId,
                     juce::Colour(0));
    label_.setColour(juce::TextEditor::outlineColourId,
                     juce::Colour(0));
    label_.setColour(juce::TextEditor::textColourId,
                     palette.textPrimary);
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
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    as.append(label_.getText(), HathorLookAndFeel::uiFontRegular(13.0f), palette.textPrimary);
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
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    juce::Colour bgColour;
    float cornerRadius = 6.0f;

    switch (role_)
    {
        case Role::User:
            bgColour = palette.surfaceContainer;
            break;
        case Role::Agent:
            bgColour = palette.surfaceContainer;
            break;
        case Role::ToolCall:
            bgColour = palette.surfaceContainer;
            cornerRadius = 3.0f;
            break;
        case Role::StatusLine:
        default:
            bgColour = palette.background;
            cornerRadius = 0.0f;
            break;
    }

    g.setColour(bgColour);
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

} // namespace hathor::ui
