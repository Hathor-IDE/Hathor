// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * SliderPanel.cpp — BPM + master-gain juce::Slider implementation.
 *
 * Requirements: 26.1, 26.2, 26.3, 26.4, 26.9
 */

#include "SliderPanel.hpp"
#include "HathorLookAndFeel.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace hathor::ui {

// ===========================================================================
// Construction / destruction
// ===========================================================================

SliderPanel::SliderPanel(hathor::control::ControlInterface& ci)
    : ci_(ci)
{
    setupBpmSlider();
    setupGainSlider();

    addAndMakeVisible(bpmLabel_);
    addAndMakeVisible(bpmSlider_);
    addAndMakeVisible(gainLabel_);
    addAndMakeVisible(gainSlider_);
}

SliderPanel::~SliderPanel()
{
    // 0.5/C2: cancel any pending async dispatch before members die.
    cancelPendingUpdate();
}

// ===========================================================================
// Slider setup helpers
// ===========================================================================

void SliderPanel::setupBpmSlider()
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // -----------------------------------------------------------------------
    // BPM label (Req 26.1)
    // -----------------------------------------------------------------------
    bpmLabel_.setText("BPM", juce::dontSendNotification);
    bpmLabel_.setFont(HathorLookAndFeel::uiFontMedium(
        HathorLookAndFeel::Typography::labelMd));
    bpmLabel_.setColour(juce::Label::textColourId,
                        palette.textSecondary);
    bpmLabel_.setJustificationType(juce::Justification::centredLeft);

    // -----------------------------------------------------------------------
    // BPM slider (Req 26.1, 26.2)
    // Range: [20, 400], step 1, initial 120
    // -----------------------------------------------------------------------
    bpmSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    bpmSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
    bpmSlider_.setRange(20.0, 400.0, 1.0);  // step = 1 (integer BPM)
    bpmSlider_.setValue(120.0, juce::dontSendNotification);

    // -----------------------------------------------------------------------
    // BPM onValueChange — post "bpm <value>" via AsyncUpdater (0.5/C2)
    // Does NOT dispatch if new integer value == last dispatched BPM.
    // -----------------------------------------------------------------------
    bpmSlider_.onValueChange = [this]()
    {
        if (suppressDispatch_)
            return;

        const int newBpm = static_cast<int>(std::round(bpmSlider_.getValue()));

        // Only dispatch if the integer BPM actually changed (Req 26.2).
        if (newBpm == lastDispatchedBpm_)
            return;

        lastDispatchedBpm_ = newBpm;

        pendingBpm_  = newBpm;
        bpmPending_  = true;
        postDispatch();
    };
}

void SliderPanel::setupGainSlider()
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // -----------------------------------------------------------------------
    // Gain label (Req 26.1)
    // -----------------------------------------------------------------------
    gainLabel_.setText("Gain", juce::dontSendNotification);
    gainLabel_.setFont(HathorLookAndFeel::uiFontMedium(
        HathorLookAndFeel::Typography::labelMd));
    gainLabel_.setColour(juce::Label::textColourId,
                         palette.textSecondary);
    gainLabel_.setJustificationType(juce::Justification::centredLeft);

    // -----------------------------------------------------------------------
    // Gain slider (Req 26.1, 26.3)
    // Range: [0.0, 2.0], continuous (no step), initial 1.0
    // -----------------------------------------------------------------------
    gainSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
    gainSlider_.setRange(0.0, 2.0);  // continuous — no step
    gainSlider_.setValue(1.0, juce::dontSendNotification);

    // -----------------------------------------------------------------------
    // Gain onValueChange — post "set-gain <value>" via AsyncUpdater (0.5/C2).
    // Value is formatted to 2 decimal places and clamped to [0.0, 2.0].
    // -----------------------------------------------------------------------
    gainSlider_.onValueChange = [this]()
    {
        if (suppressDispatch_)
            return;

        // Clamp to [0.0, 2.0] (Req 26.3)
        const float rawGain = static_cast<float>(gainSlider_.getValue());
        const float gain    = std::clamp(rawGain, 0.0f, 2.0f);

        // Format to exactly 2 decimal places (Req 26.3)
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << gain;

        pendingGain_ = std::clamp(gain, 0.0f, 2.0f);
        gainPending_ = true;
        postDispatch();
    };
}

// ===========================================================================
// Async dispatch (0.5/C2) — debounced, message-thread only
// ===========================================================================

void SliderPanel::postDispatch()
{
    // AsyncUpdater coalesces: multiple triggers before the next message-loop
    // pass result in a single handleAsyncUpdate() with the latest values.
    triggerAsyncUpdate();
}

void SliderPanel::handleAsyncUpdate()
{
    if (bpmPending_)
    {
        bpmPending_ = false;
        ci_.dispatch("bpm " + std::to_string(pendingBpm_));
    }

    if (gainPending_)
    {
        gainPending_ = false;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2)
            << std::clamp(pendingGain_, 0.0f, 2.0f);
        ci_.dispatch("set-gain " + oss.str());
    }
}

// ===========================================================================
// Bidirectional sync accessors (Req 26.4, 26.9)
// ===========================================================================

int SliderPanel::bpmDisplayValue() const
{
    return static_cast<int>(std::round(bpmSlider_.getValue()));
}

float SliderPanel::gainDisplayValue() const
{
    return static_cast<float>(gainSlider_.getValue());
}

void SliderPanel::setBpmDisplay(int bpm)
{
    // Suppress dispatch so onValueChange does not fire a command (Req 26.4).
    suppressDispatch_ = true;
    bpmSlider_.setValue(static_cast<double>(bpm), juce::dontSendNotification);
    suppressDispatch_ = false;
}

void SliderPanel::setGainDisplay(float g)
{
    // Suppress dispatch so onValueChange does not fire a command (Req 26.9).
    suppressDispatch_ = true;
    gainSlider_.setValue(static_cast<double>(g), juce::dontSendNotification);
    suppressDispatch_ = false;
}

// ===========================================================================
// juce::Component overrides
// ===========================================================================

void SliderPanel::resized()
{
    auto b = getLocalBounds();

    // Divide vertically into two equal rows: BPM (top) + Gain (bottom).
    const int rowH = b.getHeight() / 2;

    auto bpmRow  = b.removeFromTop(rowH);
    auto gainRow = b; // remainder

    // Within each row: label on the left (40 px), slider fills the rest.
    constexpr int kLabelW = 40;

    bpmLabel_.setBounds(bpmRow.removeFromLeft(kLabelW));
    bpmSlider_.setBounds(bpmRow);

    gainLabel_.setBounds(gainRow.removeFromLeft(kLabelW));
    gainSlider_.setBounds(gainRow);
}

void SliderPanel::paint(juce::Graphics& g)
{
    // Dark surface background matching ChatSidebar's bottom zone.
    g.fillAll(HathorLookAndFeel::fromComponent(*this).getPalette().surfaceLow);
}

} // namespace hathor::ui
