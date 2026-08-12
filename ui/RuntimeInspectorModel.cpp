// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * RuntimeInspectorModel.cpp — L-6: implementation of the JUCE-free runtime
 * inspection model.  See RuntimeInspectorModel.hpp for the threading and
 * audio-safety contract.
 *
 * Requirement references: L-6 §Hathor Runtime Inspection, L-6 §Audio/Thread Safety
 */

#include "RuntimeInspectorModel.hpp"

#include <chrono>
#include <utility>

namespace hathor::ui {

namespace {

/// Shared capture body used by both the synchronous API (tests) and the
/// background thread.  Queries per-tab VM state through the read-only facade
/// and publishes into `shared`.  Only ever dereferences `audio` (which
/// outlives the panels) and `shared` (which the caller keeps alive).
void captureVmsInto(AudioEngineFacade* audio,
                    const std::shared_ptr<RuntimeInspectorModel::Shared>& shared)
{
    if (audio == nullptr || shared->stopped.load(std::memory_order_acquire))
        return;

    // Which slots are worth querying?  Any slot with a pattern or a running
    // flag (typically 1–4 active tabs).  Bounded — never all 16 slots.
    std::vector<AudioEngineFacade::SlotPlayback> slots;
    try
    {
        slots = audio->listSlotPlayback();
    }
    catch (...)
    {
        slots.clear();
    }

    std::vector<AudioEngineFacade::VmStatus> vms;
    std::vector<int>                         idx;
    for (const auto& s : slots)
    {
        if (shared->stopped.load(std::memory_order_acquire))
            return;
        if (!s.hasPattern && !s.running)
            continue;
        try
        {
            vms.push_back(audio->getVmStatus(s.slotIndex));
            idx.push_back(s.slotIndex);
        }
        catch (...)
        {
            // A throwing source must never wedge the inspector.
        }
    }

    // Ensure at least one query so workerStatus is captured.  An out-of-range
    // slot returns workerStatus from the manager's atomic state without
    // touching the control-plane socket.
    if (vms.empty())
    {
        try
        {
            auto st = audio->getVmStatus(-1);
            if (!st.workerStatus.empty())
            {
                vms.push_back(std::move(st));
                idx.push_back(-1);
            }
        }
        catch (...)
        {
        }
    }

    // Derive worker liveness/restart state from the real VM queries.
    std::string workerStatus;
    uint64_t    generation = 0;
    for (std::size_t i = 0; i < vms.size(); ++i)
    {
        if (idx[i] >= 0)
        {
            if (!vms[i].workerStatus.empty())
                workerStatus = vms[i].workerStatus;
            if (vms[i].generation > generation)
                generation = vms[i].generation;
        }
    }

    {
        std::lock_guard<std::mutex> lock(shared->mtx);
        shared->snap.vmStates       = std::move(vms);
        shared->snap.vmSlotIndices  = std::move(idx);
        if (!workerStatus.empty())
            shared->snap.workerStatus = std::move(workerStatus);
        if (generation > 0)
            shared->snap.workerGeneration = generation;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

RuntimeInspectorModel::~RuntimeInspectorModel()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

void RuntimeInspectorModel::setSources(AudioEngineFacade* audio,
                                       hathor::control::DiagnosticRegistry* registry) noexcept
{
    audio_ = audio;
    registry_ = registry;
}

// ---------------------------------------------------------------------------
// Quick capture (message thread)
// ---------------------------------------------------------------------------

void RuntimeInspectorModel::refreshQuick()
{
    auto shared = shared_;
    if (shared->stopped.load(std::memory_order_acquire) || audio_ == nullptr)
        return;

    RuntimeSnapshot snap;

    // All noexcept snapshot APIs — atomics only, no blocking, no mutation.
    snap.audio = audio_->getAudioStatus();
    snap.slots = audio_->listSlotPlayback();
    audio_->activeVoices(snap.voices);
    snap.workerAlive = audio_->hasWorker();

    if (registry_)
    {
        const auto counts = registry_->counts();
        snap.diagErrors   = counts.errors;
        snap.diagWarnings = counts.warnings;
        snap.diagInfo     = counts.info;
    }

    {
        std::lock_guard<std::mutex> lock(shared->mtx);
        // Preserve the async VM fields from the previous snapshot.
        snap.vmStates        = std::move(shared->snap.vmStates);
        snap.vmSlotIndices   = std::move(shared->snap.vmSlotIndices);
        snap.workerStatus    = std::move(shared->snap.workerStatus);
        snap.workerGeneration = shared->snap.workerGeneration;
        shared->snap = std::move(snap);
    }
}

RuntimeSnapshot RuntimeInspectorModel::snapshot() const
{
    std::lock_guard<std::mutex> lock(shared_->mtx);
    return shared_->snap;
}

// ---------------------------------------------------------------------------
// VM capture (background)
// ---------------------------------------------------------------------------

void RuntimeInspectorModel::captureVmsSync()
{
    captureVmsInto(audio_, shared_);
}

void RuntimeInspectorModel::requestVmCapture()
{
    auto shared = shared_;
    if (shared->stopped.load(std::memory_order_acquire))
        return;

    // Only one capture at a time.  If the previous capture has already
    // finished its body (inFlight reset at the very end), its thread object
    // is joinable but finished — join it now to avoid std::terminate when
    // assigning a fresh std::thread.
    if (shared->vmCaptureInFlight.exchange(true, std::memory_order_acq_rel))
        return;
    if (captureThread_.joinable())
        captureThread_.join();

    // The thread captures only `audio` (raw, read-only facade) and `shared`
    // by value — it never dereferences the model object itself, so it stays
    // safe even if the model is destroyed mid-capture.
    AudioEngineFacade* audio = audio_;
    captureThread_ = std::thread([audio, shared]()
    {
        captureVmsInto(audio, shared);
        shared->vmCaptureInFlight.store(false, std::memory_order_release);
    });
}

bool RuntimeInspectorModel::vmCaptureInFlight() const noexcept
{
    return shared_->vmCaptureInFlight.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void RuntimeInspectorModel::shutdown()
{
    shared_->stopped.store(true, std::memory_order_release);

    // Move the thread out so this object's destructor never sees a joinable
    // thread (which would std::terminate).
    auto t = std::move(captureThread_);
    if (t.joinable())
    {
        // Bounded wait: a healthy control-plane query finishes in
        // milliseconds.  If the worker is hung, the query can block up to
        // the IPC timeout (~5 s) — detach rather than hang the message
        // thread; the thread holds `shared` + `audio` by value and exits at
        // its next stopped_ check.  (AudioEngine outlives the panels and
        // its shutdownWorker() kills the hung socket, releasing any
        // in-flight query.)
        for (int i = 0; i < 50 && shared_->vmCaptureInFlight.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        if (shared_->vmCaptureInFlight.load(std::memory_order_acquire))
            t.detach();
        else
            t.join();
    }
}

} // namespace hathor::ui
