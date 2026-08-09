// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * BakeProgressDialog.cpp — implementation.
 *
 * Requirements: B8-K6 §5, B8-K6 §6
 */

#include "BakeProgressDialog.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static juce::String stageToString(BakeProgressDialog::Stage s) noexcept
{
    switch (s)
    {
        case BakeProgressDialog::Stage::Preparing:    return "Preparing";
        case BakeProgressDialog::Stage::Rendering:    return "Rendering";
        case BakeProgressDialog::Stage::Publishing:   return "Publishing";
        case BakeProgressDialog::Stage::Registering:  return "Registering sample";
        case BakeProgressDialog::Stage::ShuttingDown: return "Shutting down ChucK runtime";
        case BakeProgressDialog::Stage::Finishing:    return "Finishing";
        case BakeProgressDialog::Stage::Completed:    return "Completed";
    }
    return "Unknown";
}

static juce::String stageDescription(BakeProgressDialog::Stage s) noexcept
{
    switch (s)
    {
        case BakeProgressDialog::Stage::Preparing:    return "Validating source and render parameters";
        case BakeProgressDialog::Stage::Rendering:    return "Rendering audio from ChucK instrument";
        case BakeProgressDialog::Stage::Publishing:   return "Writing WAV to disk";
        case BakeProgressDialog::Stage::Registering:  return "Registering sample in SampleBank";
        case BakeProgressDialog::Stage::ShuttingDown: return "Shutting down ChucK VM and thread";
        case BakeProgressDialog::Stage::Finishing:    return "Refreshing autocomplete and Explorer";
        case BakeProgressDialog::Stage::Completed:    return "Bake complete";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

BakeProgressDialog::BakeProgressDialog(juce::String instrumentName,
                                       hathor::AssetTarget target,
                                       juce::Component* parent)
    : juce::Component()
    , progressValue_(0.0)
    , progressBar_(progressValue_)
    , titleLabel_()
    , stageLabel_()
    , detailLabel_()
    , closeButton_()
{
    instrumentName_ = std::move(instrumentName);
    target_ = target;

    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    setName("BakeProgressDialog");
    setAlwaysOnTop(true);
    setOpaque(false);
    setSize(kWindowWidth, kWindowHeight);

    // Position centred on parent or primary display.
    if (parent != nullptr)
    {
        const juce::Rectangle<int> parentBounds = parent->getBounds();
        const int cx = parentBounds.getCentreX();
        const int cy = parentBounds.getCentreY();
        setBounds(cx - kWindowWidth / 2, cy - kWindowHeight / 2,
                  kWindowWidth, kWindowHeight);
    }
    else if (const auto* primaryDisplay = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const juce::Rectangle<int> display = primaryDisplay->userArea;
        setBounds(display.getCentreX() - kWindowWidth / 2,
                  display.getCentreY() - kWindowHeight / 2,
                  kWindowWidth, kWindowHeight);
    }

    // Title
    titleLabel_.setText("Baking \"" + instrumentName_ + "\"…",
                        juce::dontSendNotification);
    titleLabel_.setFont(HathorLookAndFeel::fontSemiBold(16.0f));
    titleLabel_.setColour(juce::Label::textColourId, palette.textPrimary);
    titleLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel_);

    // Stage label
    stageLabel_.setFont(HathorLookAndFeel::fontMedium(13.0f));
    stageLabel_.setColour(juce::Label::textColourId, palette.textPrimary);
    stageLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(stageLabel_);

    // Detail label
    detailLabel_.setFont(HathorLookAndFeel::fontRegular(11.0f));
    detailLabel_.setColour(juce::Label::textColourId, palette.textSecondary);
    detailLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(detailLabel_);

    // Progress bar
    progressBar_.setBounds(8, kWindowHeight - 30, kWindowWidth - 16, kProgressBarHeight);
    // ProgressBar uses juce::ResizableCornerComponent for styling — set colours via LookAndFeel.
    addAndMakeVisible(progressBar_);

    // Close button — top-right corner.
    closeButton_.setButtonText("");
    closeButton_.setTooltip("Close");
    closeButton_.onClick = [this]() {
        setVisible(false);
        if (juce::DialogWindow* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
    };
    closeButton_.setBounds(kWindowWidth - 24, 4, 16, 16);
    addAndMakeVisible(closeButton_);

    updateStageText();
}

BakeProgressDialog::~BakeProgressDialog()
{
    stopTimer();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void BakeProgressDialog::setStage(Stage stage) noexcept
{
    currentStage_ = stage;
    updateStageText();

    if (stage == Stage::Rendering)
        progressBar_.setVisible(true);

    repaint();

    if (stage == Stage::Completed)
        complete();
    else if (stage != Stage::Preparing && !isTimerRunning())
        startTimer(50);
}

void BakeProgressDialog::setProgress(double fraction) noexcept
{
    progressValue_ = juce::jlimit(0.0, 1.0, fraction);
    repaint();
}

void BakeProgressDialog::complete()
{
    isComplete_ = true;
    isFailed_ = false;

    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    stageLabel_.setColour(juce::Label::textColourId, palette.accent);

    const juce::String targetStr =
        (target_ == hathor::AssetTarget::Studio) ? "Studio asset"
                                                   : "Live Jam session asset";

    const juce::String msg = "Baked \"" + instrumentName_ + "\" to Song\n"
                             + targetStr + " created";

    statusMessage_ = msg;
    stageLabel_.setText("Completed", juce::dontSendNotification);
    detailLabel_.setText(msg, juce::dontSendNotification);
    progressBar_.setVisible(false);
    progressValue_ = 1.0;

    // Auto-dismiss after a short delay.
    autoDismissCount_.store(kAutoDismissMs / 50, std::memory_order_relaxed);
    if (!isTimerRunning())
        startTimer(50);
}

void BakeProgressDialog::fail(const juce::String& errorMessage)
{
    isComplete_ = false;
    isFailed_ = true;
    errorMessage_ = errorMessage;

    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    stageLabel_.setColour(juce::Label::textColourId, palette.error);

    stageLabel_.setText("Failed: " + stageToString(currentStage_),
                        juce::dontSendNotification);
    detailLabel_.setText(errorMessage, juce::dontSendNotification);
    closeButton_.setVisible(true);

    stopTimer();
}

// ---------------------------------------------------------------------------
// juce::Timer
// ---------------------------------------------------------------------------

void BakeProgressDialog::timerCallback()
{
    if (isComplete_)
    {
        int remaining = autoDismissCount_.load(std::memory_order_relaxed) - 1;
        if (remaining <= 0)
        {
            stopTimer();
            setVisible(false);
            if (juce::DialogWindow* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState(0);
        }
        else
        {
            autoDismissCount_.store(remaining, std::memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------------------
// juce::Component overrides
// ---------------------------------------------------------------------------

void BakeProgressDialog::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    g.fillAll(palette.surfaceLow.withAlpha(0.95f));

    // Window outline with rounded corners.
    g.setColour(palette.surfaceHighest.withAlpha(0.3f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1),
                           HathorLookAndFeel::Radius::small, 1.0f);
}

void BakeProgressDialog::resized()
{
    titleLabel_.setBounds(0, 12, kWindowWidth, 24);
    closeButton_.setBounds(kWindowWidth - 24, 4, 16, 16);

    stageLabel_.setBounds(16, 50, kWindowWidth - 32, 20);
    detailLabel_.setBounds(16, 74, kWindowWidth - 32, 16);
    progressBar_.setBounds(8, kWindowHeight - 30, kWindowWidth - 16, kProgressBarHeight);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void BakeProgressDialog::updateStageText()
{
    stageLabel_.setText(stageToString(currentStage_),
                        juce::dontSendNotification);
    detailLabel_.setText(stageDescription(currentStage_),
                         juce::dontSendNotification);
}

} // namespace hathor::ui
