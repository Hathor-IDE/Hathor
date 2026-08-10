// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * BakeOrchestrator.cpp — implementation of the B8-K6 bake pipeline.
 *
 * Requirements: B8-K6 (all sections), B8-K1–K5
 */

#include "BakeOrchestrator.hpp"
#include "BakeTargetDialog.hpp"

#include <fstream>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

BakeOrchestrator::BakeOrchestrator(AudioEngine& audio, StatusCallback statusSink)
    : juce::Component()
    , audio_(audio)
    , statusSink_(std::move(statusSink))
{
    projectDir_ = juce::File::getCurrentWorkingDirectory().getFullPathName();
}

BakeOrchestrator::~BakeOrchestrator()
{
    if (renderHandle_.has_value())
    {
        renderHandle_->cancel();
        renderHandle_.reset();
    }

    // Dismiss the progress dialog if still visible.
    if (dialog_ != nullptr)
    {
        dialog_->setVisible(false);
        dialog_.reset();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool BakeOrchestrator::bakeFromTab(const juce::String& ckSourcePath,
                                   const juce::String& ckSourceCode,
                                   uint8_t tabId,
                                   juce::Component* parent)
{
    // Re-entrancy guard (B8-K6 §11: no concurrent bakes of the same instrument).
    {
        std::lock_guard<std::mutex> lock(bakeMutex_);
        if (activeBakes_.contains(tabId))
        {
            if (statusSink_)
                statusSink_("A bake is already in progress for this instrument.");
            return false;
        }
        activeBakes_.insert(tabId);
    }

    // Store the source path/code for persistence after the target dialog.
    pendingSourcePath_ = ckSourcePath;
    pendingSourceCode_ = ckSourceCode;
    pendingTabId_ = tabId;

    // Stage 0: Show B8-K1 target selection dialog (modal, Studio default).
    beginTargetSelection(ckSourcePath, ckSourceCode, tabId, parent);

    return true;
}

bool BakeOrchestrator::isBaking(uint8_t tabId) const noexcept
{
    std::lock_guard<std::mutex> lock(bakeMutex_);
    return activeBakes_.contains(tabId);
}

void BakeOrchestrator::cancelBake(uint8_t tabId) noexcept
{
    if (renderHandle_.has_value())
    {
        renderHandle_->cancel();
    }
    clearBaking(tabId);
}

// ---------------------------------------------------------------------------
// Pipeline stages
// ---------------------------------------------------------------------------

void BakeOrchestrator::beginTargetSelection(const juce::String& /*ckSourcePath*/,
                                            const juce::String& ckSourceCode,
                                            uint8_t tabId,
                                            juce::Component* parent)
{
    // B8-K1: Show the target selection dialog (modal, Studio default).
    // The callback fires when the user clicks "Bake"; if they cancel,
    // onSelected is not called.
    showBakeTargetDialog(parent, [this, ckSourceCode, tabId](hathor::AssetTarget target) {
        // Cache the selected target for downstream source persistence.
        pendingTarget_ = target;

        // Validate source — must not be empty (B8-K6 §3).
        if (ckSourceCode.trim().isEmpty())
        {
            showFailure("Source validation", "ChucK source is empty — nothing to bake.");
            clearBaking(tabId);
            return;
        }

        // Proceed to B8-K2: background render.
        startBackgroundRender(ckSourceCode, tabId, target, nullptr);
    });
}

void BakeOrchestrator::startBackgroundRender(const juce::String& ckSourceCode,
                                             uint8_t tabId,
                                             hathor::AssetTarget target,
                                             juce::Component* parent)
{
    // Derive the instrument name from the source path (B8-K1 §5).
    juce::String instrumentNameStr = "untitled";
    if (!pendingSourcePath_.isEmpty())
        instrumentNameStr = juce::File(pendingSourcePath_).getFileNameWithoutExtension();

    // Resolve the render destination path via AudioEngine::resolveRenderPath (B8-K1).
    // For LiveJam, AudioEngine uses its internally-managed LiveJamSessionManager.
    const auto destPath = audio_.resolveRenderPath(
        target,
        instrumentNameStr.toStdString(),
        projectDir_.toStdString());

    if (destPath.empty())
    {
        showFailure("Path resolution", "Could not resolve render destination path.");
        clearBaking(tabId);
        return;
    }

    // Create the progress dialog (B8-K6 §5).
    dialog_ = std::make_unique<BakeProgressDialog>(instrumentNameStr, target, parent);
    dialog_->setStage(BakeProgressDialog::Stage::Preparing);

    // Show the progress dialog as a non-modal floating window (B8-K6 §6).
    dialog_->toFront(true);
    dialog_->setVisible(true);
    dialog_->setAlwaysOnTop(true);

    // Start the background render (B8-K2).
    // AudioEngine::startBakeRender() returns immediately with a RenderHandle.
    const uint64_t numSamples = 44100 * 4; // 4 seconds — configurable per instrument
    const unsigned sampleRate = 44100;

    renderHandle_ = audio_.startBakeRender(
        tabId,
        ckSourceCode.toStdString(),
        numSamples,
        sampleRate,
        destPath,
        [this](const hathor::RenderResult& result) {
            onRenderComplete(result);
        });

    dialog_->setStage(BakeProgressDialog::Stage::Rendering);
}

void BakeOrchestrator::onRenderComplete(const hathor::RenderResult& result)
{
    // This callback runs on the ChucK render thread — must not touch JUCE UI directly.
    // Marshal all UI updates to the message thread (B8-K6 §6).
    juce::MessageManager::callAsync([this, result]() {
        // Check if this bake was cancelled.
        if (result.state == hathor::RenderState::Cancelled)
        {
            showFailure("Render", "Bake was cancelled.");
            clearBaking(pendingTabId_);
            return;
        }

        if (!result.success)
        {
            showFailure("Render",
                         "B8-K2 render failed: " +
                         juce::String(result.errorMessage));
            clearBaking(pendingTabId_);
            return;
        }

        // Cache the result for use in downstream stages.
        result_ = result;

        // B8-K3: Shut down the VM for this tab (runtime teardown).
        dialog_->setStage(BakeProgressDialog::Stage::ShuttingDown);
        shutdownBakedRuntime(pendingTabId_);
    });
}

void BakeOrchestrator::shutdownBakedRuntime(uint8_t tabId)
{
    // B8-K3: Tell AudioEngine to shut down the VM + thread for this tab.
    // This sends a shutdown command to the audio worker.
    bool stopped = audio_.stopCkTab(tabId);

    if (!stopped)
    {
        // Non-fatal: the VM may already be stopped. Continue to registration.
        if (statusSink_)
            statusSink_("Warning: could not shut down ChucK tab " +
                        juce::String(tabId) + " — proceeding anyway.");
    }

    // B8-K4: Register the sample in the SampleBank.
    dialog_->setStage(BakeProgressDialog::Stage::Registering);
    registerSample(result_);
}

void BakeOrchestrator::registerSample(const hathor::RenderResult& /*renderResult*/)
{
    // Resolve the published WAV path from the cached render result.
    const auto wavPath = result_.outputPath;

    if (wavPath.empty())
    {
        showFailure("File check", "Render produced no output path.");
        clearBaking(pendingTabId_);
        return;
    }

    // Verify the file exists before registering (B8-K4 §3: validate before register).
    if (!std::filesystem::exists(wavPath))
    {
        showFailure("File check",
                     "Published WAV file not found on disk: " +
                     juce::String(wavPath.string()));
        clearBaking(pendingTabId_);
        return;
    }

    // Derive the instrument name from the WAV path stem.
    const juce::String instrumentName = juce::File(juce::String(wavPath.string()))
        .getFileNameWithoutExtension();

    // B8-K5 §4: Persist the .ck source alongside the .wav for Studio bakes.
    // This must happen BEFORE sample registration so that if source persistence
    // fails, we report the error rather than claiming full success.
    if (pendingTarget_ == hathor::AssetTarget::Studio)
    {
        const auto instrDir = hathor::AssetPathResolver(
            projectDir_.toStdString()).studioInstrumentsDir();

        const auto ckPath = instrDir / (instrumentName.toStdString() + ".ck");

        std::error_code ec;
        std::filesystem::create_directories(instrDir, ec);
        if (ec)
        {
            showFailure("Source persistence",
                         "Failed to create instruments dir: " + juce::String(ec.message()));
            clearBaking(pendingTabId_);
            return;
        }

        std::ofstream ofs(ckPath);
        if (!ofs)
        {
            showFailure("Source persistence",
                         "Failed to open " + juce::String(ckPath.string()) + " for writing.");
            clearBaking(pendingTabId_);
            return;
        }
        ofs << pendingSourceCode_.toStdString();
        if (!ofs)
        {
            showFailure("Source persistence",
                         "Failed to write .ck source to " + juce::String(ckPath.string()));
            clearBaking(pendingTabId_);
            return;
        }
    }

    // B8-K4: Register the sample in SampleBank via AudioEngine.
    // AudioEngine::registerBakedAsset handles decoding + resampling + addEntry().
    bool registered = audio_.registerBakedAsset(
        instrumentName.toStdString(),
        wavPath);

    if (!registered)
    {
        showFailure("SampleBank registration",
                     "Failed to register sample in SampleBank.");
        clearBaking(pendingTabId_);
        return;
    }

    dialog_->setStage(BakeProgressDialog::Stage::Finishing);

    // Stage 5: Refresh autocomplete (the .hathor_assets directory).
    refreshAutocomplete();

    // Stage 6: B8-K5 — Explorer refresh (polling timer will also catch this).
    refreshExplorer();

    // All stages complete — finalise (B8-K6: success path).
    finishBake(true, instrumentName);
    clearBaking(pendingTabId_);
}

void BakeOrchestrator::refreshAutocomplete()
{
    // The autocomplete source reads from the .hathor_assets directory.
    // Signal the status sink so EditorArea can notify the autocomplete provider.
    const juce::String instrumentName = juce::File(
        juce::String(result_.outputPath.string()))
        .getFileNameWithoutExtension();

    if (statusSink_)
        statusSink_("Sample list refreshed — \"" + instrumentName +
                    "\" available for `s \"...\"`");
}

void BakeOrchestrator::refreshExplorer()
{
    // B8-K5 §9: The ExplorerPanel has a polling timer that will detect the
    // new file within 2 seconds.  We also signal the status sink so the
    // EditorArea can trigger an explicit refresh if it holds a reference.
    if (statusSink_)
        statusSink_("Explorer refresh triggered.");
}

void BakeOrchestrator::finishBake(bool success, const juce::String& message)
{
    if (success)
    {
        if (dialog_)
            dialog_->complete();
        if (statusSink_)
            statusSink_("Baked \"" + message + "\" to Song — registered in SampleBank.");
    }
    else
    {
        showFailure("Finalisation", message);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

juce::String BakeOrchestrator::deriveInstrumentName(const juce::String& ckSourcePath)
{
    if (ckSourcePath.isEmpty())
        return "untitled";

    return juce::File(ckSourcePath).getFileNameWithoutExtension();
}

void BakeOrchestrator::showFailure(const juce::String& stageName,
                                   const juce::String& errorMessage)
{
    if (dialog_)
        dialog_->fail(stageName + ": " + errorMessage);
    else if (statusSink_)
        statusSink_("Bake failed at " + stageName + ": " + errorMessage);
}

void BakeOrchestrator::clearBaking(uint8_t tabId) noexcept
{
    {
        std::lock_guard<std::mutex> lock(bakeMutex_);
        activeBakes_.erase(tabId);
    }
    renderHandle_.reset();
    pendingSourcePath_.clear();
    pendingSourceCode_.clear();
}

} // namespace hathor::ui
