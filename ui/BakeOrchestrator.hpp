// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * BakeOrchestrator.hpp — B8-K6: "Bake to Song" workflow orchestrator.
 *
 * Coordinates the full bake pipeline:
 *
 *   Active .ck tab → B8-K1 target selection → validate source →
 *   B8-K2 background render → validate/publish WAV → B8-K3 VM shutdown →
 *   B8-K4 SampleBank registration → .hathor autocomplete refresh →
 *   B8-K5 Explorer refresh → success
 *
 * Design:
 *   - The orchestrator lives on the JUCE message thread (owned by MainWindow).
 *   - B8-K2 rendering runs entirely on a background thread inside
 *     ChuckRenderWriter — the orchestrator never blocks on it.
 *   - Progress is surfaced via BakeProgressDialog (visible, non-blocking).
 *   - Every failure stage is surfaced to the user — no silent failures.
 *   - Re-entrancy guard prevents multiple simultaneous bakes of the same instrument.
 *
 * Requirements: B8-K6 (all sections), B8-K1–K5
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "AssetTarget.hpp"
#include "BakeProgressDialog.hpp"
#include "ChuckRenderWriter.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include "AudioEngine.hpp"

namespace hathor::ui {

/**
 * BakeOrchestrator — drives the complete B8-K2→K4 bake pipeline.
 *
 * Usage:
 *   1. Construct with references to AudioEngine and a status sink.
 *   2. Call bakeFromTab() when the user invokes "Bake to Song" on a .ck tab.
 *
 * Thread safety:
 *   - bakeFromTab() is called on the JUCE message thread.
 *   - The B8-K2 completion callback is delivered on the render thread; the
 *     orchestrator marshals all UI updates via MessageManager::callAsync.
 */
class BakeOrchestrator : public juce::Component
{
public:
    using StatusCallback = std::function<void(const juce::String&)>;

    /**
     * @param audio      The AudioEngine (for startBakeRender, resolveRenderPath,
     *                   registerBakedAsset, stopCkTab, listSamples).
     * @param statusSink Called to display a transient status message (e.g. in
     *                   the EditorArea status bar).  May be nullptr.
     */
    BakeOrchestrator(AudioEngine& audio, StatusCallback statusSink = nullptr);
    ~BakeOrchestrator() override;

    // Non-copyable. (copy deletion handled by JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR below)

    // -----------------------------------------------------------------------
    // Main entry point — called by EditorArea when "Bake to Song" fires.
    //
    // Shows the B8-K1 target selection dialog, then begins the pipeline.
    // Returns false immediately if a bake is already in progress for the
    // same instrument (the user is notified).
    // -----------------------------------------------------------------------
    /**
     * Begin a bake operation for the given ChucK instrument tab.
     *
     * @param ckSourcePath  Path to the .ck file (for name derivation and source retention).
     * @param ckSourceCode  Full ChucK source code text.
     * @param tabId         Slot index of the tab (0–15).
     * @param parent        Parent component for the target-selection dialog and progress window.
     * @return true if the bake was initiated (dialog shown); false if a bake
     *         is already in progress for this instrument.
     */
    bool bakeFromTab(const juce::String& ckSourcePath,
                     const juce::String& ckSourceCode,
                     uint8_t tabId,
                     juce::Component* parent = nullptr);

    /**
     * Check whether a bake is currently in progress for the given tab.
     */
    bool isBaking(uint8_t tabId) const noexcept;

    /**
     * Cancel an in-progress bake.  Safe to call from any thread.
     * The completion callback (on the render thread) will receive a
     * Cancelled RenderResult.
     */
    void cancelBake(uint8_t tabId) noexcept;

private:
    // -----------------------------------------------------------------------
    // Pipeline stages (B8-K2 → K4)
    // -----------------------------------------------------------------------

    /// Stage 1: B8-K1 target selection + B8-K2 render start.
    /// Shows BakeTargetDialog; on confirmation, resolves the path and
    /// starts the background render.
    void beginTargetSelection(const juce::String& ckSourcePath,
                              const juce::String& ckSourceCode,
                              uint8_t tabId,
                              juce::Component* parent);

    /// Stage 2: B8-K2 background render.
    /// Called after target is selected; starts ChuckRenderWriter::startRender().
    void startBackgroundRender(const juce::String& ckSourceCode,
                               uint8_t tabId,
                               hathor::AssetTarget target,
                               juce::Component* parent);

    /// Stage 3: B8-K3 VM shutdown — destroy the baked tab's VM/thread.
    /// Called from the B8-K2 completion callback on success.
    void shutdownBakedRuntime(uint8_t tabId);

    /// Stage 4: B8-K4 SampleBank registration.
    /// Called after B8-K3; registers the .wav in the SampleBank.
    void registerSample(const hathor::RenderResult& renderResult);

    /// Stage 5: .hathor autocomplete refresh.
    /// Called after SampleBank registration; notifies the UI that
    /// new samples are available for autocomplete.
    void refreshAutocomplete();

    /// Stage 6: B8-K5 Explorer refresh.
    /// Called after autocomplete refresh; triggers ExplorerPanel rebuild.
    void refreshExplorer();

    /// Stage 7: Success / failure finalisation.
    void finishBake(bool success, const juce::String& message);

    /// The B8-K2 completion callback — runs on the render thread.
    void onRenderComplete(const hathor::RenderResult& result);

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    /// Derive the instrument name from a .ck file path (filename stem).
    static juce::String deriveInstrumentName(const juce::String& ckSourcePath);

    /// Show a persistent (non-auto-dismiss) failure on the progress dialog.
    void showFailure(const juce::String& stageName,
                     const juce::String& errorMessage);

    /// Clear the in-progress flag for a tab.
    void clearBaking(uint8_t tabId) noexcept;

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    AudioEngine&              audio_;
    StatusCallback            statusSink_;

    std::unique_ptr<BakeProgressDialog> dialog_;

    /// Re-entrancy guard: instruments currently being baked.
    mutable std::mutex      bakeMutex_;
    std::unordered_set<uint8_t> activeBakes_;

    /// Cached render handle for cancellation.
    std::optional<hathor::RenderHandle> renderHandle_;

    /// Cached render result for use in downstream pipeline stages.
    /// Populated by onRenderComplete() on the message thread.
    hathor::RenderResult result_;

     /// Pending bake context (set in bakeFromTab, consumed by startBackgroundRender).
     juce::String pendingSourcePath_;
     juce::String pendingSourceCode_;
     hathor::AssetTarget pendingTarget_{ hathor::AssetTarget::Studio };
     uint8_t      pendingTabId_{ 0 };

    /// Project directory (cwd at launch).
    juce::String projectDir_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BakeOrchestrator)
};

} // namespace hathor::ui
