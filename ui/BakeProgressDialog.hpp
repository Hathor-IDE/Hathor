// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * BakeProgressDialog.hpp — B8-K6 in-progress bake status display.
 *
 * Shows the current bake stage and progress while a background render
 * (B8-K2) is in flight.  Presented as a non-modal floating window so the
 * user can see progress without blocking interaction with the rest of the
 * app, yet it is clearly visible.
 *
 * Stages (B8-K6 §5):
 *   Preparing… → Rendering… → Publishing… → Registering sample… → Finishing… → Completed
 *
 * The dialog is driven by BakeOrchestrator via stageChanged() / completed().
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "HathorLookAndFeel.hpp"
#include "AssetTarget.hpp"
#include "ChuckRenderWriter.hpp"

#include <atomic>
#include <string>

namespace hathor::ui {

/**
 * BakeProgressDialog — floating status window for an in-progress bake.
 *
 * Lifecycle:
 *   1. Constructed when a bake starts (BakeOrchestrator creates it).
 *   2. Visible on screen — shows stage + progress bar.
 *   3. Dismissed (or self-dismisses) when the bake completes or fails.
 */
class BakeProgressDialog : public juce::Component,
                           private juce::Timer
{
public:
    enum class Stage
    {
        Preparing,    ///< validating source + render parameters
        Rendering,    ///< B8-K2 background render in progress
        Publishing,   ///< WAV being written to disk (atomic rename)
        Registering,  ///< B8-K4: SampleBank registration
        ShuttingDown, ///< B8-K3: VM/thread teardown
        Finishing,    ///< finalising SampleBank + autocomplete + Explorer refresh
        Completed,    ///< all stages done
    };

    /**
     * @param instrumentName  The .ck instrument stem (e.g. "acid_bass").
     * @param target          The selected bake target (Studio/LiveJam).
     * @param parent          Parent component for positioning.
     */
    BakeProgressDialog(juce::String instrumentName,
                       hathor::AssetTarget target,
                       juce::Component* parent = nullptr);

    ~BakeProgressDialog() override;

    // -----------------------------------------------------------------------
    // Public API — called by BakeOrchestrator
    // -----------------------------------------------------------------------

    /// Update the visible stage (called on the JUCE message thread).
    void setStage(Stage stage) noexcept;

    /// Update progress (0.0–1.0) for the progress bar.
    void setProgress(double fraction) noexcept;

    /// Show a success message and auto-dismiss after a short delay.
    void complete();

    /// Show a failure message; the dialog remains visible until dismissed.
    void fail(const juce::String& errorMessage);

    /// Returns the instrument name this dialog tracks.
    const juce::String& instrumentName() const noexcept { return instrumentName_; }

    /// Returns the selected target.
    hathor::AssetTarget target() const noexcept { return target_; }

private:
    // juce::Timer — for auto-dismiss after completion
    void timerCallback() override;

    // juce::Component
    void paint(juce::Graphics& g) override;
    void resized() override;

    void updateStageText();

    // -----------------------------------------------------------------------
    // Layout constants
    // -----------------------------------------------------------------------
    static constexpr int kWindowWidth  = 380;
    static constexpr int kWindowHeight = 180;
    static constexpr int kProgressBarHeight = 6;
    static constexpr int kAutoDismissMs = 3000;

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    double                   progressValue_{ 0.0 };
    juce::String             instrumentName_;
    hathor::AssetTarget      target_;
    Stage                    currentStage_{ Stage::Preparing };
    juce::String             statusMessage_;
    juce::String             errorMessage_;
    bool                     isComplete_{ false };
    bool                     isFailed_{ false };
    std::atomic<int>         autoDismissCount_{ 0 };

    // Subcomponents
    juce::ProgressBar        progressBar_;
    juce::Label              titleLabel_;
    juce::Label              stageLabel_;
    juce::Label              detailLabel_;
    juce::TextButton         closeButton_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BakeProgressDialog)
};

} // namespace hathor::ui
