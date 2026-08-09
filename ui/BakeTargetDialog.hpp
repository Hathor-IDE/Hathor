// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * BakeTargetDialog.hpp — B8-K1 target selection dialog (Studio vs Live Jam).
 *
 * Presented before a Bake to Song operation begins.  The user explicitly
 * chooses the destination target:
 *
 *   Studio  — permanent project asset, survives restarts.
 *   Live Jam — temporary session asset, cleaned up at session end.
 *
 * Studio is always the default selection.  The user must actively click
 * "Live Jam" to switch — there is no ambiguous or silent default.
 *
 * The dialog is modal (uses juce::DialogWindow::showModalDialog) so the caller
 * is blocked until a decision is made or the dialog is cancelled.
 *
 * Requirements: B8-K6 §3, B8-K1 §4, B8-K1 §5
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "HathorLookAndFeel.hpp"
#include "AssetTarget.hpp"

#include <functional>

namespace hathor::ui {

/**
 * BakeTargetDialog — a JUCE modal dialog for selecting the bake destination.
 *
 * Displays the two targets with clear lifetime descriptions, pre-selects
 * Studio (the default), and reports the choice back through the callback.
 */
class BakeTargetDialog : public juce::Component,
                         private juce::Button::Listener
{
public:
    using TargetSelectedCallback = std::function<void(hathor::AssetTarget)>;

    explicit BakeTargetDialog(TargetSelectedCallback onSelected);
    ~BakeTargetDialog() override = default;

    // juce::Component overrides
    void paint(juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;

    /// Returns the currently selected target (Studio or LiveJam).
    hathor::AssetTarget selectedTarget() const noexcept { return selectedTarget_; }

private:
    // juce::Button::Listener
    void buttonClicked(juce::Button* button) override;

    // ---------------------------------------------------------------------------
    // Layout constants
    // ---------------------------------------------------------------------------
    static constexpr int kDialogWidth  = 420;
    static constexpr int kDialogHeight = 280;
    static constexpr int kButtonHeight = 36;
    static constexpr int kButtonGap    = 12;

    // ---------------------------------------------------------------------------
    // Data
    // ---------------------------------------------------------------------------
    TargetSelectedCallback onSelected_;

    juce::TextButton studioButton_;
    juce::TextButton liveJamButton_;
    juce::TextButton bakeButton_;
    juce::TextButton cancelButton_;

    hathor::AssetTarget selectedTarget_{ hathor::AssetTarget::Studio };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BakeTargetDialog)
};

// ---------------------------------------------------------------------------
// Static launcher — shows the dialog modally and invokes @p onSelected
// with the chosen target when the user confirms.
//
// Returns true if the user confirmed; false if cancelled/dismissed.
// ---------------------------------------------------------------------------

/**
 * Show the BakeTargetDialog modally.
 *
 * @param parent      Parent component for modality (nullptr = desktop).
 * @param onSelected  Called with the chosen target when the user clicks "Bake".
 *                    Not called if the user cancels.
 * @return true if the user confirmed; false if cancelled/dismissed.
 */
bool showBakeTargetDialog(juce::Component* parent,
                          BakeTargetDialog::TargetSelectedCallback onSelected);

} // namespace hathor::ui
