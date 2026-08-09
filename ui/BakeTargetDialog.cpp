// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * BakeTargetDialog.cpp — implementation.
 *
 * Requirements: B8-K6 §3, B8-K1 §4, B8-K1 §5
 */

#include "BakeTargetDialog.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

BakeTargetDialog::BakeTargetDialog(TargetSelectedCallback onSelected)
    : onSelected_(std::move(onSelected))
{
    // Palette not needed here — paint() fetches it lazily.

    studioButton_.setButtonText("Studio");
    studioButton_.setTooltip("Permanent project asset — survives closing and reopening the project");
    studioButton_.setRadioGroupId(42);
    studioButton_.setClickingTogglesState(true);
    studioButton_.setToggleState(true, juce::dontSendNotification);
    studioButton_.setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
    addAndMakeVisible(studioButton_);

    liveJamButton_.setButtonText("Live Jam");
    liveJamButton_.setTooltip("Temporary session asset — cleaned up at session end");
    liveJamButton_.setRadioGroupId(42);
    liveJamButton_.setClickingTogglesState(true);
    liveJamButton_.setToggleState(false, juce::dontSendNotification);
    liveJamButton_.setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
    addAndMakeVisible(liveJamButton_);

    bakeButton_.setButtonText("Bake");
    bakeButton_.setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
    addAndMakeVisible(bakeButton_);

    cancelButton_.setButtonText("Cancel");
    cancelButton_.setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
    addAndMakeVisible(cancelButton_);
}

// ---------------------------------------------------------------------------
// juce::Component overrides
// ---------------------------------------------------------------------------

void BakeTargetDialog::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    g.fillAll(palette.surface);

    // Title
    g.setColour(palette.textPrimary);
    g.setFont(HathorLookAndFeel::fontSemiBold(HathorLookAndFeel::Typography::headlineMd));
    g.drawText("Bake to Song", getLocalBounds().removeFromTop(40),
               juce::Justification::centredTop, false);

    // Target descriptions
    const int descTop = 70;
    const int descWidth = getWidth() - 32;

    // Studio description
    juce::Rectangle<int> studioDescRect(16, descTop, descWidth, 40);
    g.setColour(palette.textSecondary);
    g.setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
    g.drawText("Studio — Permanent project asset\nStored in the project's managed asset area",
               studioDescRect,
               juce::Justification::centred, true);

    // Live Jam description
    juce::Rectangle<int> liveJamDescRect(16, descTop + 50, descWidth, 40);
    g.drawText("Live Jam — Temporary session asset\nCleaned up at session end",
               liveJamDescRect,
               juce::Justification::centred, true);
}

void BakeTargetDialog::resized()
{
    setSize(kDialogWidth, kDialogHeight);

    auto bounds = getLocalBounds().withTrimmedTop(50).reduced(16);

    studioButton_.setBounds(bounds.removeFromTop(kButtonHeight));
    bounds.removeFromTop(kButtonGap);
    liveJamButton_.setBounds(bounds.removeFromTop(kButtonHeight));
    bounds.removeFromTop(12);

    // Bottom button row
    const int footerHeight = 40;
    auto footer = bounds.removeFromBottom(footerHeight);
    const int btnW = 100;
    cancelButton_.setBounds(footer.removeFromRight(btnW).removeFromLeft(btnW));
    footer = footer.reduced(8, 0);
    bakeButton_.setBounds(footer.removeFromRight(btnW).translated(0, 0).withX(footer.getX()));
}

void BakeTargetDialog::lookAndFeelChanged()
{
    studioButton_.setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
    liveJamButton_.setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
    bakeButton_.setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
    cancelButton_.setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
}

// ---------------------------------------------------------------------------
// juce::Button::Listener
// ---------------------------------------------------------------------------

void BakeTargetDialog::buttonClicked(juce::Button* button)
{
    if (button == &studioButton_)
    {
        selectedTarget_ = hathor::AssetTarget::Studio;
    }
    else if (button == &liveJamButton_)
    {
        selectedTarget_ = hathor::AssetTarget::LiveJam;
    }
    else if (button == &bakeButton_)
    {
        if (onSelected_)
            onSelected_(selectedTarget_);
        // The dialog window is dismissed by the caller's callback chain.
        if (juce::DialogWindow* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(/*result=*/1);
    }
    else if (button == &cancelButton_)
    {
        if (juce::DialogWindow* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(/*result=*/0);
    }
}

// ---------------------------------------------------------------------------
// Modal launcher
// ---------------------------------------------------------------------------

bool showBakeTargetDialog(juce::Component* parent,
                          BakeTargetDialog::TargetSelectedCallback onSelected)
{
    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle            = "Bake to Song";
    opts.dialogBackgroundColour = HathorLookAndFeel::defaultPalette().surface;
    opts.content.setOwned(new BakeTargetDialog(std::move(onSelected)));
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar    = false;
    opts.resizable            = false;
    opts.componentToCentreAround = parent;

#if JUCE_MODAL_LOOPS_PERMITTED
    return opts.launchAsync() != nullptr;
#else
    juce::ignoreUnused(parent, onSelected);
    return false;
#endif
}

} // namespace hathor::ui
