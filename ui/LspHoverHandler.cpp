// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * LspHoverHandler.cpp — implementation of the hover tooltip.
 *
 * Requirement references: AI-4
 */

#include "LspHoverHandler.hpp"
#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

LspHoverHandler::LspHoverHandler(DismissCallback onDismiss)
    : onDismiss_(std::move(onDismiss))
{
    setInterceptsMouseClicks(false, false);
    setVisible(false);
}

void LspHoverHandler::showHover(const lsp::Hover& content,
                                const juce::Point<int>& anchor)
{
    // Build the text from all content parts
    text_.clear();
    for (const auto& mc : content.contents)
    {
        if (!text_.empty())
            text_ += "\n";
        text_ += mc.value;
    }

    if (text_.empty())
    {
        dismiss();
        return;
    }

    anchor_ = anchor;
    visible_ = true;

    // Measure text
    juce::Font font(juce::Font::getDefaultSansSerifFontName(), 12.0f, juce::Font::plain);
    juce::TextLayout layout;
    juce::AttributedString attr(text_, font, 13.0f,
                                 juce::Colours::white);

    juce::TextLayout::createLayout(layout, attr, kMaxWidth - kPadding * 2);

    displayWidth_ = kMaxWidth;
    displayHeight_ = juce::jlimit(20, 300, layout.getHeight() + kPadding * 2);

    setSize(displayWidth_, displayHeight_);

    // Position: place below the anchor, adjust if near bottom of parent
    int x = anchor_.x;
    int y = anchor_.y + 16;

    if (auto* parent = getParentComponent())
    {
        if (x + displayWidth_ > parent->getWidth())
            x = parent->getWidth() - displayWidth_ - 4;
        if (x < 0)
            x = 4;
        if (y + displayHeight_ > parent->getHeight())
            y = anchor_.y - displayHeight_ - 4;
        if (y < 0)
            y = 4;
    }

    setTopLeftPosition(x, y);
    setVisible(true);
    toFront(false);

    startTimer(kAutoDismissMs);
}

void LspHoverHandler::dismiss()
{
    if (!visible_)
        return;

    visible_ = false;
    text_.clear();
    setVisible(false);
    stopTimer();

    if (onDismiss_)
        onDismiss_();
}

void LspHoverHandler::paint(juce::Graphics& g)
{
    if (!visible_ || text_.empty())
        return;

    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Background
    g.fillAll(palette.surface.withAlpha(0.95f));

    // Border
    g.setColour(palette.accent.withAlpha(0.5f));
    g.drawRect(getLocalBounds(), 1);

    // Text
    juce::Font font(juce::Font::getDefaultMonospaceFontName(), 12.0f, juce::Font::plain);
    g.setColour(palette.textPrimary);
    g.setFont(font);
    g.drawFittedText(juce::String(text_),
                     juce::Rectangle<int>(kPadding, kPadding,
                                          getWidth() - kPadding * 2,
                                          getHeight() - kPadding * 2),
                     juce::Justification::topLeft,
                     0, 1.0f);
}

void LspHoverHandler::resized()
{
    // No child components to layout
}

void LspHoverHandler::timerCallback()
{
    dismiss();
}

void LspHoverHandler::mouseDown(const juce::MouseEvent& /*e*/)
{
    dismiss();
}

} // namespace hathor::ui
