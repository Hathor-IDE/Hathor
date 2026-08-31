// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * PermissionPromptComponent.cpp
 *
 * Inline permission prompt rendered inside ChatSidebar.
 * All methods run on the JUCE message thread.
 *
 * Requirements: 32.6
 */

#include "PermissionPromptComponent.hpp"
#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

PermissionPromptComponent::PermissionPromptComponent(int requestId,
                                                     const nlohmann::json& options,
                                                     OnRespondFn onRespond)
    : requestId_ (requestId)
    , options_   (options)
    , onRespond_ (std::move(onRespond))
{
    // -----------------------------------------------------------------------
    // Prompt label
    // -----------------------------------------------------------------------
    const auto& palette = HathorLookAndFeel::fromComponent(promptLabel_).getPalette();

    promptLabel_.setText("Agent is requesting permission:", juce::dontSendNotification);
    promptLabel_.setColour(juce::Label::textColourId, palette.textPrimary);
    promptLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(promptLabel_);

    // -----------------------------------------------------------------------
    // Countdown label
    // -----------------------------------------------------------------------
    countdownLabel_.setText(juce::String(secondsLeft_) + "s",
                            juce::dontSendNotification);
    countdownLabel_.setColour(juce::Label::textColourId, palette.textSecondary);
    countdownLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(countdownLabel_);

    // -----------------------------------------------------------------------
    // Option buttons — one per element in the options array
    // -----------------------------------------------------------------------
    // ACP v1 spec (PermissionOption): each option has optionId, name, kind.
    // We prefer "name" for the display label and "optionId" for the value
    // sent back in the RequestPermissionOutcome. Falls back gracefully
    // when an agent omits a field.
    if (options_.is_array())
    {
        for (std::size_t i = 0; i < options_.size(); ++i)
        {
            const auto& opt = options_[i];

            // Display label: prefer "name", then "title", then index.
            juce::String buttonLabel;
            if (opt.contains("name") && opt["name"].is_string())
                buttonLabel = juce::String(opt["name"].get<std::string>());
            else if (opt.contains("title") && opt["title"].is_string())
                buttonLabel = juce::String(opt["title"].get<std::string>());
            else
                buttonLabel = "Option " + juce::String(static_cast<int>(i));

            // optionId: prefer "optionId", then "id", then index as string.
            std::string optionId;
            if (opt.contains("optionId") && opt["optionId"].is_string())
                optionId = opt["optionId"].get<std::string>();
            else if (opt.contains("id") && opt["id"].is_string())
                optionId = opt["id"].get<std::string>();
            else if (opt.contains("id") && opt["id"].is_number_integer())
                optionId = std::to_string(opt["id"].get<int>());
            else
                optionId = std::to_string(i);

            auto* btn = optionButtons_.add(new juce::TextButton(buttonLabel));

            // Capture optionId by value into the lambda.
            btn->onClick = [this, id = optionId]
            {
                respond(id);
            };

            addAndMakeVisible(btn);
        }
    }

    // If no options were provided, add a single "Dismiss" button that cancels.
    if (optionButtons_.isEmpty())
    {
        auto* btn = optionButtons_.add(new juce::TextButton("Dismiss"));
        btn->onClick = [this] { respond("cancelled"); };
        addAndMakeVisible(btn);
    }
}

PermissionPromptComponent::~PermissionPromptComponent()
{
    stopTimer();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void PermissionPromptComponent::start()
{
    // Tick once per second.
    startTimerHz(1);
}

void PermissionPromptComponent::cancelIfPending()
{
    if (!responded_)
        respond("cancelled");
}

// ---------------------------------------------------------------------------
// juce::Timer
// ---------------------------------------------------------------------------

void PermissionPromptComponent::timerCallback()
{
    --secondsLeft_;

    countdownLabel_.setText(juce::String(secondsLeft_) + "s",
                            juce::dontSendNotification);

    if (secondsLeft_ <= 0)
        respond("cancelled");
}

// ---------------------------------------------------------------------------
// juce::Component
// ---------------------------------------------------------------------------

void PermissionPromptComponent::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Slightly lighter background rectangle to visually distinguish the prompt
    // from normal chat bubbles.
    g.setColour(palette.surfaceHigh);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.f);

    // Subtle border.
    g.setColour(palette.surfaceHighest);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.f, 1.f);
}

void PermissionPromptComponent::resized()
{
    constexpr int kPadding       = 8;
    constexpr int kLabelHeight   = 20;
    constexpr int kButtonHeight  = 28;
    constexpr int kButtonSpacing = 6;

    auto area = getLocalBounds().reduced(kPadding);

    // Top row: prompt label (left) + countdown label (right, fixed width).
    auto topRow = area.removeFromTop(kLabelHeight);
    countdownLabel_.setBounds(topRow.removeFromRight(40));
    promptLabel_.setBounds(topRow);

    area.removeFromTop(kPadding);

    // Option buttons in a horizontal row (wraps if not enough width).
    if (!optionButtons_.isEmpty())
    {
        const int numButtons   = optionButtons_.size();
        const int totalSpacing = kButtonSpacing * (numButtons - 1);
        const int btnWidth     = (area.getWidth() - totalSpacing) / numButtons;

        auto btnRow = area.removeFromTop(kButtonHeight);
        for (int i = 0; i < numButtons; ++i)
        {
            auto btnBounds = btnRow.removeFromLeft(btnWidth);
            if (i < numButtons - 1)
                btnRow.removeFromLeft(kButtonSpacing);
            optionButtons_[i]->setBounds(btnBounds);
        }
    }
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void PermissionPromptComponent::respond(const std::string& optionId)
{
    if (responded_)
        return;

    responded_ = true;
    stopTimer();

    // Notify parent before hiding so they can update layout.
    if (onRespond_)
        onRespond_(requestId_, optionId);

    setVisible(false);
}

} // namespace hathor::ui
