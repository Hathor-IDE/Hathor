// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

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

    // Inter UI font for hover tooltip prose (proportional).
    juce::FontOptions fontOpts;
    fontOpts = fontOpts.withTypeface(HathorLookAndFeel::interRegularTypeface());
    fontOpts = fontOpts.withHeight(12.0f);
    juce::Font font(fontOpts);

    juce::AttributedString attr;
    attr.append(juce::String(text_), font, juce::Colours::white);

    juce::TextLayout layout;
    layout.createLayout(attr, kMaxWidth - kPadding * 2);

    displayWidth_ = kMaxWidth;
    displayHeight_ = juce::jlimit(20, 300, static_cast<int>(layout.getHeight()) + kPadding * 2);

    setSize(displayWidth_, displayHeight_);

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

    g.fillAll(palette.surface.withAlpha(0.95f));

    g.setColour(palette.accent.withAlpha(0.5f));
    g.drawRect(getLocalBounds(), 1);

    // Inter UI font for hover tooltip (proportional).
    juce::FontOptions fontOpts;
    fontOpts = fontOpts.withTypeface(HathorLookAndFeel::interRegularTypeface());
    fontOpts = fontOpts.withHeight(12.0f);
    juce::Font font(fontOpts);

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
